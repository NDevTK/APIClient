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
#include "core/encoding/encoding.h"
#include "core/events/event.h"
#include "core/events/event_target.h"
#include "core/html/custom_elements.h"
#include "core/html/form_data.h"
#include "core/html/form_data_event.h"
#include "core/html/form_entry_list.h"
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

/* An ASCII CASE-INSENSITIVE match against a lower-case literal — which is what an ENUMERATED attribute's
   keywords are compared with (§2.3.2), and what §4.10.22.4 step 5.9 says of `_charset_`. Written here rather
   than reached for as `strncasecmp` because that one is the C LOCALE's, and a Turkish locale folds `I` to `ı`:
   an attribute keyword is ASCII by definition and must not depend on where the process is running. */
static bool ascii_ci_is(const char *a, size_t alen, const char *lower)
{
    size_t i, n = strlen(lower);

    if (!a || alen != n) return false;
    for (i = 0; i < n; i++) {
        char c = a[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != lower[i]) return false;
    }
    return true;
}

/* ---- §4.10.2's CATEGORIES ------------------------------------------------------------------------------------
 *
 * The lists are the standard's, stated once. Every one of them includes FORM-ASSOCIATED CUSTOM ELEMENTS, which
 * is why each takes the WRAPPER: only a custom element's definition can say it is form-associated, and the tag
 * cannot. */

/* An `input` element's `type` state, as the raw attribute — the caller compares it with ascii_ci_is. A missing
   or invalid value is the TEXT state (§4.10.5.1.1's missing/invalid value default), which every comparison here
   answers false for, so an absent attribute needs no case of its own. */
static const char *input_type_of(lxb_dom_node_t *n, size_t *plen)
{
    *plen = 0;
    if (!tag_is(n, "input")) return NULL;
    return attr_of(lxb_dom_interface_element(n), "type", plen);
}

bool html_form_is_submittable(JSContext *ctx, JSValueConst wrap)
{
    lxb_dom_node_t *n = node_of(wrap);

    if (!n || n->type != LXB_DOM_NODE_TYPE_ELEMENT) return false;
    return tag_is(n, "button") || tag_is(n, "input") || tag_is(n, "select") || tag_is(n, "textarea") ||
           custom_elements_is_form_associated(ctx, wrap);
}

/* §4.10.2's LISTED elements — `form.elements`' filter, and the wider list submittable is a subset of. */
static bool form_is_listed(JSContext *ctx, JSValueConst wrap)
{
    lxb_dom_node_t *n = node_of(wrap);

    if (!n || n->type != LXB_DOM_NODE_TYPE_ELEMENT) return false;
    return tag_is(n, "button") || tag_is(n, "fieldset") || tag_is(n, "input") || tag_is(n, "object") ||
           tag_is(n, "output") || tag_is(n, "select") || tag_is(n, "textarea") ||
           custom_elements_is_form_associated(ctx, wrap);
}

bool html_form_is_button(JSContext *ctx, JSValueConst wrap)
{
    lxb_dom_node_t *n = node_of(wrap);
    size_t tlen = 0;
    const char *t;

    (void)ctx;
    if (!n || n->type != LXB_DOM_NODE_TYPE_ELEMENT) return false;
    if (tag_is(n, "button")) return true;   /* §4.10.6: "The element is a button." */
    t = input_type_of(n, &tlen);
    /* §4.10.5.1.18/.19/.20/.21 each end "The element is a button"; no other input state does. */
    return ascii_ci_is(t, tlen, "submit") || ascii_ci_is(t, tlen, "image") ||
           ascii_ci_is(t, tlen, "reset")  || ascii_ci_is(t, tlen, "button");
}

bool html_form_is_submit_button(JSContext *ctx, JSValueConst wrap)
{
    lxb_dom_node_t *n = node_of(wrap);
    size_t tlen = 0;
    const char *t;

    (void)ctx;
    if (!n || n->type != LXB_DOM_NODE_TYPE_ELEMENT) return false;
    if (tag_is(n, "input")) {
        t = input_type_of(n, &tlen);
        /* §4.10.5.1.18 and §4.10.5.1.19: "specifically a submit button". */
        return ascii_ci_is(t, tlen, "submit") || ascii_ci_is(t, tlen, "image");
    }
    if (!tag_is(n, "button")) return false;
    /* §4.10.6: a `button` is a submit button if its `type` is in the Submit Button state, or in the AUTO state
       (missing or invalid) with neither `command` nor `commandfor` present and a parent that is not a select. */
    t = attr_of(lxb_dom_interface_element(n), "type", &tlen);
    if (ascii_ci_is(t, tlen, "submit")) return true;
    if (ascii_ci_is(t, tlen, "reset") || ascii_ci_is(t, tlen, "button")) return false;
    return !has_attr(lxb_dom_interface_element(n), "command") &&
           !has_attr(lxb_dom_interface_element(n), "commandfor") &&
           !tag_is(n->parent, "select");
}

bool html_form_is_form_element(JSValueConst v)
{
    lxb_dom_node_t *n = node_of(v);

    return n != NULL && n->type == LXB_DOM_NODE_TYPE_ELEMENT && n->ns == LXB_NS_HTML && tag_is(n, "form");
}

/* ---- a growable byte buffer, for the two algorithms here that build one ------------------------------------ */
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

/* §4.10.10's "COLLECT OPTION TEXT given option and false" — the value of an `option` that carries no `value`
   attribute. It is NOT the node's text content: the algorithm SKIPS `script` and SVG `script` subtrees, and it
   ends by STRIPPING AND COLLAPSING ASCII whitespace, so `<option>\n  Two words\n</option>` submits as
   "Two words" and not as the markup's indentation. */
static JSValue option_collect_text(JSContext *ctx, lxb_dom_node_t *root)
{
    FormBuf b = { 0 };
    lxb_dom_node_t *n = root;
    JSValue r;
    size_t i, w = 0;
    bool pending_space = false, any = false;

    for (;;) {
        bool skip_subtree = n != root && n->type == LXB_DOM_NODE_TYPE_ELEMENT && tag_is(n, "script");

        if (!skip_subtree && n->first_child) { n = n->first_child; }
        else {
            while (n != root && !n->next) n = n->parent;
            if (n == root) break;
            n = n->next;
        }
        if (n->type == LXB_DOM_NODE_TYPE_TEXT) {
            const lxb_dom_character_data_t *cd = (const lxb_dom_character_data_t *)n;
            if (cd->data.data && cd->data.length)
                fb_add(&b, (const char *)cd->data.data, cd->data.length);
        }
    }
    /* Infra's "strip and collapse ASCII whitespace": leading and trailing runs removed, every interior run
       replaced by a single U+0020. */
    for (i = 0; i < b.n; i++) {
        char c = b.s[i];
        if (c == '\t' || c == '\n' || c == '\f' || c == '\r' || c == ' ') { pending_space = any; continue; }
        if (pending_space) { b.s[w++] = ' '; pending_space = false; }
        b.s[w++] = c;
        any = true;
    }
    r = JS_NewStringLen(ctx, b.s ? b.s : "", w);
    free(b.s);
    return r;
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

    /* §4.10.11 a textarea's default value is its CHILD TEXT, raw. */
    if (magic == CTRL_TEXTAREA) {
        lxb_dom_node_t *n = lxb_dom_interface_node(el);
        lxb_char_t *txt = lxb_dom_node_text_content(n, &len);
        JSValue r;
        if (!txt) return JS_NewStringLen(ctx, "", 0);
        r = JS_NewStringLen(ctx, (const char *)txt, len);
        lxb_dom_document_destroy_text(n->owner_document, txt);
        return r;
    }
    /* §4.10.10: "The value of an option element is the value of the `value` content attribute, if there is one,
       or, if there is not, the result of COLLECT OPTION TEXT given this and false." */
    if (magic == CTRL_OPTION && !has_attr(el, "value"))
        return option_collect_text(ctx, lxb_dom_interface_node(el));
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

/* ---- the form's controls ---------------------------------------------------------------------------------
 *
 * §4.10.18.4 and §4.10.22.4 step 3 ask the SAME question of two different categories: which elements of the
 * form's ROOT — not of its subtree — have this form as their FORM OWNER. The subtree walk that stood here was
 * neither: it missed a `<input form=f>` sitting outside the form (which is what the `form` content attribute
 * exists for) and it missed FORM-ASSOCIATED CUSTOM ELEMENTS entirely, which is the same gap `new FormData(form)`
 * had from the other end. One walk, and the CATEGORY is the caller's predicate. */
typedef bool (*FormMemberPred)(JSContext *ctx, JSValueConst wrap);

static JSValue form_members(JSContext *ctx, JSValueConst form_wrap, FormMemberPred want)
{
    lxb_dom_node_t *form = node_of(form_wrap), *root, *n;
    JSValue arr = JS_NewArray(ctx);
    uint32_t k = 0;

    CHECK(!JS_IsException(arr), "forms: OOM building a form's control list");
    if (!form) return arr;
    root = node_root(form);
    for (n = root; n; ) {
        if (n->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            JSValue wrap = node_wrap(ctx, n);
            if (want(ctx, wrap)) {
                JSValue owner = html_form_owner_of(ctx, wrap);
                bool mine = JS_VALUE_GET_PTR(owner) == JS_VALUE_GET_PTR(form_wrap);
                JS_FreeValue(ctx, owner);
                if (mine) { JS_SetPropertyUint32(ctx, arr, k++, JS_DupValue(ctx, wrap)); }
            }
            JS_FreeValue(ctx, wrap);
        }
        if (n->first_child) { n = n->first_child; continue; }
        while (n && !n->next) n = (n == root) ? NULL : n->parent;
        n = n ? n->next : NULL;
    }
    return arr;
}

/* §4.10.18.4's `elements` filter: LISTED elements, "with the exception of input elements whose type attribute
   is in the Image Button state, which must, for historical reasons, be excluded from this particular
   collection". */
static bool form_elements_member(JSContext *ctx, JSValueConst wrap)
{
    lxb_dom_node_t *n = node_of(wrap);
    size_t tlen = 0;
    const char *t = input_type_of(n, &tlen);

    if (ascii_ci_is(t, tlen, "image")) return false;
    return form_is_listed(ctx, wrap);
}

JSValue html_form_submittable_controls(JSContext *ctx, JSValueConst form)
{
    return form_members(ctx, form, html_form_is_submittable);
}

/* A STATIC array — the same named gap childNodes and querySelectorAll carry; the live
   HTMLFormControlsCollection is its own component. */
static JSValue js_form_elements(JSContext *ctx, JSValueConst this_val, int magic)
{
    (void)magic;
    if (!form_elem_of(this_val)) return JS_NewArray(ctx);
    return form_members(ctx, this_val, form_elements_member);
}

/* ---- §4.10.21.3 "submit a form", and the two members that enter it ------------------------------------------
 *
 * BOTH ENTRY POINTS ARE MACHINES NOW, and the reason is the entry list. `submit()` was a plain C body on the
 * claim that "nothing on this path reaches the page's code" — which was true of the walk that used to stand
 * here and is FALSE of §4.10.22.4, whose step 7 fires a `formdata` event at the form. A page listening for one
 * appends to the FormData it is handed, and what it appends is part of the request; running that dispatch from
 * a C body would be the drive-to-completion the engine aborts on.
 *
 * IT IS NEVER FIRED. A submission mutates server state by construction, so this ends at endpoint_record. */

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

/* §4.10.22.5 "SELECTING A FORM SUBMISSION ENCODING". The document's character encoding is UTF-8 in this engine
   — the parse is Lexbor's and it decodes to it — so the only thing that can move this is `accept-charset`:
   split it on ASCII whitespace, GET AN ENCODING for each token in order, and take the first that is not a
   failure; UTF-8 when the attribute names none that is. The result is observable through exactly one entry:
   §4.10.22.4 step 5.9's `_charset_`. */
static const char *form_pick_encoding(lxb_dom_element_t *form)
{
    size_t len = 0, i;
    const char *v = attr_of(form, "accept-charset", &len);

    if (!v) return "UTF-8";
    for (i = 0; i < len; ) {
        size_t j;
        int enc;
        while (i < len && (v[i] == '\t' || v[i] == '\n' || v[i] == '\f' || v[i] == '\r' || v[i] == ' ')) i++;
        for (j = i; j < len && !(v[j] == '\t' || v[j] == '\n' || v[j] == '\f' || v[j] == '\r' || v[j] == ' '); j++)
            ;
        if (j == i) break;
        enc = encoding_lookup(v + i, j - i);
        if (enc >= 0) return encoding_name_of(enc);
        i = j;
    }
    return "UTF-8";
}

/* §4.10.21.2: the ACTION and the METHOD are the SUBMITTER's, not the form's — a submit button carrying
   `formaction`/`formmethod` overrides them, which is how one form posts to two endpoints. The submitter is the
   form itself when there was none, and a form has neither attribute, so one lookup answers both cases. */
static const char *submitter_attr(lxb_dom_element_t *submitter, lxb_dom_element_t *form,
                                  const char *own, const char *fallback, size_t *plen)
{
    const char *v = submitter != form ? attr_of(submitter, own, plen) : NULL;

    return v ? v : attr_of(form, fallback, plen);
}

/* §4.10.21.3 steps 18-23, as far as this engine goes: DERIVE the request the form would send and record it.
   `entries` is §4.10.22.4's entry list, as the FormData it was constructed into. */
static void form_record_request(JSContext *ctx, lxb_dom_element_t *form, lxb_dom_element_t *submitter,
                                JSValueConst entries)
{
    size_t alen = 0, mlen = 0;
    const char *action, *method;
    bool post;
    FormBuf b = { 0 };
    JSValue url;
    int i, n;

    action = submitter_attr(submitter, form, "formaction", "action", &alen);
    method = submitter_attr(submitter, form, "formmethod", "method", &mlen);
    post = ascii_ci_is(method, mlen, "post");

    if (action && alen) fb_add(&b, action, alen);   /* §4.10.21.3: an empty action submits to the document's URL */
    /* The entry list is the QUERY for a GET and the BODY for a POST. It is appended either way, because an @H
       record's `params` are the request's parameters and a POST's parameters are its body — what tells them
       apart is the method, recorded beside them. */
    n = form_data_entry_count(entries);
    for (i = 0; i < n; i++) {
        size_t nlen = 0;
        const char *name = form_data_entry_name(entries, i, &nlen);

        fb_str(&b, i == 0 ? "?" : "&");
        fb_add(&b, name, nlen);
        fb_str(&b, "=");
        fb_value(ctx, &b, form_data_entry_value(entries, i));
    }

    url = JS_NewStringLen(ctx, b.s ? b.s : "", b.s ? b.n : 0);
    endpoint_record(ctx, post ? "POST" : "GET", url, NULL, 0);
    JS_FreeValue(ctx, url);
    free(b.s);
}

/* WHERE THIS MACHINE RESTS. Two of §4.10.21.3's steps reach the page's code, and they are two STAGES because
   the page runs between them: step 11's `submit` event (whose verdict decides whether the request exists at
   all — a handler that calls preventDefault is doing its own fetch, and recording the form's request anyway
   would be a finding the page never makes), and step 16's entry-list construction, which fires `formdata`.
   The entry-list sub-sequence carries its OWN cursor, so this machine spends one stage on the whole of it. */
#define SUBMIT_STAGES(X) \
    X(SUBMIT_ENTER, "HTML §4.10.21.3 steps 1-10 (§4.10.21.4's submitter checks and the form's own guards, " \
                    "before step 11's event)") \
    X(SUBMIT_EVENT, "HTML §4.10.21.3 step 11 (fire an event named submit at form, cancelable), and step 13's " \
                    "verdict") \
    X(SUBMIT_ENTRIES, "HTML §4.10.21.3 steps 15-16 (pick an encoding, then construct the entry list with " \
                      "form, submitter and encoding), and the request this engine records from it")
enum { SUBMIT_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const SUBMIT_STEPS[] = { SUBMIT_STAGES(JS_STEP_STAGE_LABEL) NULL };

/* `arg`: WHICH member entered §4.10.21.3, which is the algorithm's own "submitted from submit() method". */
enum { SUBMIT_FROM_METHOD = 0, SUBMIT_FROM_REQUEST };

typedef struct JSSubmitState {
    JSStepHdr hdr;      /* FIRST — the driver writes the def and the operand bounds through it */
    uint8_t   fphase;   /* the fire request's own phase */
    JSValue   ev;       /* the event, minted once and held across the suspension (owned) */
    JSValue   cb[4];    /* the fire request buffer: [this, dispatch, target, event] */
    JSValue   submitter;             /* step 5's submitter — the form itself when none was given (owned) */
    FormEntryListRun entries;        /* §4.10.22.4's own cursor, held across its `formdata` dispatch */
} JSSubmitState;

static void js_submit_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSSubmitState *s = st;
    int k;
    v->val(ctx, &s->ev);
    v->val(ctx, &s->submitter);
    for (k = 0; k < 4; k++)
        v->val(ctx, &s->cb[k]);
    form_entry_list_visit(ctx, &s->entries, v);
}

static JSValue js_submit_fini(JSContext *ctx, void *st, bool take_result)
{
    JSSubmitState *s = st;
    int k;
    (void)take_result;
    JS_FreeValue(ctx, s->ev);
    s->ev = JS_UNDEFINED;
    JS_FreeValue(ctx, s->submitter);
    s->submitter = JS_UNDEFINED;
    for (k = 0; k < 4; k++) {
        JS_FreeValue(ctx, s->cb[k]);
        s->cb[k] = JS_UNDEFINED;
    }
    form_entry_list_release(ctx, &s->entries);
    return JS_UNDEFINED;   /* both members return undefined whatever the handlers did */
}

static int js_submit_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSSubmitState *s = st;
    bool not_canceled = true;
    int r;

    if (s->hdr.stage == SUBMIT_ENTER) {
        JSValueConst given = step_arg(&s->hdr, 0);

        s->ev = JS_UNDEFINED;
        s->submitter = JS_UNDEFINED;
        s->cb[0] = s->cb[1] = s->cb[2] = s->cb[3] = JS_UNDEFINED;
        form_entry_list_init(&s->entries);
        if (!form_elem_of(s->hdr.this_val)) {
            JS_FreeValue(ctx, cb_result);
            return JS_STEP_DONE;
        }
        /* §4.10.21.4's requestSubmit steps 1-2. `submit()` names no submitter at all, so it takes step 2's
           branch unconditionally: the submitter is the FORM, which is what makes step 5.1's "field is a button
           but it is not submitter" drop every button. */
        if (s->hdr.arg == SUBMIT_FROM_REQUEST && !JS_IsUndefined(given) && !JS_IsNull(given)) {
            if (!html_form_is_submit_button(ctx, given)) {
                JS_FreeValue(ctx, cb_result);
                JS_ThrowTypeError(ctx, "requestSubmit was given a submitter that is not a submit button");
                return JS_STEP_ABRUPT;
            }
            {
                JSValue owner = html_form_owner_of(ctx, given);
                bool mine = JS_VALUE_GET_PTR(owner) == JS_VALUE_GET_PTR(s->hdr.this_val);
                JS_FreeValue(ctx, owner);
                if (!mine) {
                    JS_FreeValue(ctx, cb_result);
                    JS_ThrowDOMException(ctx, "NotFoundError",
                                         "the submitter's form owner is not this form element");
                    return JS_STEP_ABRUPT;
                }
            }
            s->submitter = JS_DupValue(ctx, given);
        } else {
            s->submitter = JS_DupValue(ctx, s->hdr.this_val);
        }
        /* Step 5: `submit()` sets "submitted from submit() method", which is what skips the validation and the
           event entirely — so that entry point starts at the entry list. */
        s->hdr.stage = (s->hdr.arg == SUBMIT_FROM_METHOD) ? SUBMIT_ENTRIES : SUBMIT_EVENT;
    }
    if (s->hdr.stage == SUBMIT_EVENT) {
        /* Step 11: "fire an event named submit at form ... with the cancelable attribute initialized to true"
           — the ONE §2.9 dispatch, as a REQUEST, so the handlers run as ordinary preemptible page code and this
           resumes after every one of them has returned. */
        if (JS_IsUndefined(s->ev))
            s->ev = event_new(ctx, "submit", /*bubbles*/ true, /*cancelable*/ true);
        r = event_target_fire_run(ctx, &s->fphase, STEP_CB(s->cb), s->hdr.this_val, s->ev, cb_result,
                                  &not_canceled, out_cb, out_argc);
        if (r > 0) return r;
        if (r < 0) return JS_STEP_ABRUPT;
        cb_result = JS_UNDEFINED;
        if (!not_canceled) return JS_STEP_DONE;   /* step 13 */
        s->hdr.stage = SUBMIT_ENTRIES;
    }
    DCHECK(s->hdr.stage == SUBMIT_ENTRIES, "a form submission resumed into a stage §4.10.21.3 does not have");
    {
        JSValue list = JS_UNDEFINED;

        r = form_entry_list_run(ctx, &s->entries, s->hdr.this_val, s->submitter,
                                form_pick_encoding(form_elem_of(s->hdr.this_val)), cb_result, &list,
                                out_cb, out_argc);
        if (r > 0) return r;
        if (r < 0) return JS_STEP_ABRUPT;
        /* §4.10.21.3 step 17 asserts the list is not null, and it holds here for the reason the spec says: step
           2 already returned for a form whose constructing entry list is true, so nothing re-enters. */
        DCHECK(!JS_IsNull(list),
               "§4.10.21.3 step 17's assert failed — the entry list was null, which step 2 already excluded");
        form_record_request(ctx, form_elem_of(s->hdr.this_val), form_elem_of(s->submitter), list);
        JS_FreeValue(ctx, list);
    }
    return JS_STEP_DONE;
}

static const JSTrampStepDef js_submit_def = {
    sizeof(JSSubmitState), js_submit_step, js_submit_fini, SUBMIT_FROM_METHOD, .visit = js_submit_visit,
    .algorithm = "HTML §4.10.21.3 submit a form (from submit())", .steps = SUBMIT_STEPS
};

static const JSTrampStepDef js_reqsubmit_def = {
    sizeof(JSSubmitState), js_submit_step, js_submit_fini, SUBMIT_FROM_REQUEST, .visit = js_submit_visit,
    .algorithm = "HTML §4.10.21.3 submit a form (from requestSubmit)", .steps = SUBMIT_STEPS
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
 * A RESET IS TRIGGERED for a form-associated CUSTOM element at its insertion, its removal, its own `form`
 * write and §4.13.5 step 10. NOTHING triggers one for a BUILT-IN control, and that is not an oversight this
 * file can fix by adding a call: HTML's tree builder associates a parsed control through the "form element
 * pointer" as it builds, and this engine's Lexbor parse routes through none of DOM §4.2.3's insertion steps at
 * all — so for a parsed document there is no moment at which a reset could have run. An ABSENT slot therefore
 * means "no reset has run", which is a different fact from "the owner is null", and form_derive_owner answers
 * it by running steps 3-5 as they stand. The derivation is the SAME code the reset uses, so there is one
 * §4.10.18.3 here and not two, and a STORED owner always wins — which is what keeps
 * `<my-control form=nothing>`'s null answer null.
 * NAMED ABSENT: the tree builder's form element pointer, which is the only thing that can associate a
 * `<form><table><input>` the parser MOVED with the form it was written inside. */
static JSValue g_owner_key = JS_UNDEFINED;
static JSAtom  g_atom_owner = JS_ATOM_NULL;

static JSValue form_derive_owner(JSContext *ctx, JSValueConst wrap, const char *form_attr, size_t form_attr_len);

/* §4.10.18.3: "when a form-associated element is created, its form owner must be initialized to null" — so
   nothing writes a slot at creation, and the reset is what puts one there. OWNED. */
JSValue html_form_owner_of(JSContext *ctx, JSValueConst wrap)
{
    lxb_dom_node_t *n = node_of(wrap);
    size_t len = 0;
    const char *v;
    JSValue slot;

    DCHECK(g_atom_owner != JS_ATOM_NULL,
           "a form owner was asked for before html_form_declare minted its slot key");
    if (!JS_IsObject(wrap)) return JS_NULL;
    if (JS_GetOwnSlot(ctx, &slot, wrap, g_atom_owner) > 0) {
        if (JS_IsObject(slot)) return slot;
        JS_FreeValue(ctx, slot);
        return JS_NULL;
    }
    if (!n || n->type != LXB_DOM_NODE_TYPE_ELEMENT) return JS_NULL;
    v = attr_of(lxb_dom_interface_element(n), "form", &len);
    return form_derive_owner(ctx, wrap, v, len);
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

/* Steps 4-5 alone: WHICH form this element would be associated with, run over the tree as it stands. It is a
   function of its own because the RESET performs it and so does a first read of an element no reset has run
   for — one algorithm, two callers, never two spellings of it. OWNED. */
static JSValue form_derive_owner(JSContext *ctx, JSValueConst wrap, const char *form_attr, size_t form_attr_len)
{
    lxb_dom_node_t *n = node_of(wrap);

    if (form_attr) {
        /* Step 4 — a listed element with a `form` content attribute. Its ID lookup answering something that is
           not a form leaves the owner NULL, which is what makes step 5 an "otherwise" on step 4's CONDITION
           rather than on its result. */
        if (node_is_connected(n)) {
            lxb_dom_node_t *hit = form_first_by_id(node_root(n), form_attr, form_attr_len);
            if (hit && tag_is(hit, "form")) return node_wrap(ctx, hit);
        }
        return JS_NULL;
    }
    {
        lxb_dom_node_t *anc = form_nearest_ancestor(n);   /* step 5 */
        return anc ? node_wrap(ctx, anc) : JS_NULL;
    }
}

JSValue html_form_reset_owner_with_attr(JSContext *ctx, JSValueConst wrap, const char *form_attr,
                                        size_t form_attr_len, bool *pchanged)
{
    lxb_dom_node_t *n = node_of(wrap);
    JSValue was, now;

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
    now = form_derive_owner(ctx, wrap, form_attr, form_attr_len);
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

/* ---- what the entry list reads off a control ---------------------------------------------------------------
 * §4.10.22.4 walks controls it never received as a receiver, so the two pieces of state a control's entry is
 * made of are reachable by ELEMENT rather than only through the IDL accessor installed on its prototype. The
 * accessor's `magic` picks the DEFAULT that applies with no assigned state, and the tag is what decides it. */
JSValue html_form_control_value(JSContext *ctx, JSValueConst wrap)
{
    lxb_dom_node_t *n = node_of(wrap);

    return js_ctrl_get_value(ctx, wrap,
                             tag_is(n, "textarea") ? CTRL_TEXTAREA : tag_is(n, "option") ? CTRL_OPTION
                                                                                         : CTRL_INPUT);
}

bool html_form_control_checked(JSContext *ctx, JSValueConst wrap)
{
    JSValue v = js_ctrl_get_checked(ctx, wrap, 0);
    bool b = JS_ToBool(ctx, v);

    JS_FreeValue(ctx, v);
    return b;
}

const char *html_form_control_name(JSValueConst wrap, size_t *plen)
{
    lxb_dom_element_t *el = form_elem_of(wrap);

    *plen = 0;
    return el ? attr_of(el, "name", plen) : NULL;
}

JSValue html_form_checkbox_value(JSContext *ctx, JSValueConst wrap)
{
    lxb_dom_element_t *el = form_elem_of(wrap);
    size_t len = 0;
    const char *a;
    int i;

    if (!el) return JS_NewStringLen(ctx, "on", 2);
    /* The state the page assigned, first — the value slot is where an assignment lands and it is what carries a
       CONCOLIC, so `box.value = location.hash` submits as the source rather than as the markup's default. */
    i = attr_shadow_find(el, ATTR_SLOT_PROPERTY, NULL, "value");
    if (i >= 0) return JS_DupValue(ctx, attr_shadow_opaque(i));
    a = attr_of(el, "value", &len);
    return a ? JS_NewStringLen(ctx, a, len) : JS_NewStringLen(ctx, "on", 2);
}

FormFieldKind html_form_field_kind(JSContext *ctx, JSValueConst wrap)
{
    lxb_dom_node_t *n = node_of(wrap);
    size_t tlen = 0, nlen = 0;
    const char *t;

    if (!n || n->type != LXB_DOM_NODE_TYPE_ELEMENT) return FORM_FIELD_OTHER;
    if (tag_is(n, "select")) return FORM_FIELD_SELECT;
    if (!tag_is(n, "input")) {
        /* A `textarea` and a `button` both take step 5.11's "otherwise". A FACE is asked LAST here and FIRST in
           the chain, which costs nothing: nothing else can answer true for it. */
        return custom_elements_is_form_associated(ctx, wrap) ? FORM_FIELD_FACE : FORM_FIELD_OTHER;
    }
    t = attr_of(lxb_dom_interface_element(n), "type", &tlen);
    if (ascii_ci_is(t, tlen, "image")) return FORM_FIELD_IMAGE_BUTTON;
    if (ascii_ci_is(t, tlen, "checkbox") || ascii_ci_is(t, tlen, "radio")) return FORM_FIELD_CHECKBOX;
    if (ascii_ci_is(t, tlen, "file")) return FORM_FIELD_FILE;
    if (ascii_ci_is(t, tlen, "hidden")) {
        const char *name = attr_of(lxb_dom_interface_element(n), "name", &nlen);
        if (ascii_ci_is(name, nlen, "_charset_")) return FORM_FIELD_CHARSET;
    }
    return FORM_FIELD_OTHER;
}

bool html_form_has_datalist_ancestor(JSValueConst wrap)
{
    lxb_dom_node_t *n = node_of(wrap), *a;

    if (!n) return false;
    for (a = n->parent; a; a = a->parent)
        if (tag_is(a, "datalist")) return true;
    return false;
}

bool html_form_needs_dirname_entry(JSValueConst wrap)
{
    lxb_dom_node_t *n = node_of(wrap);
    size_t dlen = 0, tlen = 0;
    const char *t;

    if (!n || n->type != LXB_DOM_NODE_TYPE_ELEMENT) return false;
    if (!attr_of(lxb_dom_interface_element(n), "dirname", &dlen) || !dlen) return false;
    if (tag_is(n, "textarea")) return true;
    if (!tag_is(n, "input")) return false;
    t = attr_of(lxb_dom_interface_element(n), "type", &tlen);
    /* §3.2.6's list. The TEXT state is the missing-value AND invalid-value default, so an absent or unknown
       `type` is in it — which is why an unrecognised keyword answers true here rather than false. */
    if (!t || !tlen) return true;
    return ascii_ci_is(t, tlen, "hidden") || ascii_ci_is(t, tlen, "text") || ascii_ci_is(t, tlen, "search") ||
           ascii_ci_is(t, tlen, "tel") || ascii_ci_is(t, tlen, "url") || ascii_ci_is(t, tlen, "email") ||
           ascii_ci_is(t, tlen, "password") || ascii_ci_is(t, tlen, "submit") ||
           ascii_ci_is(t, tlen, "reset") || ascii_ci_is(t, tlen, "button") ||
           !(ascii_ci_is(t, tlen, "checkbox") || ascii_ci_is(t, tlen, "radio") ||
             ascii_ci_is(t, tlen, "file") || ascii_ci_is(t, tlen, "image") ||
             ascii_ci_is(t, tlen, "date") || ascii_ci_is(t, tlen, "month") ||
             ascii_ci_is(t, tlen, "week") || ascii_ci_is(t, tlen, "time") ||
             ascii_ci_is(t, tlen, "datetime-local") || ascii_ci_is(t, tlen, "number") ||
             ascii_ci_is(t, tlen, "range") || ascii_ci_is(t, tlen, "color"));
}

/* ---- §4.10.7's LIST OF OPTIONS, and §4.10.10's SELECTEDNESS -------------------------------------------------
 *
 * SELECTEDNESS IS THE `selected` CONTENT ATTRIBUTE HERE, and that is not an approximation of the stored state —
 * it IS the stored state given the members that exist. §4.10.10 decouples the two through an option's DIRTINESS,
 * which is set by exactly four things: `option.selected =`, `select.value =`, `select.selectedIndex =`, and the
 * user's "pick an option". This engine has none of them (§4.10.10's `defaultSelected` reflects the attribute and
 * changes no state), so dirtiness can never be true and §4.10.10's own rule — "whenever an option element's
 * selected attribute is added, IF ITS DIRTINESS IS FALSE, its selectedness must be set to true" — makes the two
 * the same boolean. The day `option.selected` lands it becomes a per-element slot beside the value state, and
 * this reads that instead. */

/* An option's DISABLEDNESS, §4.10.10's own steps — its `disabled` attribute, or the first ancestor optgroup's,
   stopping at a select/hr/datalist/option. NOT the form-control rule: an option is not a form control. */
static bool option_is_disabled(lxb_dom_node_t *opt)
{
    lxb_dom_node_t *a;

    if (has_attr(lxb_dom_interface_element(opt), "disabled")) return true;
    for (a = opt->parent; a; a = a->parent) {
        if (tag_is(a, "select") || tag_is(a, "hr") || tag_is(a, "datalist") || tag_is(a, "option")) return false;
        if (tag_is(a, "optgroup")) return has_attr(lxb_dom_interface_element(a), "disabled");
    }
    return false;
}

/* "node is an optgroup element and node has an ancestor optgroup in between itself and select". */
static bool optgroup_is_nested(lxb_dom_node_t *node, lxb_dom_node_t *select)
{
    lxb_dom_node_t *a;

    if (!tag_is(node, "optgroup")) return false;
    for (a = node->parent; a && a != select; a = a->parent)
        if (tag_is(a, "optgroup")) return true;
    return false;
}

/* §4.10.7 "to get the list of options given a select element select", verbatim. */
static JSValue select_option_list(JSContext *ctx, lxb_dom_node_t *select)
{
    JSValue arr = JS_NewArray(ctx);
    lxb_dom_node_t *node = select ? select->first_child : NULL;
    uint32_t k = 0;

    CHECK(!JS_IsException(arr), "forms: OOM building a select's list of options");
    while (node) {
        bool skip = tag_is(node, "select") || tag_is(node, "hr") || tag_is(node, "option") ||
                    tag_is(node, "datalist") || optgroup_is_nested(node, select);

        if (tag_is(node, "option")) JS_SetPropertyUint32(ctx, arr, k++, node_wrap(ctx, node));
        if (!skip && node->first_child) { node = node->first_child; continue; }
        while (node != select && !node->next) node = node->parent;
        node = (node == select) ? NULL : node->next;
    }
    return arr;
}

/* §4.10.7's DISPLAY SIZE: the `size` attribute parsed as a non-negative integer, or 4 when `multiple` is
   present and 1 otherwise. */
static long select_display_size(lxb_dom_element_t *sel)
{
    size_t len = 0, i;
    const char *v = attr_of(sel, "size", &len);
    long r = 0;

    if (!v || !len) return has_attr(sel, "multiple") ? 4 : 1;
    for (i = 0; i < len; i++) {
        if (v[i] < '0' || v[i] > '9') return has_attr(sel, "multiple") ? 4 : 1;
        r = r * 10 + (v[i] - '0');
        if (r > 1000000) return r;   /* the value only ever decides "is it 1", so magnitude past this is moot */
    }
    return r;
}

static uint32_t js_array_len(JSContext *ctx, JSValueConst arr)
{
    JSValue lv = JS_GetPropertyStr(ctx, arr, "length");
    uint32_t n = 0;

    JS_ToUint32(ctx, &n, lv);
    JS_FreeValue(ctx, lv);
    return n;
}

JSValue html_form_selected_options(JSContext *ctx, JSValueConst select)
{
    lxb_dom_node_t *n = node_of(select);
    lxb_dom_element_t *sel = n ? lxb_dom_interface_element(n) : NULL;
    JSValue list, out = JS_NewArray(ctx);
    uint32_t len, i, k = 0;
    bool multiple;
    int selected_n = 0, last_selected = -1, first_enabled = -1;

    CHECK(!JS_IsException(out), "forms: OOM building a select's selected options");
    if (!sel) return out;
    multiple = has_attr(sel, "multiple");
    list = select_option_list(ctx, n);
    len = js_array_len(ctx, list);
    for (i = 0; i < len; i++) {
        JSValue opt = JS_GetPropertyUint32(ctx, list, i);
        lxb_dom_node_t *on = node_of(opt);

        if (on && has_attr(lxb_dom_interface_element(on), "selected")) {
            selected_n++;
            last_selected = (int)i;
        }
        if (first_enabled < 0 && on && !option_is_disabled(on)) first_enabled = (int)i;
        JS_FreeValue(ctx, opt);
    }
    /* §4.10.7's SELECTEDNESS SETTING ALGORITHM step 1: a single-select showing one row with nothing selected
       selects its first non-disabled option. This is why `<select name=x><option>a<option>b</select>` submits
       `x=a` with no `selected` attribute anywhere. */
    if (!multiple && select_display_size(sel) == 1 && selected_n == 0 && first_enabled >= 0) {
        selected_n = 1;
        last_selected = first_enabled;
    }
    for (i = 0; i < len; i++) {
        JSValue opt = JS_GetPropertyUint32(ctx, list, i);
        lxb_dom_node_t *on = node_of(opt);
        bool sel_i = on && has_attr(lxb_dom_interface_element(on), "selected");

        if (!multiple) {
            /* Step 2: with two or more selected, all but the LAST are cleared — and step 1's pick is the only
               one there is when the attribute named none. */
            sel_i = (int)i == last_selected;
        }
        if (sel_i && on && !option_is_disabled(on)) JS_SetPropertyUint32(ctx, out, k++, JS_DupValue(ctx, opt));
        JS_FreeValue(ctx, opt);
    }
    JS_FreeValue(ctx, list);
    return out;
}

/* §3.1.1 document.forms — every form in the document, in tree order. */

/* Declared once per AGENT, installed per realm — see hyperlink.c for why the split exists at all. */
static int g_id_submit = -1, g_id_reqsubmit = -1, g_id_val_input = -1, g_id_val_textarea = -1,
           g_id_val_option = -1, g_id_checked = -1;

void html_form_declare(JSContext *ctx)
{
    /* §4.10.18.3's form-owner slot key — a Symbol, so the owner is not a string property of this engine's
       invention sitting on every form-associated element where a page can see it. Minted once per AGENT,
       beside the member declarations, because the key identifies the same slot in every realm. */
    DCHECK(JS_IsUndefined(g_owner_key), "html_form_declare ran twice — one instance is one agent");
    g_owner_key = JS_NewSymbol(ctx, "formOwner", false);
    CHECK(!JS_IsException(g_owner_key), "the form-owner slot key allocation failed");
    g_atom_owner = JS_ValueToAtom(ctx, g_owner_key);
    CHECK(g_atom_owner != JS_ATOM_NULL, "the form-owner slot key could not be interned");
    /* §4.10.22.1's FormDataEvent, which §4.10.22.4 step 7 fires, and §4.10.22.4's own per-form flag key —
       declared from here because both are §4.10's and this file is §4.10's declaration point. */
    form_data_event_init(ctx);
    form_entry_list_declare(ctx);
    /* BOTH submission members register their own step definition rather than declaring arguments to the args
       machine, so both install through the installer for that kind — see idl_install_step_method. `submit()`
       is one because §4.10.22.4's `formdata` event is the page's code and it runs on that path too. */
    g_id_submit = JS_RegisterStepDef(JS_GetRuntime(ctx), &js_submit_def);
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
    idl_install_step_method(ctx, form_proto, "submit", 0, g_id_submit);
    idl_install_step_method(ctx, form_proto, "requestSubmit", 0, g_id_reqsubmit);
    idl_install_accessor(ctx, input_proto, "value", js_ctrl_get_value, CTRL_INPUT, g_id_val_input);
    idl_install_accessor(ctx, textarea_proto, "value", js_ctrl_get_value, CTRL_TEXTAREA, g_id_val_textarea);
    idl_install_accessor(ctx, option_proto, "value", js_ctrl_get_value, CTRL_OPTION, g_id_val_option);
    idl_install_accessor(ctx, input_proto, "checked", js_ctrl_get_checked, 0, g_id_checked);
}

void html_form_free(JSContext *ctx)
{
    form_data_event_free(ctx);
    form_entry_list_free(ctx);
    /* The form-owner slot key is the AGENT's, so it is released with the agent — a Symbol nobody frees is a
       live GC object the runtime's own walk counts as a leak. */
    JS_FreeAtom(ctx, g_atom_owner);
    g_atom_owner = JS_ATOM_NULL;
    JS_FreeValue(ctx, g_owner_key);
    g_owner_key = JS_UNDEFINED;
}
