/* FORMS — HTML §4.10.
 *
 * WHY THIS IS THE SURFACE THIS ENGINE EXISTS FOR. A form submission is a REQUEST: a method, an action URL and a
 * set of named values, every one of which the page's own code decides. This project's rules name it directly —
 * forms are learned by EXECUTION, through `.submit()`, not by reading attributes off the tree — because the
 * values a bundle puts into its fields before submitting ARE the example values an endpoint record wants, and
 * because a form built and submitted by code has no markup to read at all.
 *
 * IT IS NEVER FIRED. A submission mutates server state by construction, and this engine's rule for such a
 * request is that it is DERIVED from the forced-exec path and never sent. So submit() ends at endpoint_record.
 *
 * THE VALUE IS NOT THE ATTRIBUTE, which is the thing a naive implementation gets wrong. HTML keeps a control's
 * `value` MODE STATE apart from its `value` content attribute on purpose: the attribute is the DEFAULT (what
 * `defaultValue` reflects), and assigning changes only the state, which is why a form reset restores the
 * attribute. So the state lives in the element's per-flow PROPERTY slot — the one textContent already uses —
 * and two things follow for free. It TIME-TRAVELS, so `input.value = x` in one forked arm is invisible to its
 * sibling. And because that slot holds a JSValue rather than bytes, a CONCOLIC assigned to it survives into the
 * submission: `input.value = location.hash` then submit() is an endpoint carrying an attacker source, which is
 * the finding rather than a footnote. */
#include <string.h>
#include <stdlib.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/idl_args.h"
#include "core/dom/attr_list.h"
#include "core/dom/collections.h"
#include "core/dom/node.h"
#include "core/events/event.h"
#include "core/events/event_target.h"
#include "core/html/html_form.h"
#include "solver/attr_shadow.h"
#include "solver/concolic.h"
#include "solver/dom_cow.h"
#include "solver/endpoint.h"

static lxb_dom_element_t *form_elem_of(JSValueConst v)
{
    lxb_dom_node_t *n = node_of(v);
    return (n && n->type == LXB_DOM_NODE_TYPE_ELEMENT) ? lxb_dom_interface_element(n) : NULL;
}

static bool tag_is(lxb_dom_node_t *n, const char *name)
{
    size_t len = 0;
    const lxb_char_t *t;

    if (!n || n->type != LXB_DOM_NODE_TYPE_ELEMENT) return false;
    t = lxb_dom_element_local_name(lxb_dom_interface_element(n), &len);
    return t && len == strlen(name) && memcmp(t, name, len) == 0;
}

static const char *attr_of(lxb_dom_element_t *el, const char *name, size_t *plen)
{
    return (const char *)lxb_dom_element_get_attribute(el, (const lxb_char_t *)name, strlen(name), plen);
}

/* IS THE ATTRIBUTE PRESENT — asked of §4.9's attribute LIST and not of its value. This read the VALUE pointer
   and called a NULL one absent, which is right for an attribute that is not there and wrong for one whose
   value is EMPTY: the HTML parser stores a valueless `<x disabled>` with no value buffer, so a parsed
   `disabled` was invisible while `setAttribute('disabled','')` (which writes a zero-length buffer) was not.
   The two must be the same attribute, and §4.9 says presence is a question about the list. */
static bool has_attr(lxb_dom_element_t *el, const char *name)
{
    return dom_attr_get_ns(el, NULL, name) != NULL;
}

/* ---- a control's VALUE state ----------------------------------------------------------------------------- */
/* magic names whose DEFAULT applies when the page has assigned no state. */
enum { CTRL_INPUT = 0, CTRL_TEXTAREA, CTRL_OPTION };

static JSValue js_ctrl_get_value(JSContext *ctx, JSValueConst this_val, int magic)
{
    lxb_dom_element_t *el = form_elem_of(this_val);
    size_t len = 0;
    const char *a;
    int i;

    if (!el) return JS_NewStringLen(ctx, "", 0);
    i = attr_shadow_find(el, ATTR_SLOT_PROPERTY, NULL, "value");
    if (i >= 0) return JS_DupValue(ctx, attr_shadow_opaque(i));   /* the state the page assigned */

    /* §4.10.11 a textarea's default value is its CHILD TEXT; §4.10.10.12 an option with no `value` attribute
       has its text as its value. Everything else falls back to the `value` content attribute. */
    if (magic == CTRL_TEXTAREA || (magic == CTRL_OPTION && !has_attr(el, "value"))) {
        lxb_dom_node_t *n = lxb_dom_interface_node(el);
        lxb_char_t *txt = lxb_dom_node_text_content(n, &len);
        JSValue r;
        if (!txt) return JS_NewStringLen(ctx, "", 0);
        r = JS_NewStringLen(ctx, (const char *)txt, len);
        lxb_dom_document_destroy_text(n->owner_document, txt);
        return r;
    }
    a = attr_of(el, "value", &len);
    return a ? JS_NewStringLen(ctx, a, len) : JS_NewStringLen(ctx, "", 0);
}

static JSValue js_ctrl_set_value(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    lxb_dom_element_t *el = form_elem_of(this_val);

    (void)magic;
    /* The per-flow chokepoint, so the assignment reverts with every other DOM write this flow made — and the
       slot keeps the VALUE, so a concolic stays a concolic all the way to the submission. */
    if (el) dom_cow_set_prop_taint(ctx, el, "value", val);
    return JS_UNDEFINED;
}

/* §4.10.5.1.15 checkedness, defaulting to the `checked` content attribute. */
static JSValue js_ctrl_get_checked(JSContext *ctx, JSValueConst this_val, int magic)
{
    lxb_dom_element_t *el = form_elem_of(this_val);
    int i;

    (void)magic;
    if (!el) return JS_FALSE;
    i = attr_shadow_find(el, ATTR_SLOT_PROPERTY, NULL, "checked");
    if (i >= 0) return JS_NewBool(ctx, JS_ToBool(ctx, attr_shadow_opaque(i)));
    return JS_NewBool(ctx, has_attr(el, "checked"));
}

static JSValue js_ctrl_set_checked(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    lxb_dom_element_t *el = form_elem_of(this_val);
    JSValue b;

    (void)magic;
    if (!el) return JS_UNDEFINED;
    b = JS_NewBool(ctx, JS_ToBool(ctx, val));
    dom_cow_set_prop_taint(ctx, el, "checked", b);   /* BORROWED by the shadow, which dups it */
    JS_FreeValue(ctx, b);
    return JS_UNDEFINED;
}

/* ---- the form's controls --------------------------------------------------------------------------------- */
/* §4.10.18.4 a form's LISTED elements, in tree order. A STATIC array — the same named gap childNodes and
   querySelectorAll carry; the live HTMLFormControlsCollection is its own component. */
static JSValue form_controls(JSContext *ctx, lxb_dom_element_t *form)
{
    lxb_dom_node_t *root = lxb_dom_interface_node(form), *n = root;
    JSValue arr = JS_NewArray(ctx);
    uint32_t k = 0;

    for (;;) {
        if (n->first_child) { n = n->first_child; }
        else {
            while (n != root && !n->next) n = n->parent;
            if (n == root) break;
            n = n->next;
        }
        if (tag_is(n, "input") || tag_is(n, "select") || tag_is(n, "textarea") || tag_is(n, "button"))
            JS_SetPropertyUint32(ctx, arr, k++, node_wrap(ctx, n));
    }
    return arr;
}

static JSValue js_form_elements(JSContext *ctx, JSValueConst this_val, int magic)
{
    lxb_dom_element_t *el = form_elem_of(this_val);
    (void)magic;
    return el ? form_controls(ctx, el) : JS_NewArray(ctx);
}

/* ---- the submission -------------------------------------------------------------------------------------- */
typedef struct { char *s; size_t n, cap; } FormBuf;

static void fb_add(FormBuf *b, const char *s, size_t len)
{
    if (b->n + len + 1 > b->cap) {
        size_t c = b->cap ? b->cap * 2 : 128;
        while (c < b->n + len + 1) c *= 2;
        b->s = realloc(b->s, c);
        CHECK(b->s != NULL, "form: OOM building a submission URL — a dropped field is a missing endpoint value");
        b->cap = c;
    }
    memcpy(b->s + b->n, s, len);
    b->n += len;
    b->s[b->n] = 0;
}

static void fb_str(FormBuf *b, const char *s) { fb_add(b, s, strlen(s)); }

/* ONE ENTRY's value as display text: a concolic contributes its SHAPE, exactly as a concolic URL does when it
   reaches endpoint_record — so `input.value = location.hash` submits as `q={hash}` and the finding says where
   the value came from rather than inventing one. */
static void fb_value(JSContext *ctx, FormBuf *b, JSValueConst v)
{
    if (concolic_is(v)) {
        const char *sh = concolic_shape_c(v);
        fb_str(b, sh ? sh : "{}");
        return;
    }
    {
        const char *s = JS_ToCString(ctx, v);
        if (s) { fb_str(b, s); JS_FreeCString(ctx, s); }
    }
}

/* §4.10.21.3 "construct the entry list". A control contributes only when it is submittable, enabled and named —
   and a checkbox or radio only when it is CHECKED, which is why the checkedness state had to exist first. */
static void form_entry_list(JSContext *ctx, lxb_dom_element_t *form, FormBuf *b)
{
    JSValue arr = form_controls(ctx, form);
    uint32_t len = 0, i;
    bool first = true;

    JS_ToUint32(ctx, &len, JS_GetPropertyStr(ctx, arr, "length"));
    for (i = 0; i < len; i++) {
        JSValue wrap = JS_GetPropertyUint32(ctx, arr, i);
        lxb_dom_element_t *c = form_elem_of(wrap);
        lxb_dom_node_t *cn = c ? lxb_dom_interface_node(c) : NULL;
        size_t nlen = 0, tlen = 0;
        const char *name, *type;
        bool skip = false;

        if (!c) { JS_FreeValue(ctx, wrap); continue; }
        name = attr_of(c, "name", &nlen);
        if (!name || !nlen || has_attr(c, "disabled") || tag_is(cn, "button")) skip = true;
        type = skip ? NULL : attr_of(c, "type", &tlen);
        if (!skip && tag_is(cn, "input") && type) {
            if ((tlen == 8 && memcmp(type, "checkbox", 8) == 0) ||
                (tlen == 5 && memcmp(type, "radio", 5) == 0)) {
                JSValue ck = js_ctrl_get_checked(ctx, wrap, 0);
                skip = !JS_ToBool(ctx, ck);
                JS_FreeValue(ctx, ck);
            } else if ((tlen == 6 && memcmp(type, "submit", 6) == 0) ||
                       (tlen == 6 && memcmp(type, "button", 6) == 0) ||
                       (tlen == 5 && memcmp(type, "reset", 5) == 0)) {
                skip = true;   /* a button-shaped input contributes only as the submitter, which submit() has none of */
            }
        }
        if (!skip) {
            JSValue v = js_ctrl_get_value(ctx, wrap, tag_is(cn, "textarea") ? CTRL_TEXTAREA : CTRL_INPUT);
            fb_str(b, first ? "?" : "&");
            first = false;
            fb_add(b, name, nlen);
            fb_str(b, "=");
            fb_value(ctx, b, v);
            JS_FreeValue(ctx, v);
        }
        JS_FreeValue(ctx, wrap);
    }
    JS_FreeValue(ctx, arr);
}

/* §4.10.21.4 "submit": DERIVE the request and record it. form.submit() does NOT fire `submit` and does not
   validate, which is exactly why it needs no machine — nothing on this path reaches the page's code. */
static void form_submit_now(JSContext *ctx, lxb_dom_element_t *form)
{
    size_t alen = 0, mlen = 0;
    const char *action, *method;
    bool post;
    FormBuf b = { 0 };
    JSValue url;

    if (!form) return;
    action = attr_of(form, "action", &alen);
    method = attr_of(form, "method", &mlen);
    post = method && mlen == 4 && (method[0] == 'p' || method[0] == 'P');

    if (action && alen) fb_add(&b, action, alen);   /* §4.10.21.3: an absent action submits to the document's URL */
    /* The entry list is the QUERY for a GET and the BODY for a POST. It is appended either way, because an @H
       record's `params` are the request's parameters and a POST's parameters are its body — what tells them
       apart is the method, recorded beside them. */
    form_entry_list(ctx, form, &b);

    url = JS_NewStringLen(ctx, b.s ? b.s : "", b.s ? b.n : 0);
    endpoint_record(ctx, post ? "POST" : "GET", url, NULL, 0);
    JS_FreeValue(ctx, url);
    free(b.s);
}

/* form.submit() — §4.10.21.4 without the event: it does NOT fire `submit` and does not validate, which is
   exactly why it needs no machine. Nothing on this path reaches the page's code. */
static JSValue js_form_submit(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    (void)argc; (void)argv; (void)magic;
    form_submit_now(ctx, form_elem_of(this_val));
    return JS_UNDEFINED;
}

/* §4.10.21.4 requestSubmit() — fires a CANCELABLE `submit` event and submits only if nothing cancelled it.
   That is what makes it a MACHINE where submit() is not: the handlers are the page's code, and their verdict
   decides whether the request exists at all. A page that wires a submit handler and calls preventDefault is
   doing its own fetch instead, and recording the form's request anyway would be a finding the page never
   makes. */
/* WHERE THIS MACHINE RESTS. §4.10.21.4's step 5.6 is the one thing here that reaches the page's code — "let
   shouldContinue be the result of firing an event named submit at form ... with the cancelable attribute
   initialized to true" — and step 5.8 is what its verdict decides. Everything after it, up to the request this
   engine records, runs with no page code in it, so one stage names that range and says so. */
#define REQSUBMIT_STAGES(X) \
    X(REQSUBMIT_FORM, "HTML §4.10.21.4 (the form this request is for, before step 5.6's event)") \
    X(REQSUBMIT_FIRE, "HTML §4.10.21.4 step 5.6 (fire an event named submit at form, cancelable), then step " \
                      "5.8's verdict and the submission it allows")
enum { REQSUBMIT_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const REQSUBMIT_STEPS[] = { REQSUBMIT_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct JSReqSubmitState {
    JSStepHdr hdr;      /* FIRST — the driver writes the def and the operand bounds through it */
    uint8_t   fphase;   /* the fire request's own phase */
    JSValue   ev;       /* the event, minted once and held across the suspension (owned) */
    JSValue   cb[4];    /* the fire request buffer: [this, dispatch, target, event] */
} JSReqSubmitState;

static void js_reqsubmit_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSReqSubmitState *s = st;
    int k;
    v->val(ctx, &s->ev);
    for (k = 0; k < 4; k++)
        v->val(ctx, &s->cb[k]);
}

static JSValue js_reqsubmit_fini(JSContext *ctx, void *st, bool take_result)
{
    JSReqSubmitState *s = st;
    int k;
    (void)take_result;
    JS_FreeValue(ctx, s->ev);
    s->ev = JS_UNDEFINED;
    for (k = 0; k < 4; k++) {
        JS_FreeValue(ctx, s->cb[k]);
        s->cb[k] = JS_UNDEFINED;
    }
    return JS_UNDEFINED;   /* §4.10.21.4 returns undefined whatever the handlers did */
}

static int js_reqsubmit_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSReqSubmitState *s = st;
    bool not_canceled = true;
    int r;

    if (s->hdr.stage == REQSUBMIT_FORM) {
        s->ev = JS_UNDEFINED;
        s->cb[0] = s->cb[1] = s->cb[2] = s->cb[3] = JS_UNDEFINED;
        s->hdr.stage = REQSUBMIT_FIRE;
        if (!form_elem_of(s->hdr.this_val)) {
            JS_FreeValue(ctx, cb_result);
            return JS_STEP_DONE;
        }
    }
    DCHECK(s->hdr.stage == REQSUBMIT_FIRE, "requestSubmit resumed into a stage §4.10.21.4 does not have");
    /* §4.10.21.4 step 4: "fire an event named submit at form, with the cancelable attribute initialized to
       true" — the ONE §2.9 dispatch, as a REQUEST, so the handlers run as ordinary preemptible page code and
       this resumes after every one of them has returned. */
    if (JS_IsUndefined(s->ev))
        s->ev = event_new(ctx, "submit", /*bubbles*/ true, /*cancelable*/ true);
    r = event_target_fire_run(ctx, &s->fphase, STEP_CB(s->cb), s->hdr.this_val, s->ev, cb_result, &not_canceled,
                              out_cb, out_argc);
    if (r > 0) return r;
    if (r < 0) return JS_STEP_ABRUPT;
    if (not_canceled)
        form_submit_now(ctx, form_elem_of(s->hdr.this_val));
    return JS_STEP_DONE;
}

static const JSTrampStepDef js_reqsubmit_def = {
    sizeof(JSReqSubmitState), js_reqsubmit_step, js_reqsubmit_fini, 0, .visit = js_reqsubmit_visit,
    .algorithm = "HTML §4.10.21.4 submit a form (from requestSubmit)", .steps = REQSUBMIT_STEPS
};

/* ---- §4.10.18.3 the form owner ------------------------------------------------------------------------------
 *
 * STORED, NOT DERIVED. The spec initialises a form-associated element's form owner to null and RESETS it at
 * named moments, and the difference is observable: a `<my-control form="nothing">` inside a `<form>` has a NULL
 * owner, because step 4's branch was taken and its ID lookup found no form — a "nearest ancestor form" derived
 * at read time would answer the enclosing form instead.
 *
 * THE STORE IS AN OWN SLOT ON THE ELEMENT'S WRAPPER, under a symbol this file minted and never published. That
 * makes it per-flow for free: two forked arms that move the same element into two different forms each see
 * their own owner, and a parked flow resumes with the one it had.
 *
 * The only form-associated elements this engine has are FORM-ASSOCIATED CUSTOM ELEMENTS. The built-in controls
 * reach their form through `form.elements`' tree walk, which stores nothing and has no formAssociatedCallback
 * to fire; the day `input.form` exists it is this same owner, reset from the same places. */
static JSValue g_owner_key = JS_UNDEFINED;
static JSAtom  g_atom_owner = JS_ATOM_NULL;

/* §4.10.18.3: "when a form-associated element is created, its form owner must be initialized to null" — so an
   element with no slot has a null owner, which is what an absent slot means and why nothing writes one at
   creation. OWNED. */
JSValue html_form_owner_of(JSContext *ctx, JSValueConst wrap)
{
    JSValue v;

    DCHECK(g_atom_owner != JS_ATOM_NULL,
           "a form owner was asked for before html_form_declare minted its slot key");
    if (!JS_IsObject(wrap)) return JS_NULL;
    if (JS_GetOwnSlot(ctx, &v, wrap, g_atom_owner) <= 0) return JS_NULL;
    if (JS_IsObject(v)) return v;
    JS_FreeValue(ctx, v);
    return JS_NULL;
}

/* CONFIGURABLE AND WRITABLE for the reason the custom-element state slot is: the owner is written again at
   every reset, and a slot defined with no flags makes the second write a silent no-op. */
static void form_set_owner(JSContext *ctx, JSValueConst wrap, JSValueConst owner)
{
    JS_DefinePropertyValue(ctx, (JSValue)wrap, g_atom_owner, JS_DupValue(ctx, owner),
                           JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE);
}

/* The nearest ANCESTOR form element, or NULL — step 2's third condition and step 5's target. */
static lxb_dom_node_t *form_nearest_ancestor(lxb_dom_node_t *n)
{
    for (n = n ? n->parent : NULL; n; n = n->parent)
        if (tag_is(n, "form")) return n;
    return NULL;
}

/* Step 4.1: "the first element in element's TREE, in tree order, to have an ID identical to element's form
   content attribute's value". The element's TREE and not the document — a detached subtree has its own root,
   and step 4's "and is connected" is what keeps this to documents in practice. */
static lxb_dom_node_t *form_first_by_id(lxb_dom_node_t *root, const char *id, size_t idlen)
{
    lxb_dom_node_t *n = root;

    for (; n; ) {
        if (n->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            size_t len = 0;
            const char *v = attr_of(lxb_dom_interface_element(n), "id", &len);
            if (v && len == idlen && memcmp(v, id, idlen) == 0) return n;
        }
        if (n->first_child) { n = n->first_child; continue; }
        while (n && !n->next) n = (n == root) ? NULL : n->parent;
        n = n ? n->next : NULL;
    }
    return NULL;
}

JSValue html_form_reset_owner_with_attr(JSContext *ctx, JSValueConst wrap, const char *form_attr,
                                        size_t form_attr_len, bool *pchanged)
{
    lxb_dom_node_t *n = node_of(wrap);
    JSValue was, now = JS_NULL;

    if (pchanged) *pchanged = false;
    if (!n || n->type != LXB_DOM_NODE_TYPE_ELEMENT) return JS_NULL;
    was = html_form_owner_of(ctx, wrap);
    /* Step 1 is "unset element's parser inserted flag", and this engine has no parser-inserted association to
       unset: HTML's tree builder sets that flag only where it associates a control with a form that is not its
       ancestor, through the "form element pointer" this engine's parser does not keep. It becomes a real field
       in the same diff that pointer does — and until then there is nothing to unset rather than a step being
       skipped.
       Step 2's early return: the owner is unchanged when the element already has one, has no `form` content
       attribute, and that owner is its nearest ancestor form. */
    if (JS_IsObject(was) && !form_attr) {
        lxb_dom_node_t *anc = form_nearest_ancestor(n);
        JSValue anc_w = anc ? node_wrap(ctx, anc) : JS_NULL;
        bool same = JS_VALUE_GET_PTR(anc_w) == JS_VALUE_GET_PTR(was);

        JS_FreeValue(ctx, anc_w);
        if (same) return was;
    }
    /* Step 3: the owner is null until step 4 or step 5 puts one back. */
    if (form_attr) {
        /* Step 4 — a form-associated custom element is LISTED, so the condition is the attribute plus
           connectedness. Its ID lookup answering something that is not a form leaves the owner NULL, which is
           what makes step 5 an "otherwise" on step 4's CONDITION rather than on its result. */
        if (node_is_connected(n)) {
            lxb_dom_node_t *hit = form_first_by_id(node_root(n), form_attr, form_attr_len);
            if (hit && tag_is(hit, "form")) now = node_wrap(ctx, hit);
        }
    } else {
        lxb_dom_node_t *anc = form_nearest_ancestor(n);   /* step 5 */
        if (anc) now = node_wrap(ctx, anc);
    }
    form_set_owner(ctx, wrap, now);
    if (pchanged) *pchanged = JS_VALUE_GET_PTR(now) != JS_VALUE_GET_PTR(was);
    JS_FreeValue(ctx, was);
    return now;
}

JSValue html_form_reset_owner(JSContext *ctx, JSValueConst wrap, bool *pchanged)
{
    lxb_dom_node_t *n = node_of(wrap);
    size_t len = 0;
    const char *v;

    if (pchanged) *pchanged = false;
    if (!n || n->type != LXB_DOM_NODE_TYPE_ELEMENT) return JS_NULL;
    v = attr_of(lxb_dom_interface_element(n), "form", &len);
    return html_form_reset_owner_with_attr(ctx, wrap, v, len, pchanged);
}

/* ---- §4.10.19 label association ----------------------------------------------------------------------------
 * A label's LABELED CONTROL is the element its `for` attribute names, when that element is labelable, and
 * otherwise its first labelable descendant. `labels` is the other direction over the same relation, which is
 * why both go through one predicate: written apart, the two stop agreeing. */

/* Is `n` the labeled control of the label `lab`? */
static bool form_label_controls(lxb_dom_node_t *lab, lxb_dom_node_t *n)
{
    size_t len = 0;
    const char *f = attr_of(lxb_dom_interface_element(lab), "for", &len);

    if (f) {
        size_t idlen = 0;
        const char *id = attr_of(lxb_dom_interface_element(n), "id", &idlen);
        /* The FIRST element in the label's tree with that ID, which is not necessarily this one. */
        return id != NULL && idlen == len && memcmp(id, f, len) == 0 &&
               form_first_by_id(node_root(lab), f, len) == n;
    }
    /* No `for`: the first LABELABLE descendant, which is where `<label><span><my-control>` finds its control
       and where `<label><x-foo>` finds none. A form-associated custom element is labelable and so are the
       built-in controls; nothing else in this engine is. */
    {
        lxb_dom_node_t *c = lab;

        for (;;) {
            if (c->first_child) { c = c->first_child; }
            else {
                while (c != lab && !c->next) c = c->parent;
                if (c == lab) return false;
                c = c->next;
            }
            if (!c || c->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
            if (c == n) return true;
            if (tag_is(c, "input") || tag_is(c, "select") || tag_is(c, "textarea") || tag_is(c, "button"))
                return false;
        }
    }
}

JSValue html_form_labels_of(JSContext *ctx, JSValueConst wrap)
{
    lxb_dom_node_t *n = node_of(wrap), *root, *c;
    JSValue arr = JS_NewArray(ctx);
    uint32_t k = 0;

    CHECK(!JS_IsException(arr), "forms: OOM building a labels NodeList");
    if (!n || n->type != LXB_DOM_NODE_TYPE_ELEMENT) return collections_static(ctx, arr);
    root = node_root(n);
    for (c = root; c; ) {
        if (c->type == LXB_DOM_NODE_TYPE_ELEMENT && tag_is(c, "label") && form_label_controls(c, n))
            JS_SetPropertyUint32(ctx, arr, k++, node_wrap(ctx, c));
        if (c->first_child) { c = c->first_child; continue; }
        while (c && !c->next) c = (c == root) ? NULL : c->parent;
        c = c ? c->next : NULL;
    }
    return collections_static(ctx, arr);
}

bool html_form_control_is_disabled(JSContext *ctx, JSValueConst wrap)
{
    lxb_dom_node_t *n = node_of(wrap), *a;

    (void)ctx;
    if (!n || n->type != LXB_DOM_NODE_TYPE_ELEMENT) return false;
    if (has_attr(lxb_dom_interface_element(n), "disabled")) return true;
    /* "a descendant of a fieldset whose disabled attribute is specified, and not a descendant of that
       fieldset's FIRST legend element child" — the legend carve-out is what keeps a disabled fieldset's own
       caption interactive, and leaving it out disables exactly the controls a page put there. */
    for (a = n->parent; a; a = a->parent) {
        lxb_dom_node_t *leg;

        if (!tag_is(a, "fieldset") || !has_attr(lxb_dom_interface_element(a), "disabled")) continue;
        for (leg = a->first_child; leg; leg = leg->next)
            if (tag_is(leg, "legend")) break;
        if (leg) {
            lxb_dom_node_t *u;
            bool inside = false;
            for (u = n; u && u != a; u = u->parent)
                if (u == leg) { inside = true; break; }
            if (inside) continue;
        }
        return true;
    }
    return false;
}

/* §3.1.1 document.forms — every form in the document, in tree order. */

/* Declared once per AGENT, installed per realm — see hyperlink.c for why the split exists at all. */
static int g_id_submit = -1, g_id_reqsubmit = -1, g_id_val_input = -1, g_id_val_textarea = -1,
           g_id_val_option = -1, g_id_checked = -1;

void html_form_declare(JSContext *ctx)
{
    static const IdlArgType NONE[1] = { IDL_ANY };

    /* §4.10.18.3's form-owner slot key — a Symbol, so the owner is not a string property of this engine's
       invention sitting on every form-associated element where a page can see it. Minted once per AGENT,
       beside the member declarations, because the key identifies the same slot in every realm. */
    DCHECK(JS_IsUndefined(g_owner_key), "html_form_declare ran twice — one instance is one agent");
    g_owner_key = JS_NewSymbol(ctx, "formOwner", false);
    CHECK(!JS_IsException(g_owner_key), "the form-owner slot key allocation failed");
    g_atom_owner = JS_ValueToAtom(ctx, g_owner_key);
    CHECK(g_atom_owner != JS_ATOM_NULL, "the form-owner slot key could not be interned");
    g_id_submit = idl_method_id(ctx, NONE, 1, js_form_submit, 0);
    idl_optional_from(0);   /* 4.10.21: `submit()` takes no arguments */
    /* requestSubmit registers its own step definition rather than declaring its arguments to the args machine,
       so it installs through the installer for that kind — see idl_install_step_method. */
    g_id_reqsubmit = JS_RegisterStepDef(JS_GetRuntime(ctx), &js_reqsubmit_def);
    g_id_val_input = idl_setter_id(ctx, IDL_DOMSTRING, false, js_ctrl_set_value, CTRL_INPUT);
    g_id_val_textarea = idl_setter_id(ctx, IDL_DOMSTRING, false, js_ctrl_set_value, CTRL_TEXTAREA);
    g_id_val_option = idl_setter_id(ctx, IDL_DOMSTRING, false, js_ctrl_set_value, CTRL_OPTION);
    g_id_checked = idl_setter_id(ctx, IDL_ANY, false, js_ctrl_set_checked, 0);   /* `boolean checked` is ToBoolean */
}

void html_form_install(JSContext *ctx, JSValueConst form_proto, JSValueConst input_proto,
                       JSValueConst textarea_proto, JSValueConst option_proto)
{
    DCHECK(g_id_submit >= 0, "§4.10's members were installed before they were declared");
    DCHECK(JS_IsObject(form_proto), "the form members were installed with no HTMLFormElement.prototype");
    idl_install_accessor(ctx, form_proto, "elements", js_form_elements, 0, -1);
    idl_install_method(ctx, form_proto, "submit", 0, g_id_submit);
    idl_install_step_method(ctx, form_proto, "requestSubmit", 0, g_id_reqsubmit);
    idl_install_accessor(ctx, input_proto, "value", js_ctrl_get_value, CTRL_INPUT, g_id_val_input);
    idl_install_accessor(ctx, textarea_proto, "value", js_ctrl_get_value, CTRL_TEXTAREA, g_id_val_textarea);
    idl_install_accessor(ctx, option_proto, "value", js_ctrl_get_value, CTRL_OPTION, g_id_val_option);
    idl_install_accessor(ctx, input_proto, "checked", js_ctrl_get_checked, 0, g_id_checked);
}

void html_form_free(JSContext *ctx)
{
    /* The form-owner slot key is the AGENT's, so it is released with the agent — a Symbol nobody frees is a
       live GC object the runtime's own walk counts as a leak. */
    JS_FreeAtom(ctx, g_atom_owner);
    g_atom_owner = JS_ATOM_NULL;
    JS_FreeValue(ctx, g_owner_key);
    g_owner_key = JS_UNDEFINED;
}
