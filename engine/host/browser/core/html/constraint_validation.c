/* CONSTRAINT VALIDATION — HTML §4.10.21.
 *
 * §4.10.22.3 step 5.4 INTERACTIVELY VALIDATES the constraints of the form it is submitting, and a negative
 * result ends the submission: no `submit` event, no entry list, no request. So this is not a decoration on the
 * submission — it decides whether the request the solver is here to learn exists at all, and a control this
 * component reports as invalid removes an endpoint that a browser would also have refused.
 *
 * THE STATES ARE COMPUTED, NEVER ASSUMED. Each of §4.10.21.1's ten is stated per control type by the standard,
 * and every one of them is read here from the element's real value and its real constraint attributes:
 * `required` against the value/checkedness/selectedness the control actually has, `pattern` against a REGEXP
 * this engine compiles and RUNS, `min`/`max`/`step` against the state's OWN convert a string to a number (a
 * date's `min` is a date and a number's is a numeral — core/html/input_number.c). Two of the ten are
 * computed FALSE for a built-in control and that is an answer rather than a gap: `tooLong` and `tooShort` are
 * conditioned by §4.10.19.3 and §4.10.19.4 on the value having been "last changed by a USER EDIT", and
 * `badInput` is stated by every input state as a fact about what "the user interface is representing" — this
 * engine has no user, so the conditions are false, not skipped. The one exception is the Color state, whose
 * bad-input clause is about the VALUE and not the interface: it is "parsing it returns failure", the same CSS
 * Color 4 parse §4.10.5.1.14's value sanitization runs, and both ask core/css/css_color.c.
 *
 * AN UNKNOWN VALUE FORKS. A control whose value came from unknown external input (`input.value =
 * location.hash`) makes "does this control satisfy its constraints" a question with two feasible answers, and
 * both worlds are real: one submits the form and records the endpoint, the other is the browser refusing. That
 * is an outcome fork, not a guess — concretising it either way would silently delete one of them. */
#include <stdio.h>
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "libregexp.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/css/css_color.h"
#include "core/dom/attr_list.h"
#include "core/dom/node.h"
#include "core/events/event.h"
#include "core/events/event_target.h"
#include "core/html/constraint_validation.h"
#include "core/html/custom_elements.h"
#include "core/html/element_internals.h"
#include "core/html/html_form.h"
#include "core/html/input_number.h"
#include "core/html/input_value.h"
#include "core/idl_args.h"
#include "core/url/url.h"
#include "solver/concolic.h"

/* ---- the element reads this component makes ---------------------------------------------------------------- */

static lxb_dom_element_t *cv_elem(JSValueConst v)
{
    lxb_dom_node_t *n = node_of(v);
    return (n && n->type == LXB_DOM_NODE_TYPE_ELEMENT) ? lxb_dom_interface_element(n) : NULL;
}

/* IS THE ATTRIBUTE PRESENT — asked of §4.9's attribute LIST and not of its value, so a parsed `<input required>`
   (which the HTML parser stores with no value buffer) and `setAttribute('required','')` are the same attribute. */
static bool cv_has(lxb_dom_element_t *el, const char *name)
{
    return el != NULL && dom_attr_get_ns(el, NULL, name) != NULL;
}

static const char *cv_attr(lxb_dom_element_t *el, const char *name, size_t *plen)
{
    *plen = 0;
    if (!el) return NULL;
    return (const char *)lxb_dom_element_get_attribute(el, (const lxb_char_t *)name, strlen(name), plen);
}

static bool cv_tag_is(const lxb_dom_node_t *n, const char *name)
{
    size_t len = 0;
    const lxb_char_t *t;

    if (!n || n->type != LXB_DOM_NODE_TYPE_ELEMENT) return false;
    t = lxb_dom_element_local_name(lxb_dom_interface_element((lxb_dom_node_t *)n), &len);
    return t && len == strlen(name) && memcmp(t, name, len) == 0;
}

static uint32_t cv_array_len(JSContext *ctx, JSValueConst arr)
{
    JSValue lv = JS_GetPropertyStr(ctx, arr, "length");
    uint32_t n = 0;

    JS_ToUint32(ctx, &n, lv);
    JS_FreeValue(ctx, lv);
    return n;
}

/* ---- §4.10.21.1's CUSTOM VALIDITY ERROR MESSAGE --------------------------------------------------------------
 *
 * "An element can have a custom validity error message defined. Initially, an element must have its custom
 * validity error message set to the empty string. When its value is not the empty string, the element is
 * suffering from a custom error."
 * Held as an own slot on the element's wrapper under a Symbol this file minted and never published — so it is
 * per-flow for free (a slot written as a property write rides the COW delta), which it has to be: two forked
 * arms that each set a different message on the same control are two timelines, not one. */
static JSValue g_custom_key = JS_UNDEFINED;
static JSAtom  g_atom_custom = JS_ATOM_NULL;

/* The message, or NULL when the element has never been given one. OWNED when non-NULL. */
static JSValue cv_custom_message(JSContext *ctx, JSValueConst wrap)
{
    JSValue v;

    DCHECK(g_atom_custom != JS_ATOM_NULL,
           "§4.10.21.1's custom validity error message was read before constraint_validation_declare minted "
           "its slot key");
    if (!JS_IsObject(wrap)) return JS_UNDEFINED;
    if (JS_GetOwnSlot(ctx, &v, wrap, g_atom_custom) <= 0) return JS_UNDEFINED;
    return v;
}

static bool cv_suffering_custom_error(JSContext *ctx, JSValueConst wrap)
{
    JSValue m = cv_custom_message(ctx, wrap);
    size_t len = 0;
    const char *s;
    bool any;

    if (!JS_IsString(m)) { JS_FreeValue(ctx, m); return false; }
    s = JS_ToCStringLen(ctx, &len, m);
    any = s != NULL && len != 0;
    if (s) JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, m);
    return any;
}

/* ---- §4.10.21.1's CANDIDACY ----------------------------------------------------------------------------------
 *
 * "A submittable element is a candidate for constraint validation except when a condition has BARRED the
 * element from constraint validation." The conditions, each where the standard states it: §4.10.19.5 disabled;
 * §4.10.10's `datalist` ancestor; §4.10.5.3.3's `readonly` on an `input` and §4.10.11's on a `textarea`;
 * §4.10.5.1.1/.20/.21's Hidden, Reset Button and Button states; and §4.10.6's `button` that is not a submit
 * button. */
bool constraint_validation_is_candidate(JSContext *ctx, JSValueConst wrap)
{
    lxb_dom_node_t *n = node_of(wrap);
    lxb_dom_element_t *el = cv_elem(wrap);

    if (!el) return false;
    if (html_form_control_is_disabled(ctx, wrap)) return false;
    if (html_form_has_datalist_ancestor(wrap)) return false;
    if (cv_tag_is(n, "textarea")) return !cv_has(el, "readonly");
    if (cv_tag_is(n, "button")) return html_form_is_submit_button(ctx, wrap);
    if (cv_tag_is(n, "input")) {
        HtmlInputState st = html_form_input_state(n);

        if (cv_has(el, "readonly")) return false;
        return !(st == INPUT_STATE_HIDDEN || st == INPUT_STATE_RESET || st == INPUT_STATE_BUTTON);
    }
    return true;   /* a `select`, and a form-associated custom element */
}

/* §4.10.18.2's MUTABILITY, which §4.10.5.3.4's and §4.10.11's "suffering from being missing" both read: an
   `input` is mutable unless it is disabled (§4.10.5.1's "when an input element is disabled, it is not mutable")
   or carries `readonly`, and a `textarea` is "mutable if it is neither disabled nor has a readonly attribute
   specified". Both conditions also BAR the element, so inside the submission walk this is always true — it is
   asked anyway because §4.10.21.1 says an element can suffer from a state even while it is barred, and a
   `validity` reader is entitled to the same answer a browser gives. */
static bool cv_is_mutable(JSContext *ctx, JSValueConst wrap)
{
    return !html_form_control_is_disabled(ctx, wrap) && !cv_has(cv_elem(wrap), "readonly");
}

/* ---- §4.10.5.1.5's VALID EMAIL ADDRESS -----------------------------------------------------------------------
 *
 *   email   = 1*( atext / "." ) "@" label *( "." label )
 *   label   = let-dig [ [ ldh-str ] let-dig ]     ; limited to a length of 63 characters by RFC 1034 §3.5
 *
 * The ABNF the standard states, not the JavaScript regular expression it offers beside it as "an implementation
 * of the above definition" — the definition is what the constraint is, and every byte outside `atext` is
 * outside it, which is also why a non-ASCII byte fails here exactly as it fails the regexp's ASCII classes. */
static bool cv_is_atext(char c)
{
    /* `c != 0` first: strchr answers with the TERMINATOR for a NUL, and a JS string may hold one. */
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           (c != 0 && strchr("!#$%&'*+/=?^_`{|}~-", c) != NULL);
}

static bool cv_is_let_dig(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

static bool cv_is_label(const char *s, size_t len)
{
    size_t i;

    if (len < 1 || len > 63) return false;
    if (!cv_is_let_dig(s[0]) || !cv_is_let_dig(s[len - 1])) return false;
    for (i = 1; i + 1 < len; i++)
        if (!cv_is_let_dig(s[i]) && s[i] != '-') return false;
    return true;
}

static bool cv_is_valid_email(const char *s, size_t len)
{
    size_t at = 0, i, start;
    bool found = false;

    for (i = 0; i < len; i++)
        if (s[i] == '@') { at = i; found = true; break; }   /* the FIRST `@`: the local part admits none */
    if (!found || at == 0 || at + 1 >= len) return false;
    for (i = 0; i < at; i++)
        if (!cv_is_atext(s[i]) && s[i] != '.') return false;
    for (start = at + 1, i = at + 1; i <= len; i++) {
        if (i == len || s[i] == '.') {
            if (!cv_is_label(s + start, i - start)) return false;
            start = i + 1;
        }
    }
    return true;
}

/* ---- the control's VALUE, as the constraints read it ---------------------------------------------------------
 *
 * A value assigned by the page rides the element's per-flow slot as a JSValue, so it can be UNKNOWN EXTERNAL
 * INPUT rather than bytes — which is the point (`input.value = location.hash` submits an attacker source). A
 * constraint over one of those has no answer to read, and the caller forks instead. */
typedef struct {
    JSValue     val;      /* owned */
    const char *s;        /* the UTF-8 bytes, NULL when the value is unknown external input */
    size_t      len;
    bool        unknown;
} CvValue;

static void cv_value_read(JSContext *ctx, CvValue *v, JSValueConst wrap)
{
    v->val = html_form_control_value(ctx, wrap);
    v->s = NULL;
    v->len = 0;
    v->unknown = concolic_is(v->val) != 0;
    if (v->unknown) return;
    DCHECK(!JS_IsObject(v->val), "a form control's value slot holds an object — the `value` setter converts to "
                                 "a DOMString, so reading this one would run the page's toString from C");
    v->s = JS_ToCStringLen(ctx, &v->len, v->val);
}

static void cv_value_free(JSContext *ctx, CvValue *v)
{
    if (v->s) JS_FreeCString(ctx, v->s);
    JS_FreeValue(ctx, v->val);
    v->s = NULL;
    v->val = JS_UNDEFINED;
}

/* ---- §4.10.21.1's VALIDITY STATES, per control ---------------------------------------------------------------
 *
 * WHICH TYPES A CONSTRAINT ATTRIBUTE APPLIES TO is part of the constraint. Every input state lists the content
 * attributes that "must not be specified and do not apply to the element", and an attribute that does not apply
 * has no effect at all — which is why `<input type=range required>` is a valid control in a browser and would
 * be permanently missing if `required` were read off every input alike. */
static bool cv_required_applies(HtmlInputState st)
{
    switch (st) {
    case INPUT_STATE_TEXT: case INPUT_STATE_SEARCH: case INPUT_STATE_TEL: case INPUT_STATE_URL:
    case INPUT_STATE_EMAIL: case INPUT_STATE_PASSWORD: case INPUT_STATE_DATE: case INPUT_STATE_MONTH:
    case INPUT_STATE_WEEK: case INPUT_STATE_TIME: case INPUT_STATE_DATETIME_LOCAL: case INPUT_STATE_NUMBER:
    case INPUT_STATE_CHECKBOX: case INPUT_STATE_RADIO: case INPUT_STATE_FILE:
        return true;
    default:
        return false;
    }
}

/* §4.10.5.3.6's `pattern` applies to the states whose value is free text the author constrains: Text, Search,
   Telephone, URL, Email and Password. */
static bool cv_pattern_applies(HtmlInputState st)
{
    switch (st) {
    case INPUT_STATE_TEXT: case INPUT_STATE_SEARCH: case INPUT_STATE_TEL: case INPUT_STATE_URL:
    case INPUT_STATE_EMAIL: case INPUT_STATE_PASSWORD:
        return true;
    default:
        return false;
    }
}

/* §4.10.5.3.5's `multiple` applies to the Email state and the File Upload state; only the Email one has
   VALUES, which is what §4.10.5.3.6's second constraint and the type-mismatch list are stated over. */
static bool cv_multiple_values(JSContext *ctx, JSValueConst wrap)
{
    lxb_dom_node_t *n = node_of(wrap);

    (void)ctx;
    return html_form_input_state(n) == INPUT_STATE_EMAIL && cv_has(cv_elem(wrap), "multiple");
}

/* §4.10.5.1.16's RADIO BUTTON GROUP: every `input` in the Radio Button state, in the same tree, with the same
   form owner (or both none), whose non-empty `name` attributes are equal. `*pany_required` is the group's own
   required-ness, which is a property of the GROUP and not of the element — "if AN element in the radio button
   group is required, and all of the input elements in the group have a checkedness that is false". */
static void cv_radio_group(JSContext *ctx, JSValueConst wrap, bool *pany_required, bool *pany_checked)
{
    lxb_dom_node_t *n = node_of(wrap), *c, *root = node_root(node_of(wrap));
    lxb_dom_element_t *el = cv_elem(wrap);
    size_t namelen = 0, olen = 0;
    const char *name = cv_attr(el, "name", &namelen);
    JSValue owner = html_form_owner_of(ctx, wrap);

    *pany_required = cv_has(el, "required");
    *pany_checked = html_form_control_checked(ctx, wrap);
    if (!name || !namelen) { JS_FreeValue(ctx, owner); return; }   /* an unnamed radio's group is itself */
    for (c = root; c; ) {
        if (c != n && c->type == LXB_DOM_NODE_TYPE_ELEMENT && cv_tag_is(c, "input") &&
            html_form_input_state(c) == INPUT_STATE_RADIO) {
            lxb_dom_element_t *ce = lxb_dom_interface_element(c);
            const char *cname = cv_attr(ce, "name", &olen);

            if (cname && olen == namelen && memcmp(cname, name, namelen) == 0) {
                JSValue w = node_wrap(ctx, c), co = html_form_owner_of(ctx, w);

                if (JS_VALUE_GET_PTR(co) == JS_VALUE_GET_PTR(owner)) {
                    if (cv_has(ce, "required")) *pany_required = true;
                    if (html_form_control_checked(ctx, w)) *pany_checked = true;
                }
                JS_FreeValue(ctx, co);
                JS_FreeValue(ctx, w);
            }
        }
        if (c->first_child) { c = c->first_child; continue; }
        while (c && !c->next) c = (c == root) ? NULL : c->parent;
        c = c ? c->next : NULL;
    }
    JS_FreeValue(ctx, owner);
}

/* ---- §4.10.5.3.6's COMPILED PATTERN REGULAR EXPRESSION -------------------------------------------------------
 *
 * "Let regexpCompletion be RegExpCreate(pattern, "v"). If regexpCompletion is an ABRUPT COMPLETION, then return
 * nothing — the element has no compiled pattern regular expression." An unparseable pattern is therefore not a
 * validation failure; it is no constraint at all, and a control carrying one submits. Then: "let anchoredPattern
 * be "^(?:" followed by pattern followed by ")$"" and compile THAT — two compiles, and the standard says why in
 * as many words: the anchors must not be what makes an otherwise invalid pattern parse.
 *
 * IT IS THE ENGINE'S OWN REGEXP. lre_compile is the compiler RegExpCreate itself runs and lre_exec_step is the
 * executor RegExpBuiltinExec runs, reached at the layer the abstract operation is defined over rather than
 * through a `RegExp` global the page can replace. The match SUSPENDS at its back-edges like every other loop in
 * this engine — `pattern="(a+)+b"` against a long value is the ReDoS shape, and it is ordinary preemptible work
 * here rather than a thread given away. */
typedef struct CvMatcher {
    int      refs;      /* two forked arms share it: nothing writes it after it is built */
    uint8_t *bc;        /* the anchored pattern's compiled program */
    uint16_t *sub;      /* the value, as UTF-16 code units — the shape the executor indexes */
    size_t   units;
} CvMatcher;

static void cv_matcher_destroy(JSContext *ctx, void *p)
{
    CvMatcher *m = p;

    if (m->bc) lre_realloc(ctx, m->bc, 0);
    js_free(ctx, m->sub);
    js_free(ctx, m);
}

/* The UTF-8 the string conversion produced, as the code units the matcher indexes. There is no public accessor
   for a JSString's own units, and passing UTF-8 bytes as if they were characters would make `^.$` false for
   every non-ASCII value; this is that conversion and nothing more. */
static uint16_t *cv_utf16_of(JSContext *ctx, const char *s, size_t len, size_t *punits)
{
    uint16_t *out = js_malloc(ctx, (len + 1) * sizeof(uint16_t));
    size_t i = 0, k = 0;

    CHECK(out != NULL, "constraint validation: OOM converting a control's value for its pattern match");
    while (i < len) {
        unsigned char c = (unsigned char)s[i];
        uint32_t cp;
        size_t extra;

        if (c < 0x80)            { cp = c;        extra = 0; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1Fu; extra = 1; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0Fu; extra = 2; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07u; extra = 3; }
        else {
            DFAIL("a form control's value is not the UTF-8 the string conversion produces");
            break;
        }
        DCHECK(i + extra < len, "a truncated UTF-8 sequence reached the pattern matcher");
        for (; extra; extra--) cp = (cp << 6) | ((unsigned char)s[++i] & 0x3Fu);
        i++;
        if (cp >= 0x10000) {
            out[k++] = (uint16_t)(0xD800 + ((cp - 0x10000) >> 10));
            out[k++] = (uint16_t)(0xDC00 + ((cp - 0x10000) & 0x3FF));
        } else {
            out[k++] = (uint16_t)cp;
        }
    }
    *punits = k;
    return out;
}

/* The element's compiled pattern regular expression over `value`, or NULL when it has none. */
static CvMatcher *cv_matcher_new(JSContext *ctx, JSValueConst wrap, const char *value, size_t vlen)
{
    lxb_dom_element_t *el = cv_elem(wrap);
    size_t plen = 0;
    const char *pat = cv_attr(el, "pattern", &plen);
    char errbuf[128];
    uint8_t *raw, *anchored;
    int bclen = 0;
    char *joined;
    CvMatcher *m;

    if (!pat) return NULL;
    /* NUL-terminated, because the compiler is handed a C string everywhere else in the engine and an attribute
       store's buffer is not one this file may assume. */
    joined = js_malloc(ctx, plen + 7);
    CHECK(joined != NULL, "constraint validation: OOM building §4.10.5.3.6's anchored pattern");
    memcpy(joined, pat, plen);
    joined[plen] = 0;
    raw = lre_compile(&bclen, errbuf, sizeof errbuf, joined, plen, LRE_FLAG_UNICODE_SETS, ctx);
    if (!raw) {   /* an abrupt RegExpCreate: the element has no compiled pattern regular expression */
        js_free(ctx, joined);
        return NULL;
    }
    lre_realloc(ctx, raw, 0);
    memmove(joined + 4, pat, plen);
    memcpy(joined, "^(?:", 4);
    memcpy(joined + 4 + plen, ")$", 2);
    joined[plen + 6] = 0;
    anchored = lre_compile(&bclen, errbuf, sizeof errbuf, joined, plen + 6, LRE_FLAG_UNICODE_SETS, ctx);
    js_free(ctx, joined);
    DCHECK(anchored != NULL, "§4.10.5.3.6's anchored pattern failed to compile although the pattern itself "
                             "compiled — the standard's two-step compile exists exactly so this cannot happen");
    if (!anchored) return NULL;
    m = js_mallocz(ctx, sizeof *m);
    CHECK(m != NULL, "constraint validation: OOM allocating a compiled pattern");
    m->refs = 1;
    m->bc = anchored;
    m->sub = cv_utf16_of(ctx, value, vlen, &m->units);
    return m;
}

/* ---- §4.10.21.2's own state ---------------------------------------------------------------------------------- */

/* WHERE THIS SUB-SEQUENCE RESTS. Each phase is a step of the algorithm it is performing, stated as the standard
   writes it — the same declaration a step machine's stages carry, for the same reason: a phase is where the
   walk parks, where a sibling flow overtakes it and where a cold-tier resume picks it up. */
#define CV_PHASES(X) \
    X(CVR_COLLECT, "HTML §4.10.21.2 statically validate the constraints step 3 (for each field in controls, in " \
                   "tree order: a candidate for constraint validation that does not satisfy its constraints is " \
                   "added to invalid controls)") \
    X(CVR_FIRE,    "HTML §4.10.21.2 statically validate the constraints step 5.1 (fire an event named invalid " \
                   "at field, with the cancelable attribute initialized to true)") \
    X(CVR_REPORT,  "HTML §4.10.21.2 interactively validate the constraints step 3 (report the problems with " \
                   "the constraints of at least one of the elements in unhandled invalid controls to the user)")
enum { CV_PHASES(JS_STEP_STAGE_ENUM) };
static const char *const CV_PHASE_STEPS[] = { CV_PHASES(JS_STEP_STAGE_LABEL) NULL };

/* And where the per-FIELD question rests, which is step 3.2's "does field satisfy its constraints" broken at
   the two points inside it that can suspend: the outcome fork over an unknown value, and the pattern match. */
#define CV_FIELD_PHASES(X) \
    X(CVF_STATES, "HTML §4.10.21.1 (the validity states the element's value and constraint attributes decide)") \
    X(CVF_FORK,   "HTML §4.10.21.1 (satisfying the constraints is one of two feasible outcomes for a value " \
                  "that is unknown external input)") \
    X(CVF_MATCH,  "HTML §4.10.5.3.6 (the compiled pattern regular expression matched against the value)")
enum { CV_FIELD_PHASES(JS_STEP_STAGE_ENUM) };
static const char *const CV_FIELD_STEPS[] = { CV_FIELD_PHASES(JS_STEP_STAGE_LABEL) NULL };

struct ConstraintValidationRun {
    uint8_t   phase;       /* CVR_* */
    uint8_t   fphase;      /* step 5.1's fire request's own phase */
    uint8_t   fld_phase;   /* CVF_* — the per-field question's own cursor */
    uint8_t   matching;    /* `ec` is live and owes an lre_exec_end */
    uint32_t  i;           /* the cursor of whichever walk is running (steps 3 and 5) */
    uint32_t  flags;       /* the field's validity states, as this component's bits */
    uint8_t   unknown_bit; /* which state the unknown value decides, for the fork's negative arm */
    JSValue   controls;    /* step 1's list of submittable elements (owned) */
    JSValue   invalid;     /* step 2's invalid controls (owned) */
    JSValue   field;       /* the control step 3.2 is asking about, held across its suspensions (owned) */
    JSValue   value;       /* that control's value, held for the same reason (owned) */
    JSValue   ev;          /* step 5.1's `invalid` event (owned) */
    EventFireCb   cb;       /* the fire request buffer: [this, dispatch, target, event] */
    CvMatcher *m;          /* the compiled pattern and the subject, shared by reference */
    uint8_t  **capture;    /* the executor's capture/register block, this arm's own */
    int       cap_alloc;
    size_t    tok;         /* which of the element's `values` is being matched */
    size_t    tok_at;      /* where in the subject that value starts */
    REExecContext ec;
    char      fork_op[48];
};

static void cv_run_elem_visit(JSContext *ctx, void *elem, JSStepVisit *v)
{
    ConstraintValidationRun *r = elem;
    int k;

    v->val(ctx, &r->controls);
    v->val(ctx, &r->invalid);
    v->val(ctx, &r->field);
    v->val(ctx, &r->value);
    v->val(ctx, &r->ev);
    STEP_CB_FOREACH(r->cb, k) v->val(ctx, &r->cb[k]);
    v->shared(ctx, (void **)&r->m, r->m ? &r->m->refs : NULL, cv_matcher_destroy);
    /* ORDER: the capture block is copied first so the executor's context can be re-pointed at whichever copy it
       now belongs to. Its remaining pointers aim into the compiled program and the subject, and the matcher
       holding both is shared by reference, so both arms keep them alive unchanged. */
    v->buf(ctx, (void **)&r->capture, sizeof(uint8_t *) * (size_t)r->cap_alloc);
    if (r->matching) v->reexec(ctx, &r->ec, r->capture);
}

void constraint_validation_visit(JSContext *ctx, ConstraintValidationRun **slot, JSStepVisit *v)
{
    /* ONE operation and not a buffer copy plus a loop here, because the two consumers need OPPOSITE ORDER: the
       clone must copy the block before taking references into it, the teardown must release those references
       before freeing it. */
    v->array(ctx, (void **)slot, sizeof(ConstraintValidationRun), 1, 1, cv_run_elem_visit);
}

static void cv_field_end(JSContext *ctx, ConstraintValidationRun *r);

/* The calling machine's teardown. It releases what the declaration above names, in the same order and by hand,
   because that machine frees its own fields rather than going through the free visitor — an ABANDONED run (a
   flow dropped inside an `invalid` handler, or one whose handler threw) ends here and must leave no half-
   finished match holding a backtracking stack. */
void constraint_validation_release(JSContext *ctx, ConstraintValidationRun **slot)
{
    ConstraintValidationRun *r = *slot;
    int k;

    if (!r) return;
    cv_field_end(ctx, r);   /* the live match, the matcher, the capture block, the field and its value */
    JS_FreeValue(ctx, r->controls);
    JS_FreeValue(ctx, r->invalid);
    JS_FreeValue(ctx, r->ev);
    STEP_CB_FOREACH(r->cb, k) JS_FreeValue(ctx, r->cb[k]);
    js_free(ctx, r);
    *slot = NULL;
}

/* ---- step 3.2: does the field SATISFY ITS CONSTRAINTS ------------------------------------------------------- */

/* The nine states that are a function of the element's attributes and its value alone. `*punknown_bit` is set
   to the state whose answer depends on unknown external input, and -1 when every one of them is decided. */
static uint32_t cv_states(JSContext *ctx, JSValueConst wrap, const CvValue *v, int *punknown_bit,
                          bool *pneed_pattern)
{
    lxb_dom_node_t *n = node_of(wrap);
    lxb_dom_element_t *el = cv_elem(wrap);
    HtmlInputState st = html_form_input_state(n);
    uint32_t flags = 0;

    *punknown_bit = -1;
    *pneed_pattern = false;
    /* §4.10.21.2 STEP 3.2 FOR A FORM-ASSOCIATED CUSTOM ELEMENT: its validity is not computed from attributes
       at all — it is exactly what §4.13.7.3's `setValidity` wrote, so this answers from those bits and asks
       none of the questions below. The bits are the SAME ten constraint_validation.h declares, which is why
       element_internals.c now expands that list instead of its own copy: two spellings would let setValidity
       set the bit this reads as a different state. */
    if (custom_elements_is_form_associated(ctx, wrap))
        return element_internals_validity_flags(ctx, wrap);
    if (cv_suffering_custom_error(ctx, wrap)) flags |= 1u << CV_CUSTOM_ERROR;

    if (cv_tag_is(n, "select")) {
        /* §4.10.7: "if the element has its required attribute specified, and either none of the option elements
           have their selectedness set to true, or the only option with selectedness true is the PLACEHOLDER
           LABEL OPTION, then the element is suffering from being missing." */
        if (cv_has(el, "required")) {
            JSValue sel = html_form_selected_options(ctx, wrap);
            uint32_t nsel = cv_array_len(ctx, sel);
            bool missing = nsel == 0;

            if (nsel == 1) {
                JSValue only = JS_GetPropertyUint32(ctx, sel, 0);
                JSValue ph = html_form_placeholder_label_option(ctx, wrap);

                missing = JS_VALUE_GET_PTR(only) == JS_VALUE_GET_PTR(ph);
                JS_FreeValue(ctx, ph);
                JS_FreeValue(ctx, only);
            }
            if (missing) flags |= 1u << CV_VALUE_MISSING;
            JS_FreeValue(ctx, sel);
        }
        return flags;
    }
    if (cv_tag_is(n, "button")) return flags;   /* §4.10.6 constrains a button by nothing but a custom error */

    if (cv_tag_is(n, "textarea")) {
        /* §4.10.11: required, mutable, and the value is the empty string. */
        if (cv_has(el, "required") && cv_is_mutable(ctx, wrap)) {
            if (v->unknown) *punknown_bit = CV_VALUE_MISSING;
            else if (!v->len) flags |= 1u << CV_VALUE_MISSING;
        }
        return flags;
    }
    DCHECK(cv_tag_is(n, "input"), "a control that is not an input, select, textarea, button or form-associated "
                                  "custom element reached §4.10.21.1 — §4.10.2's submittable list has no "
                                  "sixth member");

    /* §4.10.5.1.15 and §4.10.5.1.16: the Checkbox and Radio Button states are about CHECKEDNESS, not a value. */
    if (st == INPUT_STATE_CHECKBOX) {
        if (cv_has(el, "required") && !html_form_control_checked(ctx, wrap)) flags |= 1u << CV_VALUE_MISSING;
        return flags;
    }
    if (st == INPUT_STATE_RADIO) {
        bool group_required = false, group_checked = false;

        cv_radio_group(ctx, wrap, &group_required, &group_checked);
        if (group_required && !group_checked) flags |= 1u << CV_VALUE_MISSING;
        return flags;
    }
    if (st == INPUT_STATE_FILE) {
        /* §4.10.5.1.17: "If the element is required and the list of selected files is empty, then the element
           is suffering from being missing." BOTH conjuncts, and the second is a real question now that
           input_value.c holds the list: `required` alone answered missing for every file control, so a form
           whose picker HAD selected a file still refused to submit. There is no mutability conjunct here —
           §4.10.5.3.4's has one and this does not — so it is not written in. */
        if (cv_has(el, "required") && input_files_count(ctx, wrap) == 0) flags |= 1u << CV_VALUE_MISSING;
        return flags;
    }
    if (st == INPUT_STATE_COLOR) {
        /* §4.10.5.1.14: "While the element's value is not the empty string and PARSING IT returns failure, the
           control is suffering from bad input." That parse is CSS Color 4's `parse a CSS <color> value` — the
           same one §4.10.5.1.14's sanitization runs — so the two answer from one component and cannot drift:
           a value the sanitizer serialized is by construction one this parse accepts, and a value that reaches
           here unparseable is one the sanitizer never saw. Returning here is the state's own answer and not a
           shortcut: §4.10.5.1.14 lists `required`, `pattern`, `min`, `max`, `step`, `maxlength` and `minlength`
           among the attributes that do not apply to it, so no constraint below is stated over a colour. */
        if (v->unknown) { if (*punknown_bit < 0) *punknown_bit = CV_BAD_INPUT; }
        else if (v->len) {
            CssColor c;

            if (!css_color_parse(v->s, v->len, &c)) flags |= 1u << CV_BAD_INPUT;
        }
        return flags;
    }

    /* §4.10.5.3.4: required, its `value` IDL attribute applies and is in the mode VALUE, the element is
       mutable, and the value is the empty string. Every state left here is in mode value. */
    if (cv_has(el, "required") && cv_required_applies(st) && cv_is_mutable(ctx, wrap)) {
        if (v->unknown) *punknown_bit = CV_VALUE_MISSING;
        else if (!v->len) flags |= 1u << CV_VALUE_MISSING;
    }
    /* §4.10.5.1.4 and §4.10.5.1.5's TYPE MISMATCH. Both are stated over the whole value, and the Email state's
       `multiple` form is stated over the list of comma-separated tokens instead. */
    if (st == INPUT_STATE_URL) {
        if (v->unknown) { if (*punknown_bit < 0) *punknown_bit = CV_TYPE_MISMATCH; }
        else if (v->len) {
            UrlRecord u;
            bool ok;

            url_record_init(&u);
            /* "neither the empty string nor a VALID ABSOLUTE URL" — and an absolute URL is exactly what the URL
               parser succeeds on with no base to resolve against. */
            ok = url_parse(&u, v->s, v->len, NULL);
            url_record_free(&u);
            if (!ok) flags |= 1u << CV_TYPE_MISMATCH;
        }
    } else if (st == INPUT_STATE_EMAIL) {
        if (v->unknown) { if (*punknown_bit < 0) *punknown_bit = CV_TYPE_MISMATCH; }
        else if (v->len) {
            if (!cv_has(el, "multiple")) {
                if (!cv_is_valid_email(v->s, v->len)) flags |= 1u << CV_TYPE_MISMATCH;
            } else {
                /* §4.10.5.1.5's VALID EMAIL ADDRESS LIST: a set of comma-separated tokens each of which is a
                   valid email address. Splitting on commas strips each token's leading and trailing ASCII
                   whitespace, which is why `a@b, c@d` is a conforming list. */
                size_t start = 0, i;

                for (i = 0; i <= v->len; i++) {
                    if (i != v->len && v->s[i] != ',') continue;
                    {
                        size_t a = start, b = i;

                        while (a < b && (v->s[a] == '\t' || v->s[a] == '\n' || v->s[a] == '\f' ||
                                         v->s[a] == '\r' || v->s[a] == ' ')) a++;
                        while (b > a && (v->s[b - 1] == '\t' || v->s[b - 1] == '\n' || v->s[b - 1] == '\f' ||
                                         v->s[b - 1] == '\r' || v->s[b - 1] == ' ')) b--;
                        if (!cv_is_valid_email(v->s + a, b - a)) flags |= 1u << CV_TYPE_MISMATCH;
                    }
                    start = i + 1;
                }
            }
        }
    }
    /* §4.10.5.3.7's UNDERFLOW and OVERFLOW and §4.10.5.3.8's STEP MISMATCH — ONE branch over the SEVEN states
       that define a "convert a string to a number", because that is how the standard states them: three
       sentences that name the algorithm and never a state, so a date and a number are the same constraint over
       different arithmetic. core/html/input_number.c is that arithmetic and the domain it defines. */
    if (input_number_applies(st)) {
        if (v->unknown) { if (*punknown_bit < 0) *punknown_bit = CV_RANGE_OVERFLOW; }
        else {
            double x;
            InputStepRange rg;

            input_step_range_of(el, st, &rg);
            /* "…and the result of applying the algorithm to convert a string to a number to the string given by
               the element's value IS A NUMBER" — a value that is not one leaves all three constraints
               unsatisfiable, which is what that clause says and why the empty value is never out of range. */
            if (v->len && input_number_from_string(st, v->s, v->len, &x)) {
                if (rg.reversed) {
                    /* "When an element has a REVERSED RANGE, and ... the number obtained from that algorithm is
                       more than the maximum AND less than the minimum, the element is simultaneously suffering
                       from an underflow and suffering from an overflow." The range spans midnight, so what is
                       out of it is the interval BETWEEN the two bounds rather than what lies outside them —
                       `min=21:00 max=06:00` admits 23:00 and refuses 12:00. */
                    if (x > rg.max && x < rg.min)
                        flags |= (1u << CV_RANGE_UNDERFLOW) | (1u << CV_RANGE_OVERFLOW);
                } else {
                    if (rg.has_min && x < rg.min) flags |= 1u << CV_RANGE_UNDERFLOW;
                    if (rg.has_max && x > rg.max) flags |= 1u << CV_RANGE_OVERFLOW;
                }
                if (input_step_mismatch(x, &rg)) flags |= 1u << CV_STEP_MISMATCH;
            }
        }
    }
    /* §4.10.5.3.6's PATTERN MISMATCH is decided by running a regexp, which is the caller's next phase. An
       unknown value has no bytes to run it against, and the fork above already covers it. */
    if (cv_pattern_applies(st) && !v->unknown && v->len && cv_has(el, "pattern")) *pneed_pattern = true;
    else if (cv_pattern_applies(st) && v->unknown && cv_has(el, "pattern") && *punknown_bit < 0)
        *punknown_bit = CV_PATTERN_MISMATCH;
    /* §4.10.19.3's TOO LONG and §4.10.19.4's TOO SHORT are conditioned on the value having been "last changed
       by a USER EDIT (as opposed to a change made by a script)". Nothing in this engine is a user edit — there
       is no user — so both are false for every built-in control, computed rather than skipped. The one site
       that will set that flag is the input event's own value change, and it does not exist. */
    return flags;
}

/* Begin the match of one of the element's values. Returns false when there is nothing left to match. */
static bool cv_match_begin(JSContext *ctx, ConstraintValidationRun *r, bool multiple)
{
    size_t a, b, i;

    if (!r->m || r->tok_at > r->m->units) return false;
    if (!multiple) {
        if (r->tok) return false;
        a = 0;
        b = r->m->units;
    } else {
        a = r->tok_at;
        if (a > r->m->units) return false;
        for (i = a; i < r->m->units && r->m->sub[i] != ','; i++) ;
        b = i;
        r->tok_at = i + 1;   /* past the comma; one past the end when this was the last token */
        while (a < b && (r->m->sub[a] == '\t' || r->m->sub[a] == '\n' || r->m->sub[a] == '\f' ||
                         r->m->sub[a] == '\r' || r->m->sub[a] == ' ')) a++;
        while (b > a && (r->m->sub[b - 1] == '\t' || r->m->sub[b - 1] == '\n' || r->m->sub[b - 1] == '\f' ||
                         r->m->sub[b - 1] == '\r' || r->m->sub[b - 1] == ' ')) b--;
    }
    r->tok++;
    if (!r->capture) {
        /* The executor writes capture positions AND its temporary registers into this block, so it is sized by
           the program's alloc count and not by its capture count. */
        r->cap_alloc = lre_get_alloc_count(r->m->bc);
        if (r->cap_alloc > 0) {
            r->capture = js_malloc(ctx, sizeof(uint8_t *) * (size_t)r->cap_alloc);
            CHECK(r->capture != NULL, "constraint validation: OOM allocating a pattern match's capture block");
        }
    }
    /* The subject starts AT the token, because `^` anchors to the start of the buffer the executor was given —
       which is exactly what makes one compiled anchored pattern serve every one of the element's values. */
    lre_exec_begin(&r->ec, r->capture, r->m->bc, (const uint8_t *)(r->m->sub + a), 0, (int)(b - a), 1, ctx);
    r->matching = 1;
    return true;
}

static void cv_field_end(JSContext *ctx, ConstraintValidationRun *r)
{
    if (r->matching) { lre_exec_end(&r->ec); r->matching = 0; }
    if (r->m && --r->m->refs == 0) cv_matcher_destroy(ctx, r->m);
    r->m = NULL;
    js_free(ctx, r->capture);
    r->capture = NULL;
    r->cap_alloc = 0;
    r->tok = 0;
    r->tok_at = 0;
    r->flags = 0;
    r->fld_phase = CVF_STATES;
    JS_FreeValue(ctx, r->field);
    JS_FreeValue(ctx, r->value);
    r->field = JS_UNDEFINED;
    r->value = JS_UNDEFINED;
}

/* Step 3.2 for the field the run is standing on. Returns JS_STEP_YIELD / JS_STEP_FORK (the caller returns it),
   -1 with a throw live, or 0 once `r->flags` is the field's validity states. */
static int cv_field_step(JSContext *ctx, JSStepHdr *h, ConstraintValidationRun *r)
{
    bool multiple = cv_multiple_values(ctx, r->field);

    if (r->fld_phase == CVF_STATES) {
        CvValue v;
        int unknown_bit = -1;
        bool need_pattern = false;

        cv_value_read(ctx, &v, r->field);
        JS_FreeValue(ctx, r->value);
        r->value = JS_DupValue(ctx, v.val);
        r->flags = cv_states(ctx, r->field, &v, &unknown_bit, &need_pattern);
        if (need_pattern) r->m = cv_matcher_new(ctx, r->field, v.s, v.len);
        cv_value_free(ctx, &v);
        if (unknown_bit >= 0) {
            r->unknown_bit = (uint8_t)unknown_bit;
            r->fld_phase = CVF_FORK;
        } else if (r->m && cv_match_begin(ctx, r, multiple)) {
            r->fld_phase = CVF_MATCH;
        } else {
            return 0;
        }
    }
    if (r->fld_phase == CVF_FORK) {
        int arm = 0, rc;

        /* The position is part of the predicate: two controls fed from the same source are two independent
           questions, and one answer replayed for both would delete one of the four worlds. */
        snprintf(r->fork_op, sizeof r->fork_op, "satisfies-constraints>%u", (unsigned)r->i);
        rc = step_fork_run(ctx, h, r->value, r->fork_op, 2, JS_OUTCOME_REAL_UNSTATED, &arm);
        if (rc) return rc;
        /* OUTCOME 0 is the ordinary completion — the control satisfies its constraints and the submission goes
           on to record its request, which is the path an @S candidate re-fire must not be diverted from. */
        if (arm) r->flags |= 1u << r->unknown_bit;
        return 0;
    }
    DCHECK(r->fld_phase == CVF_MATCH,
           "the per-field constraint check resumed into a phase §4.10.21.1 does not have");
    DCHECK(CV_FIELD_STEPS[CVF_MATCH] != NULL, "the per-field phase list lost the step its last phase rests at");
    for (;;) {
        int rc = lre_exec_step(&r->ec);

        if (rc == LRE_RET_YIELD) return JS_STEP_YIELD;
        lre_exec_end(&r->ec);
        r->matching = 0;
        CHECK(rc >= 0, "a pattern match failed in the regexp executor — LRE_RET_MEMORY_ERROR is an allocation "
                       "failure and LRE_RET_BYTECODE_ERROR is a corrupted program");
        if (rc != 1) {
            /* "the element has a compiled pattern regular expression but that regular expression does not match
               the element's value" — and for the `multiple` form, does not match EACH of its values. */
            r->flags |= 1u << CV_PATTERN_MISMATCH;
            return 0;
        }
        if (!cv_match_begin(ctx, r, multiple)) return 0;
    }
}

/* ---- §4.10.21.2 ---------------------------------------------------------------------------------------------- */

int constraint_validation_interactively_run(JSContext *ctx, JSStepHdr *h, ConstraintValidationRun **slot,
                                            JSValueConst form, JSValue in, bool *ppositive,
                                            JSValue **out_cb, int *out_argc)
{
    ConstraintValidationRun *r = *slot;
    int rc;

    if (!r) {
        int k;

        r = js_mallocz(ctx, sizeof *r);
        CHECK(r != NULL, "constraint validation: OOM allocating §4.10.21.2's own state");
        *slot = r;
        /* EVERY owned field before anything that can fail: an abandoned run is released through the one
           declaration, which frees exactly what the state holds and nothing else. */
        r->controls = r->invalid = r->field = r->value = r->ev = JS_UNDEFINED;
        STEP_CB_FOREACH(r->cb, k) r->cb[k] = JS_UNDEFINED;
        r->phase = CVR_COLLECT;
        r->fld_phase = CVF_STATES;
        /* Step 1: "let controls be a list of all the submittable elements whose form owner is form, in tree
           order" — the same sentence §4.10.22.3 step 5.3 walks, read again HERE because the page's code ran in
           between and may have moved a control out of the form. */
        r->controls = html_form_submittable_controls(ctx, form);
        r->invalid = JS_NewArray(ctx);   /* step 2 */
        CHECK(!JS_IsException(r->invalid), "constraint validation: OOM building the invalid-controls list");
    }
    if (r->phase == CVR_COLLECT) {
        uint32_t n = cv_array_len(ctx, r->controls);

        while (r->i < n) {
            if (JS_IsUndefined(r->field)) {
                r->field = JS_GetPropertyUint32(ctx, r->controls, r->i);
                if (!constraint_validation_is_candidate(ctx, r->field)) {   /* step 3.1 */
                    cv_field_end(ctx, r);
                    r->i++;
                    return JS_STEP_YIELD;   /* a walk of the PAGE's size offers a suspend point per element */
                }
            }
            rc = cv_field_step(ctx, h, r);                                  /* step 3.2 */
            if (rc > 0) return rc;
            if (rc < 0) return -1;
            if (r->flags != 0)                                              /* step 3.3 */
                JS_SetPropertyUint32(ctx, r->invalid, cv_array_len(ctx, r->invalid),
                                     JS_DupValue(ctx, r->field));
            cv_field_end(ctx, r);
            r->i++;
            return JS_STEP_YIELD;
        }
        if (cv_array_len(ctx, r->invalid) == 0) {   /* step 4: a positive result */
            *ppositive = true;
            return 0;
        }
        r->i = 0;
        r->phase = CVR_FIRE;
    }
    if (r->phase == CVR_FIRE) {
        uint32_t n = cv_array_len(ctx, r->invalid);

        while (r->i < n) {
            bool not_canceled = true;
            JSValue field = JS_GetPropertyUint32(ctx, r->invalid, r->i);

            if (JS_IsUndefined(r->ev)) {
                r->ev = event_new(ctx, "invalid", /*bubbles*/ false, /*cancelable*/ true);
                if (JS_IsException(r->ev)) { r->ev = JS_UNDEFINED; JS_FreeValue(ctx, field); return -1; }
            }
            /* Step 5.1's dispatch, as a REQUEST, so the page's `invalid` handlers run as ordinary preemptible
               code and this resumes once every one of them has returned. */
            rc = event_target_fire_run(ctx, &r->fphase, STEP_CB(r->cb), field, r->ev, JS_UNDEFINED, in,
                                       &not_canceled, out_cb, out_argc);
            JS_FreeValue(ctx, field);
            in = JS_UNDEFINED;
            if (rc > 0) return rc;
            if (rc < 0) return -1;
            /* Step 5.2 adds the field to UNHANDLED INVALID CONTROLS when the event was not canceled — the list
               step 6 returns and interactive validation's step 3 reports. A canceled `invalid` is a script
               claiming responsibility for the control, which changes what a user agent PRESENTS and not the
               result, and this engine's presentation is the phase below. */
            (void)not_canceled;
            JS_FreeValue(ctx, r->ev);
            r->ev = JS_UNDEFINED;
            r->fphase = 0;   /* the next control's dispatch is its own request, not this one resumed */
            r->i++;
            return JS_STEP_YIELD;
        }
        r->phase = CVR_REPORT;
    }
    DCHECK(r->phase == CVR_REPORT, "§4.10.21.2 resumed into a phase it does not have");
    DCHECK(CV_PHASE_STEPS[CVR_REPORT] != NULL, "the phase list lost the step its last phase rests at");
    /* Interactive validation's step 3 is "report the problems with the constraints of at least one of the
       elements in unhandled invalid controls to the USER", which is a presentation with no scriptable result:
       a focus, a scroll, a bubble. This engine has no user to present to — a missing IO device, not a missing
       behaviour — and step 4 returns the negative result either way, which IS what step 5.4 examines. */
    *ppositive = false;
    return 0;
}

/* ---- §4.10.21.3's members ------------------------------------------------------------------------------------
 *
 * `willValidate` and `setCustomValidity` are the two of them that compute an answer with no walk and no page
 * code in them. The rest of the interface — `validity`, `validationMessage`, `checkValidity()`,
 * `reportValidity()` — is honestly ABSENT here rather than stubbed: each of the first two answers with a
 * ValidityState or with the message that object would explain, and the class that object belongs to is
 * element_internals.c's, which exports no way to mint one for a built-in control. They land in the same diff
 * that adds the accessor the DFAIL above names. */
static int g_id_set_custom = -1;

enum { CV_WILL_VALIDATE = 0 };

static JSValue js_cv_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    DCHECK(magic == CV_WILL_VALIDATE, "a §4.10.21.3 accessor ran with a magic this component does not declare");
    return JS_NewBool(ctx, constraint_validation_is_candidate(ctx, this_val));
}

/* "The setCustomValidity(error) method steps are: set error to the result of NORMALIZING NEWLINES given error;
   set the custom validity error message to error." Infra's normalization is CRLF and lone CR to LF. */
static JSValue js_cv_set_custom_validity(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                         int magic)
{
    size_t len = 0, i, k = 0;
    const char *s;
    char *norm;
    JSValue out;

    (void)magic;
    DCHECK(argc >= 1, "setCustomValidity reached its steps with no argument — Web IDL's own count check throws "
                      "before this and it did not run");
    if (!cv_elem(this_val)) return JS_ThrowTypeError(ctx, "setCustomValidity was called on something that is "
                                                          "not a form control");
    s = JS_ToCStringLen(ctx, &len, argv[0]);   /* already a DOMString: the declaration converted it */
    if (!s) return JS_EXCEPTION;
    norm = js_malloc(ctx, len + 1);
    CHECK(norm != NULL, "constraint validation: OOM normalizing a custom validity error message");
    for (i = 0; i < len; i++) {
        if (s[i] == '\r') {
            norm[k++] = '\n';
            if (i + 1 < len && s[i + 1] == '\n') i++;
        } else {
            norm[k++] = s[i];
        }
    }
    JS_FreeCString(ctx, s);
    out = JS_NewStringLen(ctx, norm, k);
    js_free(ctx, norm);
    if (JS_IsException(out)) return JS_EXCEPTION;
    /* CONFIGURABLE AND WRITABLE: the message is written again at every call, and a slot defined with no flags
       makes the second write a silent no-op. */
    JS_DefinePropertyValue(ctx, (JSValue)this_val, g_atom_custom, out,
                           JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE);
    return JS_UNDEFINED;
}

void constraint_validation_declare(JSContext *ctx)
{
    static const IdlArgType ONE_STR[1] = { IDL_DOMSTRING };

    DCHECK(JS_IsUndefined(g_custom_key), "constraint_validation_declare ran twice — one instance is one agent");
    g_custom_key = JS_NewSymbol(ctx, "customValidityErrorMessage", false);
    CHECK(!JS_IsException(g_custom_key), "the custom-validity slot key allocation failed");
    g_atom_custom = JS_ValueToAtom(ctx, g_custom_key);
    CHECK(g_atom_custom != JS_ATOM_NULL, "the custom-validity slot key could not be interned");
    g_id_set_custom = idl_method_id(ctx, ONE_STR, 1, js_cv_set_custom_validity, 0);
}

void constraint_validation_install(JSContext *ctx, JSValueConst input_proto, JSValueConst textarea_proto)
{
    DCHECK(g_id_set_custom >= 0, "§4.10.21.3's members were installed before they were declared");
    idl_install_accessor(ctx, input_proto, "willValidate", js_cv_get, CV_WILL_VALIDATE, -1);
    idl_install_accessor(ctx, textarea_proto, "willValidate", js_cv_get, CV_WILL_VALIDATE, -1);
    idl_install_method(ctx, input_proto, "setCustomValidity", g_id_set_custom);
    idl_install_method(ctx, textarea_proto, "setCustomValidity", g_id_set_custom);
}

void constraint_validation_free(JSRuntime *rt)
{
    /* The slot key is the AGENT's — a Symbol nobody frees is a live GC object the runtime's own walk counts as
       a leak. */
    JS_FreeAtomRT(rt, g_atom_custom);
    g_atom_custom = JS_ATOM_NULL;
    JS_FreeValueRT(rt, g_custom_key);
    g_custom_key = JS_UNDEFINED;
    g_id_set_custom = -1;
}
