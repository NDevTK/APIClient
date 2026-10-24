/* The shipped build's Intl host: the V8 of the frame this engine runs in. See intl_host.h. */
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <stdio.h>
#include <stdlib.h>

#include "check.h"
#include "core/intl/intl_host.h"
#include "core/json_buf.h"

/* The security argument for this import. It is serviced in the realm that instantiated this module, which is the
   engine's own sandboxed renderer frame (extension/renderer.html, opaque origin) and nothing else: the call is
   never relayed to the offscreen document or the service worker. That frame is already as untrusted as the page,
   and every web page can call V8's Intl, so the call grants nothing a page does not already have. The relay
   refuses a name outside the op list with an abort, the WASM build's CHECK. */
EM_JS_DEPS(intl_host_v8, "$UTF8ToString,$stringToUTF8,$lengthBytesUTF8");

EM_JS(int, intl_host_v8_relay, (const char *op, const char *request), {
    var self = intl_host_v8_relay;
    var name = UTF8ToString(Number(op));
    var req = JSON.parse(UTF8ToString(Number(request)));
    if (!self.ops) {
        var zones = new Map();
        var offsetMs = function (zone, t) {
            var f = zones.get(zone);
            if (!f) {
                f = new Intl.DateTimeFormat("en-US", { timeZone: zone, timeZoneName: "longOffset" });
                zones.set(zone, f);
            }
            var tzn = f.formatToParts(t).filter(function (p) { return p.type === "timeZoneName"; })[0].value;
            var m = /^GMT(?:([+-])([0-9][0-9]):([0-9][0-9])(?::([0-9][0-9]))?)?$/.exec(
                tzn.split(String.fromCharCode(0x2212)).join("-"));
            if (!m) abort("Intl host: an offset name " + tzn + " the relay cannot read");
            if (!m[1]) return 0;
            var ms = ((+m[2] * 60 + +m[3]) * 60 + +(m[4] || 0)) * 1000;
            return m[1] === "+" ? ms : -ms;
        };
        self.ops = Object.freeze({ __proto__: null,
            "TimeZone.offset": function (r) { return offsetMs(r.args[0], r.args[1]); },
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

static void json_value(JsonBuf *b, const IntlHostValue *v)
{
    char num[40];

    if (v->kind == INTL_VALUE_NUMBER) {
        snprintf(num, sizeof num, "%.17g", v->num);
        json_buf_raw(b, num);
        return;
    }
    DCHECK(v->kind == INTL_VALUE_STRING, "an Intl host argument of a kind intl_host.h does not list");
    json_buf_str(b, v->str);
}

char *intl_host_call(const IntlHostRequest *req)
{
    JsonBuf b = { 0 };
    char *text, *answer;
    int i, n;

    json_buf_raw(&b, "{");
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
