/* A REFERENCE TO AN OBJECT IN ANOTHER AGENT — and the one place this engine deviates from HTML's agent model
 * on purpose, so it is worth stating exactly what the deviation is.
 *
 * HTML puts same-origin documents in ONE AGENT CLUSTER, and a browser puts an agent cluster in one heap. That
 * is why `iframe.contentDocument.body` is a POINTER in Chrome and why the spec can write it as a plain
 * property read: the two documents share a heap, so there is nothing to cross. Blink only grows a remote form
 * — RemoteDOMWindow, RemoteFrame — at the CROSS-ORIGIN boundary, and at that boundary the only objects that
 * ever cross are WindowProxy and Location, because cross-origin access exposes nothing else.
 *
 * SECURITY.md puts every DOCUMENT in its own instance, same-origin included, and CLAUDE.md forbids
 * co-locating same-origin documents to avoid the transport. So this engine has same-origin documents in
 * different agents, which HTML does not, and the consequence lands exactly here: a same-origin cross-document
 * read returns an OBJECT, and an object cannot cross as bytes. What crosses is its NAME.
 *
 * WHY IT IS A PROXY. The read has to SUSPEND — the object is in another instance and the answer is not
 * available in this turn — and an exotic [[Get]] hook cannot: it is a C function that must return a value.
 * There is exactly one read shape this interpreter resolves at the OPERATOR SITE and drives on the trampoline,
 * and it is a Proxy trap (see the C-stack rules: every CALL/READ/WRITE/MEMBERSHIP/DELETE through a Proxy is
 * re-dispatched there). So a reference is a Proxy whose traps are STEP MACHINES, and `remote.title` parks the
 * flow at its own call site exactly as an await does.
 *
 * IDENTITY HOLDS ON BOTH SIDES, and neither half is optional. The exporting agent mints ONE id per object, so
 * two asks for the same object answer with the same name; the importing agent keeps ONE reference per (doc,
 * id), so two answers with the same name are the same object here. A page writes `w.document === w.document`
 * and both halves are what makes it true.
 *
 * WHAT IS NOT BUILT YET is written as a crash rather than a silence: a WRITE through a reference, a CALL
 * through one, and passing a LOCAL object as an argument to either. Each is a real mechanism (the argument
 * case needs this agent to become an exporter mid-call), and each aborts naming itself. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/frame/remote_object.h"
#include "core/idl_args.h"
#include "solver/engine.h"
#include "solver/flow.h"
#include "solver/world.h"

/* ---- THIS AGENT AS AN EXPORTER --------------------------------------------------------------------------- */

typedef struct { JSValue v; } Export;
static Export  *g_exports;
static uint32_t g_exports_n, g_exports_cap;

uint32_t remote_object_export(JSContext *ctx, JSValueConst v)
{
    uint32_t i;

    DCHECK(JS_IsObject(v), "something that is not an object was exported to another agent — a primitive crosses "
                           "as its value and needs no name");
    /* ONE ID PER OBJECT. A linear scan, because the table is one entry per object a peer has actually asked
       for — the set of objects that cross an agent boundary, not the heap. */
    for (i = 0; i < g_exports_n; i++)
        if (JS_VALUE_GET_PTR(g_exports[i].v) == JS_VALUE_GET_PTR(v)) return i + 1;
    if (g_exports_n == g_exports_cap) {
        uint32_t cap = g_exports_cap ? g_exports_cap * 2 : 16;
        Export *g = realloc(g_exports, cap * sizeof *g);
        CHECK(g != NULL, "remote object: OOM exporting an object — a dropped export is a reference the peer "
                         "holds that names nothing, and every read through it would fail");
        g_exports = g;
        g_exports_cap = cap;
    }
    /* HELD, not borrowed: the peer's reference outlives whatever else in this agent points at the object, and
       an export the collector took away would leave that reference naming freed memory. */
    g_exports[g_exports_n].v = JS_DupValue(ctx, v);
    return ++g_exports_n;
}

JSValueConst remote_object_by_id(uint32_t id)
{
    if (id == 0 || id > g_exports_n) return JS_UNDEFINED;
    return g_exports[id - 1].v;
}

/* ---- THIS AGENT AS AN IMPORTER --------------------------------------------------------------------------- */

/* The (doc, id) a reference names, kept on the PROXY'S TARGET. Not on the handler, and the difference is not
   stylistic: a trap is passed (target, property, receiver) and the target is argv[0] every time, whereas what
   `this` is inside a trap depends on how the interpreter dispatched it. Reading the identity off an argument
   the contract guarantees is the only version that cannot be wrong. */
typedef struct { uint32_t doc, id; } RefData;
static JSClassID g_ref_class;
static int       g_get_stepid = -1;
static JSValue   g_refs = JS_UNDEFINED;   /* "<doc>:<id>" -> the one reference for it (owned) */

bool remote_object_is(JSValueConst v)
{
    JSValue h;
    bool is;
    if (!JS_IsProxy(v)) return false;
    h = JS_GetProxyTarget(NULL, v);
    is = JS_GetOpaque(h, g_ref_class) != NULL;
    return is;
}

static RefData *ref_of(JSContext *ctx, JSValueConst proxy)
{
    JSValue t = JS_GetProxyTarget(ctx, proxy);
    RefData *r = JS_GetOpaque(t, g_ref_class);
    JS_FreeValue(ctx, t);
    return r;
}

uint32_t remote_object_doc(JSValueConst v)
{
    RefData *r = ref_of(NULL, v);
    DCHECK(r != NULL, "the document of something that is not a cross-agent reference was asked for");
    return r->doc;
}

uint32_t remote_object_id(JSValueConst v)
{
    RefData *r = ref_of(NULL, v);
    DCHECK(r != NULL, "the id of something that is not a cross-agent reference was asked for");
    return r->id;
}

/* WHERE THIS MACHINE RESTS. A same-origin-across-instances property read is ONE operation — §7.2.5's
   ordinary [[Get]], performed by the peer that owns the object — and the wait for that peer is a sub-sequence
   inside it rather than a step of its own. */
#define REF_GET_STAGES(X) \
    X(REF_GET_ASK = IDL_STEP_FIRST, \
      "ECMA-262 10.1.8 [[Get]], performed in the instance that owns the object (the flow suspends on the read " \
      "and resumes with what the peer's own program answered)")
enum { REF_GET_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const REF_GET_STEPS[] = { REF_GET_STAGES(JS_STEP_STAGE_LABEL) NULL };

/* §7.2.5.1's read, one agent further out: `object.get <doc> <world+ancestry> <id> <name>`. The world travels
   for the same reason it travels with a WindowProxy read — the answer is only true in it — and so does its
   ancestry, so the peer can materialize its segment by forking the nearest ancestor it holds. */
typedef struct { uint32_t req; } RefGetState;

static void ref_get_visit(JSContext *ctx, void *st, JSStepVisit *v) { (void)ctx; (void)st; (void)v; }

static int ref_get_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                        JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    RefGetState *s = st;
    JSValueConst answer;

    (void)hdr; (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);

    if (s->req == 0) {
        /* The trap is `get(target, property, receiver)`. */
        RefData *r;
        Flow *f = flow_running();
        WorldId anc[16];
        const char *name;
        char *op;
        int n_anc, k, n = 0;
        size_t cap;

        DCHECK(argc >= 2, "a cross-agent get trap ran with no property name — the Proxy machinery passes "
                          "(target, property, receiver) and this is the second");
        DCHECK(f != NULL, "a cross-agent read was issued outside a flow — there would be nothing to suspend");
        /* A SYMBOL HAS NO CROSS-AGENT NAME. It is not "absent" either — a symbol is unique to the agent that
           minted it, so the peer could not be asked about it even in principle. undefined is the answer for a
           property the peer does not have, and it is also the honest answer here. */
        if (!JS_IsString(argv[1])) { *presult = JS_UNDEFINED; return JS_STEP_DONE; }
        /* THE TARGET IS THE IDENTITY, and it is argv[0] by the trap's own contract. */
        r = JS_GetOpaque(argv[0], g_ref_class);
        DCHECK(r != NULL, "a cross-agent get trap ran with a target that is not a reference — the trap and the "
                          "target it was built with have been separated");
        if (!r) { *presult = JS_UNDEFINED; return JS_STEP_DONE; }
        name = JS_ToCString(ctx, argv[1]);
        if (!name) return JS_STEP_ABRUPT;
        n_anc = world_ancestry(f->world, anc, (int)(sizeof anc / sizeof anc[0]));
        cap = strlen(name) + 128 + (size_t)(n_anc + 1) * 64;
        op = malloc(cap);
        CHECK(op != NULL, "remote object: OOM building a cross-agent read");
        n = snprintf(op, cap, "object.get\t%s\t%s:%u",
                     world_doc_name(r->doc), world_doc_name(f->world.doc), f->world.serial);
        for (k = 0; k < n_anc && n < (int)cap; k++)
            n += snprintf(op + n, cap - (size_t)n, ",%s:%u", world_doc_name(anc[k].doc), anc[k].serial);
        snprintf(op + n, cap - (size_t)n, "\t%u\t%s", r->id, name);
        JS_FreeCString(ctx, name);
        s->req = engine_host_request(ctx, op);
        free(op);
        return JS_STEP_YIELD;   /* park; siblings run until the peer answers */
    }
    if (!engine_host_answered(s->req, &answer))
        return JS_STEP_YIELD;
    *presult = engine_host_take(ctx, s->req);
    s->req = 0;
    return JS_STEP_DONE;
}

static const IdlStepDecl REF_GET_DECL = { ref_get_step, sizeof(RefGetState), ref_get_visit, NULL,
                                         "ECMA-262 10.1.8 [[Get]] across an instance boundary",
                                         REF_GET_STEPS };

/* Everything a reference does NOT do yet, said at the site rather than left to answer wrongly. A write, a call
   and a delete each need the peer to run the operation under the asking flow's world and to accept ARGUMENTS,
   and an argument that is a local object needs this agent to export it mid-call — a real mechanism, not a
   variant of the read. */
static JSValue ref_unbuilt(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    switch (magic) {
    case 0: DFAIL("a WRITE through a cross-agent reference — build the peer-side set: it runs under the asking "
                  "flow's world and takes a VALUE, so a local object argument has to be exported first"); break;
    case 1: DFAIL("a CALL through a cross-agent reference — build the peer-side apply: same world, and its "
                  "arguments are the same export problem the write has"); break;
    default: DFAIL("a DELETE through a cross-agent reference — build the peer-side deleteProperty"); break;
    }
    return JS_UNDEFINED;
}

JSValue remote_object_ref(JSContext *ctx, uint32_t doc, uint32_t id)
{
    char key[64];
    JSValue found, handler, target, proxy;
    RefData *r;

    DCHECK(g_ref_class != 0, "a cross-agent reference was minted before remote_object_init ran");
    DCHECK(doc != 0 && doc != world_local_doc(),
           "a cross-agent reference was minted for THIS agent's own document — an object in this heap is "
           "reached directly, and routing it would suspend a flow on a question this agent is the answer to");
    DCHECK(id != 0, "a cross-agent reference was minted with no id — it would name nothing in the peer");

    /* ONE REFERENCE PER (doc, id): the other half of identity. The exporter mints one id per object; without
       this the same id would arrive twice as two objects and `w.document === w.document` would be false on
       this side instead of the other. */
    snprintf(key, sizeof key, "%u:%u", doc, id);
    found = JS_GetPropertyStr(ctx, g_refs, key);
    if (JS_IsObject(found)) return found;
    JS_FreeValue(ctx, found);

    handler = JS_NewObject(ctx);
    CHECK(!JS_IsException(handler), "a cross-agent reference's handler could not be allocated");
    JS_SetPropertyStr(ctx, handler, "get", idl_step_function(ctx, "get", 3, g_get_stepid));
    JS_SetPropertyStr(ctx, handler, "set",
                      JS_NewCFunction2(ctx, (JSCFunction *)ref_unbuilt, "set", 4, JS_CFUNC_generic_magic, 0));
    JS_SetPropertyStr(ctx, handler, "apply",
                      JS_NewCFunction2(ctx, (JSCFunction *)ref_unbuilt, "apply", 3, JS_CFUNC_generic_magic, 1));
    JS_SetPropertyStr(ctx, handler, "deleteProperty",
                      JS_NewCFunction2(ctx, (JSCFunction *)ref_unbuilt, "deleteProperty", 2,
                                       JS_CFUNC_generic_magic, 2));

    /* AN EMPTY EXTENSIBLE TARGET, which is what makes the reference fully virtual: a Proxy's invariants are
       stated against its target, and an extensible target with no own properties constrains no `get`. A target
       carrying a property would make the peer's answers checkable against this agent's guesses. It carries the
       (doc, id) as an internal slot instead, which is not a property and constrains nothing. */
    target = JS_NewObjectClass(ctx, g_ref_class);
    CHECK(!JS_IsException(target), "a cross-agent reference's target could not be allocated");
    r = calloc(1, sizeof *r);
    CHECK(r != NULL, "remote object: OOM building a cross-agent reference");
    r->doc = doc;
    r->id = id;
    JS_SetOpaque(target, r);
    proxy = JS_NewProxy(ctx, target, handler);
    JS_FreeValue(ctx, target);
    JS_FreeValue(ctx, handler);
    CHECK(!JS_IsException(proxy), "a cross-agent reference could not be allocated");
    JS_SetPropertyStr(ctx, g_refs, key, JS_DupValue(ctx, proxy));
    return proxy;
}

static void ref_finalizer(JSRuntime *rt, JSValue val)
{
    RefData *r = JS_GetOpaque(val, g_ref_class);
    (void)rt;
    free(r);
}

void remote_object_init(JSContext *ctx)
{
    JSClassDef d = { "CrossAgentReference", .finalizer = ref_finalizer };
    JSRuntime *rt = JS_GetRuntime(ctx);

    DCHECK(g_ref_class == 0, "remote_object_init ran twice — one instance is one agent");
    JS_NewClassID(rt, &g_ref_class);
    CHECK(JS_NewClass(rt, g_ref_class, &d) == 0, "the cross-agent reference class could not be registered");
    {
        /* THE TRAP'S THREE ARGUMENTS, declared so the machine PASSES them: a declaration of none converts none
           and hands the body none, and the property name is the second — the trap would then read a name it
           was never given. They are `any` because a trap's arguments are whatever the operator site had. */
        static const IdlArgType GET_ARGS[3] = { IDL_ANY, IDL_ANY, IDL_ANY };
        g_get_stepid = idl_method_id_step(ctx, GET_ARGS, 3, NULL, 0, &REF_GET_DECL, 0);
    }
    g_refs = JS_NewObject(ctx);
    CHECK(!JS_IsException(g_refs), "the cross-agent reference table could not be allocated");
}

void remote_object_free(JSContext *ctx)
{
    uint32_t i;
    for (i = 0; i < g_exports_n; i++) JS_FreeValue(ctx, g_exports[i].v);
    free(g_exports);
    g_exports = NULL;
    g_exports_n = g_exports_cap = 0;
    JS_FreeValue(ctx, g_refs);
    g_refs = JS_UNDEFINED;
    g_ref_class = 0;
    g_get_stepid = -1;
}
