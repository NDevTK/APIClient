/* DYNAMIC IMPORT — Blink core/loader's script fetcher, the discovery half.
 *
 * `import(specifier)` is how a modern bundle reaches its lazy chunks, which is the moat surface this tool
 * exists to find: code the page ships and does not run. With no module loader installed the import resolved to
 * nothing at all — no module, no error, no record, and the `.then` never ran. A page asked to load code and the
 * engine answered silently, which is the one shape an unbuilt capability must not have.
 *
 * It records the specifier — qjs_chunks reports it and the trusted host fetches it, because SECURITY.md puts
 * every byte of network behind safeFetch and this sandbox cannot reach it — and then PARKS the load.
 * ECMAScript §16.2.1.10 HostLoadImportedModule lets the host finish a load asynchronously, and §16.2.1.11
 * FinishLoadingImportedModule is the completion it hands back — which is what a browser's module fetch is. So
 * the loader hands the engine a promise of the SOURCE TEXT and returns. The promise is the flow's own pending
 * register, exactly as core/fetch parks a request: qjs_provide settles it, and the load resumes on its
 * reaction — compile, link, evaluate, and the page's `await import(...)` continues from where it suspended.
 * Nothing re-runs; the importing scope is never re-entered, which would be a replay. */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "solver/engine.h"
#include "core/agent_state.h"
#include "core/fetch/scheme_fetch.h"   /* Fetch §4.3, which decides who answers a module load's fetch */
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
    /* FETCH §4.3 SCHEME FETCH, ASKED OF THIS ENTRY AS IT IS ASKED OF EVERY OTHER. §16.2.1.10
       HostLoadImportedModule ends in HTML's "fetch a single module script", which is the same Fetch algorithm
       `fetch()` runs — so §4.3's switch on the URL's scheme decides who answers `import("data:text/javascript,
       export default 1")`, and the answer is a 200 built out of bytes already in this address space. The park
       below reaches a trusted zone that can fetch nothing but an HTTP(S) scheme, and §4.3's local answer has no
       delivery on THIS park yet, so this CRASHES naming what to build (core/fetch/scheme_fetch.h). */
    scheme_fetch_require_network(ctx, module_name);
    engine_pending_module_url(ctx, resolving[0], module_name);
    JS_FreeValue(ctx, resolving[0]);
    JS_FreeValue(ctx, resolving[1]);
    JS_ModuleLoadPending(ctx, promise);   /* ownership transfers */
    return NULL;
}

void module_loader_install(JSRuntime *rt)
{
    DCHECK(rt != NULL, "the module loader was installed on no runtime");
    /* THE REGISTER'S PRE-INIT STATE, ASSERTED HERE. It is malloc'd rather than a JS value, so a previous
       agent's leftover is invisible to both of JS_FreeRuntime's censuses and would simply be REPORTED as this
       agent's chunks — a specifier the page never imported, handed to safeFetch as a discovery. */
    DCHECK(g_chunks == NULL && g_chunk_n == 0 && g_chunk_cap == 0 && g_chunk_join == NULL,
           "the chunk register still holds a previous agent's specifiers — this agent would report imports "
           "that belong to another document");
    /* WHAT THIS COMPONENT HOLDS FOR THE AGENT, DECLARED — core/agent_state.h. The register is not a cache: it
       is what qjs_chunks answers with, so a stale entry is not a slow start, it is a document reporting
       another document's lazy chunks. §16.2.1.10 HostLoadImportedModule is the RUNTIME's hook and is released
       below rather than declared, because its storage is in the JSRuntime and there is no static here to point
       at. */
    agent_state_ptr("module_loader", &g_chunks,
                    "the register of every specifier §16.2.1.10 HostLoadImportedModule was asked for");
    agent_state_flag("module_loader", &g_chunk_n, "the number of specifiers in that register");
    agent_state_flag("module_loader", &g_chunk_cap, "the capacity of that register");
    agent_state_ptr("module_loader", &g_chunk_join, "the newline-joined register qjs_chunks answers with");
    JS_SetModuleLoaderFunc(rt, NULL, module_load, NULL);
}

/* THE REGISTER IS THE AGENT'S AND WAS FREED BY NOTHING. Every specifier is a strdup and the join is a malloc,
   so what this component leaked appears in NEITHER of JS_FreeRuntime's two censuses — a malloc'd block is not
   on gc_obj_list and is not an atom — and the only reader of a surviving one is the next agent's
   `module_loader_chunks`, which would report another document's lazy chunks as this one's. This row was on
   core/platform.c's list with an EMPTY release column and no release function existed anywhere, which is the
   arm that column's pairing silently passes. */
void module_loader_free(JSRuntime *rt)
{
    int i;

    DCHECK(rt != NULL, "the module loader was released against no runtime");
    /* THE HOOK COMES OFF FIRST, so this release is the exact inverse of the install above: §16.2.1.10
       HostLoadImportedModule is state this component put in the RUNTIME, and a component gives back what it
       claimed rather than relying on the runtime being freed shortly afterwards. */
    JS_SetModuleLoaderFunc(rt, NULL, NULL, NULL);
    for (i = 0; i < g_chunk_n; i++) free(g_chunks[i]);
    free(g_chunks);
    free(g_chunk_join);
    g_chunks = NULL;
    g_chunk_join = NULL;
    g_chunk_n = g_chunk_cap = 0;
}
