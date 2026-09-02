/* See css_sum_value.h. CSS Typed OM 1 §4.3.1 Common Numeric Operations, and the CSSNumericValue Superclass's
   SUM VALUE — create a sum value, create a CSSUnitValue from a sum value item, create a type from a unit map,
   and the product of two unit maps. The §4.3.2 Numeric Value Typing algebra it multiplies with is core/css/
   css_math.h's, where css-values-4 §10.9 Type Checking links to it. */

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"

#include "core/css/css_math.h"
#include "core/css/css_math_value.h"
#include "core/css/css_numeric_value.h"
#include "core/css/css_sum_value.h"
#include "core/css/css_unit_value.h"
#include "core/idl_args.h"
#include "solver/concolic.h"

/* §4.3.1's SEVEN ARMS, as the names solver/concolic.h keys every derivation this component mints by. They are
 * the SUM VALUE's arms and not §4.3.1's member algorithms, which is a deliberate SPLIT rather than a spelling:
 * `CSS.px(x).min(CSS.px(y))` collapses inside the `min()` member and derives under that member's name, while
 * `new CSSMathMin(CSS.px(x), CSS.px(y)).to("px")` derives here, and the two are two algorithms of this section
 * that happen to compute one number. solver/concolic.h states which direction is safe to be wrong in: two keys
 * for one value cost an extra frontier entry and decide nothing wrongly, while one key for two values decides
 * a gate the flow never asked about. A shared literal across two components would also be the copy that
 * disagrees the day one of them is edited, silently, because both spellings compose a plausible key. */
#define SV_OP_UNIT    "CSS Typed OM 1 §4.3.1 create a sum value from a CSSUnitValue"
#define SV_OP_SUM     "CSS Typed OM 1 §4.3.1 create a sum value from a CSSMathSum"
#define SV_OP_PRODUCT "CSS Typed OM 1 §4.3.1 create a sum value from a CSSMathProduct"
#define SV_OP_NEGATE  "CSS Typed OM 1 §4.3.1 create a sum value from a CSSMathNegate"
#define SV_OP_INVERT  "CSS Typed OM 1 §4.3.1 create a sum value from a CSSMathInvert"
#define SV_OP_MIN     "CSS Typed OM 1 §4.3.1 create a sum value from a CSSMathMin"
#define SV_OP_MAX     "CSS Typed OM 1 §4.3.1 create a sum value from a CSSMathMax"

/* ---- the unit map ------------------------------------------------------------------------------------------ */

static void sv_map_release(CssUnitMap *m)
{
    int i;

    for (i = 0; i < m->n; i++) free(m->e[i].unit);
    free(m->e);
    m->e = NULL;
    m->n = m->cap = 0;
}

/* ONE KEY, INSERTED IN CODE POINT ORDER OR ADDED TO THE ENTRY THAT IS ALREADY THERE — which is the whole of
   what §4.3.1's two writers of a unit map do ("set result[unit] to power", and "increment result[unit] by
   power"). The ORDER is this component's and not the spec's map's insertion order; see css_sum_value.h for the
   argument that makes that safe and for why it is required rather than merely convenient. */
static void sv_map_add(CssUnitMap *m, const char *unit, int power)
{
    size_t len = strlen(unit);
    int i;

    for (i = 0; i < m->n; i++) {
        int c = strcmp(m->e[i].unit, unit);

        if (c == 0) { m->e[i].power += power; return; }
        if (c > 0) break;
    }
    if (m->n == m->cap) {
        int want = m->cap ? m->cap * 2 : 4;
        CssSumUnit *grown = realloc(m->e, (size_t)want * sizeof *grown);

        CHECK(grown != NULL, "css-typed-om: OOM building a CSS Typed OM 1 §4.3.1 unit map");
        m->e = grown;
        m->cap = want;
    }
    memmove(&m->e[i + 1], &m->e[i], (size_t)(m->n - i) * sizeof *m->e);
    m->e[i].unit = malloc(len + 1);
    CHECK(m->e[i].unit != NULL, "css-typed-om: OOM copying a CSS Typed OM 1 §4.3.1 unit map key");
    memcpy(m->e[i].unit, unit, len + 1);
    m->e[i].power = power;
    m->n++;
}

static void sv_map_copy(CssUnitMap *dst, const CssUnitMap *src)
{
    int i;

    DCHECK(dst->n == 0, "a §4.3.1 unit map was copied over one that already has entries — every copy here is "
                        "into a freshly pushed item, so a non-empty destination is an item reused across two "
                        "steps of one arm");
    for (i = 0; i < src->n; i++) sv_map_add(dst, src->e[i].unit, src->e[i].power);
}

/* "the same unit map as subvalue" and "not all of the unit maps among the items of args are identical", which
   are ONE question — whether two maps have the same keys at the same powers. Entry-wise, because the entries
   are canonically ordered; an insertion-ordered comparison would answer NO for two maps that ARE the same map.
   IT IS NEVER ASKED ABOUT A VALUE, which is why nothing in this component forks: a unit map is strings and
   integers this component computed, and a page's own unknown never enters one. */
static bool sv_map_same(const CssUnitMap *a, const CssUnitMap *b)
{
    int i;

    if (a->n != b->n) return false;
    for (i = 0; i < a->n; i++)
        if (a->e[i].power != b->e[i].power || strcmp(a->e[i].unit, b->e[i].unit) != 0) return false;
    return true;
}

/* §4.3.1's "The product of two unit maps units1 and units2": "Let result be a copy of units1. For each unit →
   power in units2: If result[unit] exists, increment result[unit] by power. Otherwise, set result[unit] to
   power." The zero removal is the CALLER's sentence — §4.3.1's CSSMathProduct arm asks for the product "with
   all entries with a zero value removed" — and it is done here because this is the only place a zero can be
   produced (see css_sum_value.h); every other site asserts the invariant instead of re-imposing it. */
static void sv_map_product(CssUnitMap *out, const CssUnitMap *a, const CssUnitMap *b)
{
    int i;

    sv_map_copy(out, a);
    for (i = 0; i < b->n; i++) sv_map_add(out, b->e[i].unit, b->e[i].power);
    for (i = out->n - 1; i >= 0; i--) {
        if (out->e[i].power != 0) continue;
        free(out->e[i].unit);
        memmove(&out->e[i], &out->e[i + 1], (size_t)(out->n - i - 1) * sizeof *out->e);
        out->n--;
    }
}

/* THE INVARIANT css_sum_value.h STATES, asserted where a map is finished rather than imposed there. */
static void sv_map_check(const CssUnitMap *m)
{
    int i;

    for (i = 0; i < m->n; i++) {
        DCHECK(m->e[i].power != 0,
               "a CSS Typed OM 1 §4.3.1 unit map carries an entry whose power is ZERO. Only the product of two "
               "unit maps can make one, and that is the one algorithm whose own sentence removes them (\"with "
               "all entries with a zero value removed\") — so a zero here would make «[\"px\" → 0]» and «[ ]» "
               "two maps that are one map, and the merge in the CSSMathSum arm would silently keep two items "
               "where a browser keeps one");
        DCHECK(i == 0 || strcmp(m->e[i - 1].unit, m->e[i].unit) < 0,
               "a CSS Typed OM 1 §4.3.1 unit map is not in strictly increasing code point order. The order is "
               "what makes \"the same unit map\" a question about CONTENT — see core/css/css_sum_value.h — so "
               "an out-of-order or duplicated key is two items that will never merge");
    }
}

/* §4.3.1's "To create a type from a unit map unit map": "Let types be an initially empty list. For each unit →
 * power in unit map: Let type be the result of creating a type from unit. Set type's sole value to power.
 * Append type to types. Return the result of multiplying all the items of types."
 *
 * THE EMPTY MAP ANSWERS §4.3.2's «[ ]», which is "multiplying all the items" of an EMPTY list — the identity
 * of that operation, and the same type §4.3.2's create-a-type-from-a-string-unit gives "number". That is not a
 * special case written here; it is what the accumulator starts at.
 *
 * FALSE is the multiply's own failure, which §4.3.1's CSSMathSum arm turns into "return failure". */
static bool sv_type_of_map(const CssUnitMap *m, CssMathType *out)
{
    CssMathType acc = css_math_type_number();
    int i;

    for (i = 0; i < m->n; i++) {
        CssMathType t, r;
        int b, sole = -1;

        if (!css_numeric_type_from_unit(m->e[i].unit, strlen(m->e[i].unit), &t)) {
            DFAIL("CSS Typed OM 1 §4.3.1's create-a-type-from-a-unit-map met a unit §4.3.2's "
                  "create-a-type-from-a-string-unit REFUSES. Every key in a unit map came from a §4.3.3 `unit` "
                  "internal slot or from the canonical unit of that unit's compatible-unit set, and §4.3.3's "
                  "constructor throws a TypeError for a unit with no type — so this is core/css/"
                  "css_namespace.c's factory table naming a unit core/css/css_length.h's and core/css/"
                  "css_dimension.h's tables do not, and the missing unit family is the component to BUILD");
            return false;
        }
        /* "Set type's sole value to power." A type from a unit identifier is «[base → 1]», so the sole value
           is the one non-zero exponent; the "number" branch has NO sole value, and no unit map can key it. */
        for (b = 0; b < CSS_MATH_BASE_COUNT; b++) {
            if (t.exp[b] == 0) continue;
            DCHECK(sole < 0, "§4.3.2's create-a-type-from-a-string-unit answered a type with TWO non-zero "
                             "exponents — every one of its nine branches returns «[ ]» or a single base type "
                             "at exponent 1, so a second entry is that entry composing types instead of "
                             "constructing one");
            sole = b;
        }
        DCHECK(sole >= 0,
               "a CSS Typed OM 1 §4.3.1 unit map is keyed \"number\", whose type is «[ ]» and which therefore "
               "has no sole value to set. The CSSUnitValue arm's own sentence is \"If unit is \"number\", "
               "return «(value, «[ ]»)»\" — it writes NO entry for that unit — so a \"number\" key is that arm "
               "having taken its Otherwise branch for the unit it is stated to exclude");
        if (sole < 0) return false;
        t.exp[sole] = m->e[i].power;
        /* §4.3.2's multiply. NEITHER of its failure arms is reachable here, which is what makes the ORDER of
           this fold unobservable and the canonical sort in css_sum_value.h safe. §4.3.2's own arm needs both
           operands to carry a non-null percent hint and create-a-type-from-a-string-unit ends "In all cases,
           the associated percent hint is null"; core/css/css_math.h's arm needs an operand that IS §4.3.2's
           failure, and that same entry answers a type or refuses rather than answering a failure type — the
           refusal is the DFAIL above. The call is still checked: an unreachable failure arm silently returning
           a plausible type is exactly the shape CLAUDE.md names, and either arm arriving must show here. */
        if (!css_math_type_mul(&acc, &t, &r)) return false;
        acc = r;
    }
    *out = acc;
    return true;
}

/* ---- the sum value's list, and the arithmetic over its values ---------------------------------------------- */

static CssSumItem *sv_push(CssSumValue *s)
{
    CssSumItem *item;

    if (s->n == s->cap) {
        int want = s->cap ? s->cap * 2 : 4;
        CssSumItem *grown = realloc(s->v, (size_t)want * sizeof *grown);

        CHECK(grown != NULL, "css-typed-om: OOM building a CSS Typed OM 1 §4.3.1 sum value");
        s->v = grown;
        s->cap = want;
    }
    item = &s->v[s->n++];
    item->value = JS_UNDEFINED;
    item->units.e = NULL;
    item->units.n = item->units.cap = 0;
    return item;
}

void css_sum_value_release(JSContext *ctx, CssSumValue *s)
{
    int i;

    for (i = 0; i < s->n; i++) {
        JS_FreeValue(ctx, s->v[i].value);
        sv_map_release(&s->v[i].units);
    }
    free(s->v);
    s->v = NULL;
    s->n = s->cap = 0;
}

/* THE WHOLE LIST MOVES, references and all — the shape §4.3.1's unary arms are stated in ("Let values be the
   result of creating a sum value from this's value internal slot"), where the child's list IS the parent's. */
static void sv_move(CssSumValue *dst, CssSumValue *src)
{
    DCHECK(dst->n == 0, "a §4.3.1 sum value was moved onto one that already holds items — the unary arms move "
                        "exactly once, from a child that has just completed");
    *dst = *src;
    src->v = NULL;
    src->n = src->cap = 0;
}

/* A `value` internal slot's NUMBER — the Number itself, or §3.2's conversion run on an unknown's own example.
   FALSE is "there is no example yet", which is a POSITIVE statement: the code has computed no concrete, and
   choosing one here would invent a value §@H forbids inventing. */
static bool sv_number(JSContext *ctx, JSValueConst slot, double *out)
{
    return idl_number_of(ctx, IDL_DOUBLE, slot, out) != 0;
}

/* ONE ARITHMETIC STEP OF A SUM VALUE, over operands that are each either a Number or unknown external input.
 * OWNED. `concrete` is the REAL operation the caller has already run on the operands' own examples and `have`
 * says whether every one of them had one — never a rule predicting what the operation would produce.
 *
 * IT IS ONE ENTRY BECAUSE THE SEVEN ARMS DIFFER ONLY IN THE NAME AND THE OPERANDS. Every one of them computes
 * a number from a list of values under one named algorithm, so the concrete arm, the unknown arm and the
 * assertion that they cannot both be absent are stated once. */
static JSValue sv_value(JSContext *ctx, const char *op, const JSValueConst *ops, int n,
                        double concrete, bool have)
{
    JSValue example = have ? JS_NewFloat64(ctx, concrete) : JS_UNDEFINED;
    JSValue r;
    bool unknown = false;
    int i;

    DCHECK(n >= 1, "a §4.3.1 sum-value arithmetic step was given NO operands — every arm names at least the "
                   "value it is transforming");
    for (i = 0; i < n; i++)
        if (concolic_is(ops[i])) unknown = true;
    if (!unknown) {
        DCHECK(have, "a §4.3.1 sum-value step over values this engine knows the numbers of produced no number "
                     "— a Number has an example by definition, so this is idl_number_of's two answers having "
                     "come apart");
        return example;
    }
    /* THE VALUE IS UNKNOWN EXTERNAL INPUT, SO THE RESULT OF THE STEP IS TOO. A concrete number here would
       DE-TAINT what `new CSSMathSum(CSS.px(attackerNumber), CSS.in(1)).to("px")` carries into whatever
       consumes it. The derivation names every operand IN ORDER, which is what keeps two steps differing only
       in an operand it did not name two questions rather than one. */
    r = concolic_new_derived(ctx, op, ops, n, example);
    DCHECK(!JS_IsUninitialized(r),
           "solver/concolic.h's derivation refused an operand list this step had already established holds "
           "unknown external input — the two tests read the same values one loop apart");
    return r;
}

/* ---- §4.3.1's CSSUnitValue arm ------------------------------------------------------------------------------ */

/* "Let unit be the value of this's unit internal slot, and value be the value of this's value internal slot.
 * If unit is a member of a set of compatible units, and is not the set's canonical unit, multiply value by the
 * conversion ratio between unit and the canonical unit, and change unit to the canonical unit. If unit is
 * "number", return «(value, «[ ]»)». Otherwise, return «(value, «[unit → 1]»)»."
 *
 * THE MULTIPLICATION IS SKIPPED WHERE THE RATIO IS EXACTLY 1 AND THE RENAME IS NOT, which is the same argument
 * core/css/css_numeric_value.c's `to()` makes about its own ratio: `value * 1` IS `value` for everything a
 * §4.3.3 slot can hold, so multiplying would give one number a second identity and stop two values that are
 * one value from comparing. The rename still happens, and it is what makes `new CSSUnitValue(1, "PX")` and
 * `CSS.px(1)` one key in a unit map: css-values-4 §5.4.1 Compatible Units relates units by a static factor and
 * CSS matches unit identifiers ASCII case-insensitively, so "PX" is px and px is the set's canonical unit. A
 * unit in NO set keeps its own bytes — «["EM" → 1]» is not «["em" → 1]» — which is §4.3.1's own shape, and is
 * the same code-point comparison its `equals` is written against. */
static void sv_leaf(JSContext *ctx, JSValueConst v, CssSumValue *out)
{
    const char *unit = css_unit_value_unit(v), *canon = NULL;
    JSValue slot = css_unit_value_value(ctx, v);
    double per = 1.0;
    CssSumItem *item;

    DCHECK(css_unit_value_is(v), "§4.3.1's create-a-sum-value reached its CSSUnitValue arm over something that "
                                 "is not one — the walk asks css_math_value_is first and every other "
                                 "CSSNumericValue in this engine is a CSSUnitValue");
    if (css_numeric_canonical(unit, strlen(unit), &per, &canon)) {
        if (per != 1.0) {
            double x = 0.0;
            bool have = sv_number(ctx, slot, &x);
            JSValue scaled = sv_value(ctx, SV_OP_UNIT, (const JSValueConst *)&slot, 1, x * per, have);

            JS_FreeValue(ctx, slot);
            slot = scaled;
        }
        unit = canon;
    }
    item = sv_push(out);
    item->value = slot;
    if (strcmp(unit, "number") != 0) sv_map_add(&item->units, unit, 1);
    sv_map_check(&item->units);
}

/* ---- the walk ----------------------------------------------------------------------------------------------- */

/* ONE NODE OF THE WALK. `node` and `items` are OWNED by the frame, which is what makes the stack's lifetime a
   property of the stack rather than of the tree it walks. `acc` is the arm's own accumulator — §4.3.1 names it
   `values` in four arms and `args` in the min/max pair — and `vals` is the min/max arm's ordered list of sole
   values, which is the one arm whose answer is a function of every argument at once. */
typedef struct {
    JSValue     node;
    JSValue     items;
    CssMathOp   op;
    uint32_t    i, n;
    CssSumValue acc;
    JSValue    *vals;
    int         nvals, capvals;
} SvFrame;

static void sv_frame_release(JSContext *ctx, SvFrame *f)
{
    int i;

    JS_FreeValue(ctx, f->node);
    JS_FreeValue(ctx, f->items);
    css_sum_value_release(ctx, &f->acc);
    for (i = 0; i < f->nvals; i++) JS_FreeValue(ctx, f->vals[i]);
    free(f->vals);
    f->vals = NULL;
    f->nvals = f->capvals = 0;
}

static void sv_frame_push(JSContext *ctx, SvFrame **stk, int *depth, int *cap, JSValueConst node)
{
    SvFrame *f;
    JSValue len;
    uint32_t n = 0;

    if (*depth == *cap) {
        int want = *cap ? *cap * 2 : 16;
        SvFrame *grown = realloc(*stk, (size_t)want * sizeof *grown);

        CHECK(grown != NULL, "css-typed-om: OOM walking a CSSMathValue for CSS Typed OM 1 §4.3.1's sum value");
        *stk = grown;
        *cap = want;
    }
    f = &(*stk)[*depth];
    memset(f, 0, sizeof *f);
    f->node = JS_DupValue(ctx, node);
    f->op = css_math_value_op(node);
    f->items = css_math_value_items(ctx, node);
    len = JS_GetPropertyStr(ctx, f->items, "length");
    JS_ToUint32(ctx, &n, len);
    JS_FreeValue(ctx, len);
    f->n = n;
    DCHECK(n >= 1, "a §4.3.4 math value carries an EMPTY operand list. Its constructor's own step is \"If args "
                   "is empty, throw a SyntaxError\" and a unary operator's slot is one value, so an empty list "
                   "is a record written past core/css/css_math_value.c's one mint");
    /* §4.3.1's CSSMathProduct arm: "Let values initially be the sum value «(1, «[ ]»)». (I.e. what you'd get
       from 1.)" — the identity of the cross product below, stated by the section rather than chosen here. */
    if (f->op == CSS_MATH_OP_PRODUCT) {
        CssSumItem *seed = sv_push(&f->acc);

        seed->value = JS_NewFloat64(ctx, 1.0);
    }
    (*depth)++;
}

static void sv_vals_push(SvFrame *f, JSValue v)   /* CONSUMES v */
{
    if (f->nvals == f->capvals) {
        int want = f->capvals ? f->capvals * 2 : 4;
        JSValue *grown = realloc(f->vals, (size_t)want * sizeof *grown);

        CHECK(grown != NULL, "css-typed-om: OOM collecting a §4.3.1 CSSMathMin/Max argument list");
        f->vals = grown;
        f->capvals = want;
    }
    f->vals[f->nvals++] = v;
}

/* ONE COMPLETED CHILD, FOLDED INTO ITS PARENT'S ARM. `child` is consumed by the caller either way; what this
   entry takes from it, it dups or moves. FALSE is §4.3.1's "return failure" for the arm. */
static bool sv_consume(JSContext *ctx, SvFrame *f, CssSumValue *child)
{
    int i, j;

    switch (f->op) {
    case CSS_MATH_OP_SUM:
        /* "For each subvalue of value: If values already contains an item with the same unit map as subvalue,
           increment that item's value by the value of subvalue. Otherwise, append subvalue to values." */
        for (i = 0; i < child->n; i++) {
            CssSumItem *sub = &child->v[i], *hit = NULL;

            for (j = 0; j < f->acc.n; j++)
                if (sv_map_same(&f->acc.v[j].units, &sub->units)) { hit = &f->acc.v[j]; break; }
            if (hit != NULL) {
                JSValueConst ops[2];
                double a = 0.0, b = 0.0;
                bool have;

                ops[0] = hit->value;
                ops[1] = sub->value;
                have = sv_number(ctx, ops[0], &a) && sv_number(ctx, ops[1], &b);
                {
                    JSValue sum = sv_value(ctx, SV_OP_SUM, ops, 2, a + b, have);

                    JS_FreeValue(ctx, hit->value);
                    hit->value = sum;
                }
            } else {
                CssSumItem *item = sv_push(&f->acc);

                item->value = JS_DupValue(ctx, sub->value);
                sv_map_copy(&item->units, &sub->units);
                sv_map_check(&item->units);
            }
        }
        return true;
    case CSS_MATH_OP_PRODUCT: {
        /* "For each item1 in values: For each item2 in new values: Let item be a tuple with its value set to
           the product of the values of item1 and item2, and its unit map set to the product of the unit maps
           of item1 and item2, with all entries with a zero value removed. Append item to temp. Set values to
           temp." */
        CssSumValue temp = { NULL, 0, 0 };

        for (i = 0; i < f->acc.n; i++)
            for (j = 0; j < child->n; j++) {
                CssSumItem *item = sv_push(&temp);
                JSValueConst ops[2];
                double a = 0.0, b = 0.0;
                bool have;

                ops[0] = f->acc.v[i].value;
                ops[1] = child->v[j].value;
                have = sv_number(ctx, ops[0], &a) && sv_number(ctx, ops[1], &b);
                item->value = sv_value(ctx, SV_OP_PRODUCT, ops, 2, a * b, have);
                sv_map_product(&item->units, &f->acc.v[i].units, &child->v[j].units);
                sv_map_check(&item->units);
            }
        css_sum_value_release(ctx, &f->acc);
        f->acc = temp;
        return true;
    }
    case CSS_MATH_OP_NEGATE:
    case CSS_MATH_OP_INVERT:
        /* "Let values be the result of creating a sum value from this's value internal slot." The list IS the
           parent's; the transform is the arm's next step and stands in sv_finish. */
        sv_move(&f->acc, child);
        return true;
    case CSS_MATH_OP_MIN:
    case CSS_MATH_OP_MAX:
        /* "If any item of args is failure, or has a length greater than one, return failure. If not all of the
           unit maps among the items of args are identical, return failure." */
        if (child->n != 1) return false;
        if (f->acc.n == 0) {
            CssSumItem *keep = sv_push(&f->acc);

            sv_map_copy(&keep->units, &child->v[0].units);
            sv_map_check(&keep->units);
        } else if (!sv_map_same(&f->acc.v[0].units, &child->v[0].units)) {
            return false;
        }
        sv_vals_push(f, JS_DupValue(ctx, child->v[0].value));
        return true;
    case CSS_MATH_OP_N:
        break;
    }
    DFAIL("a §4.3.1 create-a-sum-value frame carries no operator");
    return false;
}

/* THE ARM'S LAST STEPS, once every child has been folded in. On TRUE the frame's accumulator MOVES to `out`;
   on FALSE — §4.3.1's "return failure" — it stays in the frame for sv_frame_release to free. */
static bool sv_finish(JSContext *ctx, SvFrame *f, CssSumValue *out)
{
    int i;

    switch (f->op) {
    case CSS_MATH_OP_SUM: {
        /* "Create a type from the unit map of each item of values, and add all the types together. If the
           result is failure, return failure." LEFT TO RIGHT over `values`, because §4.3.2's add is NOT
           associative — its percent-hint arm provisionally applies a hint to both operands and keeps the first
           that makes their entries agree — and core/css/css_math.h says so in its own words. */
        CssMathType acc;

        DCHECK(f->acc.n >= 1,
               "§4.3.1's CSSMathSum arm finished with an EMPTY list of values. Every child of a math value "
               "yields at least one item — the CSSUnitValue arm yields exactly one and every other arm is a "
               "fold over children that each do — so an empty list is a §4.3.4 mint that accepted no operands");
        for (i = 0; i < f->acc.n; i++) {
            CssMathType t, r;

            if (!sv_type_of_map(&f->acc.v[i].units, &t)) return false;
            if (i == 0) { acc = t; continue; }
            if (!css_math_type_add(&acc, &t, &r)) return false;
            acc = r;
        }
        break;
    }
    case CSS_MATH_OP_PRODUCT:
        break;
    case CSS_MATH_OP_NEGATE:
        /* "Negate the value of each item of values." */
        for (i = 0; i < f->acc.n; i++) {
            JSValueConst slot = f->acc.v[i].value;
            double x = 0.0;
            bool have = sv_number(ctx, slot, &x);
            JSValue neg = sv_value(ctx, SV_OP_NEGATE, &slot, 1, -x, have);

            JS_FreeValue(ctx, f->acc.v[i].value);
            f->acc.v[i].value = neg;
        }
        break;
    case CSS_MATH_OP_INVERT: {
        /* "If the length of values is more than one, return failure. Invert (find the reciprocal of) the value
           of the item in values, and negate the value of each entry in its unit map."
           THERE IS NO ZERO TEST IN THIS ARM AND ONE MUST NOT BE ADDED. §4.3.1's invert-a-CSSNumericValue has
           the RangeError; this algorithm does not, so 1/0 is IEEE-754's infinity and a refusal invented here
           would be a TypeError no standard states — and, worse, a BRANCH over a value, which is the one thing
           this component is written to have none of. */
        JSValueConst slot;
        double x = 0.0;
        bool have;
        JSValue inv;

        if (f->acc.n != 1) return false;
        slot = f->acc.v[0].value;
        have = sv_number(ctx, slot, &x);
        inv = sv_value(ctx, SV_OP_INVERT, &slot, 1, 1.0 / x, have);
        JS_FreeValue(ctx, f->acc.v[0].value);
        f->acc.v[0].value = inv;
        for (i = 0; i < f->acc.v[0].units.n; i++)
            f->acc.v[0].units.e[i].power = -f->acc.v[0].units.e[i].power;
        sv_map_check(&f->acc.v[0].units);
        break;
    }
    case CSS_MATH_OP_MIN:
    case CSS_MATH_OP_MAX: {
        /* "Return the item of args whose sole item has the smallest value" — and its maximum twin.
           IT IS A VALUE AND NOT A SELECTION, which is what keeps it from being a branch over unknown input.
           The step above has already refused every argument whose unit map differs, so the items it chooses
           among differ ONLY in their value and the one it returns is «(the smallest of those values, that one
           unit map)». Computing it that way is not an approximation of the selection — it is the same item
           under every assignment of the unknowns — and it is why an unknown among the arguments stays unknown
           instead of deciding a comparison the page never wrote. */
        double acc = 0.0;
        bool have = true;

        DCHECK(f->acc.n == 1 && f->nvals >= 1,
               "§4.3.1's CSSMathMin/Max arm finished with no argument at all — its frame collects one value "
               "per operand and §4.3.4's mint refuses an empty operand list");
        for (i = 0; i < f->nvals; i++) {
            double x = 0.0;

            if (!sv_number(ctx, f->vals[i], &x)) { have = false; break; }
            if (i == 0) acc = x;
            /* §3.2.7's RESTRICTED `double` refused a NaN at every boundary that writes a §4.3.3 slot, so this
               comparison is total and IEEE-754's own ordering is the answer. */
            else if (f->op == CSS_MATH_OP_MIN) acc = x < acc ? x : acc;
            else acc = x > acc ? x : acc;
        }
        JS_FreeValue(ctx, f->acc.v[0].value);
        /* A ONE-ARGUMENT MIN IS ITS ARGUMENT, and that is not an optimisation: minting a derivation for it
           would give the SAME NUMBER a second identity, so `new CSSMathMin(CSS.px(x)).to("px")` would stop
           comparing equal to `CSS.px(x)` and would file a constraint entry over a question already answered.
           It is the same sentence core/css/css_numeric_value.c's fold states about a one-item fold. */
        if (f->nvals == 1)
            f->acc.v[0].value = JS_DupValue(ctx, f->vals[0]);
        else
            f->acc.v[0].value = sv_value(ctx, f->op == CSS_MATH_OP_MIN ? SV_OP_MIN : SV_OP_MAX,
                                         (const JSValueConst *)f->vals, f->nvals, acc, have);
        break;
    }
    case CSS_MATH_OP_N:
        DFAIL("a §4.3.1 create-a-sum-value frame carries no operator");
        return false;
    }
    sv_move(out, &f->acc);
    return true;
}

bool css_sum_value_create(JSContext *ctx, JSValueConst v, CssSumValue *out)
{
    SvFrame *stk = NULL;
    CssSumValue pending = { NULL, 0, 0 };
    int depth = 0, cap = 0;
    bool have_pending = false, ok = true;

    DCHECK(out != NULL, "§4.3.1's create-a-sum-value was handed nowhere to write the list — the list IS the "
                        "answer and the failure half alone is not what either caller reads");
    out->v = NULL;
    out->n = out->cap = 0;
    DCHECK(css_numeric_value_is(ctx, v),
           "§4.3.1's create-a-sum-value was asked about something that is not a CSSNumericValue — every caller "
           "is a member body past its Web IDL §3.7.7 Operations brand check or an operand of a §4.3.4 list "
           "the mint rectified");

    if (!css_math_value_is(v)) {
        sv_leaf(ctx, v, &pending);
        have_pending = true;
    } else {
        sv_frame_push(ctx, &stk, &depth, &cap, v);
    }

    while (depth > 0) {
        SvFrame *f = &stk[depth - 1];

        if (have_pending) {
            ok = sv_consume(ctx, f, &pending);
            css_sum_value_release(ctx, &pending);
            have_pending = false;
            if (!ok) break;
            continue;
        }
        if (f->i < f->n) {
            JSValue child = JS_GetPropertyUint32(ctx, f->items, f->i);

            f->i++;
            DCHECK(css_numeric_value_is(ctx, child),
                   "a §4.3.4 operand list held something that is not a CSSNumericValue — the mint rectifies "
                   "every item, so anything else was put there past it");
            if (css_math_value_is(child)) {
                /* THE PUSH NEEDS NO GROWTH CHECK OF ITS OWN AND `f` IS NOT TOUCHED AFTER IT: sv_frame_push may
                   realloc the stack, and re-reading the top of it at the head of the loop is what makes that
                   safe. A frame pointer held across a push is the shape that turns a walk into a
                   use-after-free the first time a tree is deep enough to grow. */
                sv_frame_push(ctx, &stk, &depth, &cap, child);
            } else {
                sv_leaf(ctx, child, &pending);
                have_pending = true;
            }
            JS_FreeValue(ctx, child);
            continue;
        }
        ok = sv_finish(ctx, f, &pending);
        sv_frame_release(ctx, f);
        depth--;
        if (!ok) break;
        have_pending = true;
    }

    if (!ok) {
        css_sum_value_release(ctx, &pending);
        while (depth > 0) { depth--; sv_frame_release(ctx, &stk[depth]); }
        free(stk);
        return false;
    }
    DCHECK(depth == 0 && have_pending,
           "§4.3.1's create-a-sum-value left its walk with no answer and no failure — the loop ends only by "
           "finishing the root frame, which sets one, or by a failure that returns above this line");
    free(stk);
    *out = pending;
    return true;
}

/* ---- §4.3.1's create a CSSUnitValue from a sum value item -------------------------------------------------- */

JSValue css_sum_value_unit_value(JSContext *ctx, const CssSumItem *item)
{
    DCHECK(item != NULL, "§4.3.1's create-a-CSSUnitValue-from-a-sum-value-item was handed no item");
    /* "If item has more than one entry in its unit map, return failure." */
    if (item->units.n > 1) return JS_UNDEFINED;
    /* "If item has no entries in its unit map, return a new CSSUnitValue whose unit internal slot is set to
       "number", and whose value internal slot is set to item's value." */
    if (item->units.n == 0) return css_unit_value_new(ctx, JS_DupValue(ctx, item->value), "number");
    /* "Otherwise, item has a single entry in its unit map. If that entry's value is anything other than 1,
       return failure. Otherwise, return a new CSSUnitValue whose unit internal slot is set to that entry's
       key, and whose value internal slot is set to item's value." */
    if (item->units.e[0].power != 1) return JS_UNDEFINED;
    return css_unit_value_new(ctx, JS_DupValue(ctx, item->value), item->units.e[0].unit);
}
