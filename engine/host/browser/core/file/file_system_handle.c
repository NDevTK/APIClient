/* FileSystemHandle, FileSystemFileHandle and FileSystemDirectoryHandle — File System Standard §2.2 "The
 * FileSystemHandle interface", §2.3 "The FileSystemFileHandle interface", §2.4 "The FileSystemDirectoryHandle
 * interface".
 *
 * A BARE § IN A COMMENT HERE IS THE FILE SYSTEM STANDARD; EVERY CRASH MESSAGE NAMES IT IN FULL INSTEAD, and
 * the asymmetry is the point — a banner convention is exactly what a reader standing at an abort cannot see,
 * because the only text a crash prints is the message. `engine/citegen.mjs` cannot see it either: it resolves a
 * standard-less citation by whichever standard the file's other citations name most, so a file whose only
 * ANCHORED citations belong to a neighbouring standard has every bare number in it answered by that neighbour.
 * This one's neighbour is Web IDL, and Web IDL HAS a §2.1, a §2.2, a §2.3 and a §2.4 — "Names", "Interfaces",
 * "Interface mixins", "Callback interfaces". That is the worst state a citation can be in and it is worse than
 * an invalid number: a wrong document behind a plausible number, with nothing anywhere to say so.
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
 * settling one calls a resolving function — ECMAScript §27.5.1.3 "CreateResolvingFunctions ( toResolve )"
 * step 2.f reads `then` off whatever it is resolved with, which is the page's code and therefore a request
 * rather than a call from C. And `createWritable` runs §2.5's stream creation, which suspends on its own start
 * promise. A member that ran either from a C activation would be the drive-to-completion this engine aborts
 * on.
 *
 * THEY ARE ONE MACHINE WITH A MAGIC. The seven of them differ in the ALGORITHM and agree in everything around
 * it: locate the entry, decide a completion, settle the promise. Written as seven machines that shape would be
 * copied seven times and the settle would be seven chances to drop a rejection.
 *
 * §2.4.1 "Directory iteration" is the same machine one layer down. `async_iterable<USVString,
 * FileSystemHandle>` is Web IDL §3.7.10 "Asynchronous iterable declarations", whose whole binding — `entries`,
 * `keys`, `values`, %Symbol.asyncIterator%, the iterator object and its `next` — is core/idl_async_iter.c's,
 * and all this file supplies is the two hooks Web IDL §2.5.10 "Asynchronously iterable declarations" requires
 * the accompanying prose to define — the initialization steps that give the iterator its past-results set, and
 * "get the next iteration result" — both of which the File System Standard writes under its own §2.4.1. Each
 * is a step machine for the same reason every member above is. THE STANDARD ON §2.5.10 IS LOAD-BEARING: this
 * file's default standard has a §2.5 "The FileSystemWritableFileStream interface" that stops at §2.5.3, so an
 * unqualified §2.5.10 reads as a section the File System Standard does not have.
 *
 * WHAT IS ABSENT AND WHY. §2.6's FileSystemSyncAccessHandle is `[Exposed=DedicatedWorker]` and this engine
 * has no WorkerGlobalScope, so its exposure set is empty here. File System Access §2.3 "The FileSystemHandle
 * interface"'s `queryPermission`/`requestPermission` are a PARTIAL interface of this one and live in
 * core/file/file_system_access.c beside the "file-system" powerful feature they are the two doors onto; they
 * install onto the prototype this file builds, through fs_handle_proto. */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/agent_state.h"
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

/* THE LOCATOR IS REACHED FROM THE OBJECT AND THIS ENTRY READS NO STATIC OF THIS FILE — see core/agent_state.h
   on what a release owes a finalizer. fs_handle_free is a row on core/platform.h's release column and gives
   g_handle_class back to 0, and platform_agent_free runs BEFORE the collection that finalizes the page's
   object graph, so a handle a page still held at teardown reached here with the class id already zero and
   JS_GetOpaque(val, 0) answered NULL for it: the whole locator — its path array, every component string and
   its root — leaked, and it leaked SILENTLY, because a malloc'd block is invisible to both of
   JS_FreeRuntime's censuses. JS_GetAnyOpaque asks the object, which is the question the collector answered by
   dispatching here; the id is not read because only g_handle_class names this function, so a handle is the
   only thing that can arrive. */
static void fsh_finalizer(JSRuntime *rt, JSValue val)
{
    JSClassID id = 0;
    FsLocator *l = JS_GetAnyOpaque(val, &id);
    int i;

    (void)rt; (void)id;
    /* NOT `if (!l) return;`. fs_handle_new is the one mint and it builds the record COMPLETE before attaching
       it, with nothing between JS_NewObjectProtoClass and JS_SetOpaque that allocates on the JS heap or
       returns — so there is no half-built handle for a collection to meet. */
    DCHECK(l != NULL, "a FileSystemHandle was finalized with no locator — File System Standard §2.2 The "
                      "FileSystemHandle interface says a handle IS associated with one, and fs_handle_new "
                      "attaches it before the object can reach a collection");
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
    DCHECK(*npath >= 1, "a handle's locator holds a path with no items — File System Standard §2.1 Concepts' "
                        "file system path is a list of ONE OR "
                        "MORE strings, and fs_handle_new asserts that at the mint");
    return true;
}

JSValue fs_handle_proto(JSContext *ctx)
{
    DCHECK(g_handle_class != 0, "File System Standard §2.2 The FileSystemHandle interface's prototype was "
                                "asked for before fs_handle_init declared the class");
    return JS_GetClassProto(ctx, g_handle_class);
}

JSValue fs_handle_new(JSContext *ctx, bool directory, const char *root, const char *const *path, int npath)
{
    JSValue proto = JS_GetClassProto(ctx, directory ? g_dir_handle_class : g_file_handle_class);
    JSValue obj;
    FsLocator *l;
    int i;

    DCHECK(npath >= 1, "a FileSystemHandle was minted with a path of no items — File System Standard §2.1 "
                       "Concepts' file system path is a list of ONE OR MORE strings, and `handle.name` is its "
                       "last item");
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

/* §2.3's "To create a child FileSystemFileHandle given a directory locator parentLocator and a string name in
   a Realm realm" AND §2.4's create-a-child FileSystemDirectoryHandle, which is the same sentence with the other
   interface in it. THEY ARE TWO OPERATIONS AND THIS IS ONE FUNCTION, so the two names are written out rather
   than folded into one quotation with a slash in it: no section of the standard contains the merged sentence,
   and a reader who went looking for it would find neither. childRoot is a copy of the parent's root, and
   childPath is the parent's path cloned with `name` appended. The `realm` both operations take is this one —
   a C member runs in the realm that defined it. OWNED. */
static JSValue fsh_child(JSContext *ctx, const FsLocator *parent, const char *name, bool directory)
{
    const char **path;
    JSValue h;
    int i;

    DCHECK(parent->directory, "File System Standard §2.3-§2.4's create a child FileSystemFileHandle / "
                              "FileSystemDirectoryHandle was given a locator that is not a DIRECTORY LOCATOR "
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

/* WEB IDL §3.7.6 "Attributes"' BRAND CHECK, which for these two is the opaque: `FileSystemHandle.prototype.kind`
   read off a plain object is a TypeError and a page tells that apart from `undefined`. The number used to read
   §3.7.5, which is "Constants" and defines nothing about a receiver; the check is in §3.7.6's create-an-
   attribute-getter steps — "If jsValue does not implement target, then … throw a TypeError". */
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
    X(FSH_STREAM, "File System §2.3.2 The createWritable() method step 5.7.2 (create a new " \
                  "FileSystemWritableFileStream for entry — which suspends on the start promise Streams " \
                  "§5.5.4 Default controllers' SetUpWritableStreamDefaultController builds)") \
    X(FSH_SETTLE, "File System §2.3-§2.4 the queued storage task of this member (resolve or reject result — " \
                  "ECMAScript §27.5.1.3 CreateResolvingFunctions step 2.f's `then` read is the page's)")
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

    /* "If child's locator's root is not root's locator's root, resolve result with null, and abort these
       steps." */
    if (strcmp(child->root, root->root)) return JS_NULL;
    if (root->npath > child->npath) return JS_NULL;
    for (i = 0; i < root->npath; i++)
        if (strcmp(root->path[i], child->path[i])) return JS_NULL;
    out = JS_NewArray(ctx);
    CHECK(!JS_IsException(out), "file system handle: File System Standard §2.1 Concepts' resolve a file "
                                "system locator could not allocate its relative path");
    /* STEP 2.8, AND THE STANDARD'S OWN WORDS FOR IT ARE "For each index of the range from rootPath's size to
       rootPath's size, exclusive, append childPath.[[index]] to relativePath." — a range from a number to
       ITSELF, which is empty, so read literally §2.1 Concepts' resolve a file system locator answers « » for
       EVERY descendant. This loop runs to childPath's size instead, and that is a DEPARTURE rather than a
       transcription: §2.4.5 The resolve() method's own prose says "If child is a direct child of directory,
       path will be an array containing child's name" and its example asserts `relative_path.pop() ===
       file_ref.name`, both of which the literal reading contradicts. The empty range is what the SAME-PATH case
       wants, and step 2.4 has already answered « » for that four steps earlier — which is what makes the second
       `rootPath` a typo for `childPath` rather than a rule. Steps 2.4 and 2.8 are one loop here: for equal paths
       it appends nothing, which is step 2.4's « ». The quotation stays verbatim so the next reader compares this
       loop against what the standard SAYS and not against what it meant; the sub-numbers are counted with LIST
       DEPTH, and step 2.6 holds a nested one-item list that a flat count would promote to a peer. */
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

            DCHECK(o != NULL, "File System Standard §2.2.1 The isSameEntry() method's `other` reached the body as "
                              "something that is not a FileSystemHandle — "
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

            DCHECK(o != NULL, "File System Standard §2.4.5 The resolve() method's `possibleDescendant` "
                              "reached the body as something that is not a FileSystemHandle");
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
                   "File System Standard §2.3.1 The getFile() method located a DIRECTORY entry — a "
                   "FileSystemFileHandle's locator's kind is file, and File System Standard "
                   "§2.1 Concepts' locate an entry answers a file entry or null for one");
            s->value = file_system_file_new(ctx, entry);
            break;
        case M_CREATE_WRITABLE:
            if (JS_IsNull(entry)) {
                s->value = fsh_dom_error(ctx, "NotFoundError", "the file this handle names no longer exists");
                reject = 1;
                break;
            }
            DCHECK(file_system_is_file(entry), "File System Standard §2.3.2 The createWritable() "
                                              "method located a DIRECTORY entry");
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
                   "a File System Standard §2.4 The FileSystemDirectoryHandle interface member located a FILE "
                   "entry — a FileSystemDirectoryHandle's locator's kind is directory, and File "
                   "System Standard §2.1 Concepts' locate an entry answers a directory entry or null "
                   "for one");
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
        /* §2.3.2 step 5.7.2, and §2.5's creation is a sub-sequence because "set up stream" resolves a start
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

    DCHECK(hdr->stage == FSH_SETTLE, "a FileSystemHandle member resumed into a stage File System "
                                     "Standard §2.3-§2.4 does not have");
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
    DCHECK(JS_IsUndefined(*pstate), "File System Standard §2.4.1 Directory iteration's asynchronous "
                                    "iterator initialization steps ran on an iterator that already had "
                                    "state — Web IDL §3.7.10 step 3.1.6 runs them exactly once, at the mint");
    *pstate = set;
    return 0;
}

/* §2.4.1 steps 2.3.3-2.3.8: the first child whose name `past` does not already contain, appended to `past` and
   turned into a child handle. Answers the two-element value pair a PAIR declaration resolves with — « child's
   name, result » — or Web IDL §2.5.10's end of iteration when there is no such child. OWNED, or JS_EXCEPTION.
   THE SUB-NUMBERS HERE ARE COUNTED WITH LIST DEPTH. File System §2.4.1's step 2.3.2 CONTAINS a nested
   one-item list ("Assert: directory is a directory entry."), so a flat count of the <li>s under step 2.3
   promotes that assert to a peer and every step from 2.3.3 down reads one too high — which is how this block came to cite 2.3.4-2.3.8 for an
   algorithm whose last step IS 2.3.8. */
static JSValue fsdir_iter_pick(JSContext *ctx, const FsLocator *l, JSValueConst dir, JSValueConst past)
{
    int n = file_system_child_count(ctx, dir), i;

    DCHECK(JS_IsObject(past), "File System Standard §2.4.1 Directory iteration's get the next "
                              "iteration result reached its steps with no past results set — Web IDL "
                              "§3.7.10 step 3.1.6 runs the initialization steps that create it before the "
                              "iterator is ever handed out");
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
        /* step 2.3.5: "Append child's name to iterator's past results." */
        JS_DefinePropertyValue(ctx, past, a, JS_TRUE, JS_PROP_C_W_E);
        JS_FreeAtom(ctx, a);
        /* steps 2.3.6-2.3.7: a child FileSystemFileHandle for a file entry, a child FileSystemDirectoryHandle
           for a directory entry — which is fsh_child, the same operation §2.4.2 and §2.4.3 reach. */
        nm = file_system_name_cstr(ctx, child);
        CHECK(nm != NULL, "file system handle: File System Standard §2.4.1 Directory "
                          "iteration reached a child entry with no name");
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
    /* step 2.3.4: "If child is null, resolve promise with undefined and abort these steps" — which is Web IDL
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
   ECMAScript §27.5.1.3 CreateResolvingFunctions step 2.f's `then` read on the value pair it is handed (an Array, one
   `Object.defineProperty(Array.prototype, "then", …)` away from being the page's code). Numbered from
   IDL_ASYNC_ITER_STEP_FIRST and joined onto Web IDL §3.7.10.2's own stages at the declaration, so a flow parked
   there reports the File System Standard's step rather than the Web IDL step that RUNS this algorithm. */
#define FSD_STAGES(X) \
    X(FSD_NEXT_SETTLE, \
      "File System §2.4.1 Directory iteration, get the next iteration result steps 2.3.2, 2.3.4 and 2.3.8 " \
      "(rejecting promise with a " \
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
   iterator". A STEP because its last act settles a promise, and a resolving function reaches ECMAScript
   §27.5.1.3 CreateResolvingFunctions step 2.f's `then` read on what it is given — the sixth sub-step of
   `resolveSteps`, which ecmarkup renders lower-alpha at that depth. This read "step 8" until it was checked
   against the text: step 2.h is `Let thenAction be then.[[Value]]`, one step PAST the Get, so the number named
   a real step that is not the one the sentence is about — which for a value pair is an Array, one
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
        DCHECK(l != NULL, "File System Standard §2.4.1 Directory iteration reached a target that "
                          "is not a FileSystemHandle — the iterator's target is the receiver Web IDL "
                          "§3.7.10 step 3.1.3 has already brand-checked");
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
                   "File System Standard §2.4.1 Directory iteration located a FILE entry — a "
                   "FileSystemDirectoryHandle's locator's kind is directory, and File System "
                   "Standard §2.1 Concepts' locate an entry answers a directory entry or null for one");
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
           "File System Standard §2.4.1 Directory iteration's get the next "
           "iteration result resumed at a step it never rests at");
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
    idl_install_method(ctx, base, "isSameEntry", g_id_is_same_entry);
    JS_SetClassProto(ctx, g_handle_class, JS_DupValue(ctx, base));

    file_p = JS_NewObjectProto(ctx, base);
    CHECK(!JS_IsException(file_p), "FileSystemFileHandle.prototype could not be allocated");
    idl_interface_tag(ctx, file_p, "FileSystemFileHandle");
    idl_install_method(ctx, file_p, "getFile", g_id_get_file);
    idl_install_method(ctx, file_p, "createWritable", g_id_create_writable);
    JS_SetClassProto(ctx, g_file_handle_class, JS_DupValue(ctx, file_p));

    dir_p = JS_NewObjectProto(ctx, base);
    CHECK(!JS_IsException(dir_p), "FileSystemDirectoryHandle.prototype could not be allocated");
    idl_interface_tag(ctx, dir_p, "FileSystemDirectoryHandle");
    idl_install_method(ctx, dir_p, "getFileHandle", g_id_get_file_handle);
    idl_install_method(ctx, dir_p, "getDirectoryHandle", g_id_get_directory_handle);
    idl_install_method(ctx, dir_p, "removeEntry", g_id_remove_entry);
    idl_install_method(ctx, dir_p, "resolve", g_id_resolve);
    /* §2.4.1's `async_iterable<USVString, FileSystemHandle>` — a PAIR declaration, so Web IDL §3.7.10's steps
       3-5 (`entries`, %Symbol.asyncIterator%, `keys`, `values`) and §3.7.10.2's asynchronous iterator prototype
       object, both built for THIS realm. It is installed here, inside the [SecureContext] gate above, because
       §3.3.13 removes every member of these interfaces in a non-secure realm and an iterator prototype nothing
       can reach would be an object built for nobody. */
    idl_async_iter_install_pair(ctx, dir_p, g_dir_iter_handle);
    JS_SetClassProto(ctx, g_dir_handle_class, JS_DupValue(ctx, dir_p));

    /* §3.7.1's INTERFACE OBJECTS, on THIS realm's global. None of the three declares a constructor, so `new
       FileSystemHandle()` is a TypeError — and their presence is what tells a feature-detecting bundle the API
       exists at all, which is exactly the gate this component was built to stop lying about. */
    global = JS_GetGlobalObject(ctx);
    idl_define_global_property_reference(ctx, global, "FileSystemHandle",
                                         idl_interface_object(ctx, "FileSystemHandle", base));
    idl_define_global_property_reference(ctx, global, "FileSystemFileHandle",
                                         idl_interface_object(ctx, "FileSystemFileHandle", file_p));
    idl_define_global_property_reference(ctx, global, "FileSystemDirectoryHandle",
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

    DCHECK(g_handle_class == 0, "fs_handle_init ran twice — File System Standard §2.2 The FileSystemHandle "
                                "interface's class and its members are declared once per AGENT");
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
    /* EVERY ONE OF THE SEVEN RETURNS A PROMISE TYPE, so Web IDL §3.7.7 Operations' create an operation
       function rejects rather than throws for the whole call — and here that is not a formality, because the
       ARGUMENT CONVERSIONS are where these members fail in practice and every one of them runs BEFORE the
       body: `dir.getFileHandle()` (arity), `dir.getFileHandle(name, 5)` (a dictionary from a non-object) and
       `handle.isSameEntry({})` (the declared interface brand below) all threw where a browser hands back a
       rejected promise. The body's own §3.7.7 handling — the capability minted before its first failure, the
       locator check settling rather than throwing — covers only what happens after that. */
    g_id_is_same_entry = idl_method_id_step(ctx, HANDLE_ARG, 1, NULL, 0, &FSH_DECL, M_IS_SAME_ENTRY);
    idl_iface_brand(g_handle_class);
    idl_returns_promise();   /* §2.2.1 The isSameEntry() method: `Promise<boolean>` */
    g_id_resolve = idl_method_id_step(ctx, HANDLE_ARG, 1, NULL, 0, &FSH_DECL, M_RESOLVE);
    idl_iface_brand(g_handle_class);
    idl_returns_promise();   /* §2.4.5 The resolve() method: `Promise<sequence<USVString>?>` */
    g_id_get_file = idl_method_id_step(ctx, NULL, 0, NULL, 0, &FSH_DECL, M_GET_FILE);
    idl_returns_promise();   /* §2.3.1 The getFile() method: `Promise<File>` */
    g_id_create_writable = idl_method_id_step(ctx, OPTS_ONLY, 1, WRITABLE_OPTIONS, 1, &FSH_DECL,
                                              M_CREATE_WRITABLE);
    idl_optional_from(0);
    idl_returns_promise();   /* §2.3.2 The createWritable() method: `Promise<FileSystemWritableFileStream>` */
    g_id_get_file_handle = idl_method_id_step(ctx, NAME_OPTS, 2, GET_FILE_OPTIONS, 1, &FSH_DECL,
                                              M_GET_FILE_HANDLE);
    idl_optional_from(1);
    idl_returns_promise();   /* §2.4.2 The getFileHandle() method: `Promise<FileSystemFileHandle>` */
    g_id_get_directory_handle = idl_method_id_step(ctx, NAME_OPTS, 2, GET_FILE_OPTIONS, 1, &FSH_DECL,
                                                   M_GET_DIRECTORY_HANDLE);
    idl_optional_from(1);
    idl_returns_promise();   /* §2.4.3 The getDirectoryHandle() method: `Promise<FileSystemDirectoryHandle>` */
    g_id_remove_entry = idl_method_id_step(ctx, NAME_OPTS, 2, REMOVE_OPTIONS, 1, &FSH_DECL, M_REMOVE_ENTRY);
    idl_optional_from(1);
    idl_returns_promise();   /* §2.4.4 The removeEntry() method: `Promise<undefined>` */
    /* §2.4.1's asynchronous iteration, declared with the members it sits beside because the class, the three
       method ids and the two step machines are the AGENT's, exactly as every id above is. */
    g_dir_iter_handle = idl_async_iter_declare(ctx, &FS_DIR_ITER_OPS);
    agent_state_class("file_system_handle", &g_handle_class, "§2.2's opaque and brand, and the declaration latch");
    agent_state_class("file_system_handle", &g_file_handle_class, "§2.3's per-realm prototype slot");
    agent_state_class("file_system_handle", &g_dir_handle_class, "§2.4's per-realm prototype slot");
    agent_state_id("file_system_handle", &g_dir_iter_handle, "§2.4.1's asynchronous iteration declaration");
    realm_declare_intrinsic(fs_handle_install_realm);
}

void fs_handle_free(void)
{
    /* The prototypes and the interface objects are the REALMS' — each is released with its context; a handle's
       locator is released by the finalizer. What the agent holds is three class ids in a runtime that is going
       away with them.
       THE FINALIZER RUNS AFTER THIS, which is why it reads none of the three: a handle a page still held is
       finalized in a collection this release has already happened before. See core/agent_state.h. */
    g_id_is_same_entry = g_id_get_file = g_id_create_writable = -1;
    g_id_get_file_handle = g_id_get_directory_handle = g_id_remove_entry = g_id_resolve = -1;
    g_dir_iter_handle = -1;
    g_handle_class = g_file_handle_class = g_dir_handle_class = 0;
}
