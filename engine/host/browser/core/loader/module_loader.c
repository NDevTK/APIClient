/* DYNAMIC IMPORT — Blink core/loader's script fetcher, the discovery half.
 *
 * `import(specifier)` is how a modern bundle reaches its lazy chunks, which is the moat surface this tool
 * exists to find: code the page ships and does not run. With no module loader installed the import resolved to
 * nothing at all — no module, no error, no record, and the `.then` never ran. A page asked to load code and the
 * engine answered silently, which is the one shape an unbuilt capability must not have.
 *
 * It PARKS the load, and the park is the whole of what it records. The trusted host fetches the address,
 * because SECURITY.md puts every byte of network behind safeFetch and this sandbox cannot reach it.
 *
 * IT USED TO KEEP A SECOND REGISTER OF SPECIFIERS BESIDE THE PARK, and that register is deleted rather than
 * repaired. Its purpose was to tell the trusted zone that these particular addresses were CODE, so the reply
 * had to be JS-typed — and it could only ever say that about the addresses THIS FILE recorded. A document's
 * own `<script src>` and an injected one park in the same register through the same seam and were in no such
 * list, so they reached the chokepoint with no load class at all and a cross-origin HTML or JSON body served
 * for one of them was ingested as data and compiled. A question some entries ask and others do not is one
 * missing capability wearing two names: the question is Fetch §2.2.5 "Requests"' DESTINATION, it is a property
 * of the REQUEST, and it now rides the park like the method does (solver/engine.h). A side table filled by one
 * producer cannot answer for the others, and nothing about it could say so.
 * ECMAScript §16.2.1.10 HostLoadImportedModule lets the host finish a load asynchronously, and §16.2.1.11
 * FinishLoadingImportedModule is the completion it hands back — which is what a browser's module fetch is. So
 * the loader hands the engine a promise of the SOURCE TEXT and returns. The promise is the flow's own pending
 * register, exactly as core/fetch parks a request: qjs_provide settles it, and the load resumes on its
 * reaction — compile, link, evaluate, and the page's `await import(...)` continues from where it suspended.
 * Nothing re-runs; the importing scope is never re-entered, which would be a replay. */
#include "check.h"
#include "quickjs.h"
#include "solver/engine.h"
#include "core/fetch/scheme_fetch.h"   /* Fetch §4.3, which decides who answers a module load's fetch */
#include "core/loader/module_loader.h"

static JSModuleDef *module_load(JSContext *ctx, const char *module_name, void *opaque)
{
    JSValue promise, resolving[2];

    (void)opaque;
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
    /* WHAT THIS COMPONENT HOLDS FOR THE AGENT IS ONE THING AND IT IS NOT A STATIC HERE: §16.2.1.10
       HostLoadImportedModule is the RUNTIME's hook, so there is no address for core/agent_state.h to name and
       the release below is the whole of the declaration. The register of specifiers that used to sit beside it
       — and that used to be the thing this pairing was about — is gone with the side list it fed. */
    JS_SetModuleLoaderFunc(rt, NULL, module_load, NULL);
}

/* THE HOOK IS THE AGENT'S AND WAS GIVEN BACK BY NOTHING. This row was on core/platform.c's list with an EMPTY
   release column and no release function existed anywhere, which is the arm that column's pairing silently
   passes. A component gives back what it claimed rather than relying on the runtime being freed shortly
   afterwards — and the exactness matters here for the corpus hosts, which take a runtime down and bring
   another up per file. */
void module_loader_free(JSRuntime *rt)
{
    DCHECK(rt != NULL, "the module loader was released against no runtime");
    JS_SetModuleLoaderFunc(rt, NULL, NULL, NULL);
}
