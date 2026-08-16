/* StorageManager, and File System §3's ACCESS TO THE BUCKET FILE SYSTEM.
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

static JSClassID g_sm_class;
static int       g_obj_slot = -1;
static int       g_id_get_directory = -1;

/* WEB IDL §3.7.5's BRAND CHECK. `StorageManager.prototype.getDirectory.call({})` is a TypeError, and a page
   tells that apart from a rejected promise. */
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
                 "27.2.1.3.2 step 8's `then` read is the page's)")
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
           routed through §7.2.5.1's same-origin check — this document's navigable compared against itself,
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

/* ---- the per-realm install ------------------------------------------------------------------------------ */

/* Storage §2's `[SameObject] readonly attribute StorageManager storage` on `partial interface Navigator`. The
   [SameObject] guarantee comes from the realm slot rather than from a cache in the getter, which is the same
   reason §6.4.4's UserActivation is minted with the realm. */
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
    /* §3's partial is `[SecureContext]`, and Web IDL §3.3.13 REMOVES a member in a non-secure realm rather than
       making it throw — `'getDirectory' in navigator.storage` is what a bundle feature-detects with, and the
       three answers (absent, throwing, undefined) are three different branches. */
    idl_install_method_exposed(ctx, proto, "getDirectory", 0, g_id_get_directory, IDL_SECURE_CONTEXT);
    JS_SetClassProto(ctx, g_sm_class, JS_DupValue(ctx, proto));

    global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "StorageManager", idl_interface_object(ctx, "StorageManager", proto));
    JS_FreeValue(ctx, global);

    obj = JS_NewObjectProtoClass(ctx, proto, g_sm_class);
    JS_FreeValue(ctx, proto);
    CHECK(!JS_IsException(obj), "the Navigator's StorageManager could not be allocated");
    realm_value_set(ctx, g_obj_slot, obj);

    /* THE MEMBER GOES ON THE NAVIGATOR THIS REALM ALREADY BUILT — a partial interface adds to the object, it
       does not make a second one. Storage §2 marks the whole partial `[SecureContext]`. */
    nav = navigator_object(ctx);
    DCHECK(JS_IsObject(nav), "Storage §2's `storage` was installed on a realm with no Navigator — navigator.c's "
                             "intrinsic has to be DECLARED first, and core/realm.h runs them in declaration "
                             "order");
    idl_install_accessor_exposed(ctx, nav, "storage", sm_get_storage, 0, -1, IDL_SECURE_CONTEXT);
    JS_FreeValue(ctx, nav);
}

void storage_manager_init(JSContext *ctx)
{
    JSClassDef d = { "StorageManager" };

    DCHECK(g_obj_slot < 0, "storage_manager_init ran twice — the class and the slot are declared once per AGENT");
    JS_NewClassID(JS_GetRuntime(ctx), &g_sm_class);
    CHECK(JS_NewClass(JS_GetRuntime(ctx), g_sm_class, &d) == 0,
          "StorageManager: the per-realm prototype slot could not be declared");
    g_obj_slot = realm_value_declare(ctx, "Storage §2 the Navigator's associated StorageManager");
    g_id_get_directory = idl_method_id_step(ctx, NULL, 0, NULL, 0, &SM_DECL, 0);
    agent_state_id("storage_manager", &g_obj_slot,
                   "Storage §2's associated-StorageManager realm slot, and the declaration latch");
    agent_state_id("storage_manager", &g_id_get_directory, "§3's getDirectory machine");
    realm_declare_intrinsic(storage_manager_install_realm);
}

void storage_manager_free(void)
{
    g_obj_slot = -1;
    g_id_get_directory = -1;
}
