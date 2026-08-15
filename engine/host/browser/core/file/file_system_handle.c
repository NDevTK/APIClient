/* FileSystemHandle, FileSystemFileHandle and FileSystemDirectoryHandle — File System Standard §2.2, §2.3, §2.4.
 *
 * A HANDLE IS A LOCATOR AND NOTHING ELSE. §2.2: "A FileSystemHandle object is associated with a locator", and
 * §2.1's locator is a kind, a root and a path — three facts fixed at the mint and never written again. So the
 * handle's own state is an IMMUTABLE C record behind a class opaque, the shape core/file/blob.c already uses
 * for a Blob's byte sequence, and not the internal-slot record the mutable half of this component uses: there
 * is no write for the per-flow COW delta to capture, because §2.2 has no step that changes a locator. What
 * DOES time-travel is the ENTRY the locator names, which is core/file/file_system.c's, and the two are
 * deliberately different mechanisms for that reason. "Multiple FileSystemHandle objects can have the same file
 * system locator" is then free, and `isSameEntry` compares LOCATORS rather than object identity because the
 * standard says to.
 *
 * EVERY METHOD IS A STEP MACHINE, and every one of them for the same two reasons. Each returns a PROMISE, and
 * settling one calls a resolving function — 27.2.1.3.2 step 8 reads `then` off whatever it is resolved with,
 * which is the page's code and therefore a request rather than a call from C. And `createWritable` runs §2.5's
 * stream creation, which suspends on its own start promise. A member that ran either from a C activation would
 * be the drive-to-completion this engine aborts on.
 *
 * THEY ARE ONE MACHINE WITH A MAGIC. The seven of them differ in the ALGORITHM and agree in everything around
 * it: locate the entry, decide a completion, settle the promise. Written as seven machines that shape would be
 * copied seven times and the settle would be seven chances to drop a rejection.
 *
 * §2.4.1's DIRECTORY ITERATION is the same machine one layer down. `async_iterable<USVString,
 * FileSystemHandle>` is Web IDL §3.7.10's declaration, whose whole binding — `entries`, `keys`, `values`,
 * %Symbol.asyncIterator%, the iterator object and its `next` — is core/idl_async_iter.c's, and all this file
 * supplies is §2.5.10's two hooks: the initialization steps that give the iterator its past-results set, and
 * "get the next iteration result", which is a step machine for the same reason every member above is.
 *
 * WHAT IS ABSENT AND WHY. §2.6's FileSystemSyncAccessHandle is
 * `[Exposed=DedicatedWorker]` and this engine has no WorkerGlobalScope, so its exposure set is empty here. File
 * System Access §2.3's `queryPermission`/`requestPermission` are a PARTIAL interface of this one and live in
 * core/file/file_system_access.c beside the "file-system" powerful feature they are the two doors onto; they
 * install onto the prototype this file builds, through fs_handle_proto. */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/idl_args.h"
#include "core/idl_async_iter.h"
#include "core/realm.h"
#include "core/frame/secure_context.h"
#include "core/file/blob.h"
#include "core/file/file_system.h"
#include "core/file/file_system_handle.h"
#include "core/file/file_system_writable.h"
#include "core/streams/stream_work.h"
#include "solver/concolic.h"

/* §2.1's FILE SYSTEM LOCATOR, as the C record a handle IS. */
typedef struct {
    bool   directory;   /* the locator's KIND: §2.2's FileSystemHandleKind, two values */
    char  *root;        /* the locator's ROOT — "an opaque string whose value is implementation-defined" */
    char **path;        /* the locator's PATH — "a list of one or more strings" */
    int    npath;
} FsLocator;

static JSClassID g_handle_class;   /* the opaque, and §2.2's brand — a File handle and a Directory handle wear
                                      it both, exactly as §4's File wears the Blob class */
static JSClassID g_file_handle_class, g_dir_handle_class;   /* the two derived per-realm PROTOTYPE slots */
static int g_id_is_same_entry = -1, g_id_get_file = -1, g_id_create_writable = -1;
static int g_id_get_file_handle = -1, g_id_get_directory_handle = -1, g_id_remove_entry = -1;
static int g_id_resolve = -1;

/* ---- the locator -------------------------------------------------------------------------------------- */

static void fsh_finalizer(JSRuntime *rt, JSValue val)
{
    FsLocator *l = JS_GetOpaque(val, g_handle_class);
    int i;

    (void)rt;
    if (!l) return;
    for (i = 0; i < l->npath; i++) free(l->path[i]);
    free(l->path);
    free(l->root);
    free(l);
}

bool fs_handle_is(JSValueConst v)
{
    return g_handle_class != 0 && JS_GetOpaque(v, g_handle_class) != NULL;
}

static FsLocator *fsh_locator(JSValueConst v)
{
    return g_handle_class ? JS_GetOpaque(v, g_handle_class) : NULL;
}

bool fs_handle_locator(JSValueConst v, bool *directory, const char **root, const char *const **path, int *npath)
{
    FsLocator *l = fsh_locator(v);

    if (!l) return false;
    *directory = l->directory;
    *root = l->root;
    *path = (const char *const *)l->path;
    *npath = l->npath;
    DCHECK(*npath >= 1, "a handle's locator holds a path with no items — §2.1's path is a list of ONE OR "
                        "MORE strings, and fs_handle_new asserts that at the mint");
    return true;
}

JSValue fs_handle_proto(JSContext *ctx)
{
    DCHECK(g_handle_class != 0, "§2.2's prototype was asked for before fs_handle_init declared the class");
    return JS_GetClassProto(ctx, g_handle_class);
}

JSValue fs_handle_new(JSContext *ctx, bool directory, const char *root, const char *const *path, int npath)
{
    JSValue proto = JS_GetClassProto(ctx, directory ? g_dir_handle_class : g_file_handle_class);
    JSValue obj;
    FsLocator *l;
    int i;

    DCHECK(npath >= 1, "a FileSystemHandle was minted with a path of no items — §2.1's file system path is \"a "
                       "list of ONE OR MORE strings\", and `handle.name` is its last item");
    DCHECK(!JS_IsNull(proto), "a FileSystemHandle was minted in a realm with no FileSystemHandle prototype — "
                              "all three interfaces are [SecureContext], so a NON-SECURE realm has none, and "
                              "every door onto a handle carries that same attribute; a mint here means a door "
                              "was built without it");
    obj = JS_NewObjectProtoClass(ctx, proto, g_handle_class);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj)) return obj;
    /* THE RECORD IS COMPLETE BEFORE IT IS ATTACHED, so a failure on the way leaves nothing half-owned: every
       string is copied here and the finalizer frees exactly this list. */
    l = calloc(1, sizeof *l);
    CHECK(l != NULL, "file system handle: OOM building a locator");
    l->directory = directory;
    l->root = strdup(root);
    l->npath = npath;
    l->path = calloc((size_t)npath, sizeof *l->path);
    CHECK(l->root && l->path, "file system handle: OOM building a locator's path");
    for (i = 0; i < npath; i++) {
        l->path[i] = strdup(path[i]);
        CHECK(l->path[i] != NULL, "file system handle: OOM copying a locator's path component");
    }
    JS_SetOpaque(obj, l);
    return obj;
}

/* §2.3 and §2.4's "CREATE A CHILD FileSystemFileHandle / FileSystemDirectoryHandle given a directory locator
   parentLocator and a string name": childRoot is a copy of the parent's root, and childPath is the parent's
   path cloned with `name` appended. OWNED. */
static JSValue fsh_child(JSContext *ctx, const FsLocator *parent, const char *name, bool directory)
{
    const char **path;
    JSValue h;
    int i;

    DCHECK(parent->directory, "§2.3's create a child handle was given a locator that is not a DIRECTORY LOCATOR "
                              "— a child's path extends a directory's, and a file has no children");
    path = malloc(sizeof *path * (size_t)(parent->npath + 1));
    CHECK(path != NULL, "file system handle: OOM building a child locator's path");
    for (i = 0; i < parent->npath; i++) path[i] = parent->path[i];
    path[parent->npath] = name;
    h = fs_handle_new(ctx, directory, parent->root, path, parent->npath + 1);
    free(path);
    return h;
}

/* §2.1's LOCATE AN ENTRY for a handle. OWNED, or JS_NULL. */
static JSValue fsh_locate(JSContext *ctx, const FsLocator *l)
{
    return file_system_locate(ctx, l->root, (const char *const *)l->path, l->npath);
}

/* §2.1's SAME LOCATOR: "a's kind is b's kind, a's root is b's root, and a's path is the same path as b's". */
static bool fsh_same_locator(const FsLocator *a, const FsLocator *b)
{
    int i;

    if (a->directory != b->directory) return false;
    if (strcmp(a->root, b->root)) return false;
    if (a->npath != b->npath) return false;
    for (i = 0; i < a->npath; i++)
        if (strcmp(a->path[i], b->path[i])) return false;
    return true;
}

/* ---- §2.2's two attributes ------------------------------------------------------------------------------ */

/* WEB IDL §3.7.5's BRAND CHECK, which for these two is the opaque: `FileSystemHandle.prototype.kind` read off a
   plain object is a TypeError and a page tells that apart from `undefined`. */
static FsLocator *fsh_brand(JSContext *ctx, JSValueConst this_val)
{
    FsLocator *l = fsh_locator(this_val);

    if (l) return l;
    JS_ThrowTypeError(ctx, "a FileSystemHandle member was reached on something that is not a FileSystemHandle");
    return NULL;
}

/* "The kind getter steps are to return this's locator's kind." */
static JSValue fsh_get_kind(JSContext *ctx, JSValueConst this_val, int magic)
{
    FsLocator *l = fsh_brand(ctx, this_val);

    (void)magic;
    if (!l) return JS_EXCEPTION;
    return JS_NewString(ctx, l->directory ? "directory" : "file");
}

/* "The name getter steps are to return the last item (a string) of this's locator's path." */
static JSValue fsh_get_name(JSContext *ctx, JSValueConst this_val, int magic)
{
    FsLocator *l = fsh_brand(ctx, this_val);

    (void)magic;
    if (!l) return JS_EXCEPTION;
    return JS_NewString(ctx, l->path[l->npath - 1]);
}

/* ---- the one machine every promise-returning member is -------------------------------------------------- */

#define FSH_STAGES(X) \
    X(FSH_RUN,    "File System §2.3-§2.4 the enqueued file-system-queue steps of this member (locate the " \
                  "entry, and everything the algorithm decides from it)") \
    X(FSH_STREAM, "File System §2.3.2 createWritable() step 3.5.2 (create a new FileSystemWritableFileStream " \
                  "for entry — which suspends on §5.4's start promise)") \
    X(FSH_SETTLE, "File System §2.3-§2.4 the queued storage task of this member (resolve or reject result — " \
                  "27.2.1.3.2 step 8's `then` read is the page's)")
enum { IDL_STEP_STAGE_BASE(FSH_STAGES) FSH_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const FSH_STEPS[] = { FSH_STAGES(JS_STEP_STAGE_LABEL) NULL };

enum {
    M_IS_SAME_ENTRY = 0, M_GET_FILE, M_CREATE_WRITABLE,
    M_GET_FILE_HANDLE, M_GET_DIRECTORY_HANDLE, M_REMOVE_ENTRY, M_RESOLVE
};

typedef struct {
    /* ONE StreamWork serves the settle CALL and §2.5's start sub-sequence, because the two are in different
       STAGES and neither is ever in flight while the other is. */
    StreamWork w;
    JSValue promise;   /* the capability's promise — this member's result (owned) */
    JSValue func;      /* its resolve or its reject, whichever this member settles with (owned) */
    JSValue value;     /* what it settles WITH (owned) */
    JSValue entry;     /* createWritable's file entry, held across the stream's creation (owned) */
    JSValue stream;    /* and the stream it is building (owned) */
} FsHandleState;

static void fsh_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    FsHandleState *s = st;

    stream_work_visit(ctx, &s->w, v);
    v->val(ctx, &s->promise);
    v->val(ctx, &s->func);
    v->val(ctx, &s->value);
    v->val(ctx, &s->entry);
    v->val(ctx, &s->stream);
}

/* THE REJECTION VALUE A DOMException IS, without throwing out of the member: every one of §2.3 and §2.4's
   failures REJECTS a promise the member has already returned, so the exception is a VALUE here and not a
   completion. Returns it (owned). */
static JSValue fsh_dom_error(JSContext *ctx, const char *name, const char *msg)
{
    JSValue e;

    JS_ThrowDOMException(ctx, name, "%s", msg);
    e = JS_GetException(ctx);
    CHECK(!JS_IsUndefined(e), "file system handle: a DOMException was built and no exception was live");
    return e;
}

static JSValue fsh_type_error(JSContext *ctx, const char *msg)
{
    JSValue e;

    JS_ThrowTypeError(ctx, "%s", msg);
    e = JS_GetException(ctx);
    CHECK(!JS_IsUndefined(e), "file system handle: a TypeError was built and no exception was live");
    return e;
}

/* §2.1's RESOLVE, which §2.4.5 is one call into: the relative path from `root` to `child`, or null. OWNED. */
static JSValue fsh_resolve_path(JSContext *ctx, const FsLocator *root, const FsLocator *child)
{
    JSValue out;
    int i;

    if (strcmp(child->root, root->root)) return JS_NULL;       /* "if child's root is not root's root" */
    if (root->npath > child->npath) return JS_NULL;
    for (i = 0; i < root->npath; i++)
        if (strcmp(root->path[i], child->path[i])) return JS_NULL;
    out = JS_NewArray(ctx);
    CHECK(!JS_IsException(out), "file system handle: §2.1's resolve could not allocate its relative path");
    /* "For each index of the range from rootPath's size to childPath's size, exclusive, append
       childPath.[[index]] to relativePath" — which for the same path is the empty list §2.4.5 promises. */
    for (i = root->npath; i < child->npath; i++)
        JS_DefinePropertyValueUint32(ctx, out, (uint32_t)(i - root->npath),
                                     JS_NewString(ctx, child->path[i]), JS_PROP_C_W_E);
    return out;
}

/* §2.4.2 and §2.4.3's shared body — they differ in the KIND they look for and the kind they create, and agree
   in every step around it. `*preject` takes the rejection value when there is one. OWNED result, or
   JS_UNINITIALIZED when the member rejects. */
static JSValue fsh_get_child_handle(JSContext *ctx, const FsLocator *l, JSValueConst dir, const char *name,
                                    bool want_directory, bool create, JSValue *preject)
{
    JSValue child = file_system_child(ctx, dir, name), h;

    if (!JS_IsNull(child)) {
        bool is_dir = file_system_is_directory(child);

        JS_FreeValue(ctx, child);
        /* "If child is a directory entry [when a file was asked for]: reject result with a TypeMismatchError." */
        if (is_dir != want_directory) {
            *preject = fsh_dom_error(ctx, "TypeMismatchError",
                                     want_directory ? "a file with that name already exists"
                                                    : "a directory with that name already exists");
            return JS_UNINITIALIZED;
        }
        return fsh_child(ctx, l, name, want_directory);
    }
    if (!create) {                                              /* "If options[\"create\"] is false" */
        *preject = fsh_dom_error(ctx, "NotFoundError", "no such entry");
        return JS_UNINITIALIZED;
    }
    child = want_directory ? file_system_create_directory(ctx, dir, name)
                           : file_system_create_file(ctx, dir, name);
    JS_FreeValue(ctx, child);
    h = fsh_child(ctx, l, name, want_directory);
    return h;
}

static int fsh_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                    JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    FsHandleState *s = st;
    int magic = idl_step_magic(hdr);
    int reject = 0, r;

    *presult = JS_UNDEFINED;

    if (hdr->stage == FSH_RUN) {
        FsLocator *l = fsh_locator(hdr->this_val);
        JSValue entry, funcs[2];
        const char *name = NULL;

        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        /* EVERY OWNED FIELD IN PLACE BEFORE THE FIRST THING THAT CAN THROW — the failure path tears this state
           down through fsh_visit, which names exactly what the state owns and nothing else. */
        stream_work_start(&s->w);
        s->promise = s->func = s->value = s->entry = s->stream = JS_UNDEFINED;
        if (!l) {
            JS_ThrowTypeError(ctx, "a FileSystemHandle method was called on something that is not a "
                                   "FileSystemHandle");
            return -1;
        }
        /* THE CAPABILITY IS BUILT BEFORE THE ALGORITHM RUNS, because every one of these members says "let
           result be a new promise" as its first step and then REJECTS rather than throwing. The native
           %Promise% is used with no subclass in sight, so building it constructs nothing of the page's. */
        s->promise = JS_NewPromiseCapability(ctx, funcs);
        if (JS_IsException(s->promise)) return -1;
        entry = fsh_locate(ctx, l);

        switch (magic) {
        case M_IS_SAME_ENTRY: {
            /* §2.2.1: "If this's locator is the same locator as other's locator, resolve p with true.
               Otherwise resolve p with false." The DECLARATION has already brand-tested the argument. */
            FsLocator *o = fsh_locator(argc > 0 ? argv[0] : JS_UNDEFINED);

            DCHECK(o != NULL, "§2.2.1's `other` reached the body as something that is not a FileSystemHandle — "
                              "the argument is declared IDL_INTERFACE and branded against this class, so the "
                              "conversion has already thrown for anything else");
            s->value = JS_NewBool(ctx, o && fsh_same_locator(l, o));
            break;
        }
        case M_RESOLVE: {
            /* §2.4.5: "return the result of resolving possibleDescendant's locator relative to this's
               locator" — and the resolve algorithm's own first step is the root comparison, so a handle from a
               different file system answers null rather than rejecting. */
            FsLocator *o = fsh_locator(argc > 0 ? argv[0] : JS_UNDEFINED);

            DCHECK(o != NULL, "§2.4.5's `possibleDescendant` reached the body as something that is not a "
                              "FileSystemHandle");
            s->value = o ? fsh_resolve_path(ctx, l, o) : JS_NULL;
            break;
        }
        case M_GET_FILE:
            /* §2.3.1: "If entry is null, reject result with a NotFoundError ... Assert: entry is a file
               entry." A page can delete a file through one handle and read it through another, which is
               exactly the state this rejection is for. */
            if (JS_IsNull(entry)) {
                s->value = fsh_dom_error(ctx, "NotFoundError", "the file this handle names no longer exists");
                reject = 1;
                break;
            }
            DCHECK(file_system_is_file(entry),
                   "§2.3.1's getFile() located a DIRECTORY entry — a FileSystemFileHandle's locator's kind is "
                   "\"file\", and §2.1's locate an entry answers a file entry or null for one");
            s->value = file_system_file_new(ctx, entry);
            break;
        case M_CREATE_WRITABLE:
            if (JS_IsNull(entry)) {
                s->value = fsh_dom_error(ctx, "NotFoundError", "the file this handle names no longer exists");
                reject = 1;
                break;
            }
            DCHECK(file_system_is_file(entry), "§2.3.2's createWritable() located a DIRECTORY entry");
            /* "Let lockResult be the result of taking a lock with 'shared' on entry ... If lockResult is
               'failure', reject result with a NoModificationAllowedError." The lock is what stops a
               FileSystemSyncAccessHandle being taken on a file a stream is writing. */
            if (!file_system_take_lock(ctx, entry, /*exclusive*/ false)) {
                s->value = fsh_dom_error(ctx, "NoModificationAllowedError",
                                         "the file is already locked exclusively");
                reject = 1;
                break;
            }
            s->entry = JS_DupValue(ctx, entry);
            STEP_GOTO(hdr->stage, FSH_STREAM, &s->w.phase, NULL);
            break;
        case M_GET_FILE_HANDLE:
        case M_GET_DIRECTORY_HANDLE:
        case M_REMOVE_ENTRY: {
            JSValue dict = argc > 1 ? argv[1] : JS_UNDEFINED;
            const char *nm = JS_ToCString(ctx, argc > 0 ? argv[0] : JS_UNDEFINED);

            CHECK(nm != NULL, "file system handle: a member's name argument could not be read after its "
                              "declared USVString conversion");
            name = nm;
            /* STEP 1 OF ALL THREE: "If name is not a valid file name, queue a storage task with global to
               reject result with a TypeError." A TypeError, not a DOMException — the standard is explicit, and
               a page's `catch` branches on which. */
            if (!file_system_valid_name(name, strlen(name))) {
                s->value = fsh_type_error(ctx, "not a valid file name");
                reject = 1;
                break;
            }
            if (JS_IsNull(entry)) {
                s->value = fsh_dom_error(ctx, "NotFoundError", "this directory no longer exists");
                reject = 1;
                break;
            }
            DCHECK(file_system_is_directory(entry),
                   "a §2.4 member located a FILE entry — a FileSystemDirectoryHandle's locator's kind is "
                   "\"directory\", and §2.1's locate an entry answers a directory entry or null for one");
            if (magic == M_REMOVE_ENTRY) {
                JSValue child = file_system_child(ctx, entry, name);

                /* §2.4.4 rejects with a NotFoundError when nothing of that name is there — the prose above the
                   algorithm says deleting what does not exist "is considered success", and the ALGORITHM says
                   otherwise; the algorithm is normative and browsers follow it. */
                if (JS_IsNull(child)) {
                    s->value = fsh_dom_error(ctx, "NotFoundError", "no such entry to remove");
                    reject = 1;
                    break;
                }
                /* "If child's children is not empty and options['recursive'] is false, reject result with an
                   InvalidModificationError." */
                if (file_system_is_directory(child) && !file_system_children_empty(ctx, child) &&
                    !idl_dict_bool(ctx, dict, "recursive")) {
                    JS_FreeValue(ctx, child);
                    s->value = fsh_dom_error(ctx, "InvalidModificationError",
                                             "the directory is not empty and `recursive` was not set");
                    reject = 1;
                    break;
                }
                JS_FreeValue(ctx, child);
                file_system_remove_child(ctx, entry, name);
                s->value = JS_UNDEFINED;                       /* "Resolve result with undefined" */
                break;
            }
            {
                JSValue why = JS_UNDEFINED;
                JSValue h = fsh_get_child_handle(ctx, l, entry, name, magic == M_GET_DIRECTORY_HANDLE,
                                                 idl_dict_bool(ctx, dict, "create"), &why);

                /* THE REJECTION HAS ITS OWN SLOT, because a member either resolves with a handle or rejects
                   with a reason and one variable for both would have the helper write the reason into the slot
                   whose return value then overwrites it. */
                if (JS_IsUninitialized(h)) {
                    s->value = why;
                    reject = 1;
                } else {
                    s->value = h;
                    JS_FreeValue(ctx, why);
                }
            }
            break;
        }
        default:
            DFAIL("a FileSystemHandle member ran with a magic no member of this file declares — the magic IS "
                  "the member, so an unknown one means a name was installed without a case to answer it");
            break;
        }
        JS_FreeCString(ctx, name);
        JS_FreeValue(ctx, entry);
        s->func = funcs[reject];
        JS_FreeValue(ctx, funcs[reject ^ 1]);
        if (hdr->stage == FSH_RUN) STEP_GOTO(hdr->stage, FSH_SETTLE, &s->w.phase, NULL);
    }

    if (hdr->stage == FSH_STREAM) {
        /* §2.3.2 step 3.5.2, and §2.5's creation is a sub-sequence because "set up stream" resolves a start
           promise. `keepExistingData` makes the stream's [[buffer]] a copy of the entry's binary data. */
        r = fs_writable_new_run(ctx, &s->w, s->entry, idl_dict_bool(ctx, argc > 0 ? argv[0] : JS_UNDEFINED,
                                                                    "keepExistingData"),
                                cb_result, &s->stream, out_cb, out_argc);
        if (r > 0) return r;
        if (r < 0) return -1;
        cb_result = JS_UNDEFINED;
        JS_FreeValue(ctx, s->value);
        s->value = s->stream;
        s->stream = JS_UNDEFINED;
        STEP_GOTO(hdr->stage, FSH_SETTLE, &s->w.phase, NULL);
    }

    DCHECK(hdr->stage == FSH_SETTLE, "a FileSystemHandle member resumed into a stage §2.3-§2.4 does not have");
    {
        JSValue settled = JS_UNDEFINED;

        r = step_call_run(ctx, &s->w.phase, STEP_CB(s->w.cb), s->func, JS_UNDEFINED, 1,
                          (JSValueConst *)&s->value, cb_result, &settled, out_cb, out_argc);
        if (r > 0) return r;       /* parked ON THE SETTLE; the `then` read runs with a flow base under it */
        JS_FreeValue(ctx, settled);   /* a resolving function's return value is undefined and unobservable */
    }
    *presult = s->promise;
    s->promise = JS_UNDEFINED;
    return JS_STEP_DONE;
}

static const IdlStepDecl FSH_DECL = {
    fsh_step, sizeof(FsHandleState), fsh_visit, NULL,
    "File System §2.2-§2.4 the FileSystemHandle members", FSH_STEPS
};

/* ---- §2.4.1's DIRECTORY ITERATION ---------------------------------------------------------------------------
 *
 * `async_iterable<USVString, FileSystemHandle>`. Web IDL §3.7.10 owns the whole JavaScript binding of that
 * declaration — `entries`, `keys`, `values`, %Symbol.asyncIterator%, the default asynchronous iterator object
 * and its `next` — and core/idl_async_iter.c is where it is written once. §2.5.10 requires the PROSE to define
 * two things, and these are them.
 *
 * THE ITERATION IS STATEFUL AND THE STATE IS PER-ITERATOR. §2.4.1 does not walk an index: it takes "a file
 * system entry in directory's children, such that child's name is not contained in iterator's past results",
 * which is why a directory mutated mid-iteration is explicitly allowed to yield a different set ("Entries that
 * are created or deleted while the iteration is in progress might or might not be included. No guarantees are
 * given either way") and why the children are re-read at every step rather than snapshotted.
 *
 * THE PAST RESULTS ARE A JS OBJECT, never a malloc'd C list: a flow's appends are then property writes the COW
 * delta already captures, so one flow's iteration cannot be seen by its sibling, and the set parks to the cold
 * tier with the flow that owns it. Its [[Prototype]] is NULL, so a child named "__proto__" or "toString" is an
 * ordinary key and membership is exactly own-property presence. */

static int g_dir_iter_handle = -1;

/* Web IDL §3.7.10 step 3.1.3's "implements definition", where the DEFINITION is FileSystemDirectoryHandle and
   not FileSystemHandle: `FileSystemDirectoryHandle.prototype.values.call(fileHandle)` must be a TypeError, and
   §2.2's brand is ONE class that both interfaces wear — so what tells them apart is the LOCATOR'S KIND, which
   is what §2.4's own note says it is ("A FileSystemDirectoryHandle's associated locator's kind is
   'directory'"). */
static bool fsdir_is(JSValueConst v)
{
    const FsLocator *l = fsh_locator(v);

    return l != NULL && l->directory;
}

/* §2.4.1: "The asynchronous iterator initialization steps for a FileSystemDirectoryHandle handle and its async
   iterator iterator are: Set iterator's past results to an empty set." */
static int fsdir_iter_init(JSContext *ctx, JSStepHdr *hdr, void *work, JSValueConst target, JSValueConst iter,
                           int argc, JSValueConst *argv, JSValue *pstate, JSValue in,
                           JSValue **out_cb, int *out_argc)
{
    JSValue set = JS_NewObjectProto(ctx, JS_NULL);

    (void)hdr; (void)work; (void)target; (void)iter; (void)argc; (void)argv;
    (void)out_cb; (void)out_argc;
    /* These steps are one assignment and make no request, so they never park — and the answer slot every step
       is handed is discharged here rather than left for a caller that has already given it away. */
    JS_FreeValue(ctx, in);
    if (JS_IsException(set)) return -1;
    DCHECK(JS_IsUndefined(*pstate), "§2.4.1's initialization steps ran on an iterator that already had state — "
                                    "Web IDL §3.7.10 step 3.1.6 runs them exactly once, at the mint");
    *pstate = set;
    return 0;
}

/* §2.4.1 steps 2.3.4-2.3.8: the first child whose name `past` does not already contain, appended to `past` and
   turned into a child handle. Answers the two-element value pair a PAIR declaration resolves with — « child's
   name, result » — or §2.5.10's end of iteration when there is no such child. OWNED, or JS_EXCEPTION. */
static JSValue fsdir_iter_pick(JSContext *ctx, const FsLocator *l, JSValueConst dir, JSValueConst past)
{
    int n = file_system_child_count(ctx, dir), i;

    DCHECK(JS_IsObject(past), "§2.4.1's iteration reached its steps with no past results set — Web IDL §3.7.10 "
                              "step 3.1.6 runs the initialization steps that create it before the iterator is "
                              "ever handed out");
    for (i = 0; i < n; i++) {
        JSValue name = JS_UNDEFINED;
        JSValue child = file_system_child_at(ctx, dir, i, &name);
        JSAtom a = JS_ValueToAtom(ctx, name);
        const char *nm;
        JSValue pair, handle;
        int seen;

        if (a == JS_ATOM_NULL) { JS_FreeValue(ctx, name); JS_FreeValue(ctx, child); return JS_EXCEPTION; }
        /* The set is this engine's own null-prototype object, so the membership test reaches no prototype and
           runs none of the page's code. */
        seen = JS_HasProperty(ctx, past, a);
        if (seen != 0) {
            JS_FreeAtom(ctx, a);
            JS_FreeValue(ctx, name);
            JS_FreeValue(ctx, child);
            if (seen < 0) return JS_EXCEPTION;
            continue;
        }
        /* step 2.3.6: "Append child's name to iterator's past results." */
        JS_DefinePropertyValue(ctx, past, a, JS_TRUE, JS_PROP_C_W_E);
        JS_FreeAtom(ctx, a);
        /* steps 2.3.7-2.3.8: a child FileSystemFileHandle for a file entry, a child FileSystemDirectoryHandle
           for a directory entry — which is fsh_child, the same operation §2.4.2 and §2.4.3 reach. */
        nm = file_system_name_cstr(ctx, child);
        CHECK(nm != NULL, "§2.4.1's iteration reached a child entry with no name");
        handle = fsh_child(ctx, l, nm, file_system_is_directory(child));
        JS_FreeCString(ctx, nm);
        JS_FreeValue(ctx, child);
        if (JS_IsException(handle)) { JS_FreeValue(ctx, name); return handle; }
        pair = JS_NewArray(ctx);
        if (JS_IsException(pair)) { JS_FreeValue(ctx, name); JS_FreeValue(ctx, handle); return pair; }
        /* Two defines and two checks, because each CONSUMES its value: folded into one `||` the handle is
           never handed over when the name's define fails, and leaks. */
        if (JS_DefinePropertyValueUint32(ctx, pair, 0, name, JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, handle);
            JS_FreeValue(ctx, pair);
            return JS_EXCEPTION;
        }
        if (JS_DefinePropertyValueUint32(ctx, pair, 1, handle, JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, pair);
            return JS_EXCEPTION;
        }
        return pair;
    }
    /* step 2.3.5: "If child is null, resolve promise with undefined and abort these steps" — which is Web IDL
       §2.5.10's END OF ITERATION rather than the JavaScript value `undefined`: the fulfilment of this promise
       is read by §3.7.10.2's fulfillSteps, and `undefined` there is a value a declaration may legitimately
       yield. The two say the same thing; only the marker outside the value space says it unambiguously. */
    return idl_async_iter_end(ctx);
}

/* The iteration's own step storage. ONE StreamWork, because the only sub-sequence §2.4.1 has is the settle.
   NO CURSOR BYTE. There was one — `started` — and it was this algorithm's STAGE kept where nothing could read
   it: the driver's assert cannot see a private byte, a park cannot report it, and a later build cannot resolve
   it back to a step. The stage below is the machine's own, joined onto Web IDL §3.7.10.2's at the declaration,
   and `w.phase` remains what it is — a sub-sequence cursor INSIDE that stage. */
typedef struct {
    StreamWork w;
    JSValue    promise;   /* §2.4.1 step 1's "let promise be a new promise" (owned) */
} FsDirIterWork;

/* WHERE §2.4.1's ITERATION RESTS — at its SETTLE, which is a Call of a resolving function and therefore reaches
   27.2.1.3.2 step 8's `then` read on the value pair it is handed (an Array, one
   `Object.defineProperty(Array.prototype, "then", …)` away from being the page's code). Numbered from
   IDL_ASYNC_ITER_STEP_FIRST and joined onto Web IDL §3.7.10.2's own stages at the declaration, so a flow parked
   there reports the File System Standard's step rather than the Web IDL step that RUNS this algorithm. */
#define FSD_STAGES(X) \
    X(FSD_NEXT_SETTLE, \
      "File System §2.4.1 get the next iteration result steps 2.3.2, 2.3.5 and 2.3.8 (rejecting promise with a " \
      "NotFoundError DOMException, or resolving it with end of iteration or with « child's name, result »)")
enum { IDL_ASYNC_ITER_STAGE_BASE(FSD_STAGES) FSD_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const FSD_STEPS[] = { FSD_STAGES(JS_STEP_STAGE_LABEL) NULL };

static void fsdir_iter_visit(JSContext *ctx, void *work, JSStepVisit *v)
{
    FsDirIterWork *k = work;

    stream_work_visit(ctx, &k->w, v);
    v->val(ctx, &k->promise);
}

/* §2.4.1's "To get the next iteration result for a FileSystemDirectoryHandle handle and its async iterator
   iterator". A STEP because its last act settles a promise, and a resolving function reaches 27.2.1.3.2 step
   8's `then` read on what it is given — which for a value pair is an Array, one
   `Object.defineProperty(Array.prototype, "then", …)` away from being the page's code. */
static int fsdir_iter_next(JSContext *ctx, JSStepHdr *hdr, void *work, JSValueConst target, JSValueConst iter,
                           JSValue *pstate, JSValue in, JSValue *ppromise, JSValue **out_cb, int *out_argc)
{
    FsDirIterWork *k = work;
    JSValue settled = JS_UNDEFINED;
    int r;

    (void)iter;
    /* A STAGE BELOW THIS ALGORITHM'S FIRST IS §3.7.10.2's — its next step 8.4, the stage that RUNS these
       steps, which is what a first entry looks like. */
    if (hdr->stage < IDL_ASYNC_ITER_STEP_FIRST) {
        const FsLocator *l = fsh_locator(target);
        JSValue entry, funcs[2];
        int reject = 0;

        /* EVERY OWNED FIELD IN PLACE BEFORE THE FIRST THING THAT CAN THROW — the failure path tears the whole
           machine down through the ownership declaration, which frees exactly what it names. */
        stream_work_start(&k->w);
        k->promise = JS_UNDEFINED;
        DCHECK(l != NULL, "§2.4.1's iteration reached a target that is not a FileSystemHandle — the iterator's "
                          "target is the receiver Web IDL §3.7.10 step 3.1.3 has already brand-checked");
        k->promise = JS_NewPromiseCapability(ctx, funcs);   /* step 1: "Let promise be a new promise." */
        if (JS_IsException(k->promise)) return -1;
        entry = fsh_locate(ctx, l);                         /* step 2.1: locate an entry given handle's locator */
        if (JS_IsNull(entry)) {
            /* step 2.3.2: "If directory is null, reject result with a NotFoundError DOMException." A page can
               remove a directory through one handle while another is iterating it, which is this rejection. */
            k->w.value = fsh_dom_error(ctx, "NotFoundError", "this directory no longer exists");
            reject = 1;
        } else {
            DCHECK(file_system_is_directory(entry),
                   "§2.4.1's iteration located a FILE entry — a FileSystemDirectoryHandle's locator's kind is "
                   "\"directory\", and §2.1's locate an entry answers a directory entry or null for one");
            k->w.value = fsdir_iter_pick(ctx, l, entry, *pstate);
        }
        JS_FreeValue(ctx, entry);
        if (JS_IsException(k->w.value)) {
            k->w.value = JS_UNDEFINED;
            JS_FreeValue(ctx, funcs[0]);
            JS_FreeValue(ctx, funcs[1]);
            return -1;
        }
        k->w.func = funcs[reject];
        JS_FreeValue(ctx, funcs[reject ^ 1]);
        STEP_GOTO(hdr->stage, FSD_NEXT_SETTLE, &k->w.phase, NULL);
    }
    DCHECK(hdr->stage == FSD_NEXT_SETTLE,
           "§2.4.1's get the next iteration result resumed at a step it never rests at");
    r = step_call_run(ctx, &k->w.phase, STEP_CB(k->w.cb), k->w.func, JS_UNDEFINED, 1,
                      (JSValueConst *)&k->w.value, in, &settled, out_cb, out_argc);
    if (r > 0) return r;       /* parked ON THE SETTLE; the `then` read runs with a flow base under it */
    JS_FreeValue(ctx, settled);
    *ppromise = k->promise;    /* step 3: "Return promise." */
    k->promise = JS_UNDEFINED;
    return 0;
}

/* §2.4.1 defines NO asynchronous iterator return algorithm, so `ret` is NULL and Web IDL §3.7.10.2 gives the
   iterator prototype no `return` at all — which is observable: breaking out of a `for await` loop over a
   directory runs AsyncIteratorClose, whose GetMethod finds undefined and calls nothing. An iteration this
   component had to unwind (a lock, a cursor) would be where that algorithm goes; this one holds neither. */
/* DESIGNATED, and every declaration of this shape must be: the struct has gained fields twice, and a
   POSITIONAL initializer re-aims every value after the new one — silently wherever the two types happen to
   agree, which two adjacent pointer fields always do. Naming each one is what makes a field added tomorrow
   land nowhere rather than one slot early. */
static const IdlAsyncIterOps FS_DIR_ITER_OPS = {
    .iface = "FileSystemDirectoryHandle",
    .pair = true,           /* `async_iterable<USVString, FileSystemHandle>` — two type parameters */
    .implements = fsdir_is,
    .init = fsdir_iter_init,
    .next = fsdir_iter_next,
    .ret = NULL,
    /* §2.4.1's initialization steps are ONE assignment and make no request, so they rest at no step of their
       own and the stage that runs them is the only rest point there is; the iteration itself rests at one. */
    .init_steps = NULL,
    .steps = FSD_STEPS,
    .work_size = sizeof(FsDirIterWork),
    .work_visit = fsdir_iter_visit,
    /* the declaration takes no arguments: "In the future we might want to add arguments ... to support for
       example recursive iteration" is an open issue, not a member of the IDL this engine implements. */
    .arg_types = NULL, .nargs = 0, .members = NULL, .nmembers = 0
};

/* ---- declaration and per-realm install --------------------------------------------------------------------- */

/* §2.4's THREE DICTIONARIES. Each declares ONE boolean whose IDL writes `= false`, and an ABSENT member reads
   as false through idl_dict_bool — which is the same observable, and the only one either value has. */
static const IdlDictMember GET_FILE_OPTIONS[]  = { { "create", IDL_BOOLEAN, false, NULL, 0, NULL,
                                                     IDL_DEFAULT_NONE, NULL } };
static const IdlDictMember REMOVE_OPTIONS[]    = { { "recursive", IDL_BOOLEAN, false, NULL, 0, NULL,
                                                     IDL_DEFAULT_NONE, NULL } };
static const IdlDictMember WRITABLE_OPTIONS[]  = { { "keepExistingData", IDL_BOOLEAN, false, NULL, 0, NULL,
                                                     IDL_DEFAULT_NONE, NULL } };

static void fs_handle_install_realm(JSContext *ctx)
{
    JSValue base, file_p, dir_p, prev, global;

    /* All three interfaces are `[Exposed=(Window,Worker), SecureContext]`, and §3.3.13 REMOVES an interface in
       a non-secure realm — interface object and prototype alike. Asked once, for the interfaces, because that
       is the level the extended attribute is written at. Nothing can mint a handle in such a realm either:
       every door onto one (§3's getDirectory, and File System Access's pickers when they land) carries the
       same attribute, which is what fs_handle_new's own assert says when one is reached anyway. */
    if (!secure_context_is(ctx)) return;
    prev = JS_GetClassProto(ctx, g_handle_class);
    DCHECK(JS_IsNull(prev), "fs_handle_install_realm ran twice in one realm — everything already holding the "
                            "first FileSystemHandle.prototype would answer out of a discarded object");
    JS_FreeValue(ctx, prev);

    /* §2.2's FileSystemHandle.prototype, which is not itself a class's instance prototype here: a handle wears
       one of the two DERIVED prototypes, and this one is what they chain to. */
    base = JS_NewObject(ctx);
    CHECK(!JS_IsException(base), "FileSystemHandle.prototype could not be allocated");
    idl_interface_tag(ctx, base, "FileSystemHandle");
    idl_install_accessor(ctx, base, "kind", fsh_get_kind, 0, -1);
    idl_install_accessor(ctx, base, "name", fsh_get_name, 0, -1);
    idl_install_method(ctx, base, "isSameEntry", 1, g_id_is_same_entry);
    JS_SetClassProto(ctx, g_handle_class, JS_DupValue(ctx, base));

    file_p = JS_NewObjectProto(ctx, base);
    CHECK(!JS_IsException(file_p), "FileSystemFileHandle.prototype could not be allocated");
    idl_interface_tag(ctx, file_p, "FileSystemFileHandle");
    idl_install_method(ctx, file_p, "getFile", 0, g_id_get_file);
    idl_install_method(ctx, file_p, "createWritable", 0, g_id_create_writable);
    JS_SetClassProto(ctx, g_file_handle_class, JS_DupValue(ctx, file_p));

    dir_p = JS_NewObjectProto(ctx, base);
    CHECK(!JS_IsException(dir_p), "FileSystemDirectoryHandle.prototype could not be allocated");
    idl_interface_tag(ctx, dir_p, "FileSystemDirectoryHandle");
    idl_install_method(ctx, dir_p, "getFileHandle", 1, g_id_get_file_handle);
    idl_install_method(ctx, dir_p, "getDirectoryHandle", 1, g_id_get_directory_handle);
    idl_install_method(ctx, dir_p, "removeEntry", 1, g_id_remove_entry);
    idl_install_method(ctx, dir_p, "resolve", 1, g_id_resolve);
    /* §2.4.1's `async_iterable<USVString, FileSystemHandle>` — Web IDL §3.7.10's four members and §3.7.10.2's
       asynchronous iterator prototype object, both built for THIS realm. It is installed here, inside the
       [SecureContext] gate above, because §3.3.13 removes every member of these interfaces in a non-secure
       realm and an iterator prototype nothing can reach would be an object built for nobody. */
    idl_async_iter_install(ctx, dir_p, g_dir_iter_handle);
    JS_SetClassProto(ctx, g_dir_handle_class, JS_DupValue(ctx, dir_p));

    /* §3.7.1's INTERFACE OBJECTS, on THIS realm's global. None of the three declares a constructor, so `new
       FileSystemHandle()` is a TypeError — and their presence is what tells a feature-detecting bundle the API
       exists at all, which is exactly the gate this component was built to stop lying about. */
    global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "FileSystemHandle", idl_interface_object(ctx, "FileSystemHandle", base));
    JS_SetPropertyStr(ctx, global, "FileSystemFileHandle",
                      idl_interface_object(ctx, "FileSystemFileHandle", file_p));
    JS_SetPropertyStr(ctx, global, "FileSystemDirectoryHandle",
                      idl_interface_object(ctx, "FileSystemDirectoryHandle", dir_p));
    JS_FreeValue(ctx, global);
    JS_FreeValue(ctx, base);
    JS_FreeValue(ctx, file_p);
    JS_FreeValue(ctx, dir_p);
}

void fs_handle_init(JSContext *ctx)
{
    JSRuntime *rt = JS_GetRuntime(ctx);
    JSClassDef hd = { "FileSystemHandle", fsh_finalizer };
    JSClassDef fd = { "FileSystemFileHandle" };
    JSClassDef dd = { "FileSystemDirectoryHandle" };
    static const IdlArgType HANDLE_ARG[] = { IDL_INTERFACE };
    static const IdlArgType NAME_OPTS[]  = { IDL_USVSTRING, IDL_DICT };
    static const IdlArgType OPTS_ONLY[]  = { IDL_DICT };

    DCHECK(g_handle_class == 0, "fs_handle_init ran twice — §2.2's class and its members are declared once per "
                                "AGENT");
    JS_NewClassID(rt, &g_handle_class);
    CHECK(JS_NewClass(rt, g_handle_class, &hd) == 0, "FileSystemHandle: the class could not be declared");
    JS_NewClassID(rt, &g_file_handle_class);
    CHECK(JS_NewClass(rt, g_file_handle_class, &fd) == 0,
          "FileSystemFileHandle: the per-realm prototype slot could not be declared");
    JS_NewClassID(rt, &g_dir_handle_class);
    CHECK(JS_NewClass(rt, g_dir_handle_class, &dd) == 0,
          "FileSystemDirectoryHandle: the per-realm prototype slot could not be declared");

    /* §2.2's two attributes run none of the page's code — they read the locator, which is this component's own
       C record — so they are ordinary getters and not machines, and they carry no magic. */
    g_id_is_same_entry = idl_method_id_step(ctx, HANDLE_ARG, 1, NULL, 0, &FSH_DECL, M_IS_SAME_ENTRY);
    idl_iface_brand(g_handle_class);
    g_id_resolve = idl_method_id_step(ctx, HANDLE_ARG, 1, NULL, 0, &FSH_DECL, M_RESOLVE);
    idl_iface_brand(g_handle_class);
    g_id_get_file = idl_method_id_step(ctx, NULL, 0, NULL, 0, &FSH_DECL, M_GET_FILE);
    g_id_create_writable = idl_method_id_step(ctx, OPTS_ONLY, 1, WRITABLE_OPTIONS, 1, &FSH_DECL,
                                              M_CREATE_WRITABLE);
    idl_optional_from(0);
    g_id_get_file_handle = idl_method_id_step(ctx, NAME_OPTS, 2, GET_FILE_OPTIONS, 1, &FSH_DECL,
                                              M_GET_FILE_HANDLE);
    idl_optional_from(1);
    g_id_get_directory_handle = idl_method_id_step(ctx, NAME_OPTS, 2, GET_FILE_OPTIONS, 1, &FSH_DECL,
                                                   M_GET_DIRECTORY_HANDLE);
    idl_optional_from(1);
    g_id_remove_entry = idl_method_id_step(ctx, NAME_OPTS, 2, REMOVE_OPTIONS, 1, &FSH_DECL, M_REMOVE_ENTRY);
    idl_optional_from(1);
    /* §2.4.1's asynchronous iteration, declared with the members it sits beside because the class, the three
       method ids and the two step machines are the AGENT's, exactly as every id above is. */
    g_dir_iter_handle = idl_async_iter_declare(ctx, &FS_DIR_ITER_OPS);
    realm_declare_intrinsic(fs_handle_install_realm);
}

void fs_handle_free(void)
{
    /* The prototypes and the interface objects are the REALMS' — each is released with its context; a handle's
       locator is released by the finalizer. What the agent holds is three class ids in a runtime that is going
       away with them. */
    g_id_is_same_entry = g_id_get_file = g_id_create_writable = -1;
    g_id_get_file_handle = g_id_get_directory_handle = g_id_remove_entry = g_id_resolve = -1;
    g_dir_iter_handle = -1;
    g_handle_class = g_file_handle_class = g_dir_handle_class = 0;
}
