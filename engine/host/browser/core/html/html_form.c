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

static bool has_attr(lxb_dom_element_t *el, const char *name)
{
    size_t len = 0;
    return attr_of(el, name, &len) != NULL;
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
    i = attr_shadow_find(el, ATTR_SLOT_PROPERTY, "value");
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
    i = attr_shadow_find(el, ATTR_SLOT_PROPERTY, "checked");
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
typedef struct JSReqSubmitState {
    JSStepHdr hdr;      /* FIRST — the driver writes the def and the operand bounds through it */
    uint8_t   stage;
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

    if (s->stage == 0) {
        s->ev = JS_UNDEFINED;
        s->cb[0] = s->cb[1] = s->cb[2] = s->cb[3] = JS_UNDEFINED;
        s->stage = 1;
        if (!form_elem_of(s->hdr.this_val)) {
            JS_FreeValue(ctx, cb_result);
            return JS_STEP_DONE;
        }
    }
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
    sizeof(JSReqSubmitState), js_reqsubmit_step, js_reqsubmit_fini, 0, .visit = js_reqsubmit_visit
};

/* §3.1.1 document.forms — every form in the document, in tree order. */

void html_form_install(JSContext *ctx, JSValueConst form_proto, JSValueConst input_proto,
                       JSValueConst textarea_proto, JSValueConst option_proto)
{
    static const IdlArgType NONE[1] = { IDL_ANY };
    int id;

    DCHECK(JS_IsObject(form_proto), "the form members were installed with no HTMLFormElement.prototype");
    idl_install_accessor(ctx, form_proto, "elements", js_form_elements, 0, -1);
    idl_install_method(ctx, form_proto, "submit", 0, idl_method_id(ctx, NONE, 1, js_form_submit, 0));
    idl_optional_from(0);   /* 4.10.21: `submit()` takes no arguments */
    /* requestSubmit registers its own step definition rather than declaring its arguments to the args machine,
       so it installs through the installer for that kind — see idl_install_step_method. */
    idl_install_step_method(ctx, form_proto, "requestSubmit", 0,
                            JS_RegisterStepDef(JS_GetRuntime(ctx), &js_reqsubmit_def));

    id = idl_setter_id(ctx, IDL_DOMSTRING, false, js_ctrl_set_value, CTRL_INPUT);
    idl_install_accessor(ctx, input_proto, "value", js_ctrl_get_value, CTRL_INPUT, id);
    id = idl_setter_id(ctx, IDL_DOMSTRING, false, js_ctrl_set_value, CTRL_TEXTAREA);
    idl_install_accessor(ctx, textarea_proto, "value", js_ctrl_get_value, CTRL_TEXTAREA, id);
    id = idl_setter_id(ctx, IDL_DOMSTRING, false, js_ctrl_set_value, CTRL_OPTION);
    idl_install_accessor(ctx, option_proto, "value", js_ctrl_get_value, CTRL_OPTION, id);

    id = idl_setter_id(ctx, IDL_ANY, false, js_ctrl_set_checked, 0);   /* `boolean checked` is ToBoolean */
    idl_install_accessor(ctx, input_proto, "checked", js_ctrl_get_checked, 0, id);
}


