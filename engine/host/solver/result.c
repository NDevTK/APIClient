#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "solver/result.h"
#include "solver/endpoint.h"
#include "solver/solve.h"
#include "solver/engine.h"
#include "solver/flow.h"

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
    JSValue name = JS_UNDEFINED, msg = JS_UNDEFINED, stk = JS_UNDEFINED;
    const char *ns = NULL, *ms = NULL, *ss = NULL;
    JSAtom a_name, a_msg;
    int n;

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
    /* A DOMException keeps BOTH behind accessors on its prototype, so the own-property read above finds nothing
       and the report degenerates to "an object with no own name/message" — for the single most common throw in
       a DOM engine. That cost a whole debugging cycle: an aborted sibling flow reported as an anonymous object
       when it was a NotSupportedError naming exactly what was wrong. Read the slots instead. */
    if (!JS_IsString(name)) { JS_FreeValue(ctx, name); name = JS_GetDOMExceptionName(ctx, err); }
    if (!JS_IsString(msg))  { JS_FreeValue(ctx, msg);  msg  = JS_GetDOMExceptionMessage(ctx, err); }
    /* WHERE it threw. A genuine Error keeps its stack in the [[ErrorData]] internal slot behind an accessor on
       Error.prototype, so JS_GetOwnSlot cannot see it and calling the getter would run page code (a page may
       have replaced Error.prepareStackTrace, and this runs from OUTSIDE any flow). JS_GetErrorStackString reads
       the slot directly, which is exactly the "a stored value, never an operation" rule the rest of this
       function follows. Without it a message like "not a function" names a capability and nothing else — the
       whole diagnostic is WHERE, and finding it by hand meant re-serving the library wrapped in a try/catch. */
    stk = JS_GetErrorStackString(ctx, err);
    if (JS_IsString(name)) ns = JS_ToCString(ctx, name);
    if (JS_IsString(msg))  ms = JS_ToCString(ctx, msg);
    if (JS_IsString(stk))  ss = JS_ToCString(ctx, stk);
    if (ns && ms)      n = snprintf(buf, sizeof buf, "%s: %s", ns, ms);
    else if (ms)       n = snprintf(buf, sizeof buf, "%s", ms);
    else if (ns)       n = snprintf(buf, sizeof buf, "%s", ns);
    else               n = snprintf(buf, sizeof buf, "an object with no own name/message was thrown");
    if (ss && n > 0 && (size_t)n < sizeof buf) {
        /* the first two frames, on one line — the site and its caller, which is what identifies the call. */
        const char *l1 = ss + strspn(ss, " \t\n"), *l1e = l1 + strcspn(l1, "\n");
        const char *l2 = *l1e ? l1e + 1 : l1e, *l2e;
        l2 += strspn(l2, " \t");
        l2e = l2 + strcspn(l2, "\n");
        snprintf(buf + n, sizeof buf - (size_t)n, "  [%.*s%s%.*s]",
                 (int)(l1e - l1), l1, (l2e > l2 ? " <- " : ""), (int)(l2e - l2), l2);
    }
    if (ns) JS_FreeCString(ctx, ns);
    if (ms) JS_FreeCString(ctx, ms);
    if (ss) JS_FreeCString(ctx, ss);
    JS_FreeValue(ctx, name);
    JS_FreeValue(ctx, msg);
    JS_FreeValue(ctx, stk);
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
char *result_json(JSContext *ctx) {
    char *eps = endpoint_json_array();
    char *sinks = solve_json_array(ctx);
    char *errs = errs_json_array();
    size_t n;
    char *out;

    if (!eps || !sinks || !errs) { free(eps); free(sinks); free(errs); return NULL; }
    n = strlen(eps) + strlen(sinks) + strlen(errs) + 192;
    out = malloc(n);
    if (out)
        /* THE THREE COST NUMBERS, together. A switch count on its own cannot say whether a run that took six
           times as long grew its frontier or grew the work inside each flow, and those need opposite fixes. */
        snprintf(out, n, "{\"fetchCallSites\":%s,\"securitySinks\":%s,\"pageErrors\":%s,"
                         "\"_switches\":%d,\"_flows\":%ld,\"_candidates\":%d,"
                         "\"_jobsQueued\":%ld,\"_jobsRun\":%ld}",
                 eps, sinks, errs, engine_switch_count(), flow_created_count(), solve_candidate_count(),
                 engine_jobs_queued(), engine_jobs_run());
    free(eps);
    free(sinks);
    free(errs);
    return out;
}
