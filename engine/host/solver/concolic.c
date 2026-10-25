/* The concolic value type (see concolic.h): a host component built on upstream QuickJS's public class API, so
   the qjs fork carries no value-type delta. */
#include "solver/concolic.h"
#include "quickjs-step.h"
#include "solver/absent.h"
#include "solver/flow.h"
#include "solver/decide.h"    /* the one speller of a constraint key */
#include "solver/solve.h"     /* the search is told when a candidate substitution happens (concolic_deliver) */
#include "solver/reclaim.h"   /* the engine's own allocations ask for a flow back before they fail */
#include "solver/endpoint.h"  /* the @H surface is told which globals a program names (`.global_named`) */
#include "solver/rung_entry.h" /* the scheduler is told the arms a program asks for by name */
#include "solver/metrics.h"   /* the attacker-source mint raises `_sourceReads` */
#include "core/timing/thread_work.h"   /* a constraint probe is priced by the bytes it hashes */
#include "core/timing/hr_time.h"       /* the current time the engine asks for (`.clock`) */
#include "core/crypto/secure_hash.h"   /* a long identity is the digest of its spelling */
#include "check.h"
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

/* The per-value state hung off the JSObject via JS_SetOpaque.
 *
 * `src`, `root` and `ident` are three different facts:
 *   `src` is the injection identity: the source an @S candidate substitutes at. Two derivations mint a new one,
 *   because a field read of an unknown object (`{state}.admin`) and the result of calling an unknown are
 *   independently controlled data.
 *   `root` is the delivery provenance: the set of sources whose components physically carried the bytes in. A
 *   derivation never invents one; it unions its operands' roots (derived_root_join), so a derivation over one
 *   unknown keeps that operand's root. `location.hash.slice(1)` is no longer the fragment for injection but still
 *   is for delivery, which is what a reproduction envelope and a percent-encode set ask about.
 *   `ident` says which value this is. It is composed at every derivation and keys the per-flow path constraint,
 *   so `x`, `x*2` and `x < 700` are distinct facts and deciding one never prunes an arm over another.
 * `ident` is NULL where this engine cannot spell the value exactly (an object or symbol operand, a property name
 * that does not convert). Such a value is never decided from another value's record, so both arms of every
 * branch over it stay: absence costs forks, a wrong identity costs the arm. */
/* Marks a predicate that is not an equality. It is not a JSConcolicEqOp: reusing LOOSE's zero would make
   `x < 700` claim the page wrote `==`, and a negative value cannot collide with a future member of that enum. */
enum { CMP_ALGO_NONE = -1 };
/* The affine facet of a numeric value: `value = c + Σ k[i]·T(term[i])` in every world, where each term is an
   integer-valued unknown named by its identity, bounded by AFF_TERM_MAX, and the whole is bounded by
   AFF_EXACT_MAX. Within that bound every value the form takes is an integer a double holds exactly, so the
   engine's real IEEE operation on the example and the form's arithmetic agree in every world, and a form
   whose terms cancel is the constant `c`. It is a domain fact about the value and is never inverted to
   determine a term. `term` is strcmp-sorted and distinct, `k` holds non-zero integers, `c` is an integer, and
   `n >= 1`: a form with no terms is a concrete number and never stored. */
typedef struct {
    int     n;
    char  **term;
    double *k;
    double  c;
} ConcolicAffine;
typedef struct {
    char *shape;        /* @H/@S display form */
    char *src;          /* injection identity: the source an @S candidate substitutes at */
    /* Whether `src` names this value itself. A derivation inherits its first unknown operand's `src`
       (concolic_add_hook), so in general `src` is where a candidate is injected, not a name for the value. The
       pin is keyed by `src`, so a pin may be read back only where this is 1. Set by pin_src_names_self at the
       two mints that establish it and carried by nothing: a negation copies the predicate observations
       (pred_carry_through_not) but not this, so a value that got here by any other route reads 0. */
    signed char src_self;
    /* Whether `src` is this value's own injection identity rather than one inherited from an operand: a source
       read (`src_self`), and any value whose `src` is its own shape (a member read, a call result, a `typeof`),
       written at concolic_alloc. An arithmetic, concatenation, coercion or builtin derivation carries its first
       unknown operand's `src` and reads 0. An equality pins under `src` only for a subject that owns it. */
    signed char src_own;
    char *root;         /* delivery provenance: where the bytes entered. A derivation unions its operands'
                           (one member = unchanged); a set, walked by root_member. NULL exactly when `src` is */
    /* Whose unknown each member of `root` is: a mask over ConcolicRootWhose, because `root` is a set and one
       world root is enough to clear the product's bar however many instrument roots stand beside it. The mask
       answers both quantifiers from one fact: `concolic_root_whose_any` tests a bit, `_all` tests that the mask
       is exactly that bit. Maintained only where the root is joined (derived_root_whose, beside
       derived_root_join). Zero exactly when `root` is NULL, which concolic_alloc asserts.
       CONCOLIC_WHOSE_UNSTATED is a bit of its own, not that zero: "no mint could say whose this is" and "there
       is no root" are different answers. */
    unsigned root_whose;
    char *ident;        /* identity: this exact value, composed below. NULL = this engine cannot spell it */
    /* Which predicate a branch over this value asks about, and with what polarity. `ident` names this value
       and is what a derivation composes from (`"" + !p` and `"" + p` must not collide); `br_key` names the
       value whose truth this one's truth is a function of, and keys the path constraint. For a negation that
       is the operand: §7.1.2 ToBoolean applies to `if (p)` and `if (!p)` alike, so both spellings share one
       constraint entry and a flow that decided `p` decides `!p` without forking again.
       NULL = this value's own `ident` is the key (every non-negation). Both are written together at the one
       mint that produces them (the ToBoolean mint) and asserted at their readers; `br_neg` is 1 only where
       `br_key` is set. */
    char *br_key;
    signed char br_neg; /* 1 = this value is the logical complement of the predicate `br_key` names */
    JSValue example;    /* concrete example, meaningful only where `ex_has` says one is held */
    /* Whether an example is held at all. `undefined` is a value a real read produces — a member the record the
       visitor was served does not have — so absence is this bit and never the field's value. 1 wherever the mint
       was handed bytes (concolic_alloc), and on a member read whose parent's example lacks the member. */
    uint8_t ex_has;
    /* This value's own [[Prototype]] (§10.1.1 [[GetPrototypeOf]] ( )), derived once and held so the answer has
       an identity: `Object.getPrototypeOf(x) === Object.getPrototypeOf(x)` must be true, and §10.4.7.2
       SetImmutablePrototype ( obj, proto ) step 2 compares by SameValue against what step 1 returned.
       JS_UNINITIALIZED = not yet asked, written at concolic_alloc rather than left as the allocator's zero.
       Not per-flow state and never captured: the prototype is a pure function of the value, so two flows asking
       mint one answer. What the page then writes through it is an ordinary property write the COW delta
       carries. */
    JSValue proto;
    int cmp_op;         /* for an equality result: OPCMP_EQ, `src === cmp_tok` (else OPCMP_NONE) */
    char *cmp_tok;      /* the concrete side of the equality, spelled (§7.1.19 ToString of the operand) */
    signed char cmp_kind;  /* what `cmp_tok` spells (ConcolicLit). Never separable from it: without the kind
                              decide.c pins nine characters where the page wrote `undefined`. Written with
                              `cmp_tok` at pred_new, carried with it by pred_carry_through_not. */
    /* Which equality the page wrote, which says what an arm may conclude. A `===` that held determines the
       operand (§7.2.14 IsStrictlyEqual ( x, y ) step 1 is false unless the types match); §7.2.13
       IsLooselyEqual ( x, y ) coerces, so its holding arm leaves a set (concolic.h's concolic_pin). Written with
       `cmp_op` at pred_new and carried with it through a negation, because decide.c reads both at one line.
       CMP_ALGO_NONE for a predicate that is not an equality. */
    signed char cmp_algo;
    char *cmp_subj;     /* the hole key of the unknown side (concolic_cmp_subject). cmp_op, cmp_tok, cmp_kind,
                           cmp_algo and this are one observation: written and asserted together at pred_new,
                           read together by decide.c, which pins on one arm and excludes on the other. */
    /* The unknown operand's own identity, beside the hole above. The hole is what a report prints and is
       derived lossily from the display shape (`{}` has none, two shapes can strip to one string); this is the
       exact key under which the per-flow record "this flow proved that example wrong" is filed. Written by
       pred_set_subject_ident, which asserts the hole is present.
       Absent when both sides are unknown: `x === y` over two examples proves on the contradicting arm that one
       of them is wrong but never which, so naming either would invent a fact. The pin and the exclusion refuse
       that case for the same reason. */
    char *cmp_subj_ident;
    /* For an equality, its subject's `src_own`, stated at the mint: 1 the subject is the value `src` names, 0 it
       is derived from it, -1 unstated (concolic_alloc's value, which concolic_pin refuses). Carried through a
       negation with the rest of the observation. */
    signed char cmp_src_own;
    /* For a §13.5.3.1 typeof result, the `src` of the operand it was taken of, where that operand owns its
       `src` (a source or member read); NULL otherwise. For an equality whose subject is such a result, the
       same operand's `src` (`cmp_typeof_of`), carried through a negation with the rest of the observation:
       decide.c files the arm as that operand's type facet (concolic_equality_facet) rather than a pin. Owned. */
    char *typeof_of;
    char *cmp_typeof_of;
    /* Whether this value is a typeof result (`is_typeof`), or an equality whose subject is one (`cmp_typeof`),
       whatever its operand: a typeof outcome is a fact about its operand's type, never a value to pin. */
    signed char is_typeof, cmp_typeof;
    /* The CONCOLIC_TYF_* types its mint states the compared value takes (`subj_types`, on an equality; for a
       typeof subject, its operand's), and on a typeof result its operand's (`typeof_types`): what §13.15.3's
       concatenation or a String-valued builtin fixes, which a derivation owning no `src` has no facet to
       hold. 0 on any value that is neither; written at the equality mints and the typeof hook, and carried
       through a negation with the rest of the observation. */
    unsigned short cmp_subj_types, typeof_types;
    /* The ordering's own fields, kept apart from the equality's: an equality determines a value on one arm, an
       ordering a bound on both. `rel_op` is normalised subject-on-the-left (`5 < x` arrives as `x > 5`), so
       decide.c reads a relation without knowing which operand the page wrote first. */
    RelOp rel_op;       /* for an ordering result: the relation, subject-left (else REL_NONE)  */
    char *rel_tok;      /* the concrete side spelled: the page's own §6.1.6.1.20 Number::toString ( x, radix ) */
    double rel_num;     /* …and its value, which is what a merge orders two bounds by */
    char *rel_subj;     /* …and the hole key of the unknown side. All four written together by pred_set_bound */
    /* The property name this value was read under, written only by concolic_exotic_get, so it is present
       exactly on a member read of an unknown. The call handler names the invoked method from it; recovering
       the name from the display shape would parse a string composed for a human, and a `.` in a property name
       would decide it wrong. */
    char *member;
    /* On a member read, the parent's `src` where the parent owns it, so a call through the member can narrow
       the parent's type facet (concolic_call_hook); on an ordering whose subject owns its `src`, that `src`,
       narrowed on the arm (concolic_ordering_facet). NULL otherwise. Owned. */
    char *facet_src;
    /* The call predicate's fields, a third class beside the equality's and the ordering's: a call determines
       neither a value nor an interval and still narrows. Written together by pred_set_strpred and read together
       by concolic_strpred; `sp_meth` is the presence test, as `rel_op != REL_NONE` is the ordering's. */
    char *sp_meth;      /* the method the page called on the unknown: c->member of the callee */
    char **sp_args;     /* every argument spelled, all of them or (when one is unspellable) none at all */
    int   sp_nargs;
    char *sp_subj;      /* …and the hole key of the receiver, which the emission looks a domain up by */
    /* The conjuncts this value's truth is the conjunction of: a set sorted by identity with each member once,
       so `p ∧ q` and `q ∧ p` compose one key. Recorded as a fact and never parsed back out of `ident`, which
       composes the same members under the conjunction tag; the one reader is the mint, which flattens a
       conjunct that is itself a conjunction.
       `conj_n >= 2` exactly where this value is a conjunction this engine could name, 0 elsewhere: on a
       conjunction with an unspellable member (its `ident` is NULL, so any parent is unspellable too) and on a
       negation of one, because `!(p ∧ q)` is not a conjunction. */
    char **conj;
    int    conj_n;
    /* The atom this value was spent on as a property key, written once by keyname_atom: its shape named by its
       identity, or, where the identity is absent (an Object or Symbol operand), by the site of the first
       purchase. NULL until the value is first spent as a key.
       Memoised because a site atom is composed from where the page stood at the first purchase: recomputing
       would buy a value written under one key on two lines two atoms. A field makes the atom a pure function of
       the value, which the keyname table's declaration relies on.
       Owned and freed at concolic_finalizer. The record has one mint (concolic_alloc, through reclaim_calloc,
       so the never-purchased state is the zero) and no field-by-field clone. */
    char *key_atom;
    /* The identity and shape of the value `x` this one is the string form of, else NULL: a concatenation with
       the empty String (`x + ""`, `"" + x`, a template's `"".concat(x)`) or §22.1.1.1 String ( value ). As a
       property key it is the key `x` is, so concolic_key_name_hook names its slot and records its identity from
       these (str_of_record says in which worlds). Written together once at the mint; both owned. */
    char *str_of;
    char *str_of_shape;
    /* The affine facet, or NULL. Written once, at the mint, by aff_attach; owned. */
    ConcolicAffine *aff;
    /* Which bytes of `shape` this engine spelled for an unknown (concolic_shape_hole_mask): `hole_n` sorted,
       disjoint `[start, end)` pairs in `hole_span`. Written only on a §13.15.3 string concatenation, whose
       other bytes are the page's own; NULL on every other value, whose whole shape spells one unknown. Owned. */
    int *hole_span;
    int  hole_n;
    /* ECMAScript §6.1 ECMAScript Language Types: the set of types this value takes in every world, as CTY_*
       bits. Stated only by a mint whose algorithm fixes its result's type (concolic_type_state), and asserted
       there against the example; 0 is unstated, never "no type", so a value no mint typed stays unknown-typed. */
    uint8_t ty;
    /* Whether every reply could only make this a value ECMAScript §25.5.2.1 ParseJSON ( text ) produces, or
       `undefined` where a member is absent: a String, Number, Boolean or null, or an object an ObjectLiteral
       (ECMAScript §13.2.5.5 Runtime Semantics: Evaluation) or an ArrayLiteral (ECMAScript §13.2.4.2 Runtime
       Semantics: Evaluation) defines. Such an object's
       own keys are all Strings (ECMAScript §13.2.5.6 Runtime Semantics: PropertyDefinitionEvaluation makes
       `__proto__` an own data property under ParseJSON) and its [[Prototype]] is its type's intrinsic, so a
       read the own members cannot answer is a read of an intrinsic chain (json_chain_member). Written by
       concolic_mark_json_value at a reply's mint and by concolic_exotic_get on a member; 0 says nothing. */
    uint8_t json_value;
} Concolic;

/* §6.1's eight language types, one bit each, so a mint can state a set (`x - 1` is a Number or a BigInt). */
enum {
    CTY_UNDEFINED = 1u << 0, CTY_NULL = 1u << 1, CTY_BOOLEAN = 1u << 2, CTY_STRING = 1u << 3,
    CTY_SYMBOL = 1u << 4, CTY_NUMBER = 1u << 5, CTY_BIGINT = 1u << 6, CTY_OBJECT = 1u << 7,
    CTY_NUMERIC = CTY_NUMBER | CTY_BIGINT,
};

/* One disposer for an owned list of spelled strings, shared by every record that holds one (a call predicate's
   arguments on the value and on ConcolicPred, a conjunction's conjuncts). These are C strings outside the GC
   walk, so a list freed at only some of its sites leaks. */
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

/* ── The loose equality's holding arm, as a row ──────────────────────────────────────────────────────────────
 * concolic.h says what the row is and why it is not a ConcolicPred. These three functions are its whole
 * ownership contract: several holders keep sets of rows, so a field added to the struct has one place it is
 * copied and one where it is released. */
int concolic_looseeq_same(const ConcolicLooseEq *p, ConcolicLit kind, const char *tok) {
    /* The kind is half the comparison: §7.1.19 ToString ( arg ) maps `undefined`, `null`, `0` and `false` onto
       text that is also a legal String operand, so comparing spellings alone would merge `x == undefined` with
       `x == "undefined"`. pred_new asserts the same pair and concolic_pin refuses to split it. */
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

/* The kind as a report names it (concolic.h says why this is not lit_tag). The switch has no `default`, so a
   kind added to ConcolicLit draws a -Wswitch warning here and reaches the DFAIL below at run time. */
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

/* ── The one encoding ───────────────────────────────────────────────────────────────────────────────────────
 * Every identity this file composes and every constraint key decide.c builds is a tag plus a sequence of
 * fields, each written `<byte length>:<bytes>`:
 *   - no field's contents can spell another field's boundary, so two different sequences never write the same
 *     string. A separator with a forbidden byte (concolic_source_wrap_joint's scheme) cannot work here: a
 *     member is often a property name, which a page may spell with any byte.
 *   - the length is measured and written by the same snprintf, so no buffer can truncate; a truncated key is
 *     two predicates under one name.
 * An absent member makes the whole composition absent (see the struct above).
 * A spelling longer than CONCOLIC_IDENT_SPELL_MAX is replaced by its digest (ident_digest), so every identity is
 * bounded: a member is itself a composition, and a loop that derives a value from itself twice per iteration
 * (`src = src.substring(re.exec(src)[0].length)`) would otherwise double its identity at every iteration. */
#define CONCOLIC_IDENT_SPELL_MAX 256

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

/* The identity of a spelling too long to keep: the composition `#` over the first 128 bits of the spelling's
   SHA-256, in hex. No other composition uses the tag, so a digest never equals a spelled identity, and two
   spellings share one only by a 128-bit collision. It is a function of the spelling alone, so it is the same on
   the minting flow, on a cold-tier resume and in the next session. Takes ownership of `spelled`. */
static char *ident_digest(char *spelled, size_t len)
{
    static const char HEX[] = "0123456789abcdef";
    uint8_t d[SECURE_HASH_MAX_DIGEST];
    char hex[33];
    const char *f[1];
    SecureHash h;
    char *out;
    int i;

    secure_hash_init(&h, SECURE_HASH_SHA256);
    secure_hash_update(&h, (const uint8_t *)spelled, len);
    secure_hash_finish(&h, d, sizeof d);
    free(spelled);
    for (i = 0; i < 16; i++) {
        hex[2 * i] = HEX[d[i] >> 4];
        hex[2 * i + 1] = HEX[d[i] & 15];
    }
    hex[32] = '\0';
    f[0] = hex;
    out = concolic_ident_compose("#", f, 1);
    DCHECK(out != NULL && strlen(out) <= CONCOLIC_IDENT_SPELL_MAX,
           "an identity's digest was itself longer than the bound it replaces a spelling under");
    return out;
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
    if (len > CONCOLIC_IDENT_SPELL_MAX)
        return ident_digest(out, len);
    return out;
}

/* The canonical order over a set of identity strings, kept with the encoding because it is the second half of
   the same rule: the encoding gives two sequences two keys, this gives two spellings of one set (a joint
   provenance's sources, a conjunction's conjuncts) one key, so a flow that decided under one spelling does not
   fork again under another. It orders by `strcmp` alone, and identities are text composed from program facts,
   so the order is the same on the minting flow, on a cold-tier resume and in the next session. Insertion sort;
   `order` receives indices and the members are never moved. */
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

/* The display shape, sized from its parts. concolic_exotic_get's shape is also the field path an @S candidate
   is injected at, so a truncated shape would give two sources one provenance. Measured once, written once,
   freed by the caller. */
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

/* Which kind of concrete operand this is, and whether this engine may spell it at all. Asked here and nowhere
   else, because a comparison spells its other operand twice (as half the predicate's identity and as the value
   the taken arm pins to) and two classifications could disagree. An Object or a Symbol is unspellable:
   §7.1.19 ToString ( arg ) sends an Object to ToPrimitive, which would run the page's own valueOf/toString from
   a C activation with no flow base under it, and throws a TypeError for a Symbol. */
static ConcolicLit operand_kind(JSValueConst v)
{
    if (JS_IsString(v))    return CONCOLIC_LIT_STRING;
    if (JS_IsNumber(v))    return CONCOLIC_LIT_NUMBER;
    if (JS_IsBool(v))      return CONCOLIC_LIT_BOOL;
    if (JS_IsNull(v))      return CONCOLIC_LIT_NULL;
    if (JS_IsUndefined(v)) return CONCOLIC_LIT_UNDEFINED;
    if (JS_IsBigInt(v))    return CONCOLIC_LIT_BIGINT;
    /* An Object or a Symbol: no ConcolicLit spelling. That is about the value, not about whether it can be
       named; an intrinsic is named by its realm slot, which literal_ident reaches separately. */
    return CONCOLIC_LIT_NONE;
}

/* The kind's one-letter field in a composed identity. One speller, because the identity a page's `x === 5`
   composes and the identity a component's declared member composes must be byte-identical, or they are two
   constraint entries over one predicate, each forking the other's settled gate (page_visibility.h relies on
   this). */
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

/* The pinned value, back: the one place a stored (kind, spelling) pair becomes a JSValue again, so the read
   sites cannot disagree about what a pin means. It mints the real value, not a display of it: concretize-on-pin
   decides later branches by running the real predicate, which needs the value the flow proved. A Number comes
   back through §7.1.4 ToNumber ( arg ), the inverse of the §7.1.19 ToString ( arg ) that spelled it, run on
   a String primitive so no page code runs. -0 comes back as +0, which is a witness and not a fabrication:
   §6.1.6.1.20 Number::toString ( x, radix ) spells both zeroes "0" and §7.2.14 IsStrictlyEqual ( x, y ) holds
   for `-0 === 0`. */
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

/* The operand spelled: its §7.1.19 ToString ( arg ), owned by the caller, NULL where it has none. This is also
   the value an equality pins to, and must be: `""+x` is what a pinned source contributes to a URL the page
   builds, so the pin and the identity must be the same bytes.
   The kind comes back with the spelling (`pkind` may be NULL) as an out-parameter, so the caller cannot ask
   operand_kind a second time and hold two classifications of one value. */
static char *literal_tok(JSContext *ctx, JSValueConst v, ConcolicLit *pkind)
{
    ConcolicLit kind = operand_kind(v);
    const char *s;
    char *r;

    if (pkind) *pkind = kind;
    if (kind == CONCOLIC_LIT_NONE) return NULL;
    s = JS_ToCString(ctx, v);
    if (!s) return NULL;
    r = strdup(s);
    CHECK(r, "concolic: OOM spelling a concrete operand");
    JS_FreeCString(ctx, s);
    return r;
}

/* An intrinsic's name, owned by the caller. The one speller, because the name is spent as an operand's identity
   and as its display shape. The registry is QuickJS's (JS_IntrinsicName): a realm's class_proto/class_ctor
   arrays, its global object, and the well-known symbols below JS_ATOM_END. Each is fixed at its definition, so
   the name is reproducible by the replay a resumed flow performs, which an address is not (addresses are reused
   and no park carries one).
   NULL where the value is no intrinsic. A page-created closure, RegExp or ordinary object is named by
   creation_name; literal_ident's named residual lists the classes that have no name source yet. */
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

/* A registered symbol's key, owned by the caller; one speller for intrinsic_name's reason. The registry is the
   standard's (JS_SymbolRegistryKey): §20.4.2.4 Symbol.for ( key ) returns the symbol already registered under a
   key and only otherwise appends one, so the registry is 1:1 and the key is the symbol's whole name, with no
   ordinal needed.
   A key with an embedded NUL is refused, not asserted: the key is page bytes, and a NUL-terminated copy would
   truncate, putting two symbols under one name. Refusing answers absent, so both arms of every branch over
   that symbol stay. The length JS_ToCStringLen returns is what detects it.
   NULL where the value is not a registered symbol: a well-known symbol is intrinsic_name's, and a page's
   `Symbol("x")` is literal_ident's named residual. */
static char *registry_key(JSContext *ctx, JSValueConst v)
{
    JSValue k = JS_SymbolRegistryKey(ctx, v);
    const char *s;
    size_t len;
    char *r;

    if (JS_IsUndefined(k)) return NULL;
    DCHECK(JS_IsString(k),
           "the symbol registry answered with something that is not the key's String — the atom struct IS "
           "that string, so a value of any other type here was composed somewhere other than the registry "
           "and the name about to be built from it names nothing");
    s = JS_ToCStringLen(ctx, &len, k);
    JS_FreeValue(ctx, k);
    if (!s) return NULL;
    if (len != strlen(s)) {   /* an embedded NUL — see above; absent rather than a truncated name */
        JS_FreeCString(ctx, s);
        return NULL;
    }
    r = strdup(s);
    CHECK(r, "concolic: OOM copying a registered symbol's key");
    JS_FreeCString(ctx, s);
    return r;
}

/* A page-created value's creation name, owned by the caller; one speller for intrinsic_name's reason. Unlike
   the sources above it needs an ordinal: a function body is 1:1 with its position (JS_OrphanHash), but a
   factory called three times is one site and three closures, and naming them alike would let iteration 1's
   constraint refine 2 and 3 and lose arms. The ordinal is the running flow's count, minted at the value's
   creation (JSConcolicHooks.mint_ordinal, installed below) and carried by the object.
   One question over several classes, asked once: the per-class site composer lives in the engine (a closure's
   body locator, a RegExp's source and flags, an object's creating code position) and the class namespace rides
   the returned name (`fn@`, `re@`, `ob@`), so this file keeps no list of classes.
   NULL where the engine has no creation name: a class with no site composer, a session that mints no
   ordinals, and a C function, bound function or Proxy, which JS_CreationName refuses (`arr.some(f.bind(this))`
   is ordinary input, so this must not assert on it). */
static char *creation_name(JSContext *ctx, JSValueConst v)
{
    char buf[JS_CREATION_NAME_MAX];
    int n = JS_CreationName(ctx, v, buf, sizeof buf);
    char *r;

    if (n < 0) return NULL;
    DCHECK(n < (int)sizeof buf,
           "a creation name did not fit the buffer quickjs.h's own bound sizes for it — the two are tied by "
           "an assert at the composition there, so this fires only if that bound was widened past this "
           "buffer, and a TRUNCATED name is two page-created values under one constraint key and one "
           "property atom");
    r = strdup(buf);
    CHECK(r, "concolic: OOM copying a creation name");
    return r;
}

/* A concrete operand's identity is its value, and its type is part of that: `x === 5` and `x === "5"` are two
   predicates whose operands print alike. An Object or a Symbol has no identity in that sense (its address does
   not survive a park, and coercing it would run page code from C) unless a name source below can name it;
   otherwise its identity is absent and both arms of a branch over it stay. */
static char *literal_ident(JSContext *ctx, JSValueConst v)
{
    ConcolicLit kind = operand_kind(v);
    const char *f[2];
    char *tok, *r;

    if (kind == CONCOLIC_LIT_NONE) {
        /* An intrinsic is a singleton of its realm and is named by the slot it occupies, so
           `while (n !== Object.prototype)`, the end of every prototype walk, is a branch with an identity. The
           tag is `i`, not `k`: `k` composes a spelled ConcolicLit, and one tag over both would let the string
           `"%Array%"` and %Array% share a constraint key. No pin follows from any name in this arm: literal_tok
           answers NULL, and §7.2.14 IsStrictlyEqual ( x, y ) step 1 is false for every string the solver could
           substitute. A name lets concolic_branch_decided collapse a repeated question; N distinct named
           operands still owe N questions. */
        /* Named residual. Not covered: a page-minted `Symbol("x")`, arrays, and the exotic-state classes (Map,
           Set, Promise), which have no site composer; a branch over one records no constraint and re-forks
           every time. (`{}` and `Object.create(null)` are named.) Next diff: a site composer
           for `Symbol("x")`, an atom above JS_ATOM_END that costs no object a byte; arrays need a site and
           ordinal beside `u.array.count`, which widens every JSObject on a 32-bit build. How its absence shows:
           a rendered shape with `?` at an operand position and a `~` site row climbing while `replayHits` stays
           flat; at the sharpest, concolic_exotic_own_names' `!c->ident` arm aborting on a record whose shape is
           spelled except for one `?` argument, since an absent argument makes the whole call identity absent.
           An object built inside a C builtin (every record `JSON.parse` returns) shares the site of the call,
           so its ordinal does all the naming: locality is lost there, no arm is. */
        char *nm = intrinsic_name(ctx, v);
        const char *nf[1];
        if (nm) {
            nf[0] = nm;
            r = concolic_ident_compose("i", nf, 1);
            free(nm);
            return r;
        }
        /* The tag is the registry's own name, not `i`: a shared tag would let %Symbol.iterator% and a page's
           `Symbol.for("Symbol.iterator")` compose one constraint key. */
        nm = registry_key(ctx, v);
        if (nm) {
            nf[0] = nm;
            r = concolic_ident_compose("Symbol.for", nf, 1);
            free(nm);
            return r;
        }
        /* A page-created value: the first arm whose name is not 1:1 with anything the program states, so it
           carries an ordinal taken at creation (creation_name, JS_CreationName). A value is flow-private when
           made, so the ordinal is the running flow's; an ask-time mint would let two arms of one fork stamp a
           shared value from two counters.
           The tag `fn` covers every class JS_CreationName composes a site for; the class rides the returned
           name (`fn@…#n`, `re@…#n`, `ob@…#n`), so classes cannot collide. It is not renamed to something
           class-neutral because decide.c's decision vector stores the asked question as a content hash of
           this string and parked flows carry it across the cold tier: a rename would orphan every parked arm
           over a page-created value. */
        nm = creation_name(ctx, v);
        if (!nm) return NULL;
        nf[0] = nm;
        r = concolic_ident_compose("fn", nf, 1);
        free(nm);
        return r;
    }
    tok = literal_tok(ctx, v, NULL);   /* the kind is this function's own, read and gated on above */
    if (!tok) return NULL;
    f[0] = lit_tag(kind); f[1] = tok;
    r = concolic_ident_compose("k", f, 2);
    free(tok);
    return r;
}

/* An operand's identity whichever kind it is, always owned by the caller (NULL = unspellable). */
static char *ident_of_operand(JSContext *ctx, JSValueConst v)
{
    if (concolic_is(v)) {
        const char *id = concolic_ident_c(v);
        char *r;
        if (!id) return NULL;
        r = reclaim_strdup(id);   /* a running flow's allocation: it sells the frontier's tail before it fails */
        CHECK(r, "concolic: OOM copying an operand's identity");
        return r;
    }
    return literal_ident(ctx, v);
}

/* A string operand, rendered as the page wrote it. The quotes matter: §7.1.19 ToString ( arg ) spells the
   Number 5 and the String "5" alike, and the display must not be coarser than the identity beside it. The two
   characters that could close the rendering early are escaped, so `f("a\"", "b")` and `f("a", "\"b")` differ.
   Owned. */
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

/* A literal operand as a shape: the token a page writes for it, quoted for a String and suffixed `n` for a
   BigInt, so two operands the identity separates by kind are two shapes. `tok` is borrowed; owned result. */
static char *lit_shape(ConcolicLit kind, const char *tok)
{
    char *r = NULL;

    DCHECK(tok != NULL && kind != CONCOLIC_LIT_NONE,
           "a literal was shaped with no token or no kind — literal_tok answers both or neither, and an operand "
           "with neither renders `?` at its caller");
    switch (kind) {
    case CONCOLIC_LIT_STRING: r = shape_quote(tok); break;
    /* §6.1.6.2 The BigInt Type spells 5n as "5", the Number's spelling, so the `n` keeps the two apart as the
       identity's kind does. */
    case CONCOLIC_LIT_BIGINT: r = shapef("%sn", tok); break;
    case CONCOLIC_LIT_NUMBER:
    case CONCOLIC_LIT_BOOL:
    case CONCOLIC_LIT_NULL:
    case CONCOLIC_LIT_UNDEFINED:
        /* §7.1.19 ToString ( arg ) of each of these is the literal a page writes for it, and no two of the
           four spell one token, so the token alone separates what the identity separates. */
        r = shapef("%s", tok);
        break;
    /* Refused above. Listed so the switch has no `default` and a kind added to ConcolicLit draws a -Wswitch
       warning; the release arm is the same "?" the unspellable path answers. */
    case CONCOLIC_LIT_NONE:
        r = shapef("?");
        break;
    }
    return r;
}

/* One operand's display: the shape twin of ident_of_operand. An unknown renders as its hole shape, a concrete
   operand as the literal the page wrote, an intrinsic as its %-name, and an operand this engine cannot spell as
   "?" (its identity is already absent by concolic_ident_compose's unspellable-member rule).
   Invariant: a shape separates every pair of operands its identity separates. keyname_record spends a shape to
   buy a property atom, so a coarser shape makes two distinct unknowns one slot on the page's object, and its
   assert catches that. Hence the kind is rendered (a quoted String, a BigInt's `n`): `f(x, 5)` and
   `f(x, "5")` are owed two shapes. literal_tok rather than JS_ToCString, for operand_kind's reason. Owned by
   the caller. */
static char *derived_operand_shape(JSContext *ctx, JSValueConst v)
{
    ConcolicLit kind;
    char *tok, *r;

    if (concolic_is(v)) {
        const char *sh = concolic_shape_c(v);
        return shapef("%s", sh ? sh : "{}");
    }
    tok = literal_tok(ctx, v, &kind);
    /* An intrinsic renders as the name literal_ident composed its identity from, in §6.1.7.4 Well-Known
       Intrinsic Objects notation (`%Array.prototype%`), which a reader of the fork census can act on. */
    if (!tok) {
        char *nm = intrinsic_name(ctx, v);
        char *q;
        if (nm) { r = shapef("%s", nm); free(nm); return r; }
        /* A registered symbol renders as the call the page wrote, from the key literal_ident composed its
           identity from. The key is quoted by shape_quote, so the shape separates every pair of keys the
           identity does, and `Symbol.for("k")` stays clear of a String operand's `"k"`. */
        q = registry_key(ctx, v);
        if (q) {
            char *quoted = shape_quote(q);
            r = shapef("Symbol.for(%s)", quoted);
            free(quoted); free(q);
            return r;
        }
        /* A page-created value renders as the creation name literal_ident composed its identity from. The name
           carries its class namespace (`fn@`, `re@`, `ob@`), so it cannot read as an intrinsic, a registered
           symbol, a String or a Number, and a class added to JS_CreationName moves both halves at once. */
        {
            char *cn = creation_name(ctx, v);
            if (cn) { r = shapef("%s", cn); free(cn); return r; }
        }
        return shapef("?");
    }
    r = lit_shape(kind, tok);
    free(tok);
    return r;
}

/* Mint a value derived from an unknown one. `ident` and `example` are consumed, and the candidate substitution
   applies as it does to a source read (see concolic_new). */
static JSValue concolic_derived(JSContext *ctx, const char *shape, const char *src, const char *root,
                                unsigned root_whose, char *ident, JSValue example);
/* …and the same without the substitution, for a value that is not a source read: a comparison result is a
   boolean the operator computed, and handing the attacker's payload back in its place would answer a predicate
   with a string. */
static JSValue concolic_alloc(JSContext *ctx, const char *shape, const char *src, const char *root,
                              unsigned root_whose, char *ident, JSValue example);

/* The per-flow path constraint: one map holding every fact this flow has learned about the unknown input it
   read. Concrete execution grounds all shared and interprocedural state, so only input variables are symbolic.
   Facts are keyed by what they are about: a pin by the source's `src` (what a later read of that source looks
   up), a decided truth by the predicate's key (decide.c composes it from the tested value's identity, the
   operator and both operands), a domain row by the hole key (what the emission has; see concolic_hole_key),
   and an example contradiction by the value's identity. One entry holds whatever its key names; every field
   is per-flow and travels with the entry, so a fork, a suspend and a resume carry one map. */
/* val, valkind: an EQ gate pinned the source to a concrete value on its true arm, so later reads compute the
   real @H value (`/api/admin`, never `/api/{state}.role`): concretize-on-pin. `valkind` is what the bytes
   spell, since §7.1.19 ToString ( arg ) alone cannot tell `undefined` from the nine-character string; pin_mint
   reads the pair, and the two are written and copied together.
   truth: a predicate was already decided in this flow. `if (cfg.admin)` pins nothing but fixes truthiness for
   this flow, and bundles test one flag repeatedly; without this N tests cost 2^N flows whose siblings are
   contradicted. It prunes only a branch on the same predicate the flow already fixed.
   ex_contra: this flow took an arm the value's own example says a real session does not take
   (concolic_contradict_example). */
/* ppin: the pins this flow made on values rooted at this key, kept only where the key is a declared
   principal. `event.origin`, `event.origin.toLowerCase()` and `String(event.origin)` are three identities and
   one demand on the attacker's principal. It is not `val`: writing the lowercased token under `message.origin`
   would make a later read of `event.origin` answer it, a fabricated value. A §7.2.13 IsLooselyEqual ( x, y )
   gate makes the demand without a determination, because its holding arm bounds the operand to a set.
   ptok: the String operands this flow passed to methods it called over the principal at this key, the constants
   message_compose_origins builds the flow's candidate origins from. A path fact, so a fork inherits it and a
   sibling's tests never reach it. */
/* The domain rows, each keyed by the hole and accumulated by conjunction within one flow:
   excl: the tokens an equality gate proved the hole is not, on the arm `val` does not cover. Forced
   multi-path runs both arms of every `x === "admin"`, so this is the second flow's report of its parameter.
   pred: the call predicates branched on, each as the method name, the arguments and the arm taken. A call
   determines neither a value nor an interval, so it shares a record with neither.
   bnd: the interval an ordering gate narrowed the hole to. A pointer, because most entries carry none. The two
   sides conjoin: `x > 5 && x < 100` keeps both, each narrowed (max for lo, min for hi), and a tie between an
   inclusive and an exclusive bound keeps the exclusive one.
   leq: the loose equalities that held, each as the operand the page wrote and its kind. A strict equality's
   holding arm determines a value (`val`); a loose one's (§7.2.13 IsLooselyEqual ( x, y )) bounds the operand
   to a set only that algorithm can read; both failing arms are `excl`. An exact repeat adds nothing. */
/* types_out: the types (CONCOLIC_TYF_* bits) this flow proved the source at this key does not take — its type
   facet, filed on the operand's own key by concolic_equality_facet from a typeof equality's row or from the steps
   of §7.2.13 IsLooselyEqual / §7.2.14 IsStrictlyEqual over the token; cty_of_operand narrows by it. */
/* adec, adec_what, adec_arm, adec_neg: this flow took an arm no observation of the run established (its
   example contradicts it, or it has none) at the decision this key names, and the arm determined no value (a pin states the ones that did). `adec` is the decision as the
   page wrote it; for a branch `adec_arm` is the predicate's arm and `adec_neg` the polarity `adec` reads it in,
   for an outcome `adec_arm` is the completion. Written once per key by concolic_note_assumed_branch/_outcome. */
/* One side of an interval, with its spelling. The number is what a merge orders bounds by; the text is the
   page's own §6.1.6.1.20 Number::toString ( x, radix ) of the literal, so nothing re-spells a double and
   reports a bound the source does not contain. */
typedef struct { double num; char *txt; signed char present; signed char inclusive; } BoundSide;
typedef struct { BoundSide lo, hi; } Bound;
typedef struct { char *key; char *val; signed char valkind; char **excl; int nexcl; Bound *bnd;
                 ConcolicPred *pred; int npred; ConcolicLooseEq *leq; int nleq;
                 ConcolicPrincipalPin *ppin; int nppin; char **ptok; int nptok;
                 signed char truth; signed char ex_contra;
                 char *adec; int adec_arm; signed char adec_what; signed char adec_neg;
                 unsigned short types_out; } Cons;

static void bound_side_free(BoundSide *s) { free(s->txt); s->txt = NULL; s->present = 0; }
/* The one copy of one side, used by the fork's copy-up; a field added to BoundSide must be copied here or a
   sibling inherits half a fact. */
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

/* One free for one entry, shared by the live head and a frozen segment, so a field added to the struct is freed
   in both. */
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
    for (i = 0; i < e->nppin; i++) concolic_principal_pin_release(&e->ppin[i]);
    free(e->ppin);
    for (i = 0; i < e->nptok; i++) free(e->ptok[i]);
    free(e->ptok);
    free(e->adec);
}
/* …and one measurement: the freeze's byte census reads exactly the fields the free above disposes of. */
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
    n += (long)e->nppin * (long)sizeof(ConcolicPrincipalPin);
    for (i = 0; i < e->nppin; i++)
        n += (long)strlen(e->ppin[i].tok) + 1 + (e->ppin[i].subject ? (long)strlen(e->ppin[i].subject) + 1 : 0);
    n += (long)e->nptok * (long)sizeof(char *);
    for (i = 0; i < e->nptok; i++) n += (long)strlen(e->ptok[i]) + 1;
    if (e->adec) n += (long)strlen(e->adec) + 1;
    return n;
}

/* The constraint is a mutable head over immutable, refcounted, structurally shared segments: the same
   primitive as cow.c's `CowSeg` and dom_cow.c's `DomSeg`. A frozen segment is never written again, so a fork
   hands the sibling the parent's whole knowledge by taking one reference on the chain (O(1)), and what each
   flow learns afterwards lives in its own head. A fork therefore costs what the arm has learned, not its whole
   history. A write to a key the chain already holds copies that entry up into the head (`cons_entry`), never
   writing through into a segment a sibling reads. */
/* `bytes` is the segment's cost, computed once at the freeze, so a census is not O(chain). The cold tier reads
   it, counting a shared segment once for the whole frontier, never once per flow. */
typedef struct ConsSeg { Cons *e; int n; int *hash; int hash_cap; struct ConsSeg *base; int refcount; long bytes; } ConsSeg;
/* The frozen constraint chain's own census, the twin of cow.c's and dom_cow.c's, counted where a segment's
   lifetime begins and ends. */
static long g_cons_seg_live, g_cons_seg_entries_live, g_cons_seg_bytes_live;
void concolic_chain_stats(long *segs, long *entries, long *bytes) {
    if (segs) *segs = g_cons_seg_live;
    if (entries) *entries = g_cons_seg_entries_live;
    if (bytes) *bytes = g_cons_seg_bytes_live;
}

static Cons    *g_pins = NULL;  static int g_pins_n = 0, g_pins_cap = 0;   /* the running flow's head */
static int     *g_pins_hash = NULL; static int g_pins_hash_cap = 0;        /* …and its index (key -> idx+1) */
static ConsSeg *g_pins_base = NULL;                                        /* the frozen chain under it */
/* Whether any flow in this agent has ever contradicted an example: a one-way latch, never cleared while the
   agent lives. It can only make the lookup below run when it need not, never skip it when needed, so it is a
   fast path that cannot become a correctness question; a per-flow version would have to be inherited at every
   fork, carried across a freeze and rebuilt by the cold tier. It spares every example read (two per `+` over
   an unknown) a cons_lookup, which probes the head and every frozen segment under it. */
static int g_ex_contra_any;
/* Whether any flow in this agent has ever pinned a value: the sibling latch, for the same cost. concolic_example
   asks concretize-on-pin of every value whose `src` names it. Set at the one line that writes a pin value and
   cleared only with the agent; a pin chain never arrives from outside the agent (cold.c restores a resumed
   flow with `concolic_pins_blob_empty` and it re-derives by replaying its decision vector), so a set entry
   under a clear latch cannot exist. */
static int g_pin_any;

/* Above zero while an assert reads the store: a read made only to check an invariant prices nothing, so a dev
   build retires the same work as a release one. */
static int g_cons_unpriced;
/* FNV-1a over a constraint key, priced by its bytes: the probe is the store's one primitive and its cost is the
   key's length, which grows with the path that spelled it. */
static uint32_t cons_hash(const char *k) {
    const char *p = k;
    uint32_t h = 2166136261u;
    while (*p) { h ^= (unsigned char)*p++; h *= 16777619u; }
    if (!g_cons_unpriced) thread_work_charge(THREAD_WORK_CONSTRAINT, (uint64_t)(p - k));
    return h;
}
/* The one probe, over the head's index or a segment's (a segment's index is the head's, handed over unchanged
   at the freeze). Returns the entry index or -1. */
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
    /* Sized in a local and published after: the allocation can sell a flow (solver/reclaim.h), and a `cap`
       advertising the new size over the old table would be an out-of-bounds read for a pin lookup made during
       the sale. */
    while (nc < g_pins_n * 2) nc *= 2;
    nh = reclaim_realloc(g_pins_hash, (size_t)nc * sizeof(int));
    CHECK(nh, "concolic: OOM path-constraint index");
    g_pins_hash = nh; g_pins_hash_cap = nc;
    memset(g_pins_hash, 0, (size_t)g_pins_hash_cap * sizeof(int));
    for (i = 0; i < g_pins_n; i++) cons_hash_put(i);
}
/* The whole constraint this flow can see, nearest-first: the head, then each frozen segment from the newest
   down. A head entry shadows the segment entry it was copied up from, so the fact this arm narrowed answers. */
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
/* The writable entry for `key`: the head's, copying the chain's fact up into the head the first time this flow
   narrows one it inherited. Every field copies up: a fact dropped here would vanish whenever a context switch
   fell between the gate that proved it and the request that carries the value. */
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
    /* The kind with the bytes, under the same condition: a copy-up without it would hand the sibling a spelling
       pin_mint reads back as CONCOLIC_LIT_NONE, which it refuses. */
    g_pins[g_pins_n].valkind = (below && below->val) ? below->valkind : (signed char)CONCOLIC_LIT_NONE;
    /* The exclusions, the facts below, and every later field copy up for the reason stated above. */
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
    g_pins[g_pins_n].bnd = NULL;
    if (below && below->bnd) {
        Bound *nb = malloc(sizeof *nb);
        CHECK(nb, "concolic: OOM copying an inherited bound");
        bound_side_copy(&nb->lo, &below->bnd->lo);
        bound_side_copy(&nb->hi, &below->bnd->hi);
        g_pins[g_pins_n].bnd = nb;
    }
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
    g_pins[g_pins_n].ppin = NULL;
    g_pins[g_pins_n].nppin = 0;
    if (below && below->nppin) {
        int k;
        g_pins[g_pins_n].ppin = malloc((size_t)below->nppin * sizeof(ConcolicPrincipalPin));
        CHECK(g_pins[g_pins_n].ppin, "concolic: OOM copying an inherited principal pin set");
        for (k = 0; k < below->nppin; k++)
            concolic_principal_pin_copy(&g_pins[g_pins_n].ppin[k], &below->ppin[k]);
        g_pins[g_pins_n].nppin = below->nppin;
    }
    g_pins[g_pins_n].ptok = NULL;
    g_pins[g_pins_n].nptok = 0;
    if (below && below->nptok) {
        int k;
        g_pins[g_pins_n].ptok = malloc((size_t)below->nptok * sizeof(char *));
        CHECK(g_pins[g_pins_n].ptok, "concolic: OOM copying an inherited principal operand set");
        for (k = 0; k < below->nptok; k++) {
            g_pins[g_pins_n].ptok[k] = strdup(below->ptok[k]);
            CHECK(g_pins[g_pins_n].ptok[k], "concolic: OOM copying an inherited principal operand");
        }
        g_pins[g_pins_n].nptok = below->nptok;
    }
    g_pins[g_pins_n].truth = below ? below->truth : -1;
    g_pins[g_pins_n].ex_contra = below ? below->ex_contra : 0;
    g_pins[g_pins_n].adec = (below && below->adec) ? strdup(below->adec) : NULL;
    CHECK(!(below && below->adec) || g_pins[g_pins_n].adec, "concolic: OOM copying an inherited assumed decision");
    g_pins[g_pins_n].adec_what = below ? below->adec_what : (signed char)CONCOLIC_DECISION_NONE;
    g_pins[g_pins_n].adec_arm = below ? below->adec_arm : 0;
    g_pins[g_pins_n].adec_neg = below ? below->adec_neg : 0;
    g_pins[g_pins_n].types_out = below ? below->types_out : 0;
    g_pins_n++;
    if (!g_pins_hash || g_pins_hash_cap < g_pins_n * 2) cons_hash_rebuild();
    else cons_hash_put(g_pins_n - 1);
    DCHECK(cons_index_find(g_pins, g_pins_hash, g_pins_hash_cap, key) == g_pins_n - 1,
           "a constraint entry is not findable through the index that was just given it — a later read of the "
           "same fact would fork a branch this flow has already decided");
    return &g_pins[g_pins_n - 1];
}
/* Member `i` of a root, and whether a member is a declared principal: defined with the registry below,
   declared here because the pin is their first reader. */
static int root_member(const char *root, int i, const char **p, size_t *n);
static int src_principal(const char *p, size_t n);

/* The principal pin's ownership contract (concolic.h): one copy and one release, shared by the constraint
   head's copy-up and solve.c's search record. */
void concolic_principal_pin_copy(ConcolicPrincipalPin *dst, const ConcolicPrincipalPin *src) {
    DCHECK(src->tok != NULL && src->kind != (signed char)CONCOLIC_LIT_NONE,
           "a principal pin with no operand was copied — the store writes one only where the operand is spelled");
    dst->tok = strdup(src->tok);
    CHECK(dst->tok, "concolic: OOM copying the origin a flow pinned a principal to");
    dst->subject = src->subject ? strdup(src->subject) : NULL;
    CHECK(!src->subject || dst->subject, "concolic: OOM copying the identity a principal pin compared");
    dst->kind = src->kind;
    dst->loose = src->loose;
}

void concolic_principal_pin_release(ConcolicPrincipalPin *p) {
    free(p->tok);
    free(p->subject);
    p->tok = NULL; p->subject = NULL;
}

/* One pin on one principal member's entry, appended unless the flow already made exactly this demand. */
static void principal_pin_add(const char *member, const char *subject, ConcolicLit kind, const char *tok,
                              int loose) {
    Cons *c = cons_entry(member);
    ConcolicPrincipalPin *a;
    int i;

    for (i = 0; i < c->nppin; i++) {
        const ConcolicPrincipalPin *q = &c->ppin[i];
        if (q->kind == (signed char)kind && q->loose == loose && !strcmp(q->tok, tok) &&
            (q->subject ? subject && !strcmp(q->subject, subject) : !subject))
            return;
    }
    a = reclaim_realloc(c->ppin, (size_t)(c->nppin + 1) * sizeof *a);
    CHECK(a, "concolic: OOM recording a pin on an attacker's principal");
    c->ppin = a;
    a[c->nppin].tok = strdup(tok);
    a[c->nppin].subject = subject ? strdup(subject) : NULL;
    CHECK(a[c->nppin].tok && (!subject || a[c->nppin].subject),
          "concolic: OOM recording a pin on an attacker's principal");
    a[c->nppin].kind = (signed char)kind;
    a[c->nppin].loose = (signed char)loose;
    c->nppin++;
}

/* The pin, and the principal mark beside it (see concolic.h). They are written together because nothing
   downstream can recover the mark: by the time a value reaches a sink the pinned identity is gone and only the
   root survives the derivations. The root is marked even when it equals the identity (`event.origin === X`,
   the spelling bundles write); the second write is then a no-op through `cons_entry`. */
int concolic_pin_store(const char *src, const char *root, ConcolicLit kind, const char *val,
                       JSConcolicEqOp algo) {
    Cons *c;
    int wrote = 0;
    /* The two writes are one act and two decisions, separated by the algorithm (concolic.h reads §7.2.13
       IsLooselyEqual ( x, y ) step by step). A holding `===` determines the operand (§7.2.14 IsStrictlyEqual
       ( x, y ) step 1: "If SameType(x, y) is false, return false"); a holding `==` leaves a set, so pinning it
       would pick a witness that concretize-on-pin then decides every later branch from.
       The demand survives both, so this is not an early return. `e.origin == TRUSTED` is still a demand on the
       principal, unforgeable cross-origin because two Strings go straight to §7.2.14 by §7.2.13's step 1;
       refusing the pin would hand every loose origin check to a delivery from an attacker origin. */
    DCHECK(algo == JS_CONCOLIC_EQ_LOOSE || algo == JS_CONCOLIC_EQ_STRICT ||
           algo == JS_CONCOLIC_EQ_SAME_VALUE_ZERO,
           "an arm recorded an equality observation naming an algorithm quickjs.h does not declare — §7.2.13 "
           "and §7.2.14 differ on exactly whether the holding arm determined a value, so an unnamed one has "
           "no answer to the only question this function asks of it");
    /* The kind is asserted at the mint, where the store can still refuse: a reader meeting a spelling with no
       kind could only guess "string" and compute nine characters for `x === undefined`. The kind travels from
       operand_kind through pred_new and concolic_cmp so this line has a fact to check. */
    DCHECK(kind != CONCOLIC_LIT_NONE,
           "an arm pinned a source to an operand this engine cannot spell — literal_tok answers NULL for an "
           "Object and a Symbol and the hook mints no token for one, so a pin arriving here with no kind is a "
           "token composed somewhere that does not go through that classification");
    /* Kinds the store may not hold leave no entry at all: the store accepts exactly what pin_mint can hand back,
       which makes that function's DFAILF unreachable. The guard is code, not behind the DCHECK, so a release
       build refuses too. BIGINT is concolic.h's named residual; NONE is the dev abort above.
       This refusal precedes the root mark as well: the store cannot express the operand at all, so a mark alone
       would record a demand for a principal nothing could later name. The loose case below differs: the token
       is expressible and the demand observed; only the determination is missing. */
    if (kind == CONCOLIC_LIT_BIGINT || kind == CONCOLIC_LIT_NONE) return 0;
    /* The determination, which only §7.2.14 makes. §7.2.13's holding arm leaves the operand in a set
       (`x == undefined` admits null by step 2, `x == 0` admits a numeric-looking String by step 6 and a Boolean
       by step 9, and an Object reaches the page's own ToPrimitive by step 12), so writing one member here
       would be choosing a witness. @H never invents a value.
       The narrowing is not lost: a pin is keyed by `src`, what a later read concretizes through, while the loose
       fact is a domain keyed by the hole (concolic_cmp_subject states the pair), which decide.c files through
       `concolic_looseeq` on the same arm. An unpinned source forks again at its next gate, which is sound. */
    /* A concolic carries a provenance and a root together (concolic_alloc asserts it), and the root says whose
       bytes were demanded, which the unforgeable-origin rule decides by. Asked above the split, because both
       algorithms make the demand. */
    DCHECK(root != NULL, "an equality pinned an attacker value that carries no delivery ROOT — the root is "
                         "what says WHOSE bytes were demanded, and §Attacker-sources' unforgeable-origin rule "
                         "is decided by exactly that");
    /* §7.2.10 SameValueZero ( x, y ) has §7.2.14's step 1 and differs only over NaN, so it determines too. */
    if ((algo == JS_CONCOLIC_EQ_STRICT || algo == JS_CONCOLIC_EQ_SAME_VALUE_ZERO) && src) {   /* NULL: the demand alone */
        c = cons_entry(src);
        free(c->val); c->val = strdup(val); CHECK(c->val, "concolic: OOM pin value");
        c->valkind = (signed char)kind;
        g_pin_any = 1;   /* the one write of a pin value, so the one place the latch can be raised */
        wrote = 1;
        /* `c` is dead from here: cons_entry may grow the head by realloc, so nothing below reaches through it. */
    }
    /* Filed on every principal member, because a root may name a set. concolic_principal_pins looks each
       declared principal up by its own name, so a pin under a joint key would leave `message.origin` unpinned
       for a flow that pinned a value derived from it, and the finding would offer a delivery from an
       attacker-owned origin that cannot fire. A member that is no principal carries nothing: the attacker
       writes it, and its pin is the ordinary solved case. `cons_entry` may grow the head, so nothing holds
       its result across calls. */
    if (root) {
        const char *mp;
        size_t mn;
        int mi;

        for (mi = 0; root_member(root, mi, &mp, &mn); mi++) {
            char *one;

            if (!src_principal(mp, mn)) continue;
            one = reclaim_malloc(mn + 1);
            CHECK(one, "concolic: OOM naming a member of a pinned value's delivery ROOT — a member that "
                       "could not be spelled is a principal demand this flow made and nothing can answer for");
            memcpy(one, mp, mn);
            one[mn] = '\0';
            principal_pin_add(one, src, kind, val, algo == JS_CONCOLIC_EQ_LOOSE);
            free(one);
        }
    }
    return wrote;
}
/* The hole key (see concolic.h). One speller, because it is read at two ends that would otherwise normalise the
   same hole differently and never meet. */
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

/* The negative half of the equality observation (see concolic.h). The token is the concrete side of a predicate
   the page wrote, so nothing is chosen: this records that the flow took the arm where the value is not it. */
/* The constraint key an exclusion set lives under. Composed under its own tag rather than the raw hole,
   because the map also holds raw `src` keys and composed branch identities, and a member path could spell a
   declared source. The tagged encoding keeps an `excl` key from equalling either. Caller frees. */
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

/* The constraint key a loose-equality set lives under, with its own tag for excl_key's reason: `!= "admin"`,
   `> 5`, `startsWith("/api")` and `== 0` are four claims about one hole. Distinct from `excl` in particular
   because those two are the same operand and operator on opposite arms, which a shared key would merge into
   "the value both is and is not the token". Caller frees. */
static char *leq_key(const char *hole) {
    const char *f[1];
    f[0] = hole;
    return concolic_ident_compose("looseeq", f, 1);
}

/* The holding arm of a loose equality (see concolic.h). Nothing is chosen: the operand is the concrete side the
   page wrote and the arm is the one this run took, so this records that the page's `==` held of this value. No
   value is determined. */
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
    /* The operand and its kind are one argument, asserted where they meet as concolic_pin does: `x == undefined`
       and `x == "undefined"` spell alike but their holding sets under §7.2.13 IsLooselyEqual ( x, y ) share
       nothing (the first admits undefined by step 1 and null by step 2, the second neither). Unlike
       concolic_pin this accepts a BigInt: that refusal exists because a pin must be minted back into a value,
       and a row is only printed. */
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
    {   /* The one deep copy, through concolic_looseeq_copy like every other holder; the row is assembled on
           the stack so the copy has one shape to take. */
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

/* The constraint key an interval lives under, with its own tag for excl_key's reason: `≠ "admin"` and `> 5`
   are different claims about one hole. Caller frees. */
static char *bnd_key(const char *hole) {
    const char *f[1];
    f[0] = hole;
    return concolic_ident_compose("bound", f, 1);
}

/* One side narrowed. Within one flow the observations conjoin (`x > 5 && x < 100`), and each side keeps the
   tighter of what it held and what arrived. At the same number an exclusive bound beats an inclusive one,
   because `x > 5` says strictly more than `x >= 5`. */
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

/* The constraint key a call-predicate set lives under, with its own tag for excl_key's reason. Caller frees. */
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
    {   /* The one deep copy, through concolic_pred_copy like every other holder; the borrowed row is
           assembled on the stack so the copy has one shape to take. */
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
   recursion: the chain's depth is the fork depth, which an unknown-length walk makes as deep as the walk is
   long, and C stack cannot be parked. The twin of cow_seg_unref / dom_seg_unref. */
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

/* Whether the running flow's own candidate bytes have entered the program yet (concolic.h says what this is a
   precondition of). Set only by concolic_deliver. Per flow like the pins, and carried at four points in this
   file: concolic_pins_suspend copies it into the blob (so a fork's sibling starts from the parent's value at
   the branch), concolic_pins_resume installs a parked one, concolic_pins_blob_empty answers 0, and
   concolic_clear_pins resets it for a fresh flow.
   It does not cross the cold tier: cold.c gives a resumed candidate an empty blob because the flow replays the
   document from the baseline (park_rec_cand parks a recipe, not a frame chain) and re-earns this at its own
   source read. */
static int g_cand_delivered;

/* How many creation names this flow has minted: the ordinal half of a page-created value's name (QuickJS's
   JSConcolicHooks.mint_ordinal / JS_CreationName).
   Per flow, because it is a fact about the executed prefix: a replay reproduces it by reproducing the prefix,
   so a name means the same on the flow that minted it and on the flow the cold tier resumes. A per-agent
   counter would depend on which flows were interleaved, and every replay would diverge at its first name.
   One counter across all sites rather than one per site: it makes every (site, ordinal) pair a flow mints
   distinct, which is what the identity needs, without a per-site map that would itself have to fork, park and
   resume. What per-site counters would add is locality (a divergence would shift one site's ordinals, not
   every later one's).
   One flow never mints one ordinal twice. Two siblings minting the same one is not a collision: a value is
   flow-private at creation, and a value created before the fork carries one name in both arms. */
static uint32_t g_mint_ord;

/* The per-flow concolic state's clear, not only the pins'. Callers: decide_enter gives a fresh flow an empty
   state, flow.c's teardown clears when no flow is left to own it, and concolic_pins_resume clears before
   installing a parked one. Each means "nothing this component holds is this flow's yet", so every per-flow fact
   of this component is reset here. The name says `pins` after flow.h's `pin_blob` slot. */
void concolic_clear_pins(void) {
    int i;
    for (i = 0; i < g_pins_n; i++) cons_entry_free(&g_pins[i]);
    free(g_pins); g_pins = NULL; g_pins_n = g_pins_cap = 0;
    free(g_pins_hash); g_pins_hash = NULL; g_pins_hash_cap = 0;
    cons_seg_unref(g_pins_base); g_pins_base = NULL;
    g_cand_delivered = 0;
    /* The mint counter too: left set, a flow would mint an ordinal a sibling already spent and put two values
       under one name. A fresh flow has created nothing. */
    g_mint_ord = 0;
}

/* Per-flow constraint state is swappable so interleaved flows keep their own narrowing: suspend freezes the
   live head onto the chain and returns a reference to it, resume installs a parked chain as the live one. A
   blob is one flow's state parked while another runs, and the blob a fork takes is the sibling's whole
   starting knowledge, so one function serves both.
   It carries every per-flow fact this component holds: the chain (`seg`), `delivered`, and `mint_ord`, so the
   sibling inherits the parent's count at the branch and the two arms mint forward independently. The struct
   is named after flow.h's `pin_blob` slot; its content is this component's. */
typedef struct { ConsSeg *seg; int delivered; uint32_t mint_ord; } PinBlob;
void *concolic_pins_suspend(void) {
    PinBlob *b = reclaim_malloc(sizeof *b);
    CHECK(b, "concolic: the path constraint could not be parked — the frontier never drops a work item, and a "
             "flow whose constraint is lost would re-fork every branch it has already decided");
    /* Taken, not moved: this serves a fork as well as a park, so the parent keeps holding what it had. */
    b->delivered = g_cand_delivered;
    b->mint_ord = g_mint_ord;   /* taken, not moved, for the same reason */
    if (g_pins_n == 0) {
        /* Nothing learned since the last freeze: the blob is one more reference on the chain the flow
           already stands on. Otherwise a park/resume pair with no writes between would push an empty segment
           per switch and the chain's depth would count switches rather than forks. */
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
    /* After the clear, which resets every per-flow fact this component holds and would erase a restore made
       before it. */
    g_cand_delivered = b->delivered;
    g_mint_ord = b->mint_ord;
}
/* A constraint that has learned nothing, for a flow that resumes without having run in this session (the cold
   tier's). Such a flow is not fresh (it stands on a recorded decision chain, so it is resumed, not entered)
   and holds no knowledge (it re-derives every fact as it replays the gates that produced it). The resume path
   rightly asserts a blob exists; this satisfies it with an empty chain rather than a NULL. */
void *concolic_pins_blob_empty(void) {
    PinBlob *b = reclaim_malloc(sizeof *b);
    CHECK(b, "concolic: a resumed flow's empty path constraint could not be allocated");
    b->seg = NULL;
    /* Nothing delivered either: the flow replays from the baseline and the substitution happens again in this
       session. */
    b->delivered = 0;
    /* Nothing minted: the replay re-creates every value it named in the same order, so a carried count would
       shift every name off the one it recorded and every replayed question would miss its key. */
    b->mint_ord = 0;
    return b;
}

void concolic_pins_blob_free(void *blob) {
    PinBlob *b = blob; if (!b) return;
    cons_seg_unref(b->seg);
    free(b);
}

/* The ordinal half of a creation name, answered at the engine's creation seam (JSConcolicHooks.mint_ordinal).
   Nonzero is the contract, hence the pre-increment: QuickJS reads 0 as "no creation name", so the first mint is
   1. Apart from the per-flow reset and restore points above, this is the counter's only writer. */
static uint32_t concolic_mint_ordinal_hook(JSContext *ctx) {
    (void)ctx;
    DCHECK(g_mint_ord != UINT32_MAX,
           "a flow's creation-ordinal counter reached its last value — the next mint would WRAP to 0, which "
           "this engine reads as 'no name', and the one after it would start re-issuing ordinals this flow "
           "has already spent, putting two values under one constraint key and losing an arm rather than a "
           "fork. What this bounds is the number of NAMEABLE VALUES one flow creates — it read 'the number of "
           "closures' while the counter was spent only on closures and RegExps, and a plain object now spends "
           "one too, so the population is every object the page makes and is larger by orders of magnitude. "
           "Reaching it is a statement about the width of this counter and never a reason to reset it; what it "
           "asks for is a 64-bit ordinal, which fits all three carriers without widening JSObject's union");
    return ++g_mint_ord;
}
/* ECMAScript §21.3.2.28 Math.random ( ) (JSConcolicHooks.random). Each call spends one of this flow's ordinals,
   so its identity (`Math.random()#n`) is distinct from every other call's and a replay of the path names it
   again; the address shape (`{Math.random()}`) is one for every call. The example is SplitMix64 over the
   ordinal, scaled onto [+0, 1) by its high 53 bits: a function of the path and never of a clock. A host that
   does not explore answers the example itself, which is the browser's Number. */
static JSValue concolic_random_hook(JSContext *ctx) {
    uint32_t ord = concolic_mint_ordinal_hook(ctx);
    uint64_t z = (uint64_t)ord * 0x9E3779B97F4A7C15ull;
    char src[32];
    int w;
    double ex;

    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z ^= z >> 31;
    ex = (double)(z >> 11) * 0x1p-53;
    DCHECK(ex >= 0.0 && ex < 1.0, "Math.random's example left [+0, 1)");
    if (!concolic_is_exploring())
        return JS_NewFloat64(ctx, ex);
    w = snprintf(src, sizeof src, "Math.random()#%u", ord);
    DCHECK(w > 0 && (size_t)w < sizeof src, "a Math.random call's provenance did not fit its buffer");
    (void)w;
    return concolic_new(ctx, "{Math.random()}", src, CONCOLIC_WHOSE_WORLD, JS_NewFloat64(ctx, ex));
}

/* The pin this flow holds for `src`, as the value it is. The in-file read sites take a JSValue, never bytes, so
   they cannot decide differently what bytes mean. JS_UNINITIALIZED = no pin, distinct from a pin of
   `undefined`, which must answer undefined. */
static JSValue pin_of(JSContext *ctx, const char *src) {
    const Cons *c = cons_lookup(src);
    if (!c || !c->val) return JS_UNINITIALIZED;
    return pin_mint(ctx, (ConcolicLit)c->valkind, c->val);
}

static JSClassID g_concolic_class = 0;   /* runtime-allocated; 0 until concolic_init */

/* The whose-mask of an operand: the one in-file read, spelled once because every derivation that threads a root
   threads this beside it. Zero for a non-concolic, like concolic_root_c's NULL: a concrete operand contributes
   no root, so a union over a mixed operand list needs no per-operand guard. */
static unsigned root_whose_of(JSValueConst v)
{
    const Concolic *c = g_concolic_class ? JS_GetOpaque(v, g_concolic_class) : NULL;
    return c ? c->root_whose : 0u;
}

/* The mask for a derivation whose root fell back to its own shape. Four derivations below spell their root as
   `root ? root : shape`: no operand carried a provenance, so the value is its own root, concolic_alloc's pair
   assert requires a nonzero mask, and the operands' union is empty. UNSTATED is the true answer there: nothing
   has stated whose this unknown is (the structured-clone rebuild gives the same answer for the same reason). */
static unsigned root_whose_or_unstated(unsigned m)
{
    return m ? m : (1u << CONCOLIC_WHOSE_UNSTATED);
}

/* ---- the affine facet (ConcolicAffine) ---------------------------------------------------------------------- */

/* A term is at most 2^32 in magnitude: every term is minted from a Web IDL `long` (2^31) less an integer the
   mint asserts is within 2^31. A form whose bound exceeds 2^53 leaves the integers a double holds exactly. */
#define AFF_TERM_MAX  4294967296.0
#define AFF_EXACT_MAX 9007199254740992.0

static void aff_free(ConcolicAffine *a)
{
    int i;

    if (!a) return;
    for (i = 0; i < a->n; i++) free(a->term[i]);
    free(a->term);
    free(a->k);
    free(a);
}

static const ConcolicAffine *aff_of(JSValueConst v)
{
    const Concolic *c = g_concolic_class ? JS_GetOpaque(v, g_concolic_class) : NULL;

    return c ? c->aff : NULL;
}

/* One operand of an affine combination: a value carrying the facet, or a concrete Number that is an integer
   the bound admits. Anything else (a String, a concolic with no facet, a fraction) has no affine form.
   No value with a form is ever -0, which is what makes a cancelled form exactly `c`: §6.1.6.1.7 Number::add
   gives -0 only for two -0 operands and Number::subtract only for -0 less +0. So -0 is no operand here, and
   the operators that turn +0 into -0 (negation, a product by a negative number) leave no form. */
typedef struct { const ConcolicAffine *form; double c; } AffOperand;

static int aff_operand(JSContext *ctx, JSValueConst v, AffOperand *o)
{
    double d = 0.0;

    o->form = NULL;
    o->c = 0.0;
    if (concolic_is(v)) return (o->form = aff_of(v)) != NULL;
    if (!JS_IsNumber(v) || JS_ToFloat64(ctx, &d, v) != 0) return 0;
    if (!isfinite(d) || d != trunc(d) || fabs(d) > AFF_EXACT_MAX || (d == 0.0 && signbit(d))) return 0;
    o->c = d;
    return 1;
}

/* `ka·a + kb·b`, both integers. Returns 1 with `*out` set to the new form, 0 with `*out` NULL and `*konst` set
   when every term cancels, and -1 when the result leaves the bound. The term lists are sorted, so this is a
   merge. */
static int aff_lin(const AffOperand *a, double ka, const AffOperand *b, double kb, ConcolicAffine **out,
                   double *konst)
{
    int na = a->form ? a->form->n : 0, nb = b->form ? b->form->n : 0, i = 0, j = 0, n = 0;
    double c = ka * (a->form ? a->form->c : a->c) + kb * (b->form ? b->form->c : b->c), bound;
    ConcolicAffine *r;

    DCHECK(ka == trunc(ka) && kb == trunc(kb), "an affine combination was asked for a fractional coefficient");
    *out = NULL;
    *konst = 0.0;
    bound = fabs(c);
    r = reclaim_calloc(1, sizeof *r);
    CHECK(r, "concolic: OOM composing an affine form");
    r->term = reclaim_malloc((size_t)(na + nb + 1) * sizeof *r->term);
    r->k = reclaim_malloc((size_t)(na + nb + 1) * sizeof *r->k);
    CHECK(r->term && r->k, "concolic: OOM composing an affine form's terms");
    while (i < na || j < nb) {
        int cmp = i >= na ? 1 : j >= nb ? -1 : strcmp(a->form->term[i], b->form->term[j]);
        const char *t = cmp <= 0 ? a->form->term[i] : b->form->term[j];
        double k = (cmp <= 0 ? ka * a->form->k[i++] : 0.0) + (cmp >= 0 ? kb * b->form->k[j++] : 0.0);

        if (k == 0.0) continue;
        r->term[n] = strdup(t);
        CHECK(r->term[n], "concolic: OOM copying an affine term");
        r->k[n++] = k;
        bound += fabs(k) * AFF_TERM_MAX;
    }
    r->n = n;
    r->c = c;
    if (bound > AFF_EXACT_MAX) { aff_free(r); return -1; }
    if (n == 0) { aff_free(r); *konst = c; return 0; }
    *out = r;
    return 1;
}

/* The identity of a value with a form: the form itself, so two derivations of one function name one value.
   Heap string the caller owns. */
static char *aff_ident(const ConcolicAffine *a)
{
    const char **f = reclaim_malloc((size_t)(2 * a->n + 1) * sizeof *f);
    char **owned = reclaim_malloc((size_t)(a->n + 1) * sizeof *owned);
    char *id;
    int i;

    CHECK(f && owned, "concolic: OOM naming an affine form");
    owned[a->n] = shapef("%.17g", a->c);
    f[0] = owned[a->n];
    for (i = 0; i < a->n; i++) {
        owned[i] = shapef("%.17g", a->k[i]);
        f[1 + 2 * i] = a->term[i];
        f[2 + 2 * i] = owned[i];
    }
    id = concolic_ident_compose("aff", f, 2 * a->n + 1);
    for (i = 0; i <= a->n; i++) free(owned[i]);
    free(owned);
    free(f);
    return id;
}

/* Hang `a` on `v`'s record, consumed. `v` may be a plain value — a candidate re-fire delivers one — and the form
   is then dropped with nothing to describe. */
static void aff_attach(JSValueConst v, ConcolicAffine *a)
{
    Concolic *c = g_concolic_class ? JS_GetOpaque(v, g_concolic_class) : NULL;

    if (!c) { aff_free(a); return; }
    DCHECK(c->aff == NULL, "an affine form was attached twice to one value — the facet is written at the mint");
    c->aff = a;
}

/* The concrete Number a cancelled form stands for. `example`, when the operands carried one, is the real
   operation the engine ran, and it must be that constant: the operands' forms say so for every world. */
static JSValue aff_constant(JSContext *ctx, double konst, JSValueConst example, const char *op)
{
    double ex = 0.0;

    if (!JS_IsUndefined(example)) {
        int ok = JS_ToFloat64(ctx, &ex, example) == 0;

        DCHECKF(ok && ex == konst,
                "the operands of `%s` carry affine forms whose terms cancel to %.17g, and the operation run on "
                "their examples gave %.17g — one operand's form is not the function its example was computed by",
                op, konst, ex);
    }
    return JS_NewInt64(ctx, (int64_t)konst);
}

/* Mark that this value's `src` names this value (`src_self`). Called only from the two mints that read a pin
   under `src`: a source read (through concolic_derived) and a member read (through concolic_alloc). A function
   because a candidate re-fire makes the mint return the attacker's bytes from concolic_deliver, not a concolic,
   and then there is no record to stamp. */
static void pin_src_names_self(JSValueConst v) {
    Concolic *c = g_concolic_class ? JS_GetOpaque(v, g_concolic_class) : NULL;
    if (!c) return;   /* a candidate re-fire delivered a plain value here — there is no record to stamp */
    /* A source with no provenance is a real state, so this is a guard: concolic_new keeps such a source without
       an identity, and its pin arm spells the same case. With no key there is no determination to read back,
       so the flag stays 0. */
    if (!c->src) return;
    c->src_self = 1;
    c->src_own = 1;
}

/* An equality's statement of whether its subject owns `src`, written once at the two equality mints. */
static void pred_set_src_own(JSValueConst pred, int own) {
    Concolic *c = g_concolic_class ? JS_GetOpaque(pred, g_concolic_class) : NULL;

    DCHECK(c != NULL && c->cmp_op != OPCMP_NONE && c->cmp_src_own == -1 && (own == 0 || own == 1),
           "a subject's ownership of `src` was stated on something that is not a fresh equality result, or "
           "stated twice, or stated as neither");
    c->cmp_src_own = (signed char)own;
}

/* An equality's statement of the types its subject's mint fixes, written once at the two equality mints. */
static void pred_set_subject_types(JSValueConst pred, unsigned types) {
    Concolic *c = g_concolic_class ? JS_GetOpaque(pred, g_concolic_class) : NULL;

    DCHECK(c != NULL && c->cmp_op != OPCMP_NONE && c->cmp_subj_types == 0 && types != 0,
           "a subject's type set was stated on something that is not a fresh equality result, or stated twice, "
           "or stated empty");
    c->cmp_subj_types = (unsigned short)types;
}

/* See concolic.h. Concretize-on-pin names the value the equality compared and nothing it was derived from:
   `'x-' + t === 'x-dark'` determines the concatenation, and writing `x-dark` under `t`'s `src` would make every
   later read of `t` answer bytes `t` never held. So only a subject that owns `src` is pinned there. A derived
   subject's equality pins nothing (its arm is the branch decision decide.c records, and an assumption the
   witness states); the demand on the root is made either way. */
void concolic_entail(JSValueConst eq)
{
    const char *f[1] = { concolic_branch_ident_c(eq) };
    char *key;
    int had, pinned;

    DCHECK(concolic_is(eq) && f[0] != NULL && !concolic_branch_neg(eq),
           "an entailed equality is not a predicate this engine can key, or is a negation");
    key = concolic_ident_compose("branch", f, 1);
    CHECK(key, "concolic: OOM composing an entailed equality's branch key");
    had = concolic_branch_decided(key);
    DCHECK(had != 0, "an equality this flow's own predicates entail was decided false on this flow's path");
    if (had == 1) { free(key); return; }                     /* already filed on this flow's path */
    concolic_constrain_branch(key, 1);
    free(key);
    pinned = concolic_pin(eq);
    DCHECK(pinned, "an entailed equality pinned nothing: its subject is a derivation, not a value with a source");
    (void)pinned;
}

int concolic_pin(JSValueConst cmp) {
    const Concolic *c = g_concolic_class ? JS_GetOpaque(cmp, g_concolic_class) : NULL;

    DCHECK(c != NULL && c->cmp_op != OPCMP_NONE && c->cmp_tok != NULL && c->src != NULL,
           "an equality's holding arm was pinned off a value that records no equality with a token and a "
           "provenance — decide.c pins only where concolic_cmp answered all three");
    DCHECK(c->cmp_src_own == 0 || c->cmp_src_own == 1,
           "an equality reached the pin without stating whether its subject is the value its `src` names — the "
           "two equality mints state it, so this result came through a third, and its pin would be filed "
           "under a source the subject may only have been derived from");
    return concolic_pin_store(c->cmp_src_own ? c->src : NULL, c->root, (ConcolicLit)c->cmp_kind, c->cmp_tok,
                              (JSConcolicEqOp)c->cmp_algo);
}

/* Whether a Number or a BigInt could spell `s` under §6.1.6.1.20 Number::toString ( x, radix ) or
   §6.1.6.2.21 BigInt::toString ( x, radix ) with radix 10: digits, a sign, a point and an exponent, or one of
   the two non-finite names. A wider test only refuses more pins. */
static int tostring_numeric_spelling(const char *s)
{
    size_t i;

    if (strcmp(s, "NaN") == 0 || strcmp(s, "Infinity") == 0 || strcmp(s, "-Infinity") == 0) return 1;
    if (s[0] == '\0') return 0;
    for (i = 0; s[i] != '\0'; i++)
        if (!strchr("0123456789.-+e", s[i])) return 0;
    return 1;
}

static unsigned tyf_of_cty(unsigned ty);

int concolic_pin_tostring(JSValueConst v, const char *s)
{
    const Concolic *c = g_concolic_class ? JS_GetOpaque(v, g_concolic_class) : NULL;
    unsigned ty;

    DCHECK(c != NULL && s != NULL, "a ToString determination was asked of a value that is not unknown input, "
                                   "or of no spelling");
    if (!c->src_self || !c->src) return 0;
    ty = tyf_of_cty(c->ty) & concolic_types(c->src);
    if (ty & (CONCOLIC_TYF_OBJECT | CONCOLIC_TYF_FUNCTION)) return 0;
    if ((ty & CONCOLIC_TYF_NULL) && strcmp(s, "null") == 0) return 0;
    if ((ty & CONCOLIC_TYF_UNDEFINED) && strcmp(s, "undefined") == 0) return 0;
    if ((ty & CONCOLIC_TYF_BOOLEAN) && (strcmp(s, "true") == 0 || strcmp(s, "false") == 0)) return 0;
    if ((ty & (CONCOLIC_TYF_NUMBER | CONCOLIC_TYF_BIGINT)) && tostring_numeric_spelling(s)) return 0;
    if (!(ty & CONCOLIC_TYF_STRING)) return 0;
    return concolic_pin_store(c->src, c->root, CONCOLIC_LIT_STRING, s, JS_CONCOLIC_EQ_STRICT);
}

/* The bytes this flow has proved this value holds (concolic.h says who may read it and why it answers bytes
   where the in-file reads answer a JSValue). */
const char *concolic_pin_bytes(JSValueConst v) {
    Concolic *c = g_concolic_class ? JS_GetOpaque(v, g_concolic_class) : NULL;
    const Cons *e;

    /* The precondition is this function's whole soundness, and it is checked, not asserted: a derived value is
       ordinary page code (`el.setAttribute('t', 'x-' + cfg.theme)` carries `cfg.theme`'s `src`), so it
       answers none and its consumer stays where it was. */
    if (!c || !c->src_self || !c->src) return NULL;
    e = cons_lookup(c->src);
    if (!e || !e->val) return NULL;
    /* Borrowed from the constraint entry, which bounds how long it may be held: only concolic_pin,
       concolic_clear_pins and concolic_pins_resume replace or release it (a write, a fresh flow, a context
       switch), none of which happens inside a caller that runs no page code and reaches no rest point. A
       caller keeping it longer copies it. */
    return e->val;
}

/* The same question asked of a source path rather than a value (concolic.h says which to reach for). The same
   lookup and test as `pin_of`, spelled once so a caller outside this file gets the mint's answer. */
int concolic_src_pin(const char *src, ConcolicLit *kind, const char **val) {
    const Cons *e = src ? cons_lookup(src) : NULL;

    if (!e || !e->val) return 0;
    *kind = (ConcolicLit)e->valkind;
    *val = e->val;
    return 1;
}

int concolic_src_pinned(const char *src) {
    const Cons *e;

    if (!src) return 0;
    e = cons_lookup(src);
    return (e && e->val) ? 1 : 0;
}
/* The same question asked by an assert, which prices nothing (g_cons_unpriced). */
static int src_pinned_unpriced(const char *src) {
    int r;

    g_cons_unpriced++;
    r = concolic_src_pinned(src);
    g_cons_unpriced--;
    return r;
}

/* One entry of an enumeration over the constraint (concolic.h's concolic_pins_each and
   concolic_assumed_decisions_each). `e` is entry `i` of the segment whose index is `hash`; the head is one such
   segment. The visible copy of a key is the one cons_lookup answers, so a shadowed entry is skipped and its
   key is visited where the nearer copy stands. Asserts the store's invariants on every entry it reads. */
typedef void (*ConsVisit)(const Cons *e, void *ud);
static void cons_each_entry(const Cons *seg, int i, const int *hash, int cap, ConsVisit visit, void *ud) {
    const Cons *e = &seg[i];
    const Cons *seen;

    DCHECK(e->key != NULL, "a constraint entry holds no key — cons_entry writes the key before anything else");
    DCHECKF(cons_index_find(seg, hash, cap, e->key) == i,
            "the constraint key `%s` is not the entry its own segment's index answers — a key held twice in one "
            "segment is two facts about one value, and a lookup answers whichever the probe reaches first",
            e->key);
    DCHECKF(!e->val == (e->valkind == (signed char)CONCOLIC_LIT_NONE),
            "the pin under `%s` and its kind disagree about whether a value is pinned — concolic_pin writes the "
            "two together and cons_entry copies them up together", e->key);
    DCHECKF(e->valkind != (signed char)CONCOLIC_LIT_BIGINT,
            "the store holds a BigInt pin under `%s`, a kind concolic_pin refuses because pin_mint cannot mint "
            "it back", e->key);
    DCHECK(!e->val || g_pin_any, "a pin is stored while the agent's pin latch says no flow ever pinned one");
    DCHECKF(!e->adec == (e->adec_what == (signed char)CONCOLIC_DECISION_NONE),
            "the assumed decision under `%s` and its kind disagree about whether one is recorded — the note "
            "writes both", e->key);
    DCHECKF(e->adec_what != (signed char)CONCOLIC_DECISION_BRANCH ||
            ((e->adec_arm == 0 || e->adec_arm == 1) && (e->adec_neg == 0 || e->adec_neg == 1) &&
             e->truth == e->adec_arm),
            "the assumed branch under `%s` states an arm the constraint's own truth for that key does not — "
            "decide_arm records the truth before the note, and nothing rewrites either", e->key);
    DCHECKF(e->adec_what != (signed char)CONCOLIC_DECISION_OUTCOME || (e->adec_arm >= 0 && e->truth == 1),
            "the assumed outcome under `%s` names a completion its key's settled truth denies — outcome_settle "
            "records truth 1 under the completion's key before the note", e->key);
    seen = cons_lookup(e->key);
    if (seen != e) {
        DCHECKF(!e->val || (seen && seen->val),
                "a nearer copy of `%s` dropped the pin the entry under it holds — cons_entry's copy-up carries "
                "the value, and nothing clears one", e->key);
        DCHECKF(!e->adec || (seen && seen->adec),
                "a nearer copy of `%s` dropped the assumed decision the entry under it holds", e->key);
        return;
    }
    visit(e, ud);
}

static void cons_each(ConsVisit visit, void *ud) {
    const ConsSeg *s;
    int i;

    for (i = 0; i < g_pins_n; i++) cons_each_entry(g_pins, i, g_pins_hash, g_pins_hash_cap, visit, ud);
    for (s = g_pins_base; s; s = s->base)
        for (i = 0; i < s->n; i++) cons_each_entry(s->e, i, s->hash, s->hash_cap, visit, ud);
}

typedef struct { ConcolicPinVisit visit; void *ud; } PinsEach;
static void pins_each_one(const Cons *e, void *ud) {
    const PinsEach *p = ud;
    if (e->val) p->visit(e->key, (ConcolicLit)e->valkind, e->val, p->ud);
}

void concolic_pins_each(ConcolicPinVisit visit, void *ud) {
    PinsEach p;

    DCHECK(visit != NULL, "the pin store was enumerated with no visitor");
    p.visit = visit; p.ud = ud;
    cons_each(pins_each_one, &p);
}

typedef struct { ConcolicDecisionVisit visit; void *ud; } DecisionsEach;
static void decisions_each_one(const Cons *e, void *ud) {
    const DecisionsEach *d = ud;
    if (!e->adec) return;
    d->visit((ConcolicDecision)e->adec_what, e->adec,
             e->adec_what == (signed char)CONCOLIC_DECISION_BRANCH ? (e->adec_arm ^ e->adec_neg) : e->adec_arm,
             d->ud);
}

void concolic_assumed_decisions_each(ConcolicDecisionVisit visit, void *ud) {
    DecisionsEach d;

    DCHECK(visit != NULL, "the assumed decisions were enumerated with no visitor");
    d.visit = visit; d.ud = ud;
    cons_each(decisions_each_one, &d);
}

void concolic_note_assumed_branch(const char *key, const char *shape, int arm, int neg) {
    Cons *c;

    DCHECK(key != NULL && shape != NULL, "an assumed branch was noted with no key or no spelling");
    DCHECK((arm == 0 || arm == 1) && (neg == 0 || neg == 1), "an assumed branch was noted with no arm");
    c = cons_entry(key);
    DCHECKF(c->truth == arm,
            "an assumed branch was noted under `%s` for an arm the constraint does not hold — decide_arm records "
            "the arm before decide_branch notes it", key);
    if (c->adec) {
        DCHECKF(c->adec_what == (signed char)CONCOLIC_DECISION_BRANCH && c->adec_arm == arm,
                "a branch noted assumed under `%s` was noted again for a different decision", key);
        return;   /* `if (p)` and `if (!p)` are one decision; the first spelling states it */
    }
    c->adec = strdup(shape);
    CHECK(c->adec, "concolic: OOM recording an assumed decision");
    c->adec_what = (signed char)CONCOLIC_DECISION_BRANCH;
    c->adec_arm = arm;
    c->adec_neg = (signed char)neg;
}

void concolic_note_assumed_outcome(const char *key, const char *op, const char *operand_shape, int completion) {
    Cons *c;
    char *spelled;

    DCHECK(key != NULL && op != NULL, "an assumed outcome was noted with no key or no operation");
    DCHECK(completion >= 0, "an assumed outcome was noted with no completion");
    /* Spelled before the entry is taken: shapef allocates through reclaim, and cons_entry's result is dead
       across anything that may grow the head. */
    spelled = shapef("%s(%s)", op, operand_shape ? operand_shape : "{}");
    c = cons_entry(key);
    DCHECKF(c->truth == 1,
            "an assumed outcome was noted under `%s`, a completion the constraint has not settled — outcome_settle "
            "runs before the note", key);
    if (c->adec) {
        DCHECKF(c->adec_what == (signed char)CONCOLIC_DECISION_OUTCOME && c->adec_arm == completion,
                "an outcome noted assumed under `%s` was noted again for a different completion", key);
        free(spelled);
        return;
    }
    c->adec = spelled;
    c->adec_what = (signed char)CONCOLIC_DECISION_OUTCOME;
    c->adec_arm = completion;
    c->adec_neg = 0;
}

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
    free(c->typeof_of);
    free(c->cmp_typeof_of);
    free(c->rel_tok);
    free(c->rel_subj);
    free(c->member);
    free(c->facet_src);
    free(c->sp_meth);
    ident_list_free(c->sp_args, c->sp_nargs);
    free(c->sp_subj);
    ident_list_free(c->conj, c->conj_n);
    free(c->key_atom);
    free(c->str_of);
    free(c->str_of_shape);
    aff_free(c->aff);
    free(c->hole_span);
    JS_FreeValueRT(rt, c->example);
    /* The second owned JSValue on this record. A field added to the struct is an obligation here and at
       concolic_gc_mark together. JS_UNINITIALIZED is not refcounted, so the never-asked case needs no test. */
    JS_FreeValueRT(rt, c->proto);
    free(c);
}

static void concolic_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func) {
    Concolic *c = JS_GetOpaque(val, g_concolic_class);
    if (c && !JS_IsUndefined(c->example)) JS_MarkValue(rt, c->example, mark_func);
    /* The derived prototype is reachable only from here: another value of this class, so the chain a page
       walks is these records pointing at each other, and an unreported one would be collected while live. */
    if (c && !JS_IsUninitialized(c->proto)) JS_MarkValue(rt, c->proto, mark_func);
}

/* @S candidate injection: during a verification re-run, every read of the source g_cand_at returns
   g_cand_bytes instead of a concolic, so the real code builds the exploit and it fires. g_cand_src and
   g_cand_payload are the search's identity and breakout as the running flow carries them; concolic_set_candidate
   derives the other two from the search's root. */
static char *g_cand_src = NULL, *g_cand_payload = NULL, *g_cand_at = NULL, *g_cand_bytes = NULL;

/* Whether this source is the one a candidate re-fire substitutes. One spelling, because it is asked at every
   mint a source can be reached through, including a source installed as a plain property value (see
   concolic_derived). */
static int cand_matches(const char *src) {
    return g_cand_at && src && !strcmp(src, g_cand_at);
}

/* Whether the mint will answer a plain value for a source (concolic.h says why it is this disjunction). Both
   arms are this file's: the pin is `concolic_new`'s early return and the substitution is `concolic_derived`'s,
   so a consumer composing the pair would copy a rule it cannot see change. */
/* The running flow's further deliveries (concolic_set_deliveries): the candidate row's other sources, each
   answering the structured value its JSON text spells. Copies, replaced at every switch-in. */
static char **g_dl_src, **g_dl_val;
static int g_dl_n;

static int deliv_find(const char *src) {
    for (int i = 0; src && i < g_dl_n; i++) if (!strcmp(g_dl_src[i], src)) return i;
    return -1;
}

/* Whether a source read answers a delivered value rather than an unknown: the search's own bytes or a further
   delivery. */
static int src_delivered(const char *src) {
    return cand_matches(src) || deliv_find(src) >= 0;
}

int concolic_src_determined(const char *src) {
    return src_delivered(src) || concolic_src_pinned(src);
}

/* The declared sources and what their component does to an attacker's bytes. A source that declares nothing
   delivers as-is, which is right for injected server state (`window.__STATE`): the attacker writes that JSON
   directly and no component transforms it.
   Every row carries its claimant: the array is this component's storage, but each row is another component's
   agent state (core/platform.h), so `concolic_undeclare_sources` is keyed by it, the duplicate assert names
   the owner from it, and concolic_free names it when a release did not finish. The pointer is the caller's
   static component name, which outlives the agent it names (as in core/agent_state.h). */
/* `principal` is a column of the same row, not a second registry: whether the attacker writes this value or
   merely owns it (concolic.h states the pair; HTML §9.3.2.2 "User agents" states why one exists). The
   claimant's release then takes the property with the row. */
/* A derived row carries `parent`, the declared row whose bytes carry its value, and one transform from its own
   value to its parent's: `key` (with `serialize`) for a tuple of an application/x-www-form-urlencoded list,
   or `path` for an element reached by named access over unknown markup. A row whose payload is the component's
   own bytes has none of them. A candidate walks the chain to its base (concolic_set_candidate). */
typedef struct { const char *component; char *src; char *encode; char prefix; SourceDeliverKind deliver;
                 int principal; char *key; char *parent; char *(*serialize)(const char *s, size_t n);
                 char *path; }
        SourceDelivery;
static SourceDelivery *g_srcs;
static int g_srcs_n, g_srcs_cap;
/* The precondition rows (concolic.h): a second array beside the delivery rows rather than a column of them,
   because every delivery reader (the encode set, the prefix, the carrier, concolic_deliver) matches a row by
   source and would otherwise read a precondition as a mechanism. Same claimant and give-back as g_srcs. */
typedef struct { const char *component; char *src; SourcePreconditionKind kind; } SourcePrecondition;
static SourcePrecondition *g_pres;
static int g_pres_n, g_pres_cap;


/* The joint domain's separator (concolic.h says what reaches it, why the identity is the set and not an
   expression over it, and why narrowing the joint narrows no member). It is part of the identity, so a member
   containing it would let two sets compose one key; the mint asserts no member does. Declared above the
   registry, its second reader. */
#define CONCOLIC_JOINT_SEP " & "

/* Member `i` of a root, as a slice of it. A root is one source's name or several joined by the separator above,
   and the mint asserts no member contains the separator, which makes this walk exact. `*p`/`*n` point into
   `root` and are never copied; nothing here outlives the call. Answers 0 past the last member, so a caller
   loops and never first asks whether the root is joint: a single source answers at i == 0 and stops. */
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

/* The declared row for one member, matched over the slice. The terminator test is half the comparison:
   `strncmp` alone would match `location.hash` against a declared `location.hashish`. */
static int src_row(const char *p, size_t n)
{
    int i;

    for (i = 0; i < g_srcs_n; i++)
        if (!strncmp(g_srcs[i].src, p, n) && g_srcs[i].src[n] == '\0') return i;
    return -1;
}

/* Which declared source a root names, over a root that may name several: the one walk both registry readers
 * below share. An exact strcmp would match no row for a joint root and answer 0 / NULL, which the report reads
 * as the positive statement "no component carries these bytes".
 *
 * A joint with one declaring member answers with that member: `location.search` joined with the viewport width
 * carries attacker bytes only through the query. That is the ordinary case, since most joints are over
 * environment facts that declare nothing.
 *
 * Two declaring members have no single true answer (each has its own percent-encode set and address
 * component), so that crashes. What to build is the per-member emission: one candidate seeded per declaring
 * member, each with its own envelope, emitting the one that fires. */
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

/* Composed below, beside the mint whose identity rule it states; declared here because the derivations that
   read it run above it. */
static char *concolic_joint_join(const char *const *parts, const int *order, int n);

/* The delivery root of a value derived from several operands: the set of roots its unknown operands entered
 * through. The one speller for concolic_arith_hook, concolic_add_hook and concolic_new_derived.
 *
 * A set, because bytes that arrived through two components entered through two: `location.hash +
 * location.search` rides half in the fragment and half in the query, which location.c declares with different
 * percent-encode sets and address components. Naming either alone would make the reproduction envelope name a
 * component the payload did not ride. One member is the ordinary case and composes to itself byte for byte
 * (`x.slice(1) + x.slice(2)`).
 * Flattened, because an operand may already carry a joint root: `(cookie + search) + hash` and
 * `(cookie + hash) + search` must name one set. Deduplicated rather than asserted, unlike
 * concolic_source_wrap_joint, because a repeated member is the common case here. Canonically ordered through
 * ident_set_order, so `a + b` and `b + a` share one root.
 * This does not answer `src` (see concolic.h): a candidate is injected at one source read.
 * NULL when no operand carries a root, which concolic_alloc reads as the other half of a NULL `src`. The
 * operands are borrowed; the returned string is owned by the caller. */
/* …and whose unknown that joined set is: the OR of the operands' masks, kept at the join so a property over a
 * whole root set is one field read. Never recovered from the joined string, which says nothing about whose each
 * member is; that fact is stated at the mint and carried. Answers 0 exactly when derived_root_join answers
 * NULL, so concolic_alloc's `!!root == !!root_whose` holds by construction. */
static unsigned derived_root_whose(const JSValueConst *operands, int n)
{
    unsigned m = 0u;
    int i;

    for (i = 0; i < n; i++) m |= root_whose_of(operands[i]);
    return m;
}

static char *derived_root_join(const JSValueConst *operands, int n)
{
    const char **view;
    char **mem;
    int *order;
    int cap = 0, cnt = 0, i, j, k;
    char *out;

    /* Sized from the operands' own member counts, not a constant, so there is no cap on how many sources a
       value may be a function of. Two walks, because the count is unknown until the first is made. */
    for (i = 0; i < n; i++) {
        const char *root = concolic_root_c(operands[i]);
        const char *p;
        size_t len;

        if (!root) continue;   /* NULL for a concrete operand and for an unknown that entered through nothing */
        for (j = 0; root_member(root, j, &p, &len); j++) cap++;
    }
    if (!cap) return NULL;
    mem   = reclaim_malloc((size_t)cap * sizeof *mem);
    view  = reclaim_malloc((size_t)cap * sizeof *view);
    order = reclaim_malloc((size_t)cap * sizeof *order);
    CHECK(mem && view && order,
          "concolic: OOM composing the delivery ROOT of a value derived from several operands — a value whose "
          "root could not be spelled reports that no component carries its bytes to the victim");
    for (i = 0; i < n; i++) {
        const char *root = concolic_root_c(operands[i]);
        const char *p;
        size_t len;

        if (!root) continue;
        for (j = 0; root_member(root, j, &p, &len); j++) {
            /* The terminator test is half the comparison, as at src_row; `&&` short-circuits, so a shorter
               member is never indexed past its own NUL. */
            for (k = 0; k < cnt; k++)
                if (!strncmp(mem[k], p, len) && mem[k][len] == '\0') break;
            if (k < cnt) continue;   /* a set holds each member once */
            mem[cnt] = reclaim_malloc(len + 1);
            CHECK(mem[cnt], "concolic: OOM naming a member of a derived value's delivery ROOT");
            memcpy(mem[cnt], p, len);
            mem[cnt][len] = '\0';
            view[cnt] = mem[cnt];
            cnt++;
        }
    }
    DCHECK(cnt >= 1 && cnt <= cap,
           "the two walks over one operand list disagreed about how many members its roots hold — they ask "
           "root_member the same question over the same strings, so a count that moved between them means the "
           "operands were mutated in between and the array about to be joined is not the one that was sized");
    ident_set_order(view, cnt, order);
    out = concolic_joint_join(view, order, cnt);
    for (k = 0; k < cnt; k++) free(mem[k]);
    free(mem);
    free((void *)view);
    free(order);
    return out;
}

/* The token a report carries for a mechanism, spelled once so the engine owns the source vocabulary: the
   declaration side is the enum, the emission side is this, and a delivery layer switches on what comes out. */
static const char *deliver_token(SourceDeliverKind k)
{
    switch (k) {
    case SRC_DELIVER_ADDRESS:           return "address";
    case SRC_DELIVER_REFERRING_ADDRESS: return "referring-address";
    case SRC_DELIVER_USER_FILE:         return "user-file";
    case SRC_DELIVER_CROSS_DOCUMENT_MESSAGE: return "cross-document-message";
    case SRC_DELIVER_USER_INPUT:        return "user-input";
    case SRC_DELIVER_NAVIGABLE_NAME:    return "navigable-name";
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
    /* The two halves of one declaration must agree: a value carried in the victim's own address has the
       component it is carried at, and any other mechanism has none. A `#` beside a `plant` would prepend a
       character no browser puts there. */
    DCHECK((deliver == SRC_DELIVER_ADDRESS) == (prefix != 0),
           "a source's delivery mechanism and its address component disagree — only a value carried in the "
           "victim's own address has a component, and it always has one");
    for (i = 0; i < g_srcs_n; i++)
        if (!strcmp(g_srcs[i].src, src)) {
            /* Who already owns it says what went wrong: the same component twice is a declaration that ran
               twice or a dynamic claimant that did not ask first; a different component is two claimants
               over one source, either of whose release takes a row it does not own. */
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
    /* A source the attacker writes, which nearly every source is. The exception says so with its own call
       (concolic_declare_source_principal); written here at the mint so the column is never uninitialized. */
    g_srcs[g_srcs_n].principal = 0;
    g_srcs[g_srcs_n].key = NULL;
    g_srcs[g_srcs_n].parent = NULL;
    g_srcs[g_srcs_n].serialize = NULL;
    g_srcs[g_srcs_n].path = NULL;
    CHECK(g_srcs[g_srcs_n].src && g_srcs[g_srcs_n].encode, "concolic: OOM declaring a source's delivery");
    g_srcs_n++;
}

/* The claim, given back by the component that made it (core/platform.h): a declared row's storage is this
 * array and its claim is the declaring component's, so the claimant removes its own rows at its release and
 * concolic_free asserts at this component's release that none are left.
 *
 * Keyed by the claimant, not the source: core/file/file_system.c declares `file:NAME` per file the device has
 * held, and a release naming each source would force it to keep a parallel list of what it declared.
 *
 * Not a bulk reset: removing exactly the caller's rows is what makes `g_srcs_n == 0` a fact about the claimants
 * and lets the assert name the one that did not finish. Zero rows is a legitimate answer for a component whose
 * rows are dynamic. */
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
        free(g_srcs[i].key);
        free(g_srcs[i].parent);
        free(g_srcs[i].path);
        /* The order of the rest is preserved rather than closed with a swap: lookups are by identity, but a
           walk's order must not depend on which unrelated component released first. */
        memmove(&g_srcs[i], &g_srcs[i + 1], (size_t)(g_srcs_n - i - 1) * sizeof *g_srcs);
        g_srcs_n--;
    }
    /* …and its precondition rows, the same claim in the second array. */
    i = 0;
    while (i < g_pres_n) {
        if (strcmp(g_pres[i].component, component) != 0) { i++; continue; }
        free(g_pres[i].src);
        memmove(&g_pres[i], &g_pres[i + 1], (size_t)(g_pres_n - i - 1) * sizeof *g_pres);
        g_pres_n--;
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
            /* The duplicate assert seen from the other end: a claimant asking about a source another component
               declared is about to skip a declaration it owes or give back a row it does not own. */
            DCHECK(strcmp(g_srcs[i].component, component) == 0,
                   "a component asked whether it owns an attacker source that a DIFFERENT component declared "
                   "— one source is owned by one component in both directions, so this is either a source "
                   "identity two components spell the same way or a claimant reaching into another's rows");
            return 1;
        }
    return 0;
}

/* The second half of a row (concolic.h says what a principal is and decides). A separate call rather than a
 * sixth argument, so only the exception spells it (core/platform.h's rule for optional claim properties). It
 * refuses a row this component does not own, and a row that does not exist yet. */
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

/* The pins this flow made on each attacker principal (see concolic.h). A walk of the registry rather than a
 * keyed lookup, because the flow must not be taught which sources are principals; this array is the one place
 * that knows. O(declared principals), one indexed chain probe each. */
int concolic_principal_pins(void (*cb)(void *user, const char *src, const ConcolicPrincipalPin *pin, int n),
                            void *user)
{
    int i, found = 0;

    for (i = 0; i < g_srcs_n; i++) {
        const Cons *c;
        if (!g_srcs[i].principal) continue;
        c = cons_lookup(g_srcs[i].src);
        if (!c || !c->nppin) continue;
        DCHECK(c->ppin != NULL, "a principal's pin count stands over no pins");
        found++;
        if (cb) cb(user, g_srcs[i].src, c->ppin, c->nppin);
    }
    return found;
}

/* The principal answering a root member, matched over the slice like src_row. */
static int src_principal(const char *p, size_t n)
{
    int row = src_row(p, n);
    return row >= 0 && g_srcs[row].principal;
}

int concolic_root_delivered_any(const char *root)
{
    const char *p;
    size_t n;

    for (int i = 0; root_member(root, i, &p, &n); i++)
        if (src_row(p, n) >= 0) return 1;
    return 0;
}

int concolic_root_delivered_all(const char *root)
{
    const char *p;
    size_t n;
    int i;

    for (i = 0; root_member(root, i, &p, &n); i++)
        if (src_row(p, n) < 0) return 0;
    return i > 0;
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

const char *concolic_source_delivery_key(const char *root)
{
    int row = root_declared_row(root);

    while (row >= 0 && !g_srcs[row].key && g_srcs[row].parent)
        row = src_row(g_srcs[row].parent, strlen(g_srcs[row].parent));
    return row < 0 ? NULL : g_srcs[row].key;
}

static const char *precondition_token(SourcePreconditionKind k)
{
    switch (k) {
    case SRC_PRE_SAME_ORIGIN_STORE: return "same-origin";
    case SRC_PRE_APPLICATION_STATE: return "application-state";
    }
    DFAIL("a source declared a precondition with no report token");
    return NULL;
}

void concolic_declare_source_precondition(const char *component, const char *src, SourcePreconditionKind kind)
{
    int i;

    CHECK(src != NULL, "a precondition source was declared with no identity");
    DCHECK(component != NULL && *component, "a precondition source was declared by no component — the row is a "
                                            "claim its claimant gives back at its own release");
    for (i = 0; i < g_srcs_n; i++)
        DCHECKF(strcmp(g_srcs[i].src, src) != 0, "`%s` was declared a precondition source and %s already "
                "declared it an attacker source — a source either has a browser delivery or names what must "
                "already hold, never both", src, g_srcs[i].component);
    for (i = 0; i < g_pres_n; i++)
        DCHECKF(strcmp(g_pres[i].src, src) != 0, "the precondition source `%s` was declared twice (by %s first) "
                "— a claimant with dynamic rows asks concolic_source_precondition_declared_by first", src,
                g_pres[i].component);
    if (g_pres_n == g_pres_cap) {
        int c = g_pres_cap ? g_pres_cap * 2 : 8;
        SourcePrecondition *a = reclaim_realloc(g_pres, (size_t)c * sizeof *a);
        CHECK(a != NULL, "concolic: OOM declaring a precondition source");
        g_pres = a; g_pres_cap = c;
    }
    g_pres[g_pres_n].component = component;
    g_pres[g_pres_n].src = strdup(src);
    g_pres[g_pres_n].kind = kind;
    CHECK(g_pres[g_pres_n].src != NULL, "concolic: OOM declaring a precondition source");
    g_pres_n++;
}

int concolic_source_precondition_declared_by(const char *component, const char *src)
{
    int i;

    DCHECK(component != NULL && *component && src != NULL,
           "the precondition registry was asked whether a component owns a row, with no component or no source");
    for (i = 0; i < g_pres_n; i++)
        if (!strcmp(g_pres[i].src, src)) {
            DCHECK(strcmp(g_pres[i].component, component) == 0,
                   "a component asked whether it owns a precondition source a DIFFERENT component declared");
            return 1;
        }
    return 0;
}

const char *concolic_source_precondition(const char *root)
{
    const char *p;
    size_t n;
    int i, j;

    for (i = 0; root_member(root, i, &p, &n); i++)
        for (j = 0; j < g_pres_n; j++)
            if (!strncmp(g_pres[j].src, p, n) && g_pres[j].src[n] == '\0')
                return precondition_token(g_pres[j].kind);
    return NULL;
}

/* The attacker's site and page (concolic.h), per process: the trusted zone states it before any document runs. */
static char g_attacker_site[254] = "attacker.test";
static char g_attacker_page[sizeof g_attacker_site + 10] = "https://attacker.test/";

void concolic_set_attacker_site(const char *host)
{
    size_t n = host ? strlen(host) : 0;

    CHECK(n > 0 && n < sizeof g_attacker_site && strspn(host, "abcdefghijklmnopqrstuvwxyz0123456789-.") == n &&
          host[0] != '.' && host[0] != '-' && host[n - 1] != '.' && strchr(host, '.'),
          "concolic: the attacker site is not a lowercase registrable host name");
    memcpy(g_attacker_site, host, n + 1);
    snprintf(g_attacker_page, sizeof g_attacker_page, "https://%s/", host);
}

const char *concolic_attacker_site(void) { return g_attacker_site; }
const char *concolic_attacker_page(void) { return g_attacker_page; }

const char *concolic_source_attacker_page(const char *root)
{
    int row = root_declared_row(root);

    return row >= 0 && g_srcs[row].deliver == SRC_DELIVER_REFERRING_ADDRESS ? concolic_attacker_page() : NULL;
}

const char *concolic_source_encodes(const char *root)
{
    int row = root_declared_row(root);

    return row < 0 ? NULL : g_srcs[row].encode;
}

/* What the mechanism itself does to a byte nobody declared: the other half of a delivery.
 *
 * A declared `encode` set states only what distinguishes one component from its siblings (location.c declares
 * the backtick for the fragment, the apostrophe and `#` for the query); the part every URL component shares is
 * the standard's, and `bytes_probe` addresses each declared byte by one index character, so it could not hold
 * that set anyway. The shared part is asked of the `deliver` column, so a mechanism added
 * later must answer the question (the switch has no default, so the compiler warns).
 *
 * Two answers. A payload serialized into a URL inherits URL §1.3 "Percent-encoded bytes"'s C0 control
 * percent-encode set ("C0 controls and all code points greater than U+007E (~)"), of which the fragment, query
 * and special-query sets are supersets. A payload carried verbatim inherits nothing: HTML §9.3.3 "Posting
 * messages" step 7 serializes and step 8.4 deserializes without transforming a string, file_system.c reads a
 * file verbatim, and §7.2.2.1's name is the string the attacker gave it. */
typedef enum { CARRIER_URL = 0, CARRIER_VERBATIM } CarrierKind;

static CarrierKind root_carrier(const char *root)
{
    int row = root_declared_row(root);

    /* A root with no row is a state the one caller has already excluded (it returns the payload untouched when
       neither the root nor the injection point declares, and asserts they name the same source otherwise), so
       reaching here means the two facts were threaded from different operands. */
    if (row < 0) {
        DFAIL("a candidate's delivery ROOT declares no source while its injection point declares one — the "
              "encode set and the carrier are the two halves of ONE row's answer, so a root with no row means "
              "the payload is about to be delivered under a mechanism nothing in this registry states for it");
        return CARRIER_VERBATIM;   /* release only — dev stops at the assert above */
    }
    switch (g_srcs[row].deliver) {
    case SRC_DELIVER_ADDRESS:                return CARRIER_URL;        /* the victim's own address */
    case SRC_DELIVER_REFERRING_ADDRESS:      return CARRIER_URL;        /* the address the victim arrives from */
    case SRC_DELIVER_USER_FILE:              return CARRIER_VERBATIM;   /* file_system.c: read verbatim */
    case SRC_DELIVER_CROSS_DOCUMENT_MESSAGE: return CARRIER_VERBATIM;   /* HTML §9.3.3 steps 7/8.4 */
    case SRC_DELIVER_USER_INPUT:             return CARRIER_VERBATIM;   /* input_value.c sanitizes at the edit */
    case SRC_DELIVER_NAVIGABLE_NAME:         return CARRIER_VERBATIM;   /* HTML §7.2.2.1: the name as written */
    }
    DFAIL("a source declared a delivery mechanism that does not say what it does to an UNDECLARED byte — the "
          "declared set is only the part that distinguishes one component from its siblings, so every "
          "mechanism must also state whether it serializes its payload into a URL (and so inherits URL §1.3 "
          "\"Percent-encoded bytes\"'s C0 control percent-encode set) or carries it verbatim. Without that a candidate is delivered under whichever answer this "
          "switch was written for first");
    return CARRIER_VERBATIM;
}

/* The bytes no row declares and every mechanism must answer for, read by the delivery loop below.
   `< 0x20` is Infra's C0 control ("a code point in the range U+0000 NULL to U+001F INFORMATION SEPARATOR ONE,
   inclusive"); `> 0x7E` is URL §1.3 "Percent-encoded bytes"'s "all code points greater than U+007E (~)", so DEL
   needs no clause of its own. Byte-wise `%XX` over a UTF-8 payload is §1.3's UTF-8 percent-encode, which
   percent-encodes "each byte of encodeOutput". */
static int carrier_shared_byte(unsigned char b) { return b < 0x20 || b > 0x7E; }

/* JSConcolicHooks.lead: the domain fact the delivery declaration holds, for a builtin deciding whether one of
   its completions is feasible. The prefix is what the component says its value carries (`#` for a fragment,
   `?` for a query), the same declaration the candidate delivery is built from. A derived value
   (`location.hash.slice(1)`) has its own source identity and no declaration, so it answers 0: slicing is what
   removes the prefix. */
static int concolic_lead_hook(JSValueConst v)
{
    const char *src = concolic_src_c(v);
    int i;
    if (!src) return 0;
    for (i = 0; i < g_srcs_n; i++)
        if (!strcmp(g_srcs[i].src, src)) return (unsigned char)g_srcs[i].prefix;
    return 0;
}

/* The candidate as the page reads it. The solver's payload is what the attacker puts in the URL; this is what
 * the browser hands the page, the only thing a breakout can be decided against.
 *
 * The two halves of the declaration are asked with different keys, because only one survives a derivation.
 *   The encode set is a fact about the root: the browser percent-encoded the bytes on the way into the address
 *   and `location.hash.slice(1)` does not decode them, so a candidate substituted at the slice's result is
 *   still the encoded form. Asked of `src`, it would go missing for every derivation and a `<` would fire in
 *   the model that Chrome encodes in a fragment.
 *   The prefix is a fact about the injection point: slicing removes the `#` (see concolic_lead_hook), so it
 *   comes off `src`'s own row, which a derived identity does not have.
 * When `src` has a row it is the root's row: a value carrying a declared source's name as its identity
 * inherited the root unchanged too. Asserted, because the two are threaded from an operand each. */
/* The candidate's own bytes entering the program, observed for the running flow `f` (concolic_deliver's
   precondition checks are the caller's): the live fact, the arrival rung, the search's substitution count and
   the marks that its addresses may rest on bytes this engine chose. */
static void cand_observe_delivery(Flow *f)
{
    g_cand_delivered = 1;
    /* The bottom ladder rung, written here because this is its site. Every other rung reports at a sink,
       and each rung needs an observation site strictly before what it measures; without this a candidate
       that replayed many gates without reaching its source read would rank with one never served. It
       states only that the bytes are in the program, which needs no taint tracker; recorded at solve.c's
       sink entries it would add nothing the sink rungs do not carry.
       It is not `g_cand_delivered`: that flag is the live answer for this replay, cleared by
       concolic_clear_pins when a restarted candidate replays from the baseline, so solve.c's sinks see 0.
       The rung is the comparator and monotone; flow_observe_rung's early return makes re-delivery free. */
    flow_observe_rung(f, FLOW_RUNG_DELIVERED);
    DCHECK(f->fire.kind != FIRE_NONE,
           "an @S substitution is being delivered for a flow whose fire target says it is no candidate");
    /* The same event counted on the search, the one unit a reader sees; it orders nothing and outlives the
       flows. It lets a parked report tell "bytes never entered" from "entered and reached no sink" and from
       "sinks ran carrying none", which otherwise all read `turns:N,reached:0,survived:0`. Not a credit:
       solve.c raises a counter and moves no ordering. */
    if (f->fire.kind == FIRE_SINK) solve_observe_substitution(f);   /* a verification is no search's (solve_flow_begin) */
    /* Every candidate delivers bytes this engine chose (a breakout, or the value a witness's pin chose), so
       from here an address the run composes may rest on them, and its requests say so (flow.h's
       `path_forced` and `path_pinned`). Whose act the run is, which holds from its first opcode, is the
       separate fact flow_path_driven answers. */
    flow_mark_forced_arm();
    flow_mark_pinned_value();
}

static JSValue concolic_deliver(JSContext *ctx, const char *src, const char *root, const char *payload)
{
    const SourceDelivery *at = NULL;   /* the injection point's row, if it has one: the component's prefix */
    const char *encode;                /* …and the root's percent-encode set, which every derivation inherits */
    CarrierKind carrier;               /* …and what the root's mechanism does to a byte that set does not name */
    const unsigned char *p;
    char *out;
    size_t o = 0, n;
    int i;

    /* The one point at which the attacker's bytes become a value in the page's program, so it is the
     * observation site for concolic_candidate_delivered. Both callers reach here through cand_matches, so there
     * is no second door.
     * The payload is checked, not defaulted: an empty-string delivery would have every rung measure a
     * zero-length payload. CHECK, not DCHECK, because it is dereferenced below.
     * The bytes belong to the flow credited for them: concolic_set_candidate asserts the match at switch-in,
     * and this asks again as the bytes enter, because the rungs and any PoC are recorded against
     * `flow_running()`. */
    {
        Flow *f = flow_running();
        CHECK(payload != NULL,
              "concolic: an @S candidate is being delivered with no payload — the substitution is (src,payload) "
              "as one identity and solve.c refuses to seed a flow holding half of it, so a NULL here is an "
              "installation that skipped concolic_set_candidate's own pairing; delivered as the empty string "
              "it would read as a source the page composed nothing out of");
        /* CHECKed, not only DCHECKed: the ladder write below dereferences it in every build. */
        CHECK(f != NULL,
              "concolic: an @S candidate's bytes are entering the program with no flow running — a delivery is "
              "an observation ABOUT a flow (its ladder rung, its survival fraction, its PoC), so a delivery "
              "belonging to nobody is a substitution installed outside the scheduler's switch");
        DCHECK(f->cand_payload != NULL && g_cand_payload != NULL && !strcmp(f->cand_payload, g_cand_payload) &&
               g_cand_bytes != NULL && !strcmp(payload, g_cand_bytes),
               "an @S candidate's bytes are entering the program under a flow that is not carrying them — the "
               "survival and arrival rungs, and any PoC, are recorded against the RUNNING flow, so this "
               "delivery is about to be credited to a flow whose own payload is a different string");
        cand_observe_delivery(f);
    }
    for (i = 0; i < g_srcs_n; i++)
        if (src && !strcmp(g_srcs[i].src, src)) { at = &g_srcs[i]; break; }
    encode = concolic_source_encodes(root);
    /* The root's declaring member, not the whole root, because a root may name a set: `location.hash +
       {state}.region` has two members of which only the fragment declares anything, and a whole-string
       comparison would abort on that ordinary composition. The question is whether the encode set and the
       prefix come from one row. A root with two declaring members never reaches here (root_declared_row
       refuses first). The walk is side-effect-free, so calling it twice in the condition is fine. */
    DCHECK(!at || (root && root_declared_row(root) >= 0 && &g_srcs[root_declared_row(root)] == at),
           "a candidate is being delivered at an injection point that names a DECLARED source while its "
           "delivery root's DECLARING MEMBER names a different one — the two are threaded from an operand "
           "each, so this value took them from two, and the payload would be percent-encoded for one source "
           "and prefixed for the other");
    if (!at && !encode) return JS_NewString(ctx, payload);   /* nothing carries these bytes: delivered as itself */
    DCHECK(!at || !at->parent, "a candidate was delivered at a derived row's own read — concolic_set_candidate "
                               "delivers it at the chain's base component, whose real parse yields the row");
    if (!encode) encode = "";
    /* The same key as the encode set: the declared and the shared halves of one row's answer, so reading them
       from different rows would apply one mechanism's rule to another's bytes. */
    carrier = root_carrier(root);

    n = strlen(payload);
    out = reclaim_malloc(n * 3 + 2 + strlen(concolic_attacker_page()) + 1);
    CHECK(out != NULL, "concolic: OOM delivering a candidate");
    if (at && at->prefix) out[o++] = at->prefix;
    /* A referrer is the attacker page's whole URL, its address before the payload included. */
    if (at && at->deliver == SRC_DELIVER_REFERRING_ADDRESS) {
        o = strlen(concolic_attacker_page());
        memcpy(out, concolic_attacker_page(), o);
    }
    for (p = (const unsigned char *)payload; *p; p++) {
        /* A byte outside printable US-ASCII is the mechanism's to answer for, not the row's (root_carrier). */
        int shared = carrier_shared_byte(*p);

        if ((shared && carrier == CARRIER_URL) || strchr(encode, (char)*p)) {
            static const char HEX[] = "0123456789ABCDEF";
            out[o++] = '%'; out[o++] = HEX[*p >> 4]; out[o++] = HEX[*p & 15];
        } else {
            out[o++] = (char)*p;
        }
    }
    out[o] = 0;
    /* Chrome hands a referrer longer than its limit as the attacker page's origin, so such a candidate arrives
       without its payload. */
    if (at && at->deliver == SRC_DELIVER_REFERRING_ADDRESS && o > REFERRER_CHROME_LIMIT)
        o = strlen(concolic_attacker_page());
    {
        JSValue r = JS_NewStringLen(ctx, out, o);
        free(out);
        return r;
    }
}
/* The installed substitution belongs to the running flow: asserted here, where it is installed by its only
   caller, the scheduler. A mismatch would have an exploring flow read an attacker payload where its concolic
   source belongs, silently stop forking and stop being a detectable sink. Two-sided: a candidate flow must
   have its payload installed, and any other flow must have none. */
/* The markup composer the claimant of named-access rows registers (concolic_set_markup_composer). */
static const char *g_markup_component;
static char *(*g_markup_compose)(const char *path, const char *href);

/* One row's transform from its value to its parent's: the tuple `key=<serialized bytes>`, or the markup whose
   parse makes the named access along `path` reach `bytes`. Owned; NULL where the composer refuses the path. */
static char *row_transform(int row, const char *bytes)
{
    char *out = NULL;

    if (g_srcs[row].key) {
        char *v = g_srcs[row].serialize(bytes, strlen(bytes));
        size_t n;

        CHECK(v != NULL, "concolic: OOM serializing a candidate as a tuple's value");
        n = strlen(g_srcs[row].key) + strlen(v) + 2;
        out = malloc(n);
        CHECK(out != NULL, "concolic: OOM composing a candidate's tuple");
        snprintf(out, n, "%s=%s", g_srcs[row].key, v);
        free(v);
    } else {
        DCHECK(g_srcs[row].path && g_markup_compose, "a derived row has neither a key nor a composable path");
        out = g_markup_compose(g_srcs[row].path, bytes);
    }
    return out;
}

/* Walk `root`'s chain from `bytes` (consumed) toward its base, applying each row's transform, and stop at the
   base, or before the first keyed row when `stop_at_key`. Answers the row reached and the bytes there (owned,
   NULL when a composer refused a path); -1 and the bytes unchanged for a root no component declares. */
static int row_chain_walk(const char *root, char **bytes, int stop_at_key)
{
    int row = root ? root_declared_row(root) : -1;

    while (row >= 0 && g_srcs[row].parent && *bytes && !(stop_at_key && g_srcs[row].key)) {
        char *next = row_transform(row, *bytes);

        free(*bytes);
        *bytes = next;
        row = src_row(g_srcs[row].parent, strlen(g_srcs[row].parent));
        DCHECK(row >= 0, "a derived row's parent is no declared row");
    }
    return row;
}

char *concolic_candidate_poc(const char *root, const char *payload)
{
    char *bytes = strdup(payload);

    CHECK(bytes != NULL, "concolic: OOM composing a PoC");
    row_chain_walk(root, &bytes, 1);
    DCHECK(bytes != NULL, "a fired PoC's markup could not be composed from the path its candidate was delivered by");
    return bytes;
}

void concolic_set_candidate(const char *src, const char *root, const char *payload) {
    int row;

    free(g_cand_src); free(g_cand_payload); free(g_cand_at); free(g_cand_bytes);
    g_cand_src = g_cand_payload = g_cand_at = g_cand_bytes = NULL;
    DCHECK(!!src == !!payload, "an @S substitution was installed with an identity and no payload, or the reverse");
    if (src) {
        g_cand_src = strdup(src);
        g_cand_payload = strdup(payload);
        CHECK(g_cand_src && g_cand_payload, "concolic: OOM installing an @S candidate");
        /* One delivery per source per world: the payload enters at the root's declared base source read,
           through each derived row's transform on the way (a tuple, URL §5.2; markup for a named access), so
           every read of that source answers it and every derivation re-runs on it. A root no component
           declares (a precondition, or a witness verification, which passes no root) substitutes at the
           identity. A path the composer refuses delivers nothing, and the candidate cannot fire. */
        g_cand_bytes = strdup(payload);
        CHECK(g_cand_bytes != NULL, "concolic: OOM installing an @S candidate's delivery");
        row = row_chain_walk(root, &g_cand_bytes, 0);
        if (g_cand_bytes) {
            g_cand_at = strdup(row >= 0 ? g_srcs[row].src : src);
            CHECK(g_cand_at != NULL, "concolic: OOM installing an @S candidate's delivery");
        }
    }
    {
        const Flow *f = flow_running();
        DCHECK(!!src == !!(f && f->cand_src),
               "the installed @S substitution does not match the running flow — an exploring flow would read "
               "the previous candidate's payload in place of its concolic source");
    }
}

void concolic_set_deliveries(char *const *srcs, char *const *vals, int n) {
    const Flow *f = flow_running();

    for (int i = 0; i < g_dl_n; i++) { free(g_dl_src[i]); free(g_dl_val[i]); }
    free(g_dl_src); free(g_dl_val);
    g_dl_src = g_dl_val = NULL;
    g_dl_n = 0;
    DCHECK(n == (f ? f->cand_dn : 0) && (n == 0 || g_cand_src),
           "the installed deliveries are not the running flow's, or ride no candidate");
    if (n == 0) return;
    g_dl_src = malloc((size_t)n * sizeof *g_dl_src);
    g_dl_val = malloc((size_t)n * sizeof *g_dl_val);
    CHECK(g_dl_src && g_dl_val, "concolic: OOM installing a candidate's deliveries");
    for (int i = 0; i < n; i++) {
        int row = src_row(srcs[i], strlen(srcs[i]));

        DCHECK(deliv_find(srcs[i]) < 0 && row >= 0 && g_srcs[row].deliver == SRC_DELIVER_CROSS_DOCUMENT_MESSAGE,
               "a further delivery names a source twice, or a source that is not a cross-document message's: "
               "only a message's data and origin are delivered this way");
        (void)row;
        g_dl_src[i] = strdup(srcs[i]);
        g_dl_val[i] = strdup(vals[i]);
        CHECK(g_dl_src[i] && g_dl_val[i], "concolic: OOM installing a candidate's delivery");
        g_dl_n = i + 1;
    }
}

/* The structured value a further delivery posts: HTML §9.3.3 step 8.4's StructuredDeserializeWithTransfer
   of what the attacker document serialized, which for the JSON-expressible value a composition spells is
   ParseJSON over its text in this realm. Where the search injects inside this message, the payload rides in
   the value and the delivery is the candidate's own (cand_observe_delivery); otherwise it is bytes this engine
   chose. */
static JSValue deliver_structured(JSContext *ctx, int i) {
    Flow *f = flow_running();
    JSValue v;

    CHECK(f != NULL, "concolic: a candidate's delivery is entering the program with no flow running");
    if (g_cand_at && !strcmp(g_cand_at, g_dl_src[i])) {
        cand_observe_delivery(f);
    } else {
        flow_mark_forced_arm();
        flow_mark_pinned_value();
    }
    v = JS_ParseJSON(ctx, g_dl_val[i], strlen(g_dl_val[i]), "<delivery>");
    CHECK(!JS_IsException(v), "concolic: a delivery's composed JSON did not parse — message_compose writes it");
    return v;
}

/* Whether the running flow's own candidate bytes have entered the program (the static above says where the
   fact lives and how it travels). It is the precondition of every observation about a candidate's bytes:
   solve.c's sink entries test `concolic_is(arg)`, which is true of every literal the page writes, so without
   this the survival rung would measure the longest run of candidate bytes in strings the page built itself,
   and a breakout is punctuation (`'><svg onload=X9()>`) found in almost any markup.
   0 is not "not yet": it states that nothing in this program is this flow's, so a string that looks like the
   candidate's is the page's own. Read at the sink, never latched by a reader.
   It is not FLOW_RUNG_DELIVERED (flow.h), the WFQ's monotone comparator coordinate: this is the live answer
   and is cleared for a flow entered fresh, because a restarted candidate's bytes are not in its replay. See
   the writer in concolic_deliver for why the opposite reset rules are both needed. */
int concolic_candidate_at_root(const char *root) {
    int row = root ? root_declared_row(root) : -1;

    while (row >= 0 && g_srcs[row].parent) row = src_row(g_srcs[row].parent, strlen(g_srcs[row].parent));
    if (row < 0 || !g_cand_at) return 0;
    return !strcmp(g_cand_at, g_srcs[row].src) && g_cand_delivered;
}

/* The constraint key under which a flow records that one of its branches was decided over a declared
   principal (concolic_note_principal_branch). Not a branch identity: concolic_ident_compose's tag keeps it apart
   from every `branch` key. */
#define PRINCIPAL_DECIDED_TAG "principal-decided"

/* The String operands of a method the page called over a declared principal (an origin), filed on each
   principal member of the receiver's root in the running flow's constraint (Cons.ptok), once each. */
static void principal_tokens_note(JSContext *ctx, JSValueConst recv, int argc, JSValueConst *argv) {
    const char *root = concolic_root_c(recv), *p;
    size_t n;

    for (int i = 0; root_member(root, i, &p, &n); i++) {
        char *member;

        if (!src_principal(p, n)) continue;
        member = strndup(p, n);
        CHECK(member, "concolic: OOM naming a principal's operand set");
        for (int a = 0; a < argc; a++) {
            const char *tok;
            Cons *c;
            int dup = 0;

            if (!JS_IsString(argv[a])) continue;
            tok = JS_ToCString(ctx, argv[a]);
            CHECK(tok, "concolic: OOM reading a principal's operand");
            c = cons_entry(member);
            for (int k = 0; k < c->nptok && !dup; k++) dup = !strcmp(c->ptok[k], tok);
            if (!dup && *tok) {
                char **grown = reclaim_realloc(c->ptok, (size_t)(c->nptok + 1) * sizeof *grown);

                CHECK(grown, "concolic: OOM recording a principal's operand");
                c->ptok = grown;
                c->ptok[c->nptok] = strdup(tok);
                CHECK(c->ptok[c->nptok], "concolic: OOM recording a principal's operand");
                c->nptok++;
            }
            JS_FreeCString(ctx, tok);
        }
        free(member);
    }
}

void concolic_principal_tokens_each(const char *src, void (*cb)(void *ud, const char *tok), void *ud) {
    const Cons *c;

    DCHECK(src && cb, "a principal's operands were enumerated with no principal or no visitor");
    c = cons_lookup(src);
    if (!c) return;
    for (int i = 0; i < c->nptok; i++) cb(ud, c->ptok[i]);
}

void concolic_note_principal_branch(JSValueConst cond) {
    const char *root = concolic_root_c(cond), *p;
    size_t n;

    for (int i = 0; root_member(root, i, &p, &n); i++) {
        if (src_principal(p, n)) {
            const char *f[1] = { "" };
            char *key = concolic_ident_compose(PRINCIPAL_DECIDED_TAG, f, 1);

            CHECK(key, "concolic: OOM naming a principal decision");
            cons_entry(key)->truth = 1;
            free(key);
            return;
        }
    }
}

int concolic_principal_decided(void) {
    const char *f[1] = { "" };
    char *key = concolic_ident_compose(PRINCIPAL_DECIDED_TAG, f, 1);
    const Cons *c;

    CHECK(key, "concolic: OOM naming a principal decision");
    c = cons_lookup(key);
    free(key);
    return c && c->truth == 1;
}

int concolic_candidate_delivered(void) {
    DCHECK(!g_cand_delivered || g_cand_payload != NULL,
           "a flow is recorded as having delivered its @S payload while no substitution is installed — the "
           "delivery is set at concolic_deliver, which cand_matches only reaches with one installed, so the "
           "two have come apart and every sink observation below this is about to be attributed on a fact "
           "belonging to a candidate that is no longer running");
    return g_cand_delivered;
}

/* The member's own example, computed by running the real read on the parent's example.
 *
 * A concolic whose example is a record is what the real codec hands back:
 * §25.5.2 JSON.parse ( text [ , reviver ] ) over an unknown text runs the engine's own JSON.parse on the
 * source's example, so a loaded config's `region` reaches an @H parameter with the value the run had. A member the record does not hold
 * yields no example, which keeps the gate over it forking instead of inventing a value.
 *
 * Not [[Get]] but JS_GetOwnSlotDesc, which runs none of the page's code: a prototype walk would answer
 * `hasOwnProperty` for members no server wrote, a getter is page code with no flow base under this C
 * activation, and whether the record holds the member is [[GetOwnProperty]]'s question. An accessor therefore
 * answers no example; JSON.parse builds only data properties, so the only route to one is a reviver, which
 * §25.5.2 step 9 hands the walk to. */
/* §10.4.3.5 StringGetOwnProperty ( string, propertyKey ): the own members of a value whose example is a
 * String (§10.4.3 String Exotic Objects is the one primitive wrapper with own properties). Answering them runs
 * the real op on a value the flow had (§6.1.4 The String Type's indexing and `length`) and only fills the
 * derived member's example; the member stays unknown.
 * The attributes are §10.4.3's: step 10 gives an index { [[Writable]]: false, [[Enumerable]]: true,
 * [[Configurable]]: false } and §10.4.3.4 StringCreate ( value, proto ) step 8 makes `length` all false, so
 * the internal methods that write ask JS_IsString first.
 * Step 2's CanonicalNumericIndexString makes `01`, `1.0`, `+1`, ` 1` and `-0` names, not indices; a
 * digits-only test with no leading zero is that intersected with steps 3, 4 and 8.
 * The value comes from the engine's own read, which answers `length` and an in-range index before the
 * page-patchable prototype is consulted; an index at or past the end, the read that would reach the
 * prototype, is answered here as step 8 does.
 * 1 = the string holds this own member; `*out_value` is owned and `*out_flags` is its C/W/E. 0 = none. */
/* How many code units a String example holds: §6.1.4 The String Type's count, the `length` §10.4.3.4 step 8
   defines and the bound §10.4.3.5 step 8 tests an index against. Read through the engine's O(1) answer for a
   String primitive, for the reason above. */
static int64_t string_example_units(JSContext *ctx, JSValueConst example)
{
    JSValue lv;
    int64_t len = 0;

    DCHECK(JS_IsString(example), "the code units of a non-String example were counted — every caller asks "
                                 "JS_IsString first, so this is a walk that reached a record whose example is "
                                 "something else entirely");
    lv = JS_GetPropertyStr(ctx, example, "length");
    DCHECK(JS_IsNumber(lv),
           "a String primitive answered its own `length` with something that is not a Number — the read is "
           "§10.4.3.4 StringCreate ( value, proto ) step 8's own property and the engine answers it off the "
           "string's own count before any prototype is consulted, so a non-Number here is a read that went "
           "somewhere else");
    JS_ToInt64(ctx, &len, lv);
    JS_FreeValue(ctx, lv);
    return len;
}

static int string_example_slot(JSContext *ctx, JSValueConst example, JSAtom atom,
                               JSValue *out_value, int *out_flags)
{
    const char *name;
    int64_t len, i;
    uint64_t idx = 0;
    int is_len, is_idx = 0;

    if (!JS_IsString(example))
        return 0;
    name = JS_AtomToCString(ctx, atom);
    if (!name)
        return 0;   /* a name that would not convert names no member of a String */
    is_len = !strcmp(name, "length");
    if (!is_len && name[0] >= '0' && name[0] <= '9' && !(name[0] == '0' && name[1] != '\0')) {
        /* Ten digits cover every index (2^32-2 has ten); a longer run leaves `name[i]` non-NUL and answers
           "not a member", as step 8 does for an index past the end. The bound also keeps the accumulate inside
           uint64_t. */
        for (i = 0; name[i] && i < 10; i++) {
            if (name[i] < '0' || name[i] > '9') break;
            idx = idx * 10 + (uint64_t)(name[i] - '0');
        }
        is_idx = (name[i] == '\0');
    }
    JS_FreeCString(ctx, name);
    if (!is_len && !is_idx)
        return 0;
    len = string_example_units(ctx, example);
    if (is_len) {
        *out_value = JS_NewInt64(ctx, len);
        *out_flags = 0;                    /* §10.4.3.4 step 8: not W, not E, not C */
        return 1;
    }
    if (idx >= (uint64_t)len)
        return 0;                          /* §10.4.3.5 step 8: an index at or past the end holds nothing */
    *out_value = JS_GetPropertyUint32(ctx, example, (uint32_t)idx);
    DCHECK(JS_IsString(*out_value),
           "a String primitive answered an IN-RANGE index with something that is not a String — the engine "
           "answers one off the string's own code units and returns before the primitive's prototype is "
           "reached, so a non-String here is a read that fell through to String.prototype, which the page "
           "owns and which this internal method has no flow base to run");
    *out_flags = JS_PROP_ENUMERABLE;       /* §10.4.3.5 step 10: E only */
    return 1;
}

/* One read of the example's own slot, answering both questions asked about a member: what it holds and whether
   it holds it, with its attributes. They are one §6.2.6 The Property Descriptor Specification Type value, so
   one read answers both. 1 = the example holds an own slot for `atom`; `*out_value` is owned (JS_UNDEFINED for
   an accessor) and `*out_flags` is the slot's C/W/E. 0 = it holds none. */
static int example_slot(JSContext *ctx, JSValueConst parent_example, JSAtom atom,
                        JSValue *out_value, int *out_flags)
{
    JSPropertyDescriptor pd;
    int has;

    *out_value = JS_UNDEFINED;
    *out_flags = 0;
    /* A primitive example has the members §10.4.3 gives it and no others, which for everything but a String is
       none (see string_example_slot). */
    if (!JS_IsObject(parent_example))
        return string_example_slot(ctx, parent_example, atom, out_value, out_flags);
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

/* The member's example, read off the parent's by §10.1.8.1 OrdinaryGet ( obj, propertyKey, receiver ): 1 with
   `*out` owned (`undefined` included) where one is held, 0 where none is. The walk decides where the member is:
   the parent's own slot, then each [[Prototype]], a primitive's being its realm's %Number.prototype% and kin
   (§6.2.5.5 GetValue ( refRecord ) step 3.a, §7.1.20 ToObject ( arg )). A data property found anywhere is its
   value, so `x.toFixed` over a Number example carries %Number.prototype.toFixed% for the call over it to run.
   A member no object on the chain holds is `undefined`, step 2.b's answer at a null prototype. An accessor
   holds none: step 7 would call the page's getter, which this read has no flow to run. A Proxy link holds none
   for the same reason; it is refused rather than asserted because a primitive's chain is the page's to change. */
static int example_member(JSContext *ctx, JSValueConst parent_example, JSAtom atom, JSValue *out)
{
    JSValue o, proto;
    int flags = 0, found, held;

    *out = JS_UNDEFINED;
    if (JS_IsUndefined(parent_example) || JS_IsNull(parent_example))
        return 0;   /* GetValue step 3.a's ToObject throws here: a real read has no member to answer */
    found = example_slot(ctx, parent_example, atom, out, &flags);
    o = found ? JS_NULL : JS_GetPrototype(ctx, parent_example);
    while (!found && JS_IsObject(o)) {
        if (JS_IsProxy(o)) {
            JS_FreeValue(ctx, o);
            return 0;
        }
        found = example_slot(ctx, o, atom, out, &flags);
        proto = found ? JS_NULL : JS_GetPrototype(ctx, o);
        JS_FreeValue(ctx, o);
        o = proto;
    }
    CHECK(!JS_IsException(o), "concolic: an example's prototype could not be read");
    /* Where the walk stopped and what the presence bit will say are one fact: a held example is a data
       property's value or the chain's end, never an accessor, and the chain's end holds `undefined`. */
    held = !found || !(flags & JS_PROP_GETSET);
    DCHECK(held ? (found || JS_IsUndefined(*out)) : JS_IsUndefined(*out),
           "a member's example disagrees with where OrdinaryGet found the member — an accessor must hold no "
           "bytes and a member absent along the whole chain must hold `undefined`, or the presence bit the "
           "mint sets from this answer states a value the walk never read");
    return held;
}

/* The [[Prototype]] of each type a ParseJSON value can have, as this realm's intrinsic: an ObjectLiteral's
   %Object.prototype% (ECMAScript §13.2.5.5 Runtime Semantics: Evaluation), an ArrayLiteral's ArrayCreate
   default %Array.prototype% (ECMAScript §10.4.2.2 ArrayCreate ( length [ , proto ] ) step 2), and what
   ECMAScript §7.1.20 ToObject ( arg ) wraps a String, Number or Boolean base in. null has no chain: ECMAScript
   §6.2.5.5 GetValue ( refRecord ) step 3.a throws first. */
static const JSClassID json_type_class[] = {
    JS_CLASS_OBJECT, JS_CLASS_ARRAY, JS_CLASS_STRING, JS_CLASS_NUMBER, JS_CLASS_BOOLEAN,
};

/* §10.1.8.1 OrdinaryGet ( obj, propertyKey, receiver ) for a key no own member of a JSON value holds, asked of
   every type the value can have, since a reply decides the type. 1 with `*out` owned when every chain answers
   the same data value (SameValue; `undefined` where no link holds the key): the read's answer whichever type
   the reply sends. 0 when two chains disagree, or one holds an accessor or a Proxy, whose answer is the
   page's code. `own_length` leaves out the Array and String chains, whose own `length` answers first. */
static int json_chain_member(JSContext *ctx, JSAtom atom, int own_length, JSValue *out)
{
    JSValue first = JS_UNINITIALIZED;
    size_t i;

    *out = JS_UNDEFINED;
    for (i = 0; i < sizeof json_type_class / sizeof json_type_class[0]; i++) {
        JSClassID id = json_type_class[i];
        JSValue proto, v;
        int held, same;

        if (own_length && (id == JS_CLASS_ARRAY || id == JS_CLASS_STRING))
            continue;
        proto = JS_GetClassProto(ctx, id);
        DCHECK(JS_IsObject(proto), "a realm answered a JSON type's intrinsic prototype with no object — the "
                                   "realm is built with all five, so this one was built without one");
        held = example_member(ctx, proto, atom, &v);
        JS_FreeValue(ctx, proto);
        if (!held) {
            JS_FreeValue(ctx, first);
            return 0;
        }
        if (JS_IsUninitialized(first)) {
            first = v;
            continue;
        }
        same = JS_IsSameValue(ctx, first, v);
        JS_FreeValue(ctx, v);
        if (!same) {
            JS_FreeValue(ctx, first);
            return 0;
        }
    }
    DCHECK(!JS_IsUninitialized(first), "no JSON type's chain was asked — at least the Object chain always is");
    *out = first;
    return 1;
}

/* What a Symbol-keyed read of a JSON value stands on, asserted of its example, which is the parse's own
   output: it holds no own member under `atom`, and its [[Prototype]] is this realm's %Object.prototype% or
   %Array.prototype%, so the realm json_chain_member reads intrinsics from is the one that parsed it. */
static void json_value_assert_read(JSContext *ctx, const Concolic *c, JSAtom atom)
{
#if APICLIENT_DEV
    JSValue proto, op, ap, ev;
    int eflags, own;

    if (!c->ex_has)
        return;
    own = example_slot(ctx, c->example, atom, &ev, &eflags);
    JS_FreeValue(ctx, ev);
    DCHECK(!own, "a JSON value's example holds a Symbol-keyed own member — ParseJSON keys every member by a "
                 "String, so this example was built by something other than the parse that minted the value");
    if (!JS_IsObject(c->example))
        return;
    proto = JS_GetPrototype(ctx, c->example);
    op = JS_GetClassProto(ctx, JS_CLASS_OBJECT);
    ap = JS_GetClassProto(ctx, JS_CLASS_ARRAY);
    DCHECK(JS_IsSameValue(ctx, proto, op) || JS_IsSameValue(ctx, proto, ap),
           "a JSON value's example has a [[Prototype]] that is neither this realm's %Object.prototype% nor its "
           "%Array.prototype% — it was parsed in another realm, and the chains json_chain_member reads are this "
           "one's. Carry the parsing realm on the value and read that realm's intrinsics");
    JS_FreeValue(ctx, proto);
    JS_FreeValue(ctx, op);
    JS_FreeValue(ctx, ap);
#else
    (void)ctx; (void)c; (void)atom;
#endif
}

/* Whether a property key is a Symbol: the one key kind no ParseJSON object holds. */
static int atom_is_symbol(JSContext *ctx, JSAtom atom)
{
    JSValue k = JS_AtomToValue(ctx, atom);
    int sym = JS_IsSymbol(k);

    JS_FreeValue(ctx, k);
    return sym;
}

/* Exotic [[Get]]: reading any field of a concolic value yields a derived concolic carrying the field-path
   identity ("{state}.admin"), which is also the @S injection identity. This lets `if (state.admin)` fork and
   lets a candidate inject at a precise source. The derived value carries the parent's example read through,
   so a field the record holds arrives with the server's bytes; either way it stays opaque for control flow,
   because a loaded `features.admin:false` must not concretize the gate or the admin endpoint is lost. */
/* JSConcolicHooks.get_value_base: §6.2.5.5 GetValue ( refRecord ) step 3.a, `? ToObject(refRecord.[[Base]])`,
   over an unknown base, asked by the interpreter's property-read operators before their [[Get]]. ToObject
   throws a TypeError on an undefined or null base, so where this session's own value is one of those the
   session stops at this read, and a flow reading on stands on a world this engine chose. It is asked here and
   not in [[Get]], which every algorithm reaches, many after handling a nullish value themselves
   (§20.1.3.6 Object.prototype.toString answers "[object Undefined]" before its tag read).
   Named residual. Not covered: the session's own world, which throws here and moves on to the page's next
   task; the flow reads on, marked forced, and no flow stands for it. Next diff builds: the get opcodes over
   an unknown ask branch_arm_fork whether the base is nullish, the session's arm raising step 3.a's TypeError
   and the sibling re-reading with that decision replayed. Absence shows as: a document whose every record
   after such a read is graded forced, with no derived record of any later statement. */
static void concolic_get_value_base_hook(JSContext *ctx, JSValueConst base)
{
    JSValue bex;

    DCHECK(concolic_is(base), "GetValue's base hook was asked of a value that is not unknown input");
    if (!concolic_example_present(base)) return;
    bex = concolic_example(ctx, base);
    if (JS_IsUndefined(bex) || JS_IsNull(bex)) flow_mark_forced_arm();
    JS_FreeValue(ctx, bex);
}

/* THE MEMBER STRUCTURE OF A ROOT: which member path of which declared root each member read is, by its `src` and
   its identity. It is a fact about naming, not about a world: a member read's `src` and identity are a pure
   function of its parent's and the field, so one session-wide table answers every flow, and nothing reads a
   structure back out of a spelling. Filled at the member mint; a root's own row (path length 0) is filed with
   its first member. */
typedef struct { char *src, *ident, *root; char **path; int n; } MemberRow;
static MemberRow *g_members;
static int g_members_n, g_members_cap;
static int *g_members_ix, g_members_ix_cap;   /* open addressing over `src`, -1 empty; at most half full */

static uint64_t member_hash(const char *s)
{
    uint64_t h = 1469598103934665603ull;

    for (; *s; s++) { h ^= (unsigned char)*s; h *= 1099511628211ull; }
    return h;
}

static int member_row_of(const char *src)
{
    if (!g_members_ix_cap) return -1;
    for (uint64_t i = member_hash(src) & (uint64_t)(g_members_ix_cap - 1);; i = (i + 1) & (uint64_t)(g_members_ix_cap - 1)) {
        int r = g_members_ix[i];
        if (r < 0) return -1;
        if (!strcmp(g_members[r].src, src)) return r;
    }
}

static void member_ix_put(int row)
{
    uint64_t i = member_hash(g_members[row].src) & (uint64_t)(g_members_ix_cap - 1);

    while (g_members_ix[i] >= 0) i = (i + 1) & (uint64_t)(g_members_ix_cap - 1);
    g_members_ix[i] = row;
}

static void member_ix_grow(void)
{
    int nc = g_members_ix_cap ? g_members_ix_cap * 2 : 64;
    int *ix = malloc((size_t)nc * sizeof *ix);

    CHECK(ix, "concolic: OOM growing the member structure index");
    for (int i = 0; i < nc; i++) ix[i] = -1;
    free(g_members_ix);
    g_members_ix = ix;
    g_members_ix_cap = nc;
    for (int r = 0; r < g_members_n; r++) member_ix_put(r);
}

static int member_row_add(const char *src, const char *ident, const char *root, const char *const *path, int n)
{
    MemberRow *r;

    if (g_members_n == g_members_cap) {
        int nc = g_members_cap ? g_members_cap * 2 : 16;
        MemberRow *g = realloc(g_members, (size_t)nc * sizeof *g);
        CHECK(g, "concolic: OOM growing the member structure table");
        g_members = g; g_members_cap = nc;
    }
    r = &g_members[g_members_n];
    r->src = strdup(src);
    r->ident = ident ? strdup(ident) : NULL;
    r->root = strdup(root);
    r->n = n;
    r->path = n ? malloc((size_t)n * sizeof *r->path) : NULL;
    CHECK(r->src && r->root && (!ident || r->ident) && (!n || r->path), "concolic: OOM filing a member's structure");
    for (int i = 0; i < n; i++) { r->path[i] = strdup(path[i]); CHECK(r->path[i], "concolic: OOM filing a member path"); }
    g_members_n++;
    if (g_members_n * 2 > g_members_ix_cap) member_ix_grow();
    else member_ix_put(g_members_n - 1);
    return g_members_n - 1;
}

/* Files the member read `src` (identity `ident`) of the parent `c` under field `field`, where the parent is a
   declared root's own read or a member already filed. */
static void member_row_file(const Concolic *c, const char *src, const char *ident, const char *field)
{
    int p;

    if (!c->src || !c->root || !field) return;
    p = member_row_of(c->src);
    if (p < 0 && strcmp(c->src, c->root)) return;   /* neither a root's own read nor a filed member */
    if (member_row_of(src) >= 0) return;
    if (p < 0 && !strcmp(c->src, c->root) && root_declared_row(c->root) >= 0)
        p = member_row_add(c->src, c->ident, c->root, NULL, 0);
    if (p < 0) return;
    {
        int n = g_members[p].n + 1;
        const char **path = malloc((size_t)n * sizeof *path);

        CHECK(path, "concolic: OOM composing a member path");
        for (int i = 0; i < n - 1; i++) path[i] = g_members[p].path[i];
        path[n - 1] = field;
        member_row_add(src, ident, g_members[p].root, path, n);
        free(path);
    }
}

void concolic_members_each(const char *root, ConcolicMemberVisit visit, void *ud)
{
    for (int i = 0; i < g_members_n; i++)
        if (!strcmp(g_members[i].root, root))
            visit(g_members[i].src, g_members[i].ident, (const char *const *)g_members[i].path, g_members[i].n, ud);
}

int concolic_member_of_root(const char *src, const char *root)
{
    int r = src ? member_row_of(src) : -1;

    return r >= 0 && !strcmp(g_members[r].root, root);
}

int concolic_value_truth(const char *ident)
{
    const char *f[1] = { ident };
    char *key;
    int arm;

    if (!ident) return -1;
    key = concolic_ident_compose("branch", f, 1);
    CHECK(key, "concolic: OOM composing a value's branch key");
    arm = concolic_branch_decided(key);
    free(key);
    return arm;
}
static char *row_segment(const char *name);

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
    member_row_file(c, shape, ident, field);
    if (field) JS_FreeCString(ctx, field);
    if (cand_matches(shape)) {                               /* candidate run: this source -> the concrete breakout */
        r = concolic_deliver(ctx, shape, c->root, g_cand_bytes);
        free(shape); free(ident);
        return r;
    }
    r = pin_of(ctx, shape);                                  /* an EQ gate pinned this source -> the real value */
    if (!JS_IsUninitialized(r)) {
        free(shape); free(ident);
        return r;
    }
    /* Ties the arm above to the predicate, while both spellings of one question are in hand: `pin_of` and
       `concolic_src_pinned` each test the same chain, and a divergence would be silent in both directions.
       The candidate arm has already returned, so no substitution disjunct is needed here (concolic_new's copy
       needs one because its candidate path falls through). This goes when `pin_of` is routed through the
       predicate, making the divergence unspellable. */
    DCHECK(!src_pinned_unpriced(shape),
           "a source this flow had PINNED reached the ordinary member mint — `pin_of` answered NO PIN for a "
           "path `concolic_src_pinned` says is pinned, so the two spellings of one question over one chain "
           "have diverged. Every later read of this member composes a value this flow has already proved "
           "otherwise, and decide.c's fork-over-pinned row counts a fork the mint was to have decided");
    /* A JSON value's own keys are Strings, so a Symbol names none of them and the read is its type's chain.
       Where every type's chain agrees, that answer is the read's whatever the reply sends: concrete, with no
       unknown to fork on or to deliver a candidate at.
       Named residual. Not covered: chains that disagree (Array and String hold @@iterator; Object, Number and
       Boolean do not) leave the read the unknown member below. Next diff: ask the type through the branch seam
       and answer each arm from its own chain. How its absence shows: a flow that decided `Array.isArray(x)`
       still forks a branch over `x[Symbol.iterator]`. */
    if (c->json_value && atom_is_symbol(ctx, atom)) {
        JSValue v;

        json_value_assert_read(ctx, c, atom);
        if (json_chain_member(ctx, atom, 0, &v)) {
            free(shape); free(ident);
            return v;
        }
    }
    /* src = the field path (a precise @S injection point); root = the parent's, unchanged. A field of an
       unknown object is controlled separately, hence the new injection identity, but arrives by the same route
       as the object's bytes, and a report must say so. */
    {
        JSValue mex;
        int held = c->ex_has && example_member(ctx, c->example, atom, &mex);
        int nrow = c->root ? root_declared_row(c->root) : -1;
        char *nsrc = NULL;

        /* A member of an element named access reached over unknown markup is reached along a longer path, so
           it roots at its own named-access row: the markup that makes this member the payload differs. */
        if (nrow >= 0 && g_srcs[nrow].path && !atom_is_symbol(ctx, atom)) {
            const char *mname = JS_AtomToCString(ctx, atom);

            CHECK(mname != NULL, "concolic: OOM reading a member's name");
            {
                char *seg = row_segment(mname), *path = shapef("%s.%s", g_srcs[nrow].path, mname);

                nsrc = shapef("%s.%s", g_srcs[nrow].src, seg);
                concolic_declare_named_source(nsrc, g_srcs[nrow].parent, path);
                free(seg);
                free(path);
            }
            JS_FreeCString(ctx, mname);
        }
        r = nsrc ? concolic_alloc(ctx, nsrc, nsrc, nsrc, c->root_whose, ident, held ? mex : JS_UNDEFINED)
                 : concolic_alloc(ctx, shape, shape, c->root, c->root_whose, ident, held ? mex : JS_UNDEFINED);
        free(nsrc);
        if (held) {
            Concolic *rc = JS_GetOpaque(r, g_concolic_class);

            DCHECK(rc != NULL, "a member read was minted as something that is not a concolic value");
            rc->ex_has = 1;
        }
    }
    /* A String-keyed member of a JSON value is an own member, itself a JSON value, or absent, when the read
       goes to the type's chain. Where no chain holds the key, the absent read is `undefined` and the member is
       a JSON value too. An Array's or String's own `length` answers before its chain, so that chain is not
       asked for it. */
    if (c->json_value && !atom_is_symbol(ctx, atom)) {
        Concolic *rc = JS_GetOpaque(r, g_concolic_class);
        const char *name = JS_AtomToCString(ctx, atom);
        int is_len = name && !strcmp(name, "length");
        JSValue v;

        if (name) JS_FreeCString(ctx, name);
        DCHECK(rc != NULL, "a member read was minted as something that is not a concolic value");
        if (json_chain_member(ctx, atom, is_len, &v)) {
            rc->json_value = JS_IsUndefined(v);
            JS_FreeValue(ctx, v);
        }
    }
    /* A member read's `src` is its own shape, so it names this value: the precondition the `pin_of` arm above
       stands on. Recorded for readers other than this mint, such as the CSS cascade asking about a value
       `el.setAttribute('t', cfg.theme)` stashed in the DOM, which have only the JSValue. */
    pin_src_names_self(r);
    /* The name this read went through, kept on the value: the one true source for the method name the call
       handler reports, since a property may be called `a.b` and parsing `{x}.a.b` would answer `b`. Re-asked
       of the atom because `field` was freed above. A name that would not convert leaves this NULL, like the
       identity: a call over it states no predicate and both arms stay. */
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
            if (c->src_own == 1 && c->src) {
                rc->facet_src = strdup(c->src);
                CHECK(rc->facet_src, "concolic: OOM naming a member read's parent");
            }
            JS_FreeCString(ctx, name);
        }
    }
    free(shape);
    return r;
}

/* The type bit of a value that is not an unknown. */
static unsigned cty_of_concrete(JSValueConst v)
{
    if (JS_IsUndefined(v)) return CTY_UNDEFINED;
    if (JS_IsNull(v)) return CTY_NULL;
    if (JS_IsBool(v)) return CTY_BOOLEAN;
    if (JS_IsString(v)) return CTY_STRING;
    if (JS_IsSymbol(v)) return CTY_SYMBOL;
    if (JS_IsNumber(v)) return CTY_NUMBER;
    if (JS_IsBigInt(v)) return CTY_BIGINT;
    DCHECK(JS_IsObject(v), "a value of none of ECMAScript §6.1's eight language types was asked its type");
    return CTY_OBJECT;
}

/* A SOURCE'S TYPE FACET is a set of CONCOLIC_TYF_* bits: §6.1's eight language types with Object split by
   [[Call]], since §13.5.3.1 typeof tells the two apart. */
#define TYF_OBJECTS (CONCOLIC_TYF_OBJECT | CONCOLIC_TYF_FUNCTION)

/* §13.5.3.1's table: the types each string it answers covers, 0 for a string it never answers. */
static unsigned tyf_of_typeof(const char *name)
{
    static const struct { const char *name; unsigned tyf; } ROW[] = {
        { "undefined", CONCOLIC_TYF_UNDEFINED }, { "object", CONCOLIC_TYF_NULL | CONCOLIC_TYF_OBJECT },
        { "boolean", CONCOLIC_TYF_BOOLEAN }, { "string", CONCOLIC_TYF_STRING }, { "symbol", CONCOLIC_TYF_SYMBOL },
        { "number", CONCOLIC_TYF_NUMBER }, { "bigint", CONCOLIC_TYF_BIGINT }, { "function", CONCOLIC_TYF_FUNCTION },
    };

    for (size_t i = 0; i < sizeof ROW / sizeof ROW[0]; i++) if (!strcmp(ROW[i].name, name)) return ROW[i].tyf;
    return 0;
}

/* The facet bits a type set covers (every bit for the empty set, which states nothing), and back. */
static unsigned tyf_of_cty(unsigned ty)
{
    unsigned r = 0;

    if (!ty) return CONCOLIC_TYF_ALL;
    if (ty & CTY_UNDEFINED) r |= CONCOLIC_TYF_UNDEFINED;
    if (ty & CTY_NULL) r |= CONCOLIC_TYF_NULL;
    if (ty & CTY_BOOLEAN) r |= CONCOLIC_TYF_BOOLEAN;
    if (ty & CTY_STRING) r |= CONCOLIC_TYF_STRING;
    if (ty & CTY_SYMBOL) r |= CONCOLIC_TYF_SYMBOL;
    if (ty & CTY_NUMBER) r |= CONCOLIC_TYF_NUMBER;
    if (ty & CTY_BIGINT) r |= CONCOLIC_TYF_BIGINT;
    if (ty & CTY_OBJECT) r |= TYF_OBJECTS;
    return r;
}

static unsigned cty_of_tyf(unsigned tyf)
{
    unsigned ty = 0;

    if (tyf & CONCOLIC_TYF_UNDEFINED) ty |= CTY_UNDEFINED;
    if (tyf & CONCOLIC_TYF_NULL) ty |= CTY_NULL;
    if (tyf & CONCOLIC_TYF_BOOLEAN) ty |= CTY_BOOLEAN;
    if (tyf & CONCOLIC_TYF_STRING) ty |= CTY_STRING;
    if (tyf & CONCOLIC_TYF_SYMBOL) ty |= CTY_SYMBOL;
    if (tyf & CONCOLIC_TYF_NUMBER) ty |= CTY_NUMBER;
    if (tyf & CONCOLIC_TYF_BIGINT) ty |= CTY_BIGINT;
    if (tyf & TYF_OBJECTS) ty |= CTY_OBJECT;
    return ty;
}

/* Has any flow filed a type facet? A gate on the lookup below only: a facet is per flow and lives in the
   constraint, so this answers "may one exist", never which. */
static int g_type_facet_any;

/* The types this flow proved `c`'s source does not take: its type facet (Cons.types_out). Only a value that owns
   its `src` is that source; a derivation carries its operand's `src` and is not it. */
static unsigned tyf_out_of(const Concolic *c)
{
    const Cons *e;

    if (!g_type_facet_any || !c || c->src_own != 1 || !c->src) return 0;
    e = cons_lookup(c->src);
    return e ? e->types_out : 0;
}

/* The types an operand takes: a concrete value's one, an unknown's stated set narrowed by its type facet in the
   running flow, 0 for an unknown neither a mint nor a facet typed. */
static unsigned cty_of_operand(JSValueConst v)
{
    const Concolic *c = concolic_is(v) ? JS_GetOpaque(v, g_concolic_class) : NULL;
    unsigned out;

    if (!c) return cty_of_concrete(v);
    out = tyf_out_of(c);
    return out ? cty_of_tyf(tyf_of_cty(c->ty) & ~out) : c->ty;
}

/* §13.5.3.1 typeof's answer where the operand's remaining types fall in one row of its table: its type set and
   its type facet in the running flow. NULL when more than one row remains. */
static const char *tyf_typeof_fixed(JSValueConst v)
{
    static const char *const NAME[] = { "undefined", "object", "boolean", "string", "symbol", "number", "bigint",
                                        "function" };
    const Concolic *c = concolic_is(v) ? JS_GetOpaque(v, g_concolic_class) : NULL;
    unsigned tyf = c ? tyf_of_cty(c->ty) & ~tyf_out_of(c) : tyf_of_cty(cty_of_concrete(v));

    for (size_t i = 0; i < sizeof NAME / sizeof NAME[0]; i++) {
        unsigned row = tyf_of_typeof(NAME[i]);
        if (tyf && !(tyf & ~row)) return NAME[i];
    }
    return NULL;
}

/* The types x may take for §7.2.13 IsLooselyEqual ( x, y ) to answer `holds` against the concrete operand `y`
   (kind `kind`, spelled `tok`), each type decided by the step that answers for it. `num` is ! ToNumber ( y )
   for a String `y` (NaN where it has no numeric value), and `big` whether StringToBigInt ( y ) is defined.
   On the holding arm a type is admitted when some value of it makes the steps answer true; on the failing arm
   when some value makes them answer false, which every type with more than one value does. */
static unsigned tyf_loose(ConcolicLit kind, const char *tok, double num, int big, int holds)
{
    unsigned t = 0;
    double n = kind == CONCOLIC_LIT_NUMBER ? strtod(tok, NULL) : kind == CONCOLIC_LIT_STRING ? num : 0;
    int nan = (kind == CONCOLIC_LIT_NUMBER && (!strcmp(tok, "NaN"))) || (kind == CONCOLIC_LIT_STRING && n != n);
    int zero_one = !nan && (n == 0 || n == 1);

    if (!holds) {
        /* Null and Undefined each hold one value, so the steps decide them outright: steps 2-3 (or step 1 for
           the same type) answer true against null or undefined and false against anything else. */
        t = CONCOLIC_TYF_ALL & ~(CONCOLIC_TYF_NULL | CONCOLIC_TYF_UNDEFINED);
        if (kind != CONCOLIC_LIT_NULL && kind != CONCOLIC_LIT_UNDEFINED)
            t |= CONCOLIC_TYF_NULL | CONCOLIC_TYF_UNDEFINED;
        return t;
    }
    switch (kind) {
    case CONCOLIC_LIT_NULL: case CONCOLIC_LIT_UNDEFINED:
        /* Steps 1-3, and step 4's [[IsHTMLDDA]] object (the host is a web browser). */
        return CONCOLIC_TYF_NULL | CONCOLIC_TYF_UNDEFINED | CONCOLIC_TYF_OBJECT;
    case CONCOLIC_LIT_NUMBER:
        if (nan) return 0;   /* step 1's IsStrictlyEqual and every conversion answer false against NaN */
        t = CONCOLIC_TYF_NUMBER | CONCOLIC_TYF_STRING | TYF_OBJECTS;   /* steps 1, 6, 12 */
        if (zero_one) t |= CONCOLIC_TYF_BOOLEAN;                         /* step 9: ToNumber(x) is 0 or 1 */
        if (isfinite(n) && n == floor(n)) t |= CONCOLIC_TYF_BIGINT;      /* step 13: an integral value */
        return t;
    case CONCOLIC_LIT_STRING:
        t = CONCOLIC_TYF_STRING | TYF_OBJECTS;                           /* steps 1 and 12 */
        if (!nan) t |= CONCOLIC_TYF_NUMBER;                              /* step 5 */
        if (zero_one) t |= CONCOLIC_TYF_BOOLEAN;                         /* steps 9 then 5 */
        if (big) t |= CONCOLIC_TYF_BIGINT;                               /* step 7 */
        return t;
    case CONCOLIC_LIT_BOOL:
        /* Step 10 makes y a Number, 0 or 1, which every type step 1, 6, 12 or 13 relates to it admits. */
        return CONCOLIC_TYF_BOOLEAN | CONCOLIC_TYF_NUMBER | CONCOLIC_TYF_STRING | CONCOLIC_TYF_BIGINT | TYF_OBJECTS;
    case CONCOLIC_LIT_BIGINT:
        t = CONCOLIC_TYF_BIGINT | CONCOLIC_TYF_NUMBER | CONCOLIC_TYF_STRING | TYF_OBJECTS;   /* steps 1, 8, 12, 13 */
        if (!strcmp(tok, "0") || !strcmp(tok, "1")) t |= CONCOLIC_TYF_BOOLEAN;           /* step 9 */
        return t;
    default:
        return CONCOLIC_TYF_ALL;
    }
}

/* The types x may take for §7.2.14 IsStrictlyEqual ( x, y ) to answer `holds` against the concrete `y`: step 1
   makes the holding arm y's own type, and on the failing arm only a one-value type equal to y is excluded. */
static unsigned tyf_strict(ConcolicLit kind, const char *tok, int holds)
{
    unsigned own = kind == CONCOLIC_LIT_STRING ? CONCOLIC_TYF_STRING : kind == CONCOLIC_LIT_NUMBER ? CONCOLIC_TYF_NUMBER
                 : kind == CONCOLIC_LIT_BOOL ? CONCOLIC_TYF_BOOLEAN : kind == CONCOLIC_LIT_NULL ? CONCOLIC_TYF_NULL
                 : kind == CONCOLIC_LIT_UNDEFINED ? CONCOLIC_TYF_UNDEFINED : kind == CONCOLIC_LIT_BIGINT ? CONCOLIC_TYF_BIGINT
                 : 0;

    if (!own) return CONCOLIC_TYF_ALL;
    if (holds) return kind == CONCOLIC_LIT_NUMBER && !strcmp(tok, "NaN") ? 0 : own;   /* step 2's Number::equal */
    return own & (CONCOLIC_TYF_NULL | CONCOLIC_TYF_UNDEFINED) ? CONCOLIC_TYF_ALL & ~own : CONCOLIC_TYF_ALL;
}

/* A facet left with no type is a world no value produces: an equality's emptied arm is refined away before it
   is filed (concolic_equality_outcome), so the narrowings that can reach it are an operation that throws a
   TypeError for every remaining type, a throw arm its hook does not fork. `file`/`line` are the narrowing's. */
#define type_facet_narrow(src, admitted) type_facet_narrow_at((src), (admitted), __FILE__, __LINE__)
static void type_facet_narrow_at(const char *src, unsigned admitted, const char *file, int line)
{
    Cons *e = cons_entry(src);

    e->types_out |= (unsigned short)(CONCOLIC_TYF_ALL & ~admitted);
    g_type_facet_any = 1;
    DCHECKF(e->types_out != CONCOLIC_TYF_ALL,
            "%s:%d left %s no type: the operation throws a TypeError in every world this flow admits — build "
            "its throw arm as a forked completion, as a throwing builtin forks one", file, line, src);
}

/* The types whose values hold a callable under the property `name`, as the running realm's intrinsic chains
   stand: Undefined and Null hold none (§6.2.5.5 GetValue ( refRecord ) step 3.a's ToObject throws), a
   primitive type holds one where its %T.prototype% chain does (an accessor or a Proxy link answers by the
   page's code, so it keeps the type), and an Object or a function may hold its own. */
static unsigned tyf_callable_member(JSContext *ctx, const char *name)
{
    static const struct { JSClassID id; unsigned tyf; } PRIM[] = {
        { JS_CLASS_BOOLEAN, CONCOLIC_TYF_BOOLEAN }, { JS_CLASS_STRING, CONCOLIC_TYF_STRING },
        { JS_CLASS_SYMBOL, CONCOLIC_TYF_SYMBOL }, { JS_CLASS_NUMBER, CONCOLIC_TYF_NUMBER },
        { JS_CLASS_BIG_INT, CONCOLIC_TYF_BIGINT },
    };
    JSAtom atom = JS_NewAtom(ctx, name);
    unsigned t = TYF_OBJECTS;

    CHECK(atom != JS_ATOM_NULL, "concolic: OOM naming a called member");
    for (size_t i = 0; i < sizeof PRIM / sizeof PRIM[0]; i++) {
        JSValue proto = JS_GetClassProto(ctx, PRIM[i].id), v;

        DCHECK(JS_IsObject(proto), "a realm answered a primitive type's intrinsic prototype with no object");
        if (!example_member(ctx, proto, atom, &v) || JS_IsFunction(ctx, v)) t |= PRIM[i].tyf;
        JS_FreeValue(ctx, v);
        JS_FreeValue(ctx, proto);
    }
    JS_FreeAtom(ctx, atom);
    return t;
}

void concolic_ordering_facet(JSValueConst cond, int holds)
{
    const Concolic *c = g_concolic_class ? JS_GetOpaque(cond, g_concolic_class) : NULL;

    DCHECK(c != NULL && c->rel_op != REL_NONE, "an ordering facet was asked of a value that records no ordering");
    /* Against a finite Number: §7.2.12 IsLessThan ( x, y, leftFirst ) steps 7-8's ToNumeric throws for a Symbol,
       and an Undefined x is NaN, for which step 9.a's Number::lessThan answers undefined, which every §13.10.1
       relational production returns as false. */
    if (c->facet_src)
        type_facet_narrow(c->facet_src, CONCOLIC_TYF_ALL & ~CONCOLIC_TYF_SYMBOL &
                                        ~(holds ? CONCOLIC_TYF_UNDEFINED : 0u));
}

/* The types the equality `c` admits for the value it compares on the arm where it `holds`, and in `*subject`
   the `src` whose facet holds that value's types (NULL where it owns none): a typeof outcome by §13.5.3.1's
   row for its operand, a `==` by §7.2.13, any other equality by §7.2.14. */
static unsigned eq_admitted(JSContext *ctx, const Concolic *c, int holds, const char **subject)
{
    double num = 0;
    int big = 0;

    *subject = NULL;
    if (c->cmp_typeof) {
        unsigned row = c->cmp_kind == CONCOLIC_LIT_STRING ? tyf_of_typeof(c->cmp_tok) : 0;

        *subject = c->cmp_typeof_of;
        return holds ? row : CONCOLIC_TYF_ALL & ~row;
    }
    if (c->cmp_src_own == 1 && c->src) *subject = c->src;
    if (c->cmp_algo != JS_CONCOLIC_EQ_LOOSE) return tyf_strict((ConcolicLit)c->cmp_kind, c->cmp_tok, holds);
    if (c->cmp_kind == CONCOLIC_LIT_STRING) {
        JSValue str = JS_NewString(ctx, c->cmp_tok);
        double d;

        CHECK(!JS_IsException(str), "concolic: OOM converting an equality's token");
        CHECK(JS_ToFloat64(ctx, &d, str) == 0, "concolic: ToNumber of a String token threw");
        num = d;
        big = JS_StringToBigIntDefined(ctx, str);
        CHECK(big >= 0, "concolic: StringToBigInt of a String token threw");
        JS_FreeValue(ctx, str);
    }
    return tyf_loose((ConcolicLit)c->cmp_kind, c->cmp_tok, num, big, holds);
}

int concolic_equality_facet(JSContext *ctx, JSValueConst cond, int holds)
{
    const Concolic *c = g_concolic_class ? JS_GetOpaque(cond, g_concolic_class) : NULL;
    const char *subject;
    unsigned admitted;

    DCHECK(c != NULL && c->cmp_op != OPCMP_NONE && c->cmp_tok != NULL,
           "an equality facet was asked of a value that records no equality with a token");
    admitted = eq_admitted(ctx, c, holds, &subject);
    if (subject) type_facet_narrow(subject, admitted);
    return c->cmp_typeof != 0;
}

int concolic_equality_outcome(JSContext *ctx, JSValueConst cond)
{
    const Concolic *c = g_concolic_class ? JS_GetOpaque(cond, g_concolic_class) : NULL;
    const char *subject;
    unsigned can_hold, can_fail, types;

    if (!c || c->cmp_op == OPCMP_NONE || !c->cmp_tok) return -1;
    DCHECK(c->cmp_subj_types != 0, "an equality reached its outcome with no stated subject type set — both "
                                   "equality mints state one");
    can_hold = eq_admitted(ctx, c, 1, &subject);
    can_fail = eq_admitted(ctx, c, 0, &subject);
    types = c->cmp_subj_types & (subject ? concolic_types(subject) : CONCOLIC_TYF_ALL);
    DCHECKF(types & (can_hold | can_fail),
            "an equality against %s (typeof subject %d) over %s admits types 0x%x holding and 0x%x failing, and "
            "its subject takes only 0x%x (stated 0x%x): every arm is a world no value produces", c->cmp_tok,
            c->cmp_typeof, subject ? subject : "(no src)", can_hold, can_fail, types, c->cmp_subj_types);
    if ((types & can_hold) && (types & can_fail)) return -1;
    return (types & can_hold) != 0;
}

int concolic_cmp_is_typeof(JSValueConst cond)
{
    const Concolic *c = g_concolic_class ? JS_GetOpaque(cond, g_concolic_class) : NULL;

    return c && c->cmp_typeof;
}

unsigned concolic_types(const char *src)
{
    const Cons *e = src && g_type_facet_any ? cons_lookup(src) : NULL;

    return CONCOLIC_TYF_ALL & ~(e ? e->types_out : 0u);
}

/* State a fresh mint's type set: what its algorithm fixes the result's type to. Once per value, and the
   example the mint attached (if any) is one of those types. A candidate re-fire's mint answers the attacker's
   bytes, a real value with no record, and states nothing. `file`/`line` are the mint's. */
#define concolic_type_state(r, ty) concolic_type_state_at((r), (ty), __FILE__, __LINE__)
static void concolic_type_state_at(JSValueConst r, unsigned ty, const char *file, int line)
{
    Concolic *c;

    if (!concolic_is(r)) return;
    c = JS_GetOpaque(r, g_concolic_class);
    DCHECKF(ty != 0 && c->ty == 0, "%s:%d stated a type set that is empty, or stated one twice", file, line);
    DCHECKF(!c->ex_has || (cty_of_concrete(c->example) & ty),
            "%s:%d minted a value whose example is of a type outside the set its algorithm fixes", file, line);
    c->ty = (uint8_t)ty;
}

/* The equality algorithms, spelled once: the identity's operator field. concolic_new_cmp composes the same
   field for a component whose IDL member is a comparison, and the two must agree exactly or a page writing
   the comparison the component declares would fork again over the same predicate. There is no inequality
   spelling: concolic_cmp_hook mints `!=` and `!==` as negations of these. */
static const char *cmp_op_ident(JSConcolicEqOp op) {
    if (op == JS_CONCOLIC_EQ_STRICT) return "===";
    /* Not `===`'s spelling: the two answer NaN differently, so one key would let either decide the other. */
    if (op == JS_CONCOLIC_EQ_SAME_VALUE_ZERO) return "SameValueZero";
    DCHECK(op == JS_CONCOLIC_EQ_LOOSE,
           "an equality was spelled for an algorithm quickjs.h does not declare — the identity would name a "
           "predicate no operator produces, and two real ones would compose under it");
    return "==";
}

/* Mint a comparison result: the boolean `if (x === 'admin')` and `if (x < 700)` branch on. Its identity is
 * the operator and both operands, the whole predicate, so to decide.c it is an ordinary derived value.
 * `ia`/`ib` are the operands' identities in composition order and are consumed. `eq_kind` and `tok` are the
 * pin, which only an equality against a concrete side has; an ordering narrows a domain and determines no
 * value. `shape` is the predicate as the page wrote it, each operand spelled by derived_operand_shape, and is
 * consumed: it is what a report states a decision over this result as. */
static JSValue pred_new(JSContext *ctx, const char *op, const char *src, const char *root,
                        unsigned root_whose, char *ia, char *ib,
                        int eq_kind, int algo, ConcolicLit tok_kind, const char *tok, const char *subj,
                        char *shape)
{
    const char *f[3];
    char *ident;
    JSValue r;
    Concolic *c;

    DCHECK(op != NULL,
           "a comparison result was minted with no OPERATOR. The operator is half the predicate: without it "
           "`x < 700` and `x > 700` compose to one identity, the flow's record of either DECIDES the other, "
           "and the arm it deletes is one nothing contradicts");
    /* The pin is three fields: the operator says an arm determines something, the token says what, and the
       subject says which hole the report states it under; decide.c reads all three at one line, pinning on one
       arm and excluding on the other. Only the subject may be absent: a value shaped `{}` has no hole the @H
       surface prints (endpoint.c's path scan mints no param for one), so there is nothing to file a domain
       under. The pin is unaffected; it is keyed by `src`. */
    DCHECK(eq_kind == OPCMP_NONE ? (tok == NULL && subj == NULL) : (tok != NULL),
           "a comparison result's operator and pin token disagree about whether its arms determine anything "
           "— the two are written together at this mint and read together by decide.c, which pins on one arm "
           "and records the exclusion on the other");
    /* The token and its kind are one argument, asserted where they meet: without the kind decide.c would pin
       nine characters for `undefined`, and a kind with no spelling states a type about nothing. Neither is
       recoverable downstream, where only the pair is left. */
    DCHECK((tok != NULL) == (tok_kind != CONCOLIC_LIT_NONE),
           "a comparison result's pin token and its KIND disagree about whether the concrete side can be "
           "spelled at all — the two come from one classification (operand_kind, through literal_tok), so a "
           "value carrying one without the other is a pin whose own mint cannot say what type it pinned");
    /* The algorithm too, for the same reason: it says what the holding arm may conclude. An ordering has
       none; an equality may not omit it, because JS_CONCOLIC_EQ_LOOSE is zero and an omission would claim the
       page wrote `==`. */
    DCHECK(eq_kind == OPCMP_NONE ? algo == CMP_ALGO_NONE
                                 : (algo == JS_CONCOLIC_EQ_LOOSE || algo == JS_CONCOLIC_EQ_STRICT ||
                                    algo == JS_CONCOLIC_EQ_SAME_VALUE_ZERO),
           "a comparison result's operator and its equality ALGORITHM disagree about whether it is an equality "
           "at all — an ordering carrying an algorithm names a §7.2.13/§7.2.14 reading of a predicate neither "
           "defines, and an equality without one is a pin whose arm cannot say whether it determined a value "
           "or merely bounded it");
    DCHECK(shape != NULL, "a comparison result was minted with no display shape");
    f[0] = op; f[1] = ia; f[2] = ib;
    ident = concolic_ident_compose("?", f, 3);
    free(ia); free(ib);
    /* Not a source read, so no candidate substitution (see concolic_alloc's declaration). */
    r = concolic_alloc(ctx, shape, src, root, root_whose, ident, JS_UNDEFINED);
    free(shape);
    c = JS_GetOpaque(r, g_concolic_class);
    DCHECK(c != NULL, "a comparison result was minted as something that is not a concolic value");
    c->cmp_op = eq_kind;
    c->cmp_tok = tok ? strdup(tok) : NULL;
    CHECK(!tok || c->cmp_tok, "concolic: OOM recording the value an equality pins to");
    c->cmp_kind = (signed char)tok_kind;
    c->cmp_algo = (signed char)algo;   /* what the arm may conclude; written with `cmp_op` */
    c->cmp_subj = subj ? strdup(subj) : NULL;
    CHECK(!subj || c->cmp_subj, "concolic: OOM recording the hole an equality is a fact about");
    concolic_type_state(r, CTY_BOOLEAN);   /* §7.2.13, §7.2.14 and §7.2.12 IsLessThan each answer a Boolean */
    return r;
}

/* The unknown operand's own identity, stamped on a predicate pred_new has just minted; a second call for
   pred_set_bound's reason. Only the equality hook has an operand value to name: concolic_new_cmp's operand is
   a component's own member (HTML §6.2 Page visibility's `hidden`), so no minted operand exists to file an
   example against. Asserted as a pair with the hole: both name one operand, and decide.c would otherwise
   exclude a value it cannot name in the report, or the reverse. */
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

/* …and the same identity for an ordering whose other side is known: §7.2.12 IsLessThan's example was run on
   this operand's example and a constant, so an arm contradicting it contradicts the operand's example, as an
   equality's does. An ordering files no exclusion, so it needs no hole. */
static void pred_set_order_subject(JSValueConst pred, const char *ident) {
    Concolic *c = JS_GetOpaque(pred, g_concolic_class);

    DCHECK(c != NULL && c->cmp_op == OPCMP_NONE,
           "an ordering's operand identity was stamped on something that is not an ordering result");
    DCHECK(c->cmp_subj_ident == NULL, "an ordering was given a second subject identity");
    c->cmp_subj_ident = strdup(ident);
    CHECK(c->cmp_subj_ident, "concolic: OOM recording the value an ordering is a fact about");
}

/* The ordering's observation, stamped on a predicate pred_new has just minted. A second call rather than four
   more parameters, because only the ordering hook has one and every other caller would spell `REL_NONE, NULL,
   0, NULL`. The facts are written here together, so the one-observation assert has a single site. */
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

/* The call predicate's observation, stamped on the result concolic_call_hook has just minted; a second call for
   pred_set_bound's reason. The facts are written here together. `args` is consumed (the array and every
   string), because the caller has just built the only copy. */
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

/* Carry a predicate's observation onto its negation, unchanged.
 *
 * decide.c reads the equality, the ordering and the call predicate together with the arm: it pins on the arm
 * that makes the equality true and excludes on the other, states `rel` or `rel_op_negate(rel)` by the arm, and
 * files `holds` as the arm. So the negation lives in the arm (one XOR via `br_neg`), not in the record, which
 * would need three kinds of negation, one of which does not exist: a call predicate has no paired method.
 * This is not a derivation's inversion: `!p` and `p` are the same boolean read through §7.1.2 ToBoolean ( arg ),
 * so nothing is undone (unlike carrying `x.slice(1).startsWith("/api")` onto `x`, which would undo `slice`).
 *
 * `c->member` is not carried: a negation reads no property, and the call handler reports from that field.
 * `c->conj` is not carried either: `!(p ∧ q)` is not a conjunction, and copying the list would let
 * `(!(p ∧ q)) ∧ r` flatten into `p ∧ q ∧ r`, a different proposition. The negation enters a parent set as
 * one opaque member. */
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
    d->cmp_kind = s->cmp_kind;   /* the token's other half, carried with it */
    d->cmp_src_own = s->cmp_src_own;   /* …and where its determination may be filed */
    d->cmp_algo = s->cmp_algo;   /* …and what the arm may conclude: `!(x == L)` is `x == L` read the other way
                                    round, so dropping it would make a `==` look like a predicate with no
                                    algorithm. */
    if (s->cmp_subj)       { d->cmp_subj       = strdup(s->cmp_subj);       CHECK(d->cmp_subj,       "concolic: OOM carrying an equality's hole through a negation"); }
    if (s->cmp_subj_ident) { d->cmp_subj_ident = strdup(s->cmp_subj_ident); CHECK(d->cmp_subj_ident, "concolic: OOM carrying an equality's subject identity through a negation"); }
    if (s->cmp_typeof_of)  { d->cmp_typeof_of  = strdup(s->cmp_typeof_of);  CHECK(d->cmp_typeof_of,  "concolic: OOM carrying a typeof equality's operand through a negation"); }
    d->cmp_typeof = s->cmp_typeof;
    d->cmp_subj_types = s->cmp_subj_types;
    d->rel_op  = s->rel_op;
    d->rel_num = s->rel_num;
    if (s->rel_op != REL_NONE && s->facet_src) {
        d->facet_src = strdup(s->facet_src);
        CHECK(d->facet_src, "concolic: OOM carrying an ordering's subject through a negation");
    }
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

/* A comparison-result bool carrying `src <op> tok`, for a component whose IDL member is a comparison over its
   own source (HTML §6.2 Page visibility's `document.hidden` is `visibilityState === "hidden"`). It composes the
   identity the page's own `x === tok` composes, which makes the two one constraint entry (page_visibility.h
   relies on this). */
JSValue concolic_new_cmp(JSContext *ctx, const char *src, int op, ConcolicLit kind, const char *tok) {
    const char *sf[1], *kf[2];
    JSValue r;
    char *shape, *ts;

    DCHECK(op == OPCMP_EQ,
           "a comparison result was declared with an operator that is not an equality — an inequality is the "
           "negation of one (the caller negates this result), and an ordering is minted by the relational hook");
    /* The component states the kind: only the caller knows it. A member comparing against a number must
       compose the key the page's `x === 5` composes, not the key of a comparison against the string of its
       digits. */
    DCHECK(kind != CONCOLIC_LIT_NONE,
           "a component declared its member as a comparison against an operand this engine cannot spell — an "
           "Object or a Symbol has no identity that survives the park a resumed flow replays through, so a "
           "member whose IDL says it compares against one is stating a predicate no key can name");
    sf[0] = src;
    kf[0] = lit_tag(kind); kf[1] = tok;
    ts = lit_shape(kind, tok);
    shape = shapef("{%s}%s%s", src ? src : "", cmp_op_ident(JS_CONCOLIC_EQ_STRICT), ts);
    free(ts);
    /* The strict spelling, which makes this one entry with the page's own test: these members are defined as
       strict string equalities (HTML §6.2 Page visibility: "The `hidden` getter steps are to return true if
       this's visibility state is "hidden", otherwise false"), so a page writing the comparison out reaches
       concolic_cmp_hook with JS_CONCOLIC_EQ_STRICT. A loose IDL comparison would have to say so here.
       The member is a source read, so it is its own root, as concolic_new's is. Its hole is `src`: a declared
       source's shape is its provenance in braces (asserted by concolic_source_wrap).
       Whose unknown it is is stated here rather than asked of callers, because this entry's contract fixes it:
       a browser component's own declared member, which a person, a server or the environment drives. A door
       whose callers are not all one kind takes the word as a parameter; this one does not. */
    r = pred_new(ctx, cmp_op_ident(JS_CONCOLIC_EQ_STRICT), src, src,
                 src ? (1u << CONCOLIC_WHOSE_WORLD) : 0u,
                 concolic_ident_compose("s", sf, 1), concolic_ident_compose("k", kf, 2),
                 op, JS_CONCOLIC_EQ_STRICT, kind, tok, src, shape);
    /* The member compares its own source, so its subject is the value `src` names, typed by its facet alone. */
    pred_set_src_own(r, 1);
    pred_set_subject_types(r, CONCOLIC_TYF_ALL);
    return r;
}
/* …and the twin for a relation over two live values, for a component whose own algorithm compares two
 * operands either of which may be unknown (HTML §8.7 Timers orders one expiry against another).
 * It keeps one speller of the key: concolic_rel_hook takes the interpreter's own opcode, which QuickJS does
 * not export, and decide.c keys by the value's identity so the format lives in one place. The component names
 * the spec relation it performs and this file composes the identity, as it composes `rel%d` for the
 * interpreter; length-prefixed fields keep the two namespaces apart.
 * It pins nothing: an ordering determines no value, and an equality whose other side is unknown has no
 * concrete value to pin to. Both operands are borrowed. */
JSValue concolic_new_rel(JSContext *ctx, const char *op, JSValueConst a, JSValueConst b) {
    JSValueConst opq;
    char *sa, *sb, *shape;

    DCHECK(op != NULL,
           "a component asked for a relation over two values without naming the RELATION — the operator is "
           "half the predicate, so two different comparisons of one pair would compose to one identity and "
           "the flow's record of either would decide the other");
    DCHECK(concolic_is(a) || concolic_is(b),
           "a component asked the solver to relate two values NEITHER of which is unknown — a relation over "
           "two concrete operands is decided by running it, and minting a predicate for it would put a fork "
           "in the frontier over a question the engine can already answer");
    opq = concolic_is(a) ? a : b;
    sa = derived_operand_shape(ctx, a);
    sb = derived_operand_shape(ctx, b);
    shape = shapef("%s(%s, %s)", op, sa, sb);
    free(sa); free(sb);
    return pred_new(ctx, op, concolic_src_c(opq), concolic_root_c(opq), root_whose_of(opq),
                    ident_of_operand(ctx, a), ident_of_operand(ctx, b),
                    OPCMP_NONE, CMP_ALGO_NONE, CONCOLIC_LIT_NONE, NULL, NULL, shape);
}

/* The tag that puts a conjunction in its own namespace. Length-prefixed fields keep any tag from spelling
   another's boundary, and the bitwise `&` (carith_name) is a different string anyway. Spelled once because
   the identity and the record's invariant are both about it. */
#define CONCOLIC_CONJ_TAG "&&"

/* The conjunction's example: the real `&&` on the conjuncts' examples, attached only when both carry one.
   Read through concolic_example, never off the record, so an example the running flow has proved wrong is
   withheld and the conjunction arrives with none, and decide.c marks neither arm primary rather than the
   wrong one. */
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

/* …and the mint over two predicates (concolic.h says why a component cannot answer this, why the identity is
 * the set of conjuncts, and why a duplicate is deduplicated here where a joint provenance's crashes). Both
 * operands are borrowed; the result is owned. */
JSValue concolic_new_conj(JSContext *ctx, JSValueConst a, JSValueConst b)
{
    Concolic *side[2], *c;
    const char **members, **sorted;
    int *order;
    int n = 0, u, i, k;
    char *ident, *shape;
    JSValue res;

    DCHECK(concolic_is(a) && concolic_is(b),
           "a conjunction was minted over an operand that is NOT unknown — a decided boolean is one the "
           "caller folds away by handing back the other operand, and composing a key over it would put a "
           "fork in the frontier over a question this engine has already answered by running it");

    side[0] = JS_GetOpaque(a, g_concolic_class);
    side[1] = JS_GetOpaque(b, g_concolic_class);
    /* The conjunction as written, which is what a report states a decision over it as. The identity is the
       set and the shape the order the page gave; one set may have two shapes, never one shape two sets. */
    shape = shapef("%s&&%s", concolic_shape_c(a) ? concolic_shape_c(a) : "{}",
                   concolic_shape_c(b) ? concolic_shape_c(b) : "{}");
    /* The members, flattened: a conjunct that is itself a conjunction contributes its members, so
       `(p ∧ q) ∧ r` and `p ∧ (q ∧ r)` are one set and one key. */
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

    /* An unspellable member makes the whole conjunction unspellable: a value with no name is never decided
       from another value's record, so both arms of every branch over it stay. */
    for (i = 0; i < n; i++)
        if (!members[i]) {
            free(members); free(order);
            res = concolic_alloc(ctx, shape, NULL, NULL, 0u, NULL, conj_example(ctx, a, b));
            free(shape);
            concolic_type_state(res, CTY_BOOLEAN);
            return res;
        }

    ident_set_order(members, n, order);
    /* Deduplicated, where a joint provenance's duplicate crashes (concolic.h says which caller could have
       known). `order[u - 1]` is the last member kept and `order[i]` is not yet written, since `u <= i`. */
    for (i = 1, u = 1; i < n; i++)
        if (strcmp(members[order[u - 1]], members[order[i]]) != 0) order[u++] = order[i];
    DCHECK(u >= 1 && u <= n, "a conjunction's set came back with a member count outside the multiset it was "
                             "compacted from");
    n = u;

    if (n == 1) {
        /* `p ∧ p` is `p`, so the answer is that predicate, not a second key for it. Only two atoms can collapse
           this far: a conjunction operand contributes at least two distinct members. */
        DCHECK(side[0]->conj_n == 0 && side[1]->conj_n == 0,
               "a conjunction over a set that collapsed to ONE member had a conjunction as an operand — that "
               "operand's own members are distinct by construction, so this is a set that was stored without "
               "going through this mint's deduplication");
        free(members); free(order); free(shape);
        return JS_DupValue(ctx, a);
    }

    sorted = reclaim_malloc((size_t)n * sizeof *sorted);
    CHECK(sorted, "concolic: OOM ordering a conjunction's conjuncts");
    for (i = 0; i < n; i++) sorted[i] = members[order[i]];
    ident = concolic_ident_compose(CONCOLIC_CONJ_TAG, sorted, n);
    DCHECK(ident != NULL, "a conjunction's identity came back absent over members this mint had already "
                          "established are all spellable — concolic_ident_compose answers NULL only for a "
                          "NULL member, and the scan above rejected every one of those");

    /* Not a source read (concolic_alloc, never concolic_derived): a conjunction is a boolean this engine
       computed, and an @S candidate substituted into it would answer a predicate with a payload. */
    res = concolic_alloc(ctx, shape, NULL, NULL, 0u, ident, conj_example(ctx, a, b));
    concolic_type_state(res, CTY_BOOLEAN);
    free(shape);
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
    /* The kind is answered wherever the token is; asserted here, the one place that still sees the value both
       were written on. A spelling with no kind would be pinned as a string. */
    DCHECK(c->cmp_tok == NULL || c->cmp_kind != (signed char)CONCOLIC_LIT_NONE,
           "a comparison result carries a pin token with no KIND — the two are written together at pred_new "
           "and asserted together there, so a value holding only the spelling is one minted somewhere that "
           "does not go through that mint, and its pin would concretize a source to the wrong type");
    if (psrc) *psrc = c->src;
    if (pkind) *pkind = (ConcolicLit)c->cmp_kind;
    if (ptok) *ptok = c->cmp_tok;
    return c->cmp_op;
}

/* Which equality the page wrote (see concolic.h). A second read rather than an out-parameter on concolic_cmp
   for pred_set_subject_ident's reason: only the pinning arm asks it. A caller reaching it on a non-equality
   aborts rather than being given a default. */
JSConcolicEqOp concolic_cmp_algo(JSValueConst v) {
    Concolic *c = g_concolic_class ? JS_GetOpaque(v, g_concolic_class) : NULL;

    DCHECK(c != NULL && c->cmp_op != OPCMP_NONE,
           "the equality ALGORITHM was asked of a value that records no equality — an ordering and a call "
           "predicate have none, so the only answer available here would be a §7.2.13/§7.2.14 reading of a "
           "predicate neither defines, and the caller would pin off it");
    DCHECK(c != NULL && (c->cmp_algo == JS_CONCOLIC_EQ_LOOSE || c->cmp_algo == JS_CONCOLIC_EQ_STRICT ||
                         c->cmp_algo == JS_CONCOLIC_EQ_SAME_VALUE_ZERO),
           "an equality result carries an algorithm quickjs.h does not declare — pred_new writes it beside "
           "`cmp_op` and asserts the pair there, so a value holding one without the other is minted somewhere "
           "that does not go through that mint");
    /* The release arm answers the refusing algorithm rather than dereferencing NULL, since both asserts above
       compile out. LOOSE is the arm that makes no pin: a release build standing where dev aborts loses one
       narrowing and the source forks again at its next gate, which is sound. STRICT would concretize a source
       off a record that could not say it determined anything. */
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

/* JSConcolicCmpHook for == / === / != / !==: a concolic operand yields a concolic bool whose identity is the
   operator the page wrote and both operands, carrying the pin when one side is concrete. Matches the slow-eq
   stack effect (both operands freed, result in sp[-2]). `is_neq` flips EQ to NE; `op` names the algorithm. */
/* Whether an operator hook may run its algorithm over an example. No for an Object or a Symbol, which is a
   legitimate example for a value whose domain holds structured values (a routed message's `data`, a published
   record): §7.1.1 ToPrimitive over it runs the page's own code (valueOf, toString, @@toPrimitive), which a hook
   cannot run from C, and §7.2.14 IsStrictlyEqual ( x, y ) would compare the identity of an object this engine
   minted. Such an operand gives the result no example, so decide.c observes no arm and the domain, not the
   example's type, decides what runs. */
static int example_operable(JSValueConst v) {
    return !JS_IsObject(v) && !JS_IsSymbol(v);
}

static ConcolicExState example_state_of(const Concolic *c, const Cons **ppin);
/* See concolic.h. concolic_example answers JS_UNDEFINED for absence too, so every site that runs an operation
   over a concolic operand's example — `"a" + x`, `x * 2`, String(x), !x, the engine's builtins through
   `example_held`, the IDL conversions — asks this rather than testing the example for `undefined`. */
int concolic_example_present(JSValueConst v) {
    const Concolic *c = g_concolic_class ? JS_GetOpaque(v, g_concolic_class) : NULL;
    const Cons *pin;
    ConcolicExState st = example_state_of(c, &pin);

    return st == CONCOLIC_EX_HELD || st == CONCOLIC_EX_DETERMINED;
}

/* See concolic.h. The answer is a string or absent, so no caller can read a held `undefined` as absence. */
void concolic_state_string(JSValueConst v) { concolic_type_state(v, CTY_STRING); }
void concolic_state_string_or_null(JSValueConst v) { concolic_type_state(v, CTY_STRING | CTY_NULL); }

int concolic_type_is_string(JSValueConst v) { return concolic_is(v) && cty_of_operand(v) == CTY_STRING; }

unsigned concolic_operand_types(JSValueConst v)
{
    DCHECK(concolic_is(v), "an unknown's type set was asked of a value that is not unknown input — a concrete "
                           "value's type is its tag, which the caller reads itself");
    return tyf_of_cty(cty_of_operand(v));
}

JSValue concolic_example_string(JSContext *ctx, JSValueConst v) {
    JSValue ex, s;

    DCHECK(concolic_is(v), "an example's ToString was asked of a value that is not a concolic — a determined "
                           "operand is converted by the caller's own ToString, which may run page code");
    if (!concolic_example_present(v)) return JS_UNDEFINED;
    ex = concolic_example(ctx, v);
    if (!example_operable(ex)) { JS_FreeValue(ctx, ex); return JS_UNDEFINED; }
    s = JS_ToString(ctx, ex);
    JS_FreeValue(ctx, ex);
    CHECK(!JS_IsException(s), "OOM converting a concolic's primitive example with §7.1.19 ToString");
    return s;
}

/* The comparison's own example: the engine's real equality run over values the run already holds
 * (`JS_IsStrictEqual` is §7.2.14, `JS_IsEqual` is §7.2.13), never a re-implementation of either. The caller
 * states which algorithm through `op`, which matters because `"1" == 1` is true and `"1" === 1` is false.
 *
 * An operand with no example yields a result with no example: attacker input and server state read as a bare
 * identifier are example-free, and collapsing that to `false` would invent which arm a real session takes.
 * decide.c reads the absence as REAL_ARM_UNOBSERVED and leaves both arms unmarked.
 *
 * A concrete operand always has a value, including `undefined`, hence `hava`/`havb`: JS_UNDEFINED means "no
 * example" only on the concolic side, while `cfg.admin === undefined` compares against a real undefined. */
static JSValue cmp_example(JSContext *ctx, JSValueConst a, JSValueConst b, int ca, int cb,
                           int is_neq, JSConcolicEqOp op) {
    JSValue exa = ca ? concolic_example(ctx, a) : JS_DupValue(ctx, a);
    JSValue exb = cb ? concolic_example(ctx, b) : JS_DupValue(ctx, b);
    int hava = ca ? concolic_example_present(a) : 1;
    int havb = cb ? concolic_example_present(b) : 1;
    JSValue out = JS_UNDEFINED;

    if (hava && havb && example_operable(exa) && example_operable(exb)) {
        int eq;
        DCHECK(!concolic_is(exa) && !concolic_is(exb),
               "an example that is itself a concolic value reached the real comparison — `JS_IsEqual` would "
               "re-enter this very hook, and a value whose example is another unknown is a producer having "
               "attached a symbol where a concrete stands");
        if (op == JS_CONCOLIC_EQ_STRICT) {
            eq = JS_IsStrictEqual(ctx, exa, exb) ? 1 : 0;
        } else if (op == JS_CONCOLIC_EQ_SAME_VALUE_ZERO) {
            eq = JS_IsSameValueZero(ctx, exa, exb) ? 1 : 0;
        } else {
            DCHECK(op == JS_CONCOLIC_EQ_LOOSE,
                   "an equality reached the concolic derivation naming an algorithm quickjs.h does not "
                   "declare — 7.2.13 and 7.2.14 disagree, so an unnamed caller has no answer here");
            eq = JS_IsEqual(ctx, exa, exb);
            /* §7.2.13's only failure is an abrupt ToPrimitive, which the operand test above makes unreachable;
               a -1 means the two disagree about the operands, and the pending throw would surface in the
               page's next statement as a TypeError it never wrote. */
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
    char *tok = NULL, *iu, *io, *subj, *shape;
    ConcolicLit kind = CONCOLIC_LIT_NONE;
    JSValue res;

    if (!ca && !cb) return 0;
    /* Two Numbers whose forms differ by a constant are equal in every world or in none: §7.2.13 step 1 hands two
       Numbers to §7.2.14, which compares them by Number::equal, so both algorithms answer `a - b === 0`. */
    {
        AffOperand oa, ob;
        ConcolicAffine *d = NULL;
        double konst = 0.0;

        if (ca && cb && aff_operand(ctx, a, &oa) && aff_operand(ctx, b, &ob) &&
            aff_lin(&oa, 1.0, &ob, -1.0, &d, &konst) == 0) {
            JSValue ex = cmp_example(ctx, a, b, ca, cb, is_neq, op);
            int holds = (konst == 0.0) ^ (is_neq != 0);

            DCHECKF(JS_IsUndefined(ex) || JS_ToBool(ctx, ex) == holds,
                    "two values whose affine forms differ by the constant %.17g gave the opposite answer to "
                    "`%s` on their examples — one operand's form is not the function its example was computed "
                    "by", konst, is_neq ? "!=" : "==");
            JS_FreeValue(ctx, ex);
            JS_FreeValue(ctx, a); JS_FreeValue(ctx, b);
            sp[-2] = JS_NewBool(ctx, holds);
            return 1;
        }
        aff_free(d);
    }
    opq = ca ? a : b; other = ca ? b : a;
    /* A Boolean in every world compared with a Boolean: §7.2.14 step 2 and §7.2.13 step 1 both reach
       SameValueNonNumber, true exactly when the two are the same Boolean, so `b === true` is ToBoolean(b) and
       `b === false` is its complement. Minted as that predicate, the two comparisons branch on one constraint
       entry and no flow takes both false arms. SameValueZero agrees for Booleans. */
    if (!concolic_is(other) && JS_IsBool(other) && cty_of_operand(opq) == CTY_BOOLEAN) {
        int negate = (!JS_ToBool(ctx, other)) ^ (is_neq != 0);
        JSValue res2 = concolic_tobool_hook(ctx, opq, negate);

        DCHECK(!JS_IsUninitialized(res2), "a Boolean-typed unknown was refused by ToBoolean's own mint");
        JS_FreeValue(ctx, a); JS_FreeValue(ctx, b);
        sp[-2] = res2;
        return 1;
    }
    src = concolic_src_c(opq);
    root = concolic_root_c(opq);   /* the same operand, or concolic_alloc's assert sees two facts about two values */
    /* The pin token is the other operand spelled, through literal_tok's classification, which also returns the
       kind: §7.1.19 ToString ( arg ) maps `undefined`, `null`, `0` and `false` onto text that is also a legal
       String operand, so the token must not travel without it. An Object or Symbol has no token, and that is
       correct: §7.2.14 step 1 makes no substituted string === an object, and §7.2.13 step 12 would run the page's
       own ToPrimitive. The predicate still forks over both identities; only the pin is absent. */
    if (!concolic_is(other)) tok = literal_tok(ctx, other, &kind);
    /* Equality is symmetric, so `x === 'a'` and `'a' === x` compose one identity: the unknown operand is
       written first, and where both are unknown the two identities are ordered between themselves. */
    iu = ident_of_operand(ctx, opq);
    io = ident_of_operand(ctx, other);
    /* The two spellings of one operand agree in the direction they must: a token with no identity would pin a
       source to a value no predicate key mentions. It is an implication, not a biconditional: an identity
       with no token is legitimate where the operand is named without a spelled value (an intrinsic, a
       registered symbol, a creation name), as in `n !== Object.prototype`, which names its predicate and pins
       nothing (§7.2.14 step 1). */
    DCHECK(concolic_is(other) ? tok == NULL : (tok == NULL || io != NULL),
           "an equality's other operand was spelled as a pin token with NO predicate identity beside it — the "
           "two come from one classification, so a token the identity speller cannot name is a pin filed under "
           "a key no branch over that source will ever look up");
    if (ca && cb && iu && io && strcmp(iu, io) > 0) { char *t = iu; iu = io; io = t; }
    /* The hole the report prints for this operand, taken from the shape rather than `src`, which for a derived
       value is the braced shape the emission never spells. Minted only with a token: they are one observation. */
    subj = tok ? concolic_hole_key(concolic_shape_c(opq)) : NULL;
    /* The algorithm in the identity is the one the page wrote: `x == '1'` and `x === '1'` are different
       predicates that §7.2.13 and §7.2.14 answer differently, and one key would let a flow that decided either
       decide the other. The algorithm also travels with the arm, because `cmp_op` only says which arm makes the
       equality hold; whether that arm determined the operand is §7.2.13 IsLooselyEqual ( x, y ) versus §7.2.14
       IsStrictlyEqual ( x, y ) (a held `x == undefined` leaves undefined or null). See concolic.h's
       concolic_pin. The equality is minted here and an inequality is its negation, below. */
    {
        char *sa = derived_operand_shape(ctx, a), *sb = derived_operand_shape(ctx, b);
        /* An operator is written between its operands; SameValueZero is an abstract operation, so a call. */
        shape = op == JS_CONCOLIC_EQ_SAME_VALUE_ZERO ? shapef("SameValueZero(%s, %s)", sa, sb)
                                                     : shapef("%s%s%s", sa, cmp_op_ident(op), sb);
        free(sa); free(sb);
    }
    res = pred_new(ctx, cmp_op_ident(op), src, root, root_whose_of(opq), iu, io,
                   tok ? OPCMP_EQ : OPCMP_NONE,
                   tok ? (int)op : CMP_ALGO_NONE, kind, tok, subj, shape);
    /* Which value the predicate is about, by its own identity: read off `opq` (not `iu`, which is swapped when
       both operands are unknown), under the same condition as the hole. An operand this engine cannot spell
       gets none; the predicate still forks, pins and excludes, and only the per-flow degrade has nowhere to be
       filed, as decide.c answers for a condition with no identity. */
    if (subj && concolic_ident_c(opq)) pred_set_subject_ident(res, concolic_ident_c(opq));
    /* …and whether the operand is the value its `src` names, or a derivation that carries its operand's: the
       pin is filed under `src` only in the first case (concolic_pin). */
    if (tok) pred_set_src_own(res, ((const Concolic *)JS_GetOpaque(opq, g_concolic_class))->src_own == 1);
    /* …and, for a typeof result, the operand whose type facet the arm narrows. */
    if (tok) {
        const Concolic *oc = JS_GetOpaque(opq, g_concolic_class);
        Concolic *rc = JS_GetOpaque(res, g_concolic_class);

        if (rc) rc->cmp_typeof = oc->is_typeof;
        pred_set_subject_types(res, oc->is_typeof ? oc->typeof_types : tyf_of_cty(oc->ty));
        if (oc->typeof_of && rc) {
            rc->cmp_typeof_of = strdup(oc->typeof_of);
            CHECK(rc->cmp_typeof_of, "concolic: OOM naming a typeof equality's operand");
        }
    }
    /* …and the result's own example, run rather than derived, attached after the mint (as page_visibility.c
       does) because pred_new's parameters are the predicate and the example is what this run computed it to
       be. */
    concolic_set_example(ctx, res, cmp_example(ctx, a, b, ca, cb, 0, op));
    free(subj);
    free(tok);
    JS_FreeValue(ctx, a); JS_FreeValue(ctx, b);
    /* ECMAScript §13.11.1 Runtime Semantics: Evaluation answers `!=` and `!==` by running the equality and
       returning its complement, so an inequality is the negation of the equality's value: the same predicate
       with the polarity on the arm (concolic_tobool_hook), and one constraint entry for `a !== b` and
       `a === b`. */
    if (is_neq) {
        JSValue neg = concolic_tobool_hook(ctx, res, 1);

        DCHECK(concolic_is(neg), "an equality's own predicate was refused by the negation mint");
        JS_FreeValue(ctx, res);
        res = neg;
    }
    sp[-2] = res;
    return 1;
}
static JSValue concolic_call_hook(JSContext *ctx, JSValueConst func_obj, JSValueConst this_val,
                                  int argc, JSValueConst *argv, JSValue example);

/* The page's `s.<name>(...argv)` over the unknown string `s`, minted as concolic_exotic_get and
   concolic_call_hook mint it, with `ex` (consumed) as its example. */
static JSValue concolic_string_method(JSContext *ctx, JSValueConst s, const char *name, int argc,
                                      JSValueConst *argv, JSValue ex)
{
    JSAtom atom = JS_NewAtom(ctx, name);
    JSValue member, r;

    CHECK(atom != JS_ATOM_NULL, "concolic: OOM interning a String method's name");
    member = concolic_exotic_get(ctx, s, atom, s);
    JS_FreeAtom(ctx, atom);
    DCHECK(concolic_is(member), "the member read under a minted String method answered a concrete value, so the "
                                "string it was read off is not the unknown its own example state says it is");
    r = concolic_call_hook(ctx, member, s, argc, argv, ex);
    JS_FreeValue(ctx, member);
    return r;
}

/* `s`'s example as C bytes when it holds a String one (*pdet: the flow determined it), else NULL. */
static const char *string_example_c(JSContext *ctx, JSValueConst s, size_t *plen, int *pdet)
{
    const char *r = NULL;

    *pdet = 0;
    if (concolic_example_present(s)) {
        JSValue e = concolic_example(ctx, s);

        if (JS_IsString(e)) {
            r = JS_ToCStringLen(ctx, plen, e);
            CHECK(r != NULL, "concolic: OOM reading a String example");
            *pdet = concolic_example_state(s) == CONCOLIC_EX_DETERMINED;
        }
        JS_FreeValue(ctx, e);
    }
    return r;
}

JSValue concolic_starts_with(JSContext *ctx, JSValueConst s, JSValueConst prefix)
{
    JSValue ex = JS_UNDEFINED;
    size_t el = 0, pl = 0;
    int det;
    const char *es, *ps;

    DCHECK(concolic_is(s) && JS_IsString(prefix), "a startsWith was minted over an operand that is not unknown, or "
                                                  "with a prefix that is not a String");
    es = string_example_c(ctx, s, &el, &det);
    if (es) {
        ps = JS_ToCStringLen(ctx, &pl, prefix);
        CHECK(ps != NULL, "concolic: OOM reading a startsWith's prefix");
        ex = JS_NewBool(ctx, pl <= el && !memcmp(es, ps, pl));
        JS_FreeCString(ctx, es); JS_FreeCString(ctx, ps);
        if (det) return ex;                                  /* the flow proved the bytes: the answer is theirs */
    }
    return concolic_string_method(ctx, s, "startsWith", 1, &prefix, ex);
}

JSValue concolic_split(JSContext *ctx, JSValueConst s, char sep)
{
    char sepstr[2] = { sep, 0 };
    JSValue sv = JS_NewString(ctx, sepstr), ex = JS_UNDEFINED, r;
    size_t el = 0, i, from = 0;
    uint32_t n = 0;
    int det;
    const char *es;

    DCHECK(concolic_is(s), "a split was minted over an operand that is not unknown");
    DCHECK((unsigned char)sep < 0x80, "a minted split's separator is not one UTF-8 byte");
    es = string_example_c(ctx, s, &el, &det);
    if (es) {
        ex = JS_NewArray(ctx);
        CHECK(!JS_IsException(ex), "concolic: OOM building a split's example");
        for (i = 0; i <= el; i++)
            if (i == el || es[i] == sep) {
                JS_SetPropertyUint32(ctx, ex, n++, JS_NewStringLen(ctx, es + from, i - from));
                from = i + 1;
            }
        JS_FreeCString(ctx, es);
        if (det) { JS_FreeValue(ctx, sv); return ex; }      /* the flow proved the bytes: the answer is theirs */
    }
    r = concolic_string_method(ctx, s, "split", 1, (JSValueConst *)&sv, ex);
    JS_FreeValue(ctx, sv);
    return r;
}

JSValue concolic_strict_equal(JSContext *ctx, JSValueConst a, JSValueConst b) {
    JSValue sp[2];
    JSValue r;

    sp[0] = JS_DupValue(ctx, a);
    sp[1] = JS_DupValue(ctx, b);
    if (concolic_cmp_hook(ctx, sp + 2, 0, JS_CONCOLIC_EQ_STRICT))
        return sp[0];
    r = JS_NewBool(ctx, JS_IsStrictEqual(ctx, sp[0], sp[1]));
    JS_FreeValue(ctx, sp[0]);
    JS_FreeValue(ctx, sp[1]);
    return r;
}

/* Calling an unknown yields an unknown. `document.cookie.indexOf("role=admin")` reads a field off a concolic
   (another concolic) and calls it; a non-callable concolic would throw "not a function" and make every
   statement after the first method call on an unknown string unreachable. The result is concolic because
   nothing here knows what indexOf returns over a cookie jar this engine never saw, so the comparison after it
   forks. The arguments already ran: they are the page's own expressions. `example` is what quickjs.c's
   js_concolic_call_def got by running the callee's example on the operands' examples, or JS_UNDEFINED for
   none; it is consumed. */
static JSValue concolic_call_hook(JSContext *ctx, JSValueConst func_obj, JSValueConst this_val,
                                  int argc, JSValueConst *argv, JSValue example)
{
    Concolic *c = JS_GetOpaque(func_obj, g_concolic_class);
    char *shape, *ident, **owned;
    const char **f;
    JSValue r;
    unsigned callable = 0;
    int i;

    DCHECK(c != NULL, "the concolic call handler ran on something that is not a concolic value");
    if (concolic_is(this_val)) principal_tokens_note(ctx, this_val, argc, argv);
    /* §13.3.6.2 EvaluateCall steps 4-5 over a member read off a parent whose remaining types hold no callable
       under the name: the call throws in every world this flow admits. Named residual. Not covered: a parent
       whose facet admits types on both sides, where the throwing types' arm is not forked (the narrowing below
       takes the completing one). Next diff builds: a throw arm forked beside the completion, as the machine's
       CCALL_OUTCOME forks an example run's throw. Absence shows as: a `catch` behind `x.m()` never reached
       while x's facet still admits a type lacking `m`. */
    if (c->member && c->facet_src) {
        callable = tyf_callable_member(ctx, c->member);
        if (!(concolic_types(c->facet_src) & callable)) {
            JS_FreeValue(ctx, example);
            return JS_ThrowTypeError(ctx, "%s is not a function", c->member);
        }
    }
    /* The arguments are part of the call's identity and therefore of its shape. `x.startsWith("/api")` and
       `x.startsWith("javascript:")` are two gates over one source, and a shape is also the field path an @S
       candidate is injected at and the atom keyname_record buys: `h.slice(1)` and `h.slice(2)` must not share
       `{location.hash}.slice()`, or two distinct unknown keys land in one slot on the page's object. It is also
       the true display: the page wrote the argument. An argument this engine cannot spell (a plain object)
       makes the whole identity absent and renders "?", the one pair this rule cannot separate (see the named
       residual at keyname_record). */
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
    /* The result of calling an unknown is a new injection identity (nothing here knows what the call returned,
       so a candidate substitutes at the call), and not new bytes: whatever carried the callee's bytes in
       carried these. */
    r = concolic_derived(ctx, shape, shape, c->root, c->root_whose, ident, example);
    /* The result's type set where the callee's algorithm fixes it (JSTrampStepDef.result_types: §22.1.3.22
       String.prototype.slice returns a String). The callee's example is the callee in every world only where
       the receiver is one primitive type, since §7.3.3 GetV reads the member off that type's prototype; a
       receiver admitting Objects may hold its own member, so its call's result stays untyped. */
    if (concolic_is(r) && concolic_is(this_val)) {
        unsigned recv = cty_of_operand(this_val);

        if (recv == CTY_STRING || recv == CTY_NUMBER || recv == CTY_BOOLEAN || recv == CTY_SYMBOL ||
            recv == CTY_BIGINT) {
            JSValue fex = concolic_example(ctx, func_obj);
            unsigned rt = JS_FunctionResultTypes(fex);

            JS_FreeValue(ctx, fex);
            if (rt) concolic_type_state(r, cty_of_tyf(rt));
        }
    }
    /* The call completed, so the member read under it found a callable (§13.3.6.2 EvaluateCall steps 4-5),
       which narrows the parent the member was read off. */
    if (c->member && c->facet_src) type_facet_narrow(c->facet_src, callable);
    /* …and the predicate the page just wrote, if this result can name one. `if (path.startsWith("/api"))`
       gates `path` as `path === "/api"` does, but determines no value, so it pins nothing and carries no bound.
       (A `!`-spelled gate reaches no branch here; concolic.h's residual beside concolic_strpred names why.)
       Each absence below is a positive statement: no `member` means the page called the unknown itself
       (`x("/api")`); a receiver that is not an unknown, or is shaped `{}`, has no hole to print; and an
       unspellable argument makes the whole predicate absent, since a claim missing an operand is a different
       claim. Nothing here interprets the method: what travels is the name, the arguments and the arm the run
       took, a predicate that was evaluated rather than a rule for computing a value.
       A candidate re-fire is tested first (`concolic_is(r)`): concolic_derived hands back the attacker's
       payload, a real String with no record to stamp, and a verify run builds no @H domain (endpoint.c
       suppresses its requests for the same reason). */
    if (concolic_is(r) && c->member && concolic_is(this_val)) {
        char *subj = concolic_hole_key(concolic_shape_c(this_val));
        if (subj) {
            char **spelled = NULL;
            int ok = 1;
            /* Named residual: a regex gate files nothing. Dropping a predicate with an unspellable argument
               is right; what is narrow is the classification. operand_kind answers CONCOLIC_LIT_NONE for every
               Object, but a RegExp has a second name source, the pattern and flags the page wrote.
               Not covered: a call over an unknown receiver whose argument is a RegExp; the branch forks and the
               hole is named, but the observation is discarded, and endpoint.c's missing `predicates` reads as
               "no call predicate survived" (a pinned receiver runs the real builtin, so only unpinned ones lose).
               Next diff: an engine/qjs entry point answering a RegExp's [[OriginalSource]] and [[OriginalFlags]]
               off its internal slots, for operand_kind and literal_tok to spell; quickjs.h exports only
               JS_IsRegExp, and the slots are JSRegExp's private fields. regexp_site_hash reads the same slots
               but folds them into a 64-bit site, and a domain spelling is read by a person against the page's
               bytes. It must not go through a property Get: §22.2.6.17 RegExp.prototype.toString ( ) and
               §22.2.6.4 get RegExp.prototype.flags are Gets a subclass can override, running page code with no
               flow base. How its absence shows: an @H param with a named receiver hole renders `validValues`
               and no `predicates` key while the fork count moves at that branch. */
            if (argc) {
                spelled = reclaim_calloc((size_t)argc, sizeof *spelled);
                CHECK(spelled, "concolic: OOM spelling the arguments a gate passed over an unknown");
                for (i = 0; i < argc; i++) {
                    spelled[i] = literal_tok(ctx, argv[i], NULL);
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

/* The class's [[Call]] entry, which only a C caller invoking an unknown off the trampoline reaches: the
   interpreter's dispatch runs the call as quickjs.c's js_concolic_call_def, whose run of the callee's example
   is a request this entry has no flow to park. The release arm answers the unknown without that example. */
static JSValue concolic_call_c_entry(JSContext *ctx, JSValueConst func_obj, JSValueConst this_val,
                                     int argc, JSValueConst *argv, int flags)
{
    DCHECK(!(flags & JS_CALL_FLAG_CONSTRUCTOR),
           "a concolic value was called as a constructor — this class never sets the constructor bit, so "
           "`this_val` here is the receiver, and a new.target in that slot would be filed as one");
    DFAIL("an unknown was called from C rather than through the interpreter's dispatch — its [[Call]] is "
          "js_concolic_call_def, reached through tramp_step_def_of; route this caller onto step_call_run");
    return concolic_call_hook(ctx, func_obj, this_val, argc, argv, JS_UNDEFINED);
}

/* Ordering over an unknown is unknown. < <= > >= coerce with ToPrimitive, which a concolic cannot satisfy, so
   without this `document.cookie.indexOf("role=admin") >= 0` would throw and explore neither arm. The result is
   a concolic bool that forks like an equality gate; it pins nothing, because an ordering narrows a domain.
   Its identity is the operator and both operands in the order written: `x < 700`, `x > 700` and `700 < x` are
   different predicates, and `parseInt(gCS(a).width) < 700` must not decide `parseInt(gCS(b).width) < 300`.
   It also states its bound, so a parameter gated by `> 5` is reported as bounded. It invents no value: what
   is recorded is the relation, the literal the page wrote and the arm taken; the emission stays a
   domain-annotated shape. */
int concolic_rel_hook(JSContext *ctx, JSValue *sp, int op) {
    JSValue a = sp[-2], b = sp[-1];
    int ca = concolic_is(a), cb = concolic_is(b);
    char opid[24];
    RelOp rel;
    int w;

    if (!ca && !cb) return 0;
    /* The operator arrives as the engine's own opcode (see the hook's contract in quickjs.h). The identity keeps
       the raw opcode, since the key only has to tell the four apart; the name comes from solver/rel_op.h,
       which reads the engine's opcode table. Asked on every ordering, not only beside the branch that uses it,
       because rel_op_of_opcode is what catches the host's copy of the opcode table drifting from quickjs.c's,
       and an assert under the numeric-operand test would fire only on bundles comparing against a literal. */
    rel = rel_op_of_opcode(op);
    w = snprintf(opid, sizeof opid, "rel%d", op);
    DCHECK(w > 0 && (size_t)w < sizeof opid, "a relational operator's id did not fit its own buffer");
    (void)w;
    /* Two Numbers whose forms differ by a constant are ordered the same way in every world: §7.2.12 IsLessThan
       compares two Numbers by Number::lessThan, so the relation is the constant's against zero. */
    {
        AffOperand oa, ob;
        ConcolicAffine *d = NULL;
        double konst = 0.0;

        if (ca && cb && aff_operand(ctx, a, &oa) && aff_operand(ctx, b, &ob) &&
            aff_lin(&oa, 1.0, &ob, -1.0, &d, &konst) == 0) {
            int holds = rel == REL_LT ? konst < 0.0 : rel == REL_LE ? konst <= 0.0
                      : rel == REL_GT ? konst > 0.0 : konst >= 0.0;
#if APICLIENT_DEV
            JSValue exa = concolic_example(ctx, a), exb = concolic_example(ctx, b);
            double da = 0.0, db = 0.0;

            if (!JS_IsUndefined(exa) && !JS_IsUndefined(exb) && example_operable(exa) && example_operable(exb) &&
                JS_ToFloat64(ctx, &da, exa) == 0 && JS_ToFloat64(ctx, &db, exb) == 0)
                DCHECKF(da - db == konst,
                        "two values whose affine forms differ by the constant %.17g have examples %.17g and "
                        "%.17g — one operand's form is not the function its example was computed by", konst,
                        da, db);
            JS_FreeValue(ctx, exa); JS_FreeValue(ctx, exb);
#endif
            JS_FreeValue(ctx, a); JS_FreeValue(ctx, b);
            sp[-2] = JS_NewBool(ctx, holds);
            return 1;
        }
        aff_free(d);
    }
    {
        JSValueConst opq = ca ? a : b, other = ca ? b : a;
        char *sa = derived_operand_shape(ctx, a), *sb = derived_operand_shape(ctx, b);
        char *shape = shapef("%s%s%s", sa, rel_op_spelling(rel), sb);
        JSValue res;

        free(sa); free(sb);
        res = pred_new(ctx, opid, concolic_src_c(opq), concolic_root_c(opq), root_whose_of(opq),
                       ident_of_operand(ctx, a), ident_of_operand(ctx, b),
                       OPCMP_NONE, CMP_ALGO_NONE, CONCOLIC_LIT_NONE, NULL, NULL, shape);
        /* A bound exists only where the relation's meaning is determined, which §7.2.12 IsLessThan ( x, y,
           leftFirst ) decides: it compares as Strings only when both primitives are Strings, so a Number on
           the concrete side fixes the numeric path, while a String leaves the page performing a lexicographic
           or numeric comparison depending on the unknown. Stating a bound there would name which comparison
           the page ran, so it is absent. Non-finite likewise: `x < Infinity` narrows nothing, `x < NaN` is
           false for every x, and neither is a JSON number a report could carry. The predicate still forks;
           only the bound is absent. */
        /* The example: the real §13.10.1 operator on the operands' own examples, as cmp_example runs §7.2.13 or
           §7.2.14 for an equality, so `screen.width > 768` over a modelled width answers the arm this session
           takes. An operand with no bytes, or one that is an object or a Symbol, leaves the relation without one. */
        {
            JSValue exa = ca ? concolic_example(ctx, a) : JS_DupValue(ctx, a);
            JSValue exb = cb ? concolic_example(ctx, b) : JS_DupValue(ctx, b);

            if ((!ca || concolic_example_present(a)) && (!cb || concolic_example_present(b)) &&
                example_operable(exa) && example_operable(exb)) {
                int holds = JS_IsRelated(ctx, exa, exb, op);

                /* §7.2.12's only failure over two non-object, non-Symbol primitives is none: ToNumeric of a
                   String, a Boolean, undefined or null cannot throw, and a BigInt against a String compares or
                   answers false. */
                CHECK(holds >= 0, "concolic relation: §7.2.12 IsLessThan refused two primitives that carry no "
                                  "object and no Symbol");
                concolic_set_example(ctx, res, JS_NewBool(ctx, holds));
            }
            JS_FreeValue(ctx, exa);
            JS_FreeValue(ctx, exb);
        }
        if (!concolic_is(other) && concolic_ident_c(opq))
            pred_set_order_subject(res, concolic_ident_c(opq));
        if (!concolic_is(other) && JS_IsNumber(other)) {
            double num = 0;
            int gotn = JS_ToFloat64(ctx, &num, other);
            DCHECK(gotn == 0, "a Number operand did not convert to a double — §7.1.4 ToNumber over a value "
                              "JS_IsNumber has already answered for cannot fail, so this is the tag test and "
                              "the conversion disagreeing about what the operand is");
            if (gotn == 0 && isfinite(num)) {
                char *subj = concolic_hole_key(concolic_shape_c(opq));
                char *txt = subj ? literal_tok(ctx, other, NULL) : NULL;
                /* Normalised subject-left exactly once: `700 < x` is `x > 700`, so no consumer has to re-derive
                   which operand the page wrote first. */
                if (txt) pred_set_bound(res, ca ? rel : rel_op_mirror(rel), txt, num, subj);
                if (txt && ((const Concolic *)JS_GetOpaque(opq, g_concolic_class))->src_own == 1 &&
                    concolic_src_c(opq)) {
                    Concolic *rc = JS_GetOpaque(res, g_concolic_class);

                    rc->facet_src = strdup(concolic_src_c(opq));
                    CHECK(rc->facet_src, "concolic: OOM naming an ordering's subject");
                }
                free(txt);
                free(subj);
            }
        }
        JS_FreeValue(ctx, a); JS_FreeValue(ctx, b);
        sp[-2] = res;
    }
    return 1;
}

static char *cstr_dup(JSContext *ctx, JSValueConst v);   /* defined with the + hook below */

/* §7.1.4 ToNumber ( arg ) and §7.1.19 ToString ( arg ) over unknown input, answered where the operator
 * computes, never at the conversion boundary, which owes C a real primitive. The result keeps the source, so
 * `-location.hash` still forks a later branch and solves at a later sink: opacity survives coercion. The
 * example propagates by running the real op on the operands' examples; when an operand has none, the result
 * has none. */
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

/* Which of the two real operations this operator is. Most of §6.1.6.1 The Number Type's methods compute over
   the Number; seven narrow their operands to 32 bits first: §6.1.6.1.2 Number::bitwiseNOT ( number ),
   §6.1.6.1.9 Number::leftShift ( x, y ), §6.1.6.1.10 Number::signedRightShift ( x, y ), §6.1.6.1.11
   Number::unsignedRightShift ( x, y ) and §6.1.6.1.16 NumberBitwiseOp ( op, x, y ) for &, | and ^. A double is
   the wrong carrier there: §7.1.8 ToInt32 ( arg ) is defined for every Number including the infinities, while
   a C cast outside the int32 range is undefined behaviour. So the engine's own §7.1.8 ToInt32 ( arg ) and
   §7.1.9 ToUint32 ( arg ) run on the example and the result is an exact integer JSValue. */
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

/* The seven 32-bit operations, run on the operands' examples by the engine's own conversions. `exa`/`exb` are
   borrowed; `*out` is a new owned JSValue on success. Returns 0 when a conversion threw: an example that cannot
   convert produces no example. */
static int carith_apply32(JSContext *ctx, int op, JSValueConst exa, JSValueConst exb, JSValue *out)
{
    int32_t ia = 0, ib = 0;
    uint32_t ua = 0, ub = 0, sc;

    DCHECK(carith_is_32bit(op), "carith_apply32 reached with an operator §6.1.6.1 computes over the Number");
    DCHECK(op != JS_CARITH_NOT || JS_IsUndefined(exb),
           "§6.1.6.1.2 Number::bitwiseNOT takes ONE argument — a second operand here means the interpreter "
           "pushed a binary window for a unary operator");

    if (op == JS_CARITH_SHR) {
        /* §6.1.6.1.11: "Let leftNumber be ! ToUint32(x)." The one operator whose left side is unsigned, hence its own
           id. */
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
        /* §6.1.6.1.9/.10: ToInt32 on the left, ToUint32 on the right, shiftCount modulo 32. */
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

/* The affine form of `a op b` (aff_lin's three answers; -1 also for an operator that has none). §6.1.6.1's
   unary plus, increment and decrement keep the operand's form; a difference combines two; a product keeps one
   only when the other operand is a positive integer, since a product by 0 is ±0 by the sign of the unknown and
   one by a negative number turns +0 into -0. */
static int aff_arith(JSContext *ctx, int op, int nops, JSValueConst a, JSValueConst b, ConcolicAffine **out,
                     double *konst)
{
    AffOperand oa, ob, one = { NULL, 1.0 };

    *out = NULL;
    *konst = 0.0;
    if (!aff_operand(ctx, a, &oa) || (nops == 2 && !aff_operand(ctx, b, &ob))) return -1;
    switch (op) {
    case JS_CARITH_PLUS: return aff_lin(&oa, 1.0, &one, 0.0, out, konst);
    case JS_CARITH_INC:  return aff_lin(&oa, 1.0, &one, 1.0, out, konst);
    case JS_CARITH_DEC:  return aff_lin(&oa, 1.0, &one, -1.0, out, konst);
    case JS_CARITH_SUB:  return aff_lin(&oa, 1.0, &ob, -1.0, out, konst);
    case JS_CARITH_MUL:
        if (!oa.form && ob.form && oa.c > 0.0) return aff_lin(&ob, oa.c, &one, 0.0, out, konst);
        if (oa.form && !ob.form && ob.c > 0.0) return aff_lin(&oa, ob.c, &one, 0.0, out, konst);
        return -1;
    default: return -1;
    }
}

/* The types an arithmetic result takes. Each operator applies ToNumeric to its operands and then a Number or a
   BigInt operation, throwing a TypeError where the two numeric types differ (§13.15.3 step 4 for the binary
   ones), so a binary result is a Number where either operand cannot be a BigInt. Unary plus is ToNumber, which
   throws for a BigInt (§13.5.4.1), and §6.1.6.2.11 BigInt::unsignedRightShift always throws, so those two
   answer a Number. An operand that is unknown-typed, or an Object whose ToPrimitive is the page's, may be
   either. */
static unsigned carith_result_types(int op, int nops, JSValueConst a, JSValueConst b)
{
    unsigned ta = cty_of_operand(a), tb = nops == 2 ? cty_of_operand(b) : 0;
    int a_bigint = !ta || (ta & (CTY_BIGINT | CTY_OBJECT));
    int b_bigint = nops == 1 || !tb || (tb & (CTY_BIGINT | CTY_OBJECT));

    if (op == JS_CARITH_PLUS || op == JS_CARITH_SHR) return CTY_NUMBER;
    return (a_bigint && b_bigint) ? CTY_NUMERIC : CTY_NUMBER;
}

int concolic_arith_hook(JSContext *ctx, JSValue *sp, int op, int nops) {
    JSValue a = sp[-nops], b = nops == 2 ? sp[-1] : JS_UNDEFINED;
    int ca = concolic_is(a), cb = nops == 2 && concolic_is(b);
    const char *name;
    int unary = 0;
    char *shape, *ident, *root;
    const char *src;
    JSValueConst ops[2];
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
    /* The set of the operands' roots (derived_root_join). `src` stays the first unknown operand's, a different
       fact: a candidate is injected at one source read. */
    ops[0] = a;
    ops[1] = b;   /* JS_UNDEFINED for a unary operator, which carries no root and is skipped by the walk */
    root = derived_root_join(ops, nops);

    /* The operator and its operands are the identity, and so is the arity: `carith_name` spells negation and
       subtraction both `-`, and a one-member composition never writes a two-member one's bytes. */
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
        /* derived_operand_shape, the one speller, rather than a hand-rolled JS_ToCString: that would run the
           page's own valueOf from C for `x * obj` (§7.1.19 ToString ( arg ) sends an Object to ToPrimitive),
           and it would render `x * 2` and `x * "2"` alike. An operand this engine refuses to spell renders `?`. */
        char *sa = derived_operand_shape(ctx, a);
        char *sb = derived_operand_shape(ctx, b);
        shape = shapef("%s%s%s", sa, name, sb);
        free(sa); free(sb);
        f[0] = ia; f[1] = ib;
        ident = concolic_ident_compose(name, f, 2);
        free(ia); free(ib);
    }

    /* Run the real op on the examples when there are any: the concrete half of the triple. */
    {
        JSValue exa = ca ? concolic_example(ctx, a) : JS_DupValue(ctx, a);
        JSValue exb = nops == 2 ? (cb ? concolic_example(ctx, b) : JS_DupValue(ctx, b)) : JS_UNDEFINED;

        /* Every conversion below is §7.1.4 ToNumber ( arg ), whose object case runs ToPrimitive and so the page's
           valueOf from this C frame. A concrete Object operand is the operator's to convert on its ToPrimitive
           trampoline before this hook; a concolic one's structured example gives the result none
           (example_operable). */
        DCHECK((ca || !JS_IsObject(exa)) && (cb || nops == 1 || !JS_IsObject(exb)),
               "a CONCRETE Object operand reached the arithmetic hook unconverted — §7.1.4 ToNumber over it runs "
               "the page's valueOf, which belongs on the operator's ToPrimitive trampoline before the hook");

        if ((!ca || concolic_example_present(a)) && (nops == 1 || !cb || concolic_example_present(b)) &&
            example_operable(exa) && (nops == 1 || example_operable(exb))) {
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
            /* A conversion that threw left its exception on the context while the operator is about to return
               success, so the page's next statement would throw a TypeError it never wrote. The example is
               dropped and the throw with it: it belongs to a coercion the program did not perform. */
            if (JS_IsUndefined(example) && JS_HasException(ctx))
                JS_FreeValue(ctx, JS_GetException(ctx));
        }
        JS_FreeValue(ctx, exa); JS_FreeValue(ctx, exb);
    }

    /* The affine facet: a sum, a difference or a product by a positive integer of values with forms has one,
       and one whose terms cancel is the constant they cancel to in every world. */
    {
        ConcolicAffine *af = NULL;
        double konst = 0.0;
        int got = aff_arith(ctx, op, nops, a, b, &af, &konst);

        if (got == 0) {
            JSValue k = aff_constant(ctx, konst, example, name);

            JS_FreeValue(ctx, example);
            free(shape); free(root); free(ident);
            JS_FreeValue(ctx, sp[-nops]);
            if (nops == 2) JS_FreeValue(ctx, sp[-1]);
            sp[-nops] = k;
            return 1;
        }
        if (got == 1) { free(ident); ident = aff_ident(af); }
        res = concolic_derived(ctx, shape, src ? src : shape, root ? root : shape,
                               root_whose_or_unstated(derived_root_whose(ops, nops)), ident, example);
        if (af) aff_attach(res, af);
    }
    concolic_type_state(res, carith_result_types(op, nops, a, b));
    free(shape);
    free(root);
    JS_FreeValue(ctx, sp[-nops]);
    if (nops == 2) JS_FreeValue(ctx, sp[-1]);
    sp[-nops] = res;
    return 1;
}

/* Records on `r` that it is the string form of the unknown `u`, so as a property key it is `u`'s key (str_of).
   §7.1.21 ToPropertyKey ( arg ) is ToString(ToPrimitive(arg, string)). §22.1.1.1 String ( value ) and
   §22.1.3.5 String.prototype.concat (a template literal) convert with that same hint, so for them this is exact
   for every value but a Symbol, which concolic_key_name_hook already names with a String like every unknown
   key. `u + ""` passes no hint (§7.1.1's "default"), and is the same string wherever ToPrimitive
   hands `u` back unchanged, which JS_ToPrimitiveFree does for an unknown under every hint; its named residual
   is the world where it does not. A chain records the innermost value, so `String(u + "")` names `u`. An
   unknown with no identity, or an `r` a pin made concrete, records nothing. */
static void str_of_record(JSValueConst r, JSValueConst u)
{
    Concolic *rc = concolic_is(r) ? JS_GetOpaque(r, g_concolic_class) : NULL;
    Concolic *uc = concolic_is(u) ? JS_GetOpaque(u, g_concolic_class) : NULL;
    const char *of, *sh;

    if (!rc || !uc) return;
    of = uc->str_of ? uc->str_of : uc->ident;
    sh = uc->str_of ? uc->str_of_shape : uc->shape;
    if (!of) return;
    DCHECK(rc->str_of == NULL && rc->str_of_shape == NULL,
           "a value was recorded as the string form of a second unknown — str_of is written once, at the mint");
    DCHECK(!uc->str_of || uc->str_of_shape, "a string form recorded an identity without the shape naming its slot");
    rc->str_of = strdup(of);
    rc->str_of_shape = strdup(sh ? sh : "{}");
    CHECK(rc->str_of && rc->str_of_shape, "concolic: OOM recording which value a string form is the string of");
}

/* §7.1.19 ToString ( arg ) over unknown input: unknown, source kept, example computed by stringifying the
   example when there is one. It is the answer for §22.1.1.1 String ( value ) only, whose result is the
   coercion; a builtin that merely consumes the string derives its own result named by its own operation
   (concolic_builtin_hook, reached from the engine's shared ToString sub-sequence, JS_STEP_UNKNOWN). */
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
    /* §7.1.19 over an Object or a Symbol example runs ToPrimitive or throws, so the result has none. */
    if (concolic_example_present(v) && example_operable(ex)) {
        const char *p = JS_ToCString(ctx, ex);
        if (p) { example = JS_NewString(ctx, p); JS_FreeCString(ctx, p); }
    }
    JS_FreeValue(ctx, ex);
    r = concolic_derived(ctx, shape, src ? src : shape, root ? root : shape,
                         root_whose_or_unstated(root_whose_of(v)), ident, example);
    free(shape);
    concolic_type_state(r, CTY_STRING);   /* §22.1.1.1 String ( value ) answers a String */
    str_of_record(r, v);
    return r;
}

/* §7.1.2 ToBoolean ( arg ) over unknown input, and §13.5.7 Logical NOT Operator ( ! ) with it (quickjs.h's
 * hook contract says why the operator answers at all).
 *
 * The result is a new value with an old predicate. A new value, because a program holds and concatenates it,
 * so `"" + !p` and `"" + p` need different identities (`ident`). An old predicate, because `if (!p)` is
 * `if (p)` with the arms swapped (`br_key`/`br_neg`), so a flow that decided `p` decides `!p` instead of
 * forking again. `!!x` composes back: the inner call gives `br_key = ident(x), br_neg = 1`, the outer
 * `br_neg = 0`, so `if (!!x)`, `if (Boolean(x))` and `if (x)` are one constraint entry.
 *
 * The example is the real operation on the operand's; §7.1.2 has no ToPrimitive step, so it re-enters
 * nothing. No example in, none out, which decide_real_arm reads as "nothing observed says which arm is real".
 * Minted through concolic_alloc, not concolic_derived, for pred_new's reason: this is a computed boolean. */
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
    /* The polarity is in the identity, because `!p` and `Boolean(p)` are opposite values, and not in the
       branch key, which is the same predicate for both. */
    f[0] = negate ? "!" : "ToBoolean"; f[1] = concolic_ident_c(v);
    ident = concolic_ident_compose("tb", f, 2);
    ex = concolic_example(ctx, v);
    if (concolic_example_present(v)) {
        int b = JS_ToBool(ctx, ex);
        DCHECK(b == 0 || b == 1,
               "§7.1.2 ToBoolean over a concolic's example answered neither true nor false — it answers -1 "
               "only for an exception value, so a producer has attached a pending throw to a value as its "
               "example");
        example = JS_NewBool(ctx, b ^ negate);
    }
    JS_FreeValue(ctx, ex);
    r = concolic_alloc(ctx, shape, src, root, root_whose_of(v), ident, example);
    free(shape);
    concolic_type_state(r, CTY_BOOLEAN);   /* §7.1.2 and §13.5.7 answer a Boolean */
    rc = JS_GetOpaque(r, g_concolic_class);
    DCHECK(rc != NULL, "a ToBoolean result was minted as something that is not a concolic value");
    /* The branch key and its polarity, written together: the operand's own branch key, so a chain of negations
       keys the innermost predicate. An operand this engine cannot spell leaves both absent, and every branch
       over the result keeps both arms. */
    bkey = concolic_branch_ident_c(v);
    if (bkey) {
        rc->br_key = strdup(bkey);
        CHECK(rc->br_key, "concolic: OOM recording the predicate a negation is the complement of");
        rc->br_neg = (signed char)(concolic_branch_neg(v) ^ negate);
    }
    /* …and the observation the operand carries, verbatim (pred_carry_through_not says why the negation lives in
       the arm). */
    pred_carry_through_not(r, v);
    return r;
}


/* See concolic.h: the value twin of concolic_new_rel, over an ordered operand list. concolic_builtin_hook is its
   n == 1 case. The shape records which operation produced the value, so an @H shape reads as the expression
   the page wrote. It derives from the operands and attaches whatever example the caller hands it, JS_UNDEFINED
   included; it never computes one. */
JSValue concolic_new_derived(JSContext *ctx, const char *op, const JSValueConst *operands, int n,
                             JSValue example)
{
    const char *src, **fields;
    char **parts, *shape, *args = NULL, *ident, *root;
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
    /* An object example is accepted: this function runs no coercion, it only stores the example. A record's
       example is legitimately an object (concolic_new attaches one for server-injected state, and §25.5.2
       JSON.parse ( text [ , reviver ] ) over an unknown text attaches the parsed record), which example_slot,
       concolic_exotic_own_names and concolic_exotic_delete then model. The coercions (concolic_arith_hook, the
       `+` hook, the String hook, cmp_example) give such an operand's result no example (example_operable). */
    for (i = 0; i < n; i++)
        if (concolic_is(operands[i])) { first = i; break; }
    /* No operand is unknown, so the caller already has the answer (as concolic_builtin_hook says for a known
       operand): a derivation over concrete operands would put a fork in the frontier over an answerable
       question. */
    if (first < 0) { JS_FreeValue(ctx, example); return JS_UNINITIALIZED; }
    /* `src` is the first unknown operand's (see concolic.h), concolic_arith_hook's rule. */
    src = concolic_src_c(operands[first]);
    /* `root` is the set of all operands' roots (derived_root_join), a different fact: a candidate is injected
       at one source read. */
    root = derived_root_join(operands, n);

    parts = malloc((size_t)n * sizeof *parts);
    fields = malloc(((size_t)n + 1) * sizeof *fields);
    CHECK(parts != NULL && fields != NULL, "concolic: OOM composing a derivation over several operands");
    for (i = 0; i < n; i++) {
        parts[i] = derived_operand_shape(ctx, operands[i]);
        fields[i] = ident_of_operand(ctx, operands[i]);   /* owned; NULL is an unspellable operand */
    }
    /* The operation is the last field and the operands come first, so concolic_builtin_hook's n == 1 call
       composes `(ident, op)` and the two entries share one namespace. */
    fields[n] = op;
    ident = concolic_ident_compose("b", fields, n + 1);
    /* The shape: one operand renders as a method on it; two or more render as the operation applied to them,
       since no one operand is the subject. */
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
    /* `example` is what the caller got by running the real operation on the operands' own examples. */
    r = concolic_derived(ctx, shape, src ? src : shape, root ? root : shape,
                         root_whose_or_unstated(derived_root_whose(operands, n)), ident, example);
    free(root);
    for (i = 0; i < n; i++) { free(parts[i]); free((char *)fields[i]); }
    free(parts);
    free((void *)fields);
    free(args);
    free(shape);
    return r;
}

/* The claimant of keyed rows and its URL §5.2 value serializer (concolic_set_tuple_serializer). */
static const char *g_tuple_component;
static char *(*g_tuple_serialize)(const char *s, size_t n);

void concolic_set_tuple_serializer(const char *component, char *(*serialize)(const char *s, size_t n))
{
    DCHECK(!component == !serialize, "a tuple serializer was registered without its claimant, or the reverse");
    DCHECK(!component || !g_tuple_component, "a second component registered the URL §5.2 tuple serializer");
    g_tuple_component = component;
    g_tuple_serialize = serialize;
}

void concolic_declare_keyed_source(const char *src, const char *parent, const char *key)
{
    int prow = -1, i;

    DCHECK(g_tuple_component && src && parent && key && *key,
           "a keyed row was declared with no registered claimant, or with no identity, parent or key");
    for (i = 0; i < g_srcs_n; i++)
        if (!strcmp(g_srcs[i].src, parent)) { prow = i; break; }
    DCHECK(prow >= 0 && !g_srcs[prow].key, "a keyed row names a parent that is not a declared component");
    if (prow < 0 || concolic_source_declared_by(g_tuple_component, src)) return;
    concolic_declare_source(g_tuple_component, src, "", g_srcs[prow].prefix, g_srcs[prow].deliver);
    g_srcs[g_srcs_n - 1].key = strdup(key);
    g_srcs[g_srcs_n - 1].parent = strdup(parent);
    g_srcs[g_srcs_n - 1].serialize = g_tuple_serialize;
    CHECK(g_srcs[g_srcs_n - 1].key && g_srcs[g_srcs_n - 1].parent, "concolic: OOM declaring a tuple's key");
}

void concolic_set_markup_composer(const char *component, char *(*compose)(const char *path, const char *href))
{
    DCHECK(!component == !compose, "a markup composer was registered without its claimant, or the reverse");
    DCHECK(!component || !g_markup_component, "a second component registered the named-access markup composer");
    g_markup_component = component;
    g_markup_compose = compose;
}

void concolic_declare_named_source(const char *src, const char *parent, const char *path)
{
    int prow;

    DCHECK(g_markup_component && src && parent && path && *path,
           "a named-access row was declared with no registered claimant, or with no identity, parent or path");
    prow = src_row(parent, strlen(parent));
    DCHECK(prow >= 0 && !g_srcs[prow].path, "a named-access row names a parent that is not a markup row");
    if (prow < 0 || concolic_source_declared_by(g_markup_component, src)) return;
    concolic_declare_source(g_markup_component, src, "", g_srcs[prow].prefix, g_srcs[prow].deliver);
    g_srcs[g_srcs_n - 1].parent = strdup(parent);
    g_srcs[g_srcs_n - 1].path = strdup(path);
    CHECK(g_srcs[g_srcs_n - 1].parent && g_srcs[g_srcs_n - 1].path, "concolic: OOM declaring a named-access row");
}

/* A page-supplied name as one segment of a row's identity: bytes outside [A-Za-z0-9_-] percent-encoded, so no
   name can spell the joint separator or a brace. Owned. */
static char *row_segment(const char *name)
{
    static const char HEX[] = "0123456789ABCDEF";
    size_t n = strlen(name), o = 0, i;
    char *out = malloc(n * 3 + 1);

    CHECK(out != NULL, "concolic: OOM spelling a row segment");
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)name[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')
            out[o++] = (char)c;
        else { out[o++] = '%'; out[o++] = HEX[c >> 4]; out[o++] = HEX[c & 15]; }
    }
    out[o] = 0;
    return out;
}

char *concolic_source_chain(const char *root)
{
    int row = root ? root_declared_row(root) : -1;
    char *out = NULL;
    size_t len = 0;

    while (row >= 0 && g_srcs[row].parent) {
        const char *kind = g_srcs[row].key ? "k" : "n", *param = g_srcs[row].key ? g_srcs[row].key : g_srcs[row].path;
        size_t add = strlen(g_srcs[row].src) + strlen(g_srcs[row].parent) + strlen(param) + 6;
        char *grown = realloc(out, len + add + 1);

        CHECK(grown != NULL, "concolic: OOM spelling a row chain");
        out = grown;
        len += (size_t)snprintf(out + len, add + 1, "%s%s\x1f%s\x1f%s\x1f%s", len ? "\x1e" : "", kind,
                                g_srcs[row].src, g_srcs[row].parent, param);
        row = src_row(g_srcs[row].parent, strlen(g_srcs[row].parent));
    }
    return out;
}

void concolic_declare_source_chain(const char *chain)
{
    char *copy = strdup(chain), *rec[64];
    int n = 0, i;

    CHECK(copy != NULL, "concolic: OOM re-declaring a row chain");
    for (char *p = copy; p; ) {
        char *e = strchr(p, '\x1e');
        DCHECKF(n < 64, "a parked row chain holds more than %d rows", 64);
        if (n < 64) rec[n++] = p;
        if (e) *e++ = 0;
        p = e;
    }
    /* Declared base side first: each row's parent must already be one. */
    for (i = n - 1; i >= 0; i--) {
        char *f[4], *p = rec[i];
        int k;

        for (k = 0; k < 4; k++) { f[k] = p; p = p ? strchr(p, '\x1f') : NULL; if (p) *p++ = 0; }
        DCHECK(f[3] && (!strcmp(f[0], "k") || !strcmp(f[0], "n")), "a parked row chain holds a malformed row");
        if (!f[3]) continue;
        if (!strcmp(f[0], "k")) concolic_declare_keyed_source(f[1], f[2], f[3]);
        else concolic_declare_named_source(f[1], f[2], f[3]);
    }
    free(copy);
}

JSValue concolic_new_named_access(JSContext *ctx, JSValueConst markup, JSValueConst name, const char *name_c)
{
    JSValueConst ops[2] = { markup, name };
    const char *root = concolic_root_c(markup);
    int row = root ? root_declared_row(root) : -1;
    char *seg, *src, *ident;
    const char *fields[3];
    JSValue r;
    int i;

    DCHECK(concolic_is(markup) && name_c && *name_c, "a named access over markup was minted with no markup or name");
    if (row < 0 || g_srcs[row].path || !g_markup_component)
        return concolic_new_derived(ctx, "HTML §7.2.2.3 named access", ops, 2, JS_UNDEFINED);
    seg = row_segment(name_c);
    src = shapef("%s.named(%s)", g_srcs[row].src, seg);
    concolic_declare_named_source(src, g_srcs[row].src, name_c);
    for (i = 0; i < 2; i++) fields[i] = ident_of_operand(ctx, ops[i]);
    fields[2] = "HTML §7.2.2.3 named access";
    ident = concolic_ident_compose("b", fields, 3);
    r = concolic_derived(ctx, src, src, src, root_whose_or_unstated(derived_root_whose(ops, 2)), ident,
                         JS_UNDEFINED);
    for (i = 0; i < 2; i++) free((char *)fields[i]);
    free(seg);
    free(src);
    return r;
}


/* See concolic.h. */
JSValue concolic_new_urlencoded_value(JSContext *ctx, const char *op, JSValueConst list, JSValueConst name,
                                      const char *key, JSValue example)
{
    JSValueConst ops[2] = { list, name };
    const char *psrc = concolic_src_c(list), *fields[3];
    int row, i;
    char *src, *ident;
    JSValue r;

    DCHECK(concolic_is(list) && !concolic_is(name) && key != NULL && *key,
           "a tuple's value was minted over a list that is not unknown, a name that is, or no spelled key");
    row = root_declared_row(concolic_root_c(list));
    /* Named residual. Not covered: a list parsed from a derivation of a component (`location.hash.slice(1)`), or
       from a carrier other than the query, whose reproduction this row cannot compose. Next diff builds: the
       row for each verbatim carrier and for a fragment parsed without its `#`. Absence shows as: an @S record
       over such a tuple whose envelope delivers the whole component, and a live verify that never hits. */
    if (row < 0 || !psrc || strcmp(psrc, g_srcs[row].src) != 0 || g_srcs[row].deliver != SRC_DELIVER_ADDRESS ||
        g_srcs[row].prefix != '?')
        return concolic_new_derived(ctx, op, ops, 2, example);
    src = shapef("%s[%s]", concolic_shape_c(list), key);
    concolic_declare_keyed_source(src, psrc, key);
    for (i = 0; i < 2; i++) fields[i] = ident_of_operand(ctx, ops[i]);
    fields[2] = op;
    ident = concolic_ident_compose("b", fields, 3);
    r = concolic_derived(ctx, src, src, src, root_whose_or_unstated(derived_root_whose(ops, 2)), ident, example);
    for (i = 0; i < 2; i++) free((char *)fields[i]);
    free(src);
    return r;
}

/* See concolic.h. */
JSValue concolic_new_derived_list(JSContext *ctx, const char *op, JSValueConst list, JSValue example)
{
    int64_t n = 0, i;
    JSValue *ops, r;

    CHECK(JS_GetLength(ctx, list, &n) == 0, "concolic: an operand list's length could not be read");
    DCHECK(n >= 1 && n <= INT32_MAX, "a derivation's operand list is empty or longer than a call can carry");
    ops = js_malloc(ctx, (size_t)n * sizeof *ops);
    CHECK(ops != NULL, "concolic: OOM unpacking a derivation's operand list");
    for (i = 0; i < n; i++)
        ops[i] = JS_GetPropertyUint32(ctx, list, (uint32_t)i);
    r = concolic_new_derived(ctx, op, (const JSValueConst *)ops, (int)n, example);
    for (i = 0; i < n; i++)
        JS_FreeValue(ctx, ops[i]);
    js_free(ctx, ops);
    return r;
}

/* See concolic.h. */
JSValue concolic_buffer_run(JSContext *ctx, const JSBufferUnknownPiece *u, const uint8_t *window)
{
    JSValueConst ops[3];
    JSValue r, ex = JS_UNDEFINED;
    char op[96];

    if (!u->codec)
        return JS_DupValue(ctx, u->unknown);
    ops[0] = u->unknown;
    ops[1] = JS_NewInt64(ctx, u->image_pos);
    ops[2] = JS_NewInt32(ctx, (int32_t)u->length);
    if (u->length == 1)
        ex = JS_NewInt32(ctx, window[u->offset]);
    snprintf(op, sizeof op, "%s[]", u->codec);
    r = concolic_new_derived(ctx, op, ops, 3, ex);
    DCHECK(!JS_IsUninitialized(r), "a codec image's run named an operand that is not unknown — the engine "
                                   "records an image only over an unknown");
    return r;
}

/* One concrete run of a window, spelled for an identity: its length and FNV-1a 64 digest. */
static JSValue buffer_concrete_run(JSContext *ctx, const uint8_t *p, size_t n)
{
    uint64_t h = 0xcbf29ce484222325ull;
    char buf[64];
    size_t i;

    for (i = 0; i < n; i++) { h ^= p[i]; h *= 0x100000001b3ull; }
    snprintf(buf, sizeof buf, "bytes(%zu):%016llx", n, (unsigned long long)h);
    return JS_NewString(ctx, buf);
}

/* See concolic.h. */
JSValue concolic_buffer_operands(JSContext *ctx, JSValueConst src, const uint8_t *window, size_t len,
                                 bool always, bool *example)
{
    JSBufferUnknownPiece *u = NULL;
    int n = JS_BufferSourceUnknowns(ctx, src, &u), i;
    uint32_t k = 0;
    size_t at = 0;
    JSValue ops;

    *example = true;
    if (n == 0 && !always)
        return JS_UNDEFINED;
    ops = JS_NewArray(ctx);
    CHECK(!JS_IsException(ops), "concolic: OOM listing a buffer source's operands");
    for (i = 0; i <= n; i++) {
        size_t end = i < n ? u[i].offset : len;

        DCHECK(end >= at && end <= len, "a buffer source's unknown runs are out of order or outside the window "
                                        "whose bytes the caller copied");
        if (end > at)
            CHECK(JS_SetPropertyUint32(ctx, ops, k++, buffer_concrete_run(ctx, window + at, end - at)) >= 0,
                  "concolic: OOM listing a buffer source's operands");
        if (i == n)
            break;
        CHECK(JS_SetPropertyUint32(ctx, ops, k++, concolic_buffer_run(ctx, &u[i], window)) >= 0,
              "concolic: OOM listing a buffer source's operands");
        if (!u[i].example)
            *example = false;
        at = (size_t)u[i].offset + u[i].length;
    }
    JS_FreeBufferUnknowns(ctx, u, n);
    if (k == 0)
        CHECK(JS_SetPropertyUint32(ctx, ops, k++, buffer_concrete_run(ctx, window, 0)) >= 0,
              "concolic: OOM listing a buffer source's operands");
    return ops;
}

JSValue concolic_builtin_hook(JSContext *ctx, JSValueConst v, const char *op, JSValue example) {
    if (!concolic_is(v)) { JS_FreeValue(ctx, example); return JS_UNINITIALIZED; }
    /* One operand, through the one speller: a component calling concolic_new_derived with one operand and one
       calling this compose the same key. */
    return concolic_new_derived(ctx, op, &v, 1, example);
}

/* The bytes a DOM member needs from an argument that may be unknown: a selector, an attribute name, a class
   token, an element id. The IDL boundary passes unknown input through as itself, so a bare JS_ToCString would
   crash `document.querySelector(location.hash)`. An unknown name denotes its shape: a real string, stable per
   source, so two lookups through one source agree. Owned either way, so the call site keeps one free path.
   Where the identity is absent the shape is 1:N, so two sources can collide here (keyname_atom, which
   .key_name spends for that population, is not used by this accessor).
   Named residual. Not covered: `el.setAttribute(h.slice({}), "a")` and `el.setAttribute(h.slice({z:1}), "b")`
   compose one shape, so the element carries one attribute. Next diff: route this accessor through
   keyname_atom so a name and a property key from one value are the same bytes; separate because a name here
   is parsed (a selector, a class token), so the discriminating byte must be shown not to change the parse.
   How its absence shows: a document whose attribute name or selector is derived through an Object or Symbol
   operand reports fewer attributes than it set, while the property-key half reports the number it wrote. */
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

/* The names this file has spent an unknown on, which makes the inverse below possible. `concolic_key_name_hook`
   buys an atom with a value's provenance and domain, because §6.1.7 The Object Type says "A property key is
   either a String or a Symbol." That is right where bytes were wanted (a lookup, an atom a builtin consumes,
   a `@WHY` display) and a loss where an enumeration hands the name back to the page: a real string forks
   nothing and reports itself as computed. So the mint records what it spent, and the inverse re-mints it.
   Strings only, no JSValue: a table of live values would be a GC root this component does not own and would
   revert nothing at a context switch. The fields are what `concolic_alloc` takes, so the restore re-mints
   through the one derivation door.
   Not per-flow state and never captured: the map from a value to its name is a pure function of the value, so
   two flows minting one unknown record one entry. The slot the name goes on is an ordinary property write the
   COW delta carries. Agent state, released at concolic_free. */
/* The atom and the shape answer two questions: the atom is the bytes the page holds (what an enumeration hands
   back and the inverse looks up), the shape is the @H provenance a report renders. keyname_atom names the
   atom by the value's identity, since a shape may render two identities alike. `root_whose` is a column because the re-mint cannot recover it: an empty mask beside a real root
   fails concolic_alloc's pair assert, and UNSTATED would deny a provenance this row holds. */
typedef struct { char *atom, *shape, *src, *root, *ident; unsigned root_whose; } KeyName;
static KeyName *g_keynames; static int g_keynames_n, g_keynames_cap;
static int *g_keynames_hash; static int g_keynames_hash_cap;   /* the index (atom -> idx+1) */

/* Keyed on the atom, not the shape: the one caller that looks anything up (concolic_key_value_hook) has only
   the page's bytes, and two values may share a shape while the atom exists so they do not share one. */
static int keyname_find(const char *atom) {
    uint32_t m, h;
    if (!g_keynames_hash) return -1;
    m = (uint32_t)g_keynames_hash_cap - 1; h = cons_hash(atom) & m;
    while (g_keynames_hash[h]) {
        if (!strcmp(g_keynames[g_keynames_hash[h] - 1].atom, atom)) return g_keynames_hash[h] - 1;
        h = (h + 1) & m;
    }
    return -1;
}
static void keyname_hash_put(int idx) {   /* caller guarantees room */
    uint32_t m = (uint32_t)g_keynames_hash_cap - 1, h = cons_hash(g_keynames[idx].atom) & m;
    while (g_keynames_hash[h]) h = (h + 1) & m;
    g_keynames_hash[h] = idx + 1;
}
static void keyname_hash_rebuild(void) {
    int i, nc = 16;
    int *nh;
    /* Sized in a local and published after, for cons_hash_rebuild's reason: the allocation can sell a flow
       (solver/reclaim.h). */
    while (nc < g_keynames_n * 2) nc *= 2;
    nh = reclaim_realloc(g_keynames_hash, (size_t)nc * sizeof(int));
    CHECK(nh, "concolic: OOM indexing the names spent on unknown keys");
    g_keynames_hash = nh; g_keynames_hash_cap = nc;
    memset(g_keynames_hash, 0, (size_t)g_keynames_hash_cap * sizeof(int));
    for (i = 0; i < g_keynames_n; i++) keyname_hash_put(i);
}
/* Absent on both sides is one fact, not a miss: `src` and `root` are NULL together for a value with no
   provenance and `ident` is NULL for one this engine cannot spell, so NULL must equal NULL or the assertion
   below fires on agreement. Compiled only with its one reader: a release DCHECK is `(void)sizeof(cond)`, which
   still needs the name to resolve but emits no call, and -Wunneeded-internal-declaration would report the
   unused static. */
#if APICLIENT_DEV
static int keyname_str_same(const char *a, const char *b) { return a ? (b && !strcmp(a, b)) : !b; }
#endif

static void keyname_record(const char *atom, const char *shape, const char *src, const char *root,
                           unsigned root_whose,
                           const char *ident)
{
    KeyName *e;
    int i = keyname_find(atom);

    if (i >= 0) {
        /* One atom, one value: the claim the slot model rests on (quickjs.h's .key_name: "two writes through the
           same unknown source land in the SAME slot, two different sources in different ones"), asserted where
           a second value can arrive under a name the first holds. An invariant, because the restore is a
           function of the atom: two values under one name would mis-attribute a sink reached through the
           second source to the first, and solve its candidate at an address that never fed it.
           If it fires, the fix is at whichever speller composed the value. The rule every speller owes:
           a shape separates every pair of operands its identity separates, wherever the shape is an
           expression. concolic_add_hook's string arm is not an expression but the string the concatenation
           produced (`/api/{x}`); its other arm is. The expression spellers are concolic_call_hook,
           derived_operand_shape, concolic_key_read_hook, concolic_typeof_hook, concolic_arith_hook and
           concolic_add_hook's numeric arm, all routed through derived_operand_shape and ident_of_operand. */
        /* Named residual: the assert is blind where the identity is absent (an Object or Symbol operand):
           every field it compares agrees, yet the values differ. keyname_atom separates most of that
           population by first-purchase site. Not covered: two unspellable values first purchased at one site,
           a loop writing `o[f(a[i])]`. Next diff: the per-flow ordinal beside the site (keyname_atom's
           residual). How its absence shows: a page writing an unknown key inside a loop loses members off its
           own object (`Object.keys(o).length` short by one per collision) while this assert stays silent. */
#if APICLIENT_DEV
        DCHECKF(keyname_str_same(g_keynames[i].shape, shape) &&
                keyname_str_same(g_keynames[i].src, src) &&
                keyname_str_same(g_keynames[i].root, root) &&
                keyname_str_same(g_keynames[i].ident, ident),
                "two different unknowns bought one property name — fix the speller that composed them, so the "
                "shape separates what the identity separates. atom=[%s] stored: shape=[%s] src=[%s] root=[%s] "
                "ident=[%s]; incoming: shape=[%s] src=[%s] root=[%s] ident=[%s]",
                atom, g_keynames[i].shape, g_keynames[i].src ? g_keynames[i].src : "(null)",
                g_keynames[i].root ? g_keynames[i].root : "(null)",
                g_keynames[i].ident ? g_keynames[i].ident : "(null)", shape, src ? src : "(null)",
                root ? root : "(null)", ident ? ident : "(null)");
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
    e->atom = strdup(atom);
    CHECK(e->atom, "concolic: OOM copying the name spent on an unknown key");
    e->shape = strdup(shape);
    CHECK(e->shape, "concolic: OOM copying an unknown key's display shape");
    e->src   = src   ? strdup(src)   : NULL;
    CHECK(!src   || e->src,   "concolic: OOM copying an unknown key's provenance");
    e->root  = root  ? strdup(root)  : NULL;
    CHECK(!root  || e->root,  "concolic: OOM copying an unknown key's delivery root");
    e->root_whose = root_whose;   /* one fact with the root above — see the column */
    e->ident = ident ? strdup(ident) : NULL;
    CHECK(!ident || e->ident, "concolic: OOM copying an unknown key's identity");
    g_keynames_n++;
    if (g_keynames_hash_cap < g_keynames_n * 2) keyname_hash_rebuild();
    else keyname_hash_put(g_keynames_n - 1);
}

/* The one byte that puts a discriminated atom in its own namespace: US (0x1f), produced by no speller in this
   file, for decide.c's FORK_SITE_PREFIX reason: a shape carries the page's own text, so no printable separator
   is this file's to reserve. A shape already carrying it is refused, not asserted, since a page may write
   \x1f: such a purchase keeps its plain shape atom (an absence, not a wrong name), and if the two populations
   ever meet under one string, keyname_record's assert reports it, because `src`, `root` or `ident` differ. */
#define KEYNAME_SITE_SEP '\x1f'

/* The atom an unknown key is spent on: its shape, the separator, and a 64-bit FNV-1a digest of the identity the
 * key is named by (`ident`: the value's, or for a string form the value it is the string form of). A shape is a
 * display and may render two identities alike (every `Math.random()` call is its own root, `Math.random()#<n>`,
 * under one shape), so the name is the identity's: equal identities share one slot, different ones never do.
 * Where the identity is absent, the shape plus where the page's code stood when the value first bought one
 * (JS_RunningSiteHash, a program fact, so a replay composes the same bytes); with no page frame standing that
 * purchase keeps its shape. Memoised on the value; borrowed from it (or the caller's `sh`), never owned here.
 * Named residual. Not covered: two identity-less values first purchased at one site (`o[f(a[i])]` over
 * unnameable `a[i]`). Next diff: the creating flow's count of prior purchases at this site beside the site,
 * riding the PinBlob like the mint counter but keyed by site. How its absence shows: a document writing an
 * unknown key in a loop reports fewer own property names than it wrote, with one keyname entry. */
static const char *keyname_atom(JSContext *ctx, JSValueConst key, const char *sh, const char *ident)
{
    Concolic *c = g_concolic_class ? JS_GetOpaque(key, g_concolic_class) : NULL;
    uint64_t site, h = 0xcbf29ce484222325ull;
    const unsigned char *p;

    if (!c) return sh;
    if (c->key_atom) return c->key_atom;
    if (strchr(sh, KEYNAME_SITE_SEP)) return sh;      /* the page's own bytes — see the separator */
    if (ident) {
        for (p = (const unsigned char *)ident; *p; p++) { h ^= *p; h *= 0x100000001b3ull; }
        c->key_atom = shapef("%s%c%016llx", sh, KEYNAME_SITE_SEP, (unsigned long long)h);
    } else {
        if (!JS_RunningSiteHash(ctx, &site)) return sh;   /* no page frame: there is no site to name it by */
        c->key_atom = shapef("%s%c%016llx", sh, KEYNAME_SITE_SEP, (unsigned long long)site);
    }
    return c->key_atom;
}

/* The name an unknown key denotes: its shape (or the shape of the value it is the string form of) as a real
   string, stable per source, so every key-taking operation agrees (see the contract at JS_ToPropertyKeyInternal). */
JSValue concolic_key_name_hook(JSContext *ctx, JSValueConst key) {
    const char *sh, *atom, *ident;
    Concolic *kc;

    if (!concolic_is(key)) return JS_UNINITIALIZED;
    kc = JS_GetOpaque(key, g_concolic_class);
    /* The KEY's identity and slot, §7.1.21 ToPropertyKey ( arg ): a string form of `x` (str_of_record) is the
       key `x` is, so `o[x]`, `o[String(x)]` and `o[x + ""]` name one slot under `x`'s identity. */
    sh = kc && kc->str_of ? kc->str_of_shape : concolic_shape_c(key);
    ident = kc && kc->str_of ? kc->str_of : concolic_ident_c(key);
    /* …and the atom is named by that identity (keyname_atom). */
    atom = keyname_atom(ctx, key, sh ? sh : "{}", ident);
    /* What this trade costs is recorded as it is spent, the one place it can be undone. The atom is recorded,
       not the shape, because concolic_key_value_hook inverts by the bytes the page hands back. */
    keyname_record(atom, sh ? sh : "{}", concolic_src_c(key), concolic_root_c(key), root_whose_of(key), ident);
    return JS_NewString(ctx, atom);
}

/* JSConcolicHooks.key_value: the trade above, undone where a property name is handed back to the program as a
   value (that member says why the engine asks). */
JSValue concolic_key_value_hook(JSContext *ctx, JSValueConst name)
{
    const char *s;
    char *ident;
    int i;

    if (!JS_IsString(name)) return JS_UNINITIALIZED;
    s = JS_ToCString(ctx, name);
    /* Checked, not taken for a miss: a NULL is an allocation failure over a value that is a string, and it
       would read exactly like "no unknown was minted under this name", silently de-tainting the key. */
    CHECK(s != NULL,
          "concolic: the bytes of a delivered property name could not be read — a name this file minted for "
          "an unknown key would then answer as an ordinary string and the key's provenance and domain would "
          "be gone with nothing anywhere to say so");
    i = keyname_find(s);
    JS_FreeCString(ctx, s);
    if (i < 0) return JS_UNINITIALIZED;
    ident = g_keynames[i].ident ? strdup(g_keynames[i].ident) : NULL;
    CHECK(!g_keynames[i].ident || ident, "concolic: OOM re-minting an unknown key's identity");
    /* Through concolic_derived, not concolic_alloc, because that is the door every source mint goes through:
       a name restored while a substitution is installed for its source delivers the payload as the original
       read did, and otherwise re-mints the value the read produced. Example-free: which property the attacker
       names is exactly what is not known (concolic_key_read_hook states the same of the value it reads). */
    return concolic_derived(ctx, g_keynames[i].shape, g_keynames[i].src, g_keynames[i].root,
                            g_keynames[i].root_whose, ident, JS_UNDEFINED);
}

/* `obj[x]` with an unknown key. Nothing about x says which slot was meant, so the result is a concolic that
   keeps the key's source: a gate on the value still forks, and a sink still solves for the key that would
   reach it. Example-free on purpose: which property the attacker names is what is not known. */
/* Whether §10.1.8.1 OrdinaryGet ( obj, propertyKey, receiver ) over the primitive `base` misses for every key
   that begins with `prefix`: no own key of its §7.1.20 ToObject wrapper (a String's `length` and indices) and
   no own String key of any link of its realm's intrinsic chain begins so. 0 where it cannot say: a prefix a
   String index could begin, or a link that is not an ordinary object or a primitive's own prototype. */
static int primitive_chain_misses_prefix(JSContext *ctx, JSValueConst base, const char *prefix, size_t plen)
{
    JSClassID id = JS_IsString(base) ? JS_CLASS_STRING : JS_IsNumber(base) ? JS_CLASS_NUMBER
                 : JS_IsBool(base) ? JS_CLASS_BOOLEAN : JS_IsBigInt(base) ? JS_CLASS_BIG_INT
                 : JS_IsSymbol(base) ? JS_CLASS_SYMBOL : 0;
    JSValue o;
    int misses = 1;

    if (!id || plen == 0) return 0;
    if (id == JS_CLASS_STRING && (strncmp("length", prefix, plen < 6 ? plen : 6) == 0 ||
                                  (prefix[0] >= '0' && prefix[0] <= '9')))
        return 0;
    for (o = JS_GetClassProto(ctx, id); misses && JS_IsObject(o);) {
        JSClassID oid = JS_GetClassID(o);
        JSPropertyEnum *tab;
        uint32_t n, i;
        JSValue next;

        if (oid != JS_CLASS_OBJECT && oid != id) { misses = 0; break; }
        CHECK(JS_GetOwnPropertyNames(ctx, &tab, &n, o, JS_GPN_STRING_MASK) == 0,
              "concolic: an ordinary prototype's own keys could not be listed");
        for (i = 0; i < n && misses; i++) {
            const char *name = JS_AtomToCString(ctx, tab[i].atom);

            CHECK(name, "concolic: OOM spelling a prototype's own key");
            misses = strncmp(name, prefix, plen) != 0;
            JS_FreeCString(ctx, name);
        }
        JS_FreePropertyEnum(ctx, tab, n);
        next = JS_GetPrototype(ctx, o);
        CHECK(!JS_IsException(next), "concolic: an intrinsic prototype's [[Prototype]] could not be read");
        JS_FreeValue(ctx, o);
        o = next;
    }
    JS_FreeValue(ctx, o);
    return misses;
}

JSValue concolic_key_read_hook(JSContext *ctx, JSValueConst obj, JSValueConst key) {
    const char *src, *root, *f[2];
    char *shape, *ident, *io, *ik;
    JSValue r;

    if (!concolic_is(key)) return JS_UNINITIALIZED;
    /* A String key whose leading bytes are the page's own (a concatenation's prefix before its first hole)
       names no member of a primitive whose chain holds no key so beginning: the read is the chain's miss. */
    {
        const Concolic *kc = JS_GetOpaque(key, g_concolic_class);

        if (!JS_IsObject(obj) && kc->ty == CTY_STRING && kc->hole_span && kc->hole_span[0] > 0 &&
            primitive_chain_misses_prefix(ctx, obj, kc->shape, (size_t)kc->hole_span[0]))
            return JS_UNDEFINED;
    }
    src = concolic_src_c(key);
    root = concolic_root_c(key);
    /* Which object was read is half the identity and half the shape: `a[x]` and `b[x]` are two reads, and two
       unknown records read with one key (`__FLAGS[k]`, `__NEXT_DATA__[k]`) must not share one shape or they
       buy one atom. An object with no name source has no identity, so the composition is absent there and
       every branch over the result keeps both arms. derived_operand_shape is the speller, twin of the
       ident_of_operand call below: such an object renders `?`, the one pair the shape rule cannot separate. */
    io = ident_of_operand(ctx, obj);
    ik = ident_of_operand(ctx, key);
    { char *os = derived_operand_shape(ctx, obj);
      shape = shapef("%s[%s]", os, concolic_shape_c(key) ? concolic_shape_c(key) : "{}");
      free(os); }
    f[0] = io; f[1] = ik;
    ident = concolic_ident_compose("[]", f, 2);
    free(io); free(ik);
    r = concolic_derived(ctx, shape, src ? src : shape, root ? root : shape,
                         root_whose_or_unstated(root_whose_of(key)), ident, JS_UNDEFINED);
    free(shape);
    return r;
}

/* `typeof` an unknown is unknown. A concolic is a host object of a callable class (so `x.indexOf(...)` works),
   so a plain typeof would answer "function" and `typeof x === "function"` would take an arm decided by the
   solver's representation. The answer is a concolic carrying the operand's root, so the comparison after it
   forks. Its example is the operator run on the operand's example, where the operand holds one this flow has
   not contradicted: a session sending that value gets that type, and an address composed from it is sent. */
JSValue concolic_typeof_hook(JSContext *ctx, JSValueConst v) {
    const char *sh, *f[1];
    char *shape, *ident;
    JSValue r, example = JS_UNDEFINED;
    ConcolicExState st;

    if (!concolic_is(v)) return JS_UNINITIALIZED;
    /* An operand whose type set fixes §13.5.3.1's row is answered by the row: `typeof !x` is "boolean" in
       every world, so nothing here is unknown and nothing forks over it. */
    {
        const char *fixed = tyf_typeof_fixed(v);

        if (fixed) return JS_NewString(ctx, fixed);
    }
    st = concolic_example_state(v);
    if (st == CONCOLIC_EX_HELD || st == CONCOLIC_EX_DETERMINED) {
        JSValue ex = concolic_example(ctx, v);

        DCHECK(!concolic_is(ex), "an unknown's example is itself an unknown — concolic_example answers bytes");
        example = JS_TypeOf(ctx, ex);
        JS_FreeValue(ctx, ex);
        CHECK(!JS_IsException(example), "concolic: OOM spelling the type of an unknown's example");
    }
    /* The operand's shape, not its src: every derivation off one source inherits the src, so `typeof (x+1)`
       and `typeof (x+2)` would share a shape over two identities, and the page wrote typeof of the derived
       value. */
    sh = concolic_shape_c(v);
    shape = shapef("typeof %s", sh ? sh : "{}");
    f[0] = concolic_ident_c(v);
    ident = concolic_ident_compose("typeof", f, 1);
    r = concolic_derived(ctx, shape, shape, concolic_root_c(v), root_whose_of(v), ident, example);
    free(shape);
    concolic_type_state(r, CTY_STRING);   /* §13.5.3.1's every row is a String */
    {
        const Concolic *oc = JS_GetOpaque(v, g_concolic_class);
        Concolic *rc = concolic_is(r) ? JS_GetOpaque(r, g_concolic_class) : NULL;

        if (rc) rc->is_typeof = 1;
        if (rc) rc->typeof_types = (unsigned short)tyf_of_cty(oc->ty);
        if (rc && oc->src_own == 1 && oc->src) {
            rc->typeof_of = strdup(oc->src);
            CHECK(rc->typeof_of, "concolic: OOM naming a typeof result's operand");
        }
    }
    return r;
}

/* §7.2.2 IsArray ( argument ) over an unknown is unknown, as `typeof` is: the engine's class test sees the host
   object and answers false. The answer is a boolean derived from the operand, whose example is IsArray of the
   operand's example where this flow holds one (an example is bytes, never a Proxy, so the class test is the
   whole algorithm), so a branch over it forks and its real arm is the one the reply takes. */
JSValue concolic_isarray_hook(JSContext *ctx, JSValueConst v) {
    const char *sh, *f[1];
    char *shape, *ident;
    JSValue r, example = JS_UNDEFINED;
    ConcolicExState st;

    if (!concolic_is(v)) return JS_UNINITIALIZED;
    st = concolic_example_state(v);
    if (st == CONCOLIC_EX_HELD || st == CONCOLIC_EX_DETERMINED) {
        JSValue ex = concolic_example(ctx, v);

        DCHECK(!concolic_is(ex), "an unknown's example is itself an unknown — concolic_example answers bytes");
        example = JS_NewBool(ctx, JS_IsArray(ex));
        JS_FreeValue(ctx, ex);
    }
    sh = concolic_shape_c(v);
    shape = shapef("Array.isArray(%s)", sh ? sh : "{}");
    f[0] = concolic_ident_c(v);
    ident = concolic_ident_compose("isArray", f, 1);
    r = concolic_alloc(ctx, shape, concolic_src_c(v), concolic_root_c(v), root_whose_of(v), ident, example);
    free(shape);
    return r;
}

/* JSConcolicHooks.member_never_callable: a JSON value's own members are JSON values and never callable, so where
   no slot the page wrote holds `key` and every type's intrinsic chain answers it with a data value that is not
   callable, IsCallable(Get(v, key)) is false whatever the reply sends. An accessor or a Proxy on a chain is the
   page's code, and answers 0. */
static int concolic_has_own_slot(JSContext *ctx, JSValueConst obj, JSAtom atom);
static int concolic_member_never_callable(JSContext *ctx, JSValueConst v, JSAtom key) {
    Concolic *c = g_concolic_class ? JS_GetOpaque(v, g_concolic_class) : NULL;
    size_t i;

    if (!c || !c->json_value || concolic_has_own_slot(ctx, v, key))
        return 0;
    for (i = 0; i < sizeof json_type_class / sizeof json_type_class[0]; i++) {
        JSValue proto = JS_GetClassProto(ctx, json_type_class[i]), x;
        int held = example_member(ctx, proto, key, &x), callable = held && JS_IsFunction(ctx, x);

        JS_FreeValue(ctx, proto);
        JS_FreeValue(ctx, x);
        if (!held || callable)
            return 0;
    }
    return 1;
}

void concolic_mark_json_value(JSValueConst v) {
    Concolic *c = g_concolic_class ? JS_GetOpaque(v, g_concolic_class) : NULL;

    DCHECK(c != NULL, "a value was marked as ParseJSON's that is not a concolic — the reply readers mark what "
                      "their own mint returned, and a pin or a candidate answers bytes, which they do not mark");
    if (c) c->json_value = 1;
}

/* §7.3.12 HasProperty ( O, P ) over an example, by §10.1.7.1 OrdinaryHasProperty's walk of own slots, which runs
   none of the page's code: 1 with *has set, or 0 where a link is a Proxy, whose `has` trap is the page's. */
static int example_has(JSContext *ctx, JSValueConst ex, JSAtom atom, int *has)
{
    JSValue o = JS_DupValue(ctx, ex), ev, proto;
    int flags;

    *has = 0;
    while (JS_IsObject(o)) {
        if (JS_IsProxy(o)) { JS_FreeValue(ctx, o); return 0; }
        if (example_slot(ctx, o, atom, &ev, &flags)) {
            JS_FreeValue(ctx, ev);
            JS_FreeValue(ctx, o);
            *has = 1;
            return 1;
        }
        proto = JS_GetPrototype(ctx, o);
        JS_FreeValue(ctx, o);
        o = proto;
    }
    CHECK(!JS_IsException(o), "concolic: an example's prototype could not be read");
    return 1;
}

JSValue concolic_has_property_hook(JSContext *ctx, JSValueConst obj, JSValueConst key) {
    JSValueConst ops[2] = { obj, key };
    JSValue example = JS_UNDEFINED;
    ConcolicExState st;

    if (!concolic_is(obj)) return JS_UNINITIALIZED;
    st = concolic_example_state(obj);
    if (st == CONCOLIC_EX_HELD || st == CONCOLIC_EX_DETERMINED) {
        JSValue ex = concolic_example(ctx, obj);
        JSValue kx = concolic_is(key) ? concolic_example(ctx, key) : JS_DupValue(ctx, key);
        int has;

        /* `in` over an example that is not an Object throws, and a key with no bytes names no member. */
        if (JS_IsObject(ex) && (!concolic_is(key) || concolic_example_present(key)) && example_operable(kx)) {
            JSAtom a = JS_ValueToAtom(ctx, kx);

            CHECK(a != JS_ATOM_NULL, "concolic: OOM naming an `in` key's example");
            if (example_has(ctx, ex, a, &has)) example = JS_NewBool(ctx, has);
            JS_FreeAtom(ctx, a);
        }
        JS_FreeValue(ctx, kx);
        JS_FreeValue(ctx, ex);
    }
    return concolic_new_derived(ctx, "in", ops, 2, example);
}

/* `x in concolic` / property existence: a concolic collection "has" any key, so a membership gate still runs. */
static int concolic_exotic_has(JSContext *ctx, JSValueConst obj, JSAtom atom) {
    (void)ctx; (void)atom;
    return JS_GetOpaque(obj, g_concolic_class) != NULL;
}

/* ── The record's own surface ────────────────────────────────────────────────────────────────────────────────
 * [[Get]] and [[HasProperty]] are handed a name the page wrote, so a derived unknown is the true answer
 * (`if (blk.props.pageProps.user)` forks toward the logged-in arm). §10.1.11 [[OwnPropertyKeys]] ( ) asks with
 * no name, so the own surface is the example's, the record the server sent this visitor; any other key would
 * be a fabricated field.
 * [[GetOwnProperty]] is installed beside it because every consumer needs both: §7.3.23
 * EnumerableOwnProperties ( obj, kind ) (Object.keys, for-in), §7.3.25 CopyDataProperties ( target, source,
 * excludedItems ) and §20.1.2.1 Object.assign ( target, ...sources ) take the keys, then test [[Enumerable]].
 * The value is the one [[Get]] answers, never the example's bytes, or `Object.getOwnPropertyDescriptor` would
 * concretize a loaded `features.admin:false`. §6.1.7.3 Invariants of the Essential Internal Methods sets the
 * attributes: a data property whose value may differ over time must be [[Writable]] or [[Configurable]], and
 * each read mints afresh, so both are set; only [[Enumerable]] comes from the example. A concolic has no Proxy
 * handler; it maintains §6.1.7.3 directly, as required of implementation-provided exotic objects. */

/* Answer from real slots only (see concolic.h). A plain static: the host is one agent per instance and nothing
   here is entered from a second thread. */
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

/* Whether `obj` carries a real own slot for `atom`, the ordinary layer under this class's own surface. The mode
   makes the question answerable: with the exotic hook armed, QuickJS's [[GetOwnProperty]] answers 1 for both a
   shape slot and an example member. Runs in every build: the unknown-member materialisation and the
   disjointness assert both ask it. */
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

/* §10.1.5 [[GetOwnProperty]] ( propertyKey ) over the record, reached only after QuickJS's ordinary lookup
   found no slot, so a member the flow has materialised (see the define below) never arrives here. */
static int concolic_exotic_get_own(JSContext *ctx, JSPropertyDescriptor *desc, JSValueConst obj, JSAtom atom)
{
    Concolic *c = JS_GetOpaque(obj, g_concolic_class);
    JSValue ev;
    int eflags;

    if (!c)
        return 0;
    /* The delta reads storage, and this class has none for its members (see concolic.h). */
    if (g_slots_only)
        return 0;
    if (!example_slot(ctx, c->example, atom, &ev, &eflags))
        return 0;
    if (!desc) {
        JS_FreeValue(ctx, ev);
        return 1;
    }
    /* The example's own bytes are not the answer: they ride the derivation as its example. An accessor member
       has no value (example_slot) and is still reported as a data property, since what the getter would
       compute is unknown. */
    JS_FreeValue(ctx, ev);
    /* The attributes are the record's for an object example and §10.4.3's for a String one: every member of a
       record can be materialised (the define below moves it onto the object), so C|W is forced; a String's
       members are fixed (§10.4.3.5 step 10, §10.4.3.4 step 8), and forcing them would report a member the
       define is about to refuse. */
    desc->flags = JS_IsString(c->example)
                      ? (eflags & (JS_PROP_ENUMERABLE | JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE))
                      : ((eflags & JS_PROP_ENUMERABLE) | JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE);
    desc->getter = JS_UNDEFINED;
    desc->setter = JS_UNDEFINED;
    desc->value = concolic_exotic_get(ctx, obj, atom, obj);
    return 1;
}

/* The enumeration's question, minted once (concolic.h says why the fork cannot live at the internal method).
   The identity is the record's own under §10.1.11's name, so two consumers asking about one record ask one
   question. A record this engine cannot spell composes no identity, and the internal method says so at its
   own site. */
static JSValue own_keys_pred(JSContext *ctx, const Concolic *c)
{
    const char *f[1];
    char *ident;

    f[0] = c->ident;
    ident = concolic_ident_compose("[[OwnPropertyKeys]]", f, 1);
    if (!ident)
        return JS_UNINITIALIZED;
    /* Not a source read (concolic_alloc, never concolic_derived): a boolean about the record. */
    {
        JSValue r = concolic_alloc(ctx, "{cmp}", NULL, NULL, 0u, ident, JS_UNDEFINED);
        concolic_type_state(r, CTY_BOOLEAN);
        return r;
    }
}

JSValue concolic_own_keys_pred(JSContext *ctx, JSValueConst record)
{
    Concolic *c = g_concolic_class ? JS_GetOpaque(record, g_concolic_class) : NULL;

    /* No question for an ordinary object (its key set is its shape) or a record with an example (its key set is
       the example's, answered by the internal method below). Only an example-free record has an unknown key
       set; the condition matches the method's so the asker and the reader agree. */
    if (!c || c->ex_has)
        return JS_UNINITIALIZED;
    return own_keys_pred(ctx, c);
}

/* The name of the record's n-th unknown own member (see concolic.h). The identity and display come from
   concolic_new_derived and the conversion to a property key is the engine's JS_ValueToAtom, where §7.1.21
   ToPropertyKey ( arg )'s answer for an unknown key lives (its shape, through .key_name); composing the string
   here would copy both rules. `#n`, not `>n`: the engine spells the per-position question
   `[[OwnPropertyKeys]]>n`, and what the member is called is a different question about the same position.
   A candidate run may answer a String here (concolic_derived substitutes the payload), and a member named by
   the payload is what the re-fire is for; JS_ValueToAtom takes either. */
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
    /* Asserted, not branched past: both callers establish that `record` is this class's value, and
       concolic_new_derived answers JS_UNINITIALIZED only when no operand is unknown. */
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
    /* The same condition as concolic_own_keys_pred, written the same way: a record with an example has an
       observed key set, so no question was minted for the caller to stand on. */
    DCHECK(!c->ex_has,
           "a record whose example holds its key set was asked to materialise an UNKNOWN member — its key set "
           "is an observation about the payload this visitor was served, so the caller forked over a question "
           "concolic_own_keys_pred never minted");
    DCHECK(n >= 0, "an unknown member was asked for at a negative position — the chain's cursor is a count");
    atom = own_member_atom(ctx, record, n);
    if (atom == JS_ATOM_NULL)
        return -1;
    /* Already materialised is the ordinary case: the name is a function of the record and `n`, so a second
       enumeration in this flow replays the same arms to the same member. Re-defining it would replace a value
       the page may have written since, and a frozen record would refuse it.
       Named residual. Not covered: a [[Delete]] of a materialised member (configurable, per §6.1.7.3) is undone
       by the next enumeration in the same flow, because the chain replays its recorded answers from position 0.
       Next diff: a per-flow count of this record's unknown members already performed, so the chain resumes past
       them; it must live where the COW delta already carries state, since this class keeps no delta-captured C
       state (nothing here calls cow_capture_host_record). How its absence shows: `for (k in x) delete x[k]`
       followed by a second enumeration lists the deleted member again, and `k in x` answers true. */
    if (concolic_has_own_slot(ctx, record, atom)) {
        JS_FreeAtom(ctx, atom);
        return 1;
    }
    /* The member's value is the one a read of that name answers, through the same entry: `for (k in r) r[k]`
       and a later `r[<that name>]` are one value, and a candidate substituted for the member reaches both. */
    val = concolic_exotic_get(ctx, record, atom, record);
    /* JS_PROP_NO_EXOTIC because this is the class's own materialisation, which must not ask this class what it
       reports about a member it is making. The attributes are those concolic_exotic_get_own reports, as §6.1.7.3
       Invariants of the Essential Internal Methods requires of a data property that "may return different
       values over time". */
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

/* §10.1.11 [[OwnPropertyKeys]] ( ) over the record. QuickJS merges this list with the object's ordinary keys and
   dedups neither, so the two must be disjoint (§6.1.7.3 Invariants of the Essential Internal Methods: "The
   returned List must not contain any duplicate entries"). The define below maintains that by taking a member
   out of the example as it materialises it, and the tail asserts it. */
static int concolic_exotic_own_names(JSContext *ctx, JSPropertyEnum **ptab, uint32_t *plen, JSValueConst obj)
{
    Concolic *c = JS_GetOpaque(obj, g_concolic_class);
    JSPropertyEnum *tab = NULL;
    uint32_t n = 0;

    *ptab = NULL;
    *plen = 0;
    if (!c)
        return 0;
    if (!c->ex_has) {
        /* No example is not an empty record: answering the empty List as a fact would decide
           `Object.keys(x).length === 0` for a record the run never saw. So it is answered as the flow's own arm.
           The question (`concolic_own_keys_pred`) is asked by the consumer, which has a resume point to fork at;
           this internal method, reached from a C activation, reads the recorded answer back: concretize on the
           decision. It hands over the value and never rebuilds the key, whose format lives in one file
           (decide.h's decide_value_arm). */
        JSValue pred = own_keys_pred(ctx, c);
        int arm = decide_value_arm(pred);

        JS_FreeValue(ctx, pred);
        if (arm == 0)
            return 0;   /* this flow's decided world: the record holds no own member this run can name */
        if (arm == 1) {
            /* The arm where the record holds members also answers the empty exotic list: there is no example,
               and the members the flow decided on are ordinary slots by now (concolic_own_key_mint), already
               listed by QuickJS's key walk; listing them again would duplicate them. Not asserted: on correct
               runs `delete x[k]` can leave the record with none while this arm stands, and naming member 0
               would mint a derivation at the site where an @S delivery is recorded. The engine's own chain
               asserts the arm at every position. */
            return 0;
        } else {
            /* -1: this flow has not been asked, a fact about the consumer: whatever reached this internal
               method did not first fork on the record's key set (the engine's own-keys step machine does,
               through JSConcolicHooks.own_keys_pred). `c->ident` says which of two states this is, read the
               way own_keys_pred reads it. (i) Named: the question would have been minted, so a consumer
               reached here without the step machine and must be routed onto it. (ii) Unnamed: the seam
               correctly declined to fork a question with no key, so the arm is absent by construction; what is
               missing is a name source for the record's class (literal_ident's named residual). */
            if (!c->ident) {
                /* (ii) The record cannot be spelled: no predicate, nothing for the seam to fork. */
                DFAILF("an unknown with NO EXAMPLE was asked to enumerate itself and THIS ENGINE CANNOT SPELL "
                       "THE RECORD (its identity is absent), so concolic_own_keys_pred minted no predicate and "
                       "the own-keys seam correctly declined to fork a question with no key. The empty List "
                       "would state that the record holds nothing, which is a fact this run never observed. "
                       "THIS IS NOT A ROUTING GAP and a second fork would not reach it: what is missing is the "
                       "NAME, which is literal_ident's named residual — a page-created object is named by its "
                       "creation site (quickjs already composes one at JS_OrphanHash) PLUS the creating flow's "
                       "own count of prior creations at that site, since one `{}` in a loop is one site and a "
                       "thousand objects. Build BOTH halves: the site alone names three iterations of one loop "
                       "as one object and LOSES arms. This record reads shape=%s root=%s",
                       c->shape ? c->shape : "(none)", c->root ? c->root : "(none)");
            } else {
                /* (i) The record is named, so the question was never asked; a fact about the consumer. */
                DFAILF("an unknown with NO EXAMPLE was asked to enumerate itself and this flow holds no ARM "
                       "for its enumeration, THOUGH THE RECORD IS NAMED (%s) — so concolic_own_keys_pred "
                       "would have minted the predicate, and whatever reached this internal method did so "
                       "without asking it. The fork is NOT missing and a second one must not be built: "
                       "step_ownkeys_run asks JSConcolicHooks.own_keys_pred, forks the boolean through the "
                       "step driver, runs the unknown-member chain on the true arm and only then issues "
                       "request 11. ROUTE THE CONSUMER ONTO IT — AND DERIVE WHICH ONE RATHER THAN READING A "
                       "LIST, because an earlier form of this message listed the two routes step_request_check "
                       "names and was short by a whole SHAPE. THE PROPERTY, WHICH DOES NOT ROT AS THE ENGINE "
                       "GAINS CONSUMERS: the seam asks its question about the object THE CONSUMER NAMED, and "
                       "the keyed entry walks `fwd ? gp_fwd : gp_obj` — so this line is reached with no arm "
                       "exactly when the object FINALLY WALKED is not the object a step_ownkeys_run call was "
                       "handed. That has two shapes and a list of REQUESTERS can only express one. (1) The "
                       "request was issued elsewhere, so step_request_check never judged it: "
                       "PerformPromiseAllKeyed step 1 asks under its own step code, and the proxy invariant "
                       "driver asks from JS_CallInternal under no step code at all. (2) The request PASSED "
                       "step_request_check and its OPERAND was replaced afterwards — which no strengthening "
                       "of that assert can ever reach, because the request did come from this wrapper and "
                       "only the object changed. ECMAScript §10.5.11 \"[[OwnPropertyKeys]] ( )\" holds one "
                       "of each, and only its step 11, \"Let targetKeys be ? target.[[OwnPropertyKeys]]()\", "
                       "was named here. ITS STEP 6 IS THE OTHER SHAPE AND IS WHY THIS CRASH MEETS ORDINARY "
                       "PAGES: \"If trap is undefined, then Return ? target.[[OwnPropertyKeys]]()\" is "
                       "performed INLINE as gp_fwd over a trapless proxy's target, so a page that wraps an "
                       "unknown in `new Proxy(x, {})` and enumerates it lands here with the seam having "
                       "asked its question about the PROXY. SO RUN BOTH GREPS AND NEVER "
                       "ONE: `git grep -n 'gp_op = GP_OWNKEYS' engine/qjs/quickjs.c` names every REQUESTER, "
                       "`git grep -n 'gp_fwd =' engine/qjs/quickjs.c` names every SUBSTITUTION, and the "
                       "requester grep alone is what missed shape (2)",
                       c->ident);
            }
        }
        return 0;
    }
    if (JS_IsString(c->example)) {
        /* §10.4.3.3 [[OwnPropertyKeys]] ( ) over a String example: step 5, "For each integer i such that
           0 ≤ i < length, in ascending order", then the other own string keys, of which a String has one,
           `length`. The empty List would be false for a non-empty string. No ordinary slot can collide: the
           define below refuses every member of a String example (§10.4.3.5 makes them non-configurable), and
           the tail assertion checks it on every walk. */
        int64_t units = string_example_units(ctx, c->example), k;

        n = (uint32_t)(units + 1);
        tab = js_malloc(ctx, (size_t)n * sizeof *tab);
        CHECK(tab != NULL, "concolic: OOM enumerating the own members of a String example");
        for (k = 0; k < units; k++) {
            tab[k].atom = JS_NewAtomUInt32(ctx, (uint32_t)k);
            CHECK(tab[k].atom != JS_ATOM_NULL, "concolic: OOM naming a String example's index member");
            tab[k].is_enumerable = 1;      /* §10.4.3.5 step 10 */
        }
        tab[units].atom = JS_NewAtom(ctx, "length");
        CHECK(tab[units].atom != JS_ATOM_NULL, "concolic: OOM naming a String example's `length`");
        tab[units].is_enumerable = 0;      /* §10.4.3.4 step 8 */
        goto have_keys;
    }
    if (!JS_IsObject(c->example))
        return 0;   /* a number/boolean/bigint/null record has no own keys — `Object.keys(5)` is [] */
    DCHECK(!JS_IsProxy(c->example),
           "a concolic's example is a Proxy — enumerating it is that Proxy's `ownKeys` trap, which is the "
           "page's code, and an internal method reached from C has no flow base to run one on");
    if (JS_GetOwnPropertyNames(ctx, &tab, &n, c->example, JS_GPN_STRING_MASK | JS_GPN_SYMBOL_MASK) < 0)
        return -1;
have_keys:
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

/* §10.1.6 [[DefineOwnProperty]] ( propertyKey, propertyDesc ), reached only when the ordinary own-property scan
 * found nothing: §10.1.6.3 ValidateAndApplyPropertyDescriptor with `current` = what this class reported, so
 * attributes the incoming descriptor does not restate are current's, not false.
 *
 * Needed by the write path: §10.1.9.2 OrdinarySetWithOwnDescriptor ( obj, propertyKey, value, receiver,
 * ownDesc ) step 2.h performs `receiver.[[DefineOwnProperty]](propertyKey, valueDesc)` with only a [[Value]],
 * and without a `current` to merge against, `cfg.region = "x"` on a held member would make it
 * non-enumerable, non-writable and non-configurable.
 *
 * Materialising takes the member out of the example, which keeps the two key lists disjoint. The example is
 * the record's concrete state, and a page overwriting a member changed it. The removal is an ordinary property
 * delete, so the COW delta captures it and a context switch restores it with the slot. */
static int concolic_exotic_define_own(JSContext *ctx, JSValueConst obj, JSAtom prop, JSValueConst val,
                                      JSValueConst getter, JSValueConst setter, int flags)
{
    Concolic *c = JS_GetOpaque(obj, g_concolic_class);
    JSPropertyDescriptor cur;
    int ret, nf = flags;

    if (!c || !concolic_exotic_get_own(ctx, &cur, obj, prop))
        return JS_DefineProperty(ctx, obj, prop, val, getter, setter, flags | JS_PROP_NO_EXOTIC);

    /* A String example's own members are not materialisable, so the define is refused. §10.4.3.2
       [[DefineOwnProperty]] ( propertyKey, propertyDesc ) step 2 sends such a member to
       IsCompatiblePropertyDescriptor, which for a non-configurable, non-writable current accepts only a define
       that changes nothing, and current's [[Value]] here is a fresh derived unknown no incoming value equals.
       There is also no character to delete from a String primitive. It throws or answers false by the caller's
       rule (JS_PROP_THROW, JS_PROP_THROW_STRICT), which JS_RefuseOrThrowTypeError answers.
       Named residual. Not covered: a define that restates only attributes the member already has
       (`Object.defineProperty(x, "0", {enumerable: true})`), which IsCompatiblePropertyDescriptor accepts.
       Next diff: that comparison over `cur.flags` and `flags`, accepting where every stated field matches, the
       `nf` merge below read as a test. How its absence shows: that call throwing a TypeError where a browser
       returns the object; never for a define carrying a value. */
    if (JS_IsString(c->example)) {
        JS_FreeValue(ctx, cur.value);
        return JS_RefuseOrThrowTypeError(ctx, flags, "property is not configurable");
    }
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
        /* A define that restates no value keeps current's: `Object.defineProperty(cfg, "region",
           {enumerable:false})` must not turn a held member into `undefined`. */
        if (!(nf & JS_PROP_HAS_VALUE)) {
            nf |= JS_PROP_HAS_VALUE;
            val = cur.value;
        }
    }
    /* Out of the example before into the shape, so no window exists in which both lists name it. */
    ret = JS_DeleteProperty(ctx, c->example, prop, 0);
    DCHECK(ret > 0, "a member this class reported could not be removed from the example it was read out of — "
                    "the example's own slots are the ones JSON.parse and the reply decoders build, all of them "
                    "configurable, so a refusal is a record some other path froze");
    if (ret > 0)
        ret = JS_DefineProperty(ctx, obj, prop, val, getter, setter, nf | JS_PROP_NO_EXOTIC);
    JS_FreeValue(ctx, cur.value);
    return ret;
}

/* §10.1.10 [[Delete]] ( propertyKey ), reached only when the ordinary own-property scan found nothing. This
   class reports members as configurable, so §10.1.10.1 OrdinaryDelete ( obj, propertyKey ) step 3 requires a
   member the example holds to be removed, and the example is the only place it exists; otherwise `delete
   cfg.region` would answer true while Object.keys still listed `region`. */
static int concolic_exotic_delete(JSContext *ctx, JSValueConst obj, JSAtom prop)
{
    Concolic *c = JS_GetOpaque(obj, g_concolic_class);
    JSValue ev;
    int eflags, ret;

    if (!c || !example_slot(ctx, c->example, prop, &ev, &eflags))
        return 1;   /* nothing there to remove, which [[Delete]] reports as success */
    JS_FreeValue(ctx, ev);
    /* §10.1.10.1 step 4: a non-configurable member is not removed and the answer is false. Every own member of
       a String example is one (§10.4.3.5 step 10, §10.4.3.4 step 8), and a String primitive has no property to
       delete, so this is asked before the removal below. A bare false, not a throw: §13.5.1.2 Runtime
       Semantics: Evaluation step 4.f ("If deleteStatus is false and ref.[[Strict]] is true, throw a TypeError
       exception") raises the strict-mode TypeError from the operator. */
    if (JS_IsString(c->example))
        return 0;
    DCHECK(!JS_IsProxy(c->example),
           "a concolic's example is a Proxy — removing a member is that Proxy's `deleteProperty` trap, which "
           "is the page's code, and an internal method reached from C has no flow base to run one on");
    ret = JS_DeleteProperty(ctx, c->example, prop, 0);
    DCHECK(ret >= 0, "removing a member from a concolic's example threw — the example holds only the slots "
                     "JSON.parse and the reply decoders build and this call runs none of the page's code");
    return ret;
}

/* §10.1.1 [[GetPrototypeOf]] ( ) over an unknown. This class registers no class_proto, so without this entry
 * its objects would answer JS_NULL, a claim nothing observed and the one answer that makes a prototype walk
 * throw instead of fork. The answer is a derivation over the receiver, memoised (see the `proto` field).
 * It does not terminate on its own, so the link walk forks: §7.3.21 OrdinaryHasInstance ( ctor, instance )
 * step 6 repeats 6.a "Set instance to ? instance.[[GetPrototypeOf]]()", 6.b "If instance is null, return
 * false" and 6.c "If SameValue(proto, instance) is true, return true" in C, which a chain of derived unknowns
 * never satisfies. "How long" is a question asked at the walk, which alone holds `proto`, and it has three
 * completions (the chain ends, this link is proto, neither); js_instanceof_step declares all three.
 * §10.4.7.2 SetImmutablePrototype ( obj, proto ), reached from JS_SetPrototypeInternal, DCHECKs rather than
 * answering: step 2's SameValue between Objects never holds for a derived `current`, so any answer would
 * refuse a write Chrome accepts; the check names the ask to build (step 6.c's two-completion question).
 * It runs no page code, as the entry's contract requires: JS_GetPrototype's C callers have no flow base. */
static JSValue concolic_exotic_get_prototype(JSContext *ctx, JSValueConst obj) {
    Concolic *c = JS_GetOpaque(obj, g_concolic_class);

    DCHECK(c != NULL, "§10.1.1 [[GetPrototypeOf]] ( ) was dispatched to this class's entry for an object that "
                      "carries none of its state — the exotic table is reached through the class id, so an "
                      "object answering to it with no record is one this file did not mint");
    if (!c)
        return JS_NULL;
    /* A link the page wrote (prototype_slot_settable below): the record is minted with a null link, so a
       non-null one is a set this flow made, and it is the answer. A set to null keeps the derived answer. */
    {
        JSValue slot = JS_GetPrototypeSlot(ctx, obj);

        if (!JS_IsNull(slot))
            return slot;
    }
    /* Derived once and held. The operand is the receiver, so the answer's identity is composed from this
       value's own, and two asks about one record are one question here and at the link walk's seam. */
    if (JS_IsUninitialized(c->proto))
        c->proto = concolic_new_derived(ctx, "[[GetPrototypeOf]]", &obj, 1, JS_UNDEFINED);
    /* concolic_new_derived answers JS_UNINITIALIZED only when no operand is unknown, and the receiver reached
       this entry through this class's own exotic table; said here because JS_GetPrototype's own check would
       report only that the answer is neither an Object nor null. */
    DCHECK(!JS_IsUninitialized(c->proto),
           "the derivation of an unknown's prototype answered the no-unknown-operand sentinel over a receiver "
           "this class's own exotic table dispatched on");
    /* A new reference: [[GetPrototypeOf]]'s callers own what it returns (JS_GetPrototype's, and §10.4.7.2 step
       1's). The memo keeps its own. */
    return JS_DupValue(ctx, c->proto);
}

static JSClassExoticMethods g_concolic_exotic = {
    .get_own_property = concolic_exotic_get_own,
    .get_own_property_names = concolic_exotic_own_names,
    .delete_property = concolic_exotic_delete,
    .define_own_property = concolic_exotic_define_own,
    .get_property = concolic_exotic_get,
    .get_prototype = concolic_exotic_get_prototype,
    /* An unknown denotes a value whose [[SetPrototypeOf]] is the ordinary one (only a WindowProxy or a Location is
       an immutable prototype exotic object): where it is extensible its prototype is V after the set whichever
       arm step 2's SameValue takes, so the ordinary path writes V to the record's own link. */
    .prototype_slot_settable = true,
    .has_property = concolic_exotic_has,
    /* The lookup is a slot read of this value's own example and a derivation composed in this file, so it runs
       no page code by construction, which lets the engine's accessor walk and COW slot read run it from C. */
    .get_own_property_no_user_code = true,
    /* get_prototype_walkable is not declared: each link of a derived chain is a new unknown, so a walk run from C
       that took it would never end.
       Named residual. Not covered: an ordinary [[Set]] or accessor walk that misses on an unknown stops there, as
       at null, and never asks whether an ancestor holds a setter or getter. Next diff builds: that walk's three
       completions as a fork, as js_instanceof_step declares them. Absence shows as: a write through an unknown's
       chain landing on the receiver in every flow, with no flow in which an inherited setter runs. */
};

void concolic_init(JSContext *ctx) {
    JSRuntime *rt = JS_GetRuntime(ctx);
    /* Every host calls this once per agent before anything can mint a source or reach a sink, so the metrics
       block is brought up here and released by solver_agent_free. A document joining through `qjs_join`
       calls no `_init` and sums into the same rows, which is right for an origin-keyed agent cluster. */
    metrics_agent_init();
    /* The whose-mask holds one bit per declared member, and the record's field bounds how many there may be: a
       member past its width would shift out and answer no for a provenance a mint stated. Checked once per
       agent, where both are in hand. */
    DCHECK(CONCOLIC_WHOSE_COUNT <= (int)(sizeof(unsigned) * 8),
           "concolic.h's CONCOLIC_ROOT_WHOSE declares more members than the Concolic record's `root_whose` "
           "mask has bits — the member past the width would shift out, so concolic_root_whose_any would answer "
           "NO for a provenance its own mint stated and that population would silently stop being "
           "distinguishable from every other. Widen the field with the list");
    if (g_concolic_class == 0) {
        JS_NewClassID(rt, &g_concolic_class);
        DCHECK(g_concolic_class != 0, "concolic: class id allocation returned 0 — runtime class table exhausted");
    }
    if (!JS_IsRegisteredClass(rt, g_concolic_class)) {
        JSClassDef def = { "Concolic", .finalizer = concolic_finalizer, .gc_mark = concolic_gc_mark,
                           .call = concolic_call_c_entry, .exotic = &g_concolic_exotic };
        int r = JS_NewClass(rt, g_concolic_class, &def);
        CHECK(r == 0, "concolic: JS_NewClass failed — cannot register the solver's value type");
    }
}

/* The holder's end of the source registry: solver/engine.h's release column, run after the whole platform.
 *
 * The rows are not freed here. Each is a claim its claimant gives back at its own release, on core/platform.h's
 * release column, which platform_agent_free runs before the solver's, so an empty registry here is a checked
 * statement about the claimants. The assert names the component from the row, so this file names nobody.
 *
 * The array under them is this component's and is freed, not merely emptied: `g_srcs` is a plain malloc no
 * leak detector here sees (not a GC object, not an atom, not counted by JS_DUMP_LEAKS's js_malloc_rt count),
 * and a kept buffer would be sized by a dead runtime's reclaim allocator. The count goes with it because a
 * release build compiles the assert out, and a count over a freed array would be a use-after-free. */
static void source_overlay_withdraw(void);

void concolic_free(void)
{
    for (int i = 0; i < g_members_n; i++) {
        free(g_members[i].src); free(g_members[i].ident); free(g_members[i].root);
        for (int k = 0; k < g_members[i].n; k++) free(g_members[i].path[k]);
        free(g_members[i].path);
    }
    free(g_members);
    g_members = NULL;
    g_members_n = g_members_cap = 0;
    free(g_members_ix);
    g_members_ix = NULL;
    g_members_ix_cap = 0;
    int i;

    source_overlay_withdraw();
    /* absent.c is this component's own: its published-namespace paths name records of a document that is gone,
       and its precondition rows are claims in the registry below, so it is released before that registry's
       emptiness is asserted. */
    absent_free();
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
#if APICLIENT_DEV
    if (g_pres_n != 0)
        DFAILF("%s did not give back the precondition SOURCE `%s` before the solver's agent state was released "
               "— its claimant releases it at its own concolic_undeclare_sources", g_pres[0].component,
               g_pres[0].src);
#endif
    free(g_pres);
    g_pres = NULL;
    g_pres_n = g_pres_cap = 0;
    /* The installed @S substitution is this component's own copy: concolic_set_candidate strdups it from the
       running flow at every switch, so the last flow leaves a pair that flow_registry_free (which frees the
       flow's strings) cannot reach. Freed, not asserted null, since nothing orders the last switch against
       this call. */
    free(g_cand_src);
    free(g_cand_payload);
    free(g_cand_at);
    free(g_cand_bytes);
    g_cand_src = g_cand_payload = g_cand_at = g_cand_bytes = NULL;
    /* The delivery fact goes with the pair it is about: concolic_candidate_delivered asserts a 1 never stands
       beside a NULL payload. This is the running flow's live copy; parked copies went with their blobs. */
    g_cand_delivered = 0;
    /* The names spent on unknown keys: each row describes a value of a document that is gone, keyed by
       sources the next agent declares afresh, so a row left standing would answer the next document's
       enumeration with the last one's provenance. */
    for (i = 0; i < g_keynames_n; i++) {
        free(g_keynames[i].atom);  free(g_keynames[i].shape);
        free(g_keynames[i].src);   free(g_keynames[i].root);
        free(g_keynames[i].ident);
    }
    free(g_keynames);      g_keynames = NULL;      g_keynames_n = g_keynames_cap = 0;
    free(g_keynames_hash); g_keynames_hash = NULL; g_keynames_hash_cap = 0;
    /* The contradicted-example latch, agent state like the two above: left set, it would make the next agent
       probe a chain in which nothing has been contradicted. A cost, not a wrong answer. */
    g_ex_contra_any = 0;
    /* …and the pin latch, for the same reason. */
    g_pin_any = 0;
    /* …and the type facet latch. */
    g_type_facet_any = 0;
    /* The value class is deliberately not given back here, and that is asserted (core/dom/document.c says the
       same of its realm-mark hook). Every live Concolic outlives this call: JS_FreeRuntime's finalizer reaches
       each record through this id, and the collection before it marks each example through it. A cleared id
       would leak every record and free examples still referenced. */
    DCHECK(g_concolic_class != 0,
           "the Concolic value class was given back before the runtime that issued it — it must NOT be, and "
           "this is where that is checked: every value of this class is finalized by JS_FreeRuntime, which "
           "reaches its record through this id, and the collection before it marks each example through it");
}

/* The longest display shape (and shape-spelled provenance) a value keeps in full. A derived shape is composed over
   its operands' shapes, so a value derived from itself twice per iteration doubles it as an identity spelling
   would (concolic_ident_compose); past this length the shape is its first bytes, cut on a UTF-8 boundary, then
   `…#` and the first 128 bits of the whole spelling's SHA-256. The head keeps the root's hole readable, and the
   digest keeps two different spellings two different names, as an unknown name must be. A function of the
   spelling alone, so it is the same in every flow and session. Caller frees. */
#define CONCOLIC_SHAPE_SPELL_MAX 1024
static char *shape_bounded(const char *spelled)
{
    static const char HEX[] = "0123456789abcdef";
    size_t len = strlen(spelled), head = CONCOLIC_SHAPE_SPELL_MAX - 40;
    uint8_t d[SECURE_HASH_MAX_DIGEST];
    SecureHash h;
    char *out;
    int i;

    if (len <= CONCOLIC_SHAPE_SPELL_MAX)
        return strdup(spelled);
    while (head > 0 && ((unsigned char)spelled[head] & 0xC0) == 0x80)
        head--;
    secure_hash_init(&h, SECURE_HASH_SHA256);
    secure_hash_update(&h, (const uint8_t *)spelled, len);
    secure_hash_finish(&h, d, sizeof d);
    out = reclaim_malloc(head + 4 + 32 + 1);
    if (!out) return NULL;
    memcpy(out, spelled, head);
    memcpy(out + head, "\xe2\x80\xa6#", 4);   /* U+2026 HORIZONTAL ELLIPSIS, then the digest */
    for (i = 0; i < 16; i++) {
        out[head + 4 + 2 * i] = HEX[d[i] >> 4];
        out[head + 4 + 2 * i + 1] = HEX[d[i] & 15];
    }
    out[head + 4 + 32] = '\0';
    return out;
}

/* @BOUND, the negative control on both bounds: two spellings that share their first 2000 bytes and differ only
   after them must keep two names. Each composition is bounded at its own level over members already bounded, so
   a bounded name is a hash over the operator and its members' bounded names and the full spelling of a
   self-derived value is never built. */
void concolic_bound_selftest(void)
{
    char a[2101], b[2101];
    const char *fa[1] = { a }, *fb[1] = { b };
    char *sa, *sb, *ia, *ib;

    memset(a, 'x', 2100); memset(b, 'x', 2100);
    a[2099] = 'A'; b[2099] = 'B';
    a[2100] = b[2100] = '\0';
    sa = shape_bounded(a); sb = shape_bounded(b);
    ia = concolic_ident_compose("t", fa, 1); ib = concolic_ident_compose("t", fb, 1);
    CHECK(sa && sb && ia && ib, "concolic: OOM in the bound selftest");
    printf("@BOUND {\"shapeLen\":%zu,\"shapesDiffer\":%d,\"identLen\":%zu,\"identsDiffer\":%d}\n",
           strlen(sa), strcmp(sa, sb) != 0, strlen(ia), strcmp(ia, ib) != 0);
    CHECK(strlen(sa) <= CONCOLIC_SHAPE_SPELL_MAX && strlen(sb) <= CONCOLIC_SHAPE_SPELL_MAX,
          "a bounded display shape is longer than its bound");
    CHECK(memcmp(sa, a, 900) == 0, "a bounded display shape lost the head of its spelling");
    CHECK(strcmp(sa, sb) != 0, "two display shapes that differ past their kept head were bounded to one name");
    CHECK(strcmp(ia, ib) != 0, "two identities that differ past the bound were digested to one key");
    free(sa); free(sb); free(ia); free(ib);
}

/* `whole`: the shape is a string concatenation's, display(a)++display(b), whose hole spans index its bytes and
   which key_name spells as a property key, so it is kept whole; it grows only as the concatenated string itself
   does. Every other shape is bounded by shape_bounded. */
static JSValue concolic_alloc_shaped(JSContext *ctx, const char *shape, const char *src, const char *root,
                                     unsigned root_whose, char *ident, JSValue example, bool whole)
{
    JSValue obj;
    Concolic *c;

    DCHECK(g_concolic_class != 0, "concolic_new before concolic_init — the class is unregistered");
    /* Nothing is minted inside a slots-only span, asserted at the one mint every concolic goes through.
       concolic_slots_only_begin brackets the COW delta's baseline read and read-back, so a value minted there
       would be recorded as what a slot held and written back by the unapply as a real own slot, shadowing this
       class's [[Get]] for every sibling flow. The delta reads through JS_GetOwnSlotDesc, which runs no page code,
       so no hook is reachable from it; this is where a route that changed that would be caught. */
    DCHECK(!g_slots_only,
           "a concolic value was minted inside the COW delta's slots-only span — the delta records what a SLOT "
           "held, so a value synthesised during that read is put BACK by the unapply as a real own slot and "
           "shadows this class's [[Get]] for every sibling flow");
    /* `src` and `root` are present together or absent together: a value with a provenance is a source read
       (its own root) or derived from something that had one. A `src` with no `root` would be a report naming a
       source and stating that no navigation delivers it. */
    DCHECK(!!src == !!root,
           "a concolic value carries a provenance without a delivery ROOT, or a root with no provenance — the "
           "two are one fact about where the bytes came from and every derivation inherits the second while "
           "some of them re-mint the first, so a mismatch is a derivation that forgot to thread it");
    /* …and the mask with the root, for the same reason: a root with an empty mask would make
       `concolic_root_whose_any` answer no for every member, and the product's bar could not tell a hole this
       engine minted from one a server supplied. The mask goes wherever the root goes, and a derivation that
       forgets crashes here. CONCOLIC_WHOSE_UNSTATED is a bit, so "no mint could say" passes and only "nobody
       threaded anything" fails. */
    DCHECK(!!root == !!root_whose,
           "a concolic value carries a delivery ROOT with no statement of WHOSE unknown it is, or such a "
           "statement with no root — the two are one fact about where the bytes came from, so a mismatch is a "
           "derivation that threaded the root string and dropped the mask beside it. Every root-threading mint "
           "passes root_whose_of(the operand) or derived_root_whose(the operand list); a value no mint could "
           "speak for is CONCOLIC_WHOSE_UNSTATED, which is a bit of its own and not this empty mask");
    obj = JS_NewObjectClass(ctx, g_concolic_class);
    CHECK(!JS_IsException(obj), "concolic: the value object could not be allocated — a dropped concolic "
                                "collapses a branch to a concrete arm and deletes everything behind the other");
    c = reclaim_calloc(1, sizeof *c);
    CHECK(c, "concolic_new: OOM allocating value state — a dropped concolic corrupts the flow's domain");
    c->shape = whole ? strdup(shape) : shape_bounded(shape ? shape : "{}");
    CHECK(c->shape, "concolic: OOM copying a display shape");
    c->src = src ? shape_bounded(src) : NULL;
    CHECK(!src || c->src, "concolic: OOM copying a source's provenance");
    /* A value whose injection identity is its own display shape names itself by it: a member read, a call
       result, a `typeof`. A source read names itself under a src that is not its shape (pin_src_names_self);
       a derivation that inherited an operand's src does not. */
    c->src_own = (signed char)(c->src != NULL && !strcmp(c->src, c->shape));
    c->root = root ? strdup(root) : NULL;
    CHECK(!root || c->root, "concolic: OOM copying a value's delivery root");
    c->root_whose = root_whose;   /* one fact with the root above — see the field and the assert */
    c->ident = ident;       /* consume — NULL means this engine cannot spell the value; see the struct */
    c->example = example;   /* consume */
    c->ex_has = !JS_IsUndefined(example);
    c->cmp_op = OPCMP_NONE;
    /* The algorithm is the one field here reclaim_calloc cannot set: "not an equality" is CMP_ALGO_NONE, and
       its zero would be JS_CONCOLIC_EQ_LOOSE, claiming the page wrote `==` over a non-predicate. */
    c->cmp_algo = CMP_ALGO_NONE;
    c->cmp_src_own = -1;   /* unstated until an equality mint states it */
    /* Nobody has asked for this value's prototype yet, a state rather than a zero (see the field), written at
       the one mint every concolic goes through. */
    c->proto = JS_UNINITIALIZED;
    c->rel_op = REL_NONE;   /* reclaim_calloc already zeroes it; stated beside cmp_op so the two stay one act */
    JS_SetOpaque(obj, c);
    return obj;
}

static JSValue concolic_alloc(JSContext *ctx, const char *shape, const char *src, const char *root,
                              unsigned root_whose, char *ident, JSValue example)
{
    return concolic_alloc_shaped(ctx, shape, src, root, root_whose, ident, example, false);
}

static JSValue concolic_derived_shaped(JSContext *ctx, const char *shape, const char *src, const char *root,
                                       unsigned root_whose, char *ident, JSValue example, bool whole);

static JSValue concolic_derived(JSContext *ctx, const char *shape, const char *src, const char *root,
                                unsigned root_whose, char *ident, JSValue example)
{
    return concolic_derived_shaped(ctx, shape, src, root, root_whose, ident, example, false);
}

static JSValue concolic_derived_shaped(JSContext *ctx, const char *shape, const char *src, const char *root,
                                       unsigned root_whose, char *ident, JSValue example, bool whole)
{
    /* A candidate run substitutes one source with a breakout. Checked here, at the one place every source mint
       goes through, so a source installed as a plain property value (location.hash, document.cookie) is
       substituted too. */
    if (deliv_find(src) >= 0) {   /* a further delivery, before the search's bytes: it carries them inside */
        JS_FreeValue(ctx, example);
        free(ident);
        return deliver_structured(ctx, deliv_find(src));
    }
    if (cand_matches(src)) {
        /* `example` is owned by this call whichever value comes back, and the payload replaces it: a source
           under substitution reads the attacker's bytes, not what the address held. */
        JS_FreeValue(ctx, example);
        free(ident);
        return concolic_deliver(ctx, src, root, g_cand_bytes);
    }
    return concolic_alloc_shaped(ctx, shape, src, root, root_whose, ident, example, whole);
}

/* A source read, the root of every identity. Its identity is its provenance, because nothing derived it. A
   source with no provenance has no identity either, which keeps both arms of every branch over it. */
JSValue concolic_new(JSContext *ctx, const char *shape, const char *src, ConcolicRootWhose whose,
                     JSValue example) {
    const char *f[1];

    /* A source's shape names a hole, in braces, asserted at the one mint every source goes through.
       concolic_hole_key is the only route from a shape to a domain and reads a hole by its braces, so an
       unbraced source would have every gate observed and dropped (no subject, no bound, nothing for endpoint.c
       to look up), and its parameter would report provenance with no domain. endpoint.c cannot use a laxer
       rule: kv_pairs hands concolic_hole_key arbitrary query-value substrings, so `lang=en-US` would mint a
       hole. It also leaves the unnameable `{}` as the one meaning of a NULL subject. */
    DCHECK(shape != NULL && strchr(shape, '{') != NULL,
           "a SOURCE was minted with a display shape that names no hole — a shape carries its provenance in "
           "braces, because concolic_hole_key reads a hole by them and it is the only path from a shape to a "
           "domain. Without one this value's every gate is observed and discarded, and its parameter reports "
           "provenance with no constraint. Spell the shape as the src in braces (core/frame/location.h is the "
           "pattern); a shape that is not simply `{src}` is fine as long as it names its hole");
    /* Concretize-on-pin, at the mint where `src` is the value's own identity, which is what makes it one of the
       two places a pin may be read: a later read of a pinned source returns the pinned bytes, so a later branch
       on it is decided by running the real predicate. concolic_derived must not ask, because an arithmetic or
       builtin result carries its operand's `src`, and `+location.hash` would get the operand's string. An
       injected-state member is minted at every read, so without this the arm that proved
       `__FLAGS.role === "admin"` would keep composing `/api/user` from the logged-out example. The other read
       site is concolic_example, for a value the page already holds, under the same `src_self` precondition.
       After the candidate test: a re-fire delivering the attacker's bytes at this source must not be shadowed
       by a pin. */
    if (!src_delivered(src)) {
        JSValue pv = src ? pin_of(ctx, src) : JS_UNINITIALIZED;
        if (!JS_IsUninitialized(pv)) {
            JS_FreeValue(ctx, example);
            return pv;
        }
    }
    /* The tie `concolic_exotic_get`'s pin arm asserts. A candidate re-fire skips the arm deliberately, so a
       pinned source then reaches the mint and concolic_derived returns the attacker's bytes. */
    DCHECK(src_delivered(src) || !src || !src_pinned_unpriced(src),
           "a source this flow had PINNED reached the ordinary source mint on a path no candidate is "
           "substituting — `pin_of` answered NO PIN for a path `concolic_src_pinned` says is pinned, so the "
           "two tests over one chain have diverged and this read is about to mint an unknown for a value the "
           "flow has already determined");
    /* The pin arm answers a bare primitive, so an address composed from a source re-read after its pin is not
       concolic and endpoint.h's bar reads it as concrete, while the same source held in a page variable stays
       concolic. Deleting the arm is not the fix: the primitive concretizes every later operation, not only
       branches, and decide.c's refinement is keyed by the predicate, so a later `x === "guest"` is still forked.
       `concrete` is the right class for such an address: the pin's bytes are a literal the page's own predicate
       spelled, which a parse can state (endpoint.h's ENDPOINT_WITNESS_CLASSES banner), so the work that banner
       poses is the solver keeping more values unknown. */
    /* Named residual. Not covered: a pin whose operand is concrete by this engine's design rather than spelled
       by the page (the principal, `location.origin`, is concrete for URL building), whose bytes no parse of the
       bundle states. Next diff: a mark at `literal_tok` separating an operand this engine models as concrete
       from one the page spelled, carried through `concolic_pin`'s `kind` pair rather than as a value class.
       How its absence shows: an emitted address resting on a pin whose bytes occur in no script the run
       fetched. The population is unmeasured. */
    f[0] = src;
    /* A source read is its own root, stated once here rather than spelled by every component that owns a
       source. `whose`, by contrast, is a parameter because it cannot be derived here: `{orphan…}` and
       `{__FLAGS.role}` reach this line identically, and only the reading component knows which it is. */
    {
    /* Whose unknown this is, stated after both arms that answer a non-concolic (the pin above, the candidate
       substitution in concolic_derived), since a mark before them would have no record. It rides the root,
       `src` itself here, so the mask is empty exactly when the root is NULL. */
        JSValue r = concolic_derived(ctx, shape, src, src, src ? (1u << whose) : 0u,
                                     concolic_ident_compose("s", f, 1), example);
        /* …and its own identity, the precondition the pin arm above stands on, recorded as `src_self` so a seam
           that holds the value long after the mint (the CSS cascade, reading the DOM taint shadow) can ask. */
        pin_src_names_self(r);
#if APICLIENT_DEV
        /* …and that the write happened: `example_state_of`'s DETERMINED arm stands on `src_self`, and a source
           mint that did not set it would answer CONCOLIC_EX_NONE for a value this flow later pins. The writer
           returns void and both its guards are legitimate states, so the check is here, where the uncovered case
           is in hand. */
        {
            const Concolic *rc = concolic_is(r) ? JS_GetOpaque(r, g_concolic_class) : NULL;
            DCHECK(!rc || !rc->src || rc->src_self,
                   "a SOURCE read was minted carrying its own provenance and no claim that the provenance "
                   "NAMES it — `src_self` is what concolic_example's DETERMINED arm tests, so this value will "
                   "answer NONE for a pin taken over its own source and every address composed from it will "
                   "report a shape where this flow proved a literal");
        }
        /* …and that a non-concolic answer is one of the two determinations (a pin or a candidate substitution),
           the contract every consumer reads off the return; the two early returns are invisible in the
           signature, so a consumer must not assert that a source mint is always concolic. Stated over the whole
           function rather than this path, so it survives a reordering of the arms. */
        DCHECK(concolic_is(r) || src_delivered(src) || src_pinned_unpriced(src),
               "the source mint answered a value that is not concolic for a source this flow has NEITHER "
               "pinned NOR substituted a candidate at — the two early returns above are the only arms "
               "entitled to answer a plain value, so this is a derivation that lost the triple and every "
               "branch over it is about to be DECIDED where both worlds are still open");
#endif
        return r;
    }
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

/* A property over the whole root set (concolic.h has the two questions). The walk is paid at the join:
 * `derived_root_whose` unions the operands' masks wherever `derived_root_join` unions their names, so this is a
 * field read, and whose each member is is never recovered from its name.
 *
 * `_all` is not vacuously true on the empty set: a value with no root has mask 0, and `0 == (1u << whose)` is
 * false, which is the only sound answer. Spelled as equality with the single bit, because "no other bit set" is
 * true of the empty mask. */
int concolic_root_whose_any(JSValueConst v, ConcolicRootWhose whose) {
    DCHECK(whose >= 0 && whose < CONCOLIC_WHOSE_COUNT,
           "a property over a value's root set was asked about a provenance that is not a member of "
           "concolic.h's CONCOLIC_ROOT_WHOSE — the mask carries one bit per member, so a question outside the "
           "list reads a bit no mint can ever set and answers NO for every value in the program");
    return (root_whose_of(v) & (1u << whose)) ? 1 : 0;
}

int concolic_root_whose_all(JSValueConst v, ConcolicRootWhose whose) {
    DCHECK(whose >= 0 && whose < CONCOLIC_WHOSE_COUNT,
           "a property over a value's root set was asked about a provenance that is not a member of "
           "concolic.h's CONCOLIC_ROOT_WHOSE — the mask carries one bit per member, so a question outside the "
           "list reads a bit no mint can ever set and answers NO for every value in the program");
    return root_whose_of(v) == (1u << whose) ? 1 : 0;
}

const char *concolic_root_c(JSValueConst v) {
    Concolic *c = g_concolic_class ? JS_GetOpaque(v, g_concolic_class) : NULL;
    return c ? c->root : NULL;
}

int concolic_root_every(JSValueConst v, int (*member)(const char *p, size_t n)) {
    const char *root = concolic_root_c(v), *p;
    size_t n;
    int i;

    DCHECK(member != NULL, "a property over a value's root members was asked with no property");
    if (!root) return 0;
    for (i = 0; root_member(root, i, &p, &n); i++)
        if (!member(p, n)) return 0;
    return 1;
}

const char *concolic_ident_c(JSValueConst v) {
    Concolic *c = g_concolic_class ? JS_GetOpaque(v, g_concolic_class) : NULL;
    return c ? c->ident : NULL;
}

/* The predicate a branch over this value asks about (see the field). The fallback to the value's own identity
   is the answer, not a default: every non-negation is the predicate a branch over it tests. */
const char *concolic_branch_ident_c(JSValueConst v) {
    Concolic *c = g_concolic_class ? JS_GetOpaque(v, g_concolic_class) : NULL;
    if (!c) return NULL;
    DCHECK(c->br_key == NULL || c->ident != NULL,
           "a value names another predicate as its branch key while having no identity of its own — the two "
           "are composed from the same operand at one mint, so one present without the other is a negation "
           "whose own derivations would collide with something else's");
    return c->br_key ? c->br_key : c->ident;
}

/* …and the polarity: 0 = this value is that predicate, 1 = its complement. Applied to the arm, never to the
   key, so `if (p)` and `if (!p)` are one constraint entry with two answers. */
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


/* The example this flow may still believe: where the forced sibling drops a contradicted example, so only
 * gate-dependent values degrade to a shape while gate-independent ones stay concrete.
 *
 * What is dropped is the value the branch tested and nothing behind it: `if (cfg.admin)` over a loaded `false`,
 * taken true, says nothing about what `cfg.admin` was computed from. So the fact is keyed by the value's
 * `ident`, not its `src`, which a derived value shares with its operand. It is checked here, not at the mint,
 * because a value built before the branch has an example correct for both arms; a value built after reads
 * through here on the contradicting arm and gets none. A value this engine cannot spell keeps its example:
 * there is nowhere to file a per-flow fact (its arm is still marked FORCED).
 *
 * A proof outranks an observation, so concretize-on-pin is asked first: a value materialised into a page
 * variable before its gate (`var role = q("role"); if (role === "admin") fetch("…?role=" + role)`) is never
 * re-minted. This is a pin answering the example, not an example becoming a pin: the value stays concolic and
 * every later branch still forks. The pin must be this flow's and about this value (`src_self`), as at
 * concolic_pin_bytes. Where nothing is pinned the cost is two predicates (`g_pin_any`, `src_self`). */
/* Which of the four the accessor will answer: one decision, read by `concolic_example` to mint and by
   `concolic_example_state` to report, so the value and the fact about it cannot disagree (concolic.h says
   what each answer obliges). The pin entry is handed back from the same `cons_lookup` that decided DETERMINED.
   What this flow has proved is asked before what it was handed. No candidate guard is needed: a substituted
   source is never a concolic (concolic_deliver returns plain bytes), so there is no record to ask about.
   `*ppin` is borrowed from the constraint chain and dead at the next `cons_entry` (the head grows by realloc);
   the one caller mints from it on the next line, and pin_mint writes no constraint and runs no page code. */
static ConcolicExState example_state_of(const Concolic *c, const Cons **ppin)
{
    *ppin = NULL;
    if (!c) return CONCOLIC_EX_NONE;
    if (g_pin_any && c->src_self && c->src) {
        const Cons *p = cons_lookup(c->src);
        /* `val` is the determination and `ppin` is not: concolic_pin files a loose equality's holding
           arm without writing a value, so an entry is often present with nothing pinned (`pin_of`'s test). */
        if (p && p->val) { *ppin = p; return CONCOLIC_EX_DETERMINED; }
    }
    if (!c->ex_has) return CONCOLIC_EX_NONE;
    if (g_ex_contra_any && c->ident) {
        const Cons *e = cons_lookup(c->ident);
        if (e && e->ex_contra) return CONCOLIC_EX_CONTRADICTED;
    }
    return CONCOLIC_EX_HELD;
}

void concolic_hold_undefined(JSValueConst v) {
    Concolic *c = g_concolic_class ? JS_GetOpaque(v, g_concolic_class) : NULL;

    DCHECK(c != NULL, "an undefined example was held on a value that is not a concolic");
    DCHECK(c != NULL && !c->ex_has && JS_IsUndefined(c->example),
           "an undefined example was held on a source that already holds one — the absent mint is the one "
           "caller and mints with none, so a held example here is a second producer stating what one read "
           "answered");
    if (!c) return;
    c->ex_has = 1;   /* `example` is already JS_UNDEFINED: the presence bit is the whole write */
}

JSValue concolic_example(JSContext *ctx, JSValueConst v) {
    Concolic *c = g_concolic_class ? JS_GetOpaque(v, g_concolic_class) : NULL;
    const Cons *pin;

    switch (example_state_of(c, &pin)) {
    /* The two dereferencing arms are asserted across the seam: their operands come from example_state_of, so
       an edit there answering DETERMINED with no entry, or HELD with no record, aborts here naming the arm. */
    case CONCOLIC_EX_DETERMINED:
        DCHECK(pin != NULL,
               "the example state answered DETERMINED and handed back no constraint entry — the pin's bytes "
               "and the claim that there is one come out of ONE lookup precisely so the mint below cannot be "
               "asked to spell a determination nobody found");
        return pin_mint(ctx, (ConcolicLit)pin->valkind, pin->val);
    case CONCOLIC_EX_HELD:
        DCHECK(c != NULL,
               "the example state answered HELD for a value that carries no concolic record — HELD is a claim "
               "about a record's own `example` field, so an operand with no record can only be NONE and this "
               "arm would be reading one that does not exist");
        return JS_DupValue(ctx, c->example);
    /* No bytes to show. NONE (nothing was computed) and CONTRADICTED (a gate on this path disproved it) owe a
       caller different work, which is why concolic_example_state exists; absence is the sound value for both. */
    case CONCOLIC_EX_NONE:
    case CONCOLIC_EX_CONTRADICTED:
        break;
    }
    return JS_UNDEFINED;
}

/* …and the same decision, reported (concolic.h has the four answers and what each obliges). */
ConcolicExState concolic_example_state(JSValueConst v) {
    const Concolic *c = g_concolic_class ? JS_GetOpaque(v, g_concolic_class) : NULL;
    const Cons *pin;

    return example_state_of(c, &pin);
}

/* This flow took an arm the value's own example says a real session does not take (see the accessor above for
   the cost, and decide.c for who says so). It shares the map with the pins but is keyed by a
   concolic_ident_compose output rather than a plain source path. The two are disjoint in every spelling this
   engine produces, though only by encoding, not by a byte a path cannot contain; a collision would set
   `ex_contra` on another name's entry, degrading that value to a shape, never fabricating one, and never
   touching `val`, `excl` or `bnd`. Idempotent: a path cannot un-take an arm. */
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
    DCHECK(!c->ty || JS_IsUndefined(example) || (cty_of_concrete(example) & c->ty),
           "an example of a type outside the value's stated type set was attached after its mint — the mint's "
           "algorithm fixes the type in every world, this run's world among them");
    JS_FreeValue(ctx, c->example);
    c->example = example;   /* consume */
    c->ex_has = !JS_IsUndefined(example);
}

static char *cstr_dup(JSContext *ctx, JSValueConst v) {   /* concrete operand -> its string form (heap copy) */
    const char *s = JS_ToCString(ctx, v);
    char *r = strdup(s ? s : "");
    if (s) JS_FreeCString(ctx, s);
    return r;
}

/* §13.15.3 ApplyStringOrNumericBinaryOperator step 1.c's conclusion, run on the two examples: "Let leftString be
   ? ToString(leftPrimitive). Let rightString be ? ToString(rightPrimitive). Return the string-concatenation of
   leftString and rightString." `exa`/`exb` are borrowed; the result is new and owned, or JS_UNDEFINED for "no
   example". */
static JSValue example_string_concat(JSContext *ctx, JSValueConst exa, JSValueConst exb) {
    const char *pa, *pb;
    JSValue example = JS_UNDEFINED;

    pa = JS_ToCString(ctx, exa);
    pb = pa ? JS_ToCString(ctx, exb) : NULL;
    /* A NULL with the refusal edge underneath is the physical floor, so it is a CHECK: a `+` that silently
       produced no example would turn a computed value (`/api/us-east-1`) into a `{a}{b}` shape. */
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
    /* A refused conversion left its throw standing while the operator is about to report success. §7.1.19
       ToString ( arg ) refuses only a Symbol or an object whose ToPrimitive threw, and the caller asserts
       neither, so in dev this drains nothing and in release it drains what a compiled-out assert stopped
       saying. */
    if (JS_IsUndefined(example) && JS_HasException(ctx))
        JS_FreeValue(ctx, JS_GetException(ctx));
    return example;
}

/* §13.15.3 steps 2-7 over the two examples, once step 1.c has said neither is a String: step 2's "NOTE: At
   this point, it must be a numeric operation", "Let leftNumber be ? ToNumeric(leftValue)" and the same for the
   right (§7.1.3 ToNumeric ( arg ), which is §7.1.4 ToNumber ( arg ) for everything but a BigInt), then step 7's
   table entry for `+`, §6.1.6.1.7 Number::add ( x, y ). Number::add is IEEE 754 binary64 addition, so the
   engine runs the real operation. Borrowed operands; owned result, or JS_UNDEFINED for "no example". */
static JSValue example_number_add(JSContext *ctx, JSValueConst exa, JSValueConst exb) {
    double da = 0, db = 0;
    int oka = JS_ToFloat64(ctx, &da, exa) == 0;
    int okb = oka && JS_ToFloat64(ctx, &db, exb) == 0;

    /* §7.1.4 ToNumber step 2 ("If arg is either a Symbol or a BigInt, throw a TypeError exception") is the only
       refusal a primitive can reach, and no producer mints either as an example. A BigInt arm (§13.15.3 step
       5's SameType test, step 6's §6.1.6.2.7 BigInt::add ( x, y )) is built when a producer does. */
    DCHECK(oka && okb,
           "a concolic's example refused §13.15.3 step 3/4's §7.1.3 ToNumeric — its only primitive refusals "
           "are §7.1.4 ToNumber step 2's Symbol and BigInt, and no producer in this engine mints either as an "
           "example; build §13.15.3 step 5's SameType test and step 6's §6.1.6.2.7 BigInt::add here when one "
           "does");
    if (!oka || !okb) {
        /* The example is dropped and the throw with it: the program's `+` is over the unknown, not over this
           engine's concrete example, so the coercion that threw is not one the program performed. */
        DCHECK(JS_HasException(ctx),
               "§7.1.3 ToNumeric reported a refusal and left no exception standing — the drain below would "
               "then take the NEXT operator's throw instead of this one's");
        JS_FreeValue(ctx, JS_GetException(ctx));
        return JS_UNDEFINED;
    }
    return JS_NewFloat64(ctx, da + db);
}

/* A concatenation where either operand is concolic yields a derived concolic. `op` names which spec algorithm
   is concatenating (JSConcolicAddOp in quickjs.h); matches js_add_slow's stack effect (both operands freed,
   result in sp[-2]).
   The shape is display(a)++display(b) for both arms, a function of the operand shapes alone, because .key_name
   spells an unknown property key with it: `obj[x+1] = v` must write the slot `obj[x+1]` later reads, whether
   or not an example is known in that flow.
   The example applies §13.15.3's step 1.c test to the examples, which are concrete values this engine
   computed, so `concolic(5) + 3` gives 8, not "53". Steps 1.a and 1.b are not performed here (js_add_slow
   calls this before any coercion); the assert inside names the trampoline that owes them. The concolic result
   is the same either way: provenance, shape and identity, and a later branch still forks. */
/* `v`'s hole spans shifted by `off`, written to `out` when it is not NULL; answers how many. A non-concolic
   contributes none, and a value with no recorded spans is one unknown spelled by its whole shape. */
static int hole_spans_of(JSValueConst v, size_t off, int *out)
{
    const Concolic *c = g_concolic_class ? JS_GetOpaque(v, g_concolic_class) : NULL;
    int i;

    if (!c) return 0;
    if (!c->hole_span) {
        if (out) { out[0] = (int)off; out[1] = (int)(off + strlen(c->shape)); }
        return 1;
    }
    if (out) for (i = 0; i < 2 * c->hole_n; i++) out[i] = c->hole_span[i] + (int)off;
    return c->hole_n;
}

/* `r` is the string concatenation of `a`'s shape (`alen` bytes) and `b`'s: its holes are theirs, in order. */
static void hole_spans_concat(JSValueConst r, JSValueConst a, size_t alen, JSValueConst b)
{
    Concolic *c = g_concolic_class ? JS_GetOpaque(r, g_concolic_class) : NULL;
    int na, nb;

    if (!c) return;   /* a candidate re-fire delivered the attacker's bytes, a plain String */
    na = hole_spans_of(a, 0, NULL);
    nb = hole_spans_of(b, alen, NULL);
    DCHECK(na + nb >= 1 && c->hole_span == NULL,
           "a concatenation over an unknown composed no hole, or composed its holes twice — one operand is a "
           "concolic, so its shape is at least one hole of the result's");
    c->hole_span = reclaim_malloc((size_t)(2 * (na + nb)) * sizeof *c->hole_span);
    CHECK(c->hole_span, "concolic: OOM recording which bytes of a shape spell an unknown");
    hole_spans_of(a, 0, c->hole_span);
    hole_spans_of(b, alen, c->hole_span + 2 * na);
    c->hole_n = na + nb;
    DCHECK((size_t)c->hole_span[2 * c->hole_n - 1] <= strlen(c->shape),
           "a concatenation's last hole ends past its own shape — the operands' shapes are the result's bytes");
}

unsigned char *concolic_shape_hole_mask(JSValueConst v)
{
    const Concolic *c = g_concolic_class ? JS_GetOpaque(v, g_concolic_class) : NULL;
    size_t n;
    unsigned char *m;
    int i;

    if (!c) return NULL;
    n = strlen(c->shape);
    m = calloc(n + 1, 1);
    CHECK(m, "concolic: OOM marking which bytes of a shape spell an unknown");
    if (!c->hole_span) { memset(m, 1, n); return m; }
    for (i = 0; i < c->hole_n; i++) {
        int s = c->hole_span[2 * i], e = c->hole_span[2 * i + 1];

        DCHECK(0 <= s && s <= e && (size_t)e <= n && (i == 0 || c->hole_span[2 * i - 1] <= s),
               "a shape's hole spans are out of order or outside the shape — they are composed in operand "
               "order by hole_spans_concat and nothing else writes them");
        memset(m + s, 1, (size_t)(e - s));
    }
    return m;
}

int concolic_add_hook(JSContext *ctx, JSValue *sp, JSConcolicAddOp op) {
    JSValue a = sp[-2], b = sp[-1];
    int ca = concolic_is(a), cb = concolic_is(b);
    DCHECK(op == JS_CONCOLIC_ADD_PLUS || op == JS_CONCOLIC_ADD_CONCAT,
           "a concatenation reached the concolic derivation without naming which spec algorithm it is — "
           "13.15.3 and 22.1.3.5 disagree about the numeric arm, so an unnamed caller has no answer here");
    if (!ca && !cb) return 0;

    /* The operands as a concatenation renders them: a String operand's own bytes, unquoted. */
    char *sha = ca ? strdup(concolic_shape_c(a) ? concolic_shape_c(a) : "{}") : cstr_dup(ctx, a);
    char *shb = cb ? strdup(concolic_shape_c(b) ? concolic_shape_c(b) : "{}") : cstr_dup(ctx, b);
    CHECK(sha && shb, "concolic +: OOM shape");
    char *shape;
    const char *src = ca ? concolic_src_c(a) : concolic_src_c(b);
    JSValueConst ops[2];
    char *root;
    /* The set of the operands' roots (derived_root_join); `src` stays the first unknown operand's, a different
       fact. */
    ops[0] = a;
    ops[1] = b;
    root = derived_root_join(ops, 2);

    JSValue exa = ca ? concolic_example(ctx, a) : JS_DupValue(ctx, a);
    JSValue exb = cb ? concolic_example(ctx, b) : JS_DupValue(ctx, b);
    /* A concrete operand always has a value, including `undefined`, as in cmp_example: `"a" + undefined` is
       `"aundefined"` and `1 + undefined` is NaN, so absence is asked of the concolic side only. A concolic
       operand's structured example is absent here too: steps 1.a and 1.b would run §7.1.1 ToPrimitive over it,
       which is the page's code (example_operable). */
    int hava = ca ? concolic_example_present(a) && example_operable(exa) : 1;
    int havb = cb ? concolic_example_present(b) && example_operable(exb) : 1;
    JSValue example = JS_UNDEFINED;
    /* A concrete operand is a primitive by the time it reaches here: §7.1.1 over a raw Object runs the page's
       valueOf/toString, which belongs on the ToPrimitive trampoline (js_toprim_operand / do_toprim_tramp)
       before the arm is chosen — `x + {valueOf(){return 5}}` is an addition. */
    DCHECK((ca || example_operable(exa)) && (cb || example_operable(exb)),
           "§13.15.3 steps 1.a and 1.b ToPrimitive both operands first, and a CONCRETE operand reached the "
           "concatenation hook unconverted — it belongs on the ToPrimitive trampoline before the arm is chosen");
    if (hava && havb) {
        /* §13.15.3 step 1.c: "If leftPrimitive is a String or rightPrimitive is a String". §22.1.3.5
           String.prototype.concat ( ...args ) asks no such question (its pieces are already ToString'd), so it
           takes the string arm. */
        if (op == JS_CONCOLIC_ADD_CONCAT || JS_IsString(exa) || JS_IsString(exb))
            example = example_string_concat(ctx, exa, exb);
        else
            example = example_number_add(ctx, exa, exb);
    }
    /* Step 1.c is a disjunction, so one String operand decides the string arm whether or not the other has an
       example; the result's shape is then the page's bytes around its operands' holes. */
    /* Whether the string arm is taken in every world, which is what the result's type states: §22.1.3.5's, or
       §13.15.3 step 1.c with an operand that is a String in every world. An example decides the arm for this
       run's shape and not the type in the others. */
    unsigned ta = cty_of_operand(a), tb = cty_of_operand(b);
    int string_fixed = op == JS_CONCOLIC_ADD_CONCAT || ta == CTY_STRING || tb == CTY_STRING;
    int string_arm = string_fixed || (hava && JS_IsString(exa)) || (havb && JS_IsString(exb));
    JS_FreeValue(ctx, exa); JS_FreeValue(ctx, exb);
    /* The shape, by the arm. On the string arm it is the string the concatenation produced, so
       `"/api/" + cfg.region` renders `/api/{cfg.region}`, the @H provenance the endpoint surface is built from
       (quoting would spell `"/api/"{cfg.region}`). Elsewhere (§13.15.3's numeric arm, or an arm no example
       decides) it is the expression, through derived_operand_shape like every other operator, so a sum
       `0 + {a} + {b}` keeps its operators and `x + 5` stays apart from `x + "5"`. */
    if (string_arm) {
        size_t ln = strlen(sha) + strlen(shb) + 1;

        shape = reclaim_malloc(ln);
        CHECK(shape, "concolic +: OOM shape concat");
        snprintf(shape, ln, "%s%s", sha, shb);
    } else {
        char *ea = derived_operand_shape(ctx, a), *eb = derived_operand_shape(ctx, b);

        shape = shapef("%s+%s", ea, eb);
        free(ea); free(eb);
    }

    char *ident;
    { const char *f[2]; char *ia = ident_of_operand(ctx, a), *ib = ident_of_operand(ctx, b);
      f[0] = ia; f[1] = ib; ident = concolic_ident_compose("+", f, 2); free(ia); free(ib); }

    /* §13.15.3's numeric arm over two values with affine forms (aff_operand admits Numbers only, so step 1.c's
       String test is false for both): the forms add, and a sum whose terms cancel is that constant. */
    ConcolicAffine *af = NULL;
    double konst = 0.0;
    int got = -1;
    {
        AffOperand oa, ob;

        if (op == JS_CONCOLIC_ADD_PLUS && aff_operand(ctx, a, &oa) && aff_operand(ctx, b, &ob))
            got = aff_lin(&oa, 1.0, &ob, 1.0, &af, &konst);
    }
    if (got == 0) {
        JSValue k = aff_constant(ctx, konst, example, "+");

        JS_FreeValue(ctx, example);
        free(ident); free(sha); free(shb); free(shape); free(root);
        JS_FreeValue(ctx, a); JS_FreeValue(ctx, b);
        sp[-2] = k;
        return 1;
    }
    if (got == 1) { free(ident); ident = aff_ident(af); }

    JSValue result = concolic_derived_shaped(ctx, shape, src, root, derived_root_whose(ops, 2), ident, example,
                                             string_arm);   /* consumes example and ident */
    /* A sum or concatenation is never the source it inherits `src` from, though `x + ""` spells exactly x's
       shape, which concolic_alloc reads as owning: an equality over it must not pin or narrow x. */
    if (concolic_is(result)) ((Concolic *)JS_GetOpaque(result, g_concolic_class))->src_own = 0;
    if (af) aff_attach(result, af);
    if (string_arm) hole_spans_concat(result, a, strlen(sha), b);
    /* §13.15.3 answers a String, a Number or a BigInt; the numeric arm is a Number unless both operands may be
       BigInts (carith_result_types's reading of step 4), and is reached only when neither may be a String. */
    {
        int a_str = !ta || (ta & (CTY_STRING | CTY_OBJECT)), b_str = !tb || (tb & (CTY_STRING | CTY_OBJECT));
        int a_big = !ta || (ta & (CTY_BIGINT | CTY_OBJECT)), b_big = !tb || (tb & (CTY_BIGINT | CTY_OBJECT));
        unsigned numeric = (a_big && b_big) ? CTY_NUMERIC : CTY_NUMBER;

        concolic_type_state(result, string_fixed ? CTY_STRING : (a_str || b_str) ? (CTY_STRING | numeric) : numeric);
    }
    /* `x + ""` and `"" + x`: §13.15.3 step 1.c returns ToString(ToPrimitive(x)) concatenated with nothing, so
       the result is the string form of the one unknown operand. Recorded through a chain, so `(x + "") + ""`
       names `x` too. */
    if (string_arm && ca != cb) {
        JSValueConst u = ca ? a : b, k = ca ? b : a;

        if (JS_IsString(k)) {
            size_t kn = 0;
            const char *ks = JS_ToCStringLen(ctx, &kn, k);

            CHECK(ks, "concolic +: OOM reading the concrete operand of a concatenation");
            if (kn == 0) str_of_record(result, u);
            JS_FreeCString(ctx, ks);
        }
    }
    free(sha); free(shb); free(shape); free(root);
    JS_FreeValue(ctx, a); JS_FreeValue(ctx, b);
    sp[-2] = result;
    return 1;
}

/* The hook tables are two sets because they answer two questions. What a concolic value does once it exists
   (how it adds, compares, coerces, reports its type) is the value class's semantics and is installed wherever
   a concolic can be reached; without it `"x" + document.cookie` throws "toPrimitive" from an expression the
   page never wrote. Where a concolic comes from is an exploration choice: absent_read_hook mints one out of an
   unset global or a field the record does not hold, while a conformance run wants a ReferenceError and
   `undefined`. So a host can take the first without the second. */
/* The compile-time global-name report has two consumers, listed here where the table is assembled (this file
   already points the table at solver/absent.c's `.absent`, `.present` and `.publish`). solver/endpoint.c grades
   it against a door's ask rows, which endpoint_init resets; solver/rung_entry.c against an arm of
   `stepUnitRuns`, which nothing resets. Neither may forward to the other, and their order means nothing: they
   report over disjoint name tables.
   Exactly one answers the engine: QuickJS orders its orphan candidates (JS_OrphanTakeOne) by whether driving a
   body could reach a network door, which endpoint.c's edges answer; a rung's entry (`requestAnimationFrame`,
   `setTimeout`, `requestIdleCallback`) composes no address. So it is not `a || b`: OR-ing would prefer every
   body that touches a timer, most of a real bundle, and the order would be the heap order again. */
/* Which names denote this realm's own global object: the one statement of it on the consumer side of
   `.global_member_named`, held here because this file owns the table and both consumers are reached through it
   (testing/static_surface.mjs's `GLOBAL_OBJECTS` is a separate set over a corpus, per rung_entry.c's residual).
   A source-text test by necessity: quickjs.h's hook takes no `JSContext`, because what is reported is a fact
   about source, so the realm cannot be asked; the residual's next diff has the installers lend the set
   (browser/core/frame/window.c binds three of these, the interpreter's intrinsics the fourth). `parent` and `top`
   are absent: HTML §7.2.2 The Window object's `parent` and `top` are the navigable's and may denote another
   window, and excluding them can only cost a consumer a miss. */
static const char *const GLOBAL_SELF_NAMES[] = { "window", "self", "globalThis", "frames", NULL };

static int base_is_global(const char *base) {
    for (int i = 0; GLOBAL_SELF_NAMES[i]; i++)
        if (strcmp(GLOBAL_SELF_NAMES[i], base) == 0) return 1;
    return 0;
}

/* `<free identifier>.<member>` where the identifier denotes the global: the receiver test is made once, here,
   so neither consumer judges a `base`. A wrapper's own member, a bundler's `(0,o.requestIdleCallback)` shim and
   an `api.fetch` each read a property of a receiver that is not the global, and would raise a denominator
   neither census owes. Only the endpoint consumer answers the engine, for the reason above. */
static int compile_global_member_dispatch(const char *base, const char *member) {
    int net;

    if (!base || !member) return 0;
    if (!base_is_global(base)) return 0;
    net = endpoint_compile_global_member(member);
    rung_entry_compile_global_member(member);
    return net;
}

static int compile_global_named_dispatch(const char *name, int typeof_only) {
    int net = endpoint_compile_global_named(name, typeof_only);

    rung_entry_compile_global_named(name, typeof_only);
    return net;
}

static JSConcolicHooks g_hooks = {
    .add = concolic_add_hook, .cmp = concolic_cmp_hook, .is = concolic_is,
    .rel = concolic_rel_hook, .type_of = concolic_typeof_hook, .is_array = concolic_isarray_hook,
    .json_value = concolic_mark_json_value, .member_never_callable = concolic_member_never_callable,
    .has_property = concolic_has_property_hook,
    .arith = concolic_arith_hook, .to_str = concolic_tostr_hook,
    .to_bool = concolic_tobool_hook,
    .key_read = concolic_key_read_hook,
    .get_value_base = concolic_get_value_base_hook,
    .key_name = concolic_key_name_hook,
    /* …and the inverse, which is not optional beside it: `key_name` is the only mechanism turning an unknown
       into bytes, so a host without this would hand an enumerating page a real string where its model says an
       unknown stands, and the taint that makes the sink behind it visible would be gone. */
    .key_value = concolic_key_value_hook,
    .builtin = concolic_builtin_hook,
    .example = concolic_example,
    .call = concolic_call_hook,
    .derived = concolic_new_derived,
    .example_held = concolic_example_present,
    /* The ordinal half of a page-created value's name. With the member NULL the engine stamps 0,
       JS_CreationName answers absent for every value, and literal_ident's creation-name arm returns NULL: a host
       declining the edge, byte-identical to a build without the seam. */
    .mint_ordinal = concolic_mint_ordinal_hook,
    .random = concolic_random_hook,
    .clock = hr_time_wall_now,
    /* §10.1.11 [[OwnPropertyKeys]] ( ) asked: the engine's step_ownkeys_run asks this on the branch seam and
       the record hands its enumeration to decide_value_arm. With the member NULL, decide_value_arm answers -1
       for ever and concolic_exotic_own_names crashes naming a consumer that exists. Its own member, not
       `.builtin`: this predicate composes ("[[OwnPropertyKeys]]", {operand}) while the builtin hook composes
       ("b", {operand, op}), which would file one question under a second key, and `.builtin` carries src/root,
       which this predicate must not. */
    .own_keys_pred = concolic_own_keys_pred,
    /* …and its true arm, not optional beside it: a host that asks whether the record holds a member and cannot
       perform the answer would decide one exists and enumerate nothing. The engine says so at its own site. */
    .own_key_mint = concolic_own_key_mint,
    /* What the compiler saw: the one member here not about a value, installed with the value set because it
       decides nothing. `.absent`, `.present` and `.publish` (installed with the source overlay) are a decision
       a conformance host must be able to decline; this is a void report of a fact about source text, raised
       once per free identifier resolved against the global object, changing no arm. It is what lets the
       engine say a door was never asked because no flow reached a call the program plainly spells. Routed to
       the consumers (solver/endpoint.c, solver/rung_entry.c), which know which identifiers are door entries. */
    .global_named = compile_global_named_dispatch,
    /* …and the property spelling of the same fact, through the dispatch above, which applies the global-
       receiver test. Whether a bundle spells a door's entry name only as a property is a property of real
       bundles, measured over a mirrored corpus by `node testing/static_surface.mjs` (its PROP-ONLY column),
       not argued here. It decides nothing, like its sibling. */
    .global_member_named = compile_global_member_dispatch,
    .lead = concolic_lead_hook };

/* Concolic value propagation stays installed across scheduling and verification, because taint must flow
   during a candidate re-fire too; the exploration hooks (branch/fork/preempt) are the scheduler's. */
void concolic_install_hooks(void)
{
    JS_SetConcolicHooks(&g_hooks);
}

/* Whether this host explores. One statement, because every consequence is one decision: an unset global
   becomes unknown server-injected input rather than a ReferenceError, and a browser value the attacker
   controls becomes a source rather than the plain string the address computed. A host that does not explore
   gets the browser's own answers, as a conformance run checks. The value semantics are installed regardless;
   this decides only whether a source is minted.
   "Not declared yet" is a third state, not a default: a value minted once for a realm's lifetime (the
   Navigator members) keeps whichever answer stood when the realm's intrinsics ran, so a host must declare
   before it builds a realm, and a seam reached earlier crashes by name instead of silently deciding gates
   that should fork. */
enum { SOURCE_OVERLAY_UNDECLARED = 0, SOURCE_OVERLAY_BROWSER_ONLY, SOURCE_OVERLAY_EXPLORING };
static int g_source_overlay;

/* Whether an agent explores, and the hooks that answer it, are that agent's, but the engine's hook table is
   process-wide: a later agent with no solver would otherwise run with this one's hooks and mint sources into a
   runtime that never registered the class. concolic_free withdraws both; the next agent declares again. The
   static base table keeps its initializer for a later concolic_install_hooks. */
static void source_overlay_withdraw(void)
{
    static const JSConcolicHooks none;

    g_hooks.absent = NULL;
    g_hooks.absent_unresolved = NULL;
    g_hooks.present = NULL;
    g_hooks.publish = NULL;
    g_source_overlay = SOURCE_OVERLAY_UNDECLARED;
    JS_SetConcolicHooks(&none);
}
/* The same answer, for a component that mints a source of its own (see the header). Not folded into
   concolic_source_wrap, which also files the value in the attacker-delivery registry and counts it as attacker
   input: a data block (HTML §4.12.1 The script element) is neither, and counting it would report a page that
   read no attacker source as one that did. */
int concolic_is_exploring(void) { return g_source_overlay == SOURCE_OVERLAY_EXPLORING; }
/* …and whether the host has answered at all, which the answer cannot say. Read by core/realm.c at the one call
   every realm's intrinsics go through, the moment a lifetime member freezes the standing answer. */
int concolic_source_overlay_declared(void) { return g_source_overlay != SOURCE_OVERLAY_UNDECLARED; }

/* A host that wants the spec's own answers says so rather than getting them by silence. A conformance run
   reaches concolic values (location.c's two sources exist for any document) and needs the value semantics,
   but an unset global must stay a ReferenceError because the corpus tests it. Declaring makes "nobody has
   decided" distinguishable from "decided: no", which is what lets the assert below exist. */
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
    /* …and the spelling of that same miss that never performs a [[Get]], installed with it and never without
       it: `typeof X` on an unresolved name is answered at the opcode (§13.5.3.1 Runtime Semantics: Evaluation
       step 2.a, under §13.5.3 The typeof Operator), so `.absent` is never asked, and a census with only one
       would depend on which opcode the bundle used. It records and decides nothing. */
    g_hooks.absent_unresolved = absent_unresolved_note;
    /* …and the hit half of the same question, installed with it: a published record's members are unknown
       whether or not the record holds them, since the server chose its extent against this visitor's
       credentials, and answering one spelling symbolically and the other concretely would lose the surface the
       other half reports as reached. */
    g_hooks.present = absent_present_hook;
    /* The two ends of one channel, installed together: `.publish` marks the records a document injects and
       `.absent` is the only reader of those marks. */
    g_hooks.publish = absent_publish_hook;
    JS_SetConcolicHooks(&g_hooks);
}

/* The one seam between a value the browser computed and the solver's view of it. The browser half computes
   what the spec says the member is (`location.search` is the address's query); the solver half decides an
   attacker controls it, so it is also a symbolic source that forks control flow. One call, so a component
   need not mint a concolic itself, which a non-exploring host could not use. `computed` is consumed and
   becomes the source's example: opaque for control flow, still knowing its concrete value. */
/* A declared source's two halves must agree, asserted at the mint: its provenance (`location.hash`, what an
   @S record names and concolic_declare_source registers) and its display shape (`{location.hash}`, what the
   @H surface prints) are the first in braces, for every declaring component (location's two, document.cookie,
   document.referrer, file_system's `{file:NAME}`). A consumer spelling the source another way reads a
   mechanism as broken. Scoped to declared sources: an undeclared one may carry a shape that is not its name
   (`{hidden|visible}` is a domain, `navigator.userAgent` a member path), since a shape states what the value
   can be. */
JSValue concolic_source_wrap(JSContext *ctx, const char *shape, const char *src, JSValue computed)
{
    /* The host must have answered before this seam is asked: the answer decides whether the value forks, and a
       caller minting for a realm's lifetime cannot ask again (see the three states above). */
    DCHECK(g_source_overlay != SOURCE_OVERLAY_UNDECLARED,
           "a browser component minted an attacker source before this host said whether it EXPLORES — the "
           "answer decides whether the value forks control flow, so a mint taken here silently gets the "
           "browser-only one. The host declares once, before it builds its agent's first realm: "
           "concolic_install_source_overlay for a solver host, concolic_declare_browser_only for a "
           "conformance one");
    if (g_source_overlay != SOURCE_OVERLAY_EXPLORING)
        return computed;
    /* The one point at which this document's run acquires attacker-controlled input, counted there: every
       component owning an attacker source mints through this call. An empty @S surface has four readings
       with opposite actions: no attacker source read; read but nothing tainted reached a code-execution sink;
       reached one behind an unforgeable check, parked on a sender demand; or no sink ran. The last three are
       counted at the arrival (solve.c's detect_sink); this is the first. Counted after the overlay gate, so it
       counts values minted: a conformance host's run would otherwise report attacker input it never acquired. */
    METRIC_ADD(source_reads, 1);
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
    /* A declared attacker source is the world's: its component models a channel a person or a server drives,
       which no parse of the served bytes states. This seam is that population's one door. */
    return concolic_new(ctx, shape, src, CONCOLIC_WHOSE_WORLD, computed);
}

/* Join `parts` in the given `order` with the joint separator, so the composed string is a property of the set
   (the caller computes `order` with ident_set_order, and must use the same order for shape and key). Owned by
   the caller. */
static char *concolic_joint_join(const char *const *parts, const int *order, int n)
{
    size_t seplen = strlen(CONCOLIC_JOINT_SEP), len = 1, at = 0;
    char *out;
    int i;

    for (i = 0; i < n; i++) len += strlen(parts[order[i]]) + (i ? seplen : 0);
    /* Sized from the members, not a fixed buffer: a truncated identity would be two domains under one key. */
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

/* The joint shape and provenance of a fact set, composed in the set's canonical order (owned by the caller). */
static void joint_compose(const char *const *shapes, const char *const *srcs, int n, char **shape, char **src)
{
    int *order;
    int i;

    DCHECK(n >= 1 && shapes != NULL && srcs != NULL,
           "a joint domain was minted over NO members — a value derived from nothing is one the cascade and "
           "the layout determined, and its caller must hand back the computed value rather than ask for a "
           "domain over an empty set");
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
    *shape = concolic_joint_join(shapes, order, n);
    *src = concolic_joint_join(srcs, order, n);
    free(order);
}

JSValue concolic_source_wrap_joint(JSContext *ctx, const char *const *shapes, const char *const *srcs,
                                   int n, JSValue computed)
{
    char *shape, *src;
    JSValue out;

    if (!g_source_overlay)
        return computed;
    joint_compose(shapes, srcs, n, &shape, &src);
    out = concolic_source_wrap(ctx, shape, src, computed);
    free(shape); free(src);
    return out;
}

/* See concolic.h. The value is derived from the facts rather than read as one, so it is minted beside the
   source mint and not through it: a pin this flow holds on a fact names the fact, never this function of it. */
JSValue concolic_source_wrap_joint_affine(JSContext *ctx, const char *const *shapes, const char *const *srcs,
                                          int n, const char *term, double c, JSValue computed)
{
    ConcolicAffine *af;
    char *shape, *src;
    double ex = 0.0;
    JSValue out;

    DCHECK(g_source_overlay != SOURCE_OVERLAY_UNDECLARED,
           "a browser component minted an environment-derived value before this host said whether it "
           "EXPLORES — see concolic_source_wrap");
    if (g_source_overlay != SOURCE_OVERLAY_EXPLORING)
        return computed;
    DCHECK(term != NULL && JS_IsNumber(computed),
           "an affine environment value was minted with no term or with an example that is not a Number");
    JS_ToFloat64(ctx, &ex, computed);
    DCHECKF(c == trunc(c) && fabs(c) <= AFF_TERM_MAX / 2 && ex == trunc(ex) && fabs(ex) <= AFF_TERM_MAX / 2,
            "an affine environment value `%.17g + T` with example %.17g: the constant and the example must be "
            "integers within 2^31, so the term is an integer within 2^32 (AFF_TERM_MAX)", c, ex);
    METRIC_ADD(source_reads, 1);
    joint_compose(shapes, srcs, n, &shape, &src);
    af = reclaim_calloc(1, sizeof *af);
    CHECK(af, "concolic: OOM recording an affine form");
    af->n = 1;
    af->term = reclaim_malloc(sizeof *af->term);
    af->k = reclaim_malloc(sizeof *af->k);
    CHECK(af->term && af->k, "concolic: OOM recording an affine term");
    af->term[0] = strdup(term);
    CHECK(af->term[0], "concolic: OOM copying an affine term");
    af->k[0] = 1.0;
    af->c = c;
    out = concolic_derived(ctx, shape, src, src, 1u << CONCOLIC_WHOSE_WORLD, aff_ident(af), computed);
    aff_attach(out, af);
    free(shape); free(src);
    return out;
}
