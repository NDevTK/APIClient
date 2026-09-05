/* THE CloseWatcher INTERFACE — HTML §6.10.3. See close_watcher_interface.h for the published IDL, for why
 * this is a component of its own beside core/html/close_watcher.c, and for why two of its three operations are
 * step machines and the third is not.
 *
 * THE INTERNAL CLOSE WATCHER IS AN OWN SLOT UNDER A PRIVATE SYMBOL. "Each CloseWatcher instance has an
 * internal close watcher, which is a close watcher" — a spec struct, not a page-reachable object, so the link
 * to it is a slot this file mints a key for and never publishes. It is a slot rather than a C opaque for the
 * reason core/html/popover.c's per-element state is: a slot written as a property write is captured by the
 * heap COW delta, so a forked arm holds its own link and a parked flow resumes with the one it had — and a
 * `free`-able C record hanging off a class would need a finalizer and a gc_mark for a single reference the
 * engine already knows how to trace.
 *
 * THE BRAND IS THE CLASS AND NOT THE SLOT. Web IDL §3.7.7 Operations' brand check (and §3.7.6 Attributes' for
 * the two event-handler attributes) refuses a member invoked on anything
 * that does not implement the interface, and asking "does it have my slot" would answer for an object this
 * file happened to write one on rather than for a CloseWatcher — the two are the same today and are not the
 * same fact. The class is also what §3.7 needs anyway, because JS_SetClassProto is where this realm's
 * prototype lives.
 *
 * WHAT `new CloseWatcher()` MAKES REACHABLE, WHICH IS THE POINT OF LANDING IT. Before it, core/html's §6.10.2
 * manager had NO live establisher at all: §6.12 show popover's step 15 DFAILs before it establishes anything
 * and §4.11.4's modal `dialog` is not built, so every Window's groups was permanently empty and §6.10.2's
 * anti-abuse arithmetic — the allowance, the banking boolean, the join-the-last-group branch — had been
 * verified by reading and by two-sided DCHECKs and by nothing that ran. This constructor is its first caller,
 * and `requestClose()` and `close()` are the first callers of the three algorithms that run a watcher's
 * actions. */
#include <stdbool.h>
#include <stdint.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "core/dom/abort.h"
#include "core/dom/document.h"
#include "core/events/event_target.h"
#include "core/html/close_watcher.h"
#include "core/html/close_watcher_interface.h"
#include "solver/concolic.h"

static JSClassID g_cw_class;
static JSValue   g_slot_key = JS_UNDEFINED;   /* the internal-close-watcher slot's key — the AGENT's */
static JSAtom    g_slot_atom = JS_ATOM_NULL;
static int       g_id_ctor = -1, g_id_request_close = -1, g_id_close = -1, g_id_destroy = -1;

/* Web IDL §3.7.6 Attributes' and §3.7.7 Operations' BRAND CHECK, as the class every instance wears. */
static bool cwi_is(JSValueConst v)
{
    return JS_GetClassID(v) == g_cw_class;
}

/* "this's internal close watcher". OWNED: the caller frees. */
static JSValue cwi_internal(JSContext *ctx, JSValueConst self)
{
    JSValue w;

    DCHECK(g_slot_atom != JS_ATOM_NULL,
           "a CloseWatcher's internal close watcher was read before close_watcher_interface_declare minted "
           "its slot key");
    if (JS_GetOwnSlot(ctx, &w, self, g_slot_atom) <= 0)
        w = JS_UNDEFINED;
    DCHECK(JS_IsObject(w),
           "a CloseWatcher has no internal close watcher — §6.10.3's constructor step 4 sets it on every "
           "instance it returns and nothing ever clears it, so an instance without one was not built by that "
           "constructor");
    return w;
}

/* ---- §6.10.3's CONSTRUCTOR --------------------------------------------------------------------------------- */

/* `dictionary CloseWatcherOptions { AbortSignal signal; }` — ONE member, so Web IDL §3.2.17 Dictionary types'
   read order (inherited levels first, each level's own members sorted lexicographically among themselves) is
   satisfied by any table with one row in it. The member is neither `required` nor defaulted, which is what
   makes step 3's "If options["signal"] exists" a real question rather than a constant.
   NOT `const`, for core/html/popover.c's reason: a JSClassID is minted at agent start and a static
   initialiser cannot ask for one, so §3.2.15 Interface types' class is filled in at declare. */
/* THE §3.2.15 TAIL IS NAMED, and every declaration of this shape must name it: IdlDictMember has gained fields
   twice, so a POSITIONAL initializer that runs to the end of the struct silently re-aims every value after the
   next field added — and where two adjacent fields are both pointers, it re-aims them without a diagnostic.
   `iface` is written at the install below rather than here, because a class id is a RUNTIME registration. */
static IdlDictMember CW_OPTIONS[] = {
    { "signal", IDL_INTERFACE, false, NULL, 0, NULL, IDL_DEFAULT_NONE, NULL, .iface = 0, .iface_narrow = NULL }
};

/* §6.10.3's constructor step 3.2, "Add the following steps to options["signal"]: Destroy closeWatcher." — an
   ABORT ALGORITHM, which
   DOM §3.2 defines as engine work that runs BEFORE the `abort` event and is invisible to the page. It is a
   C-function closure over the watcher rather than a page-visible listener for exactly that reason: a listener
   would be one the page could see, remove, or have run out of order with its own.
   THE REALM IT RUNS IN IS THE ONE IT WAS MINTED IN, which is what makes the `wctx` it passes correct: a C
   function runs in the realm that DEFINED it, and this one is minted in the constructor's realm, which is
   §6.10.3's "this's relevant global object" — the very Window whose manager holds the watcher. */
static JSValue js_cw_signal_destroy(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                    int magic, JSValueConst *data)
{
    (void)this_val; (void)argc; (void)argv; (void)magic;
    DCHECK(JS_IsObject(data[0]), "§6.10.3's signal abort algorithm was minted with no close watcher to destroy");
    close_watcher_destroy(ctx, data[0]);
    return JS_UNDEFINED;
}

static JSValue js_cw_ctor(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValueConst opts = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSValue obj, watcher, sig, proto;

    (void)magic;
    if (JS_IsUndefined(this_val))
        return JS_ThrowTypeError(ctx, "constructor CloseWatcher requires 'new'");
    /* Step 1 — "If this's relevant global object's associated Document is not fully active, then throw an
       "InvalidStateError" DOMException." It is ALSO establish a close watcher's step 1 assert, which is why
       that assert can be an assert: this is the guard that makes it unreachable from here. */
    if (!document_fully_active(ctx))
        return JS_ThrowDOMException(ctx, "InvalidStateError",
                                    "a CloseWatcher cannot be constructed in a Document that is not fully "
                                    "active");
    /* The instance exists BEFORE the watcher, because the watcher's three algorithms are stated over it: both
       of §6.10.3's actions fire "at this", so the subject establish is given must be this object. */
    proto = JS_GetClassProto(ctx, g_cw_class);
    DCHECK(!JS_IsNull(proto), "a CloseWatcher was constructed in a realm that never ran its install");
    obj = JS_NewObjectProtoClass(ctx, proto, g_cw_class);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj))
        return obj;
    /* Step 2 — establish a close watcher given this's relevant global object, with the three algorithms
       close_watcher.h's CLOSE_WATCHER_KIND_CLOSE_WATCHER names. The kind IS the "with:" block: see that
       header for why three algorithms travel as an id and not as three closures. */
    watcher = close_watcher_establish(ctx, CLOSE_WATCHER_KIND_CLOSE_WATCHER, obj);
    /* Step 3 — "If options["signal"] exists". An absent member and an explicit `undefined` are the same thing
       here, because §3.2.17 (ES-to-IDL list) step 4.1.4 places a member only "if jsMemberValue is not
       undefined" and this one declares no default to be placed instead. */
    sig = idl_dict_get(ctx, opts, "signal");
    if (!JS_IsUndefined(sig)) {
        /* THE THIRD SHAPE IS THE ONE THIS ASSERT IS FOR. A declared `AbortSignal` position admits exactly two
           values — §3.2.15's brand having passed, or unknown external input, which §3.2.17's member loop
           crosses AS ITSELF so that opacity survives the boundary. The `if` below is therefore not a
           defensive test: it is the second of those two, and what this build owes it is a FORK (the world in
           which the unknown is an aborted signal, and the world in which it is not), which is what the abort
           names rather than leaving the registration silently skipped. */
        IDL_DCHECK_MEMBER(abort_signal_is(ctx, sig), sig, "signal", "`AbortSignal signal`");
        if (abort_signal_is(ctx, sig)) {
            /* Step 3.1 — a signal that has ALREADY fired destroys the watcher now, because §3.2's add-an-
               algorithm is a no-op on an aborted signal and would otherwise drop the destroy entirely. */
            if (abort_signal_aborted(ctx, sig))
                close_watcher_destroy(ctx, watcher);
            else {
                JSValueConst data[1];
                JSValue algo;

                data[0] = watcher;
                algo = JS_NewCFunctionData(ctx, js_cw_signal_destroy, 0, 0, 1, data);   /* step 3.2 */
                CHECK(!JS_IsException(algo),
                      "close watcher: the abort algorithm for a signal-scoped CloseWatcher could not be "
                      "allocated — a dropped one is a watcher that outlives the signal that owns it");
                abort_signal_add_algorithm(ctx, sig, algo);   /* BORROWED: the signal takes its own reference */
                JS_FreeValue(ctx, algo);
            }
        }
    }
    JS_FreeValue(ctx, sig);
    /* Step 4 — CONFIGURABLE AND WRITABLE, for core/html/popover.c's reason: a slot defined with no flags makes
       a later write a silent no-op, and a slot that is only ever written once still has to be a slot the
       engine can replace rather than one it cannot. */
    JS_DefinePropertyValue(ctx, obj, g_slot_atom, watcher, JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE);
    return obj;
}

/* ---- §6.10.3's requestClose() AND close(), AS ONE MACHINE ---------------------------------------------------
 *
 * ONE machine with a magic, for core/html/popover.c's reason: the two members differ in WHICH of §6.10.2's
 * algorithms they delegate to and in nothing else, and a second implementation of the delegation is a second
 * place for the receiver resolution and the brand check to disagree. */

enum { M_REQUEST_CLOSE = 0, M_CLOSE };

#define CWI_STAGES(X) \
    X(CWI_ENTER, "HTML §6.10.3 The CloseWatcher interface's requestClose()/close() prologue: Web IDL §3.7.7 " \
                 "Operations' brand check and the resolution of this's internal close watcher — two O(1) " \
                 "engine actions, " \
                 "a class-id comparison and one own-slot read") \
    X(CWI_RUN,   "HTML §6.10.3 The CloseWatcher interface's requestClose() method steps (\"to request to " \
                 "close this's internal close watcher with false\") or close() method steps (\"to close " \
                 "this's internal close watcher\") — one stage for either, because the delegation is a single " \
                 "call and every rest point inside it belongs to HTML §6.10.2 Close watcher infrastructure's " \
                 "own request, which parks and resumes at its own call site with this stage unmoved")
enum { IDL_STEP_STAGE_BASE(CWI_STAGES) CWI_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const CWI_STEPS[] = { CWI_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    JSValue         watcher;   /* this's internal close watcher, held across the suspension (owned) */
    CloseWatcherRun run;       /* §6.10.2's request, whose cursors and event this machine holds */
} CwiState;

static void cwi_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    CwiState *s = st;

    v->val(ctx, &s->watcher);
    close_watcher_run_visit(ctx, &s->run, v);
}

/* An ABANDONED run gives back request to close's step 7 flag, which its step 9 would otherwise have cleared:
   a flow dropped inside the page's own `cancel` handler must not leave this watcher refusing every later
   request at step 3. It READS the run's own owned field, so it must run before the declaration is discharged,
   which is exactly what this hook is. */
static void cwi_release(JSContext *ctx, void *st)
{
    CwiState *s = st;

    close_watcher_run_unlock(ctx, &s->run);
}

static int cwi_body(JSContext *ctx, JSStepHdr *hdr, void *state, int argc, JSValueConst *argv,
                    JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    CwiState *s = state;
    int magic = idl_step_magic(hdr);
    bool proceed = true;
    int rc;

    (void)argc; (void)argv;
    if (hdr->stage == CWI_ENTER) {
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        /* EVERY owned field before the first thing that can fail — the teardown discharges exactly what the
           declaration names, so a field handed over late is a field nothing releases. */
        s->watcher = JS_UNDEFINED;
        close_watcher_run_init(&s->run);
        if (!cwi_is(hdr->this_val)) {
            JS_ThrowTypeError(ctx, "requestClose()/close() was called on something that is not a CloseWatcher");
            return JS_STEP_ABRUPT;
        }
        s->watcher = cwi_internal(ctx, hdr->this_val);
        STEP_GOTO(hdr->stage, CWI_RUN, NULL);
    }
    DCHECK(hdr->stage == CWI_RUN, "a §6.10.3 member resumed at a stage this machine does not have");
    if (magic == M_REQUEST_CLOSE) {
        /* "The requestClose() method steps are to request to close this's internal close watcher with false."
           FALSE is the whole of the difference from the close request path: §6.10.2's step 6 then makes
           canPreventClose true without asking §6.4.1 anything, so a page's own `requestClose()` always gives
           its `cancel` handler a cancelable event. The boolean the algorithm returns is DISCARDED — the member
           returns `undefined`, and the page learns what happened from its two handlers. */
        rc = close_watcher_request_to_close_run(ctx, hdr, &s->run, s->watcher, /*requireHistory*/ false,
                                                cb_result, &proceed, out_cb, out_argc);
    } else {
        DCHECK(magic == M_CLOSE, "a §6.10.3 member ran with a magic this file has no member for");
        /* "The close() method steps are to close this's internal close watcher." */
        rc = close_watcher_close_run(ctx, &s->run, s->watcher, cb_result, out_cb, out_argc);
    }
    if (rc > 0) return rc;
    if (rc < 0) return JS_STEP_ABRUPT;
    *presult = JS_UNDEFINED;
    return JS_STEP_DONE;
}

static const IdlStepDecl CWI_DECL = {
    cwi_body, sizeof(CwiState), cwi_visit, cwi_release,
    "HTML §6.10.3 The CloseWatcher interface's requestClose() and close()", CWI_STEPS
};

/* ---- §6.10.3's destroy() ------------------------------------------------------------------------------------ */

/* "The destroy() method steps are to destroy this's internal close watcher." Destroy a close watcher only
   reads and writes the manager — no action of the watcher's is run — so this is a plain body and not a
   machine. That is §6.10.2's own line between its two halves, arriving at the IDL surface. */
static JSValue js_cw_destroy(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValue watcher;

    (void)argc; (void)argv; (void)magic;
    if (!cwi_is(this_val))
        return JS_ThrowTypeError(ctx, "destroy() was called on something that is not a CloseWatcher");
    watcher = cwi_internal(ctx, this_val);
    close_watcher_destroy(ctx, watcher);
    JS_FreeValue(ctx, watcher);
    return JS_UNDEFINED;
}

/* ---- the declaration and the per-realm install --------------------------------------------------------------- */

/* §3.7.1's INTERFACE OBJECT and §3.7.3's INTERFACE PROTOTYPE OBJECT, FOR ONE REALM. Per realm and not per
   agent, because §3.7 gives each realm its own AND because a C member runs in the realm that DEFINED it: one
   shared `close` would reach §6.10.2 with whichever document's realm built it first, and every manager it
   edited would be that document's. */
static void close_watcher_interface_install_realm(JSContext *ctx)
{
    JSValue proto, prev, ctor, global;

    DCHECK(g_id_ctor >= 0, "a realm asked for CloseWatcher before close_watcher_interface_declare declared it");
    prev = JS_GetClassProto(ctx, g_cw_class);
    DCHECK(JS_IsNull(prev), "close_watcher_interface_install_realm ran twice in one realm");
    JS_FreeValue(ctx, prev);

    /* `interface CloseWatcher : EventTarget` — CREATED over §2.7's prototype rather than re-parented, which is
       Web IDL §3.7.3's own order and the reason event_target_derived_proto exists. */
    proto = event_target_derived_proto(ctx);
    CHECK(!JS_IsException(proto), "CloseWatcher.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "CloseWatcher");
    /* `attribute EventHandler oncancel; attribute EventHandler onclose;` — declared ON THIS INTERFACE, which is
       why they arrive by a bit of their own rather than by any mixin's. Both names belong to
       GlobalEventHandlers as well, so each is one more bit on an entry that already exists. */
    event_target_install_handlers(ctx, proto, EH_CLOSE_WATCHER);
    idl_install_method(ctx, proto, "requestClose", g_id_request_close);
    idl_install_method(ctx, proto, "close", g_id_close);
    idl_install_method(ctx, proto, "destroy", g_id_destroy);
    JS_SetClassProto(ctx, g_cw_class, JS_DupValue(ctx, proto));

    ctor = idl_step_constructor(ctx, "CloseWatcher", g_id_ctor);
    CHECK(!JS_IsException(ctor), "the CloseWatcher interface object could not be allocated");
    JS_SetConstructor(ctx, ctor, proto);
    JS_FreeValue(ctx, proto);
    global = JS_GetGlobalObject(ctx);
    idl_define_global_property_reference(ctx, global, "CloseWatcher", ctor);
    JS_FreeValue(ctx, global);
}

void close_watcher_interface_declare(JSContext *ctx)
{
    static const IdlArgType CTOR_ARGS[1] = { IDL_DICT };
    JSClassDef d = { "CloseWatcher" };

    DCHECK(g_id_ctor < 0, "close_watcher_interface_declare ran twice — the class, the slot key and the four "
                          "members' pool ids are the AGENT's");
    JS_NewClassID(JS_GetRuntime(ctx), &g_cw_class);
    JS_NewClass(JS_GetRuntime(ctx), g_cw_class, &d);
    g_slot_key = JS_NewSymbol(ctx, "closeWatcherInternal", false);
    CHECK(!JS_IsException(g_slot_key), "the CloseWatcher internal-close-watcher slot key allocation failed");
    g_slot_atom = JS_ValueToAtom(ctx, g_slot_key);
    CHECK(g_slot_atom != JS_ATOM_NULL, "the CloseWatcher internal-close-watcher slot key could not be interned");

    /* The dictionary's one interface-typed member carries the class §3.2.15 brands it against. It is read
       HERE, at declaration time, and core/platform.c's list is what makes that safe: the `abort` row runs
       before the `element` row this declaration is reached through, so the class exists. idl_iface_brand's own
       refusal of a zero class is what turns the day that stops being true into an abort at this line rather
       than a silent zero read at a conversion. */
    CW_OPTIONS[0].iface = abort_signal_class();
    g_id_ctor = idl_method_id_dict(ctx, CTOR_ARGS, 1, CW_OPTIONS,
                                   (int)(sizeof(CW_OPTIONS) / sizeof(CW_OPTIONS[0])), js_cw_ctor, 0);
    /* `constructor(optional CloseWatcherOptions options = {})` — the word `optional` is what makes
       `new CloseWatcher()` a legal zero-argument construction rather than Web IDL §3.6's arity TypeError, and
       it is also what §3.7.1 Interface object's `length` is computed from. */
    idl_optional_from(0);
    g_id_request_close = idl_method_id_step(ctx, NULL, 0, NULL, 0, &CWI_DECL, M_REQUEST_CLOSE);
    g_id_close = idl_method_id_step(ctx, NULL, 0, NULL, 0, &CWI_DECL, M_CLOSE);
    g_id_destroy = idl_method_id(ctx, NULL, 0, js_cw_destroy, 0);
    realm_declare_intrinsic(close_watcher_interface_install_realm);
}

void close_watcher_interface_free(JSRuntime *rt)
{
    /* The PROTOTYPES and the interface objects are the realms' and go with their contexts. What the agent
       holds is the slot key — a Symbol nobody frees is a live GC object the runtime's own walk counts as a
       leak — plus the class id and the four pool ids, which are registrations in a runtime that is going away
       with them. */
    JS_FreeAtomRT(rt, g_slot_atom);
    g_slot_atom = JS_ATOM_NULL;
    JS_FreeValueRT(rt, g_slot_key);
    g_slot_key = JS_UNDEFINED;
    g_id_ctor = g_id_request_close = g_id_close = g_id_destroy = -1;
}
