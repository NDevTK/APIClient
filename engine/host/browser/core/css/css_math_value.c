/* See css_math_value.h. CSS Typed OM 1 §4.3.4 Complex Numeric Values: CSSMathValue objects and §6.5
   CSSMathValue Serialization — the operation nodes of a numeric value, over core/css/css_unit_value.c's
   leaves. The §4.3.2 Numeric Value Typing algebra every constructor here runs is core/css/css_math.h's, where
   css-values-4 §10.9 Type Checking links to it. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"

#include "core/agent_state.h"
#include "core/css/css_length.h"
#include "core/css/css_math.h"
#include "core/css/css_math_value.h"
#include "core/css/css_numeric_value.h"
#include "core/css/css_unit_value.h"
#include "core/idl_args.h"
#include "core/idl_indexed.h"
#include "solver/concolic.h"

/* §4.3.4's SIX CONSTRUCTIBLE INTERFACES, one JSClassID each, plus the abstract superclass and CSSNumericArray.
   ONE CLASS PER INTERFACE AND NOT ONE CLASS WITH A KIND FIELD, for two reasons that are both the spec's:
   Web IDL §3.7.3 Interface prototype object gives each interface its OWN prototype object and quickjs holds a
   realm's prototype per CLASS, and §4.3.1's equal numeric values opens with "If value1 and value2 are not
   members of the same interface, return false" — which over one shared class would have to be re-derived from
   a field instead of being the class id the object already carries. */
static JSClassID g_class[CSS_MATH_OP_N];
/* THE TWO CLASSES THAT EXIST ONLY TO HOLD A PROTOTYPE. `CSSMathValue` is abstract — §4.3.4: "CSSMathValue,
   being a pure superclass, cannot be directly constructed" — so nothing is ever branded with it; its class is
   the per-realm hook its interface prototype object hangs off. CSSNumericArray's objects are
   core/idl_indexed.c's class (anything with an indexed property getter is), so its own class is likewise only
   the prototype's home — the pattern core/geometry/dom_rect_list.c states. */
static JSClassID g_abstract_class;
static JSClassID g_array_class;

/* THE PRIVATE SLOT A CSSNumericArray'S ITEMS HANG OFF, and its BRAND: an indexed-property object is not a
   class of its own, so what cannot be forged is the own SLOT this component put on it — dom_rect_list.c's
   reasoning, and its mechanism. A JS Array, because CLAUDE.md's rule for platform data a flow holds is that it
   must fork per flow and park to the cold tier, and an Array's mutations are property writes the COW delta
   already captures while a malloc'd list captured as pointers leaks its nodes on a context switch. */
static JSValue g_items_key = JS_UNDEFINED;
static JSAtom  g_atom_items = JS_ATOM_NULL;

static int g_id_ctor[CSS_MATH_OP_N];

/* §4.3.4's IDENTIFIERS, one row per operator, spelled ONCE. Every one of the four things a row carries is the
   same fact stated for a different consumer — the interface name Web IDL §3.7.1 puts on the global and §3.7.3
   tags the prototype with, the `CSSMathOperator` string the `operator` attribute answers, and the attribute
   name §4.3.4 declares for the slot — so a seventh operator is a row rather than four edits. */
static const struct {
    const char *iface;   /* §3.7.1 Interface object's identifier */
    const char *op;      /* §4.3.4's CSSMathOperator value the `operator` attribute answers */
    const char *slot;    /* §4.3.4's attribute name for the internal slot */
} MV[CSS_MATH_OP_N] = {
    { "CSSMathSum",     "sum",     "values" },
    { "CSSMathProduct", "product", "values" },
    { "CSSMathNegate",  "negate",  "value"  },
    { "CSSMathInvert",  "invert",  "value"  },
    { "CSSMathMin",     "min",     "values" },
    { "CSSMathMax",     "max",     "values" },
};

bool css_math_op_is_list(CssMathOp op)
{
    DCHECK((unsigned)op < (unsigned)CSS_MATH_OP_N,
           "a §4.3.4 operator outside this component's enumeration was asked about its slot shape");
    return op == CSS_MATH_OP_SUM || op == CSS_MATH_OP_PRODUCT ||
           op == CSS_MATH_OP_MIN || op == CSS_MATH_OP_MAX;
}

/* ---- the record ------------------------------------------------------------------------------------------- */

/* §4.3.4's INTERNAL SLOTS, and the type its constructor already computed.
 *
 * NOTHING REWRITES EITHER FIELD AFTER THE MINT, which is why there is no solver/cow.h capture here and why
 * that is a statement rather than an omission. §4.3.4 declares `values`, `value`, `lower` and `upper`
 * `readonly` and states no other operation over them, so unlike core/css/css_unit_value.c — whose `value`
 * attribute IS writable and which therefore captures at every reach — this record has no second writer for a
 * delta to have to see. The Array INSIDE the slot is likewise unreachable for a page to mutate: what a page
 * holds is the CSSNumericArray, whose indexed properties Web IDL §3.9.1 makes non-writable and whose §3.9.3
 * [[DefineOwnProperty]] refuses a supported index outright.
 *
 * `type` IS A POD AND IS IMMUTABLE — see the header for why it is stored rather than walked for. */
typedef struct CssMathValueData {
    CssMathType type;
    JSValue     slot;   /* the CSSNumericArray for a list operator, the CSSNumericValue for a unary one. OWNED */
} CssMathValueData;

static CssMathValueData *mv_of(JSValueConst v)
{
    JSClassID id = JS_GetClassID(v);
    unsigned k;

    for (k = 0; k < CSS_MATH_OP_N; k++)
        if (g_class[k] != 0 && id == g_class[k]) return JS_GetOpaque(v, g_class[k]);
    return NULL;
}

bool css_math_value_is(JSValueConst v)
{
    return mv_of(v) != NULL;
}

CssMathOp css_math_value_op(JSValueConst v)
{
    JSClassID id = JS_GetClassID(v);
    unsigned k;

    for (k = 0; k < CSS_MATH_OP_N; k++)
        if (g_class[k] != 0 && id == g_class[k]) return (CssMathOp)k;
    DFAIL("§4.3.4's `operator` was asked of something that is not one of this component's math values — every "
          "caller asks css_math_value_is first, and the two questions read the same class id one line apart");
    return CSS_MATH_OP_SUM;
}

CssMathType css_math_value_type(JSValueConst v)
{
    CssMathValueData *d = mv_of(v);

    DCHECK(d != NULL, "§4.3.4's type was read off something that is not a math value — the type is stored at "
                      "the mint, so an absent record is an object that reached this class by another route");
    return d->type;
}

/* Web IDL §3.7.6 Attributes' BRAND CHECK for a member of one of these prototypes, and the TypeError a page
   tells apart from `undefined`. Its two callers are §4.3.4's attribute getters — `operator`, `values` and
   `value` — so the step is create-an-attribute-getter's, whose WHOLE sentence is "If jsValue does not
   implement target, then: If attribute was specified with the [LegacyLenientThis] extended attribute, then
   return undefined. Otherwise, throw a TypeError." None of the three is [LegacyLenientThis], so the throw is
   the only arm reachable here.
   THE NUMBER READ §3.7.5, WHICH IS Constants and states nothing about a receiver. */
static CssMathValueData *mv_here(JSContext *ctx, JSValueConst v)
{
    CssMathValueData *d = mv_of(v);

    if (!d) {
        JS_ThrowTypeError(ctx, "a CSSMathValue member was reached on something that is not a CSSMathValue");
        return NULL;
    }
    return d;
}

static void mv_finalizer(JSRuntime *rt, JSValue val)
{
    JSClassID id = 0;
    CssMathValueData *d = JS_GetAnyOpaque(val, &id);

    (void)id;
    /* NOT `if (!d) return;`. css_math_value_new is the one mint and it sets the record with nothing in between
       that allocates in the JS heap. */
    DCHECK(d != NULL, "a CSSMathValue was finalized with no record — §4.3.4's one mint sets it with nothing in "
                      "between that could collect");
    JS_FreeValueRT(rt, d->slot);
    free(d);
}

static void mv_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    JSClassID id = 0;
    CssMathValueData *d = JS_GetAnyOpaque(val, &id);

    (void)id;
    DCHECK(d != NULL, "a CSSMathValue was marked with no record — its slot is a counted reference and an "
                      "unmarked child is read by gc_scan as rooted from outside the heap");
    JS_MarkValue(rt, d->slot, mark_func);
}

/* ---- §4.3.4's CSSNumericArray ------------------------------------------------------------------------------ */

/* THE BRAND, and the items behind it. JS_UNDEFINED for anything that is not a CSSNumericArray. */
static JSValue na_items(JSContext *ctx, JSValueConst v)
{
    JSValue items;

    if (!JS_IsObject(v)) return JS_UNDEFINED;
    if (JS_GetOwnSlot(ctx, &items, v, g_atom_items) <= 0)   /* an own SLOT, never a lookup */
        return JS_UNDEFINED;
    return items;
}

/* §4.3.4: "The length attribute of CSSNumericArray indicates how many CSSNumericValues are contained within
   the CSSNumericArray." */
static uint32_t na_length(JSContext *ctx, JSValueConst self)
{
    JSValue items = na_items(ctx, self), len;
    uint32_t n = 0;

    if (!JS_IsObject(items)) { JS_FreeValue(ctx, items); return 0; }
    len = JS_GetPropertyStr(ctx, items, "length");
    JS_ToUint32(ctx, &n, len);
    JS_FreeValue(ctx, len);
    JS_FreeValue(ctx, items);
    return n;
}

/* §4.3.4: "The indexed property getter of CSSNumericArray retrieves the CSSNumericValue at the provided
   index." JS_UNDEFINED past the end, which is what an index outside the supported property indices is. */
static JSValue na_item(JSContext *ctx, JSValueConst self, uint32_t i)
{
    JSValue items = na_items(ctx, self), r;

    if (!JS_IsObject(items)) { JS_FreeValue(ctx, items); return JS_UNDEFINED; }
    r = JS_GetPropertyUint32(ctx, items, i);
    JS_FreeValue(ctx, items);
    DCHECK(JS_IsUndefined(r) || css_numeric_value_is(ctx, r),
           "a CSSNumericArray held something that is not a CSSNumericValue — its IDL declares "
           "`getter CSSNumericValue (unsigned long index)` and §4.3.4's constructors rectify every item before "
           "the list is built, so anything else was put there past the one mint");
    return r;
}

static const IdlIndexedDecl NA_INDEXED = { "CSSNumericArray", na_length, na_item, NULL, 0 };

static bool na_is(JSContext *ctx, JSValueConst v)
{
    JSValue items = na_items(ctx, v);
    bool ok = JS_IsObject(items);

    JS_FreeValue(ctx, items);
    return ok;
}

static JSValue js_na_length(JSContext *ctx, JSValueConst this_val, int magic)
{
    (void)magic;
    if (!na_is(ctx, this_val))
        return JS_ThrowTypeError(ctx, "CSSNumericArray.prototype.length was reached on something that is not "
                                      "a CSSNumericArray");
    return JS_NewUint32(ctx, na_length(ctx, this_val));
}

/* The wrapper §4.3.4's `values` attribute answers, minted at the CONSTRUCTOR — see the header for why a mint
   at the read would be both a second object identity and a write performed by a mere read. `items` is
   CONSUMED. */
static JSValue na_new(JSContext *ctx, JSValue items)
{
    JSValue proto, obj;

    DCHECK(g_array_class != 0, "a CSSNumericArray was built before css_math_value_init declared the interface");
    DCHECK(JS_IsArray(items),
           "a CSSNumericArray was built over something that is not an Array — the list is held as one so that "
           "it forks per flow and parks with the flow that holds it");
    proto = JS_GetClassProto(ctx, g_array_class);
    DCHECK(!JS_IsNull(proto), "a CSSNumericArray was built in a realm that never ran its prototype install");
    obj = idl_indexed_new(ctx, proto, &NA_INDEXED);
    JS_FreeValue(ctx, proto);
    CHECK(!JS_IsException(obj), "css-typed-om: a CSSNumericArray could not be allocated");
    JS_DefinePropertyValue(ctx, obj, g_atom_items, items, 0);
    return obj;
}

/* ---- §4.3.4's mint ----------------------------------------------------------------------------------------- */

JSValue css_math_value_items(JSContext *ctx, JSValueConst v)
{
    CssMathValueData *d = mv_of(v);
    JSValue arr;

    DCHECK(d != NULL, "§4.3.4's internal slot was read off something that is not a math value");
    if (!css_math_op_is_list(css_math_value_op(v))) {
        /* A UNARY OPERATOR'S SLOT IS THE VALUE ITSELF, and the callers that walk a tree want a list either way
           — §4.3.1's own arithmetic prepends "the items in this's values internal slot" for a sum and "this"
           otherwise, so a one-item list is the shape that makes both one loop. */
        arr = JS_NewArray(ctx);
        CHECK(!JS_IsException(arr), "css-typed-om: a one-item operand list could not be allocated");
        JS_SetPropertyUint32(ctx, arr, 0, JS_DupValue(ctx, d->slot));
        return arr;
    }
    arr = na_items(ctx, d->slot);
    DCHECK(JS_IsArray(arr),
           "a §4.3.4 list operator's slot does not hold a CSSNumericArray with items — the mint puts one there "
           "and nothing rewrites the slot, so this is a record written past that mint");
    return arr;
}

/* The shape rule both entries below assert, spelled once so the mint and the type fold cannot come apart about
   what an operand list for an operator looks like. */
static void mv_check_operands(JSContext *ctx, CssMathOp op, JSValueConst *items, int n)
{
    int i;

    DCHECK((unsigned)op < (unsigned)CSS_MATH_OP_N, "§4.3.4's mint was asked for an operator it does not name");
    DCHECK(items != NULL && n >= 1,
           "§4.3.4's mint was handed no operands. Its constructors' \"If args is empty, throw a SyntaxError\" "
           "is the CALLER's step because a SyntaxError is a JS exception and this entry is also reached from "
           "§4.3.1's arithmetic, whose operand lists are never empty");
    DCHECK(css_math_op_is_list(op) || n == 1,
           "§4.3.4's mint was handed several operands for an operator whose IDL declares one `CSSNumberish "
           "arg` — negate and invert have a `value` internal slot and no list for the rest to go in");
    for (i = 0; i < n; i++)
        DCHECK(css_numeric_value_is(ctx, items[i]),
               "§4.3.4's mint was handed an operand that is not a CSSNumericValue — every constructor's step 1 "
               "is \"Replace each item of args with the result of rectifying a numberish value for the item\", "
               "and §4.3's rectify answers a CSSNumericValue on both of its branches");
}

/* See css_math_value.h — §4.3.4's type table, which is also the "Let type be …" step of its four list
 * constructors and of §4.3.1's add, mul, min and max.
 *
 * THE FOLD IS LEFT TO RIGHT over the operand list, which is the order §4.3.4 states ("adding the types of each
 * of the items in its values internal slot") and is observable: §4.3.2's addition is not associative, since its
 * percent-hint arm applies a hint to both operands and keeps the first that makes their entries agree.
 *
 * A FAILURE OPERAND MAKES THE ANSWER FAILURE, and that arm is core/css/css_math.h's rather than a test here:
 * add and multiply return false for one and invert returns a failure, so the single-operand cases — a
 * CSSMathNegate, whose type §4.3.4 states is "the same as the type of its value internal slot", and a one-item
 * list, whose "adding the types of all the items" calls nothing — carry it through by simply not looking. */
CssMathType css_math_value_type_fold(JSContext *ctx, CssMathOp op, JSValueConst *items, int n)
{
    CssMathType t;
    int i;

    mv_check_operands(ctx, op, items, n);
    t = css_numeric_value_type_of(ctx, items[0]);
    if (op == CSS_MATH_OP_INVERT) {
        /* "The type is the same as the type of its value internal slot, but with all values negated." */
        t = css_math_type_invert(&t);
    }
    for (i = 1; i < n; i++) {
        CssMathType it = css_numeric_value_type_of(ctx, items[i]), sum;

        /* "…except that in step 3 it multiplies the types instead of adding" is the ONE line §4.3.4 states for
           CSSMathProduct; every other list operator adds. */
        if (op == CSS_MATH_OP_PRODUCT ? !css_math_type_mul(&t, &it, &sum) : !css_math_type_add(&t, &it, &sum))
            return css_math_type_failure();
        t = sum;
    }
    return t;
}

JSValue css_math_value_new(JSContext *ctx, CssMathOp op, JSValueConst *items, int n)
{
    CssMathValueData *d;
    CssMathType t;
    JSValue proto, obj, list;
    int i;

    mv_check_operands(ctx, op, items, n);
    DCHECK(g_class[op] != 0, "a CSSMathValue was built before css_math_value_init declared the interface");
    /* The type is computed here because it has to be STORED (see the header) — never to decide whether to
       mint. A caller with a step 3 has already run the same fold and refused. */
    t = css_math_value_type_fold(ctx, op, items, n);

    proto = JS_GetClassProto(ctx, g_class[op]);
    DCHECK(!JS_IsNull(proto), "a CSSMathValue was built in a realm with no prototype for its interface");
    obj = JS_NewObjectProtoClass(ctx, proto, g_class[op]);
    JS_FreeValue(ctx, proto);
    CHECK(!JS_IsException(obj), "css-typed-om: a CSSMathValue could not be allocated");
    d = calloc(1, sizeof *d);
    CHECK(d != NULL, "css-typed-om: a CSSMathValue's record allocation failed");
    d->type = t;
    if (css_math_op_is_list(op)) {
        list = JS_NewArray(ctx);
        CHECK(!JS_IsException(list), "css-typed-om: a CSSMathValue's operand list could not be allocated");
        for (i = 0; i < n; i++) JS_SetPropertyUint32(ctx, list, (uint32_t)i, JS_DupValue(ctx, items[i]));
        d->slot = na_new(ctx, list);
    } else {
        d->slot = JS_DupValue(ctx, items[0]);
    }
    JS_SetOpaque(obj, d);
    return obj;
}

/* ---- §6.5 CSSMathValue Serialization ------------------------------------------------------------------------ */

/* THE OUTPUT STRING, GROWN. §6.5 appends to one `s` throughout, so the walk below does too; there is no
   intermediate string per node to concatenate and therefore no quadratic assembly of a deep tree. */
typedef struct { char *p; size_t len, cap; } MvBuf;

static void mv_put(MvBuf *b, const char *s)
{
    size_t n = strlen(s);

    if (b->len + n + 1 > b->cap) {
        size_t want = (b->cap ? b->cap * 2 : 64);
        char *grown;

        while (want < b->len + n + 1) want *= 2;
        grown = realloc(b->p, want);
        CHECK(grown != NULL, "css-typed-om: OOM serializing a CSSMathValue");
        b->p = grown;
        b->cap = want;
    }
    memcpy(b->p + b->len, s, n + 1);
    b->len += n;
}

/* THE UNKNOWN LEAVES §6.5 PASSED OVER, IN THE ORDER IT PASSED OVER THEM — the operand list the one derivation
   at the end names. Order and not a set: the leaves appear in the output string in this order, so two trees
   differing only in it serialize to two different strings and must not compose one identity. */
typedef struct { JSValue *v; int n, cap; } MvLeaves;

static void mv_leaf_push(MvLeaves *l, JSContext *ctx, JSValueConst v)
{
    if (l->n == l->cap) {
        int want = l->cap ? l->cap * 2 : 8;
        JSValue *grown = realloc(l->v, (size_t)want * sizeof *grown);

        CHECK(grown != NULL, "css-typed-om: OOM collecting a CSSMathValue's unknown leaves");
        l->v = grown;
        l->cap = want;
    }
    l->v[l->n++] = JS_DupValue(ctx, v);
}

/* §6.3 CSSNumericValue Serialization's FIRST arm, over a leaf this walk has reached: "If this is a
 * CSSUnitValue, serialize a CSSUnitValue from this, passing minimum and maximum." Neither is passed here —
 * §6.5 never passes them — so §6.4's fourth step, the one that wraps the result in `calc(`…`)`, cannot run and
 * steps 1 to 3 are the whole of it.
 *
 * THE NUMBER IS THE LEAF'S EXAMPLE WHERE THE LEAF IS UNKNOWN, which is exactly what core/css/css_unit_value.c
 * does one level down: `idl_number_of` answers the number for a Number and §3.2's conversion RUN ON THAT
 * VALUE'S OWN EXAMPLE for unknown external input, and false when there is no example yet. FALSE here makes the
 * WHOLE serialization example-free — a string with one leaf missing would be a concrete answer this engine did
 * not compute, which is the invention §@H forbids. */
static bool mv_put_leaf(JSContext *ctx, MvBuf *b, MvLeaves *leaves, JSValueConst leaf)
{
    JSValue slot = css_unit_value_value(ctx, leaf);
    double n = 0.0;
    bool have;
    char *s;

    if (concolic_is(slot)) mv_leaf_push(leaves, ctx, slot);
    have = idl_number_of(ctx, IDL_DOUBLE, slot, &n) != 0;
    JS_FreeValue(ctx, slot);
    if (!have) return false;
    s = css_length_serialize_number(n, css_unit_value_suffix(css_unit_value_unit(leaf)));
    DCHECK(s != NULL, "CSSOM §6.7.2's serializer answered nothing at all — it returns an owned string for "
                      "every finite number including zero, so a null is an allocation that was not checked");
    mv_put(b, s);
    free(s);
    return true;
}

/* ONE NODE OF THE WALK. `node` and `items` are OWNED by the frame, which is what makes the stack's lifetime a
   property of the stack rather than of the tree it is walking. */
typedef struct {
    JSValue   node;
    JSValue   items;    /* the operand list, always as an Array — css_math_value_items' one-item shape included */
    CssMathOp op;
    uint32_t  i, n;
    bool      nested, parenless, opened;
} MvFrame;

/* §6.5's OPENER, which is one sentence repeated for five of the six arms: "If paren-less is true, continue to
   the next step; otherwise, if nested is true, append "(" to s; otherwise, append "calc(" to s." The min/max
   arm is the one that does not have it — it appends "min(" or "max(" unconditionally — which is also why it is
   the only arm whose closing ")" is unconditional. */
static void mv_open(MvBuf *b, const MvFrame *f)
{
    if (f->op == CSS_MATH_OP_MIN) { mv_put(b, "min("); return; }
    if (f->op == CSS_MATH_OP_MAX) { mv_put(b, "max("); return; }
    if (!f->parenless) mv_put(b, f->nested ? "(" : "calc(");
    /* The two unary arms' own second step, which stands between the opener and the operand: "Append "-" to s"
       for a negate, "Append "1 / " to s" for an invert. */
    if (f->op == CSS_MATH_OP_NEGATE) mv_put(b, "-");
    if (f->op == CSS_MATH_OP_INVERT) mv_put(b, "1 / ");
}

static void mv_close(MvBuf *b, const MvFrame *f)
{
    if (f->op == CSS_MATH_OP_MIN || f->op == CSS_MATH_OP_MAX) { mv_put(b, ")"); return; }
    if (!f->parenless) mv_put(b, ")");   /* "If paren-less is false, append ")" to s" */
}

JSValue css_math_value_serialize(JSContext *ctx, JSValueConst v)
{
    MvBuf buf = { NULL, 0, 0 };
    MvLeaves leaves = { NULL, 0, 0 };
    MvFrame *stk = NULL;
    int depth = 0, cap = 0, i;
    bool have_example = true;
    JSValue example, r;

    DCHECK(css_math_value_is(v),
           "§6's serialize-a-CSSStyleValue reached §6.5's arm on something that is not a CSSMathValue — the "
           "caller asks css_math_value_is before it asks this");
    mv_put(&buf, "");   /* the empty string §6.5 step 1 starts `s` at, materialised so `buf.p` is never NULL */

    /* THE WALK IS ITERATIVE AND ITS STACK IS ON THE HEAP, WHICH IS NOT A STYLE CHOICE. §6.5 is stated
       recursively over a tree whose DEPTH THE PAGE PICKS (`for (…) v = new CSSMathNegate(v)`), so a C function
       that called itself here would be a self-contained recursion of unbounded depth — the shape
       engine/check_recursion.mjs fails by name and CLAUDE.md's flat-C-stack rule forbids. There is no depth
       CAP either: the stack grows until the allocation floor, which is the honest limit. */
    for (;;) {
        MvFrame *f;

        if (depth == cap) {
            int want = cap ? cap * 2 : 16;
            MvFrame *grown = realloc(stk, (size_t)want * sizeof *grown);

            CHECK(grown != NULL, "css-typed-om: OOM walking a CSSMathValue for §6.5");
            stk = grown;
            cap = want;
        }
        if (depth == 0) {
            /* The root, at §6.5's own defaults: "nested, a boolean (defaulting to false if unspecified),
               paren-less, a boolean (defaulting to false if unspecified)". */
            stk[0].node = JS_DupValue(ctx, v);
            stk[0].op = css_math_value_op(v);
            stk[0].items = css_math_value_items(ctx, v);
            stk[0].i = 0;
            stk[0].nested = false;
            stk[0].parenless = false;
            stk[0].opened = false;
            {
                JSValue len = JS_GetPropertyStr(ctx, stk[0].items, "length");
                uint32_t n = 0;
                JS_ToUint32(ctx, &n, len);
                JS_FreeValue(ctx, len);
                stk[0].n = n;
            }
            depth = 1;
        }
        f = &stk[depth - 1];
        if (!f->opened) { f->opened = true; mv_open(&buf, f); continue; }
        if (f->i < f->n) {
            JSValue child = JS_GetPropertyUint32(ctx, f->items, f->i);
            JSValue descend;
            bool child_nested = true, child_parenless = false;

            DCHECK(css_numeric_value_is(ctx, child),
                   "a §4.3.4 operand list held something that is not a CSSNumericValue — the mint rectifies "
                   "every item, so anything else was put there past it");
            /* §6.5's SEPARATORS, each stated on its own arm. The sum and product arms are the two that read
               the child's INTERFACE to choose one: "If arg is a CSSMathNegate, append " - " to s, then
               serialize arg's value internal slot with nested set to true", and the same sentence with
               CSSMathInvert and " / " for a product. */
            descend = JS_DupValue(ctx, child);
            switch (f->op) {
            case CSS_MATH_OP_MIN:
            case CSS_MATH_OP_MAX:
                if (f->i > 0) mv_put(&buf, ", ");
                /* "serialize arg with nested and paren-less both true" */
                child_parenless = true;
                break;
            case CSS_MATH_OP_SUM:
                if (f->i > 0) {
                    bool neg = css_math_value_is(child) && css_math_value_op(child) == CSS_MATH_OP_NEGATE;
                    mv_put(&buf, neg ? " - " : " + ");
                    if (neg) {
                        CssMathValueData *cd = mv_of(child);
                        JS_FreeValue(ctx, descend);
                        descend = JS_DupValue(ctx, cd->slot);
                    }
                }
                break;
            case CSS_MATH_OP_PRODUCT:
                if (f->i > 0) {
                    bool inv = css_math_value_is(child) && css_math_value_op(child) == CSS_MATH_OP_INVERT;
                    mv_put(&buf, inv ? " / " : " * ");
                    if (inv) {
                        CssMathValueData *cd = mv_of(child);
                        JS_FreeValue(ctx, descend);
                        descend = JS_DupValue(ctx, cd->slot);
                    }
                }
                break;
            case CSS_MATH_OP_NEGATE:
            case CSS_MATH_OP_INVERT:
                /* The operand follows the "-" or "1 / " the opener already wrote; there is no separator. */
                break;
            case CSS_MATH_OP_N:
                DFAIL("a §6.5 walk frame carries no operator");
                break;
            }
            JS_FreeValue(ctx, child);
            f->i++;
            if (!css_math_value_is(descend)) {
                /* §6.3's CSSUnitValue arm — the leaf the tree terminates in. */
                if (!mv_put_leaf(ctx, &buf, &leaves, descend)) have_example = false;
                JS_FreeValue(ctx, descend);
                continue;
            }
            /* THE PUSH NEEDS NO GROWTH CHECK OF ITS OWN: the top of the loop grows whenever `depth == cap`,
               so `cap > depth` holds at every point below it and `stk[depth]` is always in range. A second
               realloc here would also invalidate `f`, which is the shape that turns a stack walk into a
               use-after-free the first time a tree is deep enough to grow. */
            stk[depth].node = descend;
            stk[depth].op = css_math_value_op(descend);
            stk[depth].items = css_math_value_items(ctx, descend);
            stk[depth].i = 0;
            stk[depth].nested = child_nested;
            stk[depth].parenless = child_parenless;
            stk[depth].opened = false;
            {
                JSValue len = JS_GetPropertyStr(ctx, stk[depth].items, "length");
                uint32_t n = 0;
                JS_ToUint32(ctx, &n, len);
                JS_FreeValue(ctx, len);
                stk[depth].n = n;
            }
            depth++;
            continue;
        }
        mv_close(&buf, f);
        JS_FreeValue(ctx, f->node);
        JS_FreeValue(ctx, f->items);
        if (--depth == 0) break;
    }
    free(stk);

    /* THE CONCRETE HALF — the REAL §6.5 run over the leaves' own examples, and JS_UNDEFINED where any leaf had
       none. A string assembled with one leaf missing would be a concrete answer this engine never computed. */
    example = have_example ? JS_NewStringLen(ctx, buf.p, buf.len) : JS_UNDEFINED;
    free(buf.p);
    if (leaves.n == 0) {
        DCHECK(have_example,
               "§6.5 produced no serialization for a tree with no unknown leaf — every leaf was then a Number, "
               "which has an example by definition, so this is idl_number_of's two answers having come apart");
        return example;
    }
    /* SOME LEAF IS UNKNOWN EXTERNAL INPUT, SO THE SERIALIZATION IS TOO. A concrete string here would DE-TAINT
       what `new CSSMathSum(CSS.px(attackerNumber), CSS.em(1))` carries into whatever consumes its
       stringification. The derivation names EVERY unknown leaf in the order §6.5 wrote them, which is what
       keeps two trees differing only in one leaf two questions rather than one — see css_math_value.h. */
    r = concolic_new_derived(ctx, "CSS Typed OM 1 §6.5 serialize a CSSMathValue",
                             (const JSValueConst *)leaves.v, leaves.n, example);
    DCHECK(!JS_IsUninitialized(r),
           "solver/concolic.h's derivation refused an operand list this walk had already established holds "
           "unknown external input — the two tests read the same values one loop apart");
    for (i = 0; i < leaves.n; i++) JS_FreeValue(ctx, leaves.v[i]);
    free(leaves.v);
    return r;
}

/* ---- §4.3.4's members --------------------------------------------------------------------------------------- */

/* "The operator attribute of a CSSMathValue this must, on getting, return the following string, depending on
   the interface of this" — the table MV above holds, so the interface and the string are one row. */
static JSValue js_mv_operator(JSContext *ctx, JSValueConst this_val, int magic)
{
    CssMathValueData *d = mv_here(ctx, this_val);

    (void)magic;
    if (!d) return JS_EXCEPTION;
    return JS_NewString(ctx, MV[css_math_value_op(this_val)].op);
}

/* §4.3.4's `readonly attribute CSSNumericArray values` and `readonly attribute CSSNumericValue value` — one
   body, because both are "return the internal slot" over a slot whose SHAPE the operator already decides. */
static JSValue js_mv_slot(JSContext *ctx, JSValueConst this_val, int magic)
{
    CssMathValueData *d = mv_here(ctx, this_val);

    (void)magic;
    if (!d) return JS_EXCEPTION;
    return JS_DupValue(ctx, d->slot);
}

/* §4.3.4's SIX CONSTRUCTORS AS ONE BODY. Their steps are one algorithm the section states once and then
   varies twice, in its own words: the min/max pair is "defined identically to the above, except that in the
   last step they return a new CSSMathMin or CSSMathMax object", the product is "defined identically to the
   above, except that in step 3 it multiplies the types instead of adding", and the negate/invert pair is the
   same shape over one `arg` with no type step of its own. `magic` is the operator. */
static JSValue js_mv_ctor(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    CssMathOp op = (CssMathOp)magic;
    JSValue *items, r;
    int n, i;

    (void)this_val;
    DCHECK((unsigned)op < (unsigned)CSS_MATH_OP_N,
           "a §4.3.4 constructor was installed with a magic naming no operator");
    if (!css_math_op_is_list(op)) {
        DCHECK(argc == 1,
               "a §4.3.4 unary constructor reached its body with an argument count its IDL does not declare — "
               "`CSSNumberish arg` is its one required position, so the conversion machine owed it exactly one");
        /* Step 1: "Replace arg with the result of rectifying a numberish value for arg." There is no type
           step: §4.3.4 states none for CSSMathNegate or CSSMathInvert, whose types are read off the operand. */
        items = malloc(sizeof *items);
        CHECK(items != NULL, "css-typed-om: OOM in a §4.3.4 constructor");
        items[0] = css_numeric_value_rectify(ctx, argv[0]);
        n = 1;
    } else {
        /* Step 2: "If args is empty, throw a SyntaxError." A variadic tail is not a required position, so
           §3.6's argument-count check cannot refuse this call and the section states the refusal itself. */
        if (argc == 0)
            return JS_ThrowDOMException(ctx, "SyntaxError",
                                        "%s requires at least one argument — CSS Typed OM 1 §4.3.4's "
                                        "constructor throws a SyntaxError for an empty argument list",
                                        MV[op].iface);
        items = malloc((size_t)argc * sizeof *items);
        CHECK(items != NULL, "css-typed-om: OOM in a §4.3.4 constructor");
        /* Step 1: "Replace each item of args with the result of rectifying a numberish value for the item." */
        for (i = 0; i < argc; i++) items[i] = css_numeric_value_rectify(ctx, argv[i]);
        n = argc;
    }
    /* Step 3, WHICH IS THIS SECTION'S AND NOT THE MINT'S: "Let type be the result of adding the types of all
       the items of args. If type is failure, throw a TypeError." It runs BEFORE the mint because that is the
       order §4.3.4 states it in, and it runs for the four LIST constructors only — the CSSMathNegate and
       CSSMathInvert constructors state two steps and neither is a type step, so a failure-typed operand builds
       a failure-typed object there rather than being refused. That asymmetry is the spec's, and it is why the
       type step could not stay inside the mint: §4.3.1's toSum reaches the same mint with no step 3 at all. */
    if (css_math_op_is_list(op)) {
        CssMathType t = css_math_value_type_fold(ctx, op, items, n);

        if (css_math_type_is_failure(&t)) {
            for (i = 0; i < n; i++) JS_FreeValue(ctx, items[i]);
            free(items);
            return JS_ThrowTypeError(ctx, "the arguments to %s do not have a combined CSS numeric type — "
                                          "CSS Typed OM 1 §4.3.2's %s of their types returns failure",
                                     MV[op].iface,
                                     op == CSS_MATH_OP_PRODUCT ? "multiplying" : "adding");
        }
    }
    r = css_math_value_new(ctx, op, items, n);
    for (i = 0; i < n; i++) JS_FreeValue(ctx, items[i]);
    free(items);
    return r;
}

/* ---- the per-realm install ---------------------------------------------------------------------------------- */

void css_math_value_install_realm(JSContext *ctx, JSValueConst numeric_proto)
{
    JSValue mv_proto, na_proto, proto, ctor, global, prev;
    unsigned k;

    DCHECK(g_abstract_class != 0, "a realm asked for CSSMathValue before the interface was declared");
    prev = JS_GetClassProto(ctx, g_abstract_class);
    DCHECK(JS_IsNull(prev), "css_math_value_install_realm ran twice in one realm");
    JS_FreeValue(ctx, prev);
    DCHECK(JS_IsObject(numeric_proto),
           "§4.3.4's install was handed something that is not this realm's CSSNumericValue.prototype — Web IDL "
           "§3.7.3 makes it the proto of every interface here, and core/idl_args.c asserts that link by reading "
           "the §3.7.3 Interface prototype object tag back off it");
    global = JS_GetGlobalObject(ctx);

    /* §4.3.4's CSSMathValue — abstract, so Web IDL §3.7.1 Interface object's rule that an interface with NO
       constructor still has an interface object whose call is a TypeError is exactly what a page reading
       `x instanceof CSSMathValue` and `window.CSSMathValue` gets. NOT IN QUOTATION MARKS: that sentence is
       this tree's own statement of the rule (core/css/css_unit_value.c writes it the same way), and
       engine/citegen.mjs's quotation channel reported it as a quoted run §3.7.1 does not contain when it was
       punctuated as one — which is the fabricated-quotation shape whether or not the claim is true. */
    mv_proto = JS_NewObjectProto(ctx, numeric_proto);
    CHECK(!JS_IsException(mv_proto), "CSSMathValue.prototype could not be allocated");
    idl_interface_tag(ctx, mv_proto, "CSSMathValue");
    idl_install_accessor_no_user_code(ctx, mv_proto, "operator", js_mv_operator, 0, -1);
    ctor = idl_interface_object(ctx, "CSSMathValue", mv_proto);
    CHECK(!JS_IsException(ctor), "the CSSMathValue interface object could not be allocated");
    idl_define_global_property_reference(ctx, global, "CSSMathValue", ctor);
    JS_SetClassProto(ctx, g_abstract_class, JS_DupValue(ctx, mv_proto));

    /* §4.3.4's CSSNumericArray. Its IDL declares no inheritance, so §3.7.3's last arm puts its interface
       prototype object over this realm's %Object.prototype%, which JS_NewObject already gives. */
    na_proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(na_proto), "CSSNumericArray.prototype could not be allocated");
    idl_interface_tag(ctx, na_proto, "CSSNumericArray");
    idl_install_accessor_no_user_code(ctx, na_proto, "length", js_na_length, 0, -1);
    /* Web IDL §3.7.9 Iterable declarations' define-the-iteration-methods, both clauses: step 1.1 for the
       indexed property getter and the integer `length`, and its step 1.2 because §4.3.4 declares
       `iterable<CSSNumericValue>` — which is what gives a CSSNumericArray `entries`, `keys`, `values` and
       `forEach` where an interface with only a getter has none. */
    idl_indexed_install_iterable(ctx, na_proto);
    idl_indexed_install_value_iterator(ctx, na_proto);
    ctor = idl_interface_object(ctx, "CSSNumericArray", na_proto);
    CHECK(!JS_IsException(ctor), "the CSSNumericArray interface object could not be allocated");
    idl_define_global_property_reference(ctx, global, "CSSNumericArray", ctor);
    JS_SetClassProto(ctx, g_array_class, na_proto);   /* CONSUMES na_proto */

    /* The six constructible subclasses, each over CSSMathValue.prototype for §3.7.3's inheritance arm. */
    for (k = 0; k < CSS_MATH_OP_N; k++) {
        proto = JS_NewObjectProto(ctx, mv_proto);
        CHECK(!JS_IsException(proto), "a CSSMathValue subclass prototype could not be allocated");
        idl_interface_tag(ctx, proto, MV[k].iface);
        idl_install_accessor_no_user_code(ctx, proto, MV[k].slot, js_mv_slot, 0, -1);
        ctor = idl_step_constructor(ctx, MV[k].iface, g_id_ctor[k]);
        CHECK(!JS_IsException(ctor), "a CSSMathValue subclass interface object could not be allocated");
        JS_SetConstructor(ctx, ctor, proto);
        idl_define_global_property_reference(ctx, global, MV[k].iface, ctor);
        JS_SetClassProto(ctx, g_class[k], proto);   /* CONSUMES proto */
    }
    JS_FreeValue(ctx, mv_proto);
    JS_FreeValue(ctx, global);
}

void css_math_value_init(JSContext *ctx)
{
    /* `constructor(CSSNumberish... args)` and `constructor(CSSNumberish arg)` — ONE declared position carrying
       the union, which for the variadic four applies to every argument from there on. §3.7.1 Interface
       object's `length` is 0 for a variadic tail and 1 for the unary pair's required position. */
    static const IdlArgType ONE_NUMBERISH[1] = { IDL_DOUBLE_UNLESS_IFACE };
    static const char NV_IFACE[] = "CSSNumericValue";
    JSClassDef abstract_def = { "CSSMathValue" };
    JSClassDef array_def = { "CSSNumericArray" };
    unsigned k;

    DCHECK(g_abstract_class == 0,
           "css_math_value_init ran twice — the interfaces are declared once per AGENT, and a second set of "
           "class ids would leave every math value already built branded with the first");
    JS_NewClassID(JS_GetRuntime(ctx), &g_abstract_class);
    CHECK(JS_NewClass(JS_GetRuntime(ctx), g_abstract_class, &abstract_def) == 0,
          "CSSMathValue: the class could not be declared");
    JS_NewClassID(JS_GetRuntime(ctx), &g_array_class);
    CHECK(JS_NewClass(JS_GetRuntime(ctx), g_array_class, &array_def) == 0,
          "CSSNumericArray: the class could not be declared");
    g_items_key = JS_NewSymbol(ctx, "cssNumericArrayItems", false);
    CHECK(!JS_IsException(g_items_key), "the CSSNumericArray slot key allocation failed");
    g_atom_items = JS_ValueToAtom(ctx, g_items_key);
    CHECK(g_atom_items != JS_ATOM_NULL, "the CSSNumericArray slot key could not be interned");

    for (k = 0; k < CSS_MATH_OP_N; k++) {
        /* THE CLASS NAME IS THE INTERFACE'S, so a `JS_DumpValue` of one of these names the interface rather
           than a family — the same reason each subclass gets its own class id at all. */
        JSClassDef d = { MV[k].iface, mv_finalizer, mv_gc_mark };

        JS_NewClassID(JS_GetRuntime(ctx), &g_class[k]);
        CHECK(JS_NewClass(JS_GetRuntime(ctx), g_class[k], &d) == 0,
              "a CSSMathValue subclass could not be declared");
        g_id_ctor[k] = idl_method_id(ctx, ONE_NUMBERISH, 1, js_mv_ctor, (int)k);
        if (css_math_op_is_list((CssMathOp)k)) idl_variadic();
        /* §3.2.15's `I` for the union's arm, as a PREDICATE and not a class — core/idl_args.h's own split, and
           core/css/css_numeric_value.h's entry, which is the disjunction over every subclass that exists. */
        idl_arg_iface(0, css_numeric_value_is, NV_IFACE);
        DCHECKF(g_id_ctor[k] >= 0, "§4.3.4's constructor %u did not enter the argument pool", k);
        agent_state_class("css_math_value", &g_class[k], "a CSS Typed OM 1 §4.3.4 subclass, and its brand");
        agent_state_id("css_math_value", &g_id_ctor[k], "a CSS Typed OM 1 §4.3.4 constructor declaration");
    }
    agent_state_class("css_math_value", &g_abstract_class,
                      "CSS Typed OM 1 §4.3.4's abstract CSSMathValue class, and this component's latch");
    agent_state_class("css_math_value", &g_array_class, "CSS Typed OM 1 §4.3.4's CSSNumericArray class");
    agent_state_value("css_math_value", &g_items_key, "the private Symbol a CSSNumericArray's Array hangs off");
    agent_state_atom("css_math_value", &g_atom_items, "that Symbol, interned");
    /* NO realm_declare_intrinsic HERE. §3.7.3 makes this chain one object graph with core/css/css_unit_value.c's
       three, so that component's realm install calls css_math_value_install_realm with the
       CSSNumericValue.prototype it has just built — one place creates the chain, and a second intrinsic
       ordered independently would be two files that have to agree about an order. */
}

void css_math_value_free(JSRuntime *rt)
{
    unsigned k;

    DCHECK(g_abstract_class != 0,
           "CSS Typed OM 1 §4.3.4 was released in an agent that never declared it");
    for (k = 0; k < CSS_MATH_OP_N; k++) { g_class[k] = 0; g_id_ctor[k] = -1; }
    JS_FreeAtomRT(rt, g_atom_items);
    g_atom_items = JS_ATOM_NULL;
    JS_FreeValueRT(rt, g_items_key);
    g_items_key = JS_UNDEFINED;
    g_array_class = 0;
    g_abstract_class = 0;   /* the latch the init above consults — see core/agent_state.h */
}
