/* ELEMENT INTERNALS — HTML §4.13.7.
 *
 * WHY THIS EXISTS, MEASURED. Making §4.13.5's upgrade CONSTRUCT turned a class of WPT files from "every subtest
 * fails" into "the page's own code throws", because a custom-element test's constructor calls the API the test
 * is about — and eleven files in `custom-elements/` write exactly
 *
 *     constructor() { super(); this.internals_ = this.attachInternals(); }
 *
 * so the constructor threw `TypeError: not a function` on the first line the class ran, HTML §8.1.4.6's report
 * fired an `error` event, `testharness.js` saw it and the whole file read ERROR. That is the single largest
 * named gap in the directory and it is the whole of `custom-elements/form-associated/`.
 *
 * WHAT AN ElementInternals IS. §4.13.7 gives a custom element a private handle onto the platform state a
 * BUILT-IN control has as content attributes and IDL members: its form owner and submission value, its
 * validity, its labels, its default ARIA semantics, and its custom state pseudo-classes. The handle exists so
 * that a page cannot reach that state through the element itself — `attachInternals` may be called ONCE, and
 * §4.13.4 step 14.9's `disabledFeatures` can take even that away.
 *
 * EVERY PIECE OF STATE HERE IS A JS VALUE ON A JS OBJECT, under symbols this component minted and never
 * published. That is not a shortcut around a C record: it is what the project's rules require of platform data
 * a flow can change. The submission value, the validity flags, the validation anchor, the states set and the
 * ARIA map are all things two forked arms must disagree about, and all things a parked flow has to carry to
 * the IDB cold tier and back. An own property of a wrapper is captured by the heap COW delta for free and
 * parks with the flow that wrote it; a malloc'd record would need `cow_capture_host_record` at every accessor
 * and would still be a pointer a snapshot cannot name.
 *
 * WHICH OBJECT HOLDS WHAT follows the STANDARD, not convenience. §4.13.7.1 says an ElementInternals has a
 * TARGET ELEMENT and nothing else; the submission value, the state, the validity flags, the validation
 * message, the validation anchor and the states set are all "each form-associated custom element has …" /
 * "each custom element has …", so they live on the ELEMENT's wrapper. That is what makes a second
 * `attachInternals` impossible and what would keep the state right if an element ever outlived its internals.
 *
 * WHAT IS HONESTLY ABSENT, BY NAME:
 *   - `shadowRoot`. The reason this line USED to give — "there is no Shadow DOM in this engine at all" — is no
 *     longer true and is deleted: §17 built `attachShadow`, ShadowRoot and the slot algorithms, so steps 2-3's
 *     "is a shadow host" and "target's shadow root" both have real answers now. What is still missing is steps
 *     4-5's: the root's AVAILABLE TO ELEMENT INTERNALS field, which §4.8's record carries as a WRITE
 *     (shadow_root_mark_declarative) with no reader. A getter that skipped that check would hand a page the
 *     one shadow root §4.13.7.2 hides from it, which is a WRONG answer rather than a partial one, so the
 *     member stays ABSENT until the field is readable. §4.13.4's `disable shadow` boolean is collected anyway,
 *     because it comes off the same sequence `disable internals` does.
 *   - ARIAMixin's EIGHT element-reflecting members (`ariaActiveDescendantElement` and the seven
 *     `FrozenArray<Element>?` ones). They are not content-attribute reflections at all — they are the
 *     "explicitly set attr-element" machinery, which is its own mechanism with its own lifetime rules. The 46
 *     `DOMString?` members are real reflections into §4.13.7.4's internal content attribute map and are here.
 *   - Resetting the form owner when some OTHER element's `id` changes, or when an element with an ID enters or
 *     leaves the document. Those triggers need a document-level id index, which this engine does not have
 *     (`getElementById` walks), and a tree walk per `id` write is not that index.
 *
 * THE SUBMISSION SIDE OF `setFormValue` USED TO BE ON THAT LIST AND IS BUILT. §4.13.7.3's ENTRY CONSTRUCTION
 * algorithm is a STEP of HTML §4.10.22.4, which now exists as form_entry_list.c; its step 5.3 performs the
 * construction and element_internals_submission_value below is what it reads. That is what carries a
 * form-associated custom element's value into an @H record. */
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "core/idl_iter.h"
#include "core/dom/attr_list.h"
#include "core/dom/node.h"
#include "core/dom/shadow_root.h"
#include "core/events/event.h"
#include "core/events/event_target.h"
#include "core/html/custom_elements.h"
#include "core/html/element_internals.h"
#include "core/html/constraint_validation.h"
#include "core/html/html_element.h"
#include "core/html/form_data.h"
#include "core/html/html_form.h"

static JSClassID g_internals_class, g_states_class, g_validity_class;
static int g_ready;

/* THE SLOT KEYS. Each is a Symbol this component minted and never published, so none of this state is a string
   property of the engine's invention sitting where `Object.keys` reports it. */
static JSValue g_target_key = JS_UNDEFINED;      /* ElementInternals -> its target element */
static JSAtom  g_atom_target = JS_ATOM_NULL;
static JSValue g_attached_key = JS_UNDEFINED;    /* element -> its attached internals (§4.13.2 step 5) */
static JSAtom  g_atom_attached = JS_ATOM_NULL;
static JSValue g_face_key = JS_UNDEFINED;        /* element -> its form-associated record */
static JSAtom  g_atom_face = JS_ATOM_NULL;
static JSValue g_states_key = JS_UNDEFINED;      /* element -> its states set */
static JSAtom  g_atom_states = JS_ATOM_NULL;
static JSValue g_aria_key = JS_UNDEFINED;        /* element -> §4.13.7.4's internal content attribute map */
static JSAtom  g_atom_aria = JS_ATOM_NULL;
static JSValue g_setitems_key = JS_UNDEFINED;    /* CustomStateSet -> its Array of values */
static JSAtom  g_atom_setitems = JS_ATOM_NULL;
static JSValue g_vtarget_key = JS_UNDEFINED;     /* ValidityState -> the element it reports on */
static JSAtom  g_atom_vtarget = JS_ATOM_NULL;
/* element -> its ValidityState. A SECOND key and not the one above, although one key would have worked for
   both directions: `ValidityState.prototype`'s getter brand-checks by asking for the target slot, and with one
   key an ELEMENT answers that check with its cached ValidityState — so
   `descriptor.get.call(someElement)` would report `valid: true` instead of throwing the TypeError Web IDL
   §3.7.5 requires. A brand test is only a brand test if nothing else can pass it. */
static JSValue g_validity_key = JS_UNDEFINED;
static JSAtom  g_atom_validity = JS_ATOM_NULL;

/* The FACE record's own field names — an engine-built null-prototype object, so these are ordinary atoms. */
static JSAtom g_atom_value = JS_ATOM_NULL;    /* §4.13.7.3's submission value */
static JSAtom g_atom_state = JS_ATOM_NULL;    /* §4.13.7.3's state */
static JSAtom g_atom_flags = JS_ATOM_NULL;    /* the ten validity flags, as a bitmask */
static JSAtom g_atom_message = JS_ATOM_NULL;  /* §4.13.7.3's validation message */
static JSAtom g_atom_anchor = JS_ATOM_NULL;   /* §4.13.7.3's validation anchor */
static JSAtom g_atom_custom = JS_ATOM_NULL;   /* §4.10.21.1's custom validity error message */

#define EI_SLOT_FLAGS (JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE)

/* ---- §4.10.21.1's VALIDITY STATES ----------------------------------------------------------------------------
 * The bit each flag occupies, the `ValidityStateFlags` dictionary member `setValidity` reads it from, and the
 * `ValidityState` attribute that reports it are ONE list: hand-written lists is how `tooShort` ends up set by a
 * dictionary member named `tooLong`. The DICTIONARY's own order is not this one — Web IDL §3.2.18 reads a
 * dictionary's members lexicographically and a page's getter sees it — so that list is stated separately below
 * and is checked against this one.
 * THE LIST ITSELF IS constraint_validation.h's, and this file's copy of it is deleted. §4.10.21.1 defines the
 * ten states once; a form-associated custom element's flags are WRITTEN here by setValidity and READ there by
 * "statically validate the constraints", so two spellings of the same ten bits would let one component set the
 * bit another reads as a different state — the exact failure the one-list comment above describes, one file
 * further out. */
static const char *const VALIDITY_FLAG_NAMES[CV_STATE_COUNT] = {
    CONSTRAINT_VALIDATION_STATES(CV_STATE_NAME)
};

/* ---- the records -------------------------------------------------------------------------------------------
 * Every one of these reads or creates an own slot on a wrapper. Nothing here runs the page's code: the objects
 * are engine-built and null-prototyped, so a property get on one reaches no accessor and no Proxy. */

/* An own slot's value, or JS_UNDEFINED. OWNED. */
static JSValue ei_slot(JSContext *ctx, JSValueConst obj, JSAtom key)
{
    JSValue v;

    if (!JS_IsObject(obj)) return JS_UNDEFINED;
    if (JS_GetOwnSlot(ctx, &v, obj, key) <= 0) return JS_UNDEFINED;
    return v;
}

/* The element's form-associated record, created on first use. OWNED. §4.13.7.3's initial values are what an
   absent field means: a null submission value, a null state, no validity flags, an empty validation message
   and a null validation anchor — so a record is allocated only where a member actually writes one. */
static JSValue ei_face_record(JSContext *ctx, JSValueConst el, bool create)
{
    JSValue rec = ei_slot(ctx, el, g_atom_face);

    if (JS_IsObject(rec)) return rec;
    JS_FreeValue(ctx, rec);
    if (!create) return JS_UNDEFINED;
    rec = JS_NewObjectProto(ctx, JS_NULL);
    CHECK(!JS_IsException(rec), "the form-associated custom element record could not be allocated");
    JS_SetProperty(ctx, rec, g_atom_value, JS_NULL);
    JS_SetProperty(ctx, rec, g_atom_state, JS_NULL);
    JS_SetProperty(ctx, rec, g_atom_flags, JS_NewInt32(ctx, 0));
    JS_SetProperty(ctx, rec, g_atom_message, JS_NewStringLen(ctx, "", 0));
    JS_SetProperty(ctx, rec, g_atom_anchor, JS_NULL);
    JS_SetProperty(ctx, rec, g_atom_custom, JS_NewStringLen(ctx, "", 0));
    JS_DefinePropertyValue(ctx, (JSValue)el, g_atom_face, JS_DupValue(ctx, rec), EI_SLOT_FLAGS);
    return rec;
}

/* The element's validity flags — the ten bits, zero when it has no record yet. */
static uint32_t ei_flags_of(JSContext *ctx, JSValueConst el)
{
    JSValue rec = ei_face_record(ctx, el, false), v;
    int32_t bits = 0;

    if (!JS_IsObject(rec)) { JS_FreeValue(ctx, rec); return 0; }
    v = JS_GetProperty(ctx, rec, g_atom_flags);
    JS_ToInt32(ctx, &bits, v);
    JS_FreeValue(ctx, v);
    JS_FreeValue(ctx, rec);
    return (uint32_t)bits;
}

/* §4.10.21.2's READER for a form-associated custom element's validity — the flags §4.13.7.3's `setValidity`
 * wrote, in the SAME CV_* bits constraint_validation.h declares, so "statically validate the constraints" can
 * ask a FACE the question it asks every other control.
 *
 * ONE FUNCTION, NOT TWO. The caller asked for a second — "does it have a custom error" — and that answer is
 * already the CV_CUSTOM_ERROR bit of this one; what is NOT derivable from the bits is §4.10.21.1's validation
 * MESSAGE, which is the string setValidity was given. Nothing reads that message yet (`validationMessage` is
 * absent for built-in controls too), so the reader for it is not written here: an exported accessor with no
 * caller is a shape nothing exercises, and it would arrive with a guess about what its answer should be for an
 * element setValidity never touched.
 * An element with no record answers the way the algorithm does: no record is no setValidity call, which is no
 * flags. `el` is the ELEMENT, not its ElementInternals — the flags are the element's state, which is why
 * setValidity writes them through its target rather than onto itself. */
uint32_t element_internals_validity_flags(JSContext *ctx, JSValueConst el)
{
    return ei_flags_of(ctx, el);
}

/* The element THIS ElementInternals is for — §4.13.7.1's target element, and the brand test in one. An object
   with no target slot is not an ElementInternals, and the slot's key is a symbol nothing outside can name, so
   presence IS the brand rather than standing in for one. OWNED; JS_UNDEFINED when the receiver is not one. */
static JSValue ei_target(JSContext *ctx, JSValueConst this_val)
{
    return ei_slot(ctx, this_val, g_atom_target);
}

/* THE RECEIVER CHECK EVERY MEMBER MAKES FIRST — Web IDL §3.7.5's brand check, whose failure is a TypeError
   thrown before the member's own step 1. Returns the target element (OWNED) or JS_UNDEFINED with a throw. */
static JSValue ei_target_or_throw(JSContext *ctx, JSValueConst this_val)
{
    JSValue el = ei_target(ctx, this_val);

    if (JS_IsObject(el)) return el;
    JS_FreeValue(ctx, el);
    JS_ThrowTypeError(ctx, "not an ElementInternals");
    return JS_UNDEFINED;
}

/* The target element of a member that requires a FORM-ASSOCIATED one: every §4.13.7.3 member's step 2 is "if
   element is not a form-associated custom element, then throw a NotSupportedError". OWNED, or JS_UNDEFINED
   with the throw live. */
static JSValue ei_face_target_or_throw(JSContext *ctx, JSValueConst this_val)
{
    JSValue el = ei_target_or_throw(ctx, this_val);

    if (!JS_IsObject(el)) return el;
    if (custom_elements_is_form_associated(ctx, el)) return el;
    JS_FreeValue(ctx, el);
    JS_ThrowDOMException(ctx, "NotSupportedError", "the target element is not a form-associated custom element");
    return JS_UNDEFINED;
}

/* ---- §4.13.7.5 the CustomStateSet ---------------------------------------------------------------------------
 * `setlike<DOMString>`, whose contents are a JS Array on the set's own slot: a flow that adds a state must not
 * add it for its sibling, and the Array's mutations are property writes the COW delta already captures. */

/* The set's backing Array. OWNED. */
static JSValue ei_set_items(JSContext *ctx, JSValueConst set)
{
    return ei_slot(ctx, set, g_atom_setitems);
}

static uint32_t ei_array_len(JSContext *ctx, JSValueConst arr)
{
    JSValue lv = JS_GetPropertyStr(ctx, arr, "length");
    uint32_t n = 0;

    JS_ToUint32(ctx, &n, lv);
    JS_FreeValue(ctx, lv);
    return n;
}

/* The index of `want` in the set, or -1. String comparison, because a `setlike<DOMString>`'s membership is
   Infra's string equality and not SameValueZero over whatever the page passed — the declaration has already
   converted the argument to a DOMString by the time this runs. */
static int ei_set_index(JSContext *ctx, JSValueConst items, const char *want, size_t wlen)
{
    uint32_t n = ei_array_len(ctx, items), i;

    for (i = 0; i < n; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, items, i);
        size_t elen = 0;
        const char *s = JS_ToCStringLen(ctx, &elen, e);
        bool hit = s != NULL && elen == wlen && memcmp(s, want, wlen) == 0;

        if (s) JS_FreeCString(ctx, s);
        JS_FreeValue(ctx, e);
        if (hit) return (int)i;
    }
    return -1;
}

static int ei_set_count(JSContext *ctx, JSValueConst target)
{
    JSValue items = ei_set_items(ctx, target);
    int n;

    if (!JS_IsObject(items)) { JS_FreeValue(ctx, items); return -1; }   /* the receiver is not a CustomStateSet */
    n = (int)ei_array_len(ctx, items);
    JS_FreeValue(ctx, items);
    return n;
}

/* §3.7.10's pair for a SETLIKE: the value is the key, which is what makes `entries()` yield « v, v ». */
static void ei_set_pair(JSContext *ctx, JSValueConst target, int i, JSValue *key, JSValue *value)
{
    JSValue items = ei_set_items(ctx, target);

    DCHECK(JS_IsObject(items), "a CustomStateSet iterator outlived the set it holds a reference to");
    *key = JS_GetPropertyUint32(ctx, items, (uint32_t)i);
    *value = JS_DupValue(ctx, *key);
    JS_FreeValue(ctx, items);
}

static const IdlPairIterOps EI_SET_PAIR_OPS = { ei_set_count, ei_set_pair, "CustomStateSet", true };
static int g_set_pair_handle = -1;

enum { SET_HAS = 0, SET_ADD, SET_DELETE, SET_CLEAR };

static JSValue js_states_member(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValue items = ei_set_items(ctx, this_val);
    const char *s = NULL;
    size_t slen = 0;
    int at;
    JSValue r = JS_UNDEFINED;

    if (!JS_IsObject(items)) {
        JS_FreeValue(ctx, items);
        return JS_ThrowTypeError(ctx, "not a CustomStateSet");
    }
    if (magic == SET_CLEAR) {
        JS_SetPropertyStr(ctx, items, "length", JS_NewUint32(ctx, 0));
        JS_FreeValue(ctx, items);
        return JS_UNDEFINED;
    }
    DCHECK(argc >= 1, "a CustomStateSet member reached its algorithm with no argument — Web IDL's own count "
                      "check throws before this and it did not run");
    s = JS_ToCStringLen(ctx, &slen, argv[0]);   /* a real string by now: the declaration converted it */
    if (!s) { JS_FreeValue(ctx, items); return JS_EXCEPTION; }
    at = ei_set_index(ctx, items, s, slen);
    switch (magic) {
    case SET_HAS:
        r = JS_NewBool(ctx, at >= 0);
        break;
    case SET_ADD:
        /* A set: a value already in it is not appended again, and the ORDER of the ones already there is the
           insertion order §3.7.10 iterates in. */
        if (at < 0) JS_SetPropertyUint32(ctx, items, ei_array_len(ctx, items), JS_DupValue(ctx, argv[0]));
        r = JS_DupValue(ctx, this_val);   /* `add` returns the set itself */
        break;
    default: {
        uint32_t n = ei_array_len(ctx, items), i;

        DCHECK(magic == SET_DELETE, "a CustomStateSet member ran with a magic setlike<> does not declare");
        if (at >= 0) {
            for (i = (uint32_t)at; i + 1 < n; i++)
                JS_SetPropertyUint32(ctx, items, i, JS_GetPropertyUint32(ctx, items, i + 1));
            JS_SetPropertyStr(ctx, items, "length", JS_NewUint32(ctx, n - 1));
        }
        r = JS_NewBool(ctx, at >= 0);
        break;
    }
    }
    JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, items);
    return r;
}

static JSValue js_states_size(JSContext *ctx, JSValueConst this_val, int magic)
{
    int n = ei_set_count(ctx, this_val);

    (void)magic;
    if (n < 0) return JS_ThrowTypeError(ctx, "not a CustomStateSet");
    return JS_NewUint32(ctx, (uint32_t)n);
}

/* The element's states set, created on first use — §4.13.7.5's "each custom element has a states set, which is
   a CustomStateSet, initially empty". On the ELEMENT, which is where the standard puts it, and cached there so
   `[SameObject]` holds. OWNED. */
static JSValue ei_states_set(JSContext *ctx, JSValueConst el)
{
    JSValue set = ei_slot(ctx, el, g_atom_states), proto, items;

    if (JS_IsObject(set)) return set;
    JS_FreeValue(ctx, set);
    proto = JS_GetClassProto(ctx, g_states_class);
    DCHECK(!JS_IsNull(proto), "a CustomStateSet was minted in a realm that never ran its prototype install");
    set = JS_NewObjectProtoClass(ctx, proto, g_states_class);
    JS_FreeValue(ctx, proto);
    CHECK(!JS_IsException(set), "a CustomStateSet could not be allocated");
    items = JS_NewArray(ctx);
    CHECK(!JS_IsException(items), "a CustomStateSet's value list could not be allocated");
    JS_DefinePropertyValue(ctx, set, g_atom_setitems, items, EI_SLOT_FLAGS);
    JS_DefinePropertyValue(ctx, (JSValue)el, g_atom_states, JS_DupValue(ctx, set), EI_SLOT_FLAGS);
    return set;
}

/* ---- §4.10.21.1's ValidityState ------------------------------------------------------------------------------
 * LIVE over the element, which the standard says in as many words: the object reports the flags the element
 * has NOW, so it holds the element rather than a copy of the bits. */
static JSValue js_validity_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSValue el = ei_slot(ctx, this_val, g_atom_vtarget);
    uint32_t bits;

    if (!JS_IsObject(el)) {
        JS_FreeValue(ctx, el);
        return JS_ThrowTypeError(ctx, "not a ValidityState");
    }
    bits = ei_flags_of(ctx, el);
    JS_FreeValue(ctx, el);
    /* magic CV_STATE_COUNT is `valid`: "none of the other conditions are true", which is the whole of §4.10.21.1's
       "an element satisfies its constraints if it is not suffering from any of the above validity states". */
    if (magic == CV_STATE_COUNT) return JS_NewBool(ctx, bits == 0);
    DCHECK(magic >= 0 && magic < CV_STATE_COUNT, "a ValidityState attribute ran with a magic §4.10.21.1 does not name");
    return JS_NewBool(ctx, (bits & (1u << magic)) != 0);
}

/* ---- §4.10.21.1's candidacy ---------------------------------------------------------------------------------- */

/* "A submittable element is a candidate for constraint validation except when a condition has barred it." For
   a form-associated custom element the conditions are: it is DISABLED, its `readonly` content attribute is
   specified (§4.13's own sentence), or it has a `datalist` ancestor. */
static bool ei_is_candidate(JSContext *ctx, JSValueConst el)
{
    lxb_dom_node_t *n = node_of(el), *a;

    if (!n || n->type != LXB_DOM_NODE_TYPE_ELEMENT) return false;
    if (html_form_control_is_disabled(ctx, el)) return false;
    /* PRESENCE, asked of §4.9's attribute LIST: a parsed `<x readonly>` has no value buffer at all, so a test
       on the VALUE pointer answers "absent" for exactly the markup that writes it. */
    if (dom_attr_get_ns(lxb_dom_interface_element(n), NULL, "readonly")) return false;
    for (a = n->parent; a; a = a->parent) {
        size_t len = 0;
        const lxb_char_t *t;

        if (a->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        t = lxb_dom_element_local_name(lxb_dom_interface_element(a), &len);
        if (t && len == 8 && memcmp(t, "datalist", 8) == 0) return false;
    }
    return true;
}

/* ---- §4.13.7.3 the form-associated members ------------------------------------------------------------------ */

enum { EI_FORM = 0, EI_WILL_VALIDATE, EI_VALIDITY, EI_VALIDATION_MESSAGE, EI_LABELS, EI_STATES };

static JSValue js_internals_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSValue el, r;

    /* `states` is the one getter that is NOT form-associated: §4.13.7.5's states set belongs to every custom
       element, so it takes the plain receiver check and never the NotSupportedError. */
    el = (magic == EI_STATES) ? ei_target_or_throw(ctx, this_val) : ei_face_target_or_throw(ctx, this_val);
    if (!JS_IsObject(el)) return JS_EXCEPTION;
    switch (magic) {
    case EI_FORM:
        r = html_form_owner_of(ctx, el);
        break;
    case EI_WILL_VALIDATE:
        r = JS_NewBool(ctx, ei_is_candidate(ctx, el));
        break;
    case EI_VALIDITY: {
        /* The object is cached on the ELEMENT so a page that re-reads `internals.validity` gets the same live
           object, which is what "this object is live" is for. */
        JSValue v = ei_slot(ctx, el, g_atom_validity);

        if (JS_IsObject(v)) { r = v; break; }
        JS_FreeValue(ctx, v);
        {
            JSValue proto = JS_GetClassProto(ctx, g_validity_class);
            DCHECK(!JS_IsNull(proto), "a ValidityState was minted in a realm that never ran its install");
            r = JS_NewObjectProtoClass(ctx, proto, g_validity_class);
            JS_FreeValue(ctx, proto);
            CHECK(!JS_IsException(r), "a ValidityState could not be allocated");
            JS_DefinePropertyValue(ctx, r, g_atom_vtarget, JS_DupValue(ctx, el), EI_SLOT_FLAGS);
            JS_DefinePropertyValue(ctx, (JSValue)el, g_atom_validity, JS_DupValue(ctx, r), EI_SLOT_FLAGS);
        }
        break;
    }
    case EI_VALIDATION_MESSAGE: {
        JSValue rec = ei_face_record(ctx, el, false);

        r = JS_IsObject(rec) ? JS_GetProperty(ctx, rec, g_atom_message) : JS_NewStringLen(ctx, "", 0);
        JS_FreeValue(ctx, rec);
        break;
    }
    case EI_LABELS:
        /* §4.10.19 says "it must return that NodeList object, and that same value must always be returned" —
           a LIVE collection, cached. This answers with a fresh STATIC one, which is content-correct and
           identity-wrong, and caching the static one would make it identity-right and CONTENT-wrong the first
           time a label is added. The right answer is a live collection over the label-association predicate,
           which is collections.c's to grow beside its other five; the same named gap `querySelectorAll` and
           `form.elements` carry today. */
        r = html_form_labels_of(ctx, el);
        break;
    default:
        DCHECK(magic == EI_STATES, "an ElementInternals attribute ran with a magic §4.13.7 does not declare");
        r = ei_states_set(ctx, el);
        break;
    }
    JS_FreeValue(ctx, el);
    return r;
}

/* §4.13.7.3 `setFormValue(value, state)`. The two arguments arrived through the declared
   `(File or USVString or FormData)?` union, so each is already the IDL null, a File, a FormData or a real
   string — the brand test is the union's and no `if` here repeats it. */
static JSValue js_internals_set_form_value(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                           int magic)
{
    JSValue el = ei_face_target_or_throw(ctx, this_val), rec;   /* steps 1-2 */

    (void)magic;
    if (!JS_IsObject(el)) return JS_EXCEPTION;
    rec = ei_face_record(ctx, el, true);
    /* Step 3: the submission value is `value`, or a CLONE of a FormData's entry list — never the FormData
       itself, so a page that appends to it afterwards has not changed what its element submits. */
    JS_SetProperty(ctx, rec, g_atom_value,
                   form_data_is(argv[0]) ? form_data_clone(ctx, argv[0]) : JS_DupValue(ctx, argv[0]));
    /* Steps 4-6. An OMITTED `state` sets the state to the submission value; a given one is stored (cloned when
       it is a FormData). Omitted is not the same as `null` — `setFormValue(x, null)` stores a null state — and
       an explicit `undefined` is OMITTED, because Web IDL §3.6.2 treats one passed for an optional argument
       with no default as not present. The declaration leaves such a position unconverted, so JS_IsUndefined is
       what "not present" looks like here; argc alone would call `setFormValue(x, undefined)` a two-argument
       call and store undefined as the state. */
    if (argc < 2 || JS_IsUndefined(argv[1])) {
        JSValue v = JS_GetProperty(ctx, rec, g_atom_value);
        JS_SetProperty(ctx, rec, g_atom_state, v);
    } else {
        JS_SetProperty(ctx, rec, g_atom_state,
                       form_data_is(argv[1]) ? form_data_clone(ctx, argv[1]) : JS_DupValue(ctx, argv[1]));
    }
    JS_FreeValue(ctx, rec);
    JS_FreeValue(ctx, el);
    return JS_UNDEFINED;
}

/* §4.13.7.3's `ValidityStateFlags` — in LEXICOGRAPHIC order, because Web IDL §3.2.18 reads a dictionary's own
   members that way and a page with a getter on the object observes the sequence. */
static const IdlDictMember EI_VALIDITY_FLAGS[] = {
    { "badInput",         IDL_BOOLEAN },
    { "customError",      IDL_BOOLEAN },
    { "patternMismatch",  IDL_BOOLEAN },
    { "rangeOverflow",    IDL_BOOLEAN },
    { "rangeUnderflow",   IDL_BOOLEAN },
    { "stepMismatch",     IDL_BOOLEAN },
    { "tooLong",          IDL_BOOLEAN },
    { "tooShort",         IDL_BOOLEAN },
    { "typeMismatch",     IDL_BOOLEAN },
    { "valueMissing",     IDL_BOOLEAN },
};

/* §4.13.7.3 `setValidity(flags, message, anchor)`. */
static JSValue js_internals_set_validity(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                         int magic)
{
    JSValue el = ei_face_target_or_throw(ctx, this_val), rec;   /* steps 1-2 */
    JSValueConst flags = argc > 0 ? argv[0] : JS_UNDEFINED;
    uint32_t bits = 0;
    bool have_message;
    int i;

    (void)magic;
    if (!JS_IsObject(el)) return JS_EXCEPTION;
    /* Step 4, read before step 3 needs it: "for each entry flag -> value of flags, set element's validity flag
       with the name flag to value". The dictionary is engine-built by now, so this reads no page code. */
    for (i = 0; i < CV_STATE_COUNT; i++)
        if (idl_dict_bool(ctx, flags, VALIDITY_FLAG_NAMES[i])) bits |= 1u << i;
    /* Step 3: one or more true flags with no message — or the empty string — is a TypeError, and it is thrown
       BEFORE any flag is written. */
    have_message = argc > 1 && !JS_IsUndefined(argv[1]);   /* §3.6.2: an explicit undefined is NOT GIVEN */
    if (have_message) {
        size_t mlen = 0;
        const char *m = JS_ToCStringLen(ctx, &mlen, argv[1]);   /* already a DOMString: the declaration converted it */

        if (!m) { JS_FreeValue(ctx, el); return JS_EXCEPTION; }
        if (!mlen) have_message = false;
        JS_FreeCString(ctx, m);
    }
    if (bits != 0 && !have_message) {
        JS_FreeValue(ctx, el);
        return JS_ThrowTypeError(ctx, "setValidity with a true flag requires a non-empty message");
    }
    rec = ei_face_record(ctx, el, true);
    JS_SetProperty(ctx, rec, g_atom_flags, JS_NewUint32(ctx, bits));
    /* Step 5: the empty string when no message was given or every flag is false, and the message otherwise. */
    JS_SetProperty(ctx, rec, g_atom_message,
                   (!have_message || bits == 0) ? JS_NewStringLen(ctx, "", 0) : JS_DupValue(ctx, argv[1]));
    /* Step 6: the CUSTOM validity error message is the validation message when customError is set, and empty
       otherwise. Two fields and not one, because §4.10.21.1's `setCustomValidity` writes only the second and
       every other flag leaves it alone. */
    {
        JSValue msg = JS_GetProperty(ctx, rec, g_atom_message);

        JS_SetProperty(ctx, rec, g_atom_custom,
                       (bits & (1u << CV_CUSTOM_ERROR)) ? msg : JS_NewStringLen(ctx, "", 0));
        if (!(bits & (1u << CV_CUSTOM_ERROR))) JS_FreeValue(ctx, msg);
    }
    /* Steps 7-9: an omitted anchor is the element itself; a given one must be a SHADOW-INCLUDING inclusive
       descendant of it — which is the whole point of the member, because a form-associated custom element's
       anchor is normally the `<input>` inside its own shadow tree, and a plain ancestor walk rejects exactly
       that. The relation lives in shadow_root.c; this reads it. */
    if (argc < 3 || JS_IsUndefined(argv[2])) {
        JS_SetProperty(ctx, rec, g_atom_anchor, JS_DupValue(ctx, el));
    } else {
        lxb_dom_node_t *anchor = node_of(argv[2]), *host = node_of(el);
        bool inside = shadow_root_is_shadow_including_inclusive_ancestor(host, anchor);

        if (!inside) {
            JS_FreeValue(ctx, rec);
            JS_FreeValue(ctx, el);
            return JS_ThrowDOMException(ctx, "NotFoundError",
                                        "the validation anchor is not a descendant of the target element");
        }
        JS_SetProperty(ctx, rec, g_atom_anchor, JS_DupValue(ctx, argv[2]));
    }
    JS_FreeValue(ctx, rec);
    JS_FreeValue(ctx, el);
    return JS_UNDEFINED;
}

/* ---- §4.10.21.1's check/report validity steps ---------------------------------------------------------------
 *
 * BOTH ARE MACHINES, and for the one reason anything here is: step 1.1 FIRES an `invalid` event at the
 * element, which is the page's own handlers — a loop, an `await`, a DOM mutation — and a C activation hosting
 * those is the drive-to-completion this engine aborts on. The two differ only in what the event's return value
 * is for, so they are ONE machine with a magic. */
#define EI_VALIDITY_STAGES(X) \
    X(EIV_DECIDE, "HTML §4.10.21.1 check/report validity steps step 1 (is element a candidate for constraint " \
                  "validation, and does it fail to satisfy its constraints)") \
    X(EIV_FIRE,   "HTML §4.10.21.1 check/report validity steps step 1.1 (fire an event named invalid at " \
                  "element, cancelable), and the report/return the verdict decides")
enum { EI_VALIDITY_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const EI_VALIDITY_STEPS[] = { EI_VALIDITY_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct JSEiValidityState {
    JSStepHdr hdr;      /* FIRST — the driver writes the def and the operand bounds through it */
    uint8_t   fphase;   /* the fire request's own phase */
    uint8_t   answer;   /* the boolean the steps return, yielded by fini — a machine's result IS its fini's */
    JSValue   el;       /* the target element, resolved at step 1 and held across the suspension (owned) */
    JSValue   ev;       /* the `invalid` event, minted once (owned) */
    JSValue   cb[4];    /* the fire request buffer: [this, dispatch, target, event] */
} JSEiValidityState;

static void js_ei_validity_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSEiValidityState *s = st;
    int k;

    v->val(ctx, &s->el);
    v->val(ctx, &s->ev);
    for (k = 0; k < 4; k++) v->val(ctx, &s->cb[k]);
}

static JSValue js_ei_validity_fini(JSContext *ctx, void *st, bool take_result)
{
    JSEiValidityState *s = st;
    int k;

    JS_FreeValue(ctx, s->el);
    JS_FreeValue(ctx, s->ev);
    s->el = s->ev = JS_UNDEFINED;
    for (k = 0; k < 4; k++) {
        JS_FreeValue(ctx, s->cb[k]);
        s->cb[k] = JS_UNDEFINED;
    }
    /* §4.10.21.1 returns a boolean, and a machine's RESULT is what its fini yields. A teardown that is not
       taking the result (the throw path) must yield nothing, which is what `take_result` distinguishes. */
    return take_result ? JS_NewBool(ctx, s->answer) : JS_UNDEFINED;
}

static int js_ei_validity_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSEiValidityState *s = st;
    bool not_canceled = true;
    int r;

    if (s->hdr.stage == EIV_DECIDE) {
        int k;

        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        /* EVERY owned field before the first thing that can throw: the failure path tears this state down
           through js_ei_validity_fini, which frees exactly what the state holds and nothing else. */
        s->el = s->ev = JS_UNDEFINED;
        s->answer = 0;
        for (k = 0; k < 4; k++) s->cb[k] = JS_UNDEFINED;
        s->el = ei_face_target_or_throw(ctx, s->hdr.this_val);
        if (!JS_IsObject(s->el)) return JS_STEP_ABRUPT;
        /* Step 1's condition. A candidate that satisfies its constraints fires nothing and answers true —
           and the stage is advanced only on the arm that reaches step 1.1, so a machine that never fires
           cannot report itself parked at an event it did not dispatch. */
        if (!ei_is_candidate(ctx, s->el) || ei_flags_of(ctx, s->el) == 0) {
            s->answer = 1;
            return JS_STEP_DONE;
        }
        s->hdr.stage = EIV_FIRE;
    }
    DCHECK(s->hdr.stage == EIV_FIRE, "check/report validity resumed into a stage §4.10.21.1 does not have");
    if (JS_IsUndefined(s->ev))
        s->ev = event_new(ctx, "invalid", /*bubbles*/ false, /*cancelable*/ true);
    r = event_target_fire_run(ctx, &s->fphase, STEP_CB(s->cb), s->el, s->ev, JS_UNDEFINED, cb_result, &not_canceled,
                              out_cb, out_argc);
    if (r > 0) return r;
    if (r < 0) return JS_STEP_ABRUPT;
    /* Step 1.2 exists only in the REPORT steps and it is "report the problems with the constraints of this
       element to the USER" — a user-agent presentation with no scriptable result, gated on the event not
       having been canceled. So the verdict is computed (the dispatch has to answer it) and there is nothing
       for either algorithm to DO with it here: this engine has no user to present to, which is a missing IO
       device and not a missing behaviour. That is also the whole of why the two algorithms are one
       implementation with two `.algorithm` labels; a `focus()`/scroll model is what would make them two. */
    (void)not_canceled;
    s->answer = 0;
    return JS_STEP_DONE;
}

static const JSTrampStepDef js_ei_check_validity_def = {
    sizeof(JSEiValidityState), js_ei_validity_step, js_ei_validity_fini, 0, .visit = js_ei_validity_visit,
    .algorithm = "HTML §4.10.21.1 check validity steps (from ElementInternals.checkValidity)",
    .steps = EI_VALIDITY_STEPS
};
static const JSTrampStepDef js_ei_report_validity_def = {
    sizeof(JSEiValidityState), js_ei_validity_step, js_ei_validity_fini, 0, .visit = js_ei_validity_visit,
    .algorithm = "HTML §4.10.21.1 report validity steps (from ElementInternals.reportValidity)",
    .steps = EI_VALIDITY_STEPS
};

/* ---- §4.13.7.4 the ARIA content-attribute map ----------------------------------------------------------------
 *
 * "Each custom element has an internal content attribute map, which is a map, initially empty." A `DOMString?`
 * ARIAMixin member on an ElementInternals reads and writes exactly that map, under the CONTENT ATTRIBUTE's own
 * name — which is what makes `internals.ariaLabel` and `internals.role` the DEFAULT semantics the page author
 * can still override with the real `aria-label` and `role` attributes. */
#define ARIA_MEMBERS(X) \
    X("role",                        "role") \
    X("ariaAtomic",                  "aria-atomic") \
    X("ariaAutoComplete",            "aria-autocomplete") \
    X("ariaBrailleLabel",            "aria-braillelabel") \
    X("ariaBrailleRoleDescription",  "aria-brailleroledescription") \
    X("ariaBusy",                    "aria-busy") \
    X("ariaChecked",                 "aria-checked") \
    X("ariaColCount",                "aria-colcount") \
    X("ariaColIndex",                "aria-colindex") \
    X("ariaColIndexText",            "aria-colindextext") \
    X("ariaColSpan",                 "aria-colspan") \
    X("ariaCurrent",                 "aria-current") \
    X("ariaDescription",             "aria-description") \
    X("ariaDisabled",                "aria-disabled") \
    X("ariaExpanded",                "aria-expanded") \
    X("ariaHasPopup",                "aria-haspopup") \
    X("ariaHidden",                  "aria-hidden") \
    X("ariaInvalid",                 "aria-invalid") \
    X("ariaKeyShortcuts",            "aria-keyshortcuts") \
    X("ariaLabel",                   "aria-label") \
    X("ariaLevel",                   "aria-level") \
    X("ariaLive",                    "aria-live") \
    X("ariaModal",                   "aria-modal") \
    X("ariaMultiLine",               "aria-multiline") \
    X("ariaMultiSelectable",         "aria-multiselectable") \
    X("ariaOrientation",             "aria-orientation") \
    X("ariaPlaceholder",             "aria-placeholder") \
    X("ariaPosInSet",                "aria-posinset") \
    X("ariaPressed",                 "aria-pressed") \
    X("ariaReadOnly",                "aria-readonly") \
    X("ariaRelevant",                "aria-relevant") \
    X("ariaRequired",                "aria-required") \
    X("ariaRoleDescription",         "aria-roledescription") \
    X("ariaRowCount",                "aria-rowcount") \
    X("ariaRowIndex",                "aria-rowindex") \
    X("ariaRowIndexText",            "aria-rowindextext") \
    X("ariaRowSpan",                 "aria-rowspan") \
    X("ariaSelected",                "aria-selected") \
    X("ariaSetSize",                 "aria-setsize") \
    X("ariaSort",                    "aria-sort") \
    X("ariaValueMax",                "aria-valuemax") \
    X("ariaValueMin",                "aria-valuemin") \
    X("ariaValueNow",                "aria-valuenow") \
    X("ariaValueText",               "aria-valuetext")
#define ARIA_ROW(member, attr) { member, attr },
static const struct { const char *member, *attr; } ARIA[] = { ARIA_MEMBERS(ARIA_ROW) };
#define ARIA_N ((int)(sizeof(ARIA) / sizeof(ARIA[0])))

/* The element's internal content attribute map, created on first use. OWNED. */
static JSValue ei_aria_map(JSContext *ctx, JSValueConst el, bool create)
{
    JSValue map = ei_slot(ctx, el, g_atom_aria);

    if (JS_IsObject(map)) return map;
    JS_FreeValue(ctx, map);
    if (!create) return JS_UNDEFINED;
    map = JS_NewObjectProto(ctx, JS_NULL);
    CHECK(!JS_IsException(map), "the internal content attribute map could not be allocated");
    JS_DefinePropertyValue(ctx, (JSValue)el, g_atom_aria, JS_DupValue(ctx, map), EI_SLOT_FLAGS);
    return map;
}

static JSValue js_internals_aria_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSValue el = ei_target_or_throw(ctx, this_val), map, r;

    if (!JS_IsObject(el)) return JS_EXCEPTION;
    DCHECK(magic >= 0 && magic < ARIA_N, "an ARIAMixin attribute ran with a magic the mixin does not declare");
    map = ei_aria_map(ctx, el, false);
    /* An entry the map does not have is the IDL null, which is what an unreflected `DOMString?` reads as. */
    r = JS_IsObject(map) ? JS_GetPropertyStr(ctx, map, ARIA[magic].attr) : JS_NULL;
    if (JS_IsUndefined(r)) { JS_FreeValue(ctx, r); r = JS_NULL; }
    JS_FreeValue(ctx, map);
    JS_FreeValue(ctx, el);
    return r;
}

static JSValue js_internals_aria_set(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    JSValue el = ei_target_or_throw(ctx, this_val), map;

    if (!JS_IsObject(el)) return JS_EXCEPTION;
    DCHECK(magic >= 0 && magic < ARIA_N, "an ARIAMixin attribute ran with a magic the mixin does not declare");
    map = ei_aria_map(ctx, el, true);
    /* `[Reflect]` on a `DOMString?`: assigning null REMOVES the content attribute, which for this map is the
       entry going away rather than an entry holding null — `internals.role = null` then reading it back must
       answer null either way, but the map is what the accessibility layer reads and an entry that is there
       with a null value is a `role=""` it has to special-case. */
    if (JS_IsNull(val)) {
        JSAtom a = JS_NewAtom(ctx, ARIA[magic].attr);

        CHECK(a != JS_ATOM_NULL, "an ARIA content attribute name could not be interned");
        JS_DeleteProperty(ctx, map, a, 0);
        JS_FreeAtom(ctx, a);
    } else {
        JS_SetPropertyStr(ctx, map, ARIA[magic].attr, JS_DupValue(ctx, val));
    }
    JS_FreeValue(ctx, map);
    JS_FreeValue(ctx, el);
    return JS_UNDEFINED;
}

/* ---- §4.13.2 attachInternals() ------------------------------------------------------------------------------ */

static JSValue js_html_attach_internals(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                        int magic)
{
    lxb_dom_node_t *n = node_of(this_val);
    JSValue def, internals, proto;
    int state;

    (void)argc; (void)argv; (void)magic;
    if (!n || n->type != LXB_DOM_NODE_TYPE_ELEMENT)
        return JS_ThrowTypeError(ctx, "attachInternals called on something that is not an element");
    /* Step 1: "if this's is value is not null, then throw a NotSupportedError". An `is` value is set only by
       `createElement`'s `is` option and by the parser's `is` attribute for a CUSTOMIZED BUILT-IN, and
       §4.13.4 refuses to register one at all (ce_define_checks throws NotSupportedError for `extends`) — so no
       element in this engine can carry one, and this step has nothing to test rather than being skipped. It
       becomes a real read in the diff that makes customized built-ins registrable. */
    /* Step 2: "let definition be the result of LOOKING UP A CUSTOM ELEMENT DEFINITION given this's CUSTOM
       ELEMENT REGISTRY, this's namespace, this's local name, and null." It is the lookup and not the element's
       own definition slot, and the difference is the whole reason the registry is a node's state: an element
       whose upgrade has not started yet carries NO definition, so reading the slot answered step 3's
       NotSupportedError where the spec reaches step 4's `disable internals` and step 6's state check — the
       same DOMException name for the ordinary case and the wrong step for a class that disables internals.
       The lookup goes to THIS ELEMENT'S registry, so a component defined in a scoped registry gets its own
       definition here rather than the document's answer for the same tag name. */
    def = custom_elements_definition_lookup_for_element(ctx, this_val);
    if (!JS_IsObject(def)) {                                            /* step 3 */
        JS_FreeValue(ctx, def);
        return JS_ThrowDOMException(ctx, "NotSupportedError",
                                    "attachInternals on an element with no custom element definition");
    }
    if (custom_elements_definition_flag(ctx, def, CE_DEF_DISABLE_INTERNALS)) {   /* step 4 */
        JS_FreeValue(ctx, def);
        return JS_ThrowDOMException(ctx, "NotSupportedError",
                                    "this custom element's disabledFeatures contains \"internals\"");
    }
    JS_FreeValue(ctx, def);
    internals = ei_slot(ctx, this_val, g_atom_attached);
    if (JS_IsObject(internals)) {                                       /* step 5 */
        JS_FreeValue(ctx, internals);
        return JS_ThrowDOMException(ctx, "NotSupportedError", "attachInternals has already been called");
    }
    JS_FreeValue(ctx, internals);
    /* Step 6: the state must be "precustomized" or "custom" — which is what makes `attachInternals` legal from
       inside the constructor (§4.13.5 step 8.2 sets "precustomized" before it Constructs) and illegal on an
       element whose upgrade has not started. */
    state = custom_elements_state_of_element(ctx, this_val);
    if (state != CE_STATE_PRECUSTOMIZED && state != CE_STATE_CUSTOM)
        return JS_ThrowDOMException(ctx, "NotSupportedError",
                                    "attachInternals before the element became custom");
    /* Steps 7-8. */
    proto = JS_GetClassProto(ctx, g_internals_class);
    DCHECK(!JS_IsNull(proto), "an ElementInternals was minted in a realm that never ran its prototype install");
    internals = JS_NewObjectProtoClass(ctx, proto, g_internals_class);
    JS_FreeValue(ctx, proto);
    CHECK(!JS_IsException(internals), "an ElementInternals could not be allocated");
    JS_DefinePropertyValue(ctx, internals, g_atom_target, JS_DupValue(ctx, this_val), EI_SLOT_FLAGS);
    JS_DefinePropertyValue(ctx, (JSValue)this_val, g_atom_attached, JS_DupValue(ctx, internals), EI_SLOT_FLAGS);
    return internals;
}

JSValue element_internals_submission_value(JSContext *ctx, JSValueConst wrap)
{
    JSValue rec = ei_face_record(ctx, wrap, false), v;

    /* §4.13.7.3's initial submission value is null, and an element with no record has never had one written —
       which is what an absent record means and why nothing allocates one to answer this. */
    if (!JS_IsObject(rec)) { JS_FreeValue(ctx, rec); return JS_NULL; }
    v = JS_GetProperty(ctx, rec, g_atom_value);
    JS_FreeValue(ctx, rec);
    return v;
}

/* ---- §4.13.5 step 10 and §4.10.18.3's reset ------------------------------------------------------------------ */

void element_internals_upgrade_form_steps(JSContext *ctx, JSValueConst wrap)
{
    JSValue owner;

    DCHECK(g_ready, "§4.13.5 step 10 ran before element_internals_declare");
    if (!custom_elements_is_form_associated(ctx, wrap)) return;
    /* Step 10.1: reset the form owner, and enqueue formAssociatedCallback when the element IS associated. The
       condition is the OWNER being non-null and not whether the reset changed it, which is what the step says
       and why the change-triggered reset below is a different call. */
    owner = html_form_reset_owner(ctx, wrap, NULL);
    if (JS_IsObject(owner)) {
        JSValueConst arg = owner;
        custom_elements_enqueue_form_callback(ctx, wrap, CE_FORM_CB_ASSOCIATED, 1, &arg);
    }
    JS_FreeValue(ctx, owner);
    /* Step 10.2. */
    if (html_form_control_is_disabled(ctx, wrap)) {
        JSValueConst arg = JS_TRUE;
        custom_elements_enqueue_form_callback(ctx, wrap, CE_FORM_CB_DISABLED, 1, &arg);
    }
}

void element_internals_reset_form_owner(JSContext *ctx, JSValueConst wrap,
                                        const char *form_attr, size_t form_attr_len)
{
    JSValue owner;
    bool changed = false;

    DCHECK(g_ready, "a form-owner reset ran before element_internals_declare");
    if (!custom_elements_is_form_associated(ctx, wrap)) return;
    owner = (form_attr == ELEMENT_INTERNALS_ATTR_UNCHANGED)
          ? html_form_reset_owner(ctx, wrap, &changed)
          : html_form_reset_owner_with_attr(ctx, wrap, form_attr, form_attr_len, &changed);
    /* §4.13.3: "when the user agent resets the form owner of a form-associated custom element and doing so
       CHANGES the form owner, its formAssociatedCallback is called, given the new form owner (or null if no
       owner)". The null case is a real argument, which is why this cannot be the step-10 call above. */
    if (changed) {
        JSValueConst arg = owner;
        custom_elements_enqueue_form_callback(ctx, wrap, CE_FORM_CB_ASSOCIATED, 1, &arg);
    }
    JS_FreeValue(ctx, owner);
}

/* ---- declaration and installation ---------------------------------------------------------------------------- */

static int g_id_attach = -1, g_id_set_form_value = -1, g_id_set_validity = -1;
static int g_id_check_validity = -1, g_id_report_validity = -1;
static int g_id_set_has = -1, g_id_set_add = -1, g_id_set_delete = -1, g_id_set_clear = -1;
static int g_id_aria_set[ARIA_N];

static void element_internals_install_protos(JSContext *ctx);

void element_internals_declare(JSContext *ctx)
{
    JSClassDef id = { "ElementInternals" }, sd = { "CustomStateSet" }, vd = { "ValidityState" };
    JSRuntime *rt = JS_GetRuntime(ctx);
    static const IdlArgType FORM_VALUE[2] = { IDL_FORMVALUE_NULLABLE, IDL_FORMVALUE_NULLABLE };
    static const IdlArgType SET_VALIDITY[3] = { IDL_DICT, IDL_DOMSTRING, IDL_INTERFACE };
    static const IdlArgType ONE_STR[1] = { IDL_DOMSTRING };
    static const IdlArgType NONE[1] = { IDL_ANY };
    int i;

    DCHECK(!g_ready, "element_internals_declare ran twice — one instance is one agent");
    JS_NewClassID(rt, &g_internals_class);
    JS_NewClass(rt, g_internals_class, &id);
    JS_NewClassID(rt, &g_states_class);
    JS_NewClass(rt, g_states_class, &sd);
    JS_NewClassID(rt, &g_validity_class);
    JS_NewClass(rt, g_validity_class, &vd);

    g_target_key = JS_NewSymbol(ctx, "elementInternalsTarget", false);
    g_attached_key = JS_NewSymbol(ctx, "attachedInternals", false);
    g_face_key = JS_NewSymbol(ctx, "formAssociatedState", false);
    g_states_key = JS_NewSymbol(ctx, "customStateSet", false);
    g_aria_key = JS_NewSymbol(ctx, "internalContentAttributeMap", false);
    g_setitems_key = JS_NewSymbol(ctx, "customStateSetValues", false);
    g_vtarget_key = JS_NewSymbol(ctx, "validityStateTarget", false);
    g_validity_key = JS_NewSymbol(ctx, "validityState", false);
    CHECK(!JS_IsException(g_validity_key), "the ValidityState cache key allocation failed");
    CHECK(!JS_IsException(g_target_key) && !JS_IsException(g_attached_key) && !JS_IsException(g_face_key) &&
          !JS_IsException(g_states_key) && !JS_IsException(g_aria_key) && !JS_IsException(g_setitems_key) &&
          !JS_IsException(g_vtarget_key),
          "an §4.13.7 slot key allocation failed");
    g_atom_target = JS_ValueToAtom(ctx, g_target_key);
    g_atom_attached = JS_ValueToAtom(ctx, g_attached_key);
    g_atom_face = JS_ValueToAtom(ctx, g_face_key);
    g_atom_states = JS_ValueToAtom(ctx, g_states_key);
    g_atom_aria = JS_ValueToAtom(ctx, g_aria_key);
    g_atom_setitems = JS_ValueToAtom(ctx, g_setitems_key);
    g_atom_vtarget = JS_ValueToAtom(ctx, g_vtarget_key);
    g_atom_validity = JS_ValueToAtom(ctx, g_validity_key);
    CHECK(g_atom_validity != JS_ATOM_NULL, "the ValidityState cache key could not be interned");
    g_atom_value = JS_NewAtom(ctx, "value");
    g_atom_state = JS_NewAtom(ctx, "state");
    g_atom_flags = JS_NewAtom(ctx, "flags");
    g_atom_message = JS_NewAtom(ctx, "message");
    g_atom_anchor = JS_NewAtom(ctx, "anchor");
    g_atom_custom = JS_NewAtom(ctx, "custom");
    CHECK(g_atom_target != JS_ATOM_NULL && g_atom_attached != JS_ATOM_NULL && g_atom_face != JS_ATOM_NULL &&
          g_atom_states != JS_ATOM_NULL && g_atom_aria != JS_ATOM_NULL && g_atom_setitems != JS_ATOM_NULL &&
          g_atom_vtarget != JS_ATOM_NULL && g_atom_value != JS_ATOM_NULL && g_atom_state != JS_ATOM_NULL &&
          g_atom_flags != JS_ATOM_NULL && g_atom_message != JS_ATOM_NULL && g_atom_anchor != JS_ATOM_NULL &&
          g_atom_custom != JS_ATOM_NULL,
          "an §4.13.7 slot key or record field name could not be interned");

    g_id_attach = idl_method_id(ctx, NONE, 0, js_html_attach_internals, 0);
    g_id_set_form_value = idl_method_id(ctx, FORM_VALUE, 2, js_internals_set_form_value, 0);
    idl_optional_from(1);   /* §4.13.7.3: `setFormValue(value, optional state)` */
    /* THE TWO LISTS ARE ASSERTED AGAINST EACH OTHER, not merely said to agree. The flag bits are declared in
       §4.10.21.1's order and the dictionary in Web IDL's lexicographic one, so neither can be derived from the
       other by ordering — but they must name the SAME ten members, and a member in one and not the other is a
       flag `setValidity` can never set or a dictionary key that silently does nothing. */
    CHECK((int)(sizeof(EI_VALIDITY_FLAGS) / sizeof(EI_VALIDITY_FLAGS[0])) == CV_STATE_COUNT,
          "ValidityStateFlags declares a different number of members than §4.10.21.1 has validity flags");
    for (i = 0; i < CV_STATE_COUNT; i++) {
        int j, seen = 0;
        for (j = 0; j < CV_STATE_COUNT; j++)
            if (!strcmp(EI_VALIDITY_FLAGS[j].name, VALIDITY_FLAG_NAMES[i])) seen++;
        DCHECK(seen == 1, "a §4.10.21.1 validity flag is named by no ValidityStateFlags member, or by two — "
                          "the bit it sets and the dictionary key that sets it are one member of one type");
    }
    g_id_set_validity = idl_method_id_dict(ctx, SET_VALIDITY, 3, EI_VALIDITY_FLAGS,
                                           (int)(sizeof(EI_VALIDITY_FLAGS) / sizeof(EI_VALIDITY_FLAGS[0])),
                                           js_internals_set_validity, 0);
    idl_optional_from(0);   /* every argument of setValidity is optional */
    /* `optional HTMLElement anchor` — a NODE by class and an HTML-namespace ELEMENT by narrowing, because that
       is what the IDL says and a class id alone cannot say it: every node wrapper is one class, so without the
       narrowing `setValidity(f, m, document.createElementNS('some-ns','foo'))` crosses as an anchor. */
    idl_iface_brand(node_class_id());
    idl_iface_narrow(html_element_is);
    g_id_check_validity = JS_RegisterStepDef(rt, &js_ei_check_validity_def);
    g_id_report_validity = JS_RegisterStepDef(rt, &js_ei_report_validity_def);
    g_id_set_has = idl_method_id(ctx, ONE_STR, 1, js_states_member, SET_HAS);
    g_id_set_add = idl_method_id(ctx, ONE_STR, 1, js_states_member, SET_ADD);
    g_id_set_delete = idl_method_id(ctx, ONE_STR, 1, js_states_member, SET_DELETE);
    g_id_set_clear = idl_method_id(ctx, NONE, 0, js_states_member, SET_CLEAR);
    for (i = 0; i < ARIA_N; i++)
        g_id_aria_set[i] = idl_setter_id(ctx, IDL_DOMSTRING_NULLABLE, false, js_internals_aria_set, i);
    g_set_pair_handle = idl_pair_iter_declare(ctx, &EI_SET_PAIR_OPS);
    realm_declare_intrinsic(element_internals_install_protos);
    g_ready = 1;
}

static void element_internals_install_protos(JSContext *ctx)
{
    JSValue proto, prev;
    int i;

    DCHECK(g_ready, "a realm asked for §4.13.7's prototypes before element_internals_declare ran");
    prev = JS_GetClassProto(ctx, g_internals_class);
    DCHECK(JS_IsNull(prev), "element_internals_install_protos ran twice in one realm");
    JS_FreeValue(ctx, prev);

    proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(proto), "ElementInternals.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "ElementInternals");
    idl_install_method(ctx, proto, "setFormValue", 1, g_id_set_form_value);
    idl_install_method(ctx, proto, "setValidity", 0, g_id_set_validity);
    idl_install_step_method(ctx, proto, "checkValidity", 0, g_id_check_validity);
    idl_install_step_method(ctx, proto, "reportValidity", 0, g_id_report_validity);
    idl_install_accessor(ctx, proto, "form", js_internals_get, EI_FORM, -1);
    idl_install_accessor(ctx, proto, "willValidate", js_internals_get, EI_WILL_VALIDATE, -1);
    idl_install_accessor(ctx, proto, "validity", js_internals_get, EI_VALIDITY, -1);
    idl_install_accessor(ctx, proto, "validationMessage", js_internals_get, EI_VALIDATION_MESSAGE, -1);
    idl_install_accessor(ctx, proto, "labels", js_internals_get, EI_LABELS, -1);
    idl_install_accessor(ctx, proto, "states", js_internals_get, EI_STATES, -1);
    for (i = 0; i < ARIA_N; i++)
        idl_install_accessor(ctx, proto, ARIA[i].member, js_internals_aria_get, i, g_id_aria_set[i]);
    JS_SetClassProto(ctx, g_internals_class, proto);

    proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(proto), "CustomStateSet.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "CustomStateSet");
    idl_install_method(ctx, proto, "has", 1, g_id_set_has);
    idl_install_method(ctx, proto, "add", 1, g_id_set_add);
    idl_install_method(ctx, proto, "delete", 1, g_id_set_delete);
    idl_install_method(ctx, proto, "clear", 0, g_id_set_clear);
    idl_install_accessor(ctx, proto, "size", js_states_size, 0, -1);
    idl_pair_iter_install(ctx, proto, g_set_pair_handle);
    JS_SetClassProto(ctx, g_states_class, proto);

    proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(proto), "ValidityState.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "ValidityState");
    for (i = 0; i < CV_STATE_COUNT; i++)
        idl_install_accessor(ctx, proto, VALIDITY_FLAG_NAMES[i], js_validity_get, i, -1);
    idl_install_accessor(ctx, proto, "valid", js_validity_get, CV_STATE_COUNT, -1);
    JS_SetClassProto(ctx, g_validity_class, proto);
}

void element_internals_install_html_members(JSContext *ctx, JSValueConst html_proto)
{
    DCHECK(g_id_attach >= 0, "attachInternals was installed before element_internals_declare ran");
    idl_install_method(ctx, html_proto, "attachInternals", 0, g_id_attach);
}

void element_internals_install(JSContext *ctx, JSValueConst global)
{
    static const char *const NAMES[3] = { "ElementInternals", "CustomStateSet", "ValidityState" };
    JSClassID classes[3];
    int i;

    DCHECK(g_ready, "§4.13.7's interface objects were installed before element_internals_declare ran");
    classes[0] = g_internals_class;
    classes[1] = g_states_class;
    classes[2] = g_validity_class;
    /* None of the three declares a constructor: the interface object exists to be what `instanceof` names, and
       calling or constructing it is a TypeError. */
    for (i = 0; i < 3; i++) {
        JSValue proto = JS_GetClassProto(ctx, classes[i]);

        DCHECK(!JS_IsNull(proto), "an §4.13.7 interface object was installed in a realm that never ran its "
                                  "prototype install");
        JS_SetPropertyStr(ctx, (JSValue)global, NAMES[i], idl_interface_object(ctx, NAMES[i], proto));
        JS_FreeValue(ctx, proto);
    }
}

void element_internals_free(JSContext *ctx)
{
    if (!g_ready) return;
    /* The slot keys are the AGENT's — a Symbol nobody frees is a live GC object the runtime's own walk counts
       as a leak. The prototypes are the REALMS', released with their contexts. */
    JS_FreeAtom(ctx, g_atom_target);
    JS_FreeAtom(ctx, g_atom_attached);
    JS_FreeAtom(ctx, g_atom_face);
    JS_FreeAtom(ctx, g_atom_states);
    JS_FreeAtom(ctx, g_atom_aria);
    JS_FreeAtom(ctx, g_atom_setitems);
    JS_FreeAtom(ctx, g_atom_vtarget);
    JS_FreeAtom(ctx, g_atom_validity);
    JS_FreeAtom(ctx, g_atom_value);
    JS_FreeAtom(ctx, g_atom_state);
    JS_FreeAtom(ctx, g_atom_flags);
    JS_FreeAtom(ctx, g_atom_message);
    JS_FreeAtom(ctx, g_atom_anchor);
    JS_FreeAtom(ctx, g_atom_custom);
    g_atom_target = g_atom_attached = g_atom_face = g_atom_states = g_atom_aria = JS_ATOM_NULL;
    g_atom_setitems = g_atom_vtarget = g_atom_validity = JS_ATOM_NULL;
    g_atom_value = g_atom_state = g_atom_flags = g_atom_message = g_atom_anchor = g_atom_custom = JS_ATOM_NULL;
    JS_FreeValue(ctx, g_target_key);
    JS_FreeValue(ctx, g_attached_key);
    JS_FreeValue(ctx, g_face_key);
    JS_FreeValue(ctx, g_states_key);
    JS_FreeValue(ctx, g_aria_key);
    JS_FreeValue(ctx, g_setitems_key);
    JS_FreeValue(ctx, g_vtarget_key);
    JS_FreeValue(ctx, g_validity_key);
    g_validity_key = JS_UNDEFINED;
    g_target_key = g_attached_key = g_face_key = g_states_key = g_aria_key = JS_UNDEFINED;
    g_setitems_key = g_vtarget_key = JS_UNDEFINED;
    g_ready = 0;
}
