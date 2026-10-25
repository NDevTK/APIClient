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
#include <string.h>
#include <stdlib.h>
#include "check.h"
#include "quickjs.h"
#include "solver/engine.h"
#include "core/agent_state.h"          /* the hook this component installs is what it holds for the agent */
#include "core/dom/document.h"         /* the current settings object's API base URL */
#include "core/events/event_target.h"  /* EVENT_HANDLER_SCRIPT_NAME, a null referrer */
#include "core/url/url.h"
#include "core/loader/import_map.h"
#include "core/loader/module_loader.h"
#include "core/idl_args.h"
#include "solver/concolic.h"
#include "solver/reclaim.h"

/* HTML §8.1.5.1 "The resolution algorithm"'s resolve a URL-like module specifier. Fills `out` and returns
   true for a URL; false is the algorithm's null, with `out` initialised-and-empty either way. */
static bool ml_resolve_url_like(const char *specifier, const UrlRecord *base, UrlRecord *out)
{
    size_t n = strlen(specifier);

    if (!strncmp(specifier, "/", 1) || !strncmp(specifier, "./", 2) || !strncmp(specifier, "../", 3))
        return url_parse(out, specifier, n, base);
    return url_parse(out, specifier, n, NULL);
}

/* Steps 1-3 of resolve a module specifier: the referring script's base URL, or the current settings object's
   API base URL when the referrer is null. `referrer` is the name the importing bytecode carries, which this
   engine computed: a classic or module script's base URL (solver/engine.c names every program),
   EVENT_HANDLER_SCRIPT_NAME (HTML's null referrer), or JS_EVAL_SCRIPT_NAME. Returns false only for a name that
   does not parse, which the DFAIL refuses.
   NAMED RESIDUAL. Not covered: eval code and a dynamic function's referrer, which ECMAScript §19.2.1.1
   "PerformEval ( source, strictCaller, direct )" sets to the running context's ScriptOrModule; quickjs names
   that code JS_EVAL_SCRIPT_NAME and keeps no script-or-module, so it resolves against the API base URL. The
   next diff records the caller's script name on eval and dynamic-function bytecode. Its absence shows as a
   relative `import()` inside eval'd code of an external script outside the document's directory fetching
   from the document's directory. */
static bool ml_referrer_base_url(JSContext *ctx, const char *referrer, UrlRecord *base)
{
    const char *s = referrer;

    DCHECK(referrer != NULL && referrer[0] != '\0',
           "a module specifier was resolved for a referrer with no name — every compile in this engine names "
           "its program");
    DCHECKF(strchr(referrer, '\001') == NULL,
            "the referrer `%s` is a module map key carrying import attributes — only a synthetic module is "
            "keyed so, and a synthetic module requests no modules", referrer);
    if (!strcmp(referrer, EVENT_HANDLER_SCRIPT_NAME) || !strcmp(referrer, JS_EVAL_SCRIPT_NAME))
        s = document_base_url(ctx);
    if (url_parse(base, s, strlen(s), NULL))
        return true;
    DFAILF("the module specifier referrer `%s` is no URL — solver/engine.c names a program by its document's "
           "base URL or its response URL, both serialized URLs, so this name came from a compile that names "
           "its program otherwise", referrer);
    return false;
}

/* HTML §8.1.5.1 "The resolution algorithm"'s resolve a module specifier, as quickjs's normalize hook: it
   returns the serialized URL (js_malloc'd; js_host_resolve_imported_module frees it), which is the module map
   key and the address the load parks, or NULL with a TypeError thrown. The import map half (steps 4-5 and
   9-11, and step 13's resolved module set) is core/loader/import_map.c's. */
static char *module_normalize(JSContext *ctx, const char *referrer, const char *specifier, void *opaque)
{
    UrlRecord base, as_url, result;
    char *serialized_base = NULL, *normalized = NULL, *serialized, *out = NULL;
    bool have_as_url;
    int r;

    (void)opaque;
    DCHECK(specifier != NULL, "a module specifier was resolved with no specifier");
    url_record_init(&base);
    url_record_init(&as_url);
    url_record_init(&result);
    if (!ml_referrer_base_url(ctx, referrer, &base)) {
        JS_ThrowTypeError(ctx, "the module referrer '%s' names no base URL", referrer);
        goto done;
    }
    /* Steps 6-8. */
    serialized_base = url_serialize(&base, false);
    CHECK(serialized_base != NULL, "OOM serializing a module referrer's base URL");
    have_as_url = ml_resolve_url_like(specifier, &base, &as_url);
    normalized = have_as_url ? url_serialize(&as_url, false) : reclaim_strdup(specifier);
    CHECK(normalized != NULL, "OOM normalizing a module specifier");
    /* Steps 9-11. */
    r = import_map_resolve(ctx, serialized_base, normalized, have_as_url ? &as_url : NULL, &result);
    if (r < 0) goto done;
    /* Step 12. */
    if (r == 0 && have_as_url) {
        CHECK(url_record_copy(&result, &as_url), "OOM copying a resolved module specifier");
        r = 1;
    }
    if (r == 0) {
        /* Step 14. */
        JS_ThrowTypeError(ctx, "Failed to resolve module specifier \"%s\". Relative references must start "
                               "with either \"/\", \"./\", or \"../\".", specifier);
        goto done;
    }
    /* Step 13. */
    import_map_add_resolved(ctx, serialized_base, normalized, have_as_url ? &as_url : NULL);
    serialized = url_serialize(&result, false);
    CHECK(serialized != NULL, "OOM serializing a resolved module specifier");
    out = js_strdup(ctx, serialized);
    free(serialized);
    /* js_strdup's NULL has thrown the OOM, which rejects the import like any other abrupt resolution. */
done:
    free(normalized);
    free(serialized_base);
    url_record_free(&result);
    url_record_free(&as_url);
    url_record_free(&base);
    return out;
}

/* HTML §8.1.6.7.1 HostGetImportMetaProperties(moduleRecord)'s resolveFunction, `import.meta.resolve`. Step 1's
   `? ToString(specifier)` is the declaration's DOMString conversion, so the page's toString runs on the
   trampoline. Steps 2-3 resolve against the module the function was minted for: one declaration serves every
   module, so that module's base URL rides the function object under g_ml_base_atom, read back off the callee. */
#define ML_RESOLVE_STAGES(X) \
    X(ML_RESOLVE_RUN = IDL_STEP_FIRST, \
      "HTML §8.1.6.7.1 HostGetImportMetaProperties(moduleRecord) resolveFunction steps 2-3 (resolve a module " \
      "specifier given moduleScript and specifier, and return the serialization of url)")
enum { ML_RESOLVE_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const ML_RESOLVE_STEPS[] = { ML_RESOLVE_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct { int unused; } MlResolveState;

static int   g_ml_resolve_id = -1;            /* the resolveFunction's declaration */
static JSAtom g_ml_base_atom = JS_ATOM_NULL;  /* the function object's slot holding its module's base URL */

static void ml_resolve_visit(JSContext *ctx, void *st, JSStepVisit *v) { (void)ctx; (void)st; (void)v; }

static int ml_resolve_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                           JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    JSValue bv;
    const char *base, *spec;
    char *url;
    int r;

    (void)st; (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);
    DCHECK(hdr->stage == ML_RESOLVE_RUN, "import.meta.resolve resumed at a stage §8.1.6.7.1 does not have");
    DCHECK(argc >= 1, "import.meta.resolve reached its body with no specifier — the declaration requires it");
    r = JS_GetOwnSlot(ctx, &bv, hdr->func_obj, g_ml_base_atom);
    if (r < 0) return -1;
    DCHECK(r > 0 && JS_IsString(bv),
           "import.meta.resolve ran through a function carrying no module base URL — module_import_meta writes "
           "it on the line after the mint, and nothing else mints this declaration's functions");
    base = JS_ToCString(ctx, bv);
    JS_FreeValue(ctx, bv);
    if (!base) return -1;
    /* An unknown specifier resolves to an unknown URL, its example the real resolution of the operand's. */
    if (concolic_is(argv[0])) {
        JSValue ex = concolic_example(ctx, argv[0]), real = JS_UNDEFINED;

        if (JS_IsString(ex)) {
            const char *es = JS_ToCString(ctx, ex);
            char *u;

            CHECK(es, "OOM reading an unknown module specifier's example");
            u = module_normalize(ctx, base, es, NULL);
            JS_FreeCString(ctx, es);
            if (u) { real = JS_NewString(ctx, u); js_free(ctx, u); }
            else JS_FreeValue(ctx, JS_GetException(ctx));   /* that example does not resolve: no example */
        }
        JS_FreeValue(ctx, ex);
        JS_FreeCString(ctx, base);
        *presult = concolic_new_derived(ctx, "import.meta.resolve", argv, 1, real);
        return JS_IsException(*presult) ? -1 : 0;
    }
    spec = JS_ToCString(ctx, argv[0]);
    if (!spec) { JS_FreeCString(ctx, base); return -1; }
    url = module_normalize(ctx, base, spec, NULL);   /* a failure has thrown step 2's TypeError */
    JS_FreeCString(ctx, spec);
    JS_FreeCString(ctx, base);
    if (!url) return -1;
    *presult = JS_NewString(ctx, url);
    js_free(ctx, url);
    return JS_IsException(*presult) ? -1 : 0;
}

static const IdlStepDecl ml_resolve_decl = {
    ml_resolve_step, sizeof(MlResolveState), ml_resolve_visit, NULL,
    "HTML §8.1.6.7.1 HostGetImportMetaProperties(moduleRecord) resolveFunction", ML_RESOLVE_STEPS
};

/* HTML §8.1.6.7.1 HostGetImportMetaProperties(moduleRecord), as quickjs's JSModuleImportMetaFunc: `url` is
   moduleScript's base URL serialized (step 3) and `resolve` its resolveFunction (step 5), defined as ECMAScript
   §13.3.12.1 step 4.c's CreateDataPropertyOrThrow does. This engine names a module by that base URL
   (solver/engine.c's row name; the name ml_referrer_base_url resolves a dependency against), so step 3 reads
   the name, asserted to be a serialized URL. */
static int module_import_meta(JSContext *ctx, JSModuleDef *m, JSValueConst meta)
{
    JSAtom na = JS_GetModuleName(ctx, m);
    const char *name = JS_AtomToCString(ctx, na);
    JSValue fn;

    JS_FreeAtom(ctx, na);
    if (!name) return -1;
    {
        UrlRecord u;
        bool ok;
        char *ser;

        url_record_init(&u);
        ok = url_parse(&u, name, strlen(name), NULL);
        ser = ok ? url_serialize(&u, false) : NULL;
        DCHECKF(ok && ser && !strcmp(ser, name),
                "a module named `%s` evaluated import.meta, and that is not a serialized URL — §8.1.6.7.1 step 2 "
                "asserts a JavaScript module script's base URL is not null, and this engine names a module by it",
                name);
        free(ser);
        url_record_free(&u);
    }
    fn = idl_step_function(ctx, "resolve", g_ml_resolve_id);
    if (JS_IsException(fn)) { JS_FreeCString(ctx, name); return -1; }
    if (JS_DefinePropertyValue(ctx, fn, g_ml_base_atom, JS_NewString(ctx, name), 0) < 0 ||
        JS_DefinePropertyValueStr(ctx, meta, "url", JS_NewString(ctx, name), JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, fn);
        JS_FreeCString(ctx, name);
        return -1;
    }
    JS_FreeCString(ctx, name);
    return JS_DefinePropertyValueStr(ctx, meta, "resolve", fn, JS_PROP_C_W_E) < 0 ? -1 : 0;
}

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

void module_loader_install(JSContext *ctx)
{
    static const IdlArgType RESOLVE_ARGS[1] = { IDL_DOMSTRING };
    JSRuntime *rt = JS_GetRuntime(ctx);
    JSValue key;

    DCHECK(rt != NULL, "the module loader was installed on no runtime");
    /* A SECOND AGENT IN ONE PROCESS IS WHY THIS IS AN ASSERT AND NOT AN `if (g_module_rt) return;`. That latch
       is the shape core/agent_state.h opens by recording: `fetch_init` returned early on a slot its own release
       had never given back, so the second agent got a component reporting itself installed with every handle
       stale. Here the stale value would be a runtime that is GONE, and §16.2.1.10 HostLoadImportedModule would
       be reached on the live one through a hook nobody re-registered. */
    DCHECK(g_module_rt == NULL, "the module loader is already installed for this agent — its release either "
                                "did not run or did not give the runtime back, and a re-install would leave "
                                "ECMAScript §16.2.1.10 HostLoadImportedModule pointing at the agent that went");
    JS_SetModuleLoaderFunc(rt, module_normalize, module_load, NULL);
    JS_SetModuleImportMetaFunc(rt, module_import_meta);
    g_module_rt = rt;
    /* §8.1.6.7.1's resolveFunction is CreateBuiltinFunction(steps, 1, "resolve", « »): one argument, which
       step 1 converts with ToString. */
    g_ml_resolve_id = idl_method_id_step(ctx, RESOLVE_ARGS, 1, NULL, 0, &ml_resolve_decl, 0);
    key = JS_NewSymbol(ctx, "importMetaResolveBase", false);
    CHECK(!JS_IsException(key), "the import.meta.resolve base URL slot key could not be allocated");
    g_ml_base_atom = JS_ValueToAtom(ctx, key);
    JS_FreeValue(ctx, key);
    CHECK(g_ml_base_atom != JS_ATOM_NULL, "the import.meta.resolve base URL slot key could not be interned");
    agent_state_id("module_loader", &g_ml_resolve_id, "HTML §8.1.6.7.1's resolveFunction declaration");
    agent_state_atom("module_loader", &g_ml_base_atom, "the resolveFunction's module base URL slot key");
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
    JS_SetModuleImportMetaFunc(rt, NULL);
    g_module_rt = NULL;
    JS_FreeAtomRT(rt, g_ml_base_atom);
    g_ml_base_atom = JS_ATOM_NULL;
    g_ml_resolve_id = -1;
}
