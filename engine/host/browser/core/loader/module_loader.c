/* DYNAMIC IMPORT — Blink core/loader's script fetcher, the discovery half.
 *
 * `import(specifier)` is how a modern bundle reaches its lazy chunks, which is the moat surface this tool
 * exists to find: code the page ships and does not run. With no module loader installed the import resolved to
 * nothing at all — no module, no error, no record, and the `.then` never ran. A page asked to load code and the
 * engine answered silently, which is the one shape an unbuilt capability must not have.
 *
 * It records the specifier — qjs_chunks reports it and the trusted host fetches it, because SECURITY.md puts
 * every byte of network behind safeFetch and this sandbox cannot reach it — and then PARKS the load. 16.2.1.9
 * lets the host finish a load asynchronously, which is what a browser's module fetch is, so the loader hands
 * the engine a promise of the SOURCE TEXT and returns. The promise is the flow's own pending register, exactly
 * as core/fetch parks a request: qjs_provide settles it, and the load resumes on its reaction — compile, link,
 * evaluate, and the page's `await import(...)` continues from where it suspended. Nothing re-runs; the
 * importing scope is never re-entered, which would be a replay. */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "solver/engine.h"
#include "core/loader/module_loader.h"

/* THE CHUNK REGISTER: every specifier forced execution reached, in discovery order, deduped. It never forgets
   one — a chunk discovered on one branch is still a chunk when another branch does not take it. */
static char **g_chunks;
static int    g_chunk_n, g_chunk_cap;
static char  *g_chunk_join;

static void chunk_record(const char *name)
{
    int i;
    for (i = 0; i < g_chunk_n; i++)
        if (strcmp(g_chunks[i], name) == 0)
            return;
    if (g_chunk_n == g_chunk_cap) {
        int c = g_chunk_cap ? g_chunk_cap * 2 : 8;
        char **a = realloc(g_chunks, sizeof(*a) * (size_t)c);
        CHECK(a != NULL, "the chunk register allocation failed: a dropped chunk is a lazy endpoint never seen");
        g_chunks = a; g_chunk_cap = c;
    }
    g_chunks[g_chunk_n] = strdup(name);
    CHECK(g_chunks[g_chunk_n] != NULL, "the chunk URL allocation failed");
    g_chunk_n++;
}

const char *module_loader_chunks(void)
{
    size_t need = 1;
    int i;
    free(g_chunk_join);
    g_chunk_join = NULL;
    for (i = 0; i < g_chunk_n; i++) need += strlen(g_chunks[i]) + 1;
    g_chunk_join = malloc(need);
    CHECK(g_chunk_join != NULL, "the chunk-URL join allocation failed");
    g_chunk_join[0] = '\0';
    for (i = 0; i < g_chunk_n; i++) { strcat(g_chunk_join, g_chunks[i]); strcat(g_chunk_join, "\n"); }
    return g_chunk_join;
}

static JSModuleDef *module_load(JSContext *ctx, const char *module_name, void *opaque)
{
    JSValue promise, resolving[2];

    (void)opaque;
    chunk_record(module_name);
    promise = JS_NewPromiseCapability(ctx, resolving);
    if (JS_IsException(promise))
        return NULL;   /* OOM building the capability: the throw is already in flight and rejects the import */
    /* The URL goes on the FLOW's pending register, so the reaction that finishes the load belongs to the flow
       that imported — and to its COW delta. The flow cannot finish while the chunk is owed, which is what keeps
       a lazily-imported endpoint reachable. */
    /* A dynamic import is a GET of the module's URL with no body, and what it is owed is SOURCE TEXT. It used
       to park through engine_pending_fetch_url, which is the FETCH park: the two were indistinguishable while
       a host delivered a bare body string, and stopped being so the moment a reply became the record
       `fetch()` builds a Response out of — JS_ModuleLoadPending would have been handed that record to compile.
       Its own kind settles this promise with the reply's body. */
    engine_pending_module_url(ctx, resolving[0], module_name);
    JS_FreeValue(ctx, resolving[0]);
    JS_FreeValue(ctx, resolving[1]);
    JS_ModuleLoadPending(ctx, promise);   /* ownership transfers */
    return NULL;
}

void module_loader_install(JSRuntime *rt)
{
    DCHECK(rt != NULL, "the module loader was installed on no runtime");
    JS_SetModuleLoaderFunc(rt, NULL, module_load, NULL);
}
