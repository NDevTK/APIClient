/* HTML §10.2.6.3 "Dedicated workers and the Worker interface", and HTML §10.2.4 "Processing model"'s run a
 * worker for a dedicated worker.
 *
 * A dedicated worker is a second agent in its owner's runtime: HTML §8.1.2.2 obtains it with isTopLevel false,
 * "Set agentCluster to the agent cluster which contains ownerAgent", and this instance is that one cluster. The
 * constructor builds the agent (core/platform.h's platform_create_agent), its one realm with a
 * DedicatedWorkerGlobalScope global, adds the agent to the running flow (solver/flow.h's flow_agent_add), and
 * fetches the script through the flow's register (engine_pending_resource_url, destination "worker"). The
 * delivery installs the classic script, or the module script graph's root, as the worker agent's program, which
 * runs when the scheduler selects that agent. Messages cross only through the entangled outside and inside
 * ports, as StructuredSerialize output (core/events/message_port.c).
 *
 * The parallel half of run a worker is not observable to the page, so it runs at construction; the script's
 * fetch is the first point at which the owner and the worker proceed independently. */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/agent_state.h"
#include "core/idl_args.h"
#include "core/platform.h"
#include "core/realm.h"
#include "core/dom/document.h"
#include "core/events/event.h"
#include "core/events/event_target.h"
#include "core/events/message_port.h"
#include "core/fetch/fetch.h"
#include "core/frame/policy_container.h"
#include "core/frame/secure_context.h"
#include "core/html/trusted_types.h"
#include "core/loader/script_fetch.h"
#include "core/mime/mime_type.h"
#include "core/timing/task_source.h"
#include "core/url/url.h"
#include "core/workers/worker.h"
#include "core/workers/worker_global_scope.h"
#include "solver/concolic.h"
#include "solver/engine.h"
#include "solver/flow.h"

/* §10.2.6.3's Worker: its outside port, and the realm run a worker created for it. The record is written once,
   by the constructing flow, while the object is that flow's own, so it needs no COW capture. */
typedef struct {
    JSValue outside_port;   /* owned */
    JSContext *realm;       /* the worker global scope's realm, borrowed: each flow running the agent holds it */
    JSContext *owner;       /* the outside settings' realm, the Worker's relevant realm (borrowed) */
    char *url;              /* the worker URL, serialized (owned) */
} WorkerData;

static JSClassID g_worker_class;
static JSRuntime *g_worker_rt;
static int g_ctor_stepid = -1, g_id_post = -1, g_error_task_stepid = -1;
/* Per worker realm: the Array [outside port, inside port, Worker]: run a worker enables the two queues after the
   script ran, and fires `error` at the Worker for a module script that did not parse. */
static JSClassID g_ports_slot = JS_INVALID_CLASS_ID;

static WorkerData *worker_of(JSValueConst v)
{
    return JS_GetOpaque(v, g_worker_class);
}

static void worker_finalizer(JSRuntime *rt, JSValue val)
{
    JSClassID id;
    WorkerData *w = JS_GetAnyOpaque(val, &id);

    if (!w) return;
    JS_FreeValueRT(rt, w->outside_port);
    free(w->url);
    free(w);
}

static void worker_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    JSClassID id;
    WorkerData *w = JS_GetAnyOpaque(val, &id);

    if (w) JS_MarkValue(rt, w->outside_port, mark_func);
}

/* ---- the task that fires `error` at the Worker ----------------------------------------------------------- */

/* HTML §10.2.4 run a worker, onComplete step 1.1: "Queue a global task on the DOM manipulation task source given
   worker's relevant global object to fire an event named error at worker." */
#define WORKER_ERROR_STAGES(X) \
    X(WERR_FIRE, "HTML §10.2.4 run a worker, onComplete step 1.1 (fire an event named error at worker)")
enum { WORKER_ERROR_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const WORKER_ERROR_STEPS[] = { WORKER_ERROR_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    JSStepHdr   hdr;       /* first: the driver writes the def and the operand bounds through it */
    uint8_t     fphase;
    uint8_t     started;
    JSValue     ev;        /* owned */
    EventFireCb cb;
} WorkerErrorTask;

static void worker_error_visit(JSContext *ctx, void *stp, JSStepVisit *v)
{
    WorkerErrorTask *s = stp;
    int k;

    if (!s->started) return;   /* the block is zeroed until the first step places the owned fields */
    v->val(ctx, &s->ev);
    STEP_CB_FOREACH(s->cb, k) v->val(ctx, &s->cb[k]);
}

static int worker_error_step(JSContext *ctx, void *stp, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    WorkerErrorTask *s = stp;
    int r;

    STEP_DISPATCH(WORKER_ERROR_STAGES, s->hdr.stage, s->hdr.def->algorithm, JS_STEP_ABRUPT);

    STEP_ARM(WERR_FIRE);
        if (!s->started) {
            int k;

            s->started = 1;
            s->fphase = 0;
            s->ev = JS_UNDEFINED;
            STEP_CB_FOREACH(s->cb, k) s->cb[k] = JS_UNDEFINED;
            s->ev = event_new(ctx, "error", /*bubbles*/ false, /*cancelable*/ false);
            if (JS_IsException(s->ev)) { s->ev = JS_UNDEFINED; return JS_STEP_ABRUPT; }
        }
        r = event_target_fire_run(ctx, &s->fphase, STEP_CB(s->cb), step_arg(&s->hdr, 0), s->ev, JS_UNDEFINED,
                                  cb_result, NULL, out_cb, out_argc);
        if (r > 0) return r;
        JS_FreeValue(ctx, s->ev);
        s->ev = JS_UNDEFINED;
        return r < 0 ? JS_STEP_ABRUPT : JS_STEP_DONE;
}

static const JSTrampStepDef worker_error_def = {
    sizeof(WorkerErrorTask), worker_error_step, NULL, 0,
    .visit = worker_error_visit,
    .algorithm = "HTML §10.2.4 run a worker — the task that fires `error` at a Worker whose script is null",
    .steps = WORKER_ERROR_STEPS
};

static void worker_queue_error(JSContext *ctx, JSValueConst worker)
{
    JSValue fn = JS_NewCFunction2(ctx, NULL, "workerErrorTask", 1, JS_CFUNC_step, g_error_task_stepid);

    CHECK(!JS_IsException(fn), "§10.2.4: the Worker error task's callee could not be allocated");
    JS_EnqueueCallTask(ctx, fn, 1, &worker, TASK_SOURCE_DOM_MANIPULATION);
    JS_FreeValue(ctx, fn);
}

/* ---- run a worker's script ------------------------------------------------------------------------------- */

/* The fetch's processResponseConsumeBody, then run a worker's onComplete, for "fetch a classic worker script"
   (magic 0) and the root of "fetch a module worker script graph" (magic 1). Called as a flow by the register's
   delivery with the reply record, or JS_NULL for a network error; data[0] is the Worker.
   HTML §8.1.4.2: a null body or a non-ok status is a null script; so is a reply whose extracted MIME type is not a
   JavaScript MIME type, for a classic worker only when the URL is HTTP(S) ("Other fetch schemes are exempted")
   and for a module script always ("fetch a single module script" step 13). The source text is the UTF-8 decode. */
static JSValue worker_script_deliver(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                     int magic, JSValueConst *data)
{
    JSValueConst worker = data[0], reply = argc > 0 ? argv[0] : JS_NULL;
    WorkerData *w = worker_of(worker);
    JSContext *wctx;
    bool ok, module = magic == 1;
    JSValue *frame = NULL, program = JS_UNDEFINED;

    (void)this_val;
    DCHECK(w != NULL && w->realm != NULL, "a worker script was delivered to something that is not a constructed Worker");
    wctx = w->realm;
    ok = !JS_IsNull(reply) && script_fetch_status_ok(fetch_reply_status(ctx, reply));
    if (ok && (module || !strncmp(w->url, "http:", 5) || !strncmp(w->url, "https:", 6))) {
        HeaderList hl = { 0 };
        char *content_type;
        MimeType mt;

        fetch_reply_header_list(ctx, reply, &hl);
        content_type = header_list_get(&hl, "content-type");
        mime_type_init(&mt);
        ok = mime_type_extract(&mt, content_type) && mime_type_is_javascript(&mt);
        mime_type_free(&mt);
        free(content_type);
        header_list_free(&hl);
    }
    if (ok) {
        JSValue body = fetch_reply_body(ctx, reply);
        size_t n = 0, src_n = 0;
        const uint8_t *bytes = fetch_body_bytes(ctx, body, &n);
        /* "UTF-8 decoding" is the module script's decode too, so the one decoder serves both algorithms. */
        char *src = script_fetch_module_source_text((const char *)bytes, n, &src_n);

        JS_FreeValue(ctx, body);
        CHECK(src != NULL, "§10.2.4: OOM decoding a worker's script");
        if (module) {
            /* The module script is created and run on the worker agent's own turn (flow_step_agent), where a
               parse error takes onComplete's null-script arm through the program-done hook. */
            program = JS_NewArray(wctx);
            CHECK(!JS_IsException(program), "§10.2.4: a module worker's program could not be held");
            JS_SetPropertyUint32(wctx, program, 0, JS_NewString(wctx, w->url));
            JS_SetPropertyUint32(wctx, program, 1, JS_NewStringLen(wctx, src, src_n));
        } else {
            /* "Creating a classic script": a parse error is a script whose error to rethrow is non-null, which
               onComplete treats as a null script. */
            frame = JS_FlowNew(wctx, src, src_n, w->url, JS_EVAL_TYPE_GLOBAL);
            if (!frame) {
                JS_FreeValue(wctx, JS_GetException(wctx));
                ok = false;
            }
        }
        free(src);
    }
    if (!ok) {
        /* onComplete step 1: fire `error` at the worker and discard the environment; the agent stays on the
           flow with no program and no work, so it is never selected again. */
        worker_queue_error(ctx, worker);
        return JS_UNDEFINED;
    }
    {
        FlowAgent *a;

        /* Step 10: run the script — as the worker agent's program, which the scheduler runs when it selects
           that agent (solver/engine.c's flow_step_agent). */
        a = flow_agent_of(flow_running(), JS_GetRealmAgent(wctx));
        DCHECK(a->frame == NULL && JS_IsUndefined(a->module),
               "a worker's script was installed over a program its agent already runs");
        if (module) a->module = program;
        else a->frame = frame;
    }
    return JS_UNDEFINED;
}

static void worker_enable_ports(JSContext *wctx);

static JSValue worker_evaluated(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic,
                                JSValueConst *data)
{
    (void)this_val; (void)argc; (void)argv; (void)magic; (void)data;
    worker_enable_ports(ctx);
    return JS_UNDEFINED;
}

/* Run a worker's steps after the script has run (engine_set_agent_program_done_hook): "Enable outside port's port
   message queue" and, for a dedicated worker, "enable the port message queue of the worker's implicit port". A
   module script that did not parse is onComplete's null-script arm instead. A module is run once its evaluation
   promise settles: this engine loads a module's static imports during its evaluation, where "fetch a module
   worker script graph" has loaded the whole graph before onComplete, so the body has run by then.
   Named residual. Not covered: a module with a top-level await, whose ports run a worker enables before the await
   resolves. Next diff builds: the graph fetched before evaluation, as HTML §8.1.4.2 orders it. Absence shows as: a
   module worker's message handler assigned after a top-level await receives messages a browser drops. */
static void worker_program_done(JSContext *wctx, bool ran, JSValueConst evaluation)
{
    if (!ran) {
        JSValue ports = realm_value_get(wctx, g_ports_slot), worker = JS_GetPropertyUint32(wctx, ports, 2);
        WorkerData *w = worker_of(worker);

        DCHECK(w != NULL, "a worker realm's port record names no Worker");
        worker_queue_error(w->owner, worker);
        JS_FreeValue(wctx, worker);
        JS_FreeValue(wctx, ports);
        return;
    }
    if (JS_IsObject(evaluation)) {
        JSValue fn = JS_NewCFunctionData(wctx, worker_evaluated, 1, 0, 0, NULL), r;

        CHECK(!JS_IsException(fn), "§10.2.4: a module worker's evaluation reaction could not be allocated");
        r = JS_PerformPromiseThen(wctx, evaluation, fn, fn);
        JS_FreeValue(wctx, fn);
        JS_FreeValue(wctx, r);
        return;
    }
    worker_enable_ports(wctx);
}

static void worker_enable_ports(JSContext *wctx)
{
    JSValue ports = realm_value_get(wctx, g_ports_slot);
    JSValue outside = JS_GetPropertyUint32(wctx, ports, 0), inside = JS_GetPropertyUint32(wctx, ports, 1);

    DCHECK(message_port_is(outside) && message_port_is(inside),
           "a worker's ports are not entangled — the constructor entangles them before any program is installed");
    message_port_enable(wctx, outside);
    message_port_enable(wctx, inside);
    JS_FreeValue(wctx, outside);
    JS_FreeValue(wctx, inside);
    JS_FreeValue(wctx, ports);
}

/* ---- the constructor -------------------------------------------------------------------------------------- */

typedef struct { uint8_t unused; } WorkerCtorState;
static void worker_ctor_visit(JSContext *ctx, void *st, JSStepVisit *v) { (void)ctx; (void)st; (void)v; }

#define WORKER_CTOR_STAGES(X) \
    X(WCTOR_BUILD = IDL_STEP_FIRST, \
      "HTML §10.2.6.3 new Worker(scriptURL, options) steps 1-9, and §10.2.4 run a worker up to its fetch")
enum { WORKER_CTOR_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const WORKER_CTOR_STEPS[] = { WORKER_CTOR_STAGES(JS_STEP_STAGE_LABEL) NULL };

/* `ctx` is the outside settings' realm and `url` the worker URL. Answers the new realm with its agent added to the
   running flow. */
static JSContext *worker_realm_new(JSContext *ctx, const char *url)
{
    JSContext *wctx;
    JSAgentRecord *agent;

    DCHECK(flow_running() != NULL, "a Worker was constructed outside a running flow, which its agent joins");
    wctx = JS_NewContext(JS_GetRuntime(ctx));
    CHECK(wctx != NULL, "§10.2.4: a worker's realm could not be created");
    CHECK(JS_AddIntrinsicDOMException(wctx) == 0, "the DOMException intrinsic failed to install in a worker realm");
    /* Step 4: obtain a dedicated worker agent, given canBlock true (HTML §8.1.2.2).
       Named residual. Not covered: the agent's end; its Agent Record and per-agent slots live until the runtime is
       freed, while the realm goes with the last flow holding it. Next diff builds: the record released with the
       last realm naming it, its slots given back then. Absence shows as: a runtime's agent count grows with every
       worker construction a flow replays or abandons. */
    agent = platform_create_agent(wctx, true);
    /* Step 5: a new realm whose global is a DedicatedWorkerGlobalScope; HTML §10.2.6.2's top-level creation URL is
       null, and §8.1.3.5 step 1.2.1 reads the owner's secure-context answer. */
    realm_install_intrinsics(wctx, agent, NULL, "DedicatedWorkerGlobalScope", secure_context_is(ctx));
    /* "Append owner to worker global scope's owner set": the scheduler files the worker's work under its owner's
       document. */
    document_set_worker_environment(wctx, ctx, url);
    flow_agent_add(flow_running(), agent, wctx);
    JS_FreeContext(wctx);   /* the flow's reference keeps it */
    return wctx;
}

static int worker_ctor_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                            JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    JSValue compliant, obj, type, cred, ports, deliver;
    const char *s;
    UrlRecord rec;
    WorkerData *w;
    bool parsed, module;
    FetchCredentialsMode credentials;

    (void)st; (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);
    DCHECK(hdr->stage == WCTOR_BUILD, "the Worker constructor resumed at a stage §10.2.6.3 does not have");
    if (JS_IsUndefined(hdr->this_val))
        return JS_ThrowTypeError(ctx, "constructor Worker requires 'new'"), -1;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "Worker requires a script URL"), -1;
    if (concolic_is(argv[0]))
        DFAIL("new Worker() was given an unknown script URL — the worker's program is whatever that address "
              "serves, so run a worker needs the address forked over the values the page's own code allows; "
              "build the fork at this constructor");
    /* Step 1: get trusted type compliant string with TrustedScriptURL, "Worker constructor". */
    compliant = trusted_types_compliant_string(ctx, TRUSTED_TYPE_SCRIPT_URL, argv[0], "Worker constructor");
    if (JS_IsException(compliant)) return -1;
    /* Steps 3-4: the worker URL, parsed relative to the outside settings; failure is a "SyntaxError". */
    s = JS_ToCString(ctx, compliant);
    JS_FreeValue(ctx, compliant);
    if (!s) return -1;
    url_record_init(&rec);
    parsed = fetch_parse_url(ctx, &rec, s, strlen(s));
    JS_FreeCString(ctx, s);
    if (!parsed) {
        url_record_free(&rec);
        return JS_ThrowDOMException(ctx, "SyntaxError", "the worker script URL could not be parsed"), -1;
    }
    /* WorkerOptions' `type` and `credentials`, enumerations the declaration has already converted: a module
       worker fetches a module worker script graph with the options' credentials mode. */
    type = idl_dict_get(ctx, argv[1], "type");
    cred = idl_dict_get(ctx, argv[1], "credentials");
    {
        const char *t = JS_ToCString(ctx, type), *c = JS_ToCString(ctx, cred);

        CHECK(t && c, "worker: OOM reading WorkerOptions");
        module = !strcmp(t, "module");
        credentials = !strcmp(c, "omit") ? FETCH_CREDENTIALS_OMIT
                    : !strcmp(c, "include") ? FETCH_CREDENTIALS_INCLUDE : FETCH_CREDENTIALS_SAME_ORIGIN;
        JS_FreeCString(ctx, t);
        JS_FreeCString(ctx, c);
        JS_FreeValue(ctx, type);
        JS_FreeValue(ctx, cred);
    }
    /* Step 5: the Worker object. */
    {
        JSValue proto = JS_GetClassProto(ctx, g_worker_class);

        DCHECK(!JS_IsNull(proto), "a Worker was constructed in a realm that never ran its install");
        obj = JS_NewObjectProtoClass(ctx, proto, g_worker_class);
        JS_FreeValue(ctx, proto);
    }
    if (JS_IsException(obj)) { url_record_free(&rec); return -1; }
    w = calloc(1, sizeof *w);
    CHECK(w != NULL, "worker: OOM building a Worker");
    w->url = url_serialize(&rec, /*exclude_fragment*/ false);
    url_record_free(&rec);
    CHECK(w->url != NULL, "worker: OOM serializing the worker URL");
    /* Steps 6-8: the outside port in the outside settings' realm, retargeted at the worker, and associated. */
    w->outside_port = message_port_new_in(ctx);
    CHECK(!JS_IsException(w->outside_port), "§10.2.6.3: a Worker's outside port could not be created");
    w->owner = ctx;
    JS_SetOpaque(obj, w);
    message_port_set_message_event_target(ctx, w->outside_port, obj);
    /* Step 9: run a worker. */
    w->realm = worker_realm_new(ctx, w->url);
    ports = JS_NewArray(w->realm);
    CHECK(!JS_IsException(ports), "§10.2.4: a worker's port pair could not be held");
    JS_SetPropertyUint32(w->realm, ports, 0, JS_DupValue(ctx, w->outside_port));
    {
        /* onComplete steps 3-6: the inside port in the worker's realm, retargeted at the worker global scope,
           set as its inside port, and entangled with the outside port. They run here, with the realm, rather
           than after the fetch: the message port post message steps drop a message whose targetPort is null
           ("If targetPort is null ... then return"), so a port entangled only after the fetch would lose
           everything the page posts in the meantime, which browsers deliver. The inside port's queue stays
           disabled until the script has run, so nothing is observably earlier. */
        JSValue inside = message_port_new_in(w->realm), wglobal = JS_GetGlobalObject(w->realm);

        CHECK(!JS_IsException(inside), "§10.2.4: a worker's inside port could not be created");
        message_port_set_message_event_target(w->realm, inside, wglobal);
        JS_FreeValue(w->realm, wglobal);
        worker_global_scope_set_inside_port(w->realm, inside);
        message_port_entangle(ctx, w->outside_port, inside);
        JS_SetPropertyUint32(w->realm, ports, 1, inside);
    }
    JS_SetPropertyUint32(w->realm, ports, 2, JS_DupValue(ctx, obj));
    realm_value_set(w->realm, g_ports_slot, ports);
    /* Step 13. "Fetch a classic worker script": "mode is same-origin, credentials mode is same-origin, parser
       metadata is not parser-inserted". "Fetch a module worker script graph" fetches its root as a single module
       script with mode "cors" and the options' credentials mode (HTML §8.1.4.2's fetch a worklet/module worker
       script graph). */
    {
        FetchRequest req = {
            .method = "GET", .url = w->url, .destination = "worker",
            .headers = NULL, .body = NULL, .body_len = 0,
            .metadata = csp_request_metadata("", 0, "", 0, CSP_PARSER_METADATA_NOT_PARSER_INSERTED),
            .credentials = module ? credentials : FETCH_CREDENTIALS_SAME_ORIGIN, .initiator = NULL,
            .mode = module ? FETCH_MODE_CORS : FETCH_MODE_SAME_ORIGIN,
            .referrer = "client", .referrer_policy = "", .address = FETCH_ADDRESS_CONCRETE,
        };

        deliver = JS_NewCFunctionData(ctx, worker_script_deliver, 1, module ? 1 : 0, 1, (JSValueConst *)&obj);
        CHECK(!JS_IsException(deliver), "§10.2.4: a worker script's completion steps could not be allocated");
        engine_pending_resource_url(ctx, deliver, &req);
        JS_FreeValue(ctx, deliver);
    }
    *presult = obj;
    return 0;
}

static const IdlStepDecl worker_ctor_decl = {
    worker_ctor_step, sizeof(WorkerCtorState), worker_ctor_visit, NULL,
    "HTML §10.2.6.3 new Worker(scriptURL, options)", WORKER_CTOR_STEPS
};

/* §10.2.6.3's postMessage(message, transfer) and postMessage(message, options): "run the message port post
   message steps providing this's outside port, message and options". */
static JSValue js_worker_post(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    WorkerData *w = worker_of(this_val);

    (void)magic;
    if (!w) return JS_ThrowTypeError(ctx, "Illegal invocation");
    return message_port_post_message(ctx, w->outside_port, argc, argv);
}

/* ---- install ---------------------------------------------------------------------------------------------- */

static void worker_install_realm(JSContext *ctx)
{
    JSValue proto, prev, global, ctor;

    /* Web IDL §3.8's population is every interface exposed in the realm; `Worker` is
       [Exposed=(Window,DedicatedWorker,SharedWorker)]. */
    if (!idl_exposed_in_realm(ctx, "Worker")) return;
    prev = JS_GetClassProto(ctx, g_worker_class);
    DCHECK(JS_IsNull(prev), "worker_install_realm ran twice in one realm");
    JS_FreeValue(ctx, prev);
    /* `interface Worker : EventTarget`, including AbstractWorker (`onerror`) and MessageEventTarget (`onmessage`,
       `onmessageerror`). */
    proto = event_target_derived_proto(ctx);
    idl_interface_tag(ctx, proto, "Worker");
    event_target_install_handlers(ctx, proto, EH_PORT | EH_ABSTRACT_WORKER);
    idl_install_method(ctx, proto, "postMessage", g_id_post);
    ctor = idl_step_constructor(ctx, "Worker", g_ctor_stepid);
    CHECK(!JS_IsException(ctor), "the Worker interface object could not be allocated");
    JS_SetConstructor(ctx, ctor, proto);
    global = JS_GetGlobalObject(ctx);
    idl_define_global_property_reference(ctx, global, "Worker", ctor);
    JS_FreeValue(ctx, global);
    JS_SetClassProto(ctx, g_worker_class, proto);
}

void worker_init(JSContext *ctx)
{
    JSClassDef d = { "Worker", .finalizer = worker_finalizer, .gc_mark = worker_gc_mark };
    JSRuntime *rt = JS_GetRuntime(ctx);
    static const IdlArgType CTOR_ARGS[2] = { IDL_USVSTRING, IDL_DICT };
    static const char *const TYPE_VALUES[] = { "classic", "module", NULL };
    static const char *const CREDENTIALS_VALUES[] = { "omit", "same-origin", "include", NULL };
    /* `dictionary WorkerOptions { WorkerType type = "classic"; RequestCredentials credentials = "same-origin";
       DOMString name = ""; }`, in Web IDL §3.2.17's lexicographic read order. */
    static const IdlDictMember OPTIONS[] = {
        { "credentials", IDL_ENUM, false, CREDENTIALS_VALUES, 0, NULL, IDL_DEFAULT_STRING, "same-origin" },
        { "name", IDL_DOMSTRING, false, NULL, 0, NULL, IDL_DEFAULT_STRING, "" },
        { "type", IDL_ENUM, false, TYPE_VALUES, 0, NULL, IDL_DEFAULT_STRING, "classic" },
    };
    static const IdlArgType POST_ARGS[2] = { IDL_ANY, IDL_SEQUENCE_OBJECT_OR_DICT };
    static const IdlDictMember POST_OPTS[] = { { "transfer", IDL_SEQUENCE_OBJECT, false, NULL, 0 } };

    DCHECK(g_worker_rt == NULL, "worker_init ran twice in one runtime");
    g_worker_rt = rt;
    JS_NewClassID(rt, &g_worker_class);
    CHECK(JS_NewClass(rt, g_worker_class, &d) == 0, "the Worker class could not be registered");
    g_ctor_stepid = idl_method_id_step(ctx, CTOR_ARGS, 2, OPTIONS, (int)(sizeof OPTIONS / sizeof OPTIONS[0]),
                                       &worker_ctor_decl, 0);
    idl_optional_from(1);
    g_id_post = idl_method_id_dict(ctx, POST_ARGS, 2, POST_OPTS, 1, js_worker_post, 0);
    idl_optional_from(1);
    g_error_task_stepid = JS_RegisterStepDef(rt, &worker_error_def);
    g_ports_slot = realm_value_declare(ctx, "HTML §10.2.4 run a worker's outside and inside ports, in the worker realm");
    engine_set_agent_program_done_hook(worker_program_done);
    agent_state_ptr("worker", &g_worker_rt, "the runtime §10.2.6.3's Worker was declared in, and the latch");
    agent_state_class("worker", &g_worker_class, "HTML §10.2.6.3's Worker class");
    agent_state_id("worker", &g_ctor_stepid, "§10.2.6.3's constructor machine");
    agent_state_id("worker", &g_id_post, "§10.2.6.3's postMessage pool entry");
    agent_state_id("worker", &g_error_task_stepid, "§10.2.4's error task machine");
    agent_state_realm_slot("worker", &g_ports_slot, "§10.2.4's port pair of a worker realm");
    realm_declare_intrinsic(worker_install_realm);
}

void worker_free(JSRuntime *rt)
{
    DCHECK(g_worker_rt == rt, "§10.2.6.3's Worker was released against a runtime it was not declared in");
    engine_set_agent_program_done_hook(NULL);
    agent_state_undo("worker");
}
