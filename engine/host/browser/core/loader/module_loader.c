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
#include "core/agent_state.h"          /* the hook this component installs is what it holds for the agent */
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
    /* FETCH §4.3 SCHEME FETCH IS ASKED BY THE PARK ITSELF, WHICH IS WHY THIS LINE IS NOT A CALL. §16.2.1.10
       HostLoadImportedModule ends in HTML's "fetch a single module script", the same Fetch algorithm `fetch()`
       runs — so §4.3's switch on the URL's scheme decides who answers `import("data:text/javascript,export
       default 1")`, and the answer is a 200 built out of bytes already in this address space. This entry has
       no `deliver` closure to run that answer through and needs none: §4.3's response is placed ON the pending
       record and the flow's own delivery settles this promise from it, exactly as it settles from the trusted
       zone's reply (solver/engine.c's pending_park_request). */
    /* BOTH HALVES GO ON THE PARK, AND `resolving[1]` USED TO BE FREED HERE UNUSED — a capability minted and
       dropped, which is the write-with-no-reader half of §Architecture's broken-contract pair and cost the
       engine the whole of ECMAScript §13.3.10.3 "ContinueDynamicImport ( promiseCapability, moduleCompletion
       )"'s abrupt arm. With only the resolve half recorded, a load the network failed or the trusted zone
       refused left the delivery one thing it could do: settle SUCCESS with the empty source text, which
       compiles and evaluates a valid EMPTY MODULE. `import(u).catch(h)` therefore never ran `h`, and a
       bundle's fallback chunk — the code this tool exists to reach — was unreachable through the one door a
       page opens it with. HTML §8.1.6.7.3 "HostLoadImportedModule(referrer, moduleRequest, loadState,
       payload)"'s onSingleFetchComplete is what the park now has both halves for: "If moduleScript is null,
       then set completion to ThrowCompletion(a new TypeError)". */
    engine_pending_module_url(ctx, resolving[0], resolving[1], module_name);
    JS_FreeValue(ctx, resolving[0]);
    JS_FreeValue(ctx, resolving[1]);
    JS_ModuleLoadPending(ctx, promise);   /* ownership transfers */
    return NULL;
}

/* THE RUNTIME THIS AGENT'S HOOK WENT INTO. Deleting the specifier register took this component's only
   core/agent_state.h declaration with it, and the comment that replaced it said "there is no address for
   core/agent_state.h to name" — which core/platform.c refuted at the next build, because a row with a release
   and no declaration is the arm whose pairing cannot be checked. The claim was also simply false:
   `agent_state_ptr`'s own header names this case in as many words ("a recorded JSRuntime, … a hook this
   component installed into another"), and core/fetch/fetch.c's `g_fetch_rt` is the same slot for the same
   reason. What this component holds for the agent is not the LIST it used to keep, it is the HOOK — so the
   runtime it installed into is the slot, and NULLing it is what the release has to be caught forgetting. */
static JSRuntime *g_module_rt;

void module_loader_install(JSRuntime *rt)
{
    DCHECK(rt != NULL, "the module loader was installed on no runtime");
    /* A SECOND AGENT IN ONE PROCESS IS WHY THIS IS AN ASSERT AND NOT AN `if (g_module_rt) return;`. That latch
       is the shape core/agent_state.h opens by recording: `fetch_init` returned early on a slot its own release
       had never given back, so the second agent got a component reporting itself installed with every handle
       stale. Here the stale value would be a runtime that is GONE, and §16.2.1.10 HostLoadImportedModule would
       be reached on the live one through a hook nobody re-registered. */
    DCHECK(g_module_rt == NULL, "the module loader is already installed for this agent — its release either "
                                "did not run or did not give the runtime back, and a re-install would leave "
                                "ECMAScript §16.2.1.10 HostLoadImportedModule pointing at the agent that went");
    JS_SetModuleLoaderFunc(rt, NULL, module_load, NULL);
    g_module_rt = rt;
    agent_state_ptr("module_loader", &g_module_rt, "the runtime ECMAScript §16.2.1.10 HostLoadImportedModule "
                                                   "was installed into");
}

/* THE HOOK IS THE AGENT'S AND WAS GIVEN BACK BY NOTHING. This row was on core/platform.c's list with an EMPTY
   release column and no release function existed anywhere, which is the arm that column's pairing silently
   passes. A component gives back what it claimed rather than relying on the runtime being freed shortly
   afterwards — and the exactness matters here for the corpus hosts, which take a runtime down and bring
   another up per file. */
void module_loader_free(JSRuntime *rt)
{
    DCHECK(rt != NULL, "the module loader was released against no runtime");
    /* THE RELEASE MUST BE ABOUT THE RUNTIME THE INSTALL CLAIMED, and the two are independently supplied — the
       host hands `rt` down its own teardown list while the install recorded its own. Un-hooking a runtime this
       component never hooked would report success and leave the claimed one holding the hook. */
    DCHECK(g_module_rt == rt, "the module loader is being released against a runtime it was not installed "
                              "into — the hook it claimed is on a different one and would survive this agent");
    JS_SetModuleLoaderFunc(rt, NULL, NULL, NULL);
    g_module_rt = NULL;
}
