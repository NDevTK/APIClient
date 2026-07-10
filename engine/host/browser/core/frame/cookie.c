/* document.cookie — see cookie.h. A per-CODE-FLOW cookie jar (cookies are per-flow STATE, not a headless
 * unknown): the setter upserts name=value (attributes after the first ';' stripped), the getter serializes the
 * jar. A value the bundle sets round-trips concolic — `document.cookie='session='+token` then a later
 * `document.cookie` yields the session (opaque so a gate still FORKS and @S taint holds, carrying the concrete
 * example). The AMBIENT cookies (none set this flow) read concolic-empty until the content script seeds the
 * REAL same-origin cookies via cookie_seed (the page's own cookies, same principal — no privilege gained). */
#include "core/frame/cookie.h"
#include "solver/opaque.h"   /* g_opaque */
#include "check.h"    /* CHECK — an OOM must crash, never silently truncate the cookie join */
#include <string.h>
#include <stdlib.h>

static JSValue g_cookies = JS_UNDEFINED;   /* name -> the "name=value" pair (concrete string, or a concolic opaque) */

/* The ambient cookie value when the flow has set none: concolic (opaque so a gate forks has/no-session, example
   the logged-out empty string) — or the content-script-seeded real cookies once cookie_seed runs. */
static JSValue g_cookie_ambient = JS_UNDEFINED;

static JSValue ambient(JSContext *ctx) {
    if (JS_IsString(g_cookie_ambient)) return JS_DupValue(ctx, g_cookie_ambient);   /* seeded real cookies */
    JSValue o = JS_NewOpaqueSourced(ctx, "{cookie}", "{cookie}");
    if (JS_IsOpaque(o)) JS_SetOpaqueExample(ctx, o, JS_NewString(ctx, ""));
    return o;
}

void cookie_seed(JSContext *ctx, const char *raw) {
    JS_FreeValue(ctx, g_cookie_ambient);
    g_cookie_ambient = raw ? JS_NewString(ctx, raw) : JS_UNDEFINED;   /* content-script edge: the real same-origin cookies */
}

JSValue js_cookie_set(JSContext *ctx, JSValueConst this_val, JSValueConst val) {
    (void)this_val;
    if (!JS_IsObject(g_cookies)) { JS_FreeValue(ctx, g_cookies); g_cookies = JS_NewObject(ctx); }
    int opq = JS_IsOpaque(val);
    JSValue ex = opq ? JS_OpaqueExample(ctx, val) : JS_UNDEFINED;
    const char *s = (opq && !JS_IsUndefined(ex)) ? JS_ToCString(ctx, ex) : JS_ToCString(ctx, val);   /* the pair's TEXT (example if concolic) */
    if (s) {
        const char *ns = s; while (*ns == ' ') ns++;             /* trim leading space in the name */
        const char *eq = strchr(ns, '=');
        if (eq && eq != ns) {
            size_t nl = (size_t)(eq - ns);
            char name[128];
            if (nl < sizeof name) {
                memcpy(name, ns, nl); name[nl] = 0;
                JSValue stored;
                if (opq) stored = JS_DupValue(ctx, val);         /* whole concolic pair -> @S taint preserved on round-trip */
                else { const char *semi = strchr(s, ';'); size_t pl = semi ? (size_t)(semi - s) : strlen(s); stored = JS_NewStringLen(ctx, s, pl); }   /* concrete: strip attributes */
                JS_SetPropertyStr(ctx, g_cookies, name, stored);
            }
        }
    }
    if (s) JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, ex);
    return JS_UNDEFINED;
}

JSValue js_cookie_get(JSContext *ctx, JSValueConst this_val) {
    (void)this_val;
    if (!JS_IsObject(g_cookies)) return ambient(ctx);
    JSPropertyEnum *tab = NULL; uint32_t n = 0;
    if (JS_GetOwnPropertyNames(ctx, &tab, &n, g_cookies, JS_GPN_STRING_MASK) != 0) return ambient(ctx);
    if (n == 0) { JS_FreePropertyEnum(ctx, tab, n); return ambient(ctx); }
    if (n == 1) { JSValue p = JS_GetProperty(ctx, g_cookies, tab[0].atom); JS_FreePropertyEnum(ctx, tab, n); return p; }   /* one cookie: return it whole (concolic taint intact) */
    /* multiple cookies: join "pair; pair; …" — GROW the buffer (no fixed cap; a truncation is a bound-in-disguise
       that would silently drop cookies past 4 KB). (A concolic pair still degrades to its example text here — the
       rare multi-cookie taint case; the truncation bound is the concrete fix.) */
    size_t cap = 256, off = 0; char *buf = malloc(cap); CHECK(buf, "cookie join alloc");
    for (uint32_t i = 0; i < n; i++) {
        JSValue p = JS_GetProperty(ctx, g_cookies, tab[i].atom);
        const char *ps = JS_ToCString(ctx, p);
        if (ps) {
            size_t l = strlen(ps), need = off + (off ? 2 : 0) + l + 1;
            if (need > cap) { while (need > cap) cap *= 2; buf = realloc(buf, cap); CHECK(buf, "cookie join grow"); }
            if (off) { buf[off++] = ';'; buf[off++] = ' '; }
            memcpy(buf + off, ps, l); off += l; JS_FreeCString(ctx, ps);
        }
        JS_FreeValue(ctx, p);
    }
    buf[off] = 0;
    JS_FreePropertyEnum(ctx, tab, n);
    JSValue r = JS_NewString(ctx, buf); free(buf);
    return r;
}

void cookie_free(JSContext *ctx) {
    JS_FreeValue(ctx, g_cookies); g_cookies = JS_UNDEFINED;
    JS_FreeValue(ctx, g_cookie_ambient); g_cookie_ambient = JS_UNDEFINED;
}
