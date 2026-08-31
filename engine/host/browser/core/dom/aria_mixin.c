/* ARIAMixin — WAI-ARIA 1.3 §"reflection" over HTML §2.6.1's processing models. See aria_mixin.h for the two
 * halves and why they are two.
 *
 * WHAT MAKES THE ELEMENT-REFLECTING HALF A MACHINE. §2.6.1's "get the attr-associated elements" resolves each
 * id token to "the first element, in tree order, that meets the following criteria: candidate's root is the
 * same as element's root; candidate's ID is id" — a walk of the whole tree the element is in, for a member a
 * page reads in a loop. That is work of the DOCUMENT'S size inside a property read, so it is declared the way
 * §4.2.4's getElementById is: a step machine that rests on one node per step and can be preempted, parked and
 * resumed at whatever node it reached. One walk answers every token, because a walk per token is the same walk
 * repeated — each node is asked whether its ID is one of the ids still unmatched, and the walk ends as soon as
 * they all are.
 *
 * WHERE THE EXPLICIT VALUE LIVES. §2.6.1's "explicitly set attr-element(s)" and its cached FrozenArray belong
 * to the REFLECTED TARGET and to one member, and they are JS VALUES on that target's own object — the element's
 * wrapper, or the ElementInternals — under a Symbol this component minted
 * and never published — the same answer element_internals.c gives for the same question, and for the reason the
 * project's rules state: two forked arms must be able to disagree about them, and a parked flow has to carry
 * them to the cold tier and back. A malloc'd side table would be a pointer no snapshot can name. */
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "solver/concolic.h"
#include "solver/dom_cow.h"
#include "core/idl_args.h"
#include "core/dom/aria_mixin.h"
#include "core/dom/element.h"
#include "core/dom/node.h"
#include "core/dom/shadow_root.h"

/* §2.6.1's `DOMString?` members, handed to §4.9's ONE reflection registry — the mechanism this engine already
   has for "the IDL property IS the content attribute", under the kind that states the `?`. */
#define ARIA_STR_ROW(idl, attr) { idl, attr, REFLECT_STRING_NULLABLE },
static const ElReflect ARIA_STRINGS[] = { ARIA_STRING_MEMBERS(ARIA_STR_ROW) };
#define ARIA_STRING_N ((int)(sizeof(ARIA_STRINGS) / sizeof(ARIA_STRINGS[0])))

#define ARIA_EL_ROW(idl, attr, plural) { idl, attr, plural },
static const struct { const char *idl, *attr; int plural; } ARIA_ELS[] = { ARIA_ELEMENT_MEMBERS(ARIA_EL_ROW) };
#define ARIA_EL_N ((int)(sizeof(ARIA_ELS) / sizeof(ARIA_ELS[0])))

/* The per-element state slot: a Symbol nobody published, so §2.6.1's explicit value is not a string property of
   this engine's invention sitting where `Object.keys` reports it.
   THE REFERENCE IS STRONG WHERE §2.6.1 SAYS WEAK — a KNOWN DEVIATION, stated here because nothing can assert
   it: the spec holds "a weak reference to the given value" so an assigned element that leaves the tree can be
   collected, and this holds an ordinary reference from the assigning element's wrapper. No page can observe the
   difference (weakness is unobservable to script, and no member here is reachable from a FinalizationRegistry
   or a WeakRef), so it is a LIFETIME cost and not a wrong answer: an element assigned to `ariaOwnsElements`
   lives as long as the element that names it. It becomes real the day this engine grows weak references at the
   host level, and that is the diff that removes this paragraph. */
static JSValue g_state_key = JS_UNDEFINED;
static JSAtom  g_atom_state = JS_ATOM_NULL;
/* Declared once per AGENT, installed per realm. The ids are per (target kind, member), because which target a
   member answers for is decided by the DECLARATION — the prototype it is installed on — and never re-derived
   from the receiver at the read. Asking the receiver would answer for an Element handed to
   ElementInternals.prototype's getter, which is exactly the brand hole §3.7.5 exists to close. */
static int g_refl_base = -1;
static int g_id_el_get[ARIA_TARGET_KINDS][ARIA_EL_N], g_id_el_set[ARIA_TARGET_KINDS][ARIA_EL_N];
static const AriaTargetOps *g_ops[ARIA_TARGET_KINDS];

/* THE MAGIC IS THE PAIR (target kind, member), because one machine and one setter body serve both interfaces
   and a magic is exactly how this engine passes a declaration's identity to a shared body. */
#define ARIA_MAGIC(kind, i) ((int)(kind) * ARIA_EL_N + (i))
#define ARIA_MAGIC_KIND(m)  ((AriaTargetKind)((m) / ARIA_EL_N))
#define ARIA_MAGIC_MEMBER(m) ((m) % ARIA_EL_N)

static const AriaTargetOps *aria_ops(AriaTargetKind kind)
{
    DCHECK(kind >= 0 && kind < ARIA_TARGET_KINDS && g_ops[kind] != NULL,
           "an ARIAMixin member ran for a reflected target whose four algorithms were never declared — "
           "aria_mixin_declare_target is what states them, beside the members of the interface that has them");
    return g_ops[kind];
}

void aria_mixin_declare_target(AriaTargetKind kind, const AriaTargetOps *ops)
{
    DCHECK(kind >= 0 && kind < ARIA_TARGET_KINDS, "an ARIAMixin target kind §2.6.1 does not name was declared");
    DCHECK(ops && ops->element && ops->attr_get && ops->attr_set && ops->attr_del,
           "a reflected target was declared with fewer than §2.6.1's four algorithms");
    DCHECK(g_ops[kind] == NULL, "a reflected target's algorithms were declared twice");
    g_ops[kind] = ops;
}

/* ---- §2.6.1's four algorithms FOR AN ELEMENT TARGET -------------------------------------------------------
 * "For a reflected target that is an element element": get the element returns the element itself, and the
 * three attribute operations are §4.9's, through the chokepoint every other attribute write in this engine
 * goes through — so a reflection stays captured in the running flow's DOM delta. */
static JSValue aria_element_target(JSContext *ctx, JSValueConst target)
{
    if (!element_of_value(target)) return JS_UNDEFINED;   /* not an element: §3.7.5's brand, answered by absence */
    return JS_DupValue(ctx, target);
}

/* No wrapper for the get and the set: element_attr_get and element_attr_set ARE §2.6.1's two algorithms for an
   element target, with the signatures this type states — a forwarding body would only be somewhere for the two
   to disagree. The delete needs one because the chokepoint takes the Lexbor element rather than the wrapper. */
static void aria_element_attr_del(JSContext *ctx, JSValueConst target, const char *name)
{
    lxb_dom_element_t *el = element_of_value(target);

    DCHECK(el != NULL, "an element target deleted a content attribute with no element — the brand test above "
                       "is what makes that impossible");
    dom_cow_remove_attribute(el, name);
}

static const AriaTargetOps ARIA_ELEMENT_OPS = {
    aria_element_target, element_attr_get, element_attr_set, aria_element_attr_del
};

/* ---- §2.6.1's per-TARGET, per-member state ---------------------------------------------------------------
 * One record per content attribute, a two-element Array: [0] the explicitly set attr-element(s) — an element
 * for the singular member, an Array of elements for a plural one, and UNDEFINED for the spec's null; [1] the
 * cached attr-associated elements object, which is what keeps `el.ariaOwnsElements === el.ariaOwnsElements`
 * true. The cached LIST the spec keeps beside it is not a second field: the comparison is over CONTENTS and the
 * frozen array's own contents are the contents it was built from. */
static JSValue aria_record(JSContext *ctx, JSValueConst el, int i, bool create)
{
    JSValue map, rec;

    DCHECK(i >= 0 && i < ARIA_EL_N,
           "an ARIAMixin element reflection asked for state for a member the mixin does not declare");
    if (!JS_IsObject(el)) return JS_UNDEFINED;
    if (JS_GetOwnSlot(ctx, &map, el, g_atom_state) <= 0) map = JS_UNDEFINED;
    if (!JS_IsObject(map)) {
        JS_FreeValue(ctx, map);
        if (!create) return JS_UNDEFINED;
        map = JS_NewObjectProto(ctx, JS_NULL);
        CHECK(!JS_IsException(map), "the ARIAMixin element-reflection state could not be allocated");
        JS_DefinePropertyValue(ctx, (JSValue)el, g_atom_state, JS_DupValue(ctx, map),
                               JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE);
    }
    rec = JS_GetPropertyStr(ctx, map, ARIA_ELS[i].attr);
    if (!JS_IsObject(rec)) {
        JS_FreeValue(ctx, rec);
        if (!create) { JS_FreeValue(ctx, map); return JS_UNDEFINED; }
        rec = JS_NewArray(ctx);
        CHECK(!JS_IsException(rec), "an ARIAMixin element-reflection record could not be allocated");
        JS_SetPropertyUint32(ctx, rec, 0, JS_UNDEFINED);
        JS_SetPropertyUint32(ctx, rec, 1, JS_UNDEFINED);
        JS_SetPropertyStr(ctx, map, ARIA_ELS[i].attr, JS_DupValue(ctx, rec));
    }
    JS_FreeValue(ctx, map);
    return rec;
}

/* The explicitly set attr-element(s), OWNED — UNDEFINED for §2.6.1's null. */
static JSValue aria_explicit(JSContext *ctx, JSValueConst el, int i)
{
    JSValue rec = aria_record(ctx, el, i, false), v;

    if (!JS_IsObject(rec)) { JS_FreeValue(ctx, rec); return JS_UNDEFINED; }
    v = JS_GetPropertyUint32(ctx, rec, 0);
    JS_FreeValue(ctx, rec);
    return v;
}

/* Takes ownership of `v`; UNDEFINED is §2.6.1's null. */
static void aria_set_explicit(JSContext *ctx, JSValueConst el, int i, JSValue v)
{
    JSValue rec = aria_record(ctx, el, i, true);

    if (!JS_IsObject(rec)) { JS_FreeValue(ctx, rec); JS_FreeValue(ctx, v); return; }
    JS_SetPropertyUint32(ctx, rec, 0, v);
    JS_FreeValue(ctx, rec);
}

/* §2.6.1: "if X is a descendant of any of element's shadow-including ancestors". Read from the CANDIDATE's
   side, which is the same sentence and one walk instead of two: X qualifies when some proper ancestor of X is a
   shadow-including ancestor of `el`. An element assigned from another document has none and is dropped, which
   is what stops an explicit assignment from naming a node in a tree the page cannot relate this one to. */
static bool aria_reachable(lxb_dom_node_t *el, lxb_dom_node_t *cand)
{
    lxb_dom_node_t *a;

    if (!el || !cand) return false;
    for (a = cand->parent; a; a = a->parent)
        if (a != el && shadow_root_is_shadow_including_inclusive_ancestor(a, el)) return true;
    return false;
}

/* Do two element lists hold the same elements in the same order — §2.6.1's "the contents of elements is equal
   to the contents of this's cached attr-associated elements". By the NODE the wrapper is for, because that is
   what the identity of an element is; two wrappers for one node cannot exist, and comparing values would still
   be the same question one indirection later. */
static bool aria_same_contents(JSContext *ctx, JSValueConst a, JSValueConst b)
{
    uint32_t na = 0, nb = 0, i;
    JSValue va, vb;
    bool same = true;

    if (JS_ToUint32(ctx, &na, (va = JS_GetPropertyStr(ctx, a, "length"))) < 0) na = 0;
    JS_FreeValue(ctx, va);
    if (JS_ToUint32(ctx, &nb, (vb = JS_GetPropertyStr(ctx, b, "length"))) < 0) nb = 0;
    JS_FreeValue(ctx, vb);
    if (na != nb) return false;
    for (i = 0; i < na && same; i++) {
        va = JS_GetPropertyUint32(ctx, a, i);
        vb = JS_GetPropertyUint32(ctx, b, i);
        same = element_of_value(va) == element_of_value(vb);
        JS_FreeValue(ctx, va);
        JS_FreeValue(ctx, vb);
    }
    return same;
}

/* §2.6.1's FrozenArray getter steps, over a list this member just computed. Takes ownership of `list`. */
static JSValue aria_frozen(JSContext *ctx, JSValueConst target, int i, JSValue list)
{
    JSValue rec = aria_record(ctx, target, i, true), cached;

    DCHECK(ARIA_ELS[i].plural, "a singular element reflection asked for §3.2.27's frozen array");
    if (!JS_IsObject(rec)) { JS_FreeValue(ctx, rec); return list; }
    cached = JS_GetPropertyUint32(ctx, rec, 1);
    if (JS_IsObject(cached) && aria_same_contents(ctx, cached, list)) {
        JS_FreeValue(ctx, list);
        JS_FreeValue(ctx, rec);
        return cached;   /* the SAME object — the invariant the extra caching layer exists for */
    }
    JS_FreeValue(ctx, cached);
    if (idl_freeze_array(ctx, list) < 0) {
        JS_FreeValue(ctx, list);
        JS_FreeValue(ctx, rec);
        return JS_EXCEPTION;
    }
    JS_SetPropertyUint32(ctx, rec, 1, JS_DupValue(ctx, list));
    JS_FreeValue(ctx, rec);
    return list;
}

/* ---- the walk ---------------------------------------------------------------------------------------------- */

#define ARIA_EL_STAGES(X) \
    X(ARIA_EL_START, "WAI-ARIA reflection: the explicitly set attr-element(s), or the ids to resolve") \
    X(ARIA_EL_WALK,  "WAI-ARIA reflection: the first element in tree order per id, one node per step")
enum { IDL_STEP_STAGE_BASE(ARIA_EL_STAGES) ARIA_EL_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const ARIA_EL_STEPS[] = { ARIA_EL_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    lxb_dom_node_t *root;     /* the tree the ids are resolved in — "candidate's root is element's root" */
    lxb_dom_node_t *cursor;   /* how far the walk has got: the resume point */
    char   *ids;              /* the content attribute's value, split in place into `ntok` NUL-terminated ids */
    size_t  idscap;           /* its allocation, which is what a fork copies */
    int     ntok;
    int     nmatched;         /* how many ids have their element — the walk ends when every one does */
    JSValue slots;            /* an Array, one entry per id, in the ATTRIBUTE'S order: its element or null */
} AriaElState;

static void aria_el_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    AriaElState *s = st;
    /* The cursors are Lexbor nodes and belong to the document; the id buffer is this machine's own copy,
       because the string it came from is released before the first suspension and two forked arms must not
       share one buffer either of them frees. */
    v->buf(ctx, (void **)&s->ids, s->ids ? s->idscap : 0);
    v->val(ctx, &s->slots);
}

/* The elements this member answers with, from `s->slots` — the ids that found nothing are DROPPED, which is
   §2.6.1's "if no such element exists, then continue". Takes over `s->slots`. */
static JSValue aria_collect(JSContext *ctx, AriaElState *s)
{
    JSValue out = JS_NewArray(ctx);
    uint32_t n = 0;
    int i;

    CHECK(!JS_IsException(out), "an ARIAMixin element list could not be allocated");
    for (i = 0; i < s->ntok; i++) {
        JSValue v = JS_GetPropertyUint32(ctx, s->slots, (uint32_t)i);

        if (JS_IsObject(v)) JS_SetPropertyUint32(ctx, out, n++, v);
        else JS_FreeValue(ctx, v);
    }
    return out;
}

/* §2.6.1's first branch: something was explicitly set, so nothing is resolved and no walk happens. */
static JSValue aria_from_explicit(JSContext *ctx, JSValueConst target, lxb_dom_node_t *self, int m,
                                  JSValue set)
{
    JSValue out;
    uint32_t n = 0, i, kept = 0;

    if (!ARIA_ELS[m].plural) {
        bool ok = aria_reachable(self, lxb_dom_interface_node(element_of_value(set)));

        if (ok) return set;
        JS_FreeValue(ctx, set);
        return JS_NULL;   /* "…is a descendant of any of element's shadow-including ancestors" — or null */
    }
    out = JS_NewArray(ctx);
    CHECK(!JS_IsException(out), "an ARIAMixin element list could not be allocated");
    /* UNKNOWN EXTERNAL INPUT CROSSES A SEQUENCE POSITION AS ITSELF (idl_args.c says so at the pass-through),
       so the recorded value can be a concolic rather than the list the IDL describes. §2.6.1 asks of each
       entry whether it is a DESCENDANT of one of this element's shadow-including ancestors, and a value that
       is no node is no descendant — so it contributes nothing, exactly as an unreachable element does. */
    DCHECK(JS_IsArray(set) || concolic_is(set),
           "a plural ARIAMixin element reflection recorded an explicit value that is neither a list nor "
           "unknown external input — the setter's declared type is what makes it one of those two");
    if (JS_IsArray(set)) {
        JSValue lv = JS_GetPropertyStr(ctx, set, "length");

        if (JS_ToUint32(ctx, &n, lv) < 0) n = 0;
        JS_FreeValue(ctx, lv);
    }
    for (i = 0; i < n; i++) {
        JSValue v = JS_GetPropertyUint32(ctx, set, i);

        if (aria_reachable(self, lxb_dom_interface_node(element_of_value(v))))
            JS_SetPropertyUint32(ctx, out, kept++, v);
        else
            JS_FreeValue(ctx, v);
    }
    JS_FreeValue(ctx, set);
    return aria_frozen(ctx, target, m, out);
}

/* Infra's ASCII whitespace, which is what §2.6.1 splits the content attribute on. */
static bool aria_ws(char c)
{
    return c == 0x09 || c == 0x0a || c == 0x0c || c == 0x0d || c == 0x20;
}

static int js_aria_element_get(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                               JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    AriaElState *s = st;
    int magic = idl_step_magic(hdr), m = ARIA_MAGIC_MEMBER(magic);
    const AriaTargetOps *ops = aria_ops(ARIA_MAGIC_KIND(magic));
    lxb_dom_node_t *n;

    (void)argc; (void)argv; (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);
    DCHECK(magic >= 0 && magic < ARIA_TARGET_KINDS * ARIA_EL_N,
           "an ARIAMixin element reflection ran with a magic the mixin does not declare");

    if (hdr->stage == ARIA_EL_START) {
        JSValue elv = ops->element(ctx, hdr->this_val);   /* §2.6.1 step 1, and §3.7.5's brand test in one */
        lxb_dom_element_t *el = element_of_value(elv);
        JSValue set;
        char *val;
        size_t len, i;

        *presult = JS_NULL;
        if (!JS_IsObject(elv)) {
            /* Web IDL §3.7.5: an accessor whose receiver does not implement the interface throws, and a page
               tells that apart from a null answer. The kind the DECLARATION named is what decides this — an
               Element reaching ElementInternals.prototype's getter fails it, which is the whole point of the
               kind riding the magic rather than being sniffed off the receiver. */
            JS_FreeValue(ctx, elv);
            JS_ThrowTypeError(ctx, "an ARIAMixin element reflection was read on the wrong kind of object");
            return JS_STEP_ABRUPT;
        }
        set = aria_explicit(ctx, hdr->this_val, m);
        if (!JS_IsUndefined(set)) {
            *presult = aria_from_explicit(ctx, hdr->this_val, lxb_dom_interface_node(el), m, set);
            JS_FreeValue(ctx, elv);
            return JS_IsException(*presult) ? JS_STEP_ABRUPT : JS_STEP_DONE;
        }
        JS_FreeValue(ctx, set);
        /* Nothing explicitly set: the CONTENT ATTRIBUTE decides, and an absent one is the IDL null for both
           member shapes — `null`, never an empty FrozenArray, which a page tells apart. */
        val = ops->attr_get(ctx, hdr->this_val, ARIA_ELS[m].attr);
        if (!val) { JS_FreeValue(ctx, elv); return JS_STEP_DONE; }
        len = strlen(val);
        s->idscap = len + 1;
        s->ids = js_malloc(ctx, s->idscap);
        CHECK(s->ids != NULL, "an ARIAMixin reflection could not copy the ids it must resolve");
        memcpy(s->ids, val, s->idscap);
        free(val);
        s->slots = JS_NewArray(ctx);
        CHECK(!JS_IsException(s->slots), "an ARIAMixin id table could not be allocated");
        if (ARIA_ELS[m].plural) {
            /* "let tokens be contentAttributeValue, split on ASCII whitespace" — split IN PLACE, so the ids
               are the same allocation the snapshot already carries. */
            bool in_tok = false;

            for (i = 0; i < len; i++) {
                if (aria_ws(s->ids[i])) { s->ids[i] = '\0'; in_tok = false; continue; }
                if (!in_tok) { in_tok = true; s->ntok++; }
            }
        } else if (len > 0) {
            /* The singular member resolves the attribute's WHOLE value as one id — it is not a token list, so
               a value with a space in it names an id with a space in it and finds nothing. */
            s->ntok = 1;
        }
        for (i = 0; i < (size_t)s->ntok; i++)
            JS_SetPropertyUint32(ctx, s->slots, (uint32_t)i, JS_NULL);
        if (s->ntok == 0) {
            /* An attribute that is present and names no id: an empty list for a plural member (the attribute
               IS there, so the answer is not null), and null for the singular one. */
            *presult = ARIA_ELS[m].plural ? aria_frozen(ctx, hdr->this_val, m, aria_collect(ctx, s)) : JS_NULL;
            JS_FreeValue(ctx, elv);
            return JS_IsException(*presult) ? JS_STEP_ABRUPT : JS_STEP_DONE;
        }
        /* "candidate's root is the same as ELEMENT's root" — the element §2.6.1 step 1 answered, which for an
           ElementInternals is its target element and not the receiver. */
        s->root = node_root(lxb_dom_interface_node(el));
        s->cursor = s->root;   /* INCLUSIVE: the root's own id is in the same tree as the element's */
        JS_FreeValue(ctx, elv);
        hdr->stage = ARIA_EL_WALK;
        return JS_STEP_YIELD;
    }

    DCHECK(hdr->stage == ARIA_EL_WALK, "an ARIAMixin element reflection resumed into a stage it does not have");
    n = s->cursor;
    if (n && n->type == LXB_DOM_NODE_TYPE_ELEMENT) {
        size_t vlen = 0;
        const lxb_char_t *v = lxb_dom_element_get_attribute(lxb_dom_interface_element(n),
                                                            (const lxb_char_t *)"id", 2, &vlen);
        /* DOM §4.9's attribute change steps UNSET an element's ID for the empty string, so an element whose
           `id` is "" has no ID at all and matches nothing — including an empty id token. */
        if (v && vlen) {
            const char *tok = s->ids;
            int i;

            for (i = 0; i < s->ntok; i++, tok += strlen(tok) + 1) {
                JSValue have;

                if (strlen(tok) != vlen || memcmp(tok, v, vlen)) continue;
                have = JS_GetPropertyUint32(ctx, s->slots, (uint32_t)i);
                if (JS_IsObject(have)) { JS_FreeValue(ctx, have); continue; }   /* the FIRST in tree order */
                JS_FreeValue(ctx, have);
                JS_SetPropertyUint32(ctx, s->slots, (uint32_t)i, node_wrap(ctx, n));
                s->nmatched++;
            }
        }
    }
    if (n) s->cursor = node_next_in(n, s->root);
    if (s->cursor && s->nmatched < s->ntok) return JS_STEP_YIELD;
    if (!ARIA_ELS[m].plural) {
        *presult = JS_GetPropertyUint32(ctx, s->slots, 0);
        return JS_STEP_DONE;
    }
    *presult = aria_frozen(ctx, hdr->this_val, m, aria_collect(ctx, s));
    return JS_IsException(*presult) ? JS_STEP_ABRUPT : JS_STEP_DONE;
}

static const IdlStepDecl ARIA_EL_STEP = {
    /* No release: the id copy and the id table are aria_el_visit's, discharged with the rest of the state. */
    js_aria_element_get, sizeof(AriaElState), aria_el_visit, NULL,
    "WAI-ARIA reflection: get the attr-associated element(s)", ARIA_EL_STEPS
};

static JSValue js_aria_element_set(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    int m = ARIA_MAGIC_MEMBER(magic);
    const AriaTargetOps *ops = aria_ops(ARIA_MAGIC_KIND(magic));
    JSValue elv;

    DCHECK(magic >= 0 && magic < ARIA_TARGET_KINDS * ARIA_EL_N,
           "an ARIAMixin element reflection ran with a magic the mixin does not declare");
    elv = ops->element(ctx, this_val);   /* §3.7.5's brand test, the same one the getter makes */
    if (!JS_IsObject(elv)) {
        JS_FreeValue(ctx, elv);
        return JS_ThrowTypeError(ctx, "an ARIAMixin element reflection was set on the wrong kind of object");
    }
    JS_FreeValue(ctx, elv);
    if (JS_IsNull(val)) {
        /* "Set this's explicitly set attr-element(s) to null. Run this's delete the content attribute." */
        aria_set_explicit(ctx, this_val, m, JS_UNDEFINED);
        ops->attr_del(ctx, this_val, ARIA_ELS[m].attr);
        return JS_UNDEFINED;
    }
    /* The SINGULAR member's position is §3.2.15's brand test, which unknown external input does not pass (it is
       not a platform object and its TypeError de-taints nothing); a SEQUENCE position passes it through as
       itself, which is the one shape other than a list that can arrive here. */
    DCHECK(ARIA_ELS[m].plural ? (JS_IsArray(val) || concolic_is(val)) : element_of_value(val) != NULL,
           "an ARIAMixin element reflection was assigned a value its declared IDL type does not admit — the "
           "brand test and the sequence conversion are what make that impossible, not this body");
    /* §2.6.1's ORDER, and it is load-bearing FOR AN ELEMENT TARGET: the content attribute is set to the EMPTY
       STRING first and the explicit value recorded after, because that write runs the attribute change steps
       below, which DROP whatever was explicitly set. Recording first would erase the assignment being made.
       An ElementInternals has no such steps (they are "for element reflected targets only"), so the order is
       merely the spec's there — one order, stated once, correct for both. */
    ops->attr_set(ctx, this_val, ARIA_ELS[m].attr, "");
    aria_set_explicit(ctx, this_val, m, JS_DupValue(ctx, val));
    return JS_UNDEFINED;
}

void aria_mixin_attribute_changed(JSContext *ctx, lxb_dom_element_t *el, const char *ns, const char *local)
{
    JSValue wrap, rec;
    int i;

    /* ELEMENT REFLECTED TARGETS ONLY — §2.6.1 states these steps under exactly that heading, and there is no
       ElementInternals arm to add: nothing but the member itself can write §4.13.7.4's map, so an internals'
       explicit value has no other spelling to be invalidated by. Symmetry here would delete the default ARIA
       semantics a custom element's constructor just set. */
    if (ns != NULL || !local) return;   /* "if localName is not attr or namespace is not null, then return" */
    for (i = 0; i < ARIA_EL_N; i++)
        if (!strcmp(local, ARIA_ELS[i].attr)) break;
    if (i == ARIA_EL_N) return;
    wrap = element_wrap(ctx, el);
    rec = aria_record(ctx, wrap, i, false);
    if (JS_IsObject(rec)) JS_SetPropertyUint32(ctx, rec, 0, JS_UNDEFINED);
    JS_FreeValue(ctx, rec);
    JS_FreeValue(ctx, wrap);
}

void aria_mixin_init(JSContext *ctx)
{
    int i, j, k;

    DCHECK(g_atom_state == JS_ATOM_NULL,
           "aria_mixin_init ran twice — the mixin is declared once per AGENT and a second declaration mints a "
           "second state key, under which every element already carrying one has no state at all");
    /* THE TWO TABLES ARE ASSERTED AGAINST EACH OTHER AND AGAINST THEMSELVES. A name in both would be installed
       twice and answer with whichever install ran last; a content attribute in both would be one attribute
       served by two mechanisms; a repeat within one table is a member declared twice under one magic. */
    for (i = 0; i < ARIA_STRING_N; i++) {
        for (j = 0; j < i; j++)
            DCHECK(strcmp(ARIA_STRINGS[i].idl, ARIA_STRINGS[j].idl) &&
                   strcmp(ARIA_STRINGS[i].attr, ARIA_STRINGS[j].attr),
                   "an ARIAMixin string reflection is declared twice");
        for (j = 0; j < ARIA_EL_N; j++)
            DCHECK(strcmp(ARIA_STRINGS[i].idl, ARIA_ELS[j].idl) &&
                   strcmp(ARIA_STRINGS[i].attr, ARIA_ELS[j].attr),
                   "an ARIAMixin member is declared as both a string reflection and an element reflection — "
                   "they are two processing models and one member has exactly one");
    }
    for (i = 0; i < ARIA_EL_N; i++)
        for (j = 0; j < i; j++)
            DCHECK(strcmp(ARIA_ELS[i].idl, ARIA_ELS[j].idl) && strcmp(ARIA_ELS[i].attr, ARIA_ELS[j].attr),
                   "an ARIAMixin element reflection is declared twice");

    g_state_key = JS_NewSymbol(ctx, "ariaExplicitlySetElements", false);
    CHECK(!JS_IsException(g_state_key), "the ARIAMixin state key allocation failed");
    g_atom_state = JS_ValueToAtom(ctx, g_state_key);
    CHECK(g_atom_state != JS_ATOM_NULL, "the ARIAMixin state key could not be interned");

    aria_mixin_declare_target(ARIA_TARGET_ELEMENT, &ARIA_ELEMENT_OPS);
    g_refl_base = element_declare_reflections(ctx, "Element", ARIA_STRINGS, ARIA_STRING_N);
    /* ONE DECLARATION PER (target kind, member). Both kinds are declared HERE, in the agent's one sealed pool,
       rather than each interface declaring its own: the members are one algorithm, and an interface that
       declared its own copy is how the two would come to differ. What an interface still states for itself is
       its four target ALGORITHMS. */
    for (k = 0; k < ARIA_TARGET_KINDS; k++)
        for (i = 0; i < ARIA_EL_N; i++) {
            int magic = ARIA_MAGIC(k, i);

            g_id_el_get[k][i] = idl_getter_id_step(ctx, &ARIA_EL_STEP, magic);
            /* `Element?` and `FrozenArray<Element>?` — the NULLABILITY is the type's, so
               `el.ariaOwnsElements = 3` is a TypeError from the declaration and `= null` reaches the body as
               the IDL null. A FrozenArray is §3.2.27's frozen `sequence<T>`, so the value the setter takes is
               converted as one. */
            g_id_el_set[k][i] = idl_setter_id(ctx, ARIA_ELS[i].plural ? IDL_SEQUENCE_INTERFACE_NULLABLE
                                                                     : IDL_INTERFACE_NULLABLE,
                                              false, js_aria_element_set, magic);
            /* WHAT "Element" MEANS TO THIS ENGINE: every node wrapper is ONE class, so the class id says "a
               Node" and the narrowing is what says "an Element" — the same pair §4.13.7.3's `HTMLElement
               anchor` uses, and the reason `el.ariaOwnsElements = [document]` is a TypeError from the TYPE. */
            idl_iface_brand(node_class_id());
            idl_iface_narrow(element_is);
        }
}

void aria_mixin_install_strings(JSContext *ctx, JSValueConst proto)
{
    DCHECK(g_refl_base >= 0, "a realm installed ARIAMixin before the agent declared it");
    element_install_reflections(ctx, proto, g_refl_base, ARIA_STRING_N);
}

void aria_mixin_install_elements(JSContext *ctx, JSValueConst proto, AriaTargetKind kind)
{
    int i;

    DCHECK(kind >= 0 && kind < ARIA_TARGET_KINDS, "an ARIAMixin target kind §2.6.1 does not name was installed");
    DCHECK(g_ops[kind] != NULL,
           "a realm installed a reflected target's members before that target declared its four algorithms");
    for (i = 0; i < ARIA_EL_N; i++)
        idl_install_accessor_step(ctx, proto, ARIA_ELS[i].idl, g_id_el_get[kind][i], g_id_el_set[kind][i]);
}

void aria_mixin_free(JSRuntime *rt)
{
    if (g_atom_state != JS_ATOM_NULL) { JS_FreeAtomRT(rt, g_atom_state); g_atom_state = JS_ATOM_NULL; }
    JS_FreeValueRT(rt, g_state_key);
    g_state_key = JS_UNDEFINED;
    g_refl_base = -1;
    memset(g_ops, 0, sizeof(g_ops));   /* the declarations are the AGENT's, like every id above */
}
