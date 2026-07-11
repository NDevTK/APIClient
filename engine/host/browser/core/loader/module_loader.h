/* ES-module loader — the browser-faithful static + dynamic import graph.  See module_loader.c.
 *
 * A module's `import ... from "X"` resolves SYNCHRONOUSLY at link time, but our chunk fetch is async
 * (chunk_pending -> host -> qjs_provide). So the loader compiles a dep from cached source if we have it,
 * else requests the chunk (like a browser fetches the graph), fails THIS link, and parks the importing
 * module until its deps arrive. A module is a base-owned SINGLETON: evaluated once, re-import returns the
 * cached namespace. dynamic import() never parks a persistent promise in a revertible flow — it resolves to
 * the linked singleton if ready, else opaque now + a provision-driven re-run. */
#ifndef ENGINE_HOST_MODULE_LOADER_H
#define ENGINE_HOST_MODULE_LOADER_H
#include "quickjs.h"

/* A fetched chunk body, cached by URL so the loader / dyn-import can compile it (called from qjs_provide). */
void modsrc_put(const char *url, const char *src, size_t len);
const char *modsrc_body(const char *url, size_t *plen);   /* the fetched body for url, or NULL (boot cursor blocks on a sync <script src> until non-NULL) */

/* Link a module chunk SINGLETON from its fetched source, handing back its real namespace (0 if not ready).
   Called from qjs_provide to link a provided module chunk in the base context. */
int dynimport_link(JSContext *ctx, const char *spec, JSValue *out_ns);
void pendimport_resolve(JSContext *ctx, const char *url);   /* resolve every parked import() of url with the linked namespace (qjs_provide) */

/* Defer a module whose static-import dep isn't fetched yet; retried on each provide. */
void link_inline_module(JSContext *ctx, const char *src, size_t len);   /* link an inline <script type=module> via the map (synthetic URL) */
void importmap_parse(JSContext *ctx, const char *json, size_t len);     /* parse a <script type=importmap> into the bare-specifier resolver */
void defermod_add(const char *url);      /* a URL'd module whose link deferred (dep not fetched) — retried by URL */
void defermod_retry(JSContext *ctx);     /* a chunk arrived: re-link every deferred module against the map */
int  is_moddep(const char *u);           /* was `u` a static-import dep (link in-graph, never eval standalone -> no double side effects)? */

/* quickjs hooks, registered in qjs_init (JS_SetDynImportHook / JS_SetModuleLoaderFunc). */
void         host_dyn_import(JSContext *ctx, const char *specifier, JSValueConst resolve, JSValueConst reject);
char        *host_module_normalize(JSContext *ctx, const char *base, const char *name, void *opaque);
JSModuleDef *host_module_loader(JSContext *ctx, const char *name, void *opaque);

void module_next_name(char *buf, size_t sz);   /* a unique <mod-N> name so a retry never collides with a prior link */
int  module_pending_count(void);               /* deferred modules still unlinked (teardown @WHY: unresolved graph) */
void module_loader_free(JSContext *ctx);       /* teardown: free the source / dep / pending tables */

#endif
