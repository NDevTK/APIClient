/* FILE SYSTEM ACCESS §2.2 AND §2.3 — the "file-system" powerful feature and its two members. See
 * file_system_access.h for the feature's registry columns and for why each of §2.2's four permission state
 * constraints ends up where it does.
 *
 * THE TWO ALGORITHMS §2.2 DEFINES, AS THE STANDARD WRITES THEM.
 *
 *   To query file system permission given a FileSystemHandle handle and a FileSystemPermissionMode mode:
 *     1. Let desc be a FileSystemPermissionDescriptor.
 *     2. Set desc["name"] to "file-system".  3. Set desc["handle"] to handle.  4. Set desc["mode"] to mode.
 *     5. Return desc's permission state.
 *
 *   To request file system permission given a FileSystemHandle handle and a FileSystemPermissionMode mode:
 *     1-4. (the same descriptor)
 *     5. Let status be the result of running create a PermissionStatus for desc.
 *     6. Run the permission request algorithm for the "file-system" feature, given desc and status.
 *     7. Return desc's permission state.
 *
 *   [the "file-system" feature's] permission request algorithm, given a FileSystemPermissionDescriptor desc
 *   and a PermissionStatus status:
 *     1. Run the default permission query algorithm on desc and status.
 *     2. If status's state is not "prompt", then abort these steps.
 *     3. Let settings be desc["handle"]'s relevant settings object.
 *     4. Let global be settings's global object.
 *     5. If global is not a Window, then throw a "SecurityError" DOMException.
 *     6. If global does not have transient activation, then throw a "SecurityError" DOMException.
 *     7. If settings's origin is not same origin with settings's top-level origin, then throw a "SecurityError"
 *        DOMException.
 *     8. Request permission to use desc.
 *     9. Run the default permission query algorithm on desc and status.
 *
 * WHY STEP 5's STATUS IS NOT MINTED, AND WHY THAT IS NOT A STEP DROPPED. The status this algorithm creates is
 * never returned to the page and no reference to it ever escapes: §2.3.2 resolves its promise with a
 * PermissionState STRING, not with a status. The only thing steps 1 and 9 do to it is set its state to desc's
 * permission state, and the only reader of that is step 2, which is the same value read one line earlier — so
 * the object carries nothing across. What a minted one WOULD carry is §6.3.4's awareness chain, which
 * permission_status_new seeds on every status it makes: an unreachable object asking, for the life of the
 * agent, whether the user has changed a decision that nothing can observe the answer to. That is work with no
 * observable, which is a different thing from a step with no observable, and it is why the status is absent
 * rather than built and dropped.
 *
 * EVERY ASK IS ITS OWN STAGE. Three of this algorithm's steps are questions over unknown external state —
 * step 1's permission state, step 6's transient activation, step 8's express permission — and a machine may
 * hold exactly one request outstanding, so each has a stage to itself and the two-question chains inside
 * permission_state_run and permission_request_run are separated by the phase byte those two share. */
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/agent_state.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "core/dom/document.h"
#include "core/file/file_system.h"
#include "core/file/file_system_access.h"
#include "core/file/file_system_handle.h"
#include "core/frame/window_proxy.h"
#include "core/html/user_activation.h"
#include "core/permissions/permission_store.h"

/* §4's REGISTRY ROW FOR THIS FEATURE, resolved once at the agent's declaration. It is looked up by NAME rather
   than hard-coded, because the registry is the one place the set of supported features is stated and a second
   statement of "file-system"'s index here is a second thing to keep in step with it. */
static int g_feature = -1;
static int g_id_query = -1, g_id_request = -1;

/* §2.2's `enum FileSystemPermissionMode { "read", "readwrite" }` — the values of the descriptor type's aspect
   member, in the IDL's own order, which is also the order permission_store.c's registry row states them in.
   The two lists are the SAME FACT in two files and the assert at the bottom of this one is what checks it. */
static const char *const FS_MODE_VALUES[] = { "read", "readwrite", NULL };
#define FS_MODE_READWRITE 1

/* ---- the feature's own algorithms, declared into §4's registry ---------------------------------------------- */

/* Web IDL §3.2.15's BRAND TEST for the descriptor type's `required FileSystemHandle handle`. */
static bool fsa_subject_is(JSValueConst v)
{
    return fs_handle_is(v);
}

/* §2.2's PERMISSION STATE CONSTRAINTS. Only the first of the four survives as C — see file_system_access.h for
   why the other three are discharged by identity, by §4's partial order, and by the store's own key.
   §2.2's bucket test is the File System Standard's: "a FileSystemHandle is in a bucket file system if the
   first item of its locator's path is the empty string", which is exactly what core/file/file_system.h builds
   the bucket root's path as and why the local root's is non-empty. */
static bool fsa_constraints(JSContext *ctx, const PermissionDescriptor *d, PermissionDescriptor *out, int *fixed)
{
    bool directory;
    const char *root;
    const char *const *path;
    int npath;

    (void)ctx; (void)out;
    DCHECK(d->feature == g_feature, "the file-system constraints were asked about another feature's descriptor");
    if (!fs_handle_locator(d->subject, &directory, &root, &path, &npath)) {
        DFAIL("§2.2's permission state constraints were asked about a descriptor whose `handle` is not a "
              "FileSystemHandle — §6.2.1 step 5 brands it through permission_subject_is before a descriptor "
              "exists, and §2.2's own two algorithms build the descriptor from a handle they were handed");
        return false;
    }
    (void)directory;
    DCHECK(!strcmp(root, FS_ROOT_BUCKET) || !strcmp(root, FS_ROOT_LOCAL),
           "a FileSystemHandle names a file system root this engine does not have — there are exactly two, and "
           "the bucket test below distinguishes them by the FIRST PATH ITEM rather than by the root string, so "
           "a third root would be silently treated as local");
    if (path[0][0] == '\0') {
        /* CONSTRAINT 1 — "must ALWAYS be granted", for either mode. This is what makes the bucket file system
           the door that needs no prompt, and it is CONCRETE: it is the standard's own answer rather than
           something this engine is ignorant of, so there is nothing here to fork over. */
        *fixed = PERMISSION_GRANTED;
        return true;
    }
    return false;
}

void fs_access_grant(JSContext *ctx, JSValueConst handle, bool readwrite)
{
    PermissionDescriptor d;

    DCHECK(g_feature >= 0, "§3.1's grant ran before file_system_access_init resolved the feature's registry row");
    DCHECK(fs_handle_is(handle), "§3.1's grant was given something that is not a FileSystemHandle — its own "
                                 "sentence is about \"the returned handle\", and the factories return one");
    d.feature = g_feature;
    d.subject = handle;
    /* THE READ GRANT IS ALWAYS WRITTEN, and the readwrite one only for showSaveFilePicker — §3.1's two
       paragraphs, in its own order. Writing the weak one first also means the store's §4 partial-order assert
       sees a consistent pair at every point rather than a granted-strong-with-absent-weak in between. */
    d.aspect = false;
    permission_store_set(ctx, &d, PERMISSION_GRANTED);
    if (readwrite) {
        d.aspect = true;
        permission_store_set(ctx, &d, PERMISSION_GRANTED);
    }
}

/* ---- §2.3.1's queryPermission() and §2.3.2's requestPermission() -------------------------------------------- */

#define FSA_STAGES(X)                                                                                          \
    X(FSA_STATE,   "File System Access §2.2 query/request file system permission steps 1-5 (the descriptor, "   \
                   "and its PERMISSION STATE — unknown external state, so this is where the granted, denied "   \
                   "and prompt worlds fork)")                                                                  \
    X(FSA_ACTIVE,  "File System Access §2.2 the permission request algorithm steps 3-7 (settings's global is "  \
                   "a Window; global has TRANSIENT ACTIVATION — unknown, so the SecurityError arm and the "     \
                   "prompt arm are two worlds; settings's origin is same origin with its top-level origin)")    \
    X(FSA_REQUEST, "File System Access §2.2 the permission request algorithm step 8 (Request permission to use " \
                   "desc — Permissions §5.2, whose step 3 asks the user for express permission)")               \
    X(FSA_SETTLE,  "File System Access §2.3.1/§2.3.2 step 2.2 (resolve result with state — 27.5.1.3 step 2.f's " \
                   "`then` read is the page's)")
enum { IDL_STEP_STAGE_BASE(FSA_STAGES) FSA_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const FSA_STEPS[] = { FSA_STAGES(JS_STEP_STAGE_LABEL) NULL };

enum { M_QUERY = 0, M_REQUEST };

typedef struct {
    uint8_t cphase;    /* the settle CALL's own phase */
    uint8_t ua_phase;  /* the phase byte user_activation.h and permission_store.h both require of the CALLER */
    uint8_t mode;      /* descriptor["mode"], read ONCE at the entry and carried — see below */
    uint8_t started;
    JSValue promise;   /* owned */
    /* THE CAPABILITY'S BOTH HALVES (owned), because a promise settles exactly once and which way is decided by
       whichever step fails — three stages apart from the one that built them. */
    JSValue funcs[2];
    uint8_t reject;    /* which of the two the settle calls: 0 resolve, 1 reject */
    JSValue value;     /* the PermissionState string, or the rejection reason (owned) */
    JSValue handle;    /* `this`, held because the descriptor BORROWS it across every stage (owned) */
    JSValue cb[3];
} FsAccessState;

static void fsa_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    FsAccessState *s = st;
    int k;

    if (!s->started)
        return;
    v->val(ctx, &s->promise);
    v->val(ctx, &s->funcs[0]);
    v->val(ctx, &s->funcs[1]);
    v->val(ctx, &s->value);
    v->val(ctx, &s->handle);
    STEP_CB_FOREACH(s->cb, k) v->val(ctx, &s->cb[k]);
}

/* THE DESCRIPTOR STEPS 1-4 BUILD, which are the same four in both algorithms and are rebuilt at each stage
   rather than stored: a descriptor is (feature, aspect, subject) and all three are facts this state already
   holds, so a second copy of it on the state would be a second thing to keep in step with the handle. */
static PermissionDescriptor fsa_descriptor(const FsAccessState *s)
{
    PermissionDescriptor d;

    d.feature = g_feature;
    d.aspect = (s->mode == FS_MODE_READWRITE);
    d.subject = s->handle;
    return d;
}

/* §2.3's "reject result with that exception" — the DOMException as a VALUE, because both members return a
   promise the algorithm has already created and every failure settles it. */
static JSValue fsa_security_error(JSContext *ctx, const char *msg)
{
    JSValue e;

    JS_ThrowDOMException(ctx, "SecurityError", "%s", msg);
    e = JS_GetException(ctx);
    CHECK(!JS_IsUndefined(e), "file system access: a SecurityError was built and no exception was live");
    return e;
}

static int fsa_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                    JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    FsAccessState *s = st;
    int magic = idl_step_magic(hdr);
    int state = PERMISSION_PROMPT, rc;
    bool ok = false;

    *presult = JS_UNDEFINED;

    /* THE ONE-TIME PROLOGUE IS GUARDED BY `started` AND NOT BY THE STAGE: a request that SUSPENDS re-enters at
       the SAME stage, so a capability minted here would be minted again and the page handed a promise nothing
       settles. */
    if (!s->started) {
        JSValue mode_v;
        const char *mode;
        int k;

        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        s->promise = s->funcs[0] = s->funcs[1] = s->value = s->handle = JS_UNDEFINED;
        s->cphase = s->ua_phase = s->mode = s->reject = 0;
        STEP_CB_FOREACH(s->cb, k) s->cb[k] = JS_UNDEFINED;
        s->started = 1;
        /* STEP 1 OF BOTH MEMBERS: "let result be a new promise", created before the first thing that can fail
           — Web IDL §3.7.7 turns an abrupt completion of a promise-returning operation into a rejection, so a
           brand failure is a rejected promise and not a throw, and a page's `.catch` must see it. */
        s->promise = JS_NewPromiseCapability(ctx, s->funcs);
        if (JS_IsException(s->promise)) return -1;
        /* WEB IDL §3.7.5's BRAND CHECK, asked AFTER the capability exists because §3.7.7 makes it a rejection
           for an operation whose return type is a promise — a page tells that apart from `undefined`. */
        if (!fs_handle_is(hdr->this_val)) {
            JS_ThrowTypeError(ctx, "a FileSystemHandle permission member was reached on something that is not "
                                   "a FileSystemHandle");
            s->value = JS_GetException(ctx);
            s->reject = 1;
            hdr->stage = FSA_SETTLE;
            goto settle;
        }
        s->handle = JS_DupValue(ctx, hdr->this_val);
        /* `descriptor["mode"]`, read ONCE HERE AND CARRIED. Both algorithms are stated over the mode the call
           was made with, and every stage after this one is a rest point — so re-reading it at the stage that
           uses it would read it at the wrong TIME, which is the defect that arrives with every conversion from
           a call to a job. The dictionary is the ARGUMENT MACHINE's converted object, so the read runs none of
           the page's code and the enumeration has already been checked against FS_MODE_VALUES. */
        mode_v = idl_dict_get(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, "mode");
        /* AN ABSENT ARGUMENT IS AN ABSENT DICTIONARY, WHICH IS NOT THE SAME AS `{}`. `optional
           FileSystemHandlePermissionDescriptor descriptor = {}` gives `handle.queryPermission()` a dictionary
           whose `mode` carries the IDL's default — but the argument the machine converted is ABSENT, so every
           member of it reads as undefined and the default has to be applied here. It is the IDL's own value,
           read from the one list this file and §4's registry row both state, never a second spelling of
           "read". */
        if (JS_IsUndefined(mode_v)) {
            s->mode = 0;
        } else {
            mode = JS_ToCString(ctx, mode_v);
            CHECK(mode != NULL, "file system access: `mode` could not be read back after its declared "
                                "enumeration conversion");
            DCHECK(!strcmp(mode, FS_MODE_VALUES[0]) || !strcmp(mode, FS_MODE_VALUES[1]),
                   "`mode` reached the body as a value FileSystemPermissionMode does not declare — the argument "
                   "machine converts an IDL_ENUM against the value list the member declares and throws for "
                   "anything else, so a third value means the list and this file have come apart");
            s->mode = (uint8_t)(strcmp(mode, FS_MODE_VALUES[FS_MODE_READWRITE]) == 0 ? FS_MODE_READWRITE : 0);
            JS_FreeCString(ctx, mode);
        }
        JS_FreeValue(ctx, mode_v);
    }

    if (hdr->stage == FSA_STATE) {
        PermissionDescriptor d = fsa_descriptor(s);

        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        /* §2.2's query file system permission step 5, which is also the request algorithm's step 1-2: "desc's
           permission state". Over a state nothing has decided this FORKS three ways, and all three reach code
           a bundle ships — the read path, the prompt path and the fallback. */
        rc = permission_state_run(ctx, hdr, &s->ua_phase, &d, &state);
        if (rc) return rc;
        /* §2.3.1's queryPermission is those five steps and nothing else. */
        if (magic == M_QUERY || state != PERMISSION_PROMPT) {
            s->value = JS_NewString(ctx, permission_state_str(state));
            hdr->stage = FSA_SETTLE;
            goto settle;
        }
        DCHECK(magic == M_REQUEST, "§2.2's request algorithm was reached from a member that is not "
                                   "requestPermission — the magic IS the member");
        hdr->stage = FSA_ACTIVE;
    }

    if (hdr->stage == FSA_ACTIVE) {
        /* NEITHER THIS STAGE NOR THE REQUEST BELOW IS A CALL-RESUME, so an incoming result belongs to nothing
           here; the settle has its own stage and a re-entry into it skips both. */
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        /* STEPS 3-5. "settings" is desc["handle"]'s relevant settings object, and a handle carries no realm of
           its own — it is an ordinary platform object, so its relevant settings object is the one of the realm
           it was created in, which is this one. STEP 5's "global is not a Window" is false for every global
           this engine has: there is no WorkerGlobalScope here, which is also why the whole partial interface's
           `[Exposed=(Window,Worker)]` reduces to Window. The DCHECK is what will say so if that changes. */
        DCHECK(window_proxy_is(document_window_proxy(ctx)),
               "§2.2's request algorithm step 5 was reached in a realm whose global is not a Window — every "
               "global this agent builds is one, and a WorkerGlobalScope reaching here must throw a "
               "SecurityError at this step rather than continue into step 6");
        /* STEP 6 — the ACTIVATION GATE, and the second of this algorithm's three forks. */
        rc = user_activation_transient_run(ctx, hdr, &s->ua_phase, &ok);
        if (rc) return rc;
        if (!ok) {
            s->value = fsa_security_error(ctx, "requestPermission() requires transient user activation");
            goto reject;
        }
        /* STEP 7, asked after step 6 because that is the order the algorithm states them in. */
        if (!window_proxy_same_origin_with_top(ctx)) {
            s->value = fsa_security_error(ctx, "requestPermission() was called in a document that is not same "
                                               "origin with its top-level document");
            goto reject;
        }
        hdr->stage = FSA_REQUEST;
    }

    DCHECK(hdr->stage == FSA_REQUEST || hdr->stage == FSA_SETTLE,
           "a FileSystemHandle permission member resumed into a stage §2.2 does not have");
    if (hdr->stage == FSA_REQUEST) {
        PermissionDescriptor d = fsa_descriptor(s);

        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        /* STEP 8 — Permissions §5.2, whose own step 3 asks the user for express permission and whose step 7
           writes what it learns into §3.2's store. */
        rc = permission_request_run(ctx, hdr, &s->ua_phase, &d, &state);
        if (rc) return rc;
        DCHECK(state == PERMISSION_GRANTED || state == PERMISSION_DENIED,
               "§5.2 answered a file-system request with neither granted nor denied");
        /* STEP 9 and the algorithm's step 7 — "Return desc's permission state", read again rather than assumed
           equal to what §5.2 returned, because §2.2's own constraints and §4's partial order both sit between
           the store entry and the answer. It cannot fork: §5.2's step 7 has written the entry §5.1 step 7
           reads back ahead of the unknown, which is what the assert states. */
        {
            JSValue after = permission_state(ctx, &d);
            const char *str = JS_ToCString(ctx, after);

            DCHECK(str && permission_state_of(str) >= 0,
                   "§2.2's request algorithm step 9 read a permission state that is still UNKNOWN — Permissions "
                   "§5.2 step 7 has just written a store entry for this very descriptor, and §5.1 step 7 "
                   "returns an entry ahead of step 8's source, so an unknown here means the write and the read "
                   "disagree about which descriptor they name");
            if (str) JS_FreeCString(ctx, str);
            s->value = after;
        }
        hdr->stage = FSA_SETTLE;
    }
    goto settle;

reject:
    s->reject = 1;
    hdr->stage = FSA_SETTLE;

settle:
    DCHECK(hdr->stage == FSA_SETTLE, "a FileSystemHandle permission member settled from a stage that is not the "
                                     "settle");
    {
        JSValue settled = JS_UNDEFINED;

        rc = step_call_run(ctx, &s->cphase, STEP_CB(s->cb), s->funcs[s->reject], JS_UNDEFINED, 1,
                           (JSValueConst *)&s->value, cb_result, &settled, out_cb, out_argc);
        if (rc > 0) return rc;    /* parked ON THE SETTLE; the `then` read runs with a flow base under it */
        JS_FreeValue(ctx, settled);
    }
    *presult = s->promise;
    s->promise = JS_UNDEFINED;
    return JS_STEP_DONE;
}

static const IdlStepDecl FSA_DECL = {
    /* No release. This WAS fsa_visit's list a second time, ending in `started = 0` — which lowered the very
       condition the visit reads, so the discharge that runs next walked away from a state holding all of it.
       The declaration is the one list; the teardown reads it after the completion is stated. */
    fsa_step, sizeof(FsAccessState), fsa_visit, NULL,
    "File System Access §2.3 the FileSystemHandle permission members", FSA_STEPS
};

/* §2.3's `dictionary FileSystemHandlePermissionDescriptor { FileSystemPermissionMode mode = "read"; }` — one
   member, whose TYPE is the enumeration and whose DEFAULT is the enumeration's first value. Both halves are
   the declaration's, so the body reads a value that is already one of the two. */
static const IdlDictMember FSA_PERM_OPTIONS[] = {
    { "mode", IDL_ENUM, false, FS_MODE_VALUES, 0, NULL, IDL_DEFAULT_STRING, "read" }
};

static void fs_access_install_realm(JSContext *ctx)
{
    JSValue proto = fs_handle_proto(ctx);

    /* §2.3's partial is `[SecureContext]` like the interface it extends, and core/file/file_system_handle.c
       installs NOTHING in a non-secure realm — so there is no prototype here to extend and the two members are
       absent with the rest of the interface, which is what Web IDL §3.3.13 asks for. */
    if (JS_IsNull(proto)) {
        JS_FreeValue(ctx, proto);
        return;
    }
    DCHECK(g_id_query >= 0 && g_id_request >= 0,
           "§2.3's members were installed on a realm's prototype before file_system_access_init declared them");
    idl_install_method_exposed(ctx, proto, "queryPermission", 0, g_id_query, IDL_SECURE_CONTEXT);
    idl_install_method_exposed(ctx, proto, "requestPermission", 0, g_id_request, IDL_SECURE_CONTEXT);
    JS_FreeValue(ctx, proto);
}

void file_system_access_init(JSContext *ctx)
{
    static const IdlArgType OPTS_ONLY[] = { IDL_DICT };

    DCHECK(g_feature < 0, "file_system_access_init ran twice — §4's registry entry and the two members' pool "
                          "ids are the AGENT's");
    g_feature = permission_feature_of("file-system");
    CHECK(g_feature >= 0, "Permissions §4's registry has no row for the \"file-system\" powerful feature — File "
                          "System Access §2.2 registers it, and this component's two members and its store "
                          "entries are all indexed by that row");
    DCHECK(permission_feature_subject(g_feature) != NULL,
           "§4's registry row for \"file-system\" names no SUBJECT member — the descriptor type declares "
           "`required FileSystemHandle handle`, and a row without it would let §6.2.1 build a descriptor with "
           "no handle for the constraints to be written over");
#if APICLIENT_DEV
    {
        const char *const *values = permission_feature_aspect_values(g_feature);

        DCHECK(values && !strcmp(values[0], FS_MODE_VALUES[0]) && !strcmp(values[1], FS_MODE_VALUES[1]),
               "§4's registry row for \"file-system\" and this file state `FileSystemPermissionMode`'s values "
               "differently — they are ONE enumeration written in two places, and the ORDER is what decides "
               "which value sets the aspect bit");
    }
#endif
    permission_subject_declare(g_feature, fsa_subject_is);
    permission_constraints_declare(g_feature, fsa_constraints);

    g_id_query = idl_method_id_step(ctx, OPTS_ONLY, 1, FSA_PERM_OPTIONS, 1, &FSA_DECL, M_QUERY);
    idl_optional_from(0);
    g_id_request = idl_method_id_step(ctx, OPTS_ONLY, 1, FSA_PERM_OPTIONS, 1, &FSA_DECL, M_REQUEST);
    idl_optional_from(0);
    agent_state_id("file_system_access", &g_feature,
                   "§4's registry row for the \"file-system\" powerful feature, and the declaration latch");
    agent_state_id("file_system_access", &g_id_query, "§2.3's queryPermission machine");
    agent_state_id("file_system_access", &g_id_request, "§2.3's requestPermission machine");
    realm_declare_intrinsic(fs_access_install_realm);
}

void file_system_access_free(void)
{
    g_feature = -1;
    g_id_query = g_id_request = -1;
}
