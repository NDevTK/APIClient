/* Active discovery — see discovery.h. The engine seeds one FLOW per candidate document URL; this file owns the
   candidate set and the reading of an answered one. It holds no state of its own: the candidates ARE the flows,
   and what a document teaches goes straight into the @H surface. */
#include "solver/discovery.h"
#include "solver/endpoint.h"
#include "solver/flow.h"
#include "check.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── building the candidate addresses ──────────────────────────────────────────────────────────────────── */

static char *joinz(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    char *r = malloc(la + lb + 1);
    CHECK(r, "discovery: OOM building a candidate URL");
    memcpy(r, a, la);
    memcpy(r + la, b, lb + 1);
    return r;
}

static char *join3(const char *a, const char *b, const char *c) {
    char *ab = joinz(a, b), *r = joinz(ab, c);
    free(ab);
    return r;
}

/* A GROWING STRING, for the one address whose length is not known up front: a method's URL plus however many
   query parameters the document declares. */
typedef struct { char *s; size_t n, cap; } Buf;

static void buf_add(Buf *b, const char *s, size_t n) {
    if (b->n + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap : 128;
        while (b->n + n + 1 > nc) nc *= 2;
        b->s = realloc(b->s, nc);
        CHECK(b->s, "discovery: OOM building a method URL");
        b->cap = nc;
    }
    memcpy(b->s + b->n, s, n);
    b->n += n;
    b->s[b->n] = 0;
}

static void buf_str(Buf *b, const char *s) { buf_add(b, s, strlen(s)); }

/* THE PUBLISHED LOCATIONS. Two families, and each is a place a server ANNOUNCES its own description rather than
   a guess at one: the OpenAPI/Swagger well-known paths, and Google's `$discovery/rest` with the query forms that
   change WHICH document it answers with (`labels=PANTHEON` expands visibility-labelled methods; a service that
   hosts several versions requires an explicit `version=`). The set is the one lib/discovery.js carried, minus
   the `#_internal_probe` fragment it appended — that was a marker for the page-context relay to recognise its
   own request, and this component does not go through that relay: a fragment is never sent to a server, so
   carrying one here would only make two candidates that differ by nothing look like two. */
static const char *const GENERIC_PATHS[] = {
    "/.well-known/openapi.json",
    "/.well-known/swagger.json",
    "/openapi.json",
    "/swagger.json",
    "/swagger/v1/swagger.json",
    "/api/docs",
    "/api/v1/docs",
    "/api-docs",
    "/v1/api-docs",
    NULL
};

static const char *const DISCOVERY_QUERIES[] = {
    "",
    "?labels=PANTHEON",
    "?version=v1",
    "?version=v2",
    "?version=v1beta1",
    "?version=v1alpha1",
    NULL
};

static const char CLIENTS6[] = ".clients6.google.com";
static const char GOOGLEAPIS[] = ".googleapis.com";

/* THE SIBLING ORIGIN, or NULL. `people-pa.googleapis.com` and `people-pa.clients6.google.com` are two front
   doors onto one service and they do not always publish the same document, so an origin learned as one is a
   real address for the other. It is a rename of the ORIGIN, never of a path or a value: nothing about the
   endpoints this yields is invented — the document at the far end says what they are, or there is no document
   and the flow learns nothing. A sandbox host is left alone (`staging-x.sandbox.googleapis.com` has no
   clients6 form at all). */
static char *sibling_origin(const char *origin) {
    size_t lo = strlen(origin), l6 = sizeof CLIENTS6 - 1, lg = sizeof GOOGLEAPIS - 1;
    char *r;
    if (lo > l6 && !strcmp(origin + lo - l6, CLIENTS6)) {
        r = malloc(lo - l6 + lg + 1);
        CHECK(r, "discovery: OOM building the sibling origin");
        memcpy(r, origin, lo - l6);
        memcpy(r + lo - l6, GOOGLEAPIS, lg + 1);
        return r;
    }
    if (lo > lg && !strcmp(origin + lo - lg, GOOGLEAPIS) && !strstr(origin, "sandbox")) {
        r = malloc(lo - lg + l6 + 1);
        CHECK(r, "discovery: OOM building the sibling origin");
        memcpy(r, origin, lo - lg);
        memcpy(r + lo - lg, CLIENTS6, l6 + 1);
        return r;
    }
    return NULL;
}

/* ONE CANDIDATE IS ONE FLOW, and this is the only place a discovery flow is born. It takes ownership of `url`:
   the flow owns that string until flow_remove frees it, which is what makes the candidate the flow's identity
   rather than a parameter someone has to keep beside it. */
static void seed_one(JSContext *ctx, char *url) {
    Flow *f = flow_add(ctx, JS_UNDEFINED, WORLD_NONE);   /* a probe stands on no page decision: a root world */
    DCHECK(f->disc_url == NULL,
           "a freshly minted flow already carried a discovery candidate — flow_new zeroes the record, so this "
           "flow is one the registry handed out twice and two candidates would fight over one park");
    f->disc_url = url;
}

void discovery_seed_origin(JSContext *ctx, const char *origin) {
    char *sib;
    int i;

    DCHECK(origin != NULL && *origin, "discovery was seeded for no origin at all");
    DCHECK(strstr(origin, "://") != NULL,
           "discovery was seeded with something that is not an origin — a candidate is built by appending a "
           "well-known PATH to it, so an address with no scheme yields a relative string the host cannot fetch");
    DCHECK(strchr(strstr(origin, "://") + 3, '/') == NULL,
           "discovery was seeded with an origin carrying a PATH — the candidate paths are appended, so this "
           "would probe `https://host/v1/users/openapi.json` instead of the document's published address");
    DCHECK(strchr(origin, '{') == NULL,
           "a SHAPE was seeded as an origin — §RUN, DON'T MATCH: a host the code did not compute is not a host, "
           "and fetching one would be inventing the address of a server nobody has named");

    for (i = 0; GENERIC_PATHS[i]; i++)
        seed_one(ctx, joinz(origin, GENERIC_PATHS[i]));
    for (i = 0; DISCOVERY_QUERIES[i]; i++)
        seed_one(ctx, join3(origin, "/$discovery/rest", DISCOVERY_QUERIES[i]));
    sib = sibling_origin(origin);
    if (sib) {
        for (i = 0; DISCOVERY_QUERIES[i]; i++)
            seed_one(ctx, join3(sib, "/$discovery/rest", DISCOVERY_QUERIES[i]));
        free(sib);
    }
}

/* ── reading an answered candidate ─────────────────────────────────────────────────────────────────────── */

/* A STRING PROPERTY, COPIED — or NULL when the document does not carry one, which every caller reads as the
   positive statement that it did not. A JSON-parsed object has no getters, so nothing here can run page code. */
static char *prop_str(JSContext *ctx, JSValueConst o, const char *k) {
    JSValue v;
    char *r = NULL;
    if (!JS_IsObject(o)) return NULL;
    v = JS_GetPropertyStr(ctx, o, k);
    if (JS_IsException(v)) { JS_FreeValue(ctx, JS_GetException(ctx)); return NULL; }
    if (JS_IsString(v)) {
        const char *s = JS_ToCString(ctx, v);
        if (s) { r = strdup(s); CHECK(r, "discovery: OOM copying a document field"); JS_FreeCString(ctx, s); }
    }
    JS_FreeValue(ctx, v);
    return r;
}

/* An OBJECT property, or JS_UNDEFINED. Owned by the caller either way. */
static JSValue prop_obj(JSContext *ctx, JSValueConst o, const char *k) {
    JSValue v;
    if (!JS_IsObject(o)) return JS_UNDEFINED;
    v = JS_GetPropertyStr(ctx, o, k);
    if (JS_IsException(v)) { JS_FreeValue(ctx, JS_GetException(ctx)); return JS_UNDEFINED; }
    if (!JS_IsObject(v)) { JS_FreeValue(ctx, v); return JS_UNDEFINED; }
    return v;
}

/* THE ORIGIN A DOCUMENT WAS FETCHED FROM — what a relative or absent `servers[0].url` resolves against, which
   is the address of the document itself and nothing else. */
static char *origin_of(const char *url) {
    const char *s = strstr(url, "://"), *e;
    char *r;
    if (!s) return NULL;
    e = s + 3 + strcspn(s + 3, "/");
    r = malloc((size_t)(e - url) + 1);
    CHECK(r, "discovery: OOM reading a candidate's origin");
    memcpy(r, url, (size_t)(e - url));
    r[e - url] = 0;
    return r;
}

static void upper_ascii(char *s) {
    for (; *s; s++) if (*s >= 'a' && *s <= 'z') *s = (char)(*s - 'a' + 'A');
}

/* REGISTER ONE METHOD as an endpoint of this surface. It is `endpoint_record` and nothing else: a discovery
   document's method is the same kind of finding as a call site forced execution reached, so it deduplicates by
   the same identity and the WFQ credits the flow that learned it exactly once. */
static void record_method(JSContext *ctx, const char *method, const char *url) {
    JSValue u = JS_NewString(ctx, url);
    endpoint_record(ctx, method, u, NULL, 0);
    JS_FreeValue(ctx, u);
}

/* THE QUERY PARAMETERS A METHOD DECLARES, appended as bare names. The NAME is what the document states; a
   VALUE is not, so none is written — §H "must NEVER INVENT". endpoint.c's parser reads `?a=&b=` as two params
   with no observed value, which is precisely the claim being made. */
static void append_query_names_map(JSContext *ctx, JSValueConst params, Buf *u, int *first) {
    JSPropertyEnum *tab = NULL;
    uint32_t n = 0, i;
    if (!JS_IsObject(params)) return;
    if (JS_GetOwnPropertyNames(ctx, &tab, &n, params, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) != 0) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return;
    }
    for (i = 0; i < n; i++) {
        JSValue p = JS_GetProperty(ctx, params, tab[i].atom);
        char *loc;
        const char *name;
        /* TAKEN AND DROPPED, never left pending: an exception that stays on the context is read by whichever
           call happens to look next, and this walk has several. */
        if (JS_IsException(p)) { JS_FreeValue(ctx, JS_GetException(ctx)); continue; }
        loc = prop_str(ctx, p, "location");
        name = JS_AtomToCString(ctx, tab[i].atom);
        /* A PATH parameter is already inside the method's own path template, so appending it as a query name
           would report a parameter the endpoint does not have. Discovery's default location is "query", which
           is why an absent one is one. */
        if (name && (!loc || strcmp(loc, "path") != 0)) {
            buf_str(u, *first ? "?" : "&");
            *first = 0;
            buf_str(u, name);
            buf_str(u, "=");
        }
        if (name) JS_FreeCString(ctx, name);
        free(loc);
        JS_FreeValue(ctx, p);
    }
    JS_FreePropertyEnum(ctx, tab, n);
}

/* The same fact in OpenAPI's spelling: an ARRAY of `{name, in}` rather than a map keyed by name. */
static void append_query_names_list(JSContext *ctx, JSValueConst holder, Buf *u, int *first) {
    JSValue params = JS_GetPropertyStr(ctx, holder, "parameters");
    JSValue lenv;
    uint32_t len = 0, i;
    if (JS_IsException(params)) { JS_FreeValue(ctx, JS_GetException(ctx)); return; }
    if (!JS_IsArray(params)) { JS_FreeValue(ctx, params); return; }
    lenv = JS_GetPropertyStr(ctx, params, "length");
    if (JS_ToUint32(ctx, &len, lenv) < 0) { JS_FreeValue(ctx, JS_GetException(ctx)); len = 0; }
    JS_FreeValue(ctx, lenv);
    for (i = 0; i < len; i++) {
        JSValue p = JS_GetPropertyUint32(ctx, params, i);
        char *in, *name;
        if (JS_IsException(p)) { JS_FreeValue(ctx, JS_GetException(ctx)); continue; }
        in = prop_str(ctx, p, "in");
        name = prop_str(ctx, p, "name");
        if (name && in && !strcmp(in, "query")) {
            buf_str(u, *first ? "?" : "&");
            *first = 0;
            buf_str(u, name);
            buf_str(u, "=");
        }
        free(in);
        free(name);
        JS_FreeValue(ctx, p);
    }
    JS_FreeValue(ctx, params);
}

/* ONE GOOGLE-DISCOVERY METHOD: `httpMethod` plus `flatPath` (the template with every parent resource's
   parameters expanded) or, failing that, `path`. */
static void learn_google_method(JSContext *ctx, const char *base, JSValueConst m) {
    char *http = prop_str(ctx, m, "httpMethod");
    char *path = prop_str(ctx, m, "flatPath");
    Buf u = { 0 };
    if (!path) path = prop_str(ctx, m, "path");
    if (http && *http && path) {
        const char *p = path;
        JSValue params;
        int first = 1;
        while (*p == '/') p++;   /* `base` already ends with one */
        buf_str(&u, base);
        buf_str(&u, p);
        params = prop_obj(ctx, m, "parameters");
        append_query_names_map(ctx, params, &u, &first);
        JS_FreeValue(ctx, params);
        upper_ascii(http);
        record_method(ctx, http, u.s);
    }
    free(u.s);
    free(http);
    free(path);
}

/* THE RESOURCE TREE, WALKED ITERATIVELY over an explicit stack. Recursion here would be a C stack whose depth
   is decided by a document the network handed us, and the C stack is the one thing in this engine that cannot
   be parked (CLAUDE.md §C-stack) — the same reason cold.c's chain walk is iterative. */
static void learn_google(JSContext *ctx, JSValueConst doc, const char *fallback_origin) {
    char *base = prop_str(ctx, doc, "baseUrl");
    JSValue *st = NULL;
    int n = 0, cap = 0;
    JSValue top;
    size_t bl;

    if (!base) {
        char *root = prop_str(ctx, doc, "rootUrl");
        char *sp = prop_str(ctx, doc, "servicePath");
        if (root) { base = joinz(root, sp ? sp : ""); free(root); }
        free(sp);
    }
    if (!base) base = joinz(fallback_origin, "/");
    bl = strlen(base);
    if (bl == 0 || base[bl - 1] != '/') { char *t = joinz(base, "/"); free(base); base = t; }

    top = prop_obj(ctx, doc, "resources");
    if (JS_IsObject(top)) {
        st = malloc(sizeof *st);
        CHECK(st, "discovery: OOM walking a discovery document");
        cap = 1;
        st[n++] = top;
    } else {
        JS_FreeValue(ctx, top);
    }

    while (n > 0) {
        JSValue cur = st[--n];
        JSPropertyEnum *tab = NULL;
        uint32_t cnt = 0, i;
        if (JS_GetOwnPropertyNames(ctx, &tab, &cnt, cur, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) != 0) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            JS_FreeValue(ctx, cur);
            continue;
        }
        for (i = 0; i < cnt; i++) {
            JSValue r = JS_GetProperty(ctx, cur, tab[i].atom);
            JSValue methods, sub;
            if (JS_IsException(r)) { JS_FreeValue(ctx, JS_GetException(ctx)); continue; }
            methods = prop_obj(ctx, r, "methods");
            if (JS_IsObject(methods)) {
                JSPropertyEnum *mt = NULL;
                uint32_t mn = 0, j;
                if (JS_GetOwnPropertyNames(ctx, &mt, &mn, methods, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
                    for (j = 0; j < mn; j++) {
                        JSValue m = JS_GetProperty(ctx, methods, mt[j].atom);
                        if (JS_IsException(m)) { JS_FreeValue(ctx, JS_GetException(ctx)); continue; }
                        learn_google_method(ctx, base, m);
                        JS_FreeValue(ctx, m);
                    }
                    JS_FreePropertyEnum(ctx, mt, mn);
                } else {
                    JS_FreeValue(ctx, JS_GetException(ctx));
                }
            }
            JS_FreeValue(ctx, methods);
            sub = prop_obj(ctx, r, "resources");
            if (JS_IsObject(sub)) {
                if (n >= cap) {
                    cap = cap ? cap * 2 : 8;
                    st = realloc(st, (size_t)cap * sizeof *st);
                    CHECK(st, "discovery: OOM growing the resource-tree walk");
                }
                st[n++] = sub;
            } else {
                JS_FreeValue(ctx, sub);
            }
            JS_FreeValue(ctx, r);
        }
        JS_FreePropertyEnum(ctx, tab, cnt);
        JS_FreeValue(ctx, cur);
    }
    free(st);
    free(base);
}

/* THE OPERATION KEYS OF AN OpenAPI PATH ITEM. The Path Item Object also carries `parameters`, `summary`,
   `servers` and `$ref`, so the methods are named rather than "every key that is an object" — a document whose
   `summary` happened to be one would otherwise be recorded as an endpoint with no verb. */
static const char *const OPENAPI_OPS[] = {
    "get", "put", "post", "delete", "options", "head", "patch", "trace", NULL
};

static void learn_openapi(JSContext *ctx, JSValueConst doc, const char *fallback_origin) {
    char *base = NULL;
    JSValue servers, paths;
    JSPropertyEnum *tab = NULL;
    uint32_t cnt = 0, i;

    /* OpenAPI 3's `servers[0].url`, then Swagger 2's `host` + `basePath`, then the address the document was
       fetched from — which is what a relative or absent server URL resolves against, and is a fact rather than
       a guess: this engine performed that GET. */
    servers = JS_GetPropertyStr(ctx, doc, "servers");
    if (JS_IsException(servers)) { JS_FreeValue(ctx, JS_GetException(ctx)); servers = JS_UNDEFINED; }
    if (JS_IsArray(servers)) {
        JSValue s0 = JS_GetPropertyUint32(ctx, servers, 0);
        if (JS_IsException(s0)) JS_FreeValue(ctx, JS_GetException(ctx));
        else { base = prop_str(ctx, s0, "url"); JS_FreeValue(ctx, s0); }
    }
    JS_FreeValue(ctx, servers);
    if (!base) {
        char *host = prop_str(ctx, doc, "host");
        char *bp = prop_str(ctx, doc, "basePath");
        if (host) {
            char *scheme_host = joinz("https://", host);
            base = joinz(scheme_host, bp ? bp : "");
            free(scheme_host);
        } else if (bp) {
            base = joinz(fallback_origin, bp);
        }
        free(host);
        free(bp);
    }
    if (!base) base = strdup(fallback_origin);
    CHECK(base, "discovery: OOM building an OpenAPI base URL");
    if (base[0] == '/') { char *t = joinz(fallback_origin, base); free(base); base = t; }
    { size_t bl = strlen(base); if (bl && base[bl - 1] == '/') base[bl - 1] = 0; }

    paths = prop_obj(ctx, doc, "paths");
    if (!JS_IsObject(paths)) { JS_FreeValue(ctx, paths); free(base); return; }
    if (JS_GetOwnPropertyNames(ctx, &tab, &cnt, paths, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) != 0) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        JS_FreeValue(ctx, paths);
        free(base);
        return;
    }
    for (i = 0; i < cnt; i++) {
        JSValue item = JS_GetProperty(ctx, paths, tab[i].atom);
        const char *pathkey;
        int k;
        if (JS_IsException(item)) { JS_FreeValue(ctx, JS_GetException(ctx)); continue; }
        pathkey = JS_AtomToCString(ctx, tab[i].atom);   /* after the throw check: a `continue` above would leak it */
        for (k = 0; pathkey && JS_IsObject(item) && OPENAPI_OPS[k]; k++) {
            JSValue op = prop_obj(ctx, item, OPENAPI_OPS[k]);
            Buf u = { 0 };
            char verb[8];
            int first = 1;
            if (!JS_IsObject(op)) { JS_FreeValue(ctx, op); continue; }
            buf_str(&u, base);
            if (pathkey[0] != '/') buf_str(&u, "/");
            buf_str(&u, pathkey);
            append_query_names_list(ctx, op, &u, &first);
            snprintf(verb, sizeof verb, "%s", OPENAPI_OPS[k]);
            upper_ascii(verb);
            record_method(ctx, verb, u.s);
            free(u.s);
            JS_FreeValue(ctx, op);
        }
        if (pathkey) JS_FreeCString(ctx, pathkey);
        JS_FreeValue(ctx, item);
    }
    JS_FreePropertyEnum(ctx, tab, cnt);
    JS_FreeValue(ctx, paths);
    free(base);
}

void discovery_reply(JSContext *ctx, const char *url, JSValueConst reply) {
    char *body, *origin, *kind;
    JSValue parsed;
    int is_google, is_openapi;

    DCHECK(url != NULL && *url,
           "a discovery reply arrived naming no candidate — the address is what the document's own relative "
           "server URLs resolve against, so a reply with no URL would be read against nothing");
    /* A NETWORK ERROR IS AN ANSWER: this address publishes no document. JS_NULL is Fetch §5.6's network error
       and carries no reply record at all, so this is a shape test on what the host delivered and not an `if`
       past a broken invariant — most candidates legitimately answer nothing. */
    if (!JS_IsObject(reply)) return;

    body = prop_str(ctx, reply, "body");
    if (!body) return;
    /* THE REAL CODEC, RUN. §A JS-engine encoding builtin is modeled FAITHFULLY — a 404 HTML page is not JSON
       and that is an ordinary fact about the web, so the exception is taken and dropped exactly as the fetch
       drain drops a rejected delivery. */
    parsed = JS_ParseJSON(ctx, body, strlen(body), "<discovery>");
    free(body);
    if (JS_IsException(parsed)) { JS_FreeValue(ctx, JS_GetException(ctx)); return; }
    if (!JS_IsObject(parsed)) { JS_FreeValue(ctx, parsed); return; }

    /* IS THIS A DESCRIPTION, AND WHOSE? The document says so itself — Google Discovery states
       `discoveryVersion` or `kind: "discovery#restDescription"`, OpenAPI states `openapi`, Swagger states
       `swagger`. JSON that says none of them is JSON that answered a probe, and it teaches nothing here. */
    kind = prop_str(ctx, parsed, "kind");
    { char *dv = prop_str(ctx, parsed, "discoveryVersion");
      is_google = (dv != NULL) || (kind && !strcmp(kind, "discovery#restDescription"));
      free(dv); }
    { char *oa = prop_str(ctx, parsed, "openapi");
      char *sw = prop_str(ctx, parsed, "swagger");
      is_openapi = (oa != NULL) || (sw != NULL);
      free(oa); free(sw); }
    free(kind);

    origin = origin_of(url);
    /* AND IT IS USED WITHOUT A GUARD BELOW, deliberately: every candidate this component mints is built from an
       origin, so a NULL here is this engine reading a reply to a request it never issued. An `if` past that
       would carry on into learn_* with no base address and file the document's methods under nothing. */
    DCHECK(origin != NULL,
           "a discovery candidate was fetched from an address with no scheme — every candidate this component "
           "mints is built from an origin, so a reply naming a relative URL is one nothing here issued");
    if (is_google) learn_google(ctx, parsed, origin);
    else if (is_openapi) learn_openapi(ctx, parsed, origin);
    free(origin);
    JS_FreeValue(ctx, parsed);
}
