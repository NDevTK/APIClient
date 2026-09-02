/* HTML §4.10.13 "The progress element" — see html_progress.h for why the four members are a component rather
   than rows of core/dom/element.c's reflection registry. */
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "quickjs.h"
#include "solver/concolic.h"
#include "core/dom/element.h"
#include "core/dom/node.h"
#include "core/html/html_form.h"          /* §4.10.4's label association, which `labels` is the other side of */
#include "core/html/html_progress.h"
#include "core/html/number_microsyntax.h" /* §2.3.4.3, the rules §4.10.13 parses `value` with */
#include "core/idl_args.h"

/* §4.10.13's one reflecting setter — `[CEReactions, ReflectSetter] attribute double value`. `max` needs none of
   its own: both of its directions are §2.6.1's, so its setter is declared with the same body. */
static int g_id_set_value = -1, g_id_set_max = -1;

enum { PG_VALUE = 0, PG_MAX, PG_POSITION };

/* THE NAMESPACE IS PART OF THE QUESTION — a `progress` in another namespace is a different element with a
   different interface, the same test core/html/html_base_element.c makes for `<base>`. */
static bool progress_element_is(const lxb_dom_node_t *n)
{
    size_t len = 0;
    const lxb_char_t *name;

    if (!n || n->type != LXB_DOM_NODE_TYPE_ELEMENT || n->ns != LXB_NS_HTML) return false;
    name = lxb_dom_element_local_name(lxb_dom_interface_element((lxb_dom_node_t *)n), &len);
    return name && len == 8 && memcmp(name, "progress", 8) == 0;
}

/* WEB IDL §3.7.6 Attributes' BRAND CHECK — the attribute getter's own steps, "If jsValue does not implement
   target, then:" … "Otherwise, throw a TypeError". A member is on a prototype and a page can call it on
   anything, so the receiver is a real question with a real spec answer rather than an invariant to assert.
   THE WORDS ARE THE SECTION'S NOW AND WERE THIS FILE'S BEFORE. What stood here was a true paraphrase
   punctuated as a quotation — it named the receiver and the interface in this engine's vocabulary rather than
   in §3.7.6's, whose steps say jsValue and target — and a quotation is the one thing a reader trusts most and
   verifies least. engine/citegen.mjs reported it. */
static lxb_dom_element_t *progress_receiver(JSContext *ctx, JSValueConst this_val, const char *member)
{
    lxb_dom_node_t *n = node_of(this_val);

    if (progress_element_is(n)) return lxb_dom_interface_element(n);
    JS_ThrowTypeError(ctx, "HTMLProgressElement.%s was reached on something that is not a <progress> element",
                      member);
    return NULL;
}

/* §4.10.13: "If the value attribute is omitted, then the progress bar is an indeterminate progress bar.
   Otherwise, it is a determinate progress bar." It is the attribute's PRESENCE and never its value, so
   `<progress value="x">` is DETERMINATE with a value of zero while `<progress>` answers `position` −1.
   ASKED OF THE ATTRIBUTE LIST for the reason core/dom/element.c's boolean reflection gives: lexbor leaves
   `attr->value` NULL for a valueless attribute, so a presence test over the value pointer reads `<progress
   value>` — which IS determinate — as the omitted case. */
static bool progress_determinate(lxb_dom_element_t *el)
{
    return lxb_dom_element_has_attribute(el, (const lxb_char_t *)"value", 5);
}

/* BOTH OF §4.10.13'S TWO COMPUTED READS NAME BOTH OPERANDS, IN THE ORDER THE SECTION NAMES THEM.
 *
 * Each of `value` and `position` is a function of the maximum value AND of the `value` attribute — "the
 * current value is the maximum value, if value is greater than the maximum value, and value otherwise" —
 * so neither has ONE operand that decided it: WHICH of the two the number came out of is itself decided by
 * the other. Naming only the one it came out of gave two `<progress>`es that differ in the operand it did not
 * name ONE derivation identity, and a flow's record of either then decided the other's gate. That is what the
 * residual which stood here asked for, and solver/concolic.h's `concolic_new_derived` is it: an ORDERED
 * operand list, which is the right identity here because these two answers are not symmetric in their
 * operands (`position` DIVIDES by the maximum) and because the operands play fixed roles that a sorted set
 * would let swap. `concolic_builtin_hook` is that entry's `n == 1` case, so §2.6.1's own one-operand `max`
 * getter still composes exactly the bytes it always did.
 *
 * THE ORDER IS THE SECTION'S: the sentence above names the maximum value first and the `value` attribute
 * second, and it is the one sentence both members are built on. It is a fact about the OPERAND LIST and not
 * about the arithmetic — `position` renders as `position({max}, {value})` while it divides the other way
 * round, exactly as `min({x}, {y})` renders a list rather than an expression. */

/* §4.10.13's "maximum value of the progress bar", which is also what its `max` member answers:
   `[CEReactions, ReflectPositive, ReflectDefault=1.0] attribute double max` is §2.6.1's double model in both
   directions, and its getter's steps ARE the section's own "if this does not result in an error, and if the
   parsed value is greater than zero, then the maximum value of the progress bar is that value. Otherwise … the
   maximum value of the progress bar is 1.0." Asked of core/dom/element.h so there is ONE implementation of the
   number all three members are defined over. OWNED — it may be a derivation of the `max` attribute. */
static JSValue progress_maximum(JSContext *ctx, JSValueConst this_val)
{
    return element_reflect_double_get(ctx, this_val, "max", "max", true, true, 1.0);
}

/* §4.10.13's "value of the progress bar" for a DETERMINATE bar: "user agents must parse the value attribute's
   value according to the rules for parsing floating-point number values. If this does not result in an error
   and the parsed value is greater than zero, then the value of the progress bar is that parsed value.
   Otherwise, if parsing the value attribute's value resulted in an error or a number less than or equal to
   zero, then the value of the progress bar is zero."
   `*praw` receives the attribute VALUE it was parsed from (OWNED), because that is the operand a derivation
   names.
   AN UNKNOWN WITH NO EXAMPLE HAS NO BYTES FOR THOSE RULES TO CONSUME, which is the section's own parse-error
   arm and not a hole this line defaults past — the same answer core/dom/element.c's §2.6.1 double getter
   gives, stated there, and the reason the zero it produces is a real example the derivation may carry. It is
   NOT the answer for the MAXIMUM, whose absence js_progress_get treats as an absence, because that one is a
   number rather than an attribute value and the section states no fallback arm for it. */
static double progress_value_of(JSContext *ctx, JSValueConst this_val, JSValue *praw)
{
    JSValue raw = element_attr_get_value(ctx, this_val, "value");
    JSValue concrete = concolic_is(raw) ? concolic_example(ctx, raw) : JS_DupValue(ctx, raw);
    double parsed = 0;
    bool ok = false;

    if (JS_IsString(concrete)) {
        size_t len = 0;
        const char *s = JS_ToCStringLen(ctx, &len, concrete);

        if (s) { ok = html_parse_floating_point(s, len, &parsed, NULL); JS_FreeCString(ctx, s); }
    }
    JS_FreeValue(ctx, concrete);
    *praw = raw;
    return (ok && parsed > 0) ? parsed : 0;
}

/* §4.10.13's THREE READS. `value`: "The value getter steps are to return 0 if this is an indeterminate progress
 * bar; otherwise this's current value." `position`: "If the progress bar is an indeterminate progress bar, then
 * the position IDL attribute must return −1. Otherwise, it must return the result of dividing the current value
 * by the maximum value." `max`: §2.6.1's double getter, above.
 *
 * THE CURRENT VALUE IS NOT THE VALUE OF THE PROGRESS BAR, and conflating them is what a page sees: "the current
 * value is the maximum value, if value is greater than the maximum value, and value otherwise", so
 * `<progress value=5 max=2>` reads `value` 2 and `position` 1 rather than 5 and 2.5. */
static JSValue js_progress_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    lxb_dom_element_t *el = progress_receiver(ctx, this_val,
                                              magic == PG_VALUE ? "value"
                                              : magic == PG_MAX ? "max" : "position");
    JSValueConst operands[2];
    JSValue maxv, valraw, out;
    double maximum = 0, value, current = 0, answer = 0;
    bool have;

    if (!el) return JS_EXCEPTION;
    DCHECK(magic == PG_VALUE || magic == PG_MAX || magic == PG_POSITION,
           "a §4.10.13 getter was installed with a magic that names none of its three reads — the ternary "
           "above answers `position` for every value that is neither of the other two");
    if (magic == PG_MAX) return progress_maximum(ctx, this_val);
    /* An INDETERMINATE bar has no `value` attribute at all, so there is no operand to derive from and both
       answers are the section's own constants. */
    if (!progress_determinate(el)) return JS_NewFloat64(ctx, magic == PG_VALUE ? 0 : -1);

    maxv = progress_maximum(ctx, this_val);
    if (JS_IsException(maxv)) return maxv;
    /* `idl_number_of` AND NOT A LOCAL COERCION, for its own reason: a body may not JS_ToFloat64 its way past
       unknown external input, and its answer for an unknown is the real conversion run on THAT VALUE'S OWN
       EXAMPLE. It returns 0 when there is no example, which is a positive statement this member has to
       answer for itself — see below. */
    have = idl_number_of(ctx, IDL_DOUBLE, maxv, &maximum) != 0;
    /* §4.10.13 makes the maximum value 1.0 wherever the `max` attribute is absent, unparsable, or parses to a
       number not greater than zero, so a maximum this engine HAS a number for is positive — which is what
       `position` divides by.
       AN ABSENT NUMBER IS NOT A VIOLATION OF THAT, AND THIS ASSERT USED TO SAY IT WAS. It read the example off
       the value and treated its absence as a zero maximum, and named as its cause `progress_maximum` having been
       asked without §2.6.1's [ReflectPositive]/[ReflectDefault] flags — which cannot happen, since this file
       is that call's only caller and passes them. What actually empties the example is the FLOW: `concolic_example` is per-flow, and a flow
       that has contradicted this value's example is handed nothing (solver/concolic.h), so an ordinary forced
       arm aborted the document with an assert blaming a line that was correct. There is no number, so there
       is no example, and the derivation below carries none rather than a fabricated 0. */
    DCHECK(!have || maximum > 0,
           "§4.10.13's maximum value of a progress bar came out non-positive WITH a number in hand — the "
           "section makes it 1.0 for an absent, unparsable or non-positive `max`, so a zero or negative here "
           "is element_reflect_double_get having answered against its own [ReflectPositive]/[ReflectDefault]");
    value = progress_value_of(ctx, this_val, &valraw);
    if (have) {
        current = value > maximum ? maximum : value;
        answer = magic == PG_VALUE ? current : current / maximum;
    }

    /* THE OPERANDS, IN §4.10.13'S OWN ORDER — see the note above `progress_maximum`. Both are named on every
       path, including the paths where one of them is a plain number: a concrete operand has an identity too
       (its value and its type), so naming it is what keeps `<progress value=x max=2>` and `<progress value=x
       max=9>` two derivations instead of one. */
    operands[0] = maxv;
    operands[1] = valraw;
    out = concolic_new_derived(ctx, magic == PG_VALUE ? "value" : "position", operands, 2,
                               have ? JS_NewFloat64(ctx, answer) : JS_UNDEFINED);
    if (JS_IsUninitialized(out)) {
        DCHECK(have,
               "§4.10.13's maximum value had no number although NEITHER operand holds unknown external input "
               "— a `max` attribute that is a plain string or absent reaches §2.6.1's parse-error arm and its "
               "1.0 default, so the two tests have come apart");
        out = JS_NewFloat64(ctx, answer);
    }
    JS_FreeValue(ctx, maxv);
    JS_FreeValue(ctx, valraw);
    return out;
}

/* §4.10.13's `labels`: "The labels IDL attribute provides a list of the element's labels." §4.10.2 makes
   `progress` a labelable element, and the relation is core/html/html_form.h's — written a second time here it
   would stop agreeing with the `for`/descendant rules that define it. */
static JSValue js_progress_labels(JSContext *ctx, JSValueConst this_val, int magic)
{
    (void)magic;
    if (!progress_receiver(ctx, this_val, "labels")) return JS_EXCEPTION;
    return html_form_labels_of(ctx, this_val);
}

/* §2.6.1's double SETTER for both writable members. `value` is `[ReflectSetter]` — not limited to positive
   numbers, so `progress.value = -1` WRITES "-1" and only the getter reads it back as zero. `max` is
   `[ReflectPositive]`, whose step 1 returns without writing for a value that is not greater than zero. */
static JSValue js_progress_set(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    const char *member = magic == PG_VALUE ? "value" : "max";

    DCHECK(magic == PG_VALUE || magic == PG_MAX,
           "§4.10.13 declares exactly two writable members and a setter was installed with a third magic");
    if (!progress_receiver(ctx, this_val, member)) return JS_EXCEPTION;
    element_reflect_double_set(ctx, this_val, member, member, val, magic == PG_MAX);
    return JS_UNDEFINED;
}

void html_progress_declare(JSContext *ctx)
{
    DCHECK(g_id_set_value < 0, "html_progress_declare ran twice in one agent");
    g_id_set_value = idl_setter_id(ctx, IDL_DOUBLE, false, js_progress_set, PG_VALUE);
    g_id_set_max = idl_setter_id(ctx, IDL_DOUBLE, false, js_progress_set, PG_MAX);
}

void html_progress_install(JSContext *ctx, JSValueConst proto)
{
    DCHECK(g_id_set_value >= 0 && g_id_set_max >= 0,
           "§4.10.13's members were installed before html_progress_declare declared their setters");
    idl_install_accessor(ctx, proto, "value", js_progress_get, PG_VALUE, g_id_set_value);
    idl_install_accessor(ctx, proto, "max", js_progress_get, PG_MAX, g_id_set_max);
    idl_install_accessor(ctx, proto, "position", js_progress_get, PG_POSITION, -1);
    idl_install_accessor(ctx, proto, "labels", js_progress_labels, 0, -1);
}
