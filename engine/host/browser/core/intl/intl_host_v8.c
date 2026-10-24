/* The shipped build's Intl host: the V8 of the frame this engine runs in. See intl_host.h.
 *
 * Each op is answered by V8's own Intl over the request: a service built from the instance's resolved slots
 * (kept per spelling, as the native host keeps its ICU formatters), or the resolvedOptions of one built from
 * what the engine read. */
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "core/intl/intl_host.h"
#include "core/json_buf.h"

/* The security argument for this import. It is serviced in the realm that instantiated this module, which is the
   engine's own sandboxed renderer frame (extension/renderer.html, opaque origin) and nothing else: the call is
   never relayed to the offscreen document or the service worker. That frame is already as untrusted as the page,
   and every web page can call V8's Intl, so the call grants nothing a page does not already have. The relay
   refuses a name outside the op list with an abort, the WASM build's CHECK. */
/* Named residual: V8 resolves a Link to CLDR's canonical name (Asia/Calcutta) where ECMA-402 §6.5.1
   AvailableNamedTimeZoneIdentifiers ( ) names the IANA primary (Asia/Kolkata), in TimeZone.primaryIdentifier and
   in Intl.supportedValuesOf("timeZone"). The next diff builds a generated Link-to-primary table from the tz release
   both hosts pin, which this relay applies to V8's answer. Its absence shows as the two hosts answering a Link
   zone's resolvedOptions().timeZone differently. */
EM_JS_DEPS(intl_host_v8, "$UTF8ToString,$stringToUTF8,$lengthBytesUTF8");

EM_JS(int, intl_host_v8_relay, (const char *op, const char *request), {
    var self = intl_host_v8_relay;
    var name = UTF8ToString(Number(op));
    var req = JSON.parse(UTF8ToString(Number(request)));
    if (!self.ops) {
        var memo = new Map();
        var service = function (ctor, locales, options) {
            var key = JSON.stringify([ctor, locales, options]);
            var s = memo.get(key);
            if (!s) { s = new Intl[ctor](locales, options); memo.set(key, s); }
            return s;
        };
        var pick = function (from, keys) {
            var o = {};
            keys.forEach(function (k) { if (from[k] !== undefined) o[k] = from[k]; });
            return o;
        };
        var COMPONENTS = ["weekday", "era", "year", "month", "day", "dayPeriod", "hour", "minute", "second",
                          "fractionalSecondDigits", "timeZoneName"];
        var number = function (v) {
            return v === "NaN" ? NaN : v === "Infinity" ? Infinity : v === "-Infinity" ? -Infinity : v;
        };
        var numberOptions = function (o) {
            var n = pick(o, ["numberingSystem", "style", "currency", "currencyDisplay", "currencySign", "unit",
                             "unitDisplay", "minimumIntegerDigits", "useGrouping", "notation", "compactDisplay",
                             "signDisplay", "roundingIncrement", "roundingMode", "trailingZeroDisplay"]);
            var t = o.roundingType;
            if (t !== "fractionDigits") {
                n.minimumSignificantDigits = o.minimumSignificantDigits;
                n.maximumSignificantDigits = o.maximumSignificantDigits;
            }
            if (t !== "significantDigits") {
                n.minimumFractionDigits = o.minimumFractionDigits;
                n.maximumFractionDigits = o.maximumFractionDigits;
            }
            if (t === "morePrecision" || t === "lessPrecision") n.roundingPriority = t;
            return n;
        };
        var dateOptions = function (o) {
            return pick(o, ["calendar", "numberingSystem", "timeZone", "hourCycle", "dateStyle", "timeStyle"]
                           .concat(COMPONENTS));
        };
        var withoutHc = function (tag) {
            return tag.replace(/-hc-h[0-9][0-9]/, "").replace(/-u(?=-x-|$)/, "");
        };
        var hourCycle = function (locale, h12) {
            var o = { hour: "numeric" };
            if (h12 !== undefined) o.hour12 = h12;
            return new Intl.DateTimeFormat(locale, o).resolvedOptions().hourCycle;
        };
        var offsetMs = function (zone, t) {
            var f = service("DateTimeFormat", ["en-US"], { timeZone: zone, timeZoneName: "longOffset" });
            var tzn = f.formatToParts(t).filter(function (p) { return p.type === "timeZoneName"; })[0].value;
            var m = /^GMT(?:([+-])([0-9][0-9]):([0-9][0-9])(?::([0-9][0-9]))?)?$/.exec(
                tzn.split(String.fromCharCode(0x2212)).join("-"));
            if (!m) abort("Intl host: an offset name " + tzn + " the relay cannot read");
            if (!m[1]) return 0;
            var ms = ((+m[2] * 60 + +m[3]) * 60 + +(m[4] || 0)) * 1000;
            return m[1] === "+" ? ms : -ms;
        };
        self.ops = Object.freeze({ __proto__: null,
            "Locale.canonicalize": function (r) { return Intl.getCanonicalLocales(r.args[0])[0]; },
            "Locale.supported": function (r) {
                return Intl[r.options.service].supportedLocalesOf(r.locales,
                                                                   { localeMatcher: r.options.localeMatcher });
            },
            "Collator.resolve": function (r) {
                var o = r.options;
                var ro = new Intl.Collator(r.locales, {
                    localeMatcher: o.localeMatcher, usage: o.usage, collation: o.co,
                    numeric: o.kn === undefined ? undefined : o.kn === "true", caseFirst: o.kf
                }).resolvedOptions();
                return { locale: ro.locale, collation: ro.collation, numeric: ro.numeric, caseFirst: ro.caseFirst,
                         sensitivity: o.usage === "search" ? ro.sensitivity : "variant",
                         ignorePunctuation: ro.ignorePunctuation };
            },
            "Collator.compare": function (r) {
                var c = service("Collator", r.locales, pick(r.options, ["usage", "sensitivity", "ignorePunctuation",
                                                                         "collation", "numeric", "caseFirst"]));
                var v = c.compare(r.args[0], r.args[1]);
                return v < 0 ? -1 : v > 0 ? 1 : 0;
            },
            "NumberFormat.resolve": function (r) {
                var ro = new Intl.NumberFormat(r.locales, { localeMatcher: r.options.localeMatcher,
                                                            numberingSystem: r.options.nu }).resolvedOptions();
                return { locale: ro.locale, numberingSystem: ro.numberingSystem };
            },
            "NumberFormat.currencyDigits": function (r) {
                return new Intl.NumberFormat("en", { style: "currency", currency: r.args[0] })
                    .resolvedOptions().maximumFractionDigits;
            },
            "NumberFormat.format": function (r) {
                return service("NumberFormat", r.locales, numberOptions(r.options)).format(number(r.args[0]));
            },
            "NumberFormat.formatToParts": function (r) {
                return service("NumberFormat", r.locales, numberOptions(r.options)).formatToParts(number(r.args[0]));
            },
            "TimeZone.primaryIdentifier": function (r) {
                return service("DateTimeFormat", ["en-US"], { timeZone: r.args[0] }).resolvedOptions().timeZone;
            },
            "TimeZone.offset": function (r) { return offsetMs(r.args[0], r.args[1]); },
            "DateTimeFormat.resolve": function (r) {
                var o = r.options, opts = { localeMatcher: o.localeMatcher, calendar: o.ca, numberingSystem: o.nu,
                                            hour: "numeric" };
                if (typeof o.hc === "string") opts.hourCycle = o.hc;
                if (o.hc === null) opts.hour12 = true;   /* only to drop the extension's hc, as §11.1.2 step 3 does */
                var ro = new Intl.DateTimeFormat(r.locales, opts).resolvedOptions();
                var stated = o.hc !== null && (typeof o.hc === "string" || ro.locale.indexOf("-hc-") >= 0);
                var base = withoutHc(ro.locale);
                return { locale: ro.locale, calendar: ro.calendar, numberingSystem: ro.numberingSystem,
                         hc: stated ? ro.hourCycle : null, hourCycle: hourCycle(base),
                         hourCycle12: hourCycle(base, true), hourCycle24: hourCycle(base, false) };
            },
            "DateTimeFormat.pattern": function (r) {
                var o = dateOptions(r.options);
                o.timeZone = "UTC";
                if (r.options.formatMatcher !== undefined) o.formatMatcher = r.options.formatMatcher;
                var ro = new Intl.DateTimeFormat(r.locales, o).resolvedOptions();
                var res = { hasHour: ro.hourCycle !== undefined };
                COMPONENTS.forEach(function (k) { if (ro[k] !== undefined) res[k] = ro[k]; });
                return res;
            },
            "DateTimeFormat.format": function (r) {
                return service("DateTimeFormat", r.locales, dateOptions(r.options)).format(r.args[0]);
            },
            "DateTimeFormat.formatToParts": function (r) {
                return service("DateTimeFormat", r.locales, dateOptions(r.options)).formatToParts(r.args[0]);
            },
            "Intl.supportedValuesOf": function (r) { return Intl.supportedValuesOf(r.args[0]); },
            "Host.dataVersion": function () {
                var v = typeof process === "object" && process.versions ? process.versions : null;
                return { host: v ? "v8 " + v.v8 : navigator.userAgent,
                         icu: v ? v.icu : null, cldr: v ? v.cldr : null, tz: v ? v.tz : null };
            }
        });
    }
    if (!Object.prototype.hasOwnProperty.call(self.ops, name))
        abort("Intl host: the relay was asked " + name + ", which is not one of the engine's Intl questions");
    var answer;
    try {
        answer = { ok: self.ops[name](req) };
    } catch (e) {
        if (!(e instanceof RangeError) && !(e instanceof TypeError)) throw e;
        answer = { "throw": e.name, message: String(e.message) };
    }
    self.answer = JSON.stringify(answer);
    return lengthBytesUTF8(self.answer);
});

EM_JS(void, intl_host_v8_take, (char *out, int cap), {
    stringToUTF8(intl_host_v8_relay.answer, Number(out), cap);
    intl_host_v8_relay.answer = null;
});

/* A JSON string of `n` bytes, which may hold a NUL a page's string carried. */
static void json_str_len(JsonBuf *b, const char *s, size_t n)
{
    char *z = malloc(n + 1), esc[8];
    size_t i, start = 0;

    CHECK(z != NULL, "an Intl request string could not be copied");
    json_buf_raw(b, "\"");
    for (i = 0; i <= n; i++) {
        if (i < n && s[i] != '\0' && s[i] != '"' && s[i] != '\\' && (unsigned char)s[i] >= 0x20) continue;
        memcpy(z, s + start, i - start);
        z[i - start] = '\0';
        json_buf_raw(b, z);
        if (i < n) {
            snprintf(esc, sizeof esc, "\\u%04x", (unsigned char)s[i]);
            json_buf_raw(b, esc);
        }
        start = i + 1;
    }
    json_buf_raw(b, "\"");
    free(z);
}

/* A value as JSON. A number JSON cannot spell (NaN, an infinity) travels as its ECMAScript String, which the
   relay turns back into the Number. */
static void json_value(JsonBuf *b, const IntlHostValue *v)
{
    char num[40];

    switch (v->kind) {
    case INTL_VALUE_NUMBER:
        if (isnan(v->num)) { json_buf_str(b, "NaN"); return; }
        if (isinf(v->num)) { json_buf_str(b, v->num > 0 ? "Infinity" : "-Infinity"); return; }
        snprintf(num, sizeof num, "%.17g", v->num);
        json_buf_raw(b, num);
        return;
    case INTL_VALUE_STRING: json_str_len(b, v->str, v->len); return;
    case INTL_VALUE_BOOLEAN: json_buf_raw(b, v->boolean ? "true" : "false"); return;
    case INTL_VALUE_NULL: json_buf_raw(b, "null"); return;
    }
    CHECK_FAIL("an Intl host value of a kind intl_host.h does not list");
}

char *intl_host_call(const IntlHostRequest *req)
{
    JsonBuf b = { 0 };
    char *text, *answer;
    int i, n;

    json_buf_raw(&b, "{");
    json_buf_key(&b, "locales");
    json_buf_raw(&b, "[");
    for (i = 0; i < req->nlocales; i++) {
        if (i) json_buf_raw(&b, ",");
        json_buf_str(&b, req->locales[i]);
    }
    json_buf_raw(&b, "],");
    json_buf_key(&b, "options");
    json_buf_raw(&b, "{");
    for (i = 0; i < req->nfields; i++) {
        if (i) json_buf_raw(&b, ",");
        json_buf_str(&b, req->fields[i].key);   /* a key the engine's slot and option tables name */
        json_buf_raw(&b, ":");
        json_value(&b, &req->fields[i].value);
    }
    json_buf_raw(&b, "},");
    json_buf_key(&b, "args");
    json_buf_raw(&b, "[");
    for (i = 0; i < req->nargs; i++) {
        if (i) json_buf_raw(&b, ",");
        json_value(&b, &req->args[i]);
    }
    json_buf_raw(&b, "]}");
    text = json_buf_take(&b);
    n = intl_host_v8_relay(intl_host_op_name(req->op), text);
    free(text);
    CHECK(n > 0, "the V8 Intl relay answered with no JSON document at all");
    answer = malloc((size_t)n + 1);
    CHECK(answer != NULL, "the V8 Intl relay's answer could not be allocated");
    intl_host_v8_take(answer, n + 1);
    return answer;
}

#endif
