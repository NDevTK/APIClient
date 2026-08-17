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
#include "core/dom/document.h"
#include "core/frame/sandboxing.h"
#include "core/dom/node.h"
#include "core/encoding/encoding.h"
#include "core/events/event.h"
#include "core/events/event_target.h"
#include "core/frame/navigable.h"
#include "core/html/constraint_validation.h"
#include "core/html/custom_elements.h"
#include "core/html/form_data.h"
#include "core/html/form_data_event.h"
#include "core/html/form_entry_list.h"
#include "core/html/html_dialog.h"
#include "core/html/html_form.h"
#include "core/html/input_picker.h"
#include "core/html/input_value.h"
#include "core/html/submit_event.h"
#include "core/html/user_activation.h"
#include "core/url/url.h"
#include "solver/attr_shadow.h"
#include "solver/concolic.h"
#include "solver/dom_cow.h"
#include "solver/endpoint.h"
#include "solver/solve.h"

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

/* §4.10.5.1's STATES OF THE `type` ATTRIBUTE, resolved once against the keyword table §4.10.5.1 is a section
   per. The keywords are ASCII case-insensitive like every enumerated attribute's (§2.3.2), and a missing or
   unrecognised one is the TEXT state — both defaults §4.10.5.1.2 declares. */
HtmlInputState html_form_input_state(const lxb_dom_node_t *n)
{
    static const struct { const char *keyword; HtmlInputState state; } TYPES[] = {
        { "hidden", INPUT_STATE_HIDDEN },   { "text", INPUT_STATE_TEXT },
        { "search", INPUT_STATE_SEARCH },   { "tel", INPUT_STATE_TEL },
        { "url", INPUT_STATE_URL },         { "email", INPUT_STATE_EMAIL },
        { "password", INPUT_STATE_PASSWORD }, { "date", INPUT_STATE_DATE },
        { "month", INPUT_STATE_MONTH },     { "week", INPUT_STATE_WEEK },
        { "time", INPUT_STATE_TIME },       { "datetime-local", INPUT_STATE_DATETIME_LOCAL },
        { "number", INPUT_STATE_NUMBER },   { "range", INPUT_STATE_RANGE },
        { "color", INPUT_STATE_COLOR },     { "checkbox", INPUT_STATE_CHECKBOX },
        { "radio", INPUT_STATE_RADIO },     { "file", INPUT_STATE_FILE },
        { "submit", INPUT_STATE_SUBMIT },   { "image", INPUT_STATE_IMAGE },
        { "reset", INPUT_STATE_RESET },     { "button", INPUT_STATE_BUTTON },
    };
    size_t tlen = 0, i;
    const char *t;

    if (!tag_is((lxb_dom_node_t *)n, "input")) return INPUT_STATE_NONE;
    t = attr_of(lxb_dom_interface_element((lxb_dom_node_t *)n), "type", &tlen);
    for (i = 0; i < sizeof TYPES / sizeof TYPES[0]; i++)
        if (ascii_ci_is(t, tlen, TYPES[i].keyword)) return TYPES[i].state;
    return INPUT_STATE_TEXT;
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
    HtmlInputState st;

    (void)ctx;
    if (!n || n->type != LXB_DOM_NODE_TYPE_ELEMENT) return false;
    if (tag_is(n, "button")) return true;   /* §4.10.6: "The element is a button." */
    st = html_form_input_state(n);
    /* §4.10.5.1.18/.19/.20/.21 each end "The element is a button"; no other input state does. */
    return st == INPUT_STATE_SUBMIT || st == INPUT_STATE_IMAGE ||
           st == INPUT_STATE_RESET  || st == INPUT_STATE_BUTTON;
}

bool html_form_is_submit_button(JSContext *ctx, JSValueConst wrap)
{
    lxb_dom_node_t *n = node_of(wrap);
    size_t tlen = 0;
    const char *t;

    (void)ctx;
    if (!n || n->type != LXB_DOM_NODE_TYPE_ELEMENT) return false;
    if (tag_is(n, "input")) {
        HtmlInputState st = html_form_input_state(n);
        /* §4.10.5.1.18 and §4.10.5.1.19: "specifically a submit button". */
        return st == INPUT_STATE_SUBMIT || st == INPUT_STATE_IMAGE;
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

/* The length of a JS Array this file built — the control lists every walk here is over. */
static uint32_t js_array_len(JSContext *ctx, JSValueConst arr)
{
    JSValue lv = JS_GetPropertyStr(ctx, arr, "length");
    uint32_t n = 0;

    JS_ToUint32(ctx, &n, lv);
    JS_FreeValue(ctx, lv);
    return n;
}

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

/* ---- a control's VALUE state -----------------------------------------------------------------------------
 *
 * §4.10.5's `input` is NOT one of these. Its value is §4.10.5.1's — a piece of state whose CONTENT is decided
 * by the `type` attribute's state through the value sanitization algorithm and whose relationship to the
 * `value` content attribute is decided by the dirty value flag — and it lives in input_value.c because it is a
 * different problem from "the element's assigned state, or its markup's default". What is left here is the two
 * controls whose section states exactly that (§4.10.11's textarea and §4.10.10's option) and the `button`,
 * whose value IS its `value` content attribute. */
/* magic names whose DEFAULT applies when the page has assigned no state. */
enum { CTRL_ATTRIBUTE = 0, CTRL_TEXTAREA, CTRL_OPTION };

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

/* §4.10.5.4's `value` on HTMLInputElement, which input_value.c owns in full — the four modes and §4.10.5.1's
   value sanitization algorithm. Here only Web IDL §3.7.5's BRAND CHECK, because the member is
   HTMLInputElement's: `descriptor.get.call(textarea)` is a TypeError and not the empty string a shared accessor
   would answer, and the two controls that share this file's other accessor are not `input` elements. */
static JSValue js_input_brand(JSContext *ctx, JSValueConst this_val)
{
    if (html_form_input_state(node_of(this_val)) != INPUT_STATE_NONE) return JS_UNDEFINED;
    return JS_ThrowTypeError(ctx, "HTMLInputElement's `value` was accessed on something that is not an "
                                  "HTMLInputElement");
}

static JSValue js_input_get_value(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSValue bad = js_input_brand(ctx, this_val);

    (void)magic;
    if (JS_IsException(bad)) return bad;
    return input_value_get(ctx, this_val);
}

static JSValue js_input_set_value(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    JSValue bad = js_input_brand(ctx, this_val);

    (void)magic;
    if (JS_IsException(bad)) return bad;
    return input_value_set(ctx, this_val, val);
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

/* COULD this element be form-associated at all — asked of the TAG, with no wrapper. It exists because the two
   real questions below need one (a form-associated custom element is only knowable through its definition, and
   the form owner is a per-flow slot on the wrapper), and minting a wrapper for every element of the document
   to ask them would materialise the whole tree on the first `form.elements` read. §4.10.2's categories are all
   built-in tags plus custom elements, and a custom element is exactly an element whose local name is a valid
   custom element name — so this filter drops nothing the categories contain. */
static bool form_maybe_associated(lxb_dom_node_t *n)
{
    size_t len = 0;
    const lxb_char_t *tag;

    if (tag_is(n, "button") || tag_is(n, "fieldset") || tag_is(n, "input") || tag_is(n, "object") ||
        tag_is(n, "output") || tag_is(n, "select") || tag_is(n, "textarea"))
        return true;
    tag = lxb_dom_element_local_name(lxb_dom_interface_element(n), &len);
    return tag != NULL && len != 0 && custom_elements_name_is_valid((const char *)tag, len);
}

static JSValue form_members(JSContext *ctx, JSValueConst form_wrap, FormMemberPred want)
{
    lxb_dom_node_t *form = node_of(form_wrap), *root, *n;
    JSValue arr = JS_NewArray(ctx);
    uint32_t k = 0;

    CHECK(!JS_IsException(arr), "forms: OOM building a form's control list");
    if (!form) return arr;
    root = node_root(form);
    for (n = root; n; ) {
        if (n->type == LXB_DOM_NODE_TYPE_ELEMENT && form_maybe_associated(n)) {
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

    if (html_form_input_state(n) == INPUT_STATE_IMAGE) return false;
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

/* ---- §4.10.22.3 "submit a form", and the two members that enter it ------------------------------------------
 *
 * ONE MACHINE, TWO ENTRY POINTS, AND THE DIFFERENCE BETWEEN THEM IS ONE BOOLEAN. §4.10.3's `submit()` steps are
 * "submit this from this, WITH SUBMITTED FROM SUBMIT() METHOD SET TO TRUE"; `requestSubmit(submitter)` runs its
 * own two checks and then submits with that boolean false. The boolean is exactly what step 5 tests, and step 5
 * is the whole of the difference — the user-validity walk, the interactive validation and the `submit` EVENT
 * all live under it, which is why `submit()` submits a form no handler can cancel. So the two members are two
 * DEFINITIONS over ONE step function differing in `arg`, never two algorithms.
 *
 * IT IS A MACHINE BECAUSE IT RUNS THE PAGE'S CODE, at two steps and in two different ways: step 5.6 fires a
 * `submit` event at the form, and step 7's entry-list construction fires `formdata`. Both are arbitrary page
 * code — a loop, an `await`, a DOM mutation — so each is a STAGE BOUNDARY: the flow parks there, siblings run,
 * and it resumes with the canceled flag it left behind. A C body hosting either would be the drive-to-completion
 * this engine aborts on, and the `submit` event's verdict is not a detail to skip: a handler that calls
 * preventDefault is doing its own fetch, and recording the form's request anyway is a finding the page never
 * makes.
 *
 * THE REQUEST IS DERIVED AND NEVER SENT. A submission mutates server state by construction, and this engine's
 * rule for such a request is that its values come from the forced-exec path rather than from firing it. So an
 * `http`/`https` action ends at endpoint_record, and steps 18-25 (target, noopener, target navigable, history
 * handling) are the operands of a NAVIGATION that consequently does not happen for it.
 *
 * EVERY OTHER SCHEME NAVIGATES, AND NONE OF THEM IS AN ENDPOINT — which is why step 26's table is written out
 * below as a table rather than left as a chain of scheme comparisons. Recording an endpoint for a `mailto:` or
 * a `data:` submission would put a request the form never makes onto the @H surface, which is worse than a
 * crash; and a `javascript:` action is not a request at all but a code-execution SINK, since navigating to one
 * EXECUTES it in this document. */

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

/* ---- §4.10.19.6's ATTRIBUTES FOR FORM SUBMISSION ------------------------------------------------------------
 *
 * "Attributes for form submission can be specified both on form elements and on submit buttons ... When omitted,
 * they default to the values given on the corresponding attributes on the form element." So every one of the
 * five is ONE lookup: the SUBMITTER's `form*` attribute when the submitter is a submit button and has it, and
 * the form's own otherwise. Asked of the submit-button predicate rather than of "is the submitter the form",
 * because those are two different questions the moment a submitter is something else. */
/* `pfrom`/`pname` report WHICH element and WHICH attribute the answer came from, for the one caller that needs
   more than the bytes: the action is a URL the page's own code may have written, so the solver has to be able
   to ask that attribute for its TAINT — and asking the attribute means knowing which of the two this lookup
   chose. Re-deciding it at that caller would be a second copy of §4.10.19.6's order, and the two copies would
   stop agreeing the first time either moved. Both may be NULL.
   THE FALL-THROUGH IS DECIDED BY PRESENCE, not by whether the attribute has a value buffer. "When omitted, they
   default to the values given on the corresponding attributes on the form element" — and `<button formaction="">`
   is not omitted. It matters twice over: the HTML parser stores a valueless attribute with no value buffer at
   all, so reading the VALUE pointer called an EMPTY one absent too, and every one of these five attributes then
   answered with the form's when the submitter had explicitly overridden it with the empty string (`formmethod=""`
   is an INVALID value, whose default is GET, and it was silently reading the form's `method=post`). NULL here
   means the attribute is on neither element; the empty string means it is present and empty. */
static const char *submitter_attr_from(JSContext *ctx, JSValueConst submitter, lxb_dom_element_t *form,
                                       const char *own, const char *fallback, size_t *plen,
                                       lxb_dom_element_t **pfrom, const char **pname)
{
    lxb_dom_element_t *el = NULL;
    const char *v;

    *plen = 0;
    if (pfrom) *pfrom = form;
    if (pname) *pname = fallback;
    if (html_form_is_submit_button(ctx, submitter)) el = form_elem_of(submitter);
    if (el && has_attr(el, own)) {
        v = attr_of(el, own, plen);
        if (pfrom) *pfrom = el;
        if (pname) *pname = own;
        return v ? v : "";
    }
    *plen = 0;
    if (form && has_attr(form, fallback)) {
        v = attr_of(form, fallback, plen);
        return v ? v : "";
    }
    return NULL;
}

static const char *submitter_attr(JSContext *ctx, JSValueConst submitter, lxb_dom_element_t *form,
                                  const char *own, const char *fallback, size_t *plen)
{
    return submitter_attr_from(ctx, submitter, form, own, fallback, plen, NULL, NULL);
}

/* §4.10.19.6's `method`/`formmethod` enumerated attribute: `get`, `post` and `dialog`, with the GET state as
   both the missing value default and the invalid value default. `formmethod` has NO missing value default, which
   is what makes an absent one fall through to the form's attribute rather than straight to GET — the lookup
   above is that fall-through. */
enum { FORM_METHOD_GET = 0, FORM_METHOD_POST, FORM_METHOD_DIALOG };

static int form_method_state(JSContext *ctx, JSValueConst submitter, lxb_dom_element_t *form)
{
    size_t len = 0;
    const char *v = submitter_attr(ctx, submitter, form, "formmethod", "method", &len);

    if (ascii_ci_is(v, len, "post")) return FORM_METHOD_POST;
    if (ascii_ci_is(v, len, "dialog")) return FORM_METHOD_DIALOG;
    return FORM_METHOD_GET;
}

/* §4.10.19.6's `enctype`/`formenctype`, the same shape: three keywords, with the urlencoded state as both
   defaults. It is what step 26's "Submit as entity body" switches on, and the one thing about a POST an @H
   record can carry without a body: its CONTENT TYPE. */
enum { FORM_ENCTYPE_URLENCODED = 0, FORM_ENCTYPE_MULTIPART, FORM_ENCTYPE_TEXT };

static int form_enctype_state(JSContext *ctx, JSValueConst submitter, lxb_dom_element_t *form)
{
    size_t len = 0;
    const char *v = submitter_attr(ctx, submitter, form, "formenctype", "enctype", &len);

    if (ascii_ci_is(v, len, "multipart/form-data")) return FORM_ENCTYPE_MULTIPART;
    if (ascii_ci_is(v, len, "text/plain")) return FORM_ENCTYPE_TEXT;
    return FORM_ENCTYPE_URLENCODED;
}

static const char *form_enctype_mime(int state)
{
    /* §4.10.22.3's "Submit as entity body" names the mimeType per enctype. The multipart one is stated as the
       concatenation of `multipart/form-data; boundary=` and the boundary §4.10.22.8's encoder GENERATES, and
       this surface records a request rather than encoding a body — so what is recorded is the type the form's
       own attribute names, which is the whole of what an @H record can say about it. */
    return state == FORM_ENCTYPE_MULTIPART ? "multipart/form-data"
         : state == FORM_ENCTYPE_TEXT      ? "text/plain"
                                           : "application/x-www-form-urlencoded";
}

/* §4.10.19.6: "The NO-VALIDATE STATE of an element is true if the element is a submit button and the element's
   `formnovalidate` attribute is present, or if the element's form owner's `novalidate` attribute is present,
   and false otherwise." It is a boolean-attribute PRESENCE question, so it is not the lookup above (which
   answers with a value and would read an empty `formnovalidate=""` as absent). */
static bool form_no_validate(JSContext *ctx, JSValueConst submitter, lxb_dom_element_t *form)
{
    lxb_dom_element_t *el = form_elem_of(submitter);

    if (el && html_form_is_submit_button(ctx, submitter) && has_attr(el, "formnovalidate")) return true;
    return has_attr(form, "novalidate");
}

/* §4.6.3's "GET AN ELEMENT'S TARGET", which step 20 runs given the submitter's form owner and step 19's
   `formtarget`: the given target when there is one, otherwise the element's own `target` attribute, otherwise
   the value of the `target` attribute of the FIRST `base` element in the element's node document that has one,
   otherwise the empty string. The `base` leg is not decoration — `<base target=_blank>` retargets every form
   and link in the document at once, and an engine that skipped it would decide the rows below in the wrong
   navigable while looking like it had asked. */
static const char *form_base_target(lxb_dom_node_t *form_node, size_t *plen)
{
    lxb_dom_node_t *root, *n;

    *plen = 0;
    if (!form_node || !form_node->owner_document) return NULL;
    root = lxb_dom_interface_node(form_node->owner_document);
    for (n = node_next_in(root, root); n; n = node_next_in(n, root)) {
        /* "the FIRST such base element" — the first one that HAS the attribute, which is a presence question
           like the one submitter_attr_from asks, not a question about its value being non-empty. */
        if (tag_is(n, "base") && has_attr(lxb_dom_interface_element(n), "target")) {
            const char *v = attr_of(lxb_dom_interface_element(n), "target", plen);
            return v ? v : "";
        }
    }
    return NULL;
}

static const char *form_element_target(JSContext *ctx, JSValueConst submitter, lxb_dom_element_t *form,
                                       lxb_dom_node_t *form_node, size_t *plen)
{
    const char *v = submitter_attr(ctx, submitter, form, "formtarget", "target", plen);

    if (v) return v;   /* NULL only when NEITHER element carries the attribute — see submitter_attr_from */
    return form_base_target(form_node, plen);
}

/* §7.1's RULES FOR CHOOSING A NAVIGABLE, for the only answer this engine can act in: `_self` and the empty
   string both choose the navigable the form is already in, which is this document. Every other target — a
   keyword or a name — chooses one this engine has not computed, because steps 18-21 exist for a navigation and
   the recording rows never needed a navigable. */
static bool form_target_is_self(const char *t, size_t len)
{
    return !t || len == 0 || ascii_ci_is(t, len, "_self");
}

/* §4.6.5's "CANNOT NAVIGATE", which steps 1, 5.9 and 9 each ask: "an element cannot navigate if its node
   document is not FULLY ACTIVE, or it is not an `a` element and is not CONNECTED". A form is not an `a`, so
   both clauses apply to it — and the reason the spec asks three times is that the page's code runs in between
   and can detach the form or its document, which is exactly what this machine's stages park across. */
static bool form_cannot_navigate(JSContext *ctx, JSValueConst form)
{
    return !document_fully_active(ctx) || !node_is_connected(node_of(form));
}

/* §4.10.22.3 step 4's condition, "form document's active sandboxing flag set has its SANDBOXED FORMS BROWSING
   CONTEXT FLAG set" — the flag `<iframe sandbox>` sets and `allow-forms` clears, and the step that makes a
   sandboxed form submission return with the form never submitted. */
static bool form_document_sandboxes_forms(JSContext *ctx)
{
    return (document_active_sandbox_flags(ctx) & SANDBOX_FORMS) != 0;
}

/* §4.10.21's CONSTRAINT VALIDATION belongs to constraint_validation.c — §4.10.21.1's ten validity states are a
   question about an ELEMENT and this file's only interest in them is step 5.4's verdict. What used to stand here
   was the shape of that verdict with no states behind it: a candidacy test, and a crash for every control that
   carried a constraint attribute. */

/* ---- §4.10.22.3's own state: the FIRING SUBMISSION EVENTS boolean --------------------------------------------
 *
 * "Each form element has a firing submission events boolean, initially false." It is what steps 5.1 and 5.2 make
 * step 5 non-reentrant with: a `submit` handler that calls `requestSubmit()` on the same form gets step 5.1's
 * return rather than a second event. Held as an own slot on the form's wrapper under a Symbol this file minted
 * and never published — so it is per-flow for free (a slot written as a property write is captured by the COW
 * delta), which the flag has to be: two forked arms each submitting the same form each have their own. */
static JSValue g_firing_key = JS_UNDEFINED;
static JSAtom  g_atom_firing = JS_ATOM_NULL;

static bool form_firing_events(JSContext *ctx, JSValueConst form)
{
    JSValue v;
    bool b;

    DCHECK(g_atom_firing != JS_ATOM_NULL,
           "§4.10.22.3 step 5.1 asked before html_form_declare minted the firing-submission-events key");
    if (!JS_IsObject(form)) return false;
    if (JS_GetOwnSlot(ctx, &v, form, g_atom_firing) <= 0) return false;
    b = JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);
    return b;
}

/* CONFIGURABLE AND WRITABLE for the reason the form-owner slot is: it is written twice per submission, and a
   slot defined with no flags makes the second write a silent no-op. */
static void form_firing_set(JSContext *ctx, JSValueConst form, bool on)
{
    if (!JS_IsObject(form)) return;
    JS_DefinePropertyValue(ctx, (JSValue)form, g_atom_firing, JS_NewBool(ctx, on),
                           JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE);
}

/* §4.10.22.6 "converting an entry list to a list of name-value pairs" followed by §4.10.22.7's serializer, as
   far as an @H record needs them: a CONCOLIC value contributes its shape rather than being percent-encoded into
   an invention, which is the same rule endpoint_record already applies to a concolic URL. */
static void fb_pairs(JSContext *ctx, FormBuf *b, JSValueConst entries)
{
    int i, n = form_data_entry_count(entries);

    for (i = 0; i < n; i++) {
        size_t nlen = 0;
        const char *name = form_data_entry_name(entries, i, &nlen);

        if (i) fb_str(b, "&");
        fb_add(b, name, nlen);
        fb_str(b, "=");
        fb_value(ctx, b, form_data_entry_value(entries, i));
    }
}

/* §4.10.22.9's TEXT/PLAIN ENCODING ALGORITHM, which "Mail as body" runs for a `text/plain` form: for each pair,
   the name, U+003D, the value, and a CRLF. It shares fb_value's rule for a concolic entry for the same reason
   §4.10.22.7's serializer does — a shape is what the value is known to be, and percent-encoding one would
   invent bytes. */
static void fb_text_plain(JSContext *ctx, FormBuf *b, JSValueConst entries)
{
    int i, n = form_data_entry_count(entries);

    for (i = 0; i < n; i++) {
        size_t nlen = 0;
        const char *name = form_data_entry_name(entries, i, &nlen);

        fb_add(b, name, nlen);
        fb_str(b, "=");
        fb_value(ctx, b, form_data_entry_value(entries, i));
        fb_str(b, "\r\n");
    }
}

/* An http/https submission, as far as this engine goes: DERIVE the request and record it. It is the ONE place
   step 26's plan-to-navigate is not performed, because a submission mutates server state by construction and
   this engine never fires one. The entries are already in the URL's query by the time this runs — see the two
   cells below for why the POST cell puts them there too. */
static void form_record_request(JSContext *ctx, UrlRecord *action, int method, int enctype)
{
    EndpointHeader ct;
    char *serialized;
    JSValue url;

    serialized = url_serialize(action, /*exclude_fragment*/ false);
    url = JS_NewString(ctx, serialized ? serialized : "");
    if (method == FORM_METHOD_POST) {
        ct.name = "Content-Type";
        ct.value = form_enctype_mime(enctype);
        endpoint_record(ctx, "POST", url, &ct, 1, NULL);
    } else {
        endpoint_record(ctx, "GET", url, NULL, 0, NULL);
    }
    JS_FreeValue(ctx, url);
    free(serialized);
}

/* §4.10.22.3's HAND-OFF TO EXTERNAL SOFTWARE — §7.4.2.3.4's navigate step for a URL "handled using a mechanism that
   does not affect targetNavigable ... because url's scheme is handled externally". The navigation ends there:
   the URL goes to whatever the platform registered for its scheme and the navigable keeps the document it had.
   IT HAS NO SCRIPTABLE RESULT, and that is the standard's answer rather than this engine's gap — "hand-off to
   external software" is user-agent-defined and defines nothing a page can observe, which is the same kind of
   answer `element.click()` gives for a UA with no user. What a page CAN observe is that its document did not
   change, and that is exactly what happens. It takes the URL because the operation does; an operation whose
   input is not passed to it is an operation nobody can later give a real handler to. */
static void form_hand_off_to_external(JSContext *ctx, const UrlRecord *action)
{
    (void)ctx;
    DCHECK(action != NULL && action->scheme != NULL,
           "a hand-off to external software was given a URL with no scheme — the SCHEME is the whole of what "
           "decided that this URL is handled externally, so a record without one cannot have reached here");
}

/* ---- §4.10.22.3 STEP 26'S TABLE ------------------------------------------------------------------------------
 *
 * "Select the appropriate row in the table below based on scheme as given by the first cell of each row. Then,
 * select the appropriate cell on that row based on method as given in the first cell of each column."
 *
 *                     GET                    POST
 *      http           Mutate action URL      Submit as entity body
 *      https          Mutate action URL      Submit as entity body
 *      ftp            Get action URL         Get action URL
 *      javascript     Get action URL         Get action URL
 *      data           Mutate action URL      Get action URL
 *      mailto         Mail with headers      Mail as body
 *
 * IT IS A TABLE HERE BECAUSE IT IS A TABLE THERE. Written as a chain of scheme comparisons, a missing row is
 * invisible — which is how five of them came to be crashes rather than behaviour — and the two columns of a row
 * end up in two different places. Written out, the shape of the standard's own answer is readable, which is
 * what decides the row it does not have (below).
 *
 * EVERY CELL ENDS IN "PLAN TO NAVIGATE". What differs between them is only what goes into the URL first: the
 * entry list as a query, the entry list as a body, nothing at all, or a mailto:'s headers or body. */
enum { CELL_MUTATE_ACTION_URL = 0, CELL_ENTITY_BODY, CELL_GET_ACTION_URL,
       CELL_MAIL_WITH_HEADERS, CELL_MAIL_AS_BODY };

static const struct { const char *scheme; uint8_t get, post; } SUBMIT_SCHEME_TABLE[] = {
    { "http",       CELL_MUTATE_ACTION_URL,   CELL_ENTITY_BODY },
    { "https",      CELL_MUTATE_ACTION_URL,   CELL_ENTITY_BODY },
    { "ftp",        CELL_GET_ACTION_URL,      CELL_GET_ACTION_URL },
    { "javascript", CELL_GET_ACTION_URL,      CELL_GET_ACTION_URL },
    { "data",       CELL_MUTATE_ACTION_URL,   CELL_GET_ACTION_URL },
    { "mailto",     CELL_MAIL_WITH_HEADERS,   CELL_MAIL_AS_BODY },
};
#define SUBMIT_SCHEME_TABLE_N ((int)(sizeof(SUBMIT_SCHEME_TABLE) / sizeof(SUBMIT_SCHEME_TABLE[0])))

/* THE ROW THE TABLE DOES NOT HAVE. "If scheme is not one of those listed in this table, then the behavior is
 * not defined by this specification. User agents should, in the absence of another specification defining this,
 * act in a manner analogous to that defined in this specification for similar schemes." That sentence is an
 * instruction to DECIDE, so this engine decides, and the decision is stated in the table rather than left as a
 * fallthrough that reads like an oversight.
 *
 * THE DECISION: `Get action URL`, in BOTH columns — the entry list is discarded and the parsed action is
 * navigated to unchanged.
 *
 * THE REASONING is the shape of the six rows above. A cell writes the entry list INTO the URL only where that
 * URL's query is defined to mean something to whatever consumes it: `http`/`https` in both columns (the query
 * IS the request's), `data:` in the GET column (a data: URL's query is part of the data), and `mailto:` through
 * two cells of its own (a mailto: query is a header list). The two schemes for which a query means nothing —
 * `ftp` and `javascript` — take `Get action URL` in BOTH columns, and an unlisted scheme is exactly that case:
 * nothing here knows what a query denotes for it. WHATWG URL makes this concrete rather than cautious — a
 * scheme outside the special set gets an OPAQUE PATH, so writing a query onto one does not merely fail to help,
 * it changes what the URL denotes. Discarding the entry list is the only cell that cannot corrupt the address,
 * and it is also the only one that does not depend on the method, which is the right property for a row chosen
 * without knowing what the scheme does. */
static int submit_cell_for(const char *scheme, int method)
{
    int i;

    DCHECK(scheme != NULL, "step 26 was asked for a row with no scheme — §4.4's parser cannot answer success "
                           "without one");
    DCHECK(method == FORM_METHOD_GET || method == FORM_METHOD_POST,
           "step 26's table has two columns and the dialog method reaches step 11's return long before it");
    for (i = 0; i < SUBMIT_SCHEME_TABLE_N; i++)
        if (strcmp(SUBMIT_SCHEME_TABLE[i].scheme, scheme) == 0)
            return method == FORM_METHOD_POST ? SUBMIT_SCHEME_TABLE[i].post : SUBMIT_SCHEME_TABLE[i].get;
    return CELL_GET_ACTION_URL;   /* the unlisted-scheme row, decided above */
}

/* WHERE THIS MACHINE RESTS — every stage at a step §4.10.22.3 names, and each boundary for a reason the step
   itself states. Two of them are the page's code (the `submit` event, and the `formdata` event inside the entry
   list) and two are walks of the PAGE's size (the user-validity walk and the constraint walk), which is the
   other thing a stage exists for: a walk as long as the document, run inside one opcode, is the
   drive-to-completion this engine has no other bound against. The entry-list sub-sequence carries its OWN
   cursor, so this machine spends one stage on the whole of §4.10.22.4. */
#define SUBMIT_STAGES(X, ENTRY_LABEL) \
    X(SUBMIT_ENTER,     ENTRY_LABEL) \
    X(SUBMIT_VALIDITY,  "HTML §4.10.22.3 step 5.3 (for each field in the list of submittable elements whose " \
                        "form owner is form, set field's user validity to true)") \
    X(SUBMIT_VALIDATE,  "HTML §4.10.22.3 step 5.4 (if the submitter element's no-validate state is false, " \
                        "interactively validate the constraints of form and examine the result)") \
    X(SUBMIT_FIRE,      "HTML §4.10.22.3 step 5.6 (fire an event named submit at form using SubmitEvent, with " \
                        "submitter initialized to submitterButton, bubbles true and cancelable true)") \
    X(SUBMIT_ENTRIES,   "HTML §4.10.22.3 step 7 (let entry list be the result of constructing the entry list " \
                        "with form, submitter and encoding)") \
    X(SUBMIT_REQUEST,   "HTML §4.10.22.3 step 26 (select the row for the action's scheme and the column for " \
                        "the method, and run that row's steps)") \
    X(SUBMIT_DIALOG,    "HTML §4.10.22.3 step 11.6 (close the dialog subject with result and null) — §4.11.4's " \
                        "own algorithm, which fires `beforetoggle` at the dialog and so rests here")
enum { SUBMIT_STAGES(JS_STEP_STAGE_ENUM, "") };
/* ONE list, expanded once per ALGORITHM: the two members reach this machine at two different steps of their
   OWN, and a stage's label is the step it rests at — so the entry stage names the member that entered. */
static const char *const SUBMIT_STEPS_METHOD[] = {
    SUBMIT_STAGES(JS_STEP_STAGE_LABEL,
                  "HTML §4.10.3 submit() step 1 (submit this from this, with submitted from submit() method "
                  "set to true), through §4.10.22.3 steps 1-4")
    NULL
};
static const char *const SUBMIT_STEPS_REQUEST[] = {
    SUBMIT_STAGES(JS_STEP_STAGE_LABEL,
                  "HTML §4.10.3 requestSubmit(submitter) steps 1-3 (the submit-button and form-owner checks, "
                  "then submit this form element from submitter), through §4.10.22.3 steps 1-5.2")
    NULL
};

/* `arg`: WHICH member entered, which IS the algorithm's own "submitted from submit() method" boolean. */
enum { SUBMIT_FROM_METHOD = 0, SUBMIT_FROM_REQUEST };

typedef struct JSSubmitState {
    JSStepHdr hdr;         /* FIRST — the driver writes the def and the operand bounds through it */
    uint8_t   fphase;      /* the submit-event fire request's own phase */
    uint8_t   firing_set;  /* THIS run set the form's firing-submission-events, so THIS run must clear it */
    uint32_t  i;           /* the cursor of whichever per-control walk is running (steps 5.3 and 5.4) */
    JSValue   ev;          /* the SubmitEvent, minted once and held across the suspension (owned) */
    EventFireCb   cb;       /* the fire request buffer: [this, dispatch, target, event] */
    JSValue   submitter;   /* the algorithm's `submitter` — the form itself when none was given (owned) */
    /* THE FORM, held by this machine even though the header already carries it as the receiver — because the
       shared teardown RELEASES `this_val` before a definition's own fini runs, and an abandoned run has to
       clear step 5.2's flag off the form after that point. The same reason §4.10.22.4's run holds one. */
    JSValue   form;
    JSValue   controls;    /* the walk's list of submittable elements, held across its yields (owned) */
    JSValue   entry_list;  /* step 7's entry list, held from step 8 to step 26 (owned) */
    /* Step 6's PICKED ENCODING, latched at the moment step 6 runs rather than re-read on every resume: a
       `formdata` handler can write `accept-charset`, and a re-read would hand the second half of one entry
       list an encoding the first half never saw. A static string from §4.10.22.5's table, owned by nobody. */
    const char *encoding;
    FormEntryListRun entries;        /* §4.10.22.4's own cursor, held across its `formdata` dispatch */
    /* §4.10.21.2's own cursor, held across its `invalid` dispatches and its pattern matches. NULL until step
       5.4 starts one, which is also the state a `novalidate` form's run is left in. */
    ConstraintValidationRun *validation;
    /* STEP 11'S TWO ANSWERS, LATCHED WHERE THE STEPS COMPUTE THEM AND CARRIED INTO §4.11.4 — not re-derived on
       each resume. Step 11.2's `subject` is the form's nearest ancestor `dialog` AT STEP 11, and step 11.4/11.5's
       `result` is the submitter's selected coordinate or optional value AT STEP 11; the close fires
       `beforetoggle` at the dialog, so the page's code runs in the middle and can move the form out of the
       dialog or rewrite the submitter's `value`. An operation that becomes a work item takes its inputs with
       it, and reading either back afterwards would answer with a real value belonging to the wrong moment. */
    JSValue   dialog_subject;   /* step 11.2's nearest ancestor dialog element (owned) */
    JSValue   dialog_result;    /* steps 11.3-11.5's result: JS_NULL or a string (owned) */
    /* §4.11.4's own run, held across its `beforetoggle` dispatch. NULL until step 11.6 starts one. */
    DialogCloseRun *closing;
} JSSubmitState;

static void js_submit_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSSubmitState *s = st;
    int k;

    v->val(ctx, &s->ev);
    v->val(ctx, &s->submitter);
    v->val(ctx, &s->form);
    v->val(ctx, &s->controls);
    v->val(ctx, &s->entry_list);
    v->val(ctx, &s->dialog_subject);
    v->val(ctx, &s->dialog_result);
    STEP_CB_FOREACH(s->cb, k)
        v->val(ctx, &s->cb[k]);
    form_entry_list_visit(ctx, &s->entries, v);
    constraint_validation_visit(ctx, &s->validation, v);
    html_dialog_close_visit(ctx, &s->closing, v);
}

static JSValue js_submit_fini(JSContext *ctx, void *st, bool take_result)
{
    JSSubmitState *s = st;

    (void)take_result;
    /* THE TWO FLAGS, WHICH ARE ALL THIS OWNS BESIDE ITS DECLARATION. Step 5.7 clears the firing flag on every
       path THROUGH the algorithm; an ABANDONED run — a flow dropped inside the `submit` dispatch, or one whose
       handler threw — has to clear it too, or the form can never fire a submission event again. §4.10.22.4's
       constructing-entry-list flag is the same obligation, which is why the entry-list record is UNLOCKED here
       and its references left to js_submit_visit. Everything else the state holds — the form, the event, the
       submitter, both control lists, the dialog pair, the fire request, and the validation and dialog-close
       runs — is named by that declaration and released through it. */
    if (s->firing_set) {
        form_firing_set(ctx, s->form, false);
        s->firing_set = 0;
    }
    form_entry_list_unlock(ctx, &s->entries);
    return JS_UNDEFINED;   /* both members return undefined whatever the handlers did */
}

/* The list both of step 5's walks are over — §4.10.22.3 step 5.3's and §4.10.21.2 step 1's, which are the same
   sentence. Built at the walk's first entry and released at its last, so the two walks are two READS of the
   tree: the page's code runs between them the moment §4.10.21.2 step 5's `invalid` fire exists. */
static uint32_t submit_walk_list(JSContext *ctx, JSSubmitState *s)
{
    if (JS_IsUndefined(s->controls)) {
        s->controls = html_form_submittable_controls(ctx, s->hdr.this_val);
        s->i = 0;
    }
    return js_array_len(ctx, s->controls);
}

static void submit_walk_end(JSContext *ctx, JSSubmitState *s, int next_stage)
{
    JS_FreeValue(ctx, s->controls);
    s->controls = JS_UNDEFINED;
    s->i = 0;
    s->hdr.stage = (uint16_t)next_stage;
}

/* STEP 11'S RESULT, computed where step 11 computes it: "if submitter is an input element whose type attribute
   is in the Image Button state, let (x, y) be the selected coordinate and set result to x, ',' and y;
   otherwise, if submitter is a submit button, set result to submitter's OPTIONAL VALUE". §4.10.18.1's optional
   value "largely mirrors the value but doesn't normalize to an empty string": for both `button` and
   `input type=submit` it is the `value` content attribute's value if there is one, and NULL otherwise — which
   is why `<button>` with no `value` closes the dialog without touching its `returnValue`.
   The coordinate is (0, 0) for the reason §4.10.22.4 step 5.2 states once: §4.10.5.1.19 initialises it there and
   only a user activating the control while selecting a point ever moves it. Answers JS_NULL or a string. */
static JSValue submit_dialog_result(JSContext *ctx, JSValueConst submitter)
{
    lxb_dom_element_t *el = form_elem_of(submitter);
    lxb_dom_node_t *n = node_of(submitter);
    size_t vlen = 0;
    const char *v;

    if (!el) return JS_NULL;
    if (html_form_input_state(n) == INPUT_STATE_IMAGE)
        return JS_NewString(ctx, "0,0");                            /* step 11.4 */
    if (!html_form_is_submit_button(ctx, submitter)) return JS_NULL;
    /* Step 11.5, and PRESENCE decides it: the optional value is null only when there is no `value` attribute at
       all, so `<button value="">` closes the dialog with the empty string and clears its `returnValue`, which
       is a different outcome from `<button>` leaving the previous one standing. */
    if (!has_attr(el, "value")) return JS_NULL;
    v = attr_of(el, "value", &vlen);
    return JS_NewStringLen(ctx, v ? v : "", v ? vlen : 0);
}

/* STEP 26'S CELL, performed: what this row puts into the parsed action before it plans to navigate. */
static void submit_run_cell(JSContext *ctx, JSSubmitState *s, UrlRecord *parsed, int cell, int method,
                            int enctype)
{
    FormBuf b = { 0 };

    switch (cell) {
    case CELL_MUTATE_ACTION_URL:
        /* "Let pairs be … converting to a list of name-value pairs with entry list; let query be … the
           urlencoded serializer with pairs and encoding; SET parsed action's query component to query." A
           REPLACEMENT and not an append — a form whose action already carries one (`/search?v=2`) submits to
           `/search?<the entries>`, and an append produces a URL with two `?` in it. */
        fb_pairs(ctx, &b, s->entry_list);
        url_member_set(parsed, URL_SEARCH, b.s ? b.s : "", b.n);
        break;
    case CELL_ENTITY_BODY:
        DCHECK(method == FORM_METHOD_POST,
               "§4.10.22.3's Submit as entity body asserts the method is POST, and the table only reaches it "
               "from the POST column");
        /* The cell leaves the URL alone and makes the entries the request's BODY. THEY GO IN THE URL ANYWAY,
           and that is this engine's recording rather than a misreading of the cell: an @H record carries a
           request's PARAMETERS and the method beside them, with no body field, because what a reviewer
           reproduces from one is the parameters — and the recorded method plus the Content-Type header are
           exactly what say whether they travel as a query or as a body. */
        fb_pairs(ctx, &b, s->entry_list);
        url_member_set(parsed, URL_SEARCH, b.s ? b.s : "", b.n);
        break;
    case CELL_GET_ACTION_URL:
        break;   /* "Plan to navigate to parsed action. Entry list is discarded." */
    case CELL_MAIL_WITH_HEADERS: {
        /* "…let headers be the urlencoded serializer with pairs and encoding; REPLACE occurrences of U+002B
           PLUS SIGN in headers with the string `%20`; set parsed action's query to headers." The replacement is
           the whole point of the cell: a mailto: query is a HEADER LIST, and `+` in a header value is a plus
           sign rather than the space the urlencoded form makes it. */
        size_t i;

        fb_pairs(ctx, &b, s->entry_list);
        {
            FormBuf h = { 0 };

            for (i = 0; i < b.n; i++) {
                if (b.s[i] == '+') fb_str(&h, "%20");
                else fb_add(&h, b.s + i, 1);
            }
            url_member_set(parsed, URL_SEARCH, h.s ? h.s : "", h.n);
            free(h.s);
        }
        break;
    }
    case CELL_MAIL_AS_BODY: {
        /* "Switch on enctype: text/plain → body is the text/plain encoding algorithm over pairs, then UTF-8
           PERCENT-ENCODE using the default encode set; otherwise → body is the urlencoded serializer. If parsed
           action's query is null, set it to the empty string; if it is not the empty string, append `&`; append
           `body=`; append body." The default encode set is the URL Standard's PATH percent-encode set, which is
           the same anchor that definition carries. */
        FormBuf q = { 0 };
        char *existing = url_member_get(parsed, URL_SEARCH);
        char *encoded = NULL;
        const char *body;
        size_t body_n;

        if (enctype == FORM_ENCTYPE_TEXT) {
            fb_text_plain(ctx, &b, s->entry_list);
            encoded = url_percent_encode(b.s ? b.s : "", b.n, URL_SET_PATH);
            CHECK(encoded != NULL, "form: OOM percent-encoding a mailto: body");
            body = encoded;
            body_n = strlen(encoded);
        } else {
            fb_pairs(ctx, &b, s->entry_list);
            body = b.s ? b.s : "";
            body_n = b.n;
        }
        /* url_member_get answers `search` the way the member reads it — with its leading `?` when there is a
           query and the empty string when there is none — so the `?` is dropped here and the query the
           standard is appending to is what remains. */
        if (existing && existing[0] == '?') fb_str(&q, existing + 1);
        if (q.n) fb_str(&q, "&");
        fb_str(&q, "body=");
        fb_add(&q, body, body_n);
        url_member_set(parsed, URL_SEARCH, q.s ? q.s : "", q.n);
        free(existing);
        free(encoded);
        free(q.s);
        break;
    }
    default:
        DFAIL("step 26 selected a cell §4.10.22.3's table does not have — the five behaviours below the table "
              "are the whole of it, and a sixth means the table above grew a value with no arm here");
    }
    free(b.s);
}

/* §4.10.22.3's PLAN TO NAVIGATE, which every cell ends in, dispatched on the scheme that chose the row. */
static void submit_plan_to_navigate(JSContext *ctx, JSSubmitState *s, UrlRecord *parsed, int method,
                                    int enctype, lxb_dom_element_t *form)
{
    const char *scheme = parsed->scheme;
    size_t tlen = 0;
    const char *target;
    char *serialized;

    /* http/https: the ONE case in which the planned navigation is not performed — it is a REQUEST, and this
       engine derives it from the forced-exec path instead of firing it (see this section's header). */
    if (strcmp(scheme, "http") == 0 || strcmp(scheme, "https") == 0) {
        form_record_request(ctx, parsed, method, enctype);
        return;
    }
    /* `mailto:` and `ftp:` are HANDLED EXTERNALLY, so §7.4.2.3.4's navigate hands the URL off and the navigable
       keeps the document it had. For `mailto:` that is what the two Mail cells exist to build a URL for — the
       platform's mail client is what consumes it. For `ftp:` it is a UA DECISION the standard leaves open
       ("handled using a mechanism that does not affect targetNavigable, e.g. because url's scheme is handled
       externally"), and this engine takes it: nothing here fetches FTP, the host's chokepoint is HTTP, and
       every shipping browser hands the scheme to the operating system. Neither is an endpoint, which is the
       whole reason the rows are written out — an @H record for either would be a request the form never makes.
       The target navigable does not enter into it: a hand-off affects no navigable, so there is nothing here
       that steps 18-21 would decide. */
    if (strcmp(scheme, "mailto") == 0 || strcmp(scheme, "ftp") == 0) {
        form_hand_off_to_external(ctx, parsed);
        return;
    }
    /* Everything below NAVIGATES a navigable, and steps 18-21 are what say WHICH — get an element's target,
       get an element's noopener, then the rules for choosing a navigable. This engine does not run them,
       because until now no row needed a navigable at all: the recording rows derive a request and the two
       hand-offs affect none. `_self` and the empty string are the one target whose chosen navigable is the
       form's own, which is this document, so that is the case the rows below can act in and the rest is named
       as the work it is. */
    target = form_element_target(ctx, s->submitter, form, node_of(s->hdr.this_val), &tlen);
    if (strcmp(scheme, "javascript") == 0) {
        if (!form_target_is_self(target, tlen))
            DFAIL("a form submits to a `javascript:` action while naming another navigable in `target` or "
                  "`formtarget` — §4.10.22.3 steps 18-21 choose that navigable and §7.4.2.3.2 evaluates the "
                  "URL in ITS active document, under ITS settings object and API base URL; build steps 18-21 "
                  "(get an element's target, get an element's noopener, the rules for choosing a navigable) "
                  "and evaluate the URL in the navigable they choose");
        serialized = url_serialize(parsed, /*exclude_fragment*/ false);
        CHECK(serialized != NULL, "form: OOM serializing a javascript: action URL");
        navigable_evaluate_javascript_url(ctx, serialized);
        free(serialized);
        return;
    }
    if (strcmp(scheme, "data") == 0)
        DFAIL("a form submits to a `data:` action — §4.10.22.3 step 26 plans to navigate to it (the GET cell "
              "having mutated its query first), and navigating to a data: URL builds a Document out of the "
              "URL's OWN bytes with no network involved. §7.4's navigate can only load an address the HOST "
              "fetches (navigable.c's js_nav_load_step asks `document.fetch\\t<url>`, which for a data: URL is "
              "a GET of the literal text); build the data: URL processor and the navigate that takes a "
              "RESPONSE THE ENGINE ALREADY HAS, then route this row through it");
    /* The unlisted-scheme row took `Get action URL` above, which is a decision about the TABLE; what the
       navigation then does is §7.4.2.3.4's business and depends on the scheme — `file:` and `blob:` load a
       document, a registered `web+foo:` hands off — and nothing here knows which. */
    DFAIL("a form submits to an action whose scheme §4.10.22.3's step 26 table does not list. The TABLE row is "
          "decided — `Get action URL` in both columns, so the entry list is discarded and the parsed action is "
          "navigated to unchanged (see submit_cell_for) — and what is missing is the navigation: §7.4.2.3.4's "
          "navigate has to decide, per scheme, between loading a document and handing off to external "
          "software, and this engine's navigate does neither for a scheme it has never seen");
}

/* Steps 10-26 over an entry list that exists: the method, the dialog branch, the action URL, and the row of
   step 26's table the scheme and the method select. Answers a step code, because step 11 ends in §4.11.4's
   close the dialog and that fires an event at the page. */
static int submit_request(JSContext *ctx, JSSubmitState *s)
{
    lxb_dom_element_t *form = form_elem_of(s->hdr.this_val);
    lxb_dom_node_t *n = node_of(s->hdr.this_val), *a;
    const char *base = document_base_url(ctx);
    const char *action;
    size_t alen = 0;
    UrlRecord parsed, baserec;
    int method, enctype, cell;
    bool have_base, ok;

    method = form_method_state(ctx, s->submitter, form);            /* step 10 */
    if (method == FORM_METHOD_DIALOG) {                             /* step 11 */
        for (a = n ? n->parent : NULL; a; a = a->parent)
            if (html_dialog_is_dialog(a)) break;
        if (!a) return JS_STEP_DONE;                                /* step 11.1 */
        /* Steps 11.2-11.5, computed HERE and carried on this state — see the fields' own note for why neither
           may be read back after §4.11.4 has run the page's `beforetoggle` handler. */
        DCHECK(JS_IsUndefined(s->dialog_subject),
               "step 11.2 ran twice in one submission — the subject is latched once and the stage that follows "
               "it never returns here");
        s->dialog_subject = node_wrap(ctx, a);                      /* step 11.2 */
        s->dialog_result = submit_dialog_result(ctx, s->submitter); /* steps 11.3-11.5 */
        s->hdr.stage = SUBMIT_DIALOG;                               /* step 11.6, and step 11.7's return */
        return JS_STEP_YIELD;
    }
    {
        lxb_dom_element_t *from = NULL;
        const char *aname = NULL;

        action = submitter_attr_from(ctx, s->submitter, form, "formaction", "action", &alen,
                                     &from, &aname);                                   /* step 12 */
        /* THE ACTION IS A URL SINK WHEN THE PAGE'S OWN CODE WROTE IT. Step 26 plans to NAVIGATE to whatever
           this attribute holds, and navigating to a `javascript:` URL executes it in this document — which is
           the same sink `location = x` is, and the solver's URL-context breakout is exactly the `javascript:`
           scheme. It is reported HERE and not inside the javascript: row below, because the row is chosen by
           the scheme of the value the page HAPPENED to write and the attacker's candidate is the value that
           would replace it: an action assigned from `location.hash` selects the `https` row today and the
           `javascript` row under the breakout. An attribute carrying no taint is not a sink, and the solver
           drops it — the taint is BORROWED, so nothing here frees it. */
        if (from) {
            JSValue taint = dom_cow_attr_taint(from, aname);

            if (!JS_IsUndefined(taint)) solve_url_sink(ctx, taint);
        }
    }
    if (!action || !alen) {                                                            /* step 13 */
        action = base;
        alen = base ? strlen(base) : 0;
    }
    /* Step 14: ENCODING-PARSE the action relative to the submitter's node document, which is this realm's — one
       component owns what this document's URL is. Step 15: a failure returns, which is what an action nothing
       can resolve (a relative one in a document with no address) produces. */
    url_record_init(&baserec);
    have_base = base && *base && url_parse(&baserec, base, strlen(base), NULL);
    url_record_init(&parsed);
    ok = alen != 0 && url_parse(&parsed, action, alen, have_base ? &baserec : NULL);
    url_record_free(&baserec);
    if (!ok) {
        url_record_free(&parsed);
        return JS_STEP_DONE;                                        /* step 15 */
    }
    DCHECK(parsed.scheme != NULL, "the URL parser answered success with no scheme, which §4.4 cannot produce");
    enctype = form_enctype_state(ctx, s->submitter, form);          /* step 17 */
    cell = submit_cell_for(parsed.scheme, method);                  /* step 26's row and column */
    submit_run_cell(ctx, s, &parsed, cell, method, enctype);
    submit_plan_to_navigate(ctx, s, &parsed, method, enctype, form);
    url_record_free(&parsed);
    return JS_STEP_DONE;
}

static int js_submit_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSSubmitState *s = st;
    int r;

    if (s->hdr.stage == SUBMIT_ENTER) {
        JSValueConst given = step_arg(&s->hdr, 0);
        int k;

        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        /* EVERY owned field before the first thing that can throw: the failure path tears this state down
           through js_submit_fini, which frees exactly what the state holds and nothing else. */
        s->ev = s->submitter = s->form = s->controls = s->entry_list = JS_UNDEFINED;
        s->dialog_subject = s->dialog_result = JS_UNDEFINED;
        s->encoding = NULL;
        s->validation = NULL;
        s->closing = NULL;
        s->i = 0;
        s->firing_set = 0;
        STEP_CB_FOREACH(s->cb, k) s->cb[k] = JS_UNDEFINED;
        form_entry_list_init(&s->entries);
        /* Web IDL §3.7.5's brand check: both members are HTMLFormElement's, and one invoked on anything else
           is a TypeError. It was a silent `return`, which told a page that `submit.call({})` had submitted. */
        if (!html_form_is_form_element(s->hdr.this_val)) {
            JS_ThrowTypeError(ctx, "submit/requestSubmit was called on something that is not an "
                                   "HTMLFormElement");
            return JS_STEP_ABRUPT;
        }
        s->form = JS_DupValue(ctx, s->hdr.this_val);
        if (s->hdr.arg == SUBMIT_FROM_REQUEST) {
            /* §4.10.3's requestSubmit(submitter) step 1: "if submitter is not null". The IDL declares
               `optional HTMLElement? submitter = null`, so an absent argument and an explicit undefined are
               both the IDL null and both take step 2. */
            if (!JS_IsUndefined(given) && !JS_IsNull(given)) {
                if (!html_form_is_submit_button(ctx, given)) {
                    /* Step 1.1 — a TypeError, because being a submit button is a fact about the ARGUMENT. */
                    JS_ThrowTypeError(ctx, "requestSubmit was given a submitter that is not a submit button");
                    return JS_STEP_ABRUPT;
                }
                {
                    /* Step 1.2 — a "NotFoundError" DOMException, because a submit button that belongs to
                       ANOTHER form is a relationship this form does not have rather than a wrong type. */
                    JSValue owner = html_form_owner_of(ctx, given);
                    bool mine = JS_VALUE_GET_PTR(owner) == JS_VALUE_GET_PTR(s->hdr.this_val);

                    JS_FreeValue(ctx, owner);
                    if (!mine) {
                        JS_ThrowDOMException(ctx, "NotFoundError",
                                             "the submitter's form owner is not this form element");
                        return JS_STEP_ABRUPT;
                    }
                }
                s->submitter = JS_DupValue(ctx, given);
            } else {
                s->submitter = JS_DupValue(ctx, s->hdr.this_val);   /* step 2 */
            }
        } else {
            /* §4.10.3's submit() steps: "submit this from THIS" — the submitter is the form, which is what
               makes §4.10.22.4 step 5.1's "field is a button but it is not submitter" drop every button. */
            s->submitter = JS_DupValue(ctx, s->hdr.this_val);
        }
        if (form_cannot_navigate(ctx, s->hdr.this_val)) return JS_STEP_DONE;              /* step 1 */
        /* Step 2: "if form's constructing entry list is true, then return" — a form re-entered from its own
           `formdata` handler submits NOTHING, and this is why that answer arrives before step 5.6 fires a
           second `submit` event at it. §4.10.22.4 step 1 would notice the same fact, but only after the event
           and only as the null step 8 asserts against. */
        if (form_entry_list_constructing(ctx, s->hdr.this_val)) return JS_STEP_DONE;      /* step 2 */
        if (form_document_sandboxes_forms(ctx)) return JS_STEP_DONE;                      /* steps 3-4 */
        /* Step 5's condition IS this machine's `arg`: `submit()` sets "submitted from submit() method", which
           skips the validity walk, the validation and the event entirely — so that entry point's next stage is
           the entry list. */
        if (s->hdr.arg == SUBMIT_FROM_METHOD) {
            STEP_GOTO(s->hdr.stage, SUBMIT_ENTRIES, &s->fphase, NULL);
        } else {
            if (form_firing_events(ctx, s->hdr.this_val)) return JS_STEP_DONE;            /* step 5.1 */
            form_firing_set(ctx, s->hdr.this_val, true);                                  /* step 5.2 */
            s->firing_set = 1;
            STEP_GOTO(s->hdr.stage, SUBMIT_VALIDITY, &s->fphase, NULL);
        }
    }
    if (s->hdr.stage == SUBMIT_VALIDITY) {
        /* Step 5.3: "for each element field in the list of submittable elements whose form owner is form, set
           field's USER VALIDITY to true" — §4.10.18.1's boolean, which is what makes a control that has been
           through a submission match :user-invalid afterwards. It is per-control state, so it lives in the
           element's per-flow property slot beside its value and its checkedness, and time-travels with them. */
        uint32_t n = submit_walk_list(ctx, s);

        if (s->i < n) {
            JSValue el = JS_GetPropertyUint32(ctx, s->controls, s->i++);
            lxb_dom_element_t *e = form_elem_of(el);

            if (e) dom_cow_set_prop_taint(ctx, e, "userValidity", JS_TRUE);
            JS_FreeValue(ctx, el);
            return JS_STEP_YIELD;   /* a walk of the PAGE's size offers a suspend point per element */
        }
        submit_walk_end(ctx, s, SUBMIT_VALIDATE);
    }
    if (s->hdr.stage == SUBMIT_VALIDATE) {
        /* Step 5.4: "if the submitter element's no-validate state is false, then INTERACTIVELY VALIDATE the
           constraints of form and EXAMINE THE RESULT: if the result was negative, then RETURN." A negative
           result ends the submission here — no `submit` event, no entry list, no request — which is the whole
           of what this step decides and why the validation is not advisory. §4.10.21.2 fires an `invalid` event
           at every unsatisfied control, so it is a sub-sequence that suspends, exactly like the entry list. */
        if (form_no_validate(ctx, s->submitter, form_elem_of(s->hdr.this_val))) {
            STEP_GOTO(s->hdr.stage, SUBMIT_FIRE, &s->fphase, NULL);
        } else {
            bool positive = false;

            r = constraint_validation_interactively_run(ctx, &s->hdr, &s->validation, s->hdr.this_val,
                                                        cb_result, &positive, out_cb, out_argc);
            cb_result = JS_UNDEFINED;
            if (r > 0) return r;
            if (r < 0) return JS_STEP_ABRUPT;
            constraint_validation_release(ctx, &s->validation);
            if (!positive) return JS_STEP_DONE;
            STEP_GOTO(s->hdr.stage, SUBMIT_FIRE, &s->fphase, NULL);
        }
    }
    if (s->hdr.stage == SUBMIT_FIRE) {
        bool not_canceled = true;

        if (JS_IsUndefined(s->ev)) {
            /* Step 5.5: "let submitterButton be NULL if submitter is form; otherwise let submitterButton be
               submitter" — which is the whole reason `e.submitter` is null for `form.requestSubmit()` and the
               button for `form.requestSubmit(btn)`. Step 5.6 mints the event with it. */
            JSValue button = JS_VALUE_GET_PTR(s->submitter) == JS_VALUE_GET_PTR(s->hdr.this_val)
                                 ? JS_NULL : s->submitter;   /* BORROWED: submit_event_new dups what it keeps */

            s->ev = submit_event_new(ctx, button);
            if (JS_IsException(s->ev)) {
                s->ev = JS_UNDEFINED;
                return JS_STEP_ABRUPT;
            }
        }
        /* Step 5.6's dispatch, as a REQUEST, so the handlers run as ordinary preemptible page code and this
           resumes after every one of them has returned. */
        r = event_target_fire_run(ctx, &s->fphase, STEP_CB(s->cb), s->hdr.this_val, s->ev, JS_UNDEFINED, cb_result,
                                  &not_canceled, out_cb, out_argc);
        if (r > 0) return r;
        if (r < 0) return JS_STEP_ABRUPT;
        cb_result = JS_UNDEFINED;
        form_firing_set(ctx, s->hdr.this_val, false);                                     /* step 5.7 */
        s->firing_set = 0;
        if (!not_canceled) return JS_STEP_DONE;                                           /* step 5.8 */
        /* Step 5.9: cannot navigate is asked AGAIN, because a handler had the document in its hands. */
        if (form_cannot_navigate(ctx, s->hdr.this_val)) return JS_STEP_DONE;
        STEP_GOTO(s->hdr.stage, SUBMIT_ENTRIES, &s->fphase, NULL);
    }
    if (s->hdr.stage == SUBMIT_ENTRIES) {
        JSValue list = JS_UNDEFINED;

        /* Step 6, once — see the field's own note for why this is a latch and not a read. */
        if (!s->encoding) s->encoding = form_pick_encoding(form_elem_of(s->hdr.this_val));
        r = form_entry_list_run(ctx, &s->entries, s->hdr.this_val, s->submitter, s->encoding, cb_result, &list,
                                out_cb, out_argc);                                        /* step 7 */
        if (r > 0) return r;
        if (r < 0) return JS_STEP_ABRUPT;
        /* Step 8 asserts the list is not null, and it holds here for the reason the spec says: step 2 already
           returned for a form whose constructing entry list is true, so nothing re-enters. */
        DCHECK(!JS_IsNull(list),
               "§4.10.22.3 step 8's assert failed — the entry list was null, which step 2 already excluded");
        s->entry_list = list;
        /* Step 9: cannot navigate a THIRD time, because the `formdata` handler had the document too. */
        if (form_cannot_navigate(ctx, s->hdr.this_val)) return JS_STEP_DONE;
        STEP_GOTO(s->hdr.stage, SUBMIT_REQUEST, &s->fphase, NULL);
    }
    if (s->hdr.stage == SUBMIT_REQUEST) return submit_request(ctx, s);
    DCHECK(s->hdr.stage == SUBMIT_DIALOG, "a form submission resumed into a stage §4.10.22.3 does not have");
    /* Step 11.6: "close the dialog subject with result and NULL" — the source is null because a form
       submission is not an element-sourced close the way `requestClose()` is. §4.11.4 fires `beforetoggle` at
       the dialog, so this is a sub-sequence that suspends, exactly like the entry list and the validation. */
    r = html_dialog_close_run(ctx, &s->closing, s->dialog_subject, s->dialog_result, JS_NULL, cb_result,
                              out_cb, out_argc);
    if (r > 0) return r;
    if (r < 0) return JS_STEP_ABRUPT;
    html_dialog_close_release(ctx, &s->closing);
    return JS_STEP_DONE;                                            /* step 11.7's return: no request is made */
}

static const JSTrampStepDef js_submit_def = {
    sizeof(JSSubmitState), js_submit_step, js_submit_fini, SUBMIT_FROM_METHOD, .visit = js_submit_visit,
    .algorithm = "HTML §4.10.22.3 submit a form (from HTMLFormElement.submit())",
    .steps = SUBMIT_STEPS_METHOD
};

static const JSTrampStepDef js_reqsubmit_def = {
    sizeof(JSSubmitState), js_submit_step, js_submit_fini, SUBMIT_FROM_REQUEST, .visit = js_submit_visit,
    .algorithm = "HTML §4.10.22.3 submit a form (from HTMLFormElement.requestSubmit())",
    .steps = SUBMIT_STEPS_REQUEST
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

/* THE STATES §4.10.5.3.6's `readonly` ATTRIBUTE APPLIES TO — the twelve text-entry states, which is the
   standard's own list and not a summary of it: the attribute is declared in the bookkeeping of Text, Search,
   Telephone, URL, Email, Password, Date, Month, Week, Time, Local Date and Time and Number, and in no other.
   Hidden, Range, Color, Checkbox, Radio Button, File Upload and the four button states do not have it, so a
   `readonly` written on one of those is an attribute with no meaning rather than one that immobilises it. */
static bool input_readonly_applies(HtmlInputState st)
{
    switch (st) {
    case INPUT_STATE_TEXT: case INPUT_STATE_SEARCH: case INPUT_STATE_TEL: case INPUT_STATE_URL:
    case INPUT_STATE_EMAIL: case INPUT_STATE_PASSWORD: case INPUT_STATE_DATE: case INPUT_STATE_MONTH:
    case INPUT_STATE_WEEK: case INPUT_STATE_TIME: case INPUT_STATE_DATETIME_LOCAL: case INPUT_STATE_NUMBER:
        return true;
    default:
        return false;
    }
}

bool html_form_input_is_mutable(JSContext *ctx, JSValueConst wrap)
{
    lxb_dom_node_t *n = node_of(wrap);
    HtmlInputState st = html_form_input_state(n);

    DCHECK(st != INPUT_STATE_NONE,
           "§4.10.5.1's mutability was asked of something that is not an `input` element — every other form "
           "control's editability is its own section's question (a `textarea` has its own readonly, a `select` "
           "has none at all), and answering it from this one would be that section's algorithm in the wrong "
           "file");
    if (html_form_control_is_disabled(ctx, wrap)) return false;
    return !(input_readonly_applies(st) && has_attr(lxb_dom_interface_element(n), "readonly"));
}

/* ---- what the entry list reads off a control ---------------------------------------------------------------
 * §4.10.22.4 walks controls it never received as a receiver, so the two pieces of state a control's entry is
 * made of are reachable by ELEMENT rather than only through the IDL accessor installed on its prototype. The
 * accessor's `magic` picks the DEFAULT that applies with no assigned state, and the tag is what decides it. */
JSValue html_form_control_value(JSContext *ctx, JSValueConst wrap)
{
    lxb_dom_node_t *n = node_of(wrap);

    /* An `input`'s value is §4.10.5.1's, sanitized and mode-dependent — so the entry list and the constraint
       validation read the SAME value the IDL attribute answers, which is the whole point of one accessor:
       `<input type=url value=" http://x ">` reported a typeMismatch a browser does not, because a browser
       stripped that whitespace before anything looked at the value. */
    if (html_form_input_state(n) != INPUT_STATE_NONE) return input_value_get(ctx, wrap);
    return js_ctrl_get_value(ctx, wrap,
                             tag_is(n, "textarea") ? CTRL_TEXTAREA : tag_is(n, "option") ? CTRL_OPTION
                                                                                         : CTRL_ATTRIBUTE);
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

/* Step 5.7's value IS §4.10.5.4's DEFAULT/ON MODE — "if the field element has a `value` attribute specified,
   then let value be the value of that attribute; otherwise, let value be the string `on`" is that mode's getter
   word for word, which is why the default is a word and not the empty string. So it is one algorithm and this
   is where the entry list asks for it, never a second copy that reads the slot the value mode writes: a
   `box.value = location.hash` is an assignment in DEFAULT/ON mode and lands on the CONTENT ATTRIBUTE, where the
   taint shadow keeps it a source. */
JSValue html_form_checkbox_value(JSContext *ctx, JSValueConst wrap)
{
    HtmlInputState st = html_form_input_state(node_of(wrap));

    DCHECK(st == INPUT_STATE_CHECKBOX || st == INPUT_STATE_RADIO,
           "§4.10.22.4 step 5.7's value was asked of a control that is not an `input` in the Checkbox or Radio "
           "Button state — those are the two states html_form_field_kind answers FORM_FIELD_CHECKBOX for");
    return input_value_get(ctx, wrap);
}

FormFieldKind html_form_field_kind(JSContext *ctx, JSValueConst wrap)
{
    lxb_dom_node_t *n = node_of(wrap);
    size_t nlen = 0;
    HtmlInputState st;

    if (!n || n->type != LXB_DOM_NODE_TYPE_ELEMENT) return FORM_FIELD_OTHER;
    if (tag_is(n, "select")) return FORM_FIELD_SELECT;
    if (!tag_is(n, "input")) {
        /* A `textarea` and a `button` both take step 5.11's "otherwise". A FACE is asked LAST here and FIRST in
           the chain, which costs nothing: nothing else can answer true for it. */
        return custom_elements_is_form_associated(ctx, wrap) ? FORM_FIELD_FACE : FORM_FIELD_OTHER;
    }
    st = html_form_input_state(n);
    if (st == INPUT_STATE_IMAGE) return FORM_FIELD_IMAGE_BUTTON;
    if (st == INPUT_STATE_CHECKBOX || st == INPUT_STATE_RADIO) return FORM_FIELD_CHECKBOX;
    if (st == INPUT_STATE_FILE) return FORM_FIELD_FILE;
    if (st == INPUT_STATE_HIDDEN) {
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

/* §3.2.6's AUTO-DIRECTIONALITY FORM-ASSOCIATED ELEMENTS, as a predicate over a NODE — the list two callers
   need and neither of them owns. §3.2.6's auto directionality reads such an element's VALUE instead of its
   text, and §4.10.22.4 step 5.12 submits such an element's directionality as a second entry; those are two
   uses of one list, and a second copy of it is the second answer that is always subtly wrong. */
bool html_form_is_auto_directionality_face(const lxb_dom_node_t *n)
{
    HtmlInputState st;

    if (!n || n->type != LXB_DOM_NODE_TYPE_ELEMENT) return false;
    if (tag_is((lxb_dom_node_t *)n, "textarea")) return true;
    st = html_form_input_state(n);
    /* The TEXT state is the missing-value AND invalid-value default, so an absent or unrecognised `type` is in
       it — which is why an unknown keyword answers true here rather than false. */
    switch (st) {
    case INPUT_STATE_HIDDEN: case INPUT_STATE_TEXT: case INPUT_STATE_SEARCH: case INPUT_STATE_TEL:
    case INPUT_STATE_URL: case INPUT_STATE_EMAIL: case INPUT_STATE_PASSWORD: case INPUT_STATE_SUBMIT:
    case INPUT_STATE_RESET: case INPUT_STATE_BUTTON:
        return true;
    default:
        return false;
    }
}

/* §3.2.6's ONE type-specific Undefined case: an `input` in the TELEPHONE state is 'ltr' whatever surrounds it,
   because a phone number is read left to right in every script. */
bool html_form_is_telephone_input(const lxb_dom_node_t *n)
{
    return html_form_input_state(n) == INPUT_STATE_TEL;
}

bool html_form_needs_dirname_entry(JSValueConst wrap)
{
    lxb_dom_node_t *n = node_of(wrap);
    size_t dlen = 0;

    if (!n || n->type != LXB_DOM_NODE_TYPE_ELEMENT) return false;
    if (!attr_of(lxb_dom_interface_element(n), "dirname", &dlen) || !dlen) return false;
    return html_form_is_auto_directionality_face(n);
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

JSValue html_form_placeholder_label_option(JSContext *ctx, JSValueConst select)
{
    lxb_dom_node_t *n = node_of(select);
    lxb_dom_element_t *sel = n ? lxb_dom_interface_element(n) : NULL;
    JSValue list, first, value, r = JS_NULL;
    size_t vlen = 0;
    const char *v;

    /* §4.10.7's three conditions, in the order it states them: `required` is specified, the display size is 1,
       and the FIRST option in the list of options has the empty string as its value and the select — not an
       optgroup — as its parent. */
    if (!sel || !has_attr(sel, "required") || select_display_size(sel) != 1) return JS_NULL;
    list = select_option_list(ctx, n);
    if (js_array_len(ctx, list) == 0) { JS_FreeValue(ctx, list); return JS_NULL; }
    first = JS_GetPropertyUint32(ctx, list, 0);
    JS_FreeValue(ctx, list);
    if (node_of(first) && node_of(first)->parent == n) {
        value = html_form_control_value(ctx, first);
        v = JS_ToCStringLen(ctx, &vlen, value);
        if (v && !vlen) r = JS_DupValue(ctx, first);
        if (v) JS_FreeCString(ctx, v);
        JS_FreeValue(ctx, value);
    }
    JS_FreeValue(ctx, first);
    return r;
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
    /* §4.10.22.3's own per-form boolean, under a Symbol of its own for the same reason: step 5's
       non-reentrancy is state ON THE FORM, and a page must not be able to see or write it. */
    g_firing_key = JS_NewSymbol(ctx, "firingSubmissionEvents", false);
    CHECK(!JS_IsException(g_firing_key), "the firing-submission-events slot key allocation failed");
    g_atom_firing = JS_ValueToAtom(ctx, g_firing_key);
    CHECK(g_atom_firing != JS_ATOM_NULL, "the firing-submission-events slot key could not be interned");
    /* §4.10.22.10's SubmitEvent, which step 5.6 fires; §4.10.22.11's FormDataEvent, which §4.10.22.4 step 7
       fires; and §4.10.22.4's own per-form flag key — declared from here because all three are §4.10's and
       this file is §4.10's declaration point. */
    submit_event_init(ctx);
    form_data_event_init(ctx);
    form_entry_list_declare(ctx);
    /* §4.10.21's constraint validation, declared from here for the same reason: its members go on the control
       interfaces §4.10 owns the prototypes of, and step 5.4 above is its caller. */
    constraint_validation_declare(ctx);
    /* §4.10.5.4's `files` and §4.10.5.1.17's list of selected files behind it, declared here for exactly the
       same reason: the member goes on HTMLInputElement.prototype, which §4.10 owns, and the algorithm belongs
       to the file that owns the value. Without this line the whole of input_value.c's file half was code no
       realm installed — an interface member reporting itself built and not existing. */
    input_value_declare(ctx);
    /* §4.10.5.4's showPicker(), declared here for the same reason `files` is — its member goes on
       HTMLInputElement.prototype and §4.10 owns that prototype — and it must come AFTER the line above,
       because the algorithm it ends in is input_value.c's update the file selection. */
    input_picker_declare(ctx);
    /* §6.4's USER ACTIVATION STATE — declared here because §6.4.1's per-Window record has to exist BEFORE the
       first realm is built (a realm that missed it answers §7.4.2.4's sticky-activation conjunct out of a
       record that is not there), and this is the declaration point core/html reaches the agent through. It is
       not §4.10's section; when core/html grows a declaration point that is not a form's, this line is one of
       the ones that belongs to it. */
    user_activation_init(ctx);
    /* BOTH submission members register their own step definition rather than declaring arguments to the args
       machine, so both install through the installer for that kind — see idl_install_step_method. `submit()`
       is one because §4.10.22.4's `formdata` event is the page's code and it runs on that path too. */
    g_id_submit = JS_RegisterStepDef(JS_GetRuntime(ctx), &js_submit_def);
    g_id_reqsubmit = JS_RegisterStepDef(JS_GetRuntime(ctx), &js_reqsubmit_def);
    /* §4.10.5.4's `value` takes a DOMString like the others, and input_value.c decides what its four modes do
       with it — including the File Upload state's throw, which reaches the page because a setter's exception
       propagates out of the args machine like any other. */
    g_id_val_input = idl_setter_id(ctx, IDL_DOMSTRING, false, js_input_set_value, 0);
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
    idl_install_accessor(ctx, input_proto, "value", js_input_get_value, 0, g_id_val_input);
    idl_install_accessor(ctx, textarea_proto, "value", js_ctrl_get_value, CTRL_TEXTAREA, g_id_val_textarea);
    idl_install_accessor(ctx, option_proto, "value", js_ctrl_get_value, CTRL_OPTION, g_id_val_option);
    idl_install_accessor(ctx, input_proto, "checked", js_ctrl_get_checked, 0, g_id_checked);
    constraint_validation_install(ctx, input_proto, textarea_proto);
    input_value_install(ctx, input_proto);   /* §4.10.5.4's `files`, on the prototype §4.10 owns */
    input_picker_install(ctx, input_proto);  /* §4.10.5.4's `showPicker()`, on the same prototype */
}

void html_form_free(JSRuntime *rt)
{
    submit_event_free(rt);
    form_data_event_free(rt);
    form_entry_list_free(rt);
    constraint_validation_free(rt);
    input_value_free();
    input_picker_free();
    user_activation_free();
    /* The slot keys are the AGENT's, so they are released with the agent — a Symbol nobody frees is a live GC
       object the runtime's own walk counts as a leak. */
    JS_FreeAtomRT(rt, g_atom_owner);
    g_atom_owner = JS_ATOM_NULL;
    JS_FreeValueRT(rt, g_owner_key);
    g_owner_key = JS_UNDEFINED;
    JS_FreeAtomRT(rt, g_atom_firing);
    g_atom_firing = JS_ATOM_NULL;
    JS_FreeValueRT(rt, g_firing_key);
    g_firing_key = JS_UNDEFINED;
}
