/* DYNAMIC IMPORT — Blink core/loader's script fetcher, the discovery half.
 *
 * `import(specifier)` is how a modern bundle reaches its lazy chunks, which is the moat surface this tool
 * exists to find: code the page ships and does not run. With no module loader installed the import resolved to
 * nothing at all — no module, no error, no record, and the `.then` never ran. A page asked to load code and the
 * engine answered silently, which is the one shape an unbuilt capability must not have.
 *
 * WHAT THIS DOES is record the specifier so qjs_chunks reports it and the trusted host can fetch it. WHAT IT
 * CANNOT DO YET is deliver the body: JSModuleLoaderFunc is synchronous C and the fetch is the host's, so there
 * is no point in it where the flow can park. Re-running the importing scope once the body arrives would be a
 * REPLAY, which is banned outright — the flow must SUSPEND and resume. So the import fails, loudly and by name,
 * and the import rejects the way a browser's does for a module whose fetch failed. */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
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
    (void)opaque;
    chunk_record(module_name);
    /* A module the engine cannot fetch FAILS TO LOAD, which is what a browser does for one whose network
       request did not succeed: the import promise rejects, the page's own catch runs, and everything after it
       keeps exploring. That is faithful, not a stub — and the specifier is on the register either way, which is
       the discovery this component is for. Delivering the body is the part that is missing, and it announces
       itself where a body arrives with nothing to hand it to. */
    JS_ThrowReferenceError(ctx, "module not provided: %s", module_name);
    return NULL;
}

void module_loader_install(JSRuntime *rt)
{
    DCHECK(rt != NULL, "the module loader was installed on no runtime");
    JS_SetModuleLoaderFunc(rt, NULL, module_load, NULL);
}
