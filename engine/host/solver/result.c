#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "solver/result.h"
#include "solver/endpoint.h"
#include "solver/solve.h"
#include "solver/engine.h"

/* THE PAGE'S OWN UNCAUGHT ERRORS, deduped. See result.h: a script that throws is the forcing function naming an
   unbuilt capability, and it was silent. This is a plain string set — the message is the page's own, so nothing
   here interprets it. */
static char **g_errs; static int g_errs_n, g_errs_cap;

void result_page_error(const char *msg) {
    if (!msg || !*msg) return;
    for (int i = 0; i < g_errs_n; i++) if (!strcmp(g_errs[i], msg)) return;
    if (g_errs_n >= g_errs_cap) {
        int c = g_errs_cap ? g_errs_cap * 2 : 8;
        char **a = realloc(g_errs, (size_t)c * sizeof(char *));
        if (!a) return;   /* a lost diagnostic is not worth failing a run over */
        g_errs = a; g_errs_cap = c;
    }
    g_errs[g_errs_n] = strdup(msg);
    if (g_errs[g_errs_n]) g_errs_n++;
}

/* Describe a thrown value WITHOUT running any of the page's code — see result.h. An own slot that is already a
   string is taken as-is; anything else is described by shape alone. */
void result_page_error_value(JSContext *ctx, JSValueConst err) {
    char buf[320];
    JSValue name = JS_UNDEFINED, msg = JS_UNDEFINED;
    const char *ns = NULL, *ms = NULL;
    JSAtom a_name, a_msg;

    if (JS_IsString(err)) {
        const char *s = JS_ToCString(ctx, err);   /* already a string: no coercion runs */
        if (s) { result_page_error(s); JS_FreeCString(ctx, s); }
        return;
    }
    if (!JS_IsObject(err)) { result_page_error("a non-object, non-string value was thrown"); return; }

    a_name = JS_NewAtom(ctx, "name");
    a_msg  = JS_NewAtom(ctx, "message");
    if (JS_GetOwnSlot(ctx, &name, err, a_name) <= 0) name = JS_UNDEFINED;
    if (JS_GetOwnSlot(ctx, &msg,  err, a_msg)  <= 0) msg  = JS_UNDEFINED;
    JS_FreeAtom(ctx, a_name);
    JS_FreeAtom(ctx, a_msg);
    if (JS_IsString(name)) ns = JS_ToCString(ctx, name);
    if (JS_IsString(msg))  ms = JS_ToCString(ctx, msg);
    if (ns && ms)      snprintf(buf, sizeof buf, "%s: %s", ns, ms);
    else if (ms)       snprintf(buf, sizeof buf, "%s", ms);
    else if (ns)       snprintf(buf, sizeof buf, "%s", ns);
    else               snprintf(buf, sizeof buf, "an object with no own name/message was thrown");
    if (ns) JS_FreeCString(ctx, ns);
    if (ms) JS_FreeCString(ctx, ms);
    JS_FreeValue(ctx, name);
    JS_FreeValue(ctx, msg);
    result_page_error(buf);
}

/* Append RAW (a delimiter this file controls) or ESCAPED (page-supplied text). Escaping the delimiters too was
   a bug: the quotes around each message came out as \" inside the JSON string. */
static void errs_raw(char **buf, size_t *cap, size_t *len, const char *s) {
    size_t k = strlen(s);
    if (*len + k + 1 >= *cap) {
        size_t nc = *cap ? *cap : 256;
        while (*len + k + 1 >= nc) nc *= 2;
        char *nb = realloc(*buf, nc);
        if (!nb) return;
        *buf = nb; *cap = nc;
    }
    memcpy(*buf + *len, s, k); *len += k; (*buf)[*len] = 0;
}

/* JSON-escape a page-supplied string (its own message text, so it can hold anything). */
static void errs_append(char **buf, size_t *cap, size_t *len, const char *s) {
    for (const char *p = s; *p; p++) {
        char esc[8]; int k;
        if (*p == '"' || *p == '\\') { esc[0] = '\\'; esc[1] = *p; k = 2; }
        else if ((unsigned char)*p < 0x20) { k = snprintf(esc, sizeof esc, "\\u%04x", (unsigned char)*p); }
        else { esc[0] = *p; k = 1; }
        if (*len + (size_t)k + 1 >= *cap) {
            size_t nc = *cap ? *cap * 2 : 256;
            while (*len + (size_t)k + 1 >= nc) nc *= 2;
            char *nb = realloc(*buf, nc);
            if (!nb) return;
            *buf = nb; *cap = nc;
        }
        memcpy(*buf + *len, esc, (size_t)k); *len += (size_t)k; (*buf)[*len] = 0;
    }
}

static char *errs_json_array(void) {
    char *b = NULL; size_t cap = 0, len = 0;
    errs_raw(&b, &cap, &len, "[");
    for (int i = 0; i < g_errs_n; i++) {
        if (i) errs_raw(&b, &cap, &len, ",");
        errs_raw(&b, &cap, &len, "\"");
        errs_append(&b, &cap, &len, g_errs[i]);
        errs_raw(&b, &cap, &len, "\"");
    }
    errs_raw(&b, &cap, &len, "]");
    return b ? b : strdup("[]");
}

/* The composition, and nothing else. Each surface serializes itself — endpoint.c walks its deduped endpoints,
   solve.c its fire-verified sinks — and this only decides that they are ONE document and what it is called.
   Keeping that decision in one place is the point: a second caller that wanted "just the endpoints" is how a
   host ends up assembling structure again. */
char *result_json(void) {
    char *eps = endpoint_json_array();
    char *sinks = solve_json_array();
    char *errs = errs_json_array();
    size_t n;
    char *out;

    if (!eps || !sinks || !errs) { free(eps); free(sinks); free(errs); return NULL; }
    n = strlen(eps) + strlen(sinks) + strlen(errs) + 128;
    out = malloc(n);
    if (out)
        snprintf(out, n, "{\"fetchCallSites\":%s,\"securitySinks\":%s,\"pageErrors\":%s,\"_switches\":%d}",
                 eps, sinks, errs, engine_switch_count());
    free(eps);
    free(sinks);
    free(errs);
    return out;
}
