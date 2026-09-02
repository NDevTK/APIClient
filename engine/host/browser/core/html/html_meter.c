/* HTML §4.10.14 "The meter element" — see html_meter.h for why the six numbers are one algorithm and why the
   two REFLECT_STRING rows they replace were a wrong member rather than a missing one. */
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "quickjs.h"
#include "solver/concolic.h"
#include "core/dom/element.h"
#include "core/dom/node.h"
#include "core/html/html_form.h"          /* §4.10.4's label association, which `labels` is the other side of */
#include "core/html/html_meter.h"
#include "core/html/number_microsyntax.h" /* §2.3.4.3, the rules §4.10.14 parses all six attributes with */
#include "core/idl_args.h"

/* THE SIX, INDEXED ONCE. A member of this interface is a POINT ON THE GAUGE (the minimum value, the maximum
   value, the actual value, the low boundary, the high boundary, the optimum point) and the point is NOT the
   attribute of the same name — two of the six do not read that attribute at all when it is absent (the low
   boundary defaults to the minimum VALUE, the high boundary to the maximum). What lets one index serve both is
   that §4.10.14 lists the attributes in the order it evaluates the points — "User agents must parse the min,
   max, value, low, high, and optimum attributes using the rules for parsing floating-point number values"
   — and then defines the points under headings in that same order, one per attribute; the six-heading run that
   stood here as one quotation was six separate headings joined by this file, which is why citegen.mjs could
   find no such sentence. And §2.6.2's reflected content
   attribute name is "the IDL attribute name converted to ASCII lowercase" — which for all six IS the name. A
   second table pairing them would be a hand-kept copy of that identity. */
enum { M_MINIMUM = 0, M_MAXIMUM, M_ACTUAL, M_LOW, M_HIGH, M_OPTIMUM, M_N };
static const char *const METER_NAME[M_N] = { "min", "max", "value", "low", "high", "optimum" };

static int g_id_set[M_N] = { -1, -1, -1, -1, -1, -1 };

/* A POINT ON THE GAUGE: the number, and WHICH OF THE SIX RAW ATTRIBUTES §4.10.14 READ TO PRODUCE IT, as a mask
 * of M_* bits over the caller's array of the six.
 *
 * A MASK AND NOT ONE `from`, BECAUSE NO POINT PAST THE FIRST HAS A SINGLE OPERAND THAT DECIDED IT. That is
 * what the residual which stood here asked for, and it asked too narrowly: it named only the optimum point's
 * default, "the candidate optimum point is the midpoint between the minimum value and the maximum value",
 * because that is the one number here spelled as arithmetic over two. But every clamp in this section is a
 * COMPARISON over two points, and the comparison reads both whichever one it keeps — `<meter min=A value=B>`
 * answers A when the clamp takes and B when it does not, so B is what decides whether the answer is A's number
 * at all. Naming only the point the number came out of gave two gauges differing in an operand it did not name
 * ONE derivation identity, so a flow's record of either decided the other's gate. solver/concolic.h's
 * `concolic_new_derived` takes the whole list.
 *
 * THE MASK IS ACCUMULATED BY THE ALGORITHM AND NEVER TABULATED. A per-point table naming which attributes
 * each point reads would be a second, hand-kept copy of the six paragraphs below — the same copy this file
 * already refuses to keep for the attribute names. Instead each combining step unions the masks of what it
 * combined, so the six answers fall out of the section's own evaluation order, and a step added or reordered
 * carries its reads with it.
 *
 * ORDERED, AND M_* IS THE ORDER — which is §4.10.14's own, "User agents must parse the min, max, value, low,
 * high, and optimum attributes using the rules for parsing floating-point number values". A sorted SET
 * would be the wrong identity for the same reason concolic_new_derived states: these operands play fixed
 * roles, and a `min` and a `max` that swapped places are a different gauge. */
typedef struct { double v; unsigned reads; } MeterPoint;

/* The M_* bit an attribute contributes to a point's read mask. */
#define METER_RD(i) (1u << (i))

/* THE NAMESPACE IS PART OF THE QUESTION — a `meter` in another namespace is a different element with a
   different interface, the same test core/html/html_base_element.c makes for `<base>`. */
static bool meter_element_is(const lxb_dom_node_t *n)
{
    size_t len = 0;
    const lxb_char_t *name;

    if (!n || n->type != LXB_DOM_NODE_TYPE_ELEMENT || n->ns != LXB_NS_HTML) return false;
    name = lxb_dom_element_local_name(lxb_dom_interface_element((lxb_dom_node_t *)n), &len);
    return name && len == 5 && memcmp(name, "meter", 5) == 0;
}

/* WEB IDL §3.7.6 Attributes' BRAND CHECK — the attribute getter's own steps, "If jsValue does not implement
   target, then:" … "Otherwise, throw a TypeError". What stood here was a paraphrase punctuated as a quotation
   (`this`, `the interface`), which engine/citegen.mjs reported; see core/html/html_progress.c, which carried
   the same sentence. */
static bool meter_receiver(JSContext *ctx, JSValueConst this_val, const char *member)
{
    if (meter_element_is(node_of(this_val))) return true;
    JS_ThrowTypeError(ctx, "HTMLMeterElement.%s was reached on something that is not a <meter> element", member);
    return false;
}

/* §4.10.14's "parse the … attributes using the rules for parsing floating-point number values", over ONE
   attribute value that may carry unknown external input: the REAL rules run on the concrete example, which is
   the same shape core/dom/element.c's el_reflect_ulong and element_reflect_url_get use. Answers whether a value could be
   parsed out of it — which is the exact phrase every one of the six points below branches on. */
static bool meter_parsed(JSContext *ctx, JSValueConst raw, double *out)
{
    JSValue concrete = concolic_is(raw) ? concolic_example(ctx, raw) : JS_DupValue(ctx, raw);
    bool ok = false;

    if (JS_IsString(concrete)) {
        size_t len = 0;
        const char *s = JS_ToCStringLen(ctx, &len, concrete);

        if (s) { ok = html_parse_floating_point(s, len, out, NULL); JS_FreeCString(ctx, s); }
    }
    JS_FreeValue(ctx, concrete);
    return ok;
}

/* §4.10.14's CANDIDATE FOR ONE POINT — the attribute's own parsed number when "a value could be parsed out of
   it", and otherwise whatever that paragraph's `dflt` is. The attribute is READ EITHER WAY, and its bit is set
   either way: a `max` nobody could parse a value out of is what sent the candidate to 1.0, so it decided the
   answer exactly as a parsable one would have. */
static MeterPoint meter_cand(JSContext *ctx, JSValueConst *raw, int i, MeterPoint dflt)
{
    MeterPoint r = dflt;
    double p = 0;

    if (meter_parsed(ctx, raw[i], &p)) r.v = p;
    r.reads = dflt.reads | METER_RD(i);
    return r;
}

/* §4.10.14's ONE-SIDED PICK, which only the maximum value ends in: "If the candidate maximum value is greater
   than or equal to the minimum value, then the maximum value is the candidate maximum value. Otherwise, the
   maximum value is the same as the minimum value."
   THE COMPARISON READ BOTH, WHICHEVER ONE IT KEPT, which is the whole reason the mask is unioned rather than
   taken from the winner: the loser is what decided that the winner won. */
static MeterPoint meter_pick(MeterPoint a, MeterPoint b, bool take_a)
{
    MeterPoint r = take_a ? a : b;

    r.reads = a.reads | b.reads;
    return r;
}

/* §4.10.14's TWO-SIDED CLAMP, which the other three points end in — the actual value, the low boundary and the
   optimum point word for word ("If the candidate … is less than the minimum value, then the … is the minimum
   value. Otherwise, if the candidate … is greater than the maximum value, then the … is the maximum value.
   Otherwise, the … is the candidate …"), and the HIGH BOUNDARY with one substitution.
   `lo` IS A PARAMETER AND NOT `out[M_MINIMUM]`, because the high boundary's lower bound is the LOW BOUNDARY:
   "If the candidate high boundary is less than the low boundary, then the high boundary is the low boundary."
   That is the one asymmetry in the six, and writing it as a fourth copy of the clamp is how it gets lost. */
static MeterPoint meter_clamp(MeterPoint cand, MeterPoint lo, MeterPoint hi)
{
    MeterPoint r = cand.v < lo.v ? lo : cand.v > hi.v ? hi : cand;

    r.reads = cand.reads | lo.reads | hi.reads;
    return r;
}

/* §4.10.14's SIX POINTS, in the order the section evaluates them, "as some of the values refer to earlier
 * ones". `raw` holds the six attribute values and `out` receives the six points, both indexed by M_*.
 *
 * EACH POINT'S MASK IS UNIONED BY THE STEP THAT PRODUCED IT — see MeterPoint. A maximum that was clamped up to
 * the minimum is a fact about BOTH attributes: about `min` because that is where its number came from, and
 * about `max` because that is what made the clamp take. */
static void meter_points(JSContext *ctx, JSValueConst *raw, MeterPoint *out)
{
    MeterPoint cand, mid;

    /* THE MINIMUM VALUE — "If the min attribute is specified and a value could be parsed out of it, then the
       minimum value is that value. Otherwise, the minimum value is zero." No earlier point to combine with,
       so this is the one point whose derivation names a single operand. */
    out[M_MINIMUM] = meter_cand(ctx, raw, M_MINIMUM, (MeterPoint){ 0, 0 });

    /* THE MAXIMUM VALUE — candidate 1.0 when unparsable, then the one-sided pick against the minimum. */
    cand = meter_cand(ctx, raw, M_MAXIMUM, (MeterPoint){ 1.0, 0 });
    out[M_MAXIMUM] = meter_pick(cand, out[M_MINIMUM], cand.v >= out[M_MINIMUM].v);

    /* THE ACTUAL VALUE — candidate zero when unparsable, then clamped into [minimum, maximum]. */
    cand = meter_cand(ctx, raw, M_ACTUAL, (MeterPoint){ 0, 0 });
    out[M_ACTUAL] = meter_clamp(cand, out[M_MINIMUM], out[M_MAXIMUM]);

    /* THE LOW BOUNDARY — candidate is "the same as the minimum value" when unparsable, then clamped into
       [minimum, maximum]. The default carries the minimum's OWN mask, so a low boundary that fell back to it
       names what the minimum was computed from and not merely the absent `low`. */
    cand = meter_cand(ctx, raw, M_LOW, out[M_MINIMUM]);
    out[M_LOW] = meter_clamp(cand, out[M_MINIMUM], out[M_MAXIMUM]);

    /* THE HIGH BOUNDARY — candidate is "the same as the maximum value" when unparsable, and its lower clamp is
       the LOW BOUNDARY rather than the minimum. */
    cand = meter_cand(ctx, raw, M_HIGH, out[M_MAXIMUM]);
    out[M_HIGH] = meter_clamp(cand, out[M_LOW], out[M_MAXIMUM]);

    /* THE OPTIMUM POINT — candidate is "the midpoint between the minimum value and the maximum value" when
       unparsable, then clamped into [minimum, maximum]. The midpoint is arithmetic over two points, so it is
       the case the residual on MeterPoint named, and it needs no special handling now: the default's mask is
       the union of the two it averages, and meter_cand and meter_clamp carry it the rest of the way. */
    mid.v = out[M_MINIMUM].v + (out[M_MAXIMUM].v - out[M_MINIMUM].v) / 2;
    mid.reads = out[M_MINIMUM].reads | out[M_MAXIMUM].reads;
    cand = meter_cand(ctx, raw, M_OPTIMUM, mid);
    out[M_OPTIMUM] = meter_clamp(cand, out[M_MINIMUM], out[M_MAXIMUM]);

    /* §4.10.14: "All of which will result in the following inequalities all being true". They are the section's
       own statement about its own algorithm, so a violation is this file's arithmetic and not the page's. */
    DCHECK(out[M_MINIMUM].v <= out[M_ACTUAL].v && out[M_ACTUAL].v <= out[M_MAXIMUM].v,
           "§4.10.14's `minimum value ≤ actual value ≤ maximum value` does not hold — the actual value's clamp "
           "reads the two bounds this function computed above it");
    DCHECK(out[M_MINIMUM].v <= out[M_LOW].v && out[M_LOW].v <= out[M_HIGH].v &&
           out[M_HIGH].v <= out[M_MAXIMUM].v,
           "§4.10.14's `minimum value ≤ low boundary ≤ high boundary ≤ maximum value` does not hold — the high "
           "boundary's LOWER clamp is the low boundary and not the minimum, which is the one asymmetry in the "
           "six and the one an out-of-order evaluation loses");
    DCHECK(out[M_MINIMUM].v <= out[M_OPTIMUM].v && out[M_OPTIMUM].v <= out[M_MAXIMUM].v,
           "§4.10.14's `minimum value ≤ optimum point ≤ maximum value` does not hold");
}

/* §4.10.14's six getters, each one sentence: "The value getter steps are to return this's actual value", "The
 * min getter steps are to return this's minimum value", and so on through the optimum point.
 *
 * THE DERIVATION NAMES EVERY ATTRIBUTE THE POINT WAS COMPUTED FROM, in M_* order, which is §4.10.14's own
 * parse order — see MeterPoint. `min` is the only one of the six that names a single operand, and it composes
 * exactly the bytes concolic_builtin_hook always composed for it, because that entry IS this one at n == 1. */
static JSValue js_meter_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSValueConst operands[M_N];
    JSValue raw[M_N], out;
    MeterPoint pt[M_N];
    int i, n = 0;

    DCHECK(magic >= 0 && magic < M_N, "a §4.10.14 getter was installed with a magic that names no gauge point");
    if (!meter_receiver(ctx, this_val, METER_NAME[magic])) return JS_EXCEPTION;
    for (i = 0; i < M_N; i++) {
        raw[i] = element_attr_get_value(ctx, this_val, METER_NAME[i]);
        if (JS_IsException(raw[i])) {
            while (i-- > 0) JS_FreeValue(ctx, raw[i]);
            return JS_EXCEPTION;
        }
    }
    meter_points(ctx, raw, pt);
    for (i = 0; i < M_N; i++)
        if (pt[magic].reads & METER_RD(i)) operands[n++] = raw[i];
    /* EVERY POINT READS AT LEAST ITS OWN ATTRIBUTE, parsable or not — meter_cand sets that bit on both arms,
       because an attribute nobody could parse a value out of is what sent the candidate to the paragraph's
       default. A point with an empty mask would be one this walk produced without reading anything. */
    DCHECK(n >= 1 && (pt[magic].reads & METER_RD(magic)) != 0,
           "a §4.10.14 gauge point was computed without reading the attribute of its own name — meter_cand "
           "sets that bit on the parsed and the unparsable arm alike, so an empty or foreign mask is a step "
           "in meter_points that assembled a point without going through it");
    /* An absent attribute is JS_NULL and a plain one is a String, both of which have an identity of their own,
       so a concrete operand narrows the key rather than erasing it: `<meter min=x max=2>` and `<meter min=x
       max=9>` are two derivations. JS_UNINITIALIZED means NO operand holds unknown external input, and then
       the number below is the whole answer. */
    out = concolic_new_derived(ctx, METER_NAME[magic], operands, n, JS_NewFloat64(ctx, pt[magic].v));
    if (JS_IsUninitialized(out)) out = JS_NewFloat64(ctx, pt[magic].v);
    for (i = 0; i < M_N; i++) JS_FreeValue(ctx, raw[i]);
    return out;
}

/* §4.10.14's `labels`: "The labels IDL attribute provides a list of the element's labels." §4.10.2 makes
   `meter` a labelable element, and the relation is core/html/html_form.h's. */
static JSValue js_meter_labels(JSContext *ctx, JSValueConst this_val, int magic)
{
    (void)magic;
    if (!meter_receiver(ctx, this_val, "labels")) return JS_EXCEPTION;
    return html_form_labels_of(ctx, this_val);
}

/* §2.6.1's double SETTER, which is all `[CEReactions, ReflectSetter]` asks of these six: none is limited to
   only positive numbers, so `meter.max = -1` WRITES "-1" and only the getters above clamp. */
static JSValue js_meter_set(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    DCHECK(magic >= 0 && magic < M_N, "a §4.10.14 setter was installed with a magic that names no gauge point");
    if (!meter_receiver(ctx, this_val, METER_NAME[magic])) return JS_EXCEPTION;
    element_reflect_double_set(ctx, this_val, METER_NAME[magic], METER_NAME[magic], val, false);
    return JS_UNDEFINED;
}

void html_meter_declare(JSContext *ctx)
{
    int i;

    DCHECK(g_id_set[0] < 0, "html_meter_declare ran twice in one agent");
    for (i = 0; i < M_N; i++) g_id_set[i] = idl_setter_id(ctx, IDL_DOUBLE, false, js_meter_set, i);
}

void html_meter_install(JSContext *ctx, JSValueConst proto)
{
    int i;

    DCHECK(g_id_set[0] >= 0,
           "§4.10.14's members were installed before html_meter_declare declared their setters");
    for (i = 0; i < M_N; i++)
        idl_install_accessor(ctx, proto, METER_NAME[i], js_meter_get, i, g_id_set[i]);
    idl_install_accessor(ctx, proto, "labels", js_meter_labels, 0, -1);
}
