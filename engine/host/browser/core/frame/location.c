/* The browser `location` object + external-input source getters + the principal split — see location.h. */
#include <string.h>
#include <stdlib.h>
#include "core/frame/location.h"
#include "solve.h"    /* solve_add — location.href/.assign/`location=` are @S navigation sinks */
#include "opaque.h"   /* g_opaque, js_noop */
#include "check.h"    /* CHECK — OOM must crash, never emit a prefix-less/unencoded candidate (a false PoC) */

/* Borrowed from main.c: the page principal (this TU WRITES it in set_origin + reads it for location.*), and
   the @S replay candidate (a replay flow's source getters return it instead of the opaque). */
extern const char *g_origin;
extern char *g_candidate;

static const char *g_protocol = "https:";
static const char *g_host     = "app.example.com";   /* host = hostname[:port] (location.host) */
static const char *g_hostname = "app.example.com";   /* hostname WITHOUT port (location.hostname) */
static const char *g_port     = "";                  /* port only, "" for default (location.port) */
/* Split a real origin ("https://localhost:8765") into protocol + host + hostname + port for the concrete
   location.*. A bundle that builds a URL from location.hostname/port (`"https://api."+location.hostname`)
   must see the port split out correctly, else the learned endpoint host is wrong. */
void set_origin(const char *origin) {
    static char protobuf[64], hostbuf[256], hostnamebuf[256], portbuf[16];
    const char *p;
    if (!origin || !origin[0]) return;
    g_origin = origin;
    p = strstr(origin, "://");
    if (!p) return;
    { size_t plen = (size_t)(p - origin) + 1; if (plen < sizeof protobuf) { memcpy(protobuf, origin, plen); protobuf[plen] = 0; g_protocol = protobuf; } }
    { const char *h = p + 3; size_t hlen = strlen(h); const char *slash = strchr(h, '/'); if (slash) hlen = (size_t)(slash - h);
      if (hlen < sizeof hostbuf) { memcpy(hostbuf, h, hlen); hostbuf[hlen] = 0; g_host = hostbuf;
        /* split host into hostname + port on the LAST ':' (an IPv6 literal is bracketed [::1]:port, so a ':'
           after a ']' or with no '[' is the port separator). */
        const char *colon = strrchr(hostbuf, ':');
        const char *rbrack = strrchr(hostbuf, ']');
        if (colon && colon > rbrack) {   /* has a port */
          size_t nlen = (size_t)(colon - hostbuf);
          if (nlen < sizeof hostnamebuf) { memcpy(hostnamebuf, hostbuf, nlen); hostnamebuf[nlen] = 0; g_hostname = hostnamebuf; }
          size_t plen2 = strlen(colon + 1); if (plen2 < sizeof portbuf) { memcpy(portbuf, colon + 1, plen2 + 1); g_port = portbuf; }
        } else { g_hostname = hostbuf; g_port = ""; }   /* default port */
      } }
}
/* An external-input SOURCE getter (location.hash=magic 0, location.search=magic 1). Normal exploration:
   returns the source-tagged OPAQUE (control-flow forks, @H/@S record the shape). @S REPLAY flow (g_candidate
   set): returns the CONCRETE candidate string, so the real code runs the transforms concretely and the sink
   sees a real value — reachability + breakout decided by the REAL code, in the ONE scheduler. */
static const char *g_source_tag[] = { "{hash}", "{search}", "{pm}", "{referrer}" };   /* 0=location.hash 1=location.search 2=postMessage e.data 3=document.referrer */
static const char *g_source_pfx[] = { "#", "?", "", "" };                             /* realistic leading char so slice(1)/substring behave faithfully */
/* The browser PERCENT-ENCODES the URL per the WHATWG percent-encode SETS, which DIFFER by component (VERIFIED
   on real Chrome). Both encode `< > " space` (-> %3C %3E %22 %20), but the tail differs and it MATTERS for XSS:
   the FRAGMENT set (location.hash) encodes backtick `` ` `` but NOT `'` — so a SINGLE-QUOTE-context breakout via
   raw hash WORKS; the SPECIAL-QUERY set (http/https location.search) encodes `'` but NOT backtick — so `'` is
   dead in raw search but a backtick vector lives. So a RAW read of either is encoded text and an HTML breakout
   there is a FALSE PoC unless the app DECODES it (decodeURIComponent / URLSearchParams), and the LIVE, working
   breakout char-set is source-specific. Deliver each candidate through the source's REAL browser transform so
   re-execution decides breakout faithfully — the browser's own encoding, never an invented filter. (mXSS makes
   this deeper still: the parser can MUTATE inert-looking encoded input into executable on re-serialization —
   handled where the sink output is re-parsed, not here.) postMessage data is structured, not URL-encoded. */
static char *pct_encode_url(const char *s, int enc_backtick, int enc_squote) {
    size_t n = strlen(s); char *o = malloc(n * 3 + 1); CHECK(o, "loc-encode-oom: malloc failed -> an unencoded candidate is a FALSE PoC"); size_t j = 0;
    static const char hex[] = "0123456789ABCDEF";
    for (size_t i = 0; i < n; i++) { unsigned char c = (unsigned char)s[i];
        if (c == ' ' || c == '"' || c == '<' || c == '>' || (enc_backtick && c == '`') || (enc_squote && c == '\'')) { o[j++] = '%'; o[j++] = hex[c >> 4]; o[j++] = hex[c & 15]; }
        else o[j++] = (char)c; }
    o[j] = 0; return o;
}
static JSValue js_source_get(JSContext *ctx, JSValueConst this_val, int magic) {
    if (g_candidate) {   /* a replay flow injects the CONCRETE candidate — with the source's real prefix, so
                            code that strips it (location.hash.slice(1), search.substring(1)) sees the true payload. */
        const char *pfx = g_source_pfx[magic];
        char *enc = (magic == 0) ? pct_encode_url(g_candidate, /*backtick*/1, /*squote*/0)   /* hash = FRAGMENT set: ` encoded, ' RAW */
                  : (magic == 1) ? pct_encode_url(g_candidate, /*backtick*/0, /*squote*/1)   /* search = SPECIAL-QUERY set: ' encoded, ` RAW */
                  : (magic == 3) ? pct_encode_url(g_candidate, /*backtick*/0, /*squote*/1)   /* referrer: the referring URL's query, SPECIAL-QUERY set like search */
                  : NULL;                                                                     /* pm: structured, not URL-encoded */
        const char *payload = enc ? enc : g_candidate;
        size_t lp = strlen(pfx), lc = strlen(payload);
        char *buf = malloc(lp + lc + 1); CHECK(buf, "loc-source-oom: malloc failed -> a prefix-less candidate would corrupt slice(1)/substring(1) payloads");
        memcpy(buf, pfx, lp); memcpy(buf + lp, payload, lc + 1);
        free(enc);
        if (magic == 2) {   /* postMessage e.data can be an OBJECT: return a CANDIDATE-CARRIER opaque so a FIELD
                               sink (`{html}=e.data; el.innerHTML=html`) delivers the candidate, while whole-value
                               use (`el.innerHTML=e.data`) reads the same candidate as the example. */
            JSValue c = JS_NewOpaqueSourced(ctx, g_source_tag[2], g_source_tag[2]);
            JS_SetOpaqueExample(ctx, c, JS_NewString(ctx, buf)); JS_SetOpaqueCarrier(ctx, c);
            free(buf); return c;
        }
        JSValue r = JS_NewString(ctx, buf); free(buf); return r;
    }
    return JS_NewOpaqueSourced(ctx, g_source_tag[magic], g_source_tag[magic]);   /* stamp root source identity for the per-flow value domain */
}
void def_source(JSContext *ctx, JSValueConst loc, const char *name, int magic) {
    JSAtom a = JS_NewAtom(ctx, name);
    JS_DefinePropertyGetSet(ctx, loc, a,
        JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)js_source_get, "get", 0, JS_CFUNC_getter_magic, magic),
        JS_UNDEFINED, JS_PROP_CONFIGURABLE);
    JS_FreeAtom(ctx, a);
}
/* @S: a NAVIGATION (location.href=/.assign/.replace, or location=) is a sink — a javascript: URL runs,
   an attacker-controlled URL is an open redirect. url context (solve_broke checks a javascript:X9 prefix). */
static JSValue js_location_nav(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    if (argc >= 1) solve_add(ctx, "location.assign", "url", argv[0]);
    return JS_UNDEFINED;
}
static JSValue js_location_href_get(JSContext *ctx, JSValueConst t) { return JS_NewString(ctx, g_origin); }  /* concrete base for new URL(path, location.href) */
static JSValue js_location_href_set(JSContext *ctx, JSValueConst t, JSValueConst val) {
    solve_add(ctx, "location.href", "url", val);   /* @S: location.href = javascript:/redirect */
    return JS_UNDEFINED;
}
static JSValue g_location = JS_UNDEFINED;   /* the location object; window.location is a getset over it so `location = url` is a nav sink */
JSValue js_window_location_get(JSContext *ctx, JSValueConst t) { return JS_DupValue(ctx, g_location); }
JSValue js_window_location_set(JSContext *ctx, JSValueConst t, JSValueConst val) {
    solve_add(ctx, "location", "url", val);   /* @S: `location = url` / `window.location = url` navigation */
    return JS_UNDEFINED;
}
JSValue make_location(JSContext *ctx)
{
    JSValue loc = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, loc, "origin",   JS_NewString(ctx, g_origin));
    JS_SetPropertyStr(ctx, loc, "protocol", JS_NewString(ctx, g_protocol));
    JS_SetPropertyStr(ctx, loc, "host",     JS_NewString(ctx, g_host));       /* hostname[:port] */
    JS_SetPropertyStr(ctx, loc, "hostname", JS_NewString(ctx, g_hostname));   /* WITHOUT port */
    JS_SetPropertyStr(ctx, loc, "port",     JS_NewString(ctx, g_port));       /* port only ("" = default) */
    JS_SetPropertyStr(ctx, loc, "pathname", JS_NewString(ctx, "/"));
    JSAtom ha = JS_NewAtom(ctx, "href");   /* href: getter = concrete base; SETTER = @S navigation sink */
    JS_DefinePropertyGetSet(ctx, loc, ha,
        JS_NewCFunction2(ctx, (JSCFunction *)js_location_href_get, "get href", 0, JS_CFUNC_getter, 0),
        JS_NewCFunction2(ctx, (JSCFunction *)js_location_href_set, "set href", 1, JS_CFUNC_setter, 0), JS_PROP_CONFIGURABLE);
    JS_FreeAtom(ctx, ha);
    JS_SetPropertyStr(ctx, loc, "assign",  JS_NewCFunction(ctx, js_location_nav, "assign", 1));   /* @S nav sinks */
    JS_SetPropertyStr(ctx, loc, "replace", JS_NewCFunction(ctx, js_location_nav, "replace", 1));
    JS_SetPropertyStr(ctx, loc, "reload",  JS_NewCFunction(ctx, js_noop, "reload", 0));
    JS_SetPropertyStr(ctx, loc, "toString", JS_NewCFunction(ctx, (JSCFunction *)js_location_href_get, "toString", 0));
    def_source(ctx, loc, "hash",   0);   /* external input: opaque (or candidate on @S replay) */
    def_source(ctx, loc, "search", 1);
    JS_FreeValue(ctx, g_location); g_location = JS_DupValue(ctx, loc);   /* store the singleton for window.location reads */
    return loc;
}
void location_free(JSContext *ctx) { JS_FreeValue(ctx, g_location); g_location = JS_UNDEFINED; }
const char *location_host(void) { return g_host; }
