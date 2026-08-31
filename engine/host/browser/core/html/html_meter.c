/* HTML §4.10.14 "The meter element" — see html_meter.h for why the six numbers are one algorithm and why the
   two REFLECT_STRING rows they replace were a wrong member rather than a missing one. */
#include <math.h>
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
   that §4.10.14 lists the attributes in the order it evaluates the points ("User agents must parse the min,
   max, value, low, high, and optimum attributes…", then "The minimum value / The maximum value / The actual
   value / The low boundary / The high boundary / The optimum point"), and that §2.6.2's reflected content
   attribute name is "the IDL attribute name converted to ASCII lowercase" — which for all six IS the name. A
   second table pairing them would be a hand-kept copy of that identity. */
enum { M_MINIMUM = 0, M_MAXIMUM, M_ACTUAL, M_LOW, M_HIGH, M_OPTIMUM, M_N };
static const char *const METER_NAME[M_N] = { "min", "max", "value", "low", "high", "optimum" };

static int g_id_set[M_N] = { -1, -1, -1, -1, -1, -1 };

/* A POINT ON THE GAUGE: the number, and the attribute VALUE that DECIDED it. `from` is BORROWED out of the
   caller's array of the six raw attribute values, or JS_UNDEFINED when the number came from a constant §4.10.14
   supplies (zero, 1.0) rather than from anything the page wrote.
   NAMED RESIDUAL — WHAT IS NOT COVERED: the optimum point's default is "the midpoint between the minimum value
   and the maximum value", which is computed from TWO operands, and one `from` names one of them. WHAT THE NEXT
   DIFF BUILDS: a derivation that composes BOTH operands' identities the way solver/concolic.h's
   `concolic_rel_hook` composes its operator and both operands, called by js_meter_get below in place of the
   one-operand `concolic_builtin_hook`. HOW ITS ABSENCE WOULD SHOW: a `<meter>` whose `min` and `max` both hold unknown
   external input files ONE constraint entry for `optimum`, so a flow that pins `max` decides the gate a flow
   that pins `min` would have forked. */
typedef struct { double v; JSValueConst from; } MeterPoint;

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

/* WEB IDL §3.7.6's BRAND CHECK — "if `this` does not implement the interface, throw a TypeError". */
static bool meter_receiver(JSContext *ctx, JSValueConst this_val, const char *member)
{
    if (meter_element_is(node_of(this_val))) return true;
    JS_ThrowTypeError(ctx, "HTMLMeterElement.%s was reached on something that is not a <meter> element", member);
    return false;
}

/* §4.10.14's "parse the … attributes using the rules for parsing floating-point number values", over ONE
   attribute value that may carry unknown external input: the REAL rules run on the concrete example, which is
   the same shape core/dom/element.c's el_reflect_ulong and el_reflect_url use. Answers whether a value could be
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

/* §4.10.14's SIX POINTS, in the order the section evaluates them, "as some of the values refer to earlier
 * ones". `raw` holds the six attribute values and `out` receives the six points, both indexed by M_*.
 *
 * EACH POINT'S `from` IS THE OPERAND THAT DECIDED IT, which is what the derivation names: a maximum that was
 * clamped up to the minimum is a fact about the `min` attribute and not about the `max` one, and answering
 * otherwise would give two flows that differ in `min` one identity for `max`. */
static void meter_points(JSContext *ctx, JSValueConst *raw, MeterPoint *out)
{
    MeterPoint cand;
    double p = 0;

    /* THE MINIMUM VALUE — "If the min attribute is specified and a value could be parsed out of it, then the
       minimum value is that value. Otherwise, the minimum value is zero." */
    out[M_MINIMUM] = meter_parsed(ctx, raw[M_MINIMUM], &p)
                    ? (MeterPoint){ p, raw[M_MINIMUM] } : (MeterPoint){ 0, JS_UNDEFINED };

    /* THE MAXIMUM VALUE — candidate 1.0 when unparsable, then "if the candidate maximum value is greater than
       or equal to the minimum value, then the maximum value is the candidate maximum value. Otherwise, the
       maximum value is the same as the minimum value." */
    cand = meter_parsed(ctx, raw[M_MAXIMUM], &p) ? (MeterPoint){ p, raw[M_MAXIMUM] }
                                              : (MeterPoint){ 1.0, JS_UNDEFINED };
    out[M_MAXIMUM] = cand.v >= out[M_MINIMUM].v ? cand : out[M_MINIMUM];

    /* THE ACTUAL VALUE — candidate zero when unparsable, then clamped into [minimum, maximum]. */
    cand = meter_parsed(ctx, raw[M_ACTUAL], &p) ? (MeterPoint){ p, raw[M_ACTUAL] }
                                                : (MeterPoint){ 0, JS_UNDEFINED };
    out[M_ACTUAL] = cand.v < out[M_MINIMUM].v ? out[M_MINIMUM]
                   : cand.v > out[M_MAXIMUM].v ? out[M_MAXIMUM] : cand;

    /* THE LOW BOUNDARY — candidate is "the same as the minimum value" when unparsable, then clamped into
       [minimum, maximum]. */
    cand = meter_parsed(ctx, raw[M_LOW], &p) ? (MeterPoint){ p, raw[M_LOW] } : out[M_MINIMUM];
    out[M_LOW] = cand.v < out[M_MINIMUM].v ? out[M_MINIMUM]
                : cand.v > out[M_MAXIMUM].v ? out[M_MAXIMUM] : cand;

    /* THE HIGH BOUNDARY — candidate is "the same as the maximum value" when unparsable, and its lower clamp is
       the LOW BOUNDARY rather than the minimum: "If the candidate high boundary is less than the low boundary,
       then the high boundary is the low boundary." That is the one asymmetry in the six and it is why the
       evaluation order is load-bearing. */
    cand = meter_parsed(ctx, raw[M_HIGH], &p) ? (MeterPoint){ p, raw[M_HIGH] } : out[M_MAXIMUM];
    out[M_HIGH] = cand.v < out[M_LOW].v ? out[M_LOW]
                 : cand.v > out[M_MAXIMUM].v ? out[M_MAXIMUM] : cand;

    /* THE OPTIMUM POINT — candidate is "the midpoint between the minimum value and the maximum value" when
       unparsable, then clamped into [minimum, maximum]. The midpoint is the one number here computed from two
       operands; see MeterPoint's residual for what naming one of them costs. */
    if (meter_parsed(ctx, raw[M_OPTIMUM], &p)) {
        cand.v = p;
        cand.from = raw[M_OPTIMUM];
    } else {
        cand.v = out[M_MINIMUM].v + (out[M_MAXIMUM].v - out[M_MINIMUM].v) / 2;
        cand.from = concolic_is(out[M_MINIMUM].from) ? out[M_MINIMUM].from : out[M_MAXIMUM].from;
    }
    out[M_OPTIMUM] = cand.v < out[M_MINIMUM].v ? out[M_MINIMUM]
                    : cand.v > out[M_MAXIMUM].v ? out[M_MAXIMUM] : cand;

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
 * min getter steps are to return this's minimum value", and so on through the optimum point. */
static JSValue js_meter_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSValue raw[M_N], out;
    MeterPoint pt[M_N];
    int i;

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
    out = JS_NewFloat64(ctx, pt[magic].v);
    if (concolic_is(pt[magic].from))
        out = concolic_builtin_hook(ctx, pt[magic].from, METER_NAME[magic], out);
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
