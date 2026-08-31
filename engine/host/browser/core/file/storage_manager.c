/* StorageManager — Storage §8 "API" — and File System §3's ACCESS TO THE BUCKET FILE SYSTEM.
 *
 *   [SecureContext, Exposed=(Window,Worker)]
 *   interface StorageManager {
 *     Promise<boolean> persisted();
 *     [Exposed=Window] Promise<boolean> persist();
 *     Promise<StorageEstimate> estimate();
 *   };
 *   dictionary StorageEstimate { unsigned long long usage; unsigned long long quota; };
 *
 *   [SecureContext] interface mixin NavigatorStorage { [SameObject] readonly attribute StorageManager storage; };
 *   Navigator includes NavigatorStorage;
 *
 * THE SECTION IS §8 AND NOT §2. This file cited "Storage §2" for the interface and for the Navigator member,
 * and §2 is Terminology — §8 API is where both the mixin and the interface are declared, and §5 Persistence
 * permission and §6 Usage and quota are where the two facts the members answer with live. A cited number that
 * resolves to the wrong section reads as authoritative and sends the next reader somewhere that does not say
 * what the code claims, which is worse than no citation at all. There is no committed spec index for the
 * Storage Standard, so engine/citegen.mjs counts these and checks none of them: the numbers here were verified
 * against the fetched text, and the step lists below were counted with list DEPTH tracked (§8's three
 * algorithms each have SIX top-level steps, whose step 5 is an "Otherwise, run these steps in parallel:" that
 * holds the whole of the work as SUB-items).
 *
 *   [SecureContext]
 *   partial interface StorageManager {
 *     Promise<FileSystemDirectoryHandle> getDirectory();
 *   };
 *
 *   The getDirectory() method steps are:
 *     1. Let environment be the current settings object.
 *     2. Let map be the result of running obtain a local storage bottle map with environment and "fileSystem".
 *        If this returns failure, return a promise rejected with a "SecurityError" DOMException.
 *     3. If map["root"] does not exist:
 *          1. Let dir be a new directory entry whose query access and request access algorithms always return
 *             a file system access result with a permission state of "granted" and with an error name of the
 *             empty string.
 *          2. Set dir's name to the empty string.
 *          3. Set dir's children to an empty set.
 *          4. Set map["root"] to dir.
 *     4. Let root be an implementation-defined opaque string.
 *     5. Let path be « the empty string ».
 *     6. Let handle be the result of creating a new FileSystemDirectoryHandle given root and path in the
 *        current realm.
 *     7. Assert: locating an entry given handle's locator returns a directory entry that is the same entry as
 *        map["root"].
 *     8. Return a promise resolved with handle.
 *
 * WHY THIS COMPONENT IS THE ENTRY POINT AND NOT A PICKER. §3 is the door onto the file system that needs no
 * user gesture, no permission and no prompt — it is the one the standard that DEFINES the model provides, and
 * everything §2 specifies is reachable through it alone. A picker is File System Access's separate door onto
 * the OTHER root, and it is a different standard with its own activation gate.
 *
 * STEP 2's BOTTLE MAP IS THIS AGENT'S, AND THAT IS EXACTLY WHAT THE SPEC ASKS FOR. A local storage bottle map
 * is keyed by the environment's ORIGIN, and §Security makes an instance an ORIGIN-KEYED AGENT CLUSTER — so the
 * one map an instance can have is the one this engine holds, and "obtain" cannot fail for a realm that exists.
 * The OPAQUE-ORIGIN case is the one it CAN fail for, and that is asserted rather than assumed: an opaque origin
 * has no storage key, so a document with one must reject rather than silently share the agent's filesystem.
 *
 * STEP 3 HAS ALREADY RUN. The root is built at the pre-boot COW baseline (core/file/file_system.c), because a
 * directory entry created on the first getDirectory() would be created inside whichever FLOW asked first and
 * that flow's creation would become every sibling's baseline. Nothing can observe the difference — an
 * empty directory entry named the empty string is the same object either way — and step 7's assertion is what
 * checks it, kept as a real assert rather than dropped as a spec formality. */
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/agent_state.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "core/file/file_system.h"
#include "core/file/file_system_handle.h"
#include "core/file/storage_manager.h"
#include "core/dom/document.h"
#include "core/frame/navigator.h"
#include "core/frame/window_proxy.h"
#include "core/permissions/permission_store.h"
#include "core/storage/storage_shed.h"

static JSClassID g_sm_class;
static int       g_obj_slot = -1;
static int       g_id_get_directory = -1;
/* Storage §8's three members share ONE algorithm shape and differ by magic, so they share one declaration. */
static int       g_id_storage[3] = { -1, -1, -1 };
/* Storage §5 Persistence permission's "persistent-storage" powerful feature, as Permissions §4's registry
   indexes it. Read once per AGENT, because the registry is the agent's. */
static int       g_persistent_storage = -1;

/* WEB IDL §3.7.7's BRAND CHECK, from `create an operation function`: "If jsValue does not implement the
   interface target, throw a TypeError". EVERY MEMBER OF THIS INTERFACE RETURNS A PROMISE, so §3.7.7's own last
   steps turn that throw into a REJECTION — "if an exception E was thrown: If op has a return type that is a
   promise type, then return ! Call(%Promise.reject%, %Promise%, «E»)" — and the `Try` those steps close opens
   BEFORE the brand check. `StorageManager.prototype.getDirectory.call({})` therefore hands back a rejected
   promise and does not throw; this file said the opposite, and a bundle that wrote only `.catch` around one of
   these is relying on the spec's answer rather than on that one. The conversion is core/idl_args.h's
   `idl_returns_promise`, declared beside each member, so the body still throws and the machine rejects. */
static bool sm_brand(JSContext *ctx, JSValueConst this_val)
{
    DCHECK(g_sm_class != 0, "a StorageManager member ran before storage_manager_init declared the class");
    if (JS_GetClassID(this_val) == g_sm_class) return true;
    JS_ThrowTypeError(ctx, "a StorageManager member was reached on something that is not a StorageManager");
    return false;
}

#define SM_STAGES(X) \
    X(SM_RUN,    "File System §3 getDirectory() steps 1-7 (the bottle map, the bucket file system's root " \
                 "directory entry, and the handle over it)") \
    X(SM_SETTLE, "File System §3 getDirectory() step 8 (return a promise resolved with handle — " \
                 "27.5.1.3 step 2.f's `then` read is the page's)")
enum { IDL_STEP_STAGE_BASE(SM_STAGES) SM_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const SM_STEPS[] = { SM_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    uint8_t cphase;
    JSValue promise;   /* owned */
    JSValue func;      /* its resolve or its reject (owned) */
    JSValue value;     /* the handle, or the rejection reason (owned) */
    JSValue cb[3];
} SmState;

static void sm_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    SmState *s = st;
    int k;

    v->val(ctx, &s->promise);
    v->val(ctx, &s->func);
    v->val(ctx, &s->value);
    STEP_CB_FOREACH(s->cb, k) v->val(ctx, &s->cb[k]);
}

static int sm_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                   JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    SmState *s = st;
    int r;

    (void)argc; (void)argv;
    *presult = JS_UNDEFINED;

    if (hdr->stage == SM_RUN) {
        static const char *const ROOT_PATH[] = { "" };   /* step 5: "let path be « the empty string »" */
        JSValue funcs[2], located, root_entry;
        int reject = 0;
        int k;

        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        s->promise = s->func = s->value = JS_UNDEFINED;
        s->cphase = 0;
        STEP_CB_FOREACH(s->cb, k) s->cb[k] = JS_UNDEFINED;
        if (!sm_brand(ctx, hdr->this_val)) return -1;
        s->promise = JS_NewPromiseCapability(ctx, funcs);
        if (JS_IsException(s->promise)) return -1;
        /* STEP 2's failure: an OPAQUE ORIGIN has no storage key, so there is no bottle map to obtain and the
           document must not reach this agent's filesystem. IT IS THE OPAQUE TEST, ASKED AS ONE. It used to be
           routed through §7.2.1's same-origin check — this document's navigable compared against itself,
           which was false in exactly the case where the origin serialized to "null" — and that route inverted
           the day an origin became a record: §7.1.1 step 1 makes an origin same origin with ITSELF, opaque
           included, so the check would now pass and a sandboxed document would reach the filesystem. */
        if (origin_is_opaque(window_proxy_origin(document_window_proxy(ctx)))) {
            JS_ThrowDOMException(ctx, "SecurityError",
                                 "a document with an opaque origin has no storage bottle map");
            s->value = JS_GetException(ctx);
            reject = 1;
        } else {
            /* STEPS 4-6. The root is the implementation-defined opaque string core/file/file_system.h names,
               and the handle is §2.4's create a new FileSystemDirectoryHandle in the CURRENT realm — this one,
               because a C member runs in the realm that defined it and this member was installed on this
               realm's prototype. */
            s->value = fs_handle_new(ctx, /*directory*/ true, FS_ROOT_BUCKET, ROOT_PATH, 1);
            /* STEP 7's ASSERTION, kept: "locating an entry given handle's locator returns a directory entry
               that is the same entry as map["root"]". It is what checks that the eager root and the path the
               handle was minted with agree, and it is the ONE place that agreement is stated. */
            located = file_system_locate(ctx, FS_ROOT_BUCKET, ROOT_PATH, 1);
            root_entry = file_system_root_entry(ctx, FS_ROOT_BUCKET);
            DCHECK(JS_VALUE_GET_PTR(located) == JS_VALUE_GET_PTR(root_entry) &&
                   file_system_is_directory(located),
                   "File System §3 step 7: the locator getDirectory() minted does not locate the bucket file "
                   "system's root directory entry — the path « the empty string » and the root entry's own name "
                   "are one fact stated in two files, and they have come apart");
            JS_FreeValue(ctx, located);
            JS_FreeValue(ctx, root_entry);
        }
        s->func = funcs[reject];
        JS_FreeValue(ctx, funcs[reject ^ 1]);
        STEP_GOTO(hdr->stage, SM_SETTLE, &s->cphase, NULL);
    }

    DCHECK(hdr->stage == SM_SETTLE, "getDirectory() resumed into a stage §3 does not have");
    {
        JSValue settled = JS_UNDEFINED;

        r = step_call_run(ctx, &s->cphase, STEP_CB(s->cb), s->func, JS_UNDEFINED, 1,
                          (JSValueConst *)&s->value, cb_result, &settled, out_cb, out_argc);
        if (r > 0) return r;
        JS_FreeValue(ctx, settled);
    }
    *presult = s->promise;
    s->promise = JS_UNDEFINED;
    return JS_STEP_DONE;
}

static const IdlStepDecl SM_DECL = {
    sm_step, sizeof(SmState), sm_visit, NULL,
    "File System §3 StorageManager.getDirectory()", SM_STEPS
};

/* ---- Storage §8 API's persisted(), persist() and estimate() ------------------------------------------------
 *
 * ONE MACHINE FOR THREE MEMBERS, because §8 writes them as one algorithm with a different middle. All three
 * are, verbatim: "1. Let promise be a new promise. 2. Let global be this's relevant global object. 3. Let
 * shelf be the result of running obtain a local storage shelf with this's relevant settings object. 4. If
 * shelf is failure, then reject promise with a TypeError. 5. Otherwise, run these steps in parallel: … 6.
 * Return promise." Only step 5's sub-list differs, and a second copy of steps 1-4 and 6 would be two answers
 * to what a failed shelf does.
 *
 *   persisted()  5.1 Let persisted be true if shelf's bucket map["default"]'s mode is "persistent"; otherwise
 *                    false.  5.2 Queue a storage task with global to resolve promise with persisted.
 *   persist()    5.1 Let permission be the result of requesting permission to use "persistent-storage".
 *                5.2 Let bucket be shelf's bucket map["default"].  5.3 Let persisted be true if bucket's mode
 *                    is "persistent"; otherwise false.  5.4 If persisted is false and permission is "granted",
 *                    then: 5.4.1 Set bucket's mode to "persistent". 5.4.2 If there was no internal error, then
 *                    set persisted to true.  5.5 Queue a storage task with global to resolve promise with
 *                    persisted.
 *   estimate()   5.1 Let usage be storage usage for shelf.  5.2 Let quota be storage quota for shelf.
 *                5.3 Let dictionary be a new StorageEstimate dictionary whose usage member is usage and quota
 *                    member is quota.  5.4 If there was an internal error … reject promise with a TypeError.
 *                5.5 Otherwise, queue a storage task with global to resolve promise with dictionary.
 *
 * STEP 5's "IN PARALLEL" AND ITS QUEUE-A-STORAGE-TASK ARE ONE STEP HERE, which is the same treatment every
 * other in-parallel-then-queue member in this engine takes and for the same reason: there is no second thread,
 * and the parallel half is itself preemptible because it is a step closure driven by the same scheduler. What
 * that folds away is exactly one task turn — a page that calls estimate() and then queues a task of its own
 * would, in a browser, see its task first — and what it preserves is everything the promise itself decides,
 * since a resolution is a microtask either way.
 *
 * §5.4's "IF THERE WAS NO INTERNAL ERROR" AND §8's TWO OTHER MENTIONS OF ONE ARE NOT REACHABLE HERE, and that
 * is a statement about the model rather than an omission: §8's own note calls an internal error "some kind of
 * low-level platform or hardware fault", the shelf is a JS object graph in this heap, and an allocation that
 * fails on the way to reading it is a CHECK rather than a value this algorithm may carry. So `persisted` is
 * never false-because-of-an-error, and the reject arm of estimate()'s step 5.4 has no producer.
 *
 * WHOSE SETTINGS OBJECT — step 3 says "this's relevant settings object", and this engine reads THIS REALM's.
 * They agree by construction and the model asserts it: §Security makes an instance an origin-keyed agent
 * cluster, every realm in this heap therefore has one storage key, and storage_shed.c's obtain aborts if a
 * second one ever appears. getDirectory() above states the same argument for the bottle map. */
#define SG_STAGES(X)                                                                                          \
    X(SG_RUN,    "Storage §8 persisted()/persist()/estimate() steps 1-4 (a new promise; obtain a local "       \
                 "storage shelf; a failed shelf rejects with a TypeError), and — for persisted() and "         \
                 "estimate() — the whole of step 5")                                                          \
    X(SG_PERMIT, "Storage §8 persist() step 5.1 (Permissions §5.2's request permission to use "                \
                 "\"persistent-storage\", whose step 3 asks the user and FORKS) and steps 5.2-5.4")            \
    X(SG_SETTLE, "Storage §8 step 5's queue-a-storage-task (settling the promise — 27.5.1.3 step 2.f's "       \
                 "`then` read is the page's)")
enum { IDL_STEP_STAGE_BASE(SG_STAGES) SG_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const SG_STEPS[] = { SG_STAGES(JS_STEP_STAGE_LABEL) NULL };

/* WHICH MEMBER — the magic IS the member, exactly as it is for every other declaration this engine shares. */
enum { SG_PERSISTED = 0, SG_PERSIST, SG_ESTIMATE, SG_MEMBER_N };
static const char *const SG_NAME[SG_MEMBER_N] = { "persisted", "persist", "estimate" };

typedef struct {
    uint8_t cphase;    /* the settle's call cursor */
    /* PERMISSIONS §5.2's OWN PHASE BYTE — the caller's, as permission_store.h requires, and its OWN rather than
       shared with the settle: the request delegates into §5.1's two-question chain and returns between them, so
       which question is outstanding cannot live in a C local and must not share a value with another cursor. */
    uint8_t pphase;
    JSValue promise;   /* owned */
    JSValue func;      /* its resolve or its reject (owned) */
    JSValue value;     /* the boolean, the StorageEstimate, or the rejection reason (owned) */
    /* THE SHELF, held ACROSS persist()'s permission fork (owned). It is a live object of the baseline graph, so
       a flow parked inside §5.2's ask resumes holding the same shelf its own world's writes are on. */
    JSValue shelf;
    JSValue cb[3];
} SgState;

static void sg_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    SgState *s = st;
    int k;

    v->val(ctx, &s->promise);
    v->val(ctx, &s->func);
    v->val(ctx, &s->value);
    v->val(ctx, &s->shelf);
    STEP_CB_FOREACH(s->cb, k) v->val(ctx, &s->cb[k]);
}

/* §8 step 5.3's "a new StorageEstimate dictionary whose usage member is usage and quota member is quota",
 * CONVERTED TO A JAVASCRIPT VALUE by Web IDL §3.2.17 Dictionary types: "Let O be
 * OrdinaryObjectCreate(%Object.prototype%) … For each dictionary member member declared on dictionary, in
 * LEXICOGRAPHICAL ORDER: … Perform ! CreateDataPropertyOrThrow(O, key, value)."
 *
 * THE ORDER IS OBSERVABLE AND IT IS NOT THE IDL'S DECLARATION ORDER. `StorageEstimate` declares `usage` and
 * then `quota`, and §3.2.17 sorts them, so `Object.keys(await navigator.storage.estimate())` is
 * ["quota","usage"]. CreateDataProperty and not Set, which is why these are DEFINED rather than assigned.
 * Both members are `unsigned long long`; §3.2.10's conversion to a JavaScript value is the Number, and both
 * quantities here are far below 2**53 so no value is lost in the double. */
static JSValue sg_estimate_dictionary(JSContext *ctx, double usage, double quota)
{
    JSValue o = JS_NewObject(ctx);

    CHECK(!JS_IsException(o), "Storage §8 step 5.3: a StorageEstimate could not be allocated");
    DCHECK(usage >= 0 && quota >= 0,
           "Storage §6's usage or quota came out NEGATIVE — both are `unsigned long long` members, so a "
           "negative one is a byte count that was computed by subtraction somewhere it should have been a sum");
    CHECK(JS_DefinePropertyValueStr(ctx, o, "quota", JS_NewFloat64(ctx, quota), JS_PROP_C_W_E) >= 0 &&
          JS_DefinePropertyValueStr(ctx, o, "usage", JS_NewFloat64(ctx, usage), JS_PROP_C_W_E) >= 0,
          "Storage §8 step 5.3: a StorageEstimate member could not be defined");
    return o;
}

static int sg_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                   JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    SgState *s = st;
    int magic = idl_step_magic(hdr);
    int r;

    (void)argc; (void)argv;
    *presult = JS_UNDEFINED;
    DCHECK(magic >= 0 && magic < SG_MEMBER_N, "a Storage §8 member ran with a magic no member of §8 declares");

    if (hdr->stage == SG_RUN) {
        JSValue funcs[2];
        int reject = 0;
        int k;

        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        s->promise = s->func = s->value = s->shelf = JS_UNDEFINED;
        s->cphase = s->pphase = 0;
        STEP_CB_FOREACH(s->cb, k) s->cb[k] = JS_UNDEFINED;
        if (!sm_brand(ctx, hdr->this_val)) return -1;   /* §3.7.7's Try — the machine turns it into a rejection */
        /* STEP 2's `global` is read where the algorithm uses it, which is only the queue-a-storage-task folded
           into the settle below. §8 marks persist() `[Exposed=Window]`, so it is the one member whose global
           the IDL constrains, and every global this agent builds is a Window. */
        DCHECK(magic != SG_PERSIST || window_proxy_is(document_window_proxy(ctx)),
               "Storage §8's persist() ran in a realm whose global is not a Window — its IDL declares the "
               "member `[Exposed=Window]`, so a WorkerGlobalScope must not have it on its prototype at all "
               "and the install rather than the body is where that has to be decided");
        s->promise = JS_NewPromiseCapability(ctx, funcs);   /* step 1 */
        if (JS_IsException(s->promise)) return -1;
        s->shelf = storage_shed_obtain_local_shelf(ctx);    /* step 3 */
        if (JS_IsUndefined(s->shelf)) {
            /* STEP 4. The failure is §4.4 step 2's — an OPAQUE ORIGIN has no storage key — and §8 says to
               reject with a TypeError, which is a DIFFERENT error from the "SecurityError" HTML §12.2.2 raises
               out of that same failure and from the one File System §3 step 2 names. Each standard states its
               own, so a shared error here would be one of the three getting the other two wrong. */
            JS_ThrowTypeError(ctx, "%s() was called in a document whose origin is opaque, which has no storage "
                                   "key and therefore no storage shelf", SG_NAME[magic]);
            s->value = JS_GetException(ctx);
            reject = 1;
        } else if (magic == SG_PERSISTED) {
            /* STEP 5.1. */
            s->value = JS_NewBool(ctx, storage_shed_bucket_mode(ctx, s->shelf) == STORAGE_BUCKET_PERSISTENT);
        } else if (magic == SG_ESTIMATE) {
            /* STEPS 5.1-5.3. */
            s->value = sg_estimate_dictionary(ctx, storage_shed_shelf_usage(ctx, s->shelf),
                                              storage_shed_shelf_quota(ctx, s->shelf));
        }
        /* WHICH HALF OF THE CAPABILITY THIS MEMBER WILL USE IS ALREADY DECIDED. Step 4 is the only rejection
           any of the three has — persist()'s step 5.5 RESOLVES whatever the user decided, and estimate()'s
           step 5.4 reject arm has no producer here (see the header note on internal errors) — so the other
           function is released now rather than carried across a fork that cannot need it. */
        s->func = funcs[reject];
        JS_FreeValue(ctx, funcs[reject ^ 1]);
        if (magic == SG_PERSIST && !reject)
            STEP_GOTO(hdr->stage, SG_PERMIT, &s->cphase, &s->pphase, NULL);
        else
            STEP_GOTO(hdr->stage, SG_SETTLE, &s->cphase, &s->pphase, NULL);
    }

    if (hdr->stage == SG_PERMIT) {
        PermissionDescriptor d;
        /* NOT A PERMISSION_* VALUE — the request WRITES this, and seeding it with `granted` (which is 0) would
           make a request that answered nothing indistinguishable from one the user allowed. */
        int state = -1;
        bool persisted;

        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        DCHECK(g_persistent_storage >= 0,
               "Storage §5's \"persistent-storage\" powerful feature has no row in Permissions §4's registry — "
               "storage_manager_init reads its index once per agent, so a missing one means the registry and "
               "this component disagree about the feature's name");
        d.feature = g_persistent_storage;
        /* §5's feature declares no aspect and no subject: its permission descriptor type is the default
           `PermissionDescriptor`, whose only member is `name`. */
        d.aspect = false;
        d.subject = JS_UNDEFINED;
        /* STEP 5.1 — Permissions §5.2, whose step 3 asks the user for express permission. Over a decision this
           engine has not observed that FORKS, so a bundle's granted path (the persisted branch, the offline
           cache it then fills, the endpoints behind it) and its denied path (the fallback, the re-prompt) are
           both explored. A C `if` here would delete one of them. */
        r = permission_request_run(ctx, hdr, &s->pphase, &d, &state);
        if (r) return r;
        DCHECK(state == PERMISSION_GRANTED || state == PERMISSION_DENIED,
               "Permissions §5.2 answered a \"persistent-storage\" request with neither granted nor denied");
        /* STEPS 5.2-5.3. */
        persisted = storage_shed_bucket_mode(ctx, s->shelf) == STORAGE_BUCKET_PERSISTENT;
        if (!persisted && state == PERMISSION_GRANTED) {          /* step 5.4 */
            storage_shed_bucket_set_mode(ctx, s->shelf, STORAGE_BUCKET_PERSISTENT);   /* step 5.4.1 */
            persisted = true;                                                          /* step 5.4.2 */
        }
        s->value = JS_NewBool(ctx, persisted);
        STEP_GOTO(hdr->stage, SG_SETTLE, &s->cphase, &s->pphase, NULL);
    }

    DCHECK(hdr->stage == SG_SETTLE, "a Storage §8 member resumed into a stage §8 does not have");
    {
        JSValue settled = JS_UNDEFINED;

        r = step_call_run(ctx, &s->cphase, STEP_CB(s->cb), s->func, JS_UNDEFINED, 1,
                          (JSValueConst *)&s->value, cb_result, &settled, out_cb, out_argc);
        if (r > 0) return r;    /* parked ON THE SETTLE; the `then` read runs with a flow base under it */
        JS_FreeValue(ctx, settled);
    }
    *presult = s->promise;
    s->promise = JS_UNDEFINED;
    return JS_STEP_DONE;
}

static const IdlStepDecl SG_DECL = {
    sg_step, sizeof(SgState), sg_visit, NULL,
    "Storage §8 StorageManager.persisted() / persist() / estimate()", SG_STEPS
};

/* ---- the per-realm install ------------------------------------------------------------------------------ */

/* Storage §8's `[SameObject] readonly attribute StorageManager storage` — the one member of the
   `NavigatorStorage` mixin, which §8 says `Navigator includes`. §8 states the object's identity in its own
   words — "Each environment settings object has an associated StorageManager object ... The storage getter
   steps are to return this's relevant settings object's StorageManager object" — so [SameObject] comes from
   the realm slot rather than from a cache in the getter, which is the same reason §6.4.4's UserActivation is
   minted with the realm. */
static JSValue sm_get_storage(JSContext *ctx, JSValueConst this_val, int magic)
{
    (void)this_val; (void)magic;
    return realm_value_get(ctx, g_obj_slot);
}

static void storage_manager_install_realm(JSContext *ctx)
{
    JSValue proto, prev, global, obj, nav;

    prev = JS_GetClassProto(ctx, g_sm_class);
    DCHECK(JS_IsNull(prev), "storage_manager_install_realm ran twice in one realm");
    JS_FreeValue(ctx, prev);
    proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(proto), "StorageManager.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "StorageManager");
    /* THE INTERFACE ITSELF IS `[SecureContext]` (Storage §8) and so is File System §3's partial, and Web IDL
       §3.3.13 REMOVES a member in a non-secure realm rather than making it throw — `'getDirectory' in
       navigator.storage` is what a bundle feature-detects with, and the three answers (absent, throwing,
       undefined) are three different branches. */
    idl_install_method_exposed(ctx, proto, "persisted", g_id_storage[SG_PERSISTED], IDL_SECURE_CONTEXT);
    /* §8 marks `persist()` `[Exposed=Window]` on top of the interface's own exposure. Every global this agent
       builds is a Window (there is no WorkerGlobalScope here), so the two exposures coincide and the member is
       installed under the one this engine can express; sg_step's own DCHECK is what states the other half, and
       it is what will fire the day a non-Window global reaches this prototype. */
    idl_install_method_exposed(ctx, proto, "persist", g_id_storage[SG_PERSIST], IDL_SECURE_CONTEXT);
    idl_install_method_exposed(ctx, proto, "estimate", g_id_storage[SG_ESTIMATE], IDL_SECURE_CONTEXT);
    idl_install_method_exposed(ctx, proto, "getDirectory", g_id_get_directory, IDL_SECURE_CONTEXT);
    JS_SetClassProto(ctx, g_sm_class, JS_DupValue(ctx, proto));

    global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "StorageManager", idl_interface_object(ctx, "StorageManager", proto));
    JS_FreeValue(ctx, global);

    obj = JS_NewObjectProtoClass(ctx, proto, g_sm_class);
    JS_FreeValue(ctx, proto);
    CHECK(!JS_IsException(obj), "the Navigator's StorageManager could not be allocated");
    realm_value_set(ctx, g_obj_slot, obj);

    /* THE MEMBER GOES ON THE NAVIGATOR THIS REALM ALREADY BUILT — a mixin adds to the object, it does not make
       a second one. Storage §8 marks the whole `NavigatorStorage` mixin `[SecureContext]`. */
    nav = navigator_object(ctx);
    DCHECK(JS_IsObject(nav), "Storage §8's `storage` was installed on a realm with no Navigator — navigator.c's "
                             "intrinsic has to be DECLARED first, and core/realm.h runs them in declaration "
                             "order");
    idl_install_accessor_exposed(ctx, nav, "storage", sm_get_storage, 0, -1, IDL_SECURE_CONTEXT);
    JS_FreeValue(ctx, nav);
}

void storage_manager_init(JSContext *ctx)
{
    JSClassDef d = { "StorageManager" };
    int m;

    DCHECK(g_obj_slot < 0, "storage_manager_init ran twice — the class and the slot are declared once per AGENT");
    JS_NewClassID(JS_GetRuntime(ctx), &g_sm_class);
    CHECK(JS_NewClass(JS_GetRuntime(ctx), g_sm_class, &d) == 0,
          "StorageManager: the per-realm prototype slot could not be declared");
    g_obj_slot = realm_value_declare(ctx, "Storage §8 the environment settings object's associated "
                                          "StorageManager");
    /* STORAGE §5's POWERFUL FEATURE, looked up ONCE PER AGENT. Permissions §4's registry is a static table, so
       an absent row is this component and permission_store.c disagreeing about the feature's name rather than
       a runtime condition — which is why it is asserted here and read as a fact at the member. */
    g_persistent_storage = permission_feature_of("persistent-storage");
    DCHECK(g_persistent_storage >= 0,
           "Permissions §4's registry has no row for Storage §5's \"persistent-storage\" powerful feature — "
           "§8's persist() step 5.1 requests permission to use it by that exact name, so a missing row means "
           "the member would ask for a feature this user agent claims not to support");
    g_id_get_directory = idl_method_id_step(ctx, NULL, 0, NULL, 0, &SM_DECL, 0);
    /* WEB IDL §3.7.7: `Promise<FileSystemDirectoryHandle> getDirectory()` has a PROMISE return type, so the
       brand check and every other throw inside its `Try` is a rejection rather than a throw. */
    idl_returns_promise();
    for (m = 0; m < SG_MEMBER_N; m++) {
        /* NO ARGUMENTS — §8 declares all three with an empty argument list, so there is nothing for the
           conversion to run and the whole of each member is its step machine. */
        static const char *const WHAT[SG_MEMBER_N] = {
            "Storage §8's persisted() machine", "Storage §8's persist() machine",
            "Storage §8's estimate() machine",
        };

        g_id_storage[m] = idl_method_id_step(ctx, NULL, 0, NULL, 0, &SG_DECL, m);
        idl_returns_promise();
        agent_state_id("storage_manager", &g_id_storage[m], WHAT[m]);
    }
    agent_state_id("storage_manager", &g_obj_slot,
                   "Storage §8's associated-StorageManager realm slot, and the declaration latch");
    agent_state_id("storage_manager", &g_id_get_directory, "File System §3's getDirectory machine");
    agent_state_id("storage_manager", &g_persistent_storage,
                   "Storage §5's \"persistent-storage\" row in Permissions §4's registry");
    realm_declare_intrinsic(storage_manager_install_realm);
}

void storage_manager_free(void)
{
    int m;

    g_obj_slot = -1;
    g_id_get_directory = -1;
    g_persistent_storage = -1;
    for (m = 0; m < SG_MEMBER_N; m++) g_id_storage[m] = -1;
}
