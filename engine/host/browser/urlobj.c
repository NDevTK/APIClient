/* URL + URLSearchParams objects — endpoint construction. `new URL(path, base).href|pathname|searchParams`
 * is how a huge share of bundles build request URLs, and URLSearchParams serializes query/body params (its
 * CONCOLIC serializer, js_sp_tostring, is shared with FormData). Concrete input is resolved by the REAL Lexbor
 * parser; opaque/holey input keeps its shape (RUN-DON'T-MATCH). See urlobj.h. */
#include <string.h>
#include <stdlib.h>
#include "urlobj.h"
#include "url.h"      /* has_hole */
#include "opaque.h"   /* g_opaque, js_noop */

extern char *url_resolve(const char *input, const char *base);   /* Lexbor-canonical resolve (main.c) */
extern const char *g_origin;                                     /* the page principal (main.c) */

static void url_set(JSContext *ctx, JSValue o, const char *k, const char *s, size_t n) {
    JS_SetPropertyStr(ctx, o, k, JS_NewStringLen(ctx, s, n));
}
static JSValue js_url_tostring(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{ return JS_GetPropertyStr(ctx, this_val, "href"); }
/* url.searchParams shares the concolic __fields machinery with standalone URLSearchParams (js_sp_*), PLUS a
   back-ref to its owner URL so set/append/delete REBUILD the owner's href/search -> fetch(url) emits the
   app-BUILT query (a query the app SETS is the app constructing the request: concrete, not external input).
   Reads stay opaque for unknown keys (external query input). Defs live with the sp machinery below. */
static JSValue js_sp_get(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
static void sp_init(JSContext *ctx, JSValueConst o, JSValueConst init);
static JSValue js_url_sp_set(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
static JSValue js_url_sp_delete(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
/* __fields iteration, shared by url.searchParams / standalone URLSearchParams / Headers — a MISSING forEach or
   keys/values/entries throws, and uncaught in boot that discards the whole page (fatal @WHY). */
static JSValue js_sp_forEach(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
static JSValue js_sp_keys(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
static JSValue js_sp_values(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
static JSValue js_sp_entries(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
JSValue js_url_ctor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv) {
    /* opaque (external-input-tainted) URL -> return the INPUT opaque so its SHAPE flows through unchanged
       (never concretely resolved — RUN-DON'T-MATCH). A concrete input is resolved by the REAL Lexbor parser. */
    if (argc >= 1 && JS_IsOpaque(argv[0])) return JS_DupValue(ctx, argv[0]);
    const char *input = argc >= 1 ? JS_ToCString(ctx, argv[0]) : NULL;
    const char *base  = argc >= 2 && JS_IsString(argv[1]) ? JS_ToCString(ctx, argv[1]) : NULL;
    char *resolved = (input && !has_hole(input)) ? url_resolve(input, base ? base : g_origin) : NULL;
    JSValue shaped = (input && has_hole(input)) ? JS_NewOpaqueShaped(ctx, input) : JS_UNDEFINED;  /* {}-shape string -> keep shape */
    if (input) JS_FreeCString(ctx, input);
    if (base) JS_FreeCString(ctx, base);
    if (!resolved) return JS_IsUndefined(shaped) ? js_concolic(ctx, "{url}", JS_UNDEFINED) : shaped;   /* shape/parse-fail -> opaque, never invent */
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "href", JS_NewString(ctx, resolved));
    /* Extract components from Lexbor's CANONICAL output (scheme://host/path?query#frag) — trivial split of a
       spec-parsed string, NOT a resolver reimplementation (Lexbor did the resolution). */
    const char *scol = strstr(resolved, "://");
    const char *host = scol ? scol + 3 : resolved;
    url_set(ctx, o, "protocol", resolved, scol ? (size_t)(scol - resolved) + 1 : 0);
    const char *pe = host; while (*pe && *pe != '/' && *pe != '?' && *pe != '#') pe++;
    url_set(ctx, o, "host", host, (size_t)(pe - host));
    url_set(ctx, o, "hostname", host, (size_t)(pe - host));
    url_set(ctx, o, "origin", resolved, (size_t)(pe - resolved));
    const char *path = pe; const char *q = strchr(path, '?'); const char *hsh = strchr(path, '#');
    const char *pend = q ? q : (hsh ? hsh : path + strlen(path));
    url_set(ctx, o, "pathname", *path ? path : "/", *path ? (size_t)(pend - path) : 1);
    if (q) { const char *qe = hsh ? hsh : q + strlen(q); url_set(ctx, o, "search", q, (size_t)(qe - q)); }
    else JS_SetPropertyStr(ctx, o, "search", JS_NewString(ctx, ""));
    if (hsh) JS_SetPropertyStr(ctx, o, "hash", JS_NewString(ctx, hsh)); else JS_SetPropertyStr(ctx, o, "hash", JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, o, "port", JS_NewString(ctx, ""));
    free(resolved);
    JSValue sp = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, sp, "__fields", JS_NewObject(ctx));
    { JSValue sv = JS_GetPropertyStr(ctx, o, "search"); sp_init(ctx, sp, sv); JS_FreeValue(ctx, sv); }   /* seed from the URL's existing query ("?a=1") */
    JS_SetPropertyStr(ctx, sp, "__owner", JS_DupValue(ctx, o));   /* back-ref for writeback (o<->sp cycle; cycle-GC collected) */
    JS_SetPropertyStr(ctx, sp, "get", JS_NewCFunction(ctx, js_sp_get, "get", 1));
    JS_SetPropertyStr(ctx, sp, "getAll", JS_NewCFunction(ctx, js_sp_get, "getAll", 1));
    JS_SetPropertyStr(ctx, sp, "has", JS_NewCFunction(ctx, js_sp_get, "has", 1));
    JS_SetPropertyStr(ctx, sp, "set", JS_NewCFunction(ctx, js_url_sp_set, "set", 2));
    JS_SetPropertyStr(ctx, sp, "append", JS_NewCFunction(ctx, js_url_sp_set, "append", 2));
    JS_SetPropertyStr(ctx, sp, "delete", JS_NewCFunction(ctx, js_url_sp_delete, "delete", 1));
    JS_SetPropertyStr(ctx, sp, "sort", JS_NewCFunction(ctx, js_noop, "sort", 0));
    JS_SetPropertyStr(ctx, sp, "forEach", JS_NewCFunction(ctx, js_sp_forEach, "forEach", 1));
    JS_SetPropertyStr(ctx, sp, "keys", JS_NewCFunction(ctx, js_sp_keys, "keys", 0));
    JS_SetPropertyStr(ctx, sp, "values", JS_NewCFunction(ctx, js_sp_values, "values", 0));
    JS_SetPropertyStr(ctx, sp, "entries", JS_NewCFunction(ctx, js_sp_entries, "entries", 0));
    JS_SetPropertyStr(ctx, sp, "toString", JS_NewCFunction(ctx, js_sp_tostring, "toString", 0));
    JS_SetPropertyStr(ctx, o, "searchParams", sp);
    JS_SetPropertyStr(ctx, o, "toString", JS_NewCFunction(ctx, js_url_tostring, "toString", 0));
    JS_SetPropertyStr(ctx, o, "toJSON", JS_NewCFunction(ctx, js_url_tostring, "toJSON", 0));
    return o;
}
/* URL.canParse(input, base): a static bool used to guard `new URL()`. Opaque/holey input -> true (don't
   collapse exploration on external input); concrete -> whether Lexbor actually parses it. */
JSValue js_url_canparse(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc < 1) return JS_FALSE;
    if (JS_IsOpaque(argv[0])) return JS_TRUE;
    const char *input = JS_ToCString(ctx, argv[0]);
    const char *base = (argc >= 2 && JS_IsString(argv[1])) ? JS_ToCString(ctx, argv[1]) : NULL;
    int ok = 0;
    if (input && has_hole(input)) ok = 1;
    else if (input) { char *r = url_resolve(input, base ? base : g_origin); if (r) { ok = 1; free(r); } }
    if (input) JS_FreeCString(ctx, input);
    if (base) JS_FreeCString(ctx, base);
    return ok ? JS_TRUE : JS_FALSE;
}
extern JSValue js_el_self(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);   /* clone stub, borrowed from main.c */
/* new Request(input, init): fetch(new Request(url,{method,headers,body})) is a ubiquitous modern idiom. The
   Request must carry method + HEADERS + BODY forward so fetch(req) captures the full endpoint spec (required
   auth/CSRF headers, request body) — not just the url. Resolve the url shape-aware (like URL). */
JSValue js_request_ctor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv) {
    /* opaque input -> return the input opaque (url shape flows); concrete -> Lexbor-resolved url. */
    if (argc >= 1 && JS_IsOpaque(argv[0])) return JS_DupValue(ctx, argv[0]);
    const char *input = argc >= 1 ? JS_ToCString(ctx, argv[0]) : NULL;
    char *resolved = (input && !has_hole(input)) ? url_resolve(input, g_origin) : NULL;
    JSValue rshaped = (input && has_hole(input)) ? JS_NewOpaqueShaped(ctx, input) : JS_UNDEFINED;
    if (input) JS_FreeCString(ctx, input);
    if (!resolved) return JS_IsUndefined(rshaped) ? js_concolic(ctx, "{url}", JS_UNDEFINED) : rshaped;
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "url", JS_NewString(ctx, resolved));
    JS_SetPropertyStr(ctx, o, "href", JS_NewString(ctx, resolved));   /* toString reads href -> fetch(req) sees the url */
    free(resolved);
    JSValue method = JS_UNDEFINED;
    if (argc >= 2 && JS_IsObject(argv[1])) {
        method = JS_GetPropertyStr(ctx, argv[1], "method");
        JSValue h = JS_GetPropertyStr(ctx, argv[1], "headers");   /* -> fetch(req) capture_headers */
        if (!JS_IsUndefined(h) && !JS_IsNull(h)) JS_SetPropertyStr(ctx, o, "headers", h); else JS_FreeValue(ctx, h);
        JSValue b = JS_GetPropertyStr(ctx, argv[1], "body");      /* -> fetch(req) ep.body */
        if (!JS_IsUndefined(b) && !JS_IsNull(b)) JS_SetPropertyStr(ctx, o, "body", b); else JS_FreeValue(ctx, b);
    }
    JS_SetPropertyStr(ctx, o, "method", JS_IsString(method) ? method : JS_NewString(ctx, "GET"));
    if (!JS_IsString(method)) JS_FreeValue(ctx, method);
    /* body-reading methods a bundle may call on the Request (opaque = external body); clone -> self. */
    JS_SetPropertyStr(ctx, o, "clone", JS_NewCFunction(ctx, js_el_self, "clone", 0));
    JS_SetPropertyStr(ctx, o, "json", JS_NewCFunction(ctx, js_opaque_stub, "json", 0));
    JS_SetPropertyStr(ctx, o, "text", JS_NewCFunction(ctx, js_opaque_stub, "text", 0));
    JS_SetPropertyStr(ctx, o, "arrayBuffer", JS_NewCFunction(ctx, js_opaque_stub, "arrayBuffer", 0));
    JS_SetPropertyStr(ctx, o, "formData", JS_NewCFunction(ctx, js_opaque_stub, "formData", 0));
    JS_SetPropertyStr(ctx, o, "toString", JS_NewCFunction(ctx, js_url_tostring, "toString", 0));
    return o;
}
/* (The generic js_webobj_ctor opaque-stub constructor was DELETED: every interface it once served — Headers,
   FormData, Blob, File, Response, AbortController, TextEncoder/Decoder, FileReader — now has a real
   spec-shaped component, so the shape-drifting bag that read every unlisted member as undefined is gone.) */
/* URLSearchParams: a REAL object recording init + append()/set() fields (like FormData), so
   `new URLSearchParams({team:cfg.team,r:cfg.region}).toString()` -> a CONCOLIC query string carrying both the
   SHAPE ("team={team}&r={region}") and, when every value has a concrete example, the encoded EXAMPLE
   ("team=eng+team&r=us-east-1") -- not the old bare-opaque stub that discarded the params. get() returns the
   stored (concolic) value; unknown key -> opaque (external input). */
static void sp_encode(char *buf, int cap, int *o, const char *s) {   /* form-urlencode: space->+, reserved->%XX */
    static const char *hex = "0123456789ABCDEF";
    for (; *s && *o < cap - 3; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == ' ') buf[(*o)++] = '+';
        else if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c=='-'||c=='_'||c=='.'||c=='~') buf[(*o)++] = (char)c;
        else { buf[(*o)++] = '%'; buf[(*o)++] = hex[c >> 4]; buf[(*o)++] = hex[c & 15]; }
    }
    buf[*o] = 0;
}
static void sp_store(JSContext *ctx, JSValueConst this_val, JSValueConst key, JSValueConst val) {
    JSValue f = JS_GetPropertyStr(ctx, this_val, "__fields");
    if (!JS_IsObject(f)) { JS_FreeValue(ctx, f); f = JS_NewObject(ctx); JS_SetPropertyStr(ctx, this_val, "__fields", JS_DupValue(ctx, f)); }
    const char *k = JS_ToCString(ctx, key);
    if (k) { JS_SetPropertyStr(ctx, f, k, JS_DupValue(ctx, val)); JS_FreeCString(ctx, k); }
    JS_FreeValue(ctx, f);
}
static JSValue js_sp_append(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc >= 2) sp_store(ctx, this_val, argv[0], argv[1]);
    return JS_UNDEFINED;
}
static JSValue js_sp_get(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc >= 1) {
        const char *k = JS_ToCString(ctx, argv[0]);
        JSValue f = JS_GetPropertyStr(ctx, this_val, "__fields");
        if (JS_IsObject(f) && k) {
            JSValue v = JS_GetPropertyStr(ctx, f, k);
            if (!JS_IsUndefined(v)) { JS_FreeValue(ctx, f); JS_FreeCString(ctx, k); return v; }
            JS_FreeValue(ctx, v);
        }
        JS_FreeValue(ctx, f);
        /* Unknown key -> external input, but KEYED by the param name so distinct params are DISTINCT concolic
           values: p.get('mode') and p.get('data') must not collapse to the SAME bare opaque, or a gate on one
           (`mode==='preview'`) is wrongly conflated with the sink on the other and the sibling-gated sink can't
           be solved. The key identity also gives the @H a real shape ({mode}) instead of a bare {}. */
        if (k) {
            char shp[80]; snprintf(shp, sizeof shp, "{%s}", k);
            /* When this params object was built from an external source (__root, e.g. "{search}"), ROOT-LINK the
               key: src "{search}.mode" / "{search}.data" so a sibling gate and the sink share a root — the @S
               query envelope (main.c) then builds "mode=preview&data=<breakout>" delivered at location.search,
               which the real parse decodes back. Standalone (record-built) params keep the bare-key identity. */
            JSValue rootv = JS_GetPropertyStr(ctx, this_val, "__root");
            if (JS_IsString(rootv)) {
                const char *root = JS_ToCString(ctx, rootv);
                /* '?' separates the query KEY from the root — a marker a transform chain (which appends ".slice"
                   etc.) can never produce, so main.c distinguishes a real query param from a string transform. */
                char srcp[160]; snprintf(srcp, sizeof srcp, "%s?%s", root ? root : "", k);
                JSValue o = JS_NewOpaqueSourced(ctx, shp, srcp);
                if (root) JS_FreeCString(ctx, root);
                JS_FreeValue(ctx, rootv); JS_FreeCString(ctx, k); return o;
            }
            JS_FreeValue(ctx, rootv);
            JSValue o = JS_NewOpaqueSourced(ctx, shp, k); JS_FreeCString(ctx, k); return o;
        }
    }
    return js_concolic(ctx, "{searchParam}", JS_UNDEFINED);   /* no key -> generic external input */
}
JSValue js_sp_tostring(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    JSValue f = JS_GetPropertyStr(ctx, this_val, "__fields");
    if (!JS_IsObject(f)) { JS_FreeValue(ctx, f); return JS_NewString(ctx, ""); }
    char shape[1600]; int so = 0; shape[0] = 0;
    char examp[1600]; int eo = 0; examp[0] = 0;
    int any_opaque = 0, all_examples = 1;
    JSPropertyEnum *tab = NULL; uint32_t n = 0;
    if (JS_GetOwnPropertyNames(ctx, &tab, &n, f, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
        for (uint32_t i = 0; i < n; i++) {
            const char *k = JS_AtomToCString(ctx, tab[i].atom);
            JSValue v = JS_GetProperty(ctx, f, tab[i].atom);
            int op = JS_IsOpaque(v); if (op) any_opaque = 1;
            const char *vsh; int free_vsh = 0;
            if (op) vsh = JS_OpaqueShapeC(v); else { vsh = JS_ToCString(ctx, v); free_vsh = 1; }
            if (k && vsh && so < (int)sizeof(shape) - 2) so += snprintf(shape + so, sizeof(shape) - so, "%s%s=%s", so ? "&" : "", k, vsh);
            JSValue vex = op ? JS_OpaqueExample(ctx, v) : JS_DupValue(ctx, v);
            if (JS_IsUndefined(vex) || JS_IsNull(vex)) all_examples = 0;
            else if (k && eo < (int)sizeof(examp) - 2) {
                const char *ve = JS_ToCString(ctx, vex);
                if (ve) { eo += snprintf(examp + eo, sizeof(examp) - eo, "%s%s=", eo ? "&" : "", k); sp_encode(examp, (int)sizeof(examp), &eo, ve); JS_FreeCString(ctx, ve); }
                else all_examples = 0;
            }
            JS_FreeValue(ctx, vex);
            if (vsh && free_vsh) JS_FreeCString(ctx, vsh);
            if (k) JS_FreeCString(ctx, k);
            JS_FreeValue(ctx, v);
        }
        JS_FreePropertyEnum(ctx, tab, n);
    }
    JS_FreeValue(ctx, f);
    if (!any_opaque) return JS_NewStringLen(ctx, shape, so);   /* all-concrete params -> a plain string */
    JSValue r = JS_NewOpaqueSourced(ctx, shape, "{}");         /* tainted query keeps its shape + @S taint */
    if (all_examples && JS_IsOpaque(r)) JS_SetOpaqueExample(ctx, r, JS_NewStringLen(ctx, examp, eo));
    return r;
}
/* application/x-www-form-urlencoded DECODE (the spec parse: '+' -> space, %XX -> byte) into a JS string. Real
   URLSearchParams.get returns the decoded value; without this a replay candidate delivered as an ENCODED query
   (location.search percent-encodes it) would stay encoded and never break out at the sink. */
static JSValue sp_decode(JSContext *ctx, const char *s, size_t n) {
    char *out = malloc(n + 1); if (!out) return JS_NewStringLen(ctx, s, n);
    size_t j = 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == '+') out[j++] = ' ';
        else if (c == '%' && i + 2 < n) {
            int hi = s[i+1], lo = s[i+2];
            #define HEXV(x) ((x)>='0'&&(x)<='9'?(x)-'0':((x)|32)>='a'&&((x)|32)<='f'?((x)|32)-'a'+10:-1)
            int h = HEXV(hi), l = HEXV(lo);
            if (h >= 0 && l >= 0) { out[j++] = (char)((h << 4) | l); i += 2; } else out[j++] = c;
            #undef HEXV
        } else out[j++] = c;
    }
    JSValue r = JS_NewStringLen(ctx, out, j); free(out); return r;
}
static void sp_init(JSContext *ctx, JSValueConst o, JSValueConst init) {
    if (JS_IsString(init)) {                                    /* "a=1&b=2" (leading '?' tolerated) */
        const char *s = JS_ToCString(ctx, init); if (!s) return;
        const char *p = s; if (*p == '?') p++;
        while (*p) {
            const char *amp = strchr(p, '&'); const char *end = amp ? amp : p + strlen(p);
            const char *eq = memchr(p, '=', (size_t)(end - p));
            if (eq) { JSValue k = sp_decode(ctx, p, (size_t)(eq - p)); JSValue v = sp_decode(ctx, eq + 1, (size_t)(end - eq - 1));
                      sp_store(ctx, o, k, v); JS_FreeValue(ctx, k); JS_FreeValue(ctx, v); }
            if (!amp) break; p = amp + 1;
        }
        JS_FreeCString(ctx, s);
    } else if (JS_IsObject(init)) {                            /* {k:v} record (the common case) */
        JSPropertyEnum *tab = NULL; uint32_t n = 0;
        if (JS_GetOwnPropertyNames(ctx, &tab, &n, init, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
            for (uint32_t i = 0; i < n; i++) {
                JSValue kv = JS_AtomToString(ctx, tab[i].atom);
                JSValue v = JS_GetProperty(ctx, init, tab[i].atom);
                sp_store(ctx, o, kv, v);
                JS_FreeValue(ctx, kv); JS_FreeValue(ctx, v);
            }
            JS_FreePropertyEnum(ctx, tab, n);
        }
    }
}
static JSValue js_sp_forEach(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) return JS_UNDEFINED;
    JSValue f = JS_GetPropertyStr(ctx, this_val, "__fields");
    if (JS_IsObject(f)) {
        JSPropertyEnum *tab = NULL; uint32_t n = 0;
        if (JS_GetOwnPropertyNames(ctx, &tab, &n, f, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
            for (uint32_t i = 0; i < n; i++) {
                JSValue v = JS_GetProperty(ctx, f, tab[i].atom);
                JSValue k = JS_AtomToString(ctx, tab[i].atom);
                JSValueConst args[3] = { v, k, this_val };   /* spec order: (value, key, parent) */
                JSValue r = JS_Call(ctx, argv[0], JS_UNDEFINED, 3, args);
                JS_FreeValue(ctx, r); JS_FreeValue(ctx, v); JS_FreeValue(ctx, k);
            }
            JS_FreePropertyEnum(ctx, tab, n);
        }
    }
    JS_FreeValue(ctx, f);
    return JS_UNDEFINED;
}
/* keys/values/entries -> a plain ARRAY (iterable: for-of / spread / Array.from all work; an opaque stub would
   throw in for-of). mode: 0=keys 1=values 2=entries. */
static JSValue sp_iter_build(JSContext *ctx, JSValueConst this_val, int mode) {
    JSValue arr = JS_NewArray(ctx); uint32_t idx = 0;
    JSValue f = JS_GetPropertyStr(ctx, this_val, "__fields");
    if (JS_IsObject(f)) {
        JSPropertyEnum *tab = NULL; uint32_t n = 0;
        if (JS_GetOwnPropertyNames(ctx, &tab, &n, f, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
            for (uint32_t i = 0; i < n; i++) {
                JSValue k = JS_AtomToString(ctx, tab[i].atom);
                JSValue v = JS_GetProperty(ctx, f, tab[i].atom);
                JSValue item;
                if (mode == 0) { item = k; JS_FreeValue(ctx, v); }
                else if (mode == 1) { item = v; JS_FreeValue(ctx, k); }
                else { item = JS_NewArray(ctx); JS_SetPropertyUint32(ctx, item, 0, k); JS_SetPropertyUint32(ctx, item, 1, v); }
                JS_SetPropertyUint32(ctx, arr, idx++, item);
            }
            JS_FreePropertyEnum(ctx, tab, n);
        }
    }
    JS_FreeValue(ctx, f);
    return arr;
}
static JSValue js_sp_keys(JSContext *ctx, JSValueConst t, int c, JSValueConst *v)    { return sp_iter_build(ctx, t, 0); }
static JSValue js_sp_values(JSContext *ctx, JSValueConst t, int c, JSValueConst *v)  { return sp_iter_build(ctx, t, 1); }
static JSValue js_sp_entries(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) { return sp_iter_build(ctx, t, 2); }
/* Serialize a searchParams' __fields to a CONCRETE query string (no leading '?'): concrete values %-encoded,
   an opaque value -> its example (%-encoded) if known, else its literal {shape} hole. malloc'd; caller frees. */
static char *sp_serialize(JSContext *ctx, JSValueConst sp) {
    char buf[1600]; int o = 0; buf[0] = 0;
    JSValue f = JS_GetPropertyStr(ctx, sp, "__fields");
    if (JS_IsObject(f)) {
        JSPropertyEnum *tab = NULL; uint32_t n = 0;
        if (JS_GetOwnPropertyNames(ctx, &tab, &n, f, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
            for (uint32_t i = 0; i < n; i++) {
                const char *k = JS_AtomToCString(ctx, tab[i].atom);
                JSValue v = JS_GetProperty(ctx, f, tab[i].atom);
                char *valstr = NULL; int is_shape = 0;
                if (JS_IsOpaque(v)) {
                    JSValue ex = JS_OpaqueExample(ctx, v);
                    if (!JS_IsUndefined(ex)) { const char *s = JS_ToCString(ctx, ex); if (s) { valstr = strdup(s); JS_FreeCString(ctx, s); } }
                    JS_FreeValue(ctx, ex);
                    if (!valstr) { const char *sh = JS_OpaqueShapeC(v); valstr = strdup(sh ? sh : "{}"); is_shape = 1; }
                } else { const char *s = JS_ToCString(ctx, v); if (s) { valstr = strdup(s); JS_FreeCString(ctx, s); } }
                if (k && valstr && o < (int)sizeof(buf) - 2) {
                    o += snprintf(buf + o, sizeof(buf) - o, "%s%s=", o ? "&" : "", k);
                    if (is_shape) o += snprintf(buf + o, sizeof(buf) - o, "%s", valstr);   /* keep {hole} literal for url_solve/build_query */
                    else sp_encode(buf, (int)sizeof(buf), &o, valstr);
                }
                free(valstr);
                if (k) JS_FreeCString(ctx, k);
                JS_FreeValue(ctx, v);
            }
            JS_FreePropertyEnum(ctx, tab, n);
        }
    }
    JS_FreeValue(ctx, f);
    return strdup(buf);
}
/* After a set/append/delete on url.searchParams, rebuild the owner URL's search + href so a later fetch(url)
   / url.href read sees the app-built query. */
static void url_sp_writeback(JSContext *ctx, JSValueConst sp) {
    JSValue owner = JS_GetPropertyStr(ctx, sp, "__owner");
    if (!JS_IsObject(owner)) { JS_FreeValue(ctx, owner); return; }
    char *q = sp_serialize(ctx, sp);
    char search[1700]; if (q && q[0]) snprintf(search, sizeof search, "?%s", q); else search[0] = 0;
    JS_SetPropertyStr(ctx, owner, "search", JS_NewString(ctx, search));
    JSValue vo = JS_GetPropertyStr(ctx, owner, "origin");   const char *origin = JS_ToCString(ctx, vo);
    JSValue vp = JS_GetPropertyStr(ctx, owner, "pathname"); const char *path   = JS_ToCString(ctx, vp);
    JSValue vh = JS_GetPropertyStr(ctx, owner, "hash");     const char *hash   = JS_ToCString(ctx, vh);
    char href[2048]; snprintf(href, sizeof href, "%s%s%s%s", origin ? origin : "", path ? path : "", search, hash ? hash : "");
    JS_SetPropertyStr(ctx, owner, "href", JS_NewString(ctx, href));
    if (origin) JS_FreeCString(ctx, origin); if (path) JS_FreeCString(ctx, path); if (hash) JS_FreeCString(ctx, hash);
    JS_FreeValue(ctx, vo); JS_FreeValue(ctx, vp); JS_FreeValue(ctx, vh);
    free(q); JS_FreeValue(ctx, owner);
}
static JSValue js_url_sp_set(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc >= 2) sp_store(ctx, this_val, argv[0], argv[1]);   /* __fields is name-keyed: append coalesces to set (matches standalone) */
    url_sp_writeback(ctx, this_val);
    return JS_UNDEFINED;
}
static JSValue js_url_sp_delete(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc >= 1) {
        JSValue f = JS_GetPropertyStr(ctx, this_val, "__fields");
        if (JS_IsObject(f)) { const char *k = JS_ToCString(ctx, argv[0]);
            if (k) { JSAtom a = JS_NewAtom(ctx, k); JS_DeleteProperty(ctx, f, a, 0); JS_FreeAtom(ctx, a); JS_FreeCString(ctx, k); } }
        JS_FreeValue(ctx, f);
    }
    url_sp_writeback(ctx, this_val);
    return JS_UNDEFINED;
}
JSValue js_searchparams_ctor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv) {
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "__fields", JS_NewObject(ctx));
    if (argc >= 1 && !JS_IsUndefined(argv[0]) && !JS_IsNull(argv[0])) {
        if (JS_IsOpaque(argv[0])) {   /* built from external input (location.search/hash): remember the ROOT source id so get(k) links siblings, letting the multi-hole @S query envelope co-solve a sibling gate + the sink */
            const char *rs = JS_OpaqueSrcC(argv[0]);
            if (rs && rs[0]) JS_SetPropertyStr(ctx, o, "__root", JS_NewString(ctx, rs));
        } else sp_init(ctx, o, argv[0]);
    }
    JS_SetPropertyStr(ctx, o, "get", JS_NewCFunction(ctx, js_sp_get, "get", 1));
    JS_SetPropertyStr(ctx, o, "getAll", JS_NewCFunction(ctx, js_sp_get, "getAll", 1));
    JS_SetPropertyStr(ctx, o, "has", JS_NewCFunction(ctx, js_sp_get, "has", 1));
    JS_SetPropertyStr(ctx, o, "append", JS_NewCFunction(ctx, js_sp_append, "append", 2));
    JS_SetPropertyStr(ctx, o, "set", JS_NewCFunction(ctx, js_sp_append, "set", 2));
    JS_SetPropertyStr(ctx, o, "delete", JS_NewCFunction(ctx, js_url_sp_delete, "delete", 1));   /* real: removes from __fields (no __owner -> writeback no-ops) */
    JS_SetPropertyStr(ctx, o, "sort", JS_NewCFunction(ctx, js_noop, "sort", 0));
    JS_SetPropertyStr(ctx, o, "forEach", JS_NewCFunction(ctx, js_sp_forEach, "forEach", 1));
    JS_SetPropertyStr(ctx, o, "keys", JS_NewCFunction(ctx, js_sp_keys, "keys", 0));
    JS_SetPropertyStr(ctx, o, "values", JS_NewCFunction(ctx, js_sp_values, "values", 0));
    JS_SetPropertyStr(ctx, o, "entries", JS_NewCFunction(ctx, js_sp_entries, "entries", 0));
    JS_SetPropertyStr(ctx, o, "toString", JS_NewCFunction(ctx, js_sp_tostring, "toString", 0));
    return o;
}
/* Headers: a REAL object storing append()/set() header fields (__fields), so `fetch(url,{headers:h})` with a
   `new Headers()` surfaces the REAL required headers (auth/CSRF/custom) instead of the object's own METHODS
   (the old js_webobj_ctor stub -> the header capture iterated get/set/has/... as bogus headers). js_fetch
   reads __fields when present. */
JSValue js_headers_ctor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv) {
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "__fields", JS_NewObject(ctx));
    if (argc >= 1 && !JS_IsUndefined(argv[0]) && !JS_IsNull(argv[0])) sp_init(ctx, o, argv[0]);
    JS_SetPropertyStr(ctx, o, "append", JS_NewCFunction(ctx, js_sp_append, "append", 2));
    JS_SetPropertyStr(ctx, o, "set", JS_NewCFunction(ctx, js_sp_append, "set", 2));
    JS_SetPropertyStr(ctx, o, "get", JS_NewCFunction(ctx, js_sp_get, "get", 1));
    JS_SetPropertyStr(ctx, o, "has", JS_NewCFunction(ctx, js_sp_get, "has", 1));
    JS_SetPropertyStr(ctx, o, "getSetCookie", JS_NewCFunction(ctx, js_sp_get, "getSetCookie", 0));
    JS_SetPropertyStr(ctx, o, "delete", JS_NewCFunction(ctx, js_url_sp_delete, "delete", 1));   /* real: removes from __fields */
    JS_SetPropertyStr(ctx, o, "forEach", JS_NewCFunction(ctx, js_sp_forEach, "forEach", 1));
    JS_SetPropertyStr(ctx, o, "keys", JS_NewCFunction(ctx, js_sp_keys, "keys", 0));
    JS_SetPropertyStr(ctx, o, "values", JS_NewCFunction(ctx, js_sp_values, "values", 0));
    JS_SetPropertyStr(ctx, o, "entries", JS_NewCFunction(ctx, js_sp_entries, "entries", 0));
    return o;
}
