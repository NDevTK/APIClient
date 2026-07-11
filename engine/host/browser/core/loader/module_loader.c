/* ES-module loader — see module_loader.h. */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "check.h"        /* CHECK — a dropped module source/dep silently breaks the import graph */
#include "solver/concolic.h"       /* g_concolic — dyn import() of an unresolved chunk resolves opaque now */
#include "platform/url.h"          /* has_hole — a holey specifier isn't a fetchable URL */
#include "core/loader/module_loader.h"
#include "solver/parked.h"         /* the ONE async delivery-park mechanism, shared with reply consumption */

/* Borrowed from main.c (the scheduler side): the runtime (for JS_ExecutePendingJob), the chunk-fetch
   registrar (host NEED_FETCH -> qjs_provide), the discovered-chunk-URL array + its push helper. */
extern JSRuntime *g_rt;
extern void chunk_pending_add(const char *url);
extern JSValue g_chunkurls;
extern void arr_push_str(JSContext *ctx, JSValueConst arr, const char *s);
extern const char *g_origin;   /* the page's base URL — relative module specifiers resolve against it */

typedef struct { char *url; char *src; size_t len; JSValue ns; } ModSrc;   /* ns = cached module namespace once linked (a module is a SINGLETON — evaluated once, re-import returns this) */
static ModSrc *g_modsrc = NULL; static int g_modsrc_n = 0, g_modsrc_cap = 0;
void modsrc_put(const char *url, const char *src, size_t len) {
    for (int i = 0; i < g_modsrc_n; i++) if (strcmp(g_modsrc[i].url, url) == 0) return;   /* first source wins */
    if (g_modsrc_n >= g_modsrc_cap) { int nc = g_modsrc_cap ? g_modsrc_cap * 2 : 8; ModSrc *n = realloc(g_modsrc, (size_t)nc * sizeof(ModSrc)); CHECK(n, "modsrc-oom: realloc failed — dropping a module source silently breaks its import graph"); g_modsrc = n; g_modsrc_cap = nc; }
    char *s = malloc(len + 1); CHECK(s, "modsrc-oom: source copy alloc failed"); memcpy(s, src, len); s[len] = 0;
    g_modsrc[g_modsrc_n].url = strdup(url); g_modsrc[g_modsrc_n].src = s; g_modsrc[g_modsrc_n].len = len; g_modsrc[g_modsrc_n].ns = JS_UNDEFINED; g_modsrc_n++;
}
static ModSrc *modsrc_get(const char *url) { for (int i = 0; i < g_modsrc_n; i++) if (strcmp(g_modsrc[i].url, url) == 0) return &g_modsrc[i]; return NULL; }
const char *modsrc_body(const char *url, size_t *plen) { ModSrc *m = modsrc_get(url); if (m && m->src) { if (plen) *plen = m->len; return m->src; } return NULL; }
/* URLs discovered as STATIC-import deps: link them IN-GRAPH (loader compiles them), never eval standalone
   (that would double-run their side effects — the loader already links+runs them). */
static char **g_moddep = NULL; static int g_moddep_n = 0, g_moddep_cap = 0;
static void moddep_add(const char *u) { for (int i = 0; i < g_moddep_n; i++) if (strcmp(g_moddep[i], u) == 0) return; if (g_moddep_n >= g_moddep_cap) { int nc = g_moddep_cap ? g_moddep_cap * 2 : 8; char **n = realloc(g_moddep, (size_t)nc * sizeof(char *)); CHECK(n, "moddep-oom: realloc failed — a dropped static-import dep would eval standalone + double-run side effects"); g_moddep = n; g_moddep_cap = nc; } g_moddep[g_moddep_n++] = strdup(u); }
int is_moddep(const char *u) { for (int i = 0; i < g_moddep_n; i++) if (strcmp(g_moddep[i], u) == 0) return 1; return 0; }
static int g_modseq = 0;   /* unique synthetic module-map names (<mod-N>) for inline modules + link retries */
/* Blink ModuleTreeLinker retry — a URL'd module whose link deferred (a static-import dep not yet fetched) is
   re-linked BY URL against the module map (modsrc) when any dep arrives, via dynimport_link (compile+link+eval,
   idempotent on m->ns). No source re-eval under fresh names: the module is keyed by its URL, the Blink shape. */
static char **g_defermod = NULL; static int g_defermod_n = 0, g_defermod_cap = 0;
void defermod_add(const char *url) {
    for (int i = 0; i < g_defermod_n; i++) if (strcmp(g_defermod[i], url) == 0) return;
    if (g_defermod_n >= g_defermod_cap) { int nc = g_defermod_cap ? g_defermod_cap * 2 : 8; char **n = realloc(g_defermod, (size_t)nc * sizeof(char *)); CHECK(n, "defermod-oom: realloc failed — a dropped deferred module never links, silently losing its endpoints"); g_defermod = n; g_defermod_cap = nc; }
    g_defermod[g_defermod_n++] = strdup(url);
}
void defermod_retry(JSContext *ctx) {
    int progressed = 1;
    while (progressed) {
        progressed = 0;
        for (int i = 0; i < g_defermod_n; i++) {
            JSValue ns;
            if (!dynimport_link(ctx, g_defermod[i], &ns)) continue;   /* still missing a dep -> retry on the next arrival */
            JS_FreeValue(ctx, ns);
            pendimport_resolve(ctx, g_defermod[i]);                    /* deliver the linked namespace to any parked import() of this URL */
            free(g_defermod[i]);
            for (int j = i; j < g_defermod_n - 1; j++) g_defermod[j] = g_defermod[j + 1];
            g_defermod_n--; i--; progressed = 1;
        }
    }
}

static void resolve_with(JSContext *ctx, JSValueConst resolve, JSValue val) {   /* resolve borrowed; val consumed */
    JSValue r = JS_Call(ctx, resolve, JS_UNDEFINED, 1, (JSValueConst *)&val); JS_FreeValue(ctx, r); JS_FreeValue(ctx, val);
}

/* PARKED dynamic imports: import() of a not-yet-fetched chunk PARKS its resolve in the shared delivery-park
   (solver/parked). When qjs_provide links the chunk, pendimport_resolve fires each parked import with the REAL
   namespace, so the importer's continuation resumes with concrete exports EXACTLY like a browser resolves a
   dynamic import (async-as-flow: delivery is a promise RESOLUTION, not a re-run). ONE park mechanism, shared with
   reply consumption. */
static ParkTable *g_import_park = NULL;
static JSValue import_ns_compute(JSContext *ctx, const char *url, int tag, void *ud) {   /* the value each parked import resolves with = the linked namespace */
    (void)tag; (void)ud;
    JSValue ns; return dynimport_link(ctx, url, &ns) ? ns : js_concolic(ctx, "{module}", JS_UNDEFINED);
}
/* qjs_provide calls this once the chunk is linked: resolve every parked import of this url with the real ns. */
void pendimport_resolve(JSContext *ctx, const char *url) {
    park_resolve_url(ctx, g_import_park, url, import_ns_compute, NULL);
}
/* Link a dynamic-import chunk from its FETCHED source and hand back its REAL namespace (concrete exports).
   Returns 0 if not fetched yet, or if a static dep in the chunk isn't ready (retried on the next provide). */
int dynimport_link(JSContext *ctx, const char *spec, JSValue *out_ns) {
    ModSrc *m = modsrc_get(spec);
    if (!m || !m->src) return 0;
    if (!JS_IsUndefined(m->ns)) { *out_ns = JS_DupValue(ctx, m->ns); return 1; }   /* SINGLETON: already evaluated — a boot-replay / repeat import() returns the cached namespace, never re-evaluates (re-eval aborts + violates module semantics) */
    JSValue fn = JS_Eval(ctx, m->src, m->len, spec, JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(fn)) { JS_FreeValue(ctx, JS_GetException(ctx)); return 0; }
    JSModuleDef *md = JS_VALUE_GET_PTR(fn);
    JSValue ev = JS_EvalFunction(ctx, fn);   /* instantiate (loader resolves deps) + evaluate */
    if (JS_IsException(ev)) { JS_FreeValue(ctx, JS_GetException(ctx)); JS_FreeValue(ctx, ev); return 0; }
    { JSContext *c; while (JS_ExecutePendingJob(g_rt, &c) > 0) {} }
    JS_FreeValue(ctx, ev);
    *out_ns = JS_GetModuleNamespace(ctx, md);   /* the concrete exports */
    m->ns = JS_DupValue(ctx, *out_ns);   /* cache the singleton namespace for idempotent re-import */
    return 1;
}
/* dynamic import(specifier): a module is a BASE-OWNED SINGLETON, linked once in the base context when its
   chunk arrives (qjs_provide). import() NEVER parks a persistent promise inside a flow — a parked promise
   would outlive the flow's COW revert and later resolve against torn-down state (leaking the module realm +
   running m.load on a dead context). Instead: resolve to the linked singleton if ready, else register the
   fetch and resolve OPAQUE now; the chunk-link enqueues a forking boot re-run (like a reply), and THAT
   re-run's import resolves to the singleton synchronously in a LIVE flow. So there is no async-park state at
   all — provision-driven re-runs replace it. */
void host_dyn_import(JSContext *ctx, const char *specifier, JSValueConst resolve, JSValueConst reject) {
    if (!specifier || !specifier[0] || has_hole(specifier)) { (void)reject; resolve_with(ctx, resolve, js_concolic(ctx, "{module}", JS_UNDEFINED)); return; }
    arr_push_str(ctx, g_chunkurls, specifier);   /* -> @RESULT.chunkUrls */
    JSValue ns;
    if (dynimport_link(ctx, specifier, &ns)) { resolve_with(ctx, resolve, ns); return; }   /* linked singleton -> real namespace */
    chunk_pending_add(specifier); moddep_add(specifier);   /* register the fetch */
    (void)reject;
    if (!g_import_park) g_import_park = park_new();
    park_add(ctx, g_import_park, specifier, resolve, 0);   /* PARK holding the resolve; qjs_provide resolves it with the real namespace when the chunk links (browser-faithful, no opaque settle, no boot re-run) */
}
/* IMPORT MAP (HTML "resolve a module specifier" for BARE specifiers): { spec -> url }, parsed from a
   <script type="importmap"> before any module runs. A bare `import 'foo'` maps to its URL — exact match, then
   longest trailing-slash PREFIX ("util/" -> "/utils/"). A mapped value that is relative is resolved against the
   document by the offscreen (the spec's base for import-map values), so we return it as written. */
typedef struct { char *spec; char *url; } ImportMap;
static ImportMap *g_importmap = NULL; static int g_importmap_n = 0, g_importmap_cap = 0;
static void importmap_add(const char *spec, const char *url) {
    if (!spec || !url) return;
    if (g_importmap_n >= g_importmap_cap) { int nc = g_importmap_cap ? g_importmap_cap * 2 : 8; ImportMap *n = realloc(g_importmap, (size_t)nc * sizeof(ImportMap)); CHECK(n, "importmap-oom: realloc failed — a dropped mapping silently breaks bare-specifier resolution"); g_importmap = n; g_importmap_cap = nc; }
    g_importmap[g_importmap_n].spec = strdup(spec); g_importmap[g_importmap_n].url = strdup(url); g_importmap_n++;
}
void importmap_parse(JSContext *ctx, const char *json, size_t len) {
    JSValue o = JS_ParseJSON(ctx, json, len, "<importmap>");
    if (JS_IsException(o)) { JSValue e = JS_GetException(ctx); JS_FreeValue(ctx, e); return; }   /* a malformed import map is ignored (browser reports it; resolution just falls through) */
    JSValue imports = JS_GetPropertyStr(ctx, o, "imports");
    if (JS_IsObject(imports)) {
        JSPropertyEnum *tab = NULL; uint32_t n = 0;
        if (JS_GetOwnPropertyNames(ctx, &tab, &n, imports, JS_GPN_STRING_MASK) == 0) {
            for (uint32_t i = 0; i < n; i++) {
                const char *k = JS_AtomToCString(ctx, tab[i].atom);
                JSValue v = JS_GetProperty(ctx, imports, tab[i].atom);
                const char *vs = JS_IsString(v) ? JS_ToCString(ctx, v) : NULL;
                if (k && vs) importmap_add(k, vs);
                if (k) JS_FreeCString(ctx, k);
                if (vs) JS_FreeCString(ctx, vs);
                JS_FreeValue(ctx, v);
            }
            JS_FreePropertyEnum(ctx, tab, n);
        }
    }
    JS_FreeValue(ctx, imports); JS_FreeValue(ctx, o);
}
/* Resolve a BARE specifier via the import map -> a js_strdup'd URL, or NULL if unmapped (caller keeps verbatim). */
static char *importmap_resolve(JSContext *ctx, const char *name) {
    for (int i = 0; i < g_importmap_n; i++) if (strcmp(g_importmap[i].spec, name) == 0) return js_strdup(ctx, g_importmap[i].url);   /* exact */
    int best = -1; size_t bestlen = 0;
    for (int i = 0; i < g_importmap_n; i++) {
        size_t sl = strlen(g_importmap[i].spec);
        if (sl > 0 && g_importmap[i].spec[sl - 1] == '/' && strncmp(name, g_importmap[i].spec, sl) == 0 && sl > bestlen) { best = i; bestlen = sl; }   /* longest trailing-slash prefix */
    }
    if (best < 0) return NULL;
    const char *suffix = name + bestlen; size_t ul = strlen(g_importmap[best].url);
    char *out = js_malloc(ctx, ul + strlen(suffix) + 1);
    if (out) { memcpy(out, g_importmap[best].url, ul); strcpy(out + ul, suffix); }
    return out;
}

/* Module specifier resolution (HTML "resolve a module specifier"): a RELATIVE specifier (./x, ../x) resolves
   against the IMPORTING MODULE's base URL, not the document — else a module in a subdirectory misresolves its
   siblings (./b from /sub/a.js must be /sub/b.js, not /b.js). Use the WHATWG URL parser (url_resolve). A
   root-absolute (/x), bare (x — needs an import map, not yet supported), or full-URL specifier passes through
   VERBATIM: the offscreen resolves /x against the document, which is correct. An INLINE module's base is a
   synthetic <mod-N> (not a URL) -> pass through too (inline modules ARE at the document, so ./x resolves against
   it via the offscreen). The result is returned root-relative to match the page's own specifier style (so the
   same file is never keyed two ways -> no duplicate module instance). */
char *host_module_normalize(JSContext *ctx, const char *base, const char *name, void *opaque) {
    (void)opaque;
    if (!name) return js_strdup(ctx, name);
    if (name[0] == '.') {   /* RELATIVE (./x, ../x) -> resolve against the importing module's URL */
        if (!base || base[0] == '<') return js_strdup(ctx, name);   /* inline base (<mod-N>): offscreen resolves ./x against the document (correct) */
        char *full_base = url_resolve(base, g_origin);   /* the importing module's ABSOLUTE URL (base is stored root-relative or absolute) */
        if (!full_base) return js_strdup(ctx, name);
        char *resolved = url_resolve(name, full_base);   /* ./x / ../x against the module's URL, WHATWG-correct */
        free(full_base);
        if (!resolved) return js_strdup(ctx, name);
        const char *sep = strstr(resolved, "://");       /* strip scheme://host -> a root-relative /path, the page's key style */
        const char *path = sep ? strchr(sep + 3, '/') : NULL;
        char *out = js_strdup(ctx, path ? path : resolved);
        free(resolved);
        return out;
    }
    if (name[0] != '/' && !strstr(name, "://")) {   /* BARE specifier (foo, util/x) -> the import map */
        char *mapped = importmap_resolve(ctx, name);
        if (mapped) return mapped;
    }
    return js_strdup(ctx, name);   /* root-absolute (/x), full URL, or unmapped bare -> verbatim (offscreen resolves against the document) */
}
/* Resolve a static import: compile the dep from its FETCHED source; if not fetched yet, request it like a
   browser and fail this link (the importer is retried when the chunk arrives). quickjs dedups by name, so a
   given dep URL is compiled once and shared across the graph. */
JSModuleDef *host_module_loader(JSContext *ctx, const char *name, void *opaque) {
    (void)opaque;
    ModSrc *m = modsrc_get(name);
    if (m && m->src) {
        JSValue v = JS_Eval(ctx, m->src, m->len, name, JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
        if (JS_IsException(v)) { JS_FreeValue(ctx, JS_GetException(ctx)); return NULL; }
        JSModuleDef *md = JS_VALUE_GET_PTR(v); JS_FreeValue(ctx, v); return md;   /* module kept alive by rt module_list */
    }
    moddep_add(name);
    if (!has_hole(name)) { chunk_pending_add(name); arr_push_str(ctx, g_chunkurls, name); }   /* fetch the graph */
    JS_ThrowReferenceError(ctx, "module not yet fetched: %s", name);
    return NULL;
}
/* Link an INLINE <script type=module>: mint a synthetic module-map URL, put its source in the map, and link it
   BY URL exactly like an external module chunk (dynimport_link; defer via g_defermod if a static dep isn't
   fetched). This unifies inline + external modules onto the ONE URL-keyed map + tree-linker retry — no separate
   source-based defer (pendmod is gone). An inline module isn't imported by anyone, so its synthetic URL is
   never resolved as a specifier; it only keys the map + the deferred-link retry. */
void link_inline_module(JSContext *ctx, const char *src, size_t len) {
    char synth[32]; snprintf(synth, sizeof synth, "<mod-%d>", g_modseq++);
    modsrc_put(synth, src, len);
    JSValue ns; if (dynimport_link(ctx, synth, &ns)) JS_FreeValue(ctx, ns); else defermod_add(synth);
}
void module_next_name(char *buf, size_t sz) { snprintf(buf, sz, "<mod-%d>", g_modseq++); }
int module_pending_count(void) { return g_defermod_n; }   /* deferred modules that never linked (a dep never fetched) — the teardown @WHY */
void module_loader_free(JSContext *ctx) {
    for (int i = 0; i < g_modsrc_n; i++) { free(g_modsrc[i].url); free(g_modsrc[i].src); JS_FreeValue(ctx, g_modsrc[i].ns); }
    free(g_modsrc); g_modsrc = NULL; g_modsrc_n = g_modsrc_cap = 0;
    for (int i = 0; i < g_moddep_n; i++) free(g_moddep[i]);
    free(g_moddep); g_moddep = NULL; g_moddep_n = g_moddep_cap = 0;
    for (int i = 0; i < g_defermod_n; i++) free(g_defermod[i]);
    free(g_defermod); g_defermod = NULL; g_defermod_n = g_defermod_cap = 0; g_modseq = 0;
    for (int i = 0; i < g_importmap_n; i++) { free(g_importmap[i].spec); free(g_importmap[i].url); }
    free(g_importmap); g_importmap = NULL; g_importmap_n = g_importmap_cap = 0;
    park_free(ctx, g_import_park); g_import_park = NULL;   /* never-fired parked imports freed (a never-fetched chunk's continuation doesn't run) */
}
