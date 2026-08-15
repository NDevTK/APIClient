/* AN INPUT'S VALUE — HTML §4.10.5.1's VALUE SANITIZATION ALGORITHM, and §4.10.5.4's four VALUE MODES.
 *
 * WHEN THE ALGORITHM RUNS — the part that decides whether this is a component or a smear. The standard does NOT
 * sanitize at read time. It runs the algorithm at four named moments, over the ELEMENT'S VALUE:
 *
 *   1. when the element is first created (§4.10.5.1, "When an input element is first created ... the value
 *      sanitization algorithm, if one is defined for the type attribute's state, must be invoked");
 *   2. when the `value` CONTENT ATTRIBUTE is added, set or removed AND the dirty value flag is false — the
 *      element's value is set to that attribute's value (or the empty string) and the algorithm is then run;
 *   3. when the `type` attribute CHANGES STATE — after the three value-mode transfer branches, "invoke the
 *      value sanitization algorithm, if one is defined for the type attribute's new state"; and the Email
 *      state adds one of its own ("when the multiple attribute is set or removed, the user agent must run the
 *      value sanitization algorithm");
 *   4. §4.10.5.4's `value` IDL setter in mode VALUE — set the value, set the DIRTY VALUE FLAG, then sanitize;
 *      and §4.10.5.1's reset algorithm, which clears the flag first.
 *
 * MOMENT 4 IS THIS COMPONENT'S OWN MEMBER and is built here: the assignment is sanitized AT THE ASSIGNMENT and
 * the result is what the element STORES, so nothing downstream sanitizes a second time.
 *
 * MOMENTS 1-3 ALL HANG OFF ONE PLACE — §4.9's ATTRIBUTE CHANGE STEPS, whose chokepoint is `element_attr_changed`
 * in core/dom/element.c, beside the three hooks that already fire from it. This component cannot register there,
 * so each is answered on its own terms rather than by one shrug:
 *
 *   - MOMENTS 1 and 2 (creation, and the `value` content attribute) are the SAME function. Moment 2 re-derives
 *     the element's value from that attribute at every change to it while the dirty value flag is false, so
 *     between those changes the element's value IS sanitize(state, attribute) — not approximately, identically.
 *     Computing it where the value is asked for is therefore the trigger's own result and not a lazy stand-in.
 *   - MOMENT 3 (the type change) is NOT expressible that way, because the value the type-change steps sanitize
 *     is the one the PREVIOUS state's algorithm left, and two algorithms compose. Within one value mode this
 *     file composes them for real — a stored value is re-sanitized and RE-STORED the first time it is read in a
 *     state it was not sanitized in. Across modes the steps also MOVE the value between the element and its
 *     content attribute, which nothing here can have done, so that case CRASHES naming the site to build. What
 *     makes both possible is the one extra fact recorded beside the value: the state it was sanitized in.
 *
 * THE DIRTY VALUE FLAG is what decides which of the two candidates the getter answers — the element's own value,
 * or the content attribute. Once a script assigns, the flag is true and the attribute is the DEFAULT, which is
 * why a form reset restores it and why `defaultValue` reflects it.
 *
 * AND THE FILENAME MODE IS A PROJECTION OF §4.10.5.1.17's LIST OF SELECTED FILES, which is why that list lives
 * here beside the value it is read through. `input.value` on a file control IS "C:\fakepath\" plus the first
 * selected file's name, `input.value = ""` EMPTIES that list, and `input.files` is the list itself — one piece
 * of state seen three ways, and split across two components it would be three answers that stop agreeing.
 * The list was hardcoded empty, with a comment here saying the engine "has no file picker" and so could take
 * no other branch. That was a claim about a DEVICE, not about the algorithm: the list, the FileList over it
 * (File API §5), the `accept`/`multiple` filter and §4.10.5.1.17's UPDATE THE FILE SELECTION are all defined
 * without one, and what a headless engine actually lacks is bytes to select — which core/file/file_device.c
 * models the way storage.c models a disk. So a file control now HAS a list, empty until something selects into
 * it through input_files_pick, and every reader of it reads the real thing. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "quickjs.h"
#include "core/css/css_color.h"
#include "core/dom/attr_list.h"
#include "core/dom/node.h"
#include "core/events/event.h"
#include "core/events/event_target.h"
#include "core/file/blob.h"
#include "core/file/file_device.h"
#include "core/file/file_list.h"
#include "core/html/date_time_microsyntax.h"
#include "core/html/html_form.h"
#include "core/html/input_number.h"
#include "core/html/input_value.h"
#include "core/html/number_microsyntax.h"
#include "core/idl_args.h"
#include "solver/attr_shadow.h"
#include "solver/concolic.h"
#include "solver/dom_cow.h"

/* The three per-element slots this component keeps, in the per-flow property shadow the control's value already
   used — so all three TIME-TRAVEL: a forked arm's assignment, the state it was sanitized under, and the files
   it selected are that flow's, and its sibling sees the markup's. */
#define IV_VALUE  "value"            /* §4.10.5.1's ELEMENT'S VALUE, already sanitized */
#define IV_STATE  "valueSanitizedAs" /* the `type` state that value was sanitized in — see the header comment */
#define IV_FILES  "selectedFiles"    /* §4.10.5.1.17's LIST OF SELECTED FILES, as the FileList that IS one */

/* §4.10.5.4's `files` setter, declared once per AGENT — see input_value_declare. */
static int g_id_files_set = -1;

/* §4.10.5.1's DIRTY VALUE FLAG, which is not a second slot beside the value but the SAME FACT: the flag is set
   to true by exactly the operation that gives the element a value of its own (§4.10.5.4's value-mode setter,
   and a user edit this engine has none of), and set back to false by exactly the operations that give the value
   back to the `value` content attribute (the reset algorithm, and the type-change branch that re-reads the
   attribute) — both of which drop the element's own value in the same step. So "the element holds a value" and
   "the dirty value flag is true" are one predicate, and stored twice they would be two that stop agreeing.
   Answers the INDEX of that value in the property shadow, or -1 while the flag is false: one lookup, because
   the flag and the value it guards are one entry and a second lookup could only disagree with the first. */
static int iv_dirty(lxb_dom_element_t *el)
{
    return attr_shadow_find(el, ATTR_SLOT_PROPERTY, NULL, IV_VALUE);
}

HtmlInputValueMode input_value_mode(HtmlInputState st)
{
    switch (st) {
    /* §4.10.5.1.1 and §4.10.5.1.18/.19/.20/.21 — Hidden and the four button states. */
    case INPUT_STATE_HIDDEN: case INPUT_STATE_SUBMIT: case INPUT_STATE_IMAGE:
    case INPUT_STATE_RESET:  case INPUT_STATE_BUTTON:
        return INPUT_VALUE_MODE_DEFAULT;
    /* §4.10.5.1.15 and §4.10.5.1.16 — Checkbox and Radio Button, whose default is a WORD and not the empty
       string, which is why §4.10.22.4 step 5.7 submits `on`. */
    case INPUT_STATE_CHECKBOX: case INPUT_STATE_RADIO:
        return INPUT_VALUE_MODE_DEFAULT_ON;
    case INPUT_STATE_FILE:                                   /* §4.10.5.1.17 */
        return INPUT_VALUE_MODE_FILENAME;
    default:
        /* Every remaining state — the text family, the date/time family, Number, Range and Color. */
        return INPUT_VALUE_MODE_VALUE;
    }
}

/* -1 when nothing has recorded a state for this element's value yet. */
static int iv_recorded_state(JSContext *ctx, lxb_dom_element_t *el)
{
    int i = attr_shadow_find(el, ATTR_SLOT_PROPERTY, NULL, IV_STATE);
    int32_t st = 0;

    if (i < 0) return -1;
    JS_ToInt32(ctx, &st, attr_shadow_opaque(i));
    return (int)st;
}

static void iv_record_state(JSContext *ctx, lxb_dom_element_t *el, HtmlInputState st)
{
    JSValue v = JS_NewInt32(ctx, (int)st);

    dom_cow_set_prop_taint(ctx, el, IV_STATE, v);   /* BORROWED by the shadow, which dups it */
    JS_FreeValue(ctx, v);
}

/* THE MISSING TRIGGER, NAMED WHERE IT BECOMES AN ANSWER. Moments 1-3 of §4.10.5.1 all fire from §4.9's
   attribute change steps, and this component has no registration on that chokepoint — so a `type` change runs
   none of them. Two things then go wrong and they are different: within ONE value mode the new state's
   sanitization algorithm never re-ran (recoverable here, because sanitizing the stored value under the state
   that is now current IS the composition the type-change steps performed); ACROSS modes the steps also MOVE the
   value — into the `value` content attribute on the way out of mode value, out of it on the way in, and to the
   empty string on the way into filename mode — and there is nothing here that can have done that. */
static void iv_check_type_change(JSContext *ctx, lxb_dom_element_t *el, HtmlInputState st)
{
    int was = iv_recorded_state(ctx, el);

    if (was < 0 || (HtmlInputState)was == st) return;
    if (input_value_mode((HtmlInputState)was) == input_value_mode(st)) return;
    DFAIL("an input's `type` changed between two states whose `value` IDL attribute is in DIFFERENT modes while "
          "the element held a value — §4.10.5.1's TYPE CHANGE steps transfer that value (out to the `value` "
          "content attribute when the new state is in the default or default/on mode, in from it when the new "
          "state is in the value mode, and to the empty string when the new state is in the filename mode) and "
          "then invoke the new state's value sanitization algorithm. Those steps have no site in this engine: "
          "build them, together with §4.10.5.1's `value` content attribute steps and the Email state's "
          "`multiple` trigger, on §4.9's attribute change steps in core/dom/element.c's element_attr_changed, "
          "beside custom_elements_attribute_changed and slot_attribute_changed");
}

/* §4.10.5.4's default and default/on getters read the `value` CONTENT ATTRIBUTE. Presence is a question about
   §4.9's attribute LIST and not about the value pointer — a parsed `<input value>` has no value buffer — and
   the taint shadow is consulted at the attribute's own identity so a source stored through `setAttribute` is
   still a source when it is read back. `fallback` is what an absent attribute answers. OWNED. */
static JSValue iv_content_attr(JSContext *ctx, lxb_dom_element_t *el, const char *fallback)
{
    size_t len = 0;
    const char *a;
    int si;

    if (!dom_attr_get_ns(el, NULL, "value")) return JS_NewString(ctx, fallback);
    si = attr_shadow_find(el, ATTR_SLOT_ATTRIBUTE, NULL, "value");
    if (si >= 0) return JS_DupValue(ctx, attr_shadow_opaque(si));
    a = (const char *)lxb_dom_element_get_attribute(el, (const lxb_char_t *)"value", 5, &len);
    return a ? JS_NewStringLen(ctx, a, len) : JS_NewStringLen(ctx, "", 0);
}

static bool iv_has_attr(lxb_dom_element_t *el, const char *name)
{
    return dom_attr_get_ns(el, NULL, name) != NULL;
}

/* §4.10.5.1.14's `colorspace` ENUMERATED ATTRIBUTE, whose two keywords name the Limited sRGB and Display P3
   states and whose missing value default AND invalid value default are both Limited sRGB — so this answers the
   one question that has an answer other than the default. An enumerated attribute's keywords are compared ASCII
   case-insensitively, which is why `DISPLAY-P3` is the same state. */
static bool iv_colorspace_is_display_p3(lxb_dom_element_t *el)
{
    static const char P3[] = "display-p3";
    size_t len = 0, i;
    const char *v = (const char *)lxb_dom_element_get_attribute(el, (const lxb_char_t *)"colorspace", 10, &len);

    if (!v || len != sizeof P3 - 1) return false;
    for (i = 0; i < len; i++) {
        char c = v[i];

        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != P3[i]) return false;
    }
    return true;
}

/* ---- a growable byte buffer, for the algorithms that build one ---------------------------------------------- */
typedef struct { char *s; size_t n, cap; } IvBuf;

static void iv_add(IvBuf *b, const char *s, size_t len)
{
    if (b->n + len + 1 > b->cap) {
        size_t c = b->cap ? b->cap * 2 : 64;

        while (c < b->n + len + 1) c *= 2;
        b->s = realloc(b->s, c);
        CHECK(b->s != NULL, "input: OOM sanitizing a control's value");
        b->cap = c;
    }
    if (len) memcpy(b->s + b->n, s, len);
    b->n += len;
    b->s[b->n] = 0;
}

/* Infra's "STRIP NEWLINES": remove every U+000A LF and U+000D CR. The Text, Search, Telephone and Password
   states' whole sanitization algorithm, and the first half of the URL and Email states'. */
static void iv_strip_newlines(IvBuf *b, const char *s, size_t len)
{
    size_t i, start = 0;

    for (i = 0; i <= len; i++) {
        if (i != len && s[i] != '\n' && s[i] != '\r') continue;
        iv_add(b, s + start, i - start);
        start = i + 1;
    }
}

static bool iv_is_ws(char c) { return c == '\t' || c == '\n' || c == '\f' || c == '\r' || c == ' '; }

/* Infra's "STRIP LEADING AND TRAILING ASCII WHITESPACE", over a span. */
static void iv_trim(const char *s, size_t *pstart, size_t *pend)
{
    while (*pstart < *pend && iv_is_ws(s[*pstart])) (*pstart)++;
    while (*pend > *pstart && iv_is_ws(s[*pend - 1])) (*pend)--;
}

/* ---- §4.10.5.1's VALUE SANITIZATION ALGORITHM ---------------------------------------------------------------
 *
 * Keyed on the STATE, because that is how the standard states it: each of §4.10.5.1's twenty-one sections
 * defines its own algorithm or defines none, and several that look like neighbours are not (URL and Email strip
 * whitespace where Text and Telephone do not; Range substitutes a default where Number empties the value;
 * Hidden, the checkables and the buttons define nothing at all).
 *
 * `value` is CONSUMED; the result is OWNED. `pchanged` (may be NULL) says whether the algorithm DECIDED
 * anything — which is exactly the condition under which a later type change could compose two algorithms, and
 * therefore the condition under which the state is worth recording. */
static JSValue iv_sanitize(JSContext *ctx, lxb_dom_element_t *el, HtmlInputState st, JSValue value, bool *pchanged)
{
    const char *s;
    size_t len = 0;
    IvBuf b = { 0 };
    JSValue r;

    if (pchanged) *pchanged = false;
    /* AN OPAQUE VALUE IS RETURNED AS ITSELF. The algorithms are decisions ABOUT BYTES, and an attacker source
       has none to decide over: its domain admits a string every one of these algorithms leaves alone, so the
       arm this engine keeps is that one. Emptying it instead — which is what "not a valid date string" would
       say if it were asked of a value it cannot read — would delete the source on the way to the sink and
       report a control that carries none. */
    if (concolic_is(value)) return value;

    switch (st) {
    case INPUT_STATE_HIDDEN:
    case INPUT_STATE_CHECKBOX: case INPUT_STATE_RADIO: case INPUT_STATE_FILE:
    case INPUT_STATE_SUBMIT: case INPUT_STATE_IMAGE: case INPUT_STATE_RESET: case INPUT_STATE_BUTTON:
        /* §4.10.5.1.1 and §4.10.5.1.15 through §4.10.5.1.21 define NO value sanitization algorithm. Answered
           here rather than by an "otherwise" so that a state added to the enum has to say which it is. */
        return value;
    default:
        break;
    }

    s = JS_ToCStringLen(ctx, &len, value);
    CHECK(s != NULL, "input: a control's value could not be converted for §4.10.5.1's sanitization");

    switch (st) {
    /* §4.10.5.1.2, §4.10.5.1.3 and §4.10.5.1.6: "Strip newlines from the value." */
    case INPUT_STATE_TEXT: case INPUT_STATE_SEARCH: case INPUT_STATE_TEL: case INPUT_STATE_PASSWORD:
        iv_strip_newlines(&b, s, len);
        break;
    /* §4.10.5.1.4, and §4.10.5.1.5 without `multiple`: "Strip newlines from the value, then strip leading and
       trailing ASCII whitespace from the value." The order is the standard's and it matters — a trailing
       "\n  " leaves whitespace that only the second half removes. */
    case INPUT_STATE_URL:
        iv_strip_newlines(&b, s, len);
        {
            size_t a = 0, z = b.n;

            iv_trim(b.s ? b.s : "", &a, &z);
            if (a || z != b.n) { memmove(b.s, b.s + a, z - a); b.n = z - a; b.s[b.n] = 0; }
        }
        break;
    case INPUT_STATE_EMAIL:
        if (!iv_has_attr(el, "multiple")) {
            iv_strip_newlines(&b, s, len);
            {
                size_t a = 0, z = b.n;

                iv_trim(b.s ? b.s : "", &a, &z);
                if (a || z != b.n) { memmove(b.s, b.s + a, z - a); b.n = z - a; b.s[b.n] = 0; }
            }
        } else {
            /* §4.10.5.1.5's `multiple` form, which is a DIFFERENT algorithm and not the same one twice: split
               on commas, strip each token's leading and trailing ASCII whitespace, and concatenate the tokens
               back with a single U+002C between them — so `a@b , c@d` normalises to `a@b,c@d`, empty tokens and
               all. It does NOT strip newlines, and the whitespace it strips is per TOKEN. */
            size_t start = 0, i;

            for (i = 0; i <= len; i++) {
                size_t a, z;

                if (i != len && s[i] != ',') continue;
                a = start;
                z = i;
                iv_trim(s, &a, &z);
                if (start) iv_add(&b, ",", 1);
                iv_add(&b, s + a, z - a);
                start = i + 1;
            }
        }
        break;
    /* §4.10.5.1.12: "If the value of the element is not a valid floating-point number, then set it to the empty
       string instead." The value is NOT normalised when it is one — `1e2` stays `1e2`. */
    case INPUT_STATE_NUMBER: {
        double d;
        bool valid = false;

        if (len) html_parse_floating_point(s, len, &d, &valid);
        if (valid) iv_add(&b, s, len);
        break;
    }
    /* §4.10.5.1.13: "If the value of the element is not a valid floating-point number, then set it to the best
       representation, as a floating-point number, of the DEFAULT VALUE" — "the minimum plus half the difference
       between the minimum and the maximum, unless the maximum is less than the minimum, in which case the
       default value is the minimum". This is why an empty `<input type=range>` reads back as `50` and not as
       the empty string every other state falls to. */
    case INPUT_STATE_RANGE: {
        double d;
        bool valid = false;
        InputStepRange rg;

        if (len) html_parse_floating_point(s, len, &d, &valid);
        if (valid) { iv_add(&b, s, len); break; }
        /* THE MINIMUM AND THE MAXIMUM ARE §4.10.5.3.7's, read through the component that owns them — including
           §4.10.5.1.13's defaults of 0 and 100, which are that section's statement of a DEFAULT MINIMUM and
           DEFAULT MAXIMUM and not two numbers belonging to this algorithm. Read here a second time they would
           be the pair that stops agreeing with the underflow the same control is validated by. */
        input_step_range_of(el, INPUT_STATE_RANGE, &rg);
        DCHECK(rg.has_min && rg.has_max,
               "the Range state answered with no minimum or no maximum — §4.10.5.1.13 states a default for "
               "both, so this state always has both and its default value is always a number");
        JS_FreeCString(ctx, s);
        JS_FreeValue(ctx, value);
        free(b.s);
        if (pchanged) *pchanged = true;
        return html_best_representation_of_number(ctx, rg.max < rg.min ? rg.min
                                                                       : rg.min + (rg.max - rg.min) / 2);
    }
    /* §4.10.5.1.7, §4.10.5.1.8, §4.10.5.1.9 and §4.10.5.1.10 are ONE SENTENCE FOUR TIMES: "If the value of the
       element is not a valid date / month / week / time string, then set it to the empty string instead." They
       differ only in which of §2.3.5's productions they name, and NONE of them rewrites the value it keeps — so
       an accepted value is the author's own bytes, `12:30:00.500` keeping its trailing zeros and `2015-06-06`
       its exact form. The empty string is not any of these productions, which is why an empty value falls out
       as itself with no case of its own. */
    case INPUT_STATE_DATE:
        if (html_is_valid_date_string(s, len)) iv_add(&b, s, len);
        break;
    case INPUT_STATE_MONTH:
        if (html_is_valid_month_string(s, len)) iv_add(&b, s, len);
        break;
    case INPUT_STATE_WEEK:
        if (html_is_valid_week_string(s, len)) iv_add(&b, s, len);
        break;
    case INPUT_STATE_TIME:
        if (html_is_valid_time_string(s, len, NULL)) iv_add(&b, s, len);
        break;
    /* §4.10.5.1.11 IS NOT THAT SENTENCE, and the difference is the point: "If the value of the element is a
       valid local date and time string, then set it to a valid NORMALIZED local date and time string
       representing the same date and time; otherwise, set it to the empty string instead." So this is the one
       date state that REWRITES what it keeps — §2.3.5.5 defines two productions and the one a value may be
       written in is not the one it is stored in. `2015-06-06 12:00:00.000` is accepted and stored back as
       `2015-06-06T12:00`: the U+0020 SPACE separator becomes the T, and the zero seconds are dropped because
       the normalized form's time is "expressed as the shortest possible string for the given time". */
    case INPUT_STATE_DATETIME_LOCAL: {
        HtmlDateTime dt;
        char norm[HTML_NORMALIZED_LOCAL_DATE_AND_TIME_CAP];

        /* ONE WALK ANSWERS BOTH HALVES of the sentence — whether the value is that production, and the date and
           time the normalized form is then built from. Asking the production and then parsing again would be
           two walks over the same bytes, and "the same date and time" would be whatever the second one said. */
        if (html_is_valid_local_date_and_time_string(s, len, &dt))
            iv_add(&b, norm, html_serialize_normalized_local_date_and_time(&dt, norm, sizeof norm));
        break;
    }
    case INPUT_STATE_COLOR: {
        /* §4.10.5.1.14: "Run UPDATE A COLOR WELL CONTROL COLOR for the element". That algorithm's step 2 picks
           the value out of the element the same way this component's callers already did — the `value` content
           attribute while the dirty value flag is false, the element's own value otherwise — so `value` IS its
           result, and steps 3 and 4 are the rest of it: PARSE the value as a CSS color, fall to OPAQUE BLACK
           when the parse returns failure, and set the value to `serialize a color well control color`. The
           failure branch is the one an EMPTY value takes, which is why `<input type=color>` reads back as
           `#000000` and never as the empty string. */
        CssColor c;
        char hex[8];

        /* `serialize a color well control color` steps 3 and 4. The Limited sRGB state with no `alpha`
           attribute is the branch that ends in §16.2.1's HTML-compatible serialization — the `#rrggbb` form —
           and it is the only branch reached without the `color()` function, which the CSS grammar this engine
           binds to does not have in either direction. */
        if (iv_has_attr(el, "alpha"))
            DFAIL("an `input` in the Color state has an `alpha` attribute — §4.10.5.1.14's `serialize a color "
                  "well control color` then KEEPS the colour's alpha component (step 3 makes it fully opaque "
                  "only when the attribute is absent) and step 4.4 converts the colour using the `color()` "
                  "function, so the value serializes as `color(srgb R G B / A)` rather than as a hex colour. "
                  "That form needs two things this engine has not built: CSS Color 4 §16.2.2's `color()` "
                  "serialization, and the `color()` PARSE that reads the control's own value back (lexbor's "
                  "<color> grammar answers failure for it, and css_color_parse resolves a `none` component to "
                  "0 where that serialization must preserve it) — build both in core/css/css_color.c");
        if (iv_colorspace_is_display_p3(el))
            DFAIL("an `input` in the Color state has its `colorspace` attribute in the Display P3 state — "
                  "§4.10.5.1.14's `serialize a color well control color` step 5 converts the colour to the "
                  "'display-p3' colour space and serializes it as `color(display-p3 R G B)`. That needs CSS "
                  "Color 4 §17's sRGB-to-Display-P3 conversion, §16.2.2's `color()` serialization, and the "
                  "`color()` PARSE that reads the control's own value back — build all three in "
                  "core/css/css_color.c");
        if (!css_color_parse(s, len, &c)) c = CSS_COLOR_OPAQUE_BLACK;   /* step 3's opaque black */
        /* Step 3 of the serialization: the `alpha` attribute is not specified — established above — so the
           colour's alpha component is set fully opaque, which is also what makes §16.2.1's form apply. */
        c.a = 1.0;
        css_color_serialize_html(&c, hex);
        iv_add(&b, hex, 7);
        break;
    }
    default:
        DFAIL("a state of the `type` attribute reached §4.10.5.1's value sanitization with no algorithm named "
              "for it — every one of §4.10.5.1's twenty-one sections either defines one or defines none, so "
              "name this state in one of the two groups above");
        break;
    }

    if (pchanged) *pchanged = b.n != len || (len && memcmp(b.s, s, len) != 0);
    r = JS_NewStringLen(ctx, b.s ? b.s : "", b.n);
    JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, value);
    free(b.s);
    return r;
}

/* ---- §4.10.5.4's `value` IDL attribute ---------------------------------------------------------------------- */

static HtmlInputState iv_state_of(JSValueConst wrap, lxb_dom_element_t **pel)
{
    lxb_dom_node_t *n = node_of(wrap);
    HtmlInputState st = html_form_input_state(n);

    *pel = st == INPUT_STATE_NONE ? NULL : lxb_dom_interface_element(n);
    return st;
}

JSValue input_value_get(JSContext *ctx, JSValueConst wrap)
{
    lxb_dom_element_t *el;
    HtmlInputState st = iv_state_of(wrap, &el);
    int i;

    DCHECK(st != INPUT_STATE_NONE, "§4.10.5.4's `value` getter ran on something that is not an `input` — the "
                                   "member is HTMLInputElement's and its brand check is the caller's");
    if (!el) return JS_NewStringLen(ctx, "", 0);
    iv_check_type_change(ctx, el, st);
    switch (input_value_mode(st)) {
    case INPUT_VALUE_MODE_VALUE:
        /* "On getting, return the CURRENT VALUE of the element" — and the DIRTY VALUE FLAG is what decides
           which of the two candidates that is. */
        i = iv_dirty(el);
        if (i >= 0) {
            int was = iv_recorded_state(ctx, el);
            JSValue held = JS_DupValue(ctx, attr_shadow_opaque(i)), now;
            bool changed = false;

            if (was < 0 || (HtmlInputState)was == st) return held;
            /* The state changed within mode value, so §4.10.5.1's type-change steps owed this value the new
               state's sanitization algorithm. Run it and STORE the result: the value the standard holds is the
               composition of every algorithm that has run over it, and re-deriving it from the original bytes
               on the next read would drop whichever one ran in between. */
            now = iv_sanitize(ctx, el, st, held, &changed);
            if (changed) dom_cow_set_prop_taint(ctx, el, IV_VALUE, now);
            iv_record_state(ctx, el, st);
            return now;
        }
        /* The flag is false, so §4.10.5.1's `value` CONTENT ATTRIBUTE steps decide the element's value: it IS
           that attribute (or the empty string), sanitized — which is what those steps store at every change to
           it, and therefore what the element's value is at every moment between them. */
        {
            JSValue raw = iv_content_attr(ctx, el, "");
            bool changed = false;
            JSValue san = iv_sanitize(ctx, el, st, raw, &changed);

            /* Recorded only when the algorithm DECIDED something: that is exactly when a later type change
               would have composed two algorithms and this engine would answer with one. */
            if (changed) iv_record_state(ctx, el, st);
            return san;
        }
    case INPUT_VALUE_MODE_DEFAULT:
        return iv_content_attr(ctx, el, "");
    case INPUT_VALUE_MODE_DEFAULT_ON:
        return iv_content_attr(ctx, el, "on");
    case INPUT_VALUE_MODE_FILENAME: {
        /* "On getting, return the string `C:\fakepath\` followed by the name of the FIRST FILE in the list of
           selected files, if any, or the empty string if the list is empty." The prefix cannot be mistaken for
           a path component of the name, because path components are stripped where a file enters the device. */
        static const char FAKE[] = "C:\\fakepath\\";
        JSValue first = input_files_item(ctx, wrap, 0), r;
        const char *name;
        size_t nlen;
        char *buf;

        if (JS_IsUndefined(first)) return JS_NewStringLen(ctx, "", 0);
        name = blob_file_name_of(first);
        DCHECK(name != NULL, "the list of selected files held a value that is not a File — File API §5 is a "
                             "list of File objects and file_list_new asserts that at every build, so a plain "
                             "Blob cannot have got in");
        nlen = name ? strlen(name) : 0;
        buf = malloc(sizeof FAKE - 1 + nlen);
        CHECK(buf != NULL, "input: OOM building a file control's value");
        memcpy(buf, FAKE, sizeof FAKE - 1);
        if (nlen) memcpy(buf + sizeof FAKE - 1, name, nlen);
        r = JS_NewStringLen(ctx, buf, sizeof FAKE - 1 + nlen);
        free(buf);
        JS_FreeValue(ctx, first);
        return r;
    }
    }
    DFAIL("§4.10.5.4 names four modes of the `value` IDL attribute and a fifth arrived");
    return JS_NewStringLen(ctx, "", 0);
}

JSValue input_value_set(JSContext *ctx, JSValueConst wrap, JSValueConst val)
{
    lxb_dom_element_t *el;
    HtmlInputState st = iv_state_of(wrap, &el);

    DCHECK(st != INPUT_STATE_NONE, "§4.10.5.4's `value` setter ran on something that is not an `input`");
    if (!el) return JS_UNDEFINED;
    iv_check_type_change(ctx, el, st);
    switch (input_value_mode(st)) {
    case INPUT_VALUE_MODE_VALUE: {
        JSValue now;

        /* Steps 2-4: set the element's value to the new value, set the DIRTY VALUE FLAG, and invoke the value
           sanitization algorithm. Step 1's `oldValue` and step 5's text entry cursor belong to a control with a
           cursor, which nothing here has; the three steps that decide the value are all of it. Storing the
           element's own value IS setting the flag — see iv_dirty.
           THE SLOT KEEPS THE VALUE and not its bytes, so a concolic stays a concolic all the way to §4.10.22.4's
           entry list: `input.value = location.hash` then `submit()` is an endpoint carrying an attacker
           source. */
        now = iv_sanitize(ctx, el, st, JS_DupValue(ctx, val), NULL);
        dom_cow_set_prop_taint(ctx, el, IV_VALUE, now);
        /* Recorded unconditionally on this path — the value now stored was sanitized in THIS state, and it is
           the composition with a LATER state's algorithm that has no site to run at. */
        iv_record_state(ctx, el, st);
        JS_FreeValue(ctx, now);
        return JS_UNDEFINED;
    }
    case INPUT_VALUE_MODE_DEFAULT:
    case INPUT_VALUE_MODE_DEFAULT_ON: {
        /* "On setting, set the value of the element's `value` CONTENT ATTRIBUTE to the new value" — which is
           why `checkbox.value = 'x'` is visible to `getAttribute` and a `type=text` assignment is not. */
        const char *s;
        size_t len = 0;

        if (concolic_is(val)) {
            const char *shape = concolic_shape_c(val);

            dom_cow_set_attribute(el, "value", shape ? shape : "", shape ? strlen(shape) : 0, val);
            return JS_UNDEFINED;
        }
        s = JS_ToCStringLen(ctx, &len, val);
        CHECK(s != NULL, "input: a `value` assignment could not be converted for the content attribute");
        dom_cow_set_attribute(el, "value", s, len, JS_UNDEFINED);
        JS_FreeCString(ctx, s);
        return JS_UNDEFINED;
    }
    case INPUT_VALUE_MODE_FILENAME: {
        /* "On setting, if the new value is the empty string, EMPTY the list of selected files; otherwise, throw
           an `InvalidStateError` DOMException." Both branches are real: the first is how a page clears a
           control it has driven a selection into, and the throw is not optional — a page distinguishes it from
           an assignment that silently did nothing. */
        size_t len = 0;
        const char *s = JS_ToCStringLen(ctx, &len, val);
        bool empty;

        CHECK(s != NULL, "input: a `value` assignment could not be converted for the File Upload state");
        /* A CONCOLIC is never the empty string here: its own text is a shape, and the branch this state's
           setter takes on an unknown is the one the standard takes on everything that is not "". */
        empty = len == 0 && !concolic_is(val);
        JS_FreeCString(ctx, s);
        if (empty) {
            input_files_clear(ctx, wrap);
            return JS_UNDEFINED;
        }
        return JS_ThrowDOMException(ctx, "InvalidStateError",
                                    "an `input` in the File Upload state was assigned a value other than the "
                                    "empty string");
    }
    }
    DFAIL("§4.10.5.4 names four modes of the `value` IDL attribute and a fifth arrived");
    return JS_UNDEFINED;
}

/* ---- §4.10.5.1.17's LIST OF SELECTED FILES, and §4.10.5.4's `files` ------------------------------------------
 *
 * "The input element represents a list of selected files, each file consisting of a filename, a file type, and
 * a file body." The list is per-element state and lives in the same per-flow property shadow as the value, so a
 * flow that drives a selection has one and its sibling has the markup's empty one.
 *
 * IT IS HELD AS THE FileList ITSELF, not as a list this file would wrap on each read, because §4.10.5.4 makes
 * the OBJECT observable: "return a FileList object that represents the current selected files. THE SAME OBJECT
 * must be returned until the list of selected files changes." A fresh wrapper per read would make
 * `input.files === input.files` false. */

/* The element's list, BORROWED from the shadow, or JS_UNDEFINED when nothing has ever given this element one —
   which is the empty list, and is why every reader below asks file_list_* rather than branching on it. */
static JSValue iv_files_slot(lxb_dom_element_t *el)
{
    int i = attr_shadow_find(el, ATTR_SLOT_PROPERTY, NULL, IV_FILES);

    return i < 0 ? JS_UNDEFINED : attr_shadow_opaque(i);
}

/* REPLACE the element's list. The per-flow chokepoint, so the selection reverts with every other write this
   flow made — and the shadow DUPS what it is given, so the caller keeps its own reference. */
static void iv_files_store(JSContext *ctx, lxb_dom_element_t *el, JSValueConst files)
{
    DCHECK(file_list_is(ctx, files),
           "an input's list of selected files was set to something that is not a FileList — §4.10.5.4's setter "
           "and §4.10.5.1.17's update the file selection are the only two writers and both brand what they are "
           "given");
    dom_cow_set_prop_taint(ctx, el, IV_FILES, files);
}

/* THE ELEMENT, asserted to be in the state whose algorithms these are. NULL for anything else, which is the
   same guard every member above uses for an `input` that has no element behind it. */
static lxb_dom_element_t *iv_file_control(JSValueConst wrap)
{
    lxb_dom_element_t *el;
    HtmlInputState st = iv_state_of(wrap, &el);

    DCHECK(st == INPUT_STATE_FILE,
           "an algorithm of §4.10.5.1.17's File Upload state ran on a control that is not in it — the list of "
           "selected files belongs to that state and to no other, and every caller has the state in hand");
    return st == INPUT_STATE_FILE ? el : NULL;
}

uint32_t input_files_count(JSContext *ctx, JSValueConst wrap)
{
    lxb_dom_element_t *el = iv_file_control(wrap);

    return el ? file_list_length(ctx, iv_files_slot(el)) : 0;
}

JSValue input_files_item(JSContext *ctx, JSValueConst wrap, uint32_t i)
{
    lxb_dom_element_t *el = iv_file_control(wrap);

    return el ? file_list_item(ctx, iv_files_slot(el), i) : JS_UNDEFINED;
}

JSValue input_files_get(JSContext *ctx, JSValueConst wrap)
{
    lxb_dom_element_t *el;
    HtmlInputState st = iv_state_of(wrap, &el);
    JSValue held, fresh;

    DCHECK(st != INPUT_STATE_NONE, "§4.10.5.4's `files` getter ran on something that is not an `input` — the "
                                   "member is HTMLInputElement's and its brand check is at the member");
    /* "If the IDL attribute does not apply, then it must instead return null." It applies to the File Upload
       state and to no other, and a page tells that null from an empty list. */
    if (st != INPUT_STATE_FILE || !el) return JS_NULL;
    held = iv_files_slot(el);
    if (!JS_IsUndefined(held)) return JS_DupValue(ctx, held);
    /* THE EMPTY LIST IS AN OBJECT TOO, and it is minted once and STORED — "the same object must be returned
       until the list of selected files changes", so a control nothing has selected into still answers the same
       FileList on every read. Storing it on a READ is the [SameObject] cache every live collection in this
       engine keeps, and it goes in the flow's delta like any other write. */
    fresh = file_list_new_empty(ctx);
    iv_files_store(ctx, el, fresh);
    return fresh;
}

void input_files_clear(JSContext *ctx, JSValueConst wrap)
{
    lxb_dom_element_t *el = iv_file_control(wrap);
    JSValue fresh;

    if (!el) return;
    /* EMPTYING AN EMPTY LIST CHANGES NOTHING, and §4.10.5.4 makes that observable: storing a fresh empty list
       here would change the object `input.files` answers with across an assignment that selected and
       deselected nothing. */
    if (file_list_length(ctx, iv_files_slot(el)) == 0) return;
    fresh = file_list_new_empty(ctx);
    iv_files_store(ctx, el, fresh);
    JS_FreeValue(ctx, fresh);
}

/* §4.10.5.1.17's UPDATE THE FILE SELECTION for `wrap`, given the files the user's selection produced:
 *
 *   "Queue an element task on the user interaction task source given element and the following steps:
 *      1. Update element's selected files so that it represents the user's selection.
 *      2. Fire an event named input at the input element, with the bubbles and composed attributes
 *         initialized to true.
 *      3. Fire an event named change at the input element, with the bubbles attribute initialized to true."
 *
 * The two fires are ELEMENT TASKS, which in this engine is what event_target_fire enqueues: each dispatch is a
 * first-class flow the one scheduler drives, so a listener's body — loop, await, generator — suspends and
 * resumes like any other program instead of running inside a C activation that cannot park. */
void input_files_update(JSContext *ctx, JSValueConst wrap, JSValueConst files)
{
    lxb_dom_element_t *el = iv_file_control(wrap);
    JSValue tv;

    DCHECK(file_list_is(ctx, files),
           "§4.10.5.1.17's update the file selection was given something that is not a FileList — the user's "
           "selection is a list of File objects and file_device_select is what produces one");
    if (!el) return;
    iv_files_store(ctx, el, files);   /* step 1 */
    /* Step 2. `composed` is not one of event_new's arguments because most engine fires do not set it; the
       derived form takes it, and Event.prototype is the interface §4.10.5.1.17 names ("fire an event named
       input" with no interface given is a plain Event). */
    tv = JS_NewString(ctx, "input");
    CHECK(!JS_IsException(tv), "input: the `input` event type could not be allocated");
    event_target_fire(ctx, wrap,
                      event_new_derived(ctx, event_proto(ctx), tv, /*bubbles*/ true, /*cancelable*/ false,
                                        /*composed*/ true, /*trusted*/ true),
                      JS_UNDEFINED);
    JS_FreeValue(ctx, tv);
    /* Step 3. `change` does not compose — it stops at the shadow boundary, which is what the standard says by
       initialising only `bubbles`. */
    event_target_fire(ctx, wrap, event_new(ctx, "change", /*bubbles*/ true, /*cancelable*/ false), JS_UNDEFINED);
}

uint32_t input_files_pick(JSContext *ctx, JSValueConst wrap)
{
    lxb_dom_element_t *el = iv_file_control(wrap);
    const char *accept;
    size_t alen = 0;
    JSValue files;
    uint32_t n;

    if (!el) return 0;
    /* §4.10.5.1.17: "If the element is not mutable, the user agent must not allow the user to change the
       element's selection." Asked as §4.10.5.1's own predicate rather than as `disabled` alone: the two answer
       identically for this state (`readonly` does not apply to File Upload, which §4.10.5.3.6 says by listing
       the states it does apply to), and that identity is exactly why the second speller had to go — it is one
       sentence of the standard, and §4.10.5.4's showPicker reads the same one. */
    if (!html_form_input_is_mutable(ctx, wrap)) return 0;
    accept = (const char *)lxb_dom_element_get_attribute(el, (const lxb_char_t *)"accept", 6, &alen);
    files = file_device_select(ctx, accept, alen, iv_has_attr(el, "multiple"));
    n = file_list_length(ctx, files);
    /* THE DEVICE OFFERED NOTHING THIS CONTROL ACCEPTS — the same as a user who was shown a prompt and chose
       nothing. §4.10.5.1.17's update is what happens "when the user does so", so an empty answer leaves the
       list, and the `input`/`change` events, alone. */
    if (n) input_files_update(ctx, wrap, files);
    JS_FreeValue(ctx, files);
    return n;
}

/* ---- §4.10.5.4's `files` member ----------------------------------------------------------------------------- */

/* Web IDL §3.7.5's BRAND CHECK, which is this member's own because this file installs it:
   `descriptor.get.call(textarea)` is a TypeError and not the null a non-file input answers with. */
static bool iv_is_input(JSValueConst this_val)
{
    return html_form_input_state(node_of(this_val)) != INPUT_STATE_NONE;
}

static JSValue js_input_get_files(JSContext *ctx, JSValueConst this_val, int magic)
{
    (void)magic;
    if (!iv_is_input(this_val))
        return JS_ThrowTypeError(ctx, "HTMLInputElement's `files` was accessed on something that is not an "
                                      "HTMLInputElement");
    return input_files_get(ctx, this_val);
}

static JSValue js_input_set_files(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    lxb_dom_element_t *el;
    HtmlInputState st;

    (void)magic;
    if (!iv_is_input(this_val))
        return JS_ThrowTypeError(ctx, "HTMLInputElement's `files` was assigned on something that is not an "
                                      "HTMLInputElement");
    st = iv_state_of(this_val, &el);
    /* THE TYPE'S OWN REFUSAL, MADE HERE. `attribute FileList? files` is a NULLABLE INTERFACE type and the
       declaration surface has none — IDL_INTERFACE throws a TypeError for the null that §4.10.5.4's step 1
       requires to silently return, and there is no nullable form of it (the same edge form_data.c's
       constructor states for its `HTMLElement? submitter`). So the conversion is this body's first step,
       written as the two things the type says: null and undefined are the IDL null, and anything that is not a
       FileList is a TypeError before the algorithm's step 1. */
    if (JS_IsNull(val) || JS_IsUndefined(val)) return JS_UNDEFINED;
    if (!file_list_is(ctx, val))
        return JS_ThrowTypeError(ctx, "HTMLInputElement's `files` was assigned something that is not a "
                                      "FileList");
    /* Step 1's other half: "If the IDL attribute does not apply ... then return." */
    if (st != INPUT_STATE_FILE || !el) return JS_UNDEFINED;
    /* Step 2: "Replace the element's selected files with the given value." The GIVEN OBJECT becomes the
       element's list, which is what makes `input.files = fl; input.files === fl` hold — the assignment is
       itself the change that ends the previous object's identity. */
    iv_files_store(ctx, el, val);
    return JS_UNDEFINED;
}

/* ---- §4.10.5.4's `valueAsNumber`, `valueAsDate`, `stepUp()` and `stepDown()` ---------------------------------
 *
 * THE SAME VALUE, INTERPRETED. Each of these four is stated over the element's value and one of §4.10.5.1's
 * per-state conversions — which is why they are here, beside the value they read and write, and why the
 * arithmetic they read it through is core/html/input_number.c and not written a second time in each member.
 *
 * WHAT "SET THE VALUE OF THE ELEMENT" MEANS is §4.10.5.4's value-mode setter above: the element's value, the
 * dirty value flag, and the value sanitization algorithm. So a string these produce that the state's syntax
 * does not accept is emptied by that algorithm on the way in, which is exactly what the standard's own
 * composition does and is why none of them needs a case for a conversion that had no result.
 *
 * AN UNKNOWN VALUE STAYS UNKNOWN THROUGH THEM. `input.value = location.hash` makes the number these convert to
 * unknown external input as well, and the answer is the one document.createElement gives an unknown tag: RUN
 * the real conversion on the source's own EXAMPLE where it carries one, and hand back a value that records
 * which operation produced it. A concrete NaN would decide the page's next branch, and an invented number is a
 * fabricated observation. */

/* A NUMBER OUT OF A VALUE THAT MAY BE UNKNOWN — the EXAMPLE an unknown carries, converted the way the IDL
   boundary would have converted the value itself: ToNumber on a primitive (which runs none of the page's code)
   and then §3.2's integer rule for the `long` the method declares. `dflt` is what a value with no example
   answers, which is the IDL's own default for an absent argument. */
static double iv_number_operand(JSContext *ctx, JSValueConst v, IdlArgType t, double dflt)
{
    JSValue ex = concolic_is(v) ? concolic_example(ctx, v) : JS_DupValue(ctx, v);
    double d = dflt;

    if (JS_IsNumber(ex) || JS_IsString(ex)) {
        JS_ToFloat64(ctx, &d, ex);
        if (t != IDL_UNRESTRICTED_DOUBLE) d = (double)idl_integer_of(t, d);
    }
    JS_FreeValue(ctx, ex);
    return d;
}

/* The conversion, RUN ON THE EXAMPLE the unknown value carries — never predicted from it. `as_date` picks which
   of the two per-state conversions is run, which is also what the result's recorded operation is named for.
   OWNED. */
static JSValue iv_number_of_unknown(JSContext *ctx, HtmlInputState st, JSValueConst value, bool as_date)
{
    JSValue ex = concolic_example(ctx, value), real = JS_UNDEFINED;
    double n;

    if (JS_IsString(ex)) {
        size_t len = 0;
        const char *s = JS_ToCStringLen(ctx, &len, ex);

        CHECK(s != NULL, "input: an unknown value's example could not be read for §4.10.5.4's conversion");
        if (as_date) {
            if (input_number_date_from_string(st, s, len, &n)) real = JS_NewDate(ctx, n);
        } else if (input_number_from_string(st, s, len, &n)) {
            real = JS_NewFloat64(ctx, n);
        }
        JS_FreeCString(ctx, s);
    }
    JS_FreeValue(ctx, ex);
    return concolic_builtin_hook(ctx, value, as_date ? "valueAsDate" : "valueAsNumber", real);
}

static JSValue js_input_get_value_as_number(JSContext *ctx, JSValueConst this_val, int magic)
{
    lxb_dom_element_t *el;
    HtmlInputState st;
    JSValue value;
    double n;
    bool ok;
    size_t len = 0;
    const char *s;

    (void)magic;
    if (!iv_is_input(this_val))
        return JS_ThrowTypeError(ctx, "HTMLInputElement's `valueAsNumber` was accessed on something that is "
                                      "not an HTMLInputElement");
    st = iv_state_of(this_val, &el);
    /* "On getting, if the valueAsNumber attribute DOES NOT APPLY, as defined for the input element's type
       attribute's current state, then return a Not-a-Number (NaN) value." */
    if (!el || !input_number_applies(st)) return JS_NewFloat64(ctx, NAN);
    value = input_value_get(ctx, this_val);
    if (concolic_is(value)) {
        JSValue r = iv_number_of_unknown(ctx, st, value, false);

        JS_FreeValue(ctx, value);
        return r;
    }
    /* "Otherwise, run the algorithm to convert a string to a number defined for that state to the element's
       value; if the algorithm returned a number, then return it, otherwise, return a NaN value." */
    s = JS_ToCStringLen(ctx, &len, value);
    CHECK(s != NULL, "input: a control's value could not be converted for §4.10.5.4's valueAsNumber");
    ok = input_number_from_string(st, s, len, &n);
    JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, value);
    return JS_NewFloat64(ctx, ok ? n : NAN);
}

/* "Set the value of the element to X" — §4.10.5.4's value-mode setter, which is the only writer of a control's
   value in this engine. `s` is CONSUMED, and JS_UNDEFINED is the conversion that had no result: the element's
   value is then the empty string, because a string that is not this state's syntax is what its value
   sanitization algorithm empties. */
static JSValue iv_set_converted(JSContext *ctx, JSValueConst wrap, JSValue s)
{
    JSValue empty, r;

    if (!JS_IsUndefined(s)) {
        r = input_value_set(ctx, wrap, s);
        JS_FreeValue(ctx, s);
        return r;
    }
    empty = JS_NewStringLen(ctx, "", 0);
    CHECK(!JS_IsException(empty), "input: the empty string could not be allocated for a value assignment");
    r = input_value_set(ctx, wrap, empty);
    JS_FreeValue(ctx, empty);
    return r;
}

static JSValue js_input_set_value_as_number(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    lxb_dom_element_t *el;
    HtmlInputState st;
    double d = 0;

    (void)magic;
    if (!iv_is_input(this_val))
        return JS_ThrowTypeError(ctx, "HTMLInputElement's `valueAsNumber` was assigned on something that is "
                                      "not an HTMLInputElement");
    st = iv_state_of(this_val, &el);
    if (!concolic_is(val)) {
        DCHECK(JS_IsNumber(val), "§4.10.5.4's valueAsNumber setter was handed something that is not a number — "
                                 "its IDL type is `unrestricted double` and the declaration converts it, which "
                                 "is also what makes the NaN branch below reachable rather than a type error");
        JS_ToFloat64(ctx, &d, val);
        /* "On setting, if the new value is INFINITE, then throw a TypeError exception." Before the
           does-not-apply check, which is the order the standard's two sentences are in: `text.valueAsNumber =
           Infinity` is a TypeError and not an InvalidStateError. */
        if (isinf(d))
            return JS_ThrowTypeError(ctx, "HTMLInputElement's `valueAsNumber` was assigned an infinite value");
    }
    /* "Otherwise, if the valueAsNumber attribute does not apply ... then throw an InvalidStateError." */
    if (!el || !input_number_applies(st))
        return JS_ThrowDOMException(ctx, "InvalidStateError",
                                    "`valueAsNumber` does not apply to an `input` in this state — the member "
                                    "is defined only for the states that define a conversion between their "
                                    "value and a number");
    if (concolic_is(val)) {
        /* The number is unknown external input, so the string it converts to is too — and it reaches the
           element's value as such, which is what carries the source through to §4.10.22.4's entry list. */
        double e = iv_number_operand(ctx, val, IDL_UNRESTRICTED_DOUBLE, NAN);
        JSValue real = isfinite(e) ? input_number_to_string(ctx, st, e) : JS_UNDEFINED;

        return iv_set_converted(ctx, this_val, concolic_builtin_hook(ctx, val, "valueAsNumber=", real));
    }
    /* "Otherwise, if the new value is a Not-a-Number (NaN) value, then set the value of the element to the
       empty string." */
    if (isnan(d)) return iv_set_converted(ctx, this_val, JS_UNDEFINED);
    /* "Otherwise, run the algorithm to convert a number to a string, as defined for that state, on the new
       value, and set the value of the element to the resulting string." */
    return iv_set_converted(ctx, this_val, input_number_to_string(ctx, st, d));
}

static JSValue js_input_get_value_as_date(JSContext *ctx, JSValueConst this_val, int magic)
{
    lxb_dom_element_t *el;
    HtmlInputState st;
    JSValue value;
    double t;
    bool ok;
    size_t len = 0;
    const char *s;

    (void)magic;
    if (!iv_is_input(this_val))
        return JS_ThrowTypeError(ctx, "HTMLInputElement's `valueAsDate` was accessed on something that is not "
                                      "an HTMLInputElement");
    st = iv_state_of(this_val, &el);
    /* "On getting, if the valueAsDate attribute does not apply ... then return null." */
    if (!el || !input_number_date_applies(st)) return JS_NULL;
    value = input_value_get(ctx, this_val);
    if (concolic_is(value)) {
        JSValue r = iv_number_of_unknown(ctx, st, value, true);

        JS_FreeValue(ctx, value);
        return r;
    }
    s = JS_ToCStringLen(ctx, &len, value);
    CHECK(s != NULL, "input: a control's value could not be converted for §4.10.5.4's valueAsDate");
    ok = input_number_date_from_string(st, s, len, &t);
    JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, value);
    /* "Otherwise, run the algorithm to convert a string to a Date object defined for that state to the
       element's value; if the algorithm returned a Date object, then return it, otherwise, return null."
       THE Date IS THE REALM'S OWN — JS_NewDate builds it from this context's %Date.prototype% and sets its
       [[DateValue]], which is what "return a new Date object representing midnight UTC" is. It is not
       `new Date(t)`: that would run the Date CONSTRUCTOR, which this engine makes a step machine precisely so
       that no C entry constructs one, and the spec's own words name the object and not the constructor. */
    return ok ? JS_NewDate(ctx, t) : JS_NULL;
}

static JSValue js_input_set_value_as_date(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    lxb_dom_element_t *el;
    HtmlInputState st;
    bool null_value = JS_IsNull(val) || JS_IsUndefined(val);

    (void)magic;
    if (!iv_is_input(this_val))
        return JS_ThrowTypeError(ctx, "HTMLInputElement's `valueAsDate` was assigned on something that is not "
                                      "an HTMLInputElement");
    st = iv_state_of(this_val, &el);
    /* THE TYPE'S OWN REFUSAL, MADE HERE, exactly as `files` states it: `attribute object? valueAsDate` is a
       NULLABLE OBJECT type and the declaration surface has none, so the conversion is this body's first step —
       null and undefined are the IDL null, and a PRIMITIVE is a TypeError from the TYPE, thrown before the
       algorithm's step 1. The order is observable: `text.valueAsDate = "x"` is that TypeError and not the
       InvalidStateError the same assignment of a Date would answer. */
    if (!null_value && !JS_IsObject(val))
        return JS_ThrowTypeError(ctx, "HTMLInputElement's `valueAsDate` was assigned a value that is not an "
                                      "object");
    /* Step 1: "if the valueAsDate attribute does not apply ... then throw an InvalidStateError." */
    if (!el || !input_number_date_applies(st))
        return JS_ThrowDOMException(ctx, "InvalidStateError",
                                    "`valueAsDate` does not apply to an `input` in this state — the member is "
                                    "defined only for the states that convert their value to a Date");
    /* "otherwise, if the new value is not null and not a Date object throw a TypeError exception." UNKNOWN
       EXTERNAL INPUT LANDS HERE and is refused like any other non-Date: a concolic is an object carrying a
       string source, never a platform Date, and a TypeError for it is the same answer a browser gives that
       object — it de-taints nothing, because the assignment did not happen in either world. */
    if (!null_value && !JS_IsDate(val))
        return JS_ThrowTypeError(ctx, "HTMLInputElement's `valueAsDate` was assigned an object that is not a "
                                      "Date");
    /* "otherwise, if the new value is null or a Date object representing the NAN TIME VALUE, then set the
       value of the element to the empty string" — the object's own [[DateValue]], never its `getTime`, which
       is the page's to replace. */
    if (null_value || isnan(JS_GetDateValue(val))) return iv_set_converted(ctx, this_val, JS_UNDEFINED);
    /* "otherwise, run the algorithm to convert a Date object to a string, as defined for that state, on the
       new value, and set the value of the element to the resulting string." */
    return iv_set_converted(ctx, this_val, input_number_date_to_string(ctx, st, JS_GetDateValue(val)));
}

/* §4.10.5.4's stepUp(n) and stepDown(n) STEPS 5 THROUGH 11 — the half of the algorithm that is arithmetic over
 * a starting value, split out so the unknown-value path can run the very same steps over the example rather
 * than a second implementation of them.
 *
 * `up` is which method was invoked; `have_value` is whether step 5's conversion produced a number. Answers the
 * string step 11 sets the element's value to, or JS_UNDEFINED where the algorithm RETURNS without changing
 * anything — which steps 3, 4 and 10 all do and which is why this is not a bool plus an out-parameter. */
static JSValue iv_step_apply(JSContext *ctx, HtmlInputState st, const InputStepRange *r,
                             double value, bool have_value, double n, bool up)
{
    double before;

    DCHECK(r->has_step, "§4.10.5.4's step algorithm ran with no allowed value step — step 2 throws an "
                        "InvalidStateError for that and this is reached only past it");
    /* Step 5: "If applying the algorithm to convert a string to a number to the string given by the element's
       value does not result in an error, then let value be the result of that algorithm. Otherwise, let value
       be ZERO." — which is why stepUp() on an empty number control lands on the step and not on nothing. */
    if (!have_value) value = 0;
    before = value;                                                            /* step 6 */
    if (input_step_mismatch(value, r)) {
        /* Step 7's FIRST branch: "set value to the NEAREST value that, when subtracted from the step base, is
           an integral multiple of the allowed value step, and that is LESS THAN value if the method invoked
           was the stepDown() method, and MORE THAN value otherwise." The ARGUMENT IS NOT USED HERE — an
           unaligned value snaps by one step's worth however large n is, which is the standard's own wording
           and the half of this algorithm a summary loses. */
        value = up ? input_step_align_up(value, r) : input_step_align_down(value, r);
        DCHECK(up ? value > before : value < before,
               "§4.10.5.4's step 7 snapped an unaligned value to the wrong side of it — the value is a step "
               "mismatch, so the aligned value on each side is strictly beyond it");
    } else {
        /* Step 7's SECOND branch: delta is the allowed value step multiplied by n, negated for stepDown. */
        double delta = r->step * n;

        value += up ? delta : -delta;
    }
    /* Step 8: "If the element has a minimum, and value is less than that minimum, then set value to the
       SMALLEST value that ... is an integral multiple of the allowed value step, and that is more than or
       equal to that minimum." */
    if (r->has_min && value < r->min) value = input_step_align_up(r->min, r);
    /* Step 9: the same for the maximum, with the LARGEST aligned value not greater than it. */
    if (r->has_max && value > r->max) value = input_step_align_down(r->max, r);
    /* Step 10: "If either the method invoked was the stepDown() method and value is GREATER THAN
       valueBeforeStepping, or the method invoked was the stepUp() method and value is LESS THAN
       valueBeforeStepping, then return."
       THIS IS THE STEP THE SPEC WROTE ITS OWN COUNTEREXAMPLE FOR: `<input type=number value=1 max=0>`. Step 7
       carries 1 to 2, step 9 clamps it back to 0 — below where it started — and without this step stepUp()
       would have moved the value DOWN. */
    if (up ? value < before : value > before) return JS_UNDEFINED;
    DCHECK(!input_step_mismatch(value, r),
           "§4.10.5.4's step algorithm produced a value that is a step mismatch — every branch above lands on "
           "an integral multiple of the allowed value step above the step base");
    DCHECK(!r->has_min || value >= r->min - r->step / 16777216.0,
           "§4.10.5.4's step algorithm produced a value below the element's minimum — step 8 puts it at or "
           "above one, and the only thing that moves it afterwards is step 9's clamp to the maximum, which "
           "step 4 has already established there is an aligned value below");
    DCHECK(!r->has_max || value <= r->max + r->step / 16777216.0,
           "§4.10.5.4's step algorithm produced a value above the element's maximum — step 9 is the last thing "
           "that touches it and it clamps to the largest aligned value not greater than that maximum");
    /* Step 11: "Let value as string be the result of running the algorithm to convert a number to a string, as
       defined for the input element's type attribute's current state, on value." */
    return input_number_to_string(ctx, st, value);
}

enum { IV_STEP_DOWN = 0, IV_STEP_UP = 1 };

static JSValue js_input_step(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    lxb_dom_element_t *el;
    HtmlInputState st;
    InputStepRange r;
    JSValue value, s;
    bool up = magic == IV_STEP_UP, have_value = false;
    double x = 0, n = 1;

    DCHECK(magic == IV_STEP_UP || magic == IV_STEP_DOWN,
           "§4.10.5.4's step methods were installed with a magic that is neither of the two they have");
    if (!iv_is_input(this_val))
        return JS_ThrowTypeError(ctx, "HTMLInputElement's `stepUp`/`stepDown` was called on something that is "
                                      "not an HTMLInputElement");
    st = iv_state_of(this_val, &el);
    /* Step 1: "If the stepDown() and stepUp() methods do not apply, as defined for the input element's type
       attribute's current state, then throw an InvalidStateError." They apply exactly where the state defines
       a conversion between its value and a number, which is what makes that one predicate. */
    if (!el || !input_number_applies(st))
        return JS_ThrowDOMException(ctx, "InvalidStateError",
                                    "`stepUp()`/`stepDown()` do not apply to an `input` in this state");
    input_step_range_of(el, st, &r);
    /* Step 2: "If the element has NO ALLOWED VALUE STEP, then throw an InvalidStateError." Which is what
       `step=any` means, and the reason it is a throw rather than a no-op: there is no granularity to step by. */
    if (!r.has_step)
        return JS_ThrowDOMException(ctx, "InvalidStateError",
                                    "`stepUp()`/`stepDown()` were called on an `input` whose `step` attribute "
                                    "is `any`, which gives the element no allowed value step");
    /* Step 3: "If the element has a minimum and a maximum and the minimum is GREATER THAN the maximum, then
       return." Stated over the minimum and the maximum themselves, so a REVERSED RANGE — a time control whose
       range spans midnight — returns here too: there is no direction to step in when the allowed values are
       the two ends of a period rather than an interval. */
    if (r.has_min && r.has_max && r.min > r.max) return JS_UNDEFINED;
    /* Step 4: "If the element has a minimum and a maximum and there is no value greater than or equal to the
       element's minimum and less than or equal to the element's maximum that, when subtracted from the step
       base, is an integral multiple of the allowed value step, then return." The smallest aligned value not
       below the minimum is the only candidate, so this is one comparison. */
    if (r.has_min && r.has_max && input_step_align_up(r.min, &r) > r.max) return JS_UNDEFINED;
    /* THE ARGUMENT: `optional long n = 1`, so an absent one and an explicit `undefined` are both the IDL's
       DEFAULT and not a conversion of undefined. Unknown external input carries its own example through the
       same §3.2 integer rule the declaration would have applied to it; with no example the algorithm runs on
       the default and the result below records that it is unknown rather than reporting the number it got. */
    if (argc >= 1 && !JS_IsUndefined(argv[0])) {
        DCHECK(JS_IsNumber(argv[0]) || concolic_is(argv[0]),
               "§4.10.5.4's step methods were handed an argument that is neither a number nor unknown external "
               "input — their IDL type is `long` and the declaration converts everything else");
        n = iv_number_operand(ctx, argv[0], IDL_LONG, 1);
    }
    value = input_value_get(ctx, this_val);
    {
        /* Step 5's conversion, over the element's value — or over the EXAMPLE an unknown value carries, so the
           arithmetic below is the real algorithm on a real number in both worlds. A value with no example
           leaves step 5's "otherwise, let value be zero" standing, which is what it already answers for a
           control whose value is not this state's syntax. */
        JSValue text = concolic_is(value) ? concolic_example(ctx, value) : JS_DupValue(ctx, value);

        if (JS_IsString(text)) {
            size_t len = 0;
            const char *cs = JS_ToCStringLen(ctx, &len, text);

            CHECK(cs != NULL, "input: a control's value could not be read for §4.10.5.4's stepUp/stepDown");
            have_value = input_number_from_string(st, cs, len, &x);
            JS_FreeCString(ctx, cs);
        }
        JS_FreeValue(ctx, text);
    }
    s = iv_step_apply(ctx, st, &r, x, have_value, n, up);
    if (concolic_is(value) || (argc >= 1 && concolic_is(argv[0]))) {
        /* ONE OF THE INPUTS WAS UNKNOWN, so where the value lands is unknown — including whether it moved at
           all, since steps 3, 4 and 10 all decide on the value. The element's value becomes that unknown,
           carrying whatever the real algorithm produced from the example as its own example, which is what
           keeps the source reaching §4.10.22.4's entry list through a stepped control. */
        JSValue unknown = concolic_builtin_hook(ctx, concolic_is(value) ? value : argv[0],
                                                up ? "stepUp" : "stepDown", s);

        JS_FreeValue(ctx, value);
        return iv_set_converted(ctx, this_val, unknown);
    }
    JS_FreeValue(ctx, value);
    /* Steps 3, 4 and 10's "return" — and step 11 having no string to write. A value the algorithm declined to
       move is LEFT ALONE, which is what returning from the middle of it means. */
    if (JS_IsUndefined(s)) return JS_UNDEFINED;
    return iv_set_converted(ctx, this_val, s);
}

/* §4.10.5.4's four members, declared once per AGENT beside `files` — see input_value_declare. */
static int g_id_value_as_number = -1;
static int g_id_value_as_date = -1;
static int g_id_step_up = -1;
static int g_id_step_down = -1;

void input_value_declare(JSContext *ctx)
{
    static const IdlArgType ONE_LONG[1] = { IDL_LONG };

    DCHECK(g_id_files_set < 0, "input_value_declare ran twice — its member is declared once per AGENT, and a "
                               "second declaration would mint the setter per realm");
    g_id_files_set = idl_setter_id(ctx, IDL_ANY, false, js_input_set_files, 0);
    /* `attribute unrestricted double valueAsNumber` — UNRESTRICTED, which is what makes the setter's NaN
       branch reachable at all: a plain `double` rejects NaN and the infinities at the boundary, and the
       algorithm's own "if the new value is infinite, then throw a TypeError" and "if the new value is a NaN
       value, then set the value of the element to the empty string" would both be dead. */
    g_id_value_as_number = idl_setter_id(ctx, IDL_UNRESTRICTED_DOUBLE, false, js_input_set_value_as_number, 0);
    /* `attribute object? valueAsDate` — passed through unconverted, because the declaration surface has no
       nullable-object type and the algorithm states the whole conversion itself (see the setter). */
    g_id_value_as_date = idl_setter_id(ctx, IDL_ANY, false, js_input_set_value_as_date, 0);
    /* `undefined stepUp(optional long n = 1)` and its twin. The argument is OPTIONAL FROM POSITION 0, so an
       absent one and an explicit `undefined` both leave the body to apply the IDL's default of 1. */
    g_id_step_up = idl_method_id(ctx, ONE_LONG, 1, js_input_step, IV_STEP_UP);
    idl_optional_from(0);
    g_id_step_down = idl_method_id(ctx, ONE_LONG, 1, js_input_step, IV_STEP_DOWN);
    idl_optional_from(0);
}

void input_value_install(JSContext *ctx, JSValueConst input_proto)
{
    DCHECK(g_id_files_set >= 0, "§4.10.5.4's `files` was installed before input_value_declare declared it");
    DCHECK(g_id_value_as_number >= 0 && g_id_value_as_date >= 0 && g_id_step_up >= 0 && g_id_step_down >= 0,
           "§4.10.5.4's value-as members were installed before input_value_declare declared them");
    DCHECK(JS_IsObject(input_proto), "`files` was installed with no HTMLInputElement.prototype");
    idl_install_accessor(ctx, input_proto, "files", js_input_get_files, 0, g_id_files_set);
    idl_install_accessor(ctx, input_proto, "valueAsNumber", js_input_get_value_as_number, 0,
                         g_id_value_as_number);
    idl_install_accessor(ctx, input_proto, "valueAsDate", js_input_get_value_as_date, 0, g_id_value_as_date);
    idl_install_method(ctx, input_proto, "stepUp", 0, g_id_step_up);
    idl_install_method(ctx, input_proto, "stepDown", 0, g_id_step_down);
}

void input_value_free(void)
{
    g_id_files_set = -1;
    g_id_value_as_number = -1;
    g_id_value_as_date = -1;
    g_id_step_up = -1;
    g_id_step_down = -1;
}
