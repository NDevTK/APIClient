/* FILE SYSTEM ACCESS §3 — the three local file system handle factories. See file_picker.h for what a picker
 * returns and which half of it is concolic.
 *
 * §3.1's VERIFICATION, AS THE STANDARD WRITES IT:
 *
 *   To verify that an environment is allowed to show a file picker, run these steps:
 *     1. If environment's origin is an opaque origin, return a promise rejected with a "SecurityError"
 *        DOMException.
 *     2. If environment's origin is not same origin with environment's top-level origin, return a promise
 *        rejected with a "SecurityError" DOMException.
 *     3. Let global be environment's global object.
 *     4. If global does not have transient activation, then throw a "SecurityError" DOMException.
 *
 * THREE CHECKS, AND ONE OF THEM IS A REQUEST. Steps 1 and 2 read this document's own origin and its position
 * in the navigable tree — facts this engine holds, computed, nothing to fork over — while step 4 asks whether
 * a user has interacted, which is unknown external state and therefore a FORK. Steps 1 and 2 are ONE stage and
 * step 4 is another, and the reason is not that the first two run none of the page's code: core/idl_args.h and
 * quickjs-step.h now state that the page decides nothing about where this engine may park, because what parks
 * a flow is RAM pressure, a cold-tier eviction, a cross-session resume or a flow that outranks it. Steps 1 and
 * 2 share a stage because they are ONE O(1) engine action — an opaque origin is same origin with nothing, so
 * §7.2.5.1's single comparison decides both — and every other step of these algorithms has a stage of its own.
 *
 * THE ASYMMETRY IN THOSE FOUR STEPS IS THE STANDARD'S AND IT IS NOT OBSERVABLE. Steps 1 and 2 say "return a
 * promise rejected with", step 4 says "throw" — and the callers invoke this at their own step 5, BEFORE the
 * step 6 that creates their promise. Web IDL §3.7.7 settles it: an operation whose return type is a promise
 * has an abrupt completion turned into a rejected promise, so all three failures reach the page the same way.
 * This file creates the capability before the first thing that can fail and settles every one of them on it,
 * which is what makes `showOpenFilePicker().catch(f)` run f for all three.
 *
 * §3.3's, §3.4's and §3.5's STEP 7 IN PARALLEL, AND WHAT EACH SELECTS. The three algorithms agree from step 1
 * to step 7.4 and diverge in exactly what "the selected files or directories" are: showOpenFilePicker takes
 * the file entries of the starting directory this control's filter admits (at most one unless
 * `multiple`), showDirectoryPicker takes one of its directory entries, and showSaveFilePicker takes one file
 * entry and CLEARS it — an existing one, or one created under `suggestedName` where the page suggested a name,
 * because a name the page did not suggest and the device does not hold is one only a user could have typed. */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "core/file/file_device.h"
#include "core/file/file_picker.h"
#include "core/file/file_system.h"
#include "core/file/file_system_access.h"
#include "core/file/file_system_handle.h"
#include "core/frame/window_proxy.h"
#include "core/html/user_activation.h"
#include "solver/concolic.h"

/* THE DIALOG'S OUTCOME — step 7.4's `dismissed`, the same unknown core/html/input_picker.c's file dialog is
   and declared the same way, because it IS the same dialog one standard over. The EXAMPLE is "not dismissed",
   which is what the modelled device produces, and that is also why outcome 0 is that arm. */
#define PICKER_DIALOG_SHAPE "{file picker dismissed}"
#define PICKER_DIALOG_SRC   "showFilePicker().dismissed"
#define PICKER_DIALOG_OP    "File System Access §3.3 step 7.4 (WebDriver BiDi file dialog opened)"

enum { M_OPEN = 0, M_SAVE, M_DIRECTORY };

/* §3.2.2's WellKnownDirectory, in the IDL's order. "The exact paths the various values of this enum map to is
   implementation-defined (and in some cases these might not even represent actual paths on disk)" — this agent
   maps each to a directory of that name under the local file system root, which is a path whether or not the
   device holds such a directory. A picker started in one the device does not hold shows nothing, which is the
   same answer a real user agent gives for a directory that is not there. */
static const char *const WELL_KNOWN_DIRECTORY[] = {
    "desktop", "documents", "downloads", "music", "pictures", "videos", NULL
};

/* §2.2's `FileSystemPermissionMode`, which DirectoryPickerOptions also takes. */
static const char *const FS_MODE_VALUES[] = { "read", "readwrite", NULL };

static int g_id_open = -1, g_id_save = -1, g_id_directory = -1;
/* §3.2.2's RECENTLY PICKED DIRECTORY MAP — "a map of origins to path id maps". An instance is ONE origin
   (SECURITY.md), so the outer map has exactly one entry and this IS its value: a path id map, from valid path
   ids to paths. It is a JS object built at the pre-boot baseline so a flow that remembers a directory writes
   into its own COW delta and its sibling still sees the map as it was — a malloc'd table would be one timeline
   for every flow. §3.2.2 asks user agents to "implement some mechanism to limit how many recently picked
   directories will be remembered"; this engine implements none, because a cap on remembered entries is a cap
   on distinct work and the map's size is bounded by the ids the page itself writes. */
static JSValue g_recent;
static JSRuntime *g_rt;

/* ---- §3.2.1's ACCEPT TYPES ---------------------------------------------------------------------------- */

/* §3.2.1's PROCESS ACCEPT TYPES, over the options this component can convert.
 *
 * With `types` absent, the algorithm is steps 1 and 5-7: accepts options starts empty, step 5's condition
 * ("either accepts options is empty, OR options['excludeAcceptAllOption'] is false") is TRUE by its first
 * disjunct however `excludeAcceptAllOption` was written, so the all-files option is appended and step 6's
 * TypeError is unreachable. The FILTER that option carries is "an algorithm that returns true", which over
 * this engine's device is core/file/file_device.c's accept filter with no attribute — the one place a file's
 * name and type are matched against a token set, shared with HTML §4.10.5.1.17's control so a file offered by
 * one door is offered by the other.
 *
 * With `types` PRESENT it crashes, naming the conversion — see file_picker.h. It answers nothing, because with `types`
 * absent §3.2.1 CANNOT fail: step 6's "if accepts options is empty, then throw a TypeError" is unreachable
 * once step 5 has appended the all-files option, and step 5's condition is true by its first disjunct. */
static void picker_process_accept_types(JSContext *ctx, JSValueConst options)
{
    JSValue types = idl_dict_get(ctx, options, "types");
    bool present = !JS_IsUndefined(types);

    JS_FreeValue(ctx, types);
    if (present)
        DFAIL("File System Access §3.2.1's process accept types was given `types`, whose IDL type is "
              "`sequence<FilePickerAcceptType>` — an ITERATOR-PROTOCOL conversion (Web IDL §3.2.21) whose "
              "element is a DICTIONARY carrying a `record<USVString, (USVString or sequence<USVString>)>`. "
              "core/idl_args.h declares neither: IDL_SEQUENCE_STRING_OR_DICT is the nearest and its union has a "
              "STRING arm, which would cross `types: [\"x\"]` as a string where §3.2.17 makes it a TypeError. "
              "BUILD those two declared types (a `sequence<D>` over a dictionary, and `record<K,V>` over "
              "step_ownkeys_run + step_getownprop_run, both of which quickjs-step.h already provides), then "
              "run §3.2.1's steps 1-4 over the result — INCLUDING `validate a suffix`, whose four TypeErrors "
              "(does not start with '.', a code point that is not ASCII alphanumeric / '+' / '.', ends with "
              "'.', longer than 16 code points) are reachable from NOWHERE ELSE and are therefore not written "
              "yet — and give each option's filter to picker_select in place of the all-files one");
}

/* ---- §3.2.2's STARTING DIRECTORY --------------------------------------------------------------------------- */

/* "A valid path id is a string where each character is ASCII alphanumeric or '_' or '-'." */
static bool picker_valid_path_id(const char *id, size_t len)
{
    size_t i;

    for (i = 0; i < len; i++) {
        char c = id[i];

        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              c == '_' || c == '-'))
            return false;
    }
    return true;
}

/* A PATH, as this component carries one: a JS array of strings, because a picker's starting directory is held
   across the dialog's fork and everything a flow holds across one has to PARK. */
static JSValue picker_path_new(JSContext *ctx, const char *const *items, int n)
{
    JSValue a = JS_NewArray(ctx);
    int i;

    CHECK(!JS_IsException(a), "file picker: a path could not be allocated");
    for (i = 0; i < n; i++)
        JS_DefinePropertyValueUint32(ctx, a, (uint32_t)i, JS_NewString(ctx, items[i]), JS_PROP_C_W_E);
    return a;
}

/* THE SAME PATH AS THE C VECTOR core/file/file_system.h's locate and fs_handle_new take. The strings are the
   array's own, so `free_path` releases both the vector and every string in it. Returns the item count. */
static int picker_path_c(JSContext *ctx, JSValueConst path, const char ***pout)
{
    JSValue len_v = JS_GetPropertyStr(ctx, path, "length");
    uint32_t n = 0, i;
    const char **out;

    JS_ToUint32(ctx, &n, len_v);
    JS_FreeValue(ctx, len_v);
    DCHECK(n >= 1, "a file picker path with no items — §2.1's path is a list of ONE OR MORE strings and its "
                   "first item names the root");
    out = calloc(n ? n : 1, sizeof *out);
    CHECK(out != NULL, "file picker: OOM building a path vector");
    for (i = 0; i < n; i++) {
        JSValue v = JS_GetPropertyUint32(ctx, path, i);

        out[i] = JS_ToCString(ctx, v);
        JS_FreeValue(ctx, v);
        CHECK(out[i] != NULL, "file picker: a path item could not be read as a string");
    }
    *pout = out;
    return (int)n;
}

static void picker_path_c_free(JSContext *ctx, const char **v, int n)
{
    int i;

    for (i = 0; i < n; i++) JS_FreeCString(ctx, v[i]);
    free((void *)v);
}

/* §3.2.2's DETERMINE THE DIRECTORY THE PICKER WILL START IN, in the standard's own order — the `startIn`
   handle arm takes precedence over the id, which is what "these take precedence even if an explicit id is also
   passed in" says. Returns the path (OWNED), or JS_UNINITIALIZED with a TypeError pending. */
static JSValue picker_starting_directory(JSContext *ctx, JSValueConst id_v, JSValueConst start_in)
{
    static const char *const LOCAL_ROOT[] = { FS_ROOT_LOCAL };
    const char *id = NULL;
    size_t id_len = 0;
    JSValue out;

    if (!JS_IsUndefined(id_v)) {
        id = JS_ToCStringLen(ctx, &id_len, id_v);
        CHECK(id != NULL, "file picker: `id` could not be read back after its declared DOMString conversion");
        /* STEPS 1-2, both TypeErrors and both about the id ALONE — they are checked even when `startIn` is
           going to decide the answer, because the standard checks them first. */
        if (!picker_valid_path_id(id, id_len)) {
            JS_ThrowTypeError(ctx, "a file picker `id` may contain only ASCII alphanumerics, '_' and '-'");
            JS_FreeCString(ctx, id);
            return JS_UNINITIALIZED;
        }
        if (id_len > 32) {
            JS_ThrowTypeError(ctx, "a file picker `id` is longer than 32 code points");
            JS_FreeCString(ctx, id);
            return JS_UNINITIALIZED;
        }
    }
    /* STEP 4: "If startIn is a FileSystemHandle and startIn is not in a bucket file system: let entry be the
       result of locating an entry given startIn's locator; if entry is a file entry, return the path of
       entry's parent in the local file system; if entry is a directory entry, return entry's path." A file
       entry's PARENT is its locator's path without its last item, which is what makes a path a list. */
    {
        bool directory;
        const char *root;
        const char *const *path;
        int npath;

        if (fs_handle_locator(start_in, &directory, &root, &path, &npath) && path[0][0] != '\0') {
            JSValue entry = file_system_locate(ctx, root, path, npath);
            bool is_dir = !JS_IsNull(entry) && file_system_is_directory(entry);
            bool is_file = !JS_IsNull(entry) && file_system_is_file(entry);

            JS_FreeValue(ctx, entry);
            /* An entry that is NEITHER is one the locator no longer names — the page kept a handle to a file
               another flow removed. The algorithm's two arms are both about an entry that EXISTS, so this
               falls through to steps 5-8 with the id intact, exactly as a `startIn` that is not a handle
               does. */
            if (is_dir) {
                if (id) JS_FreeCString(ctx, id);
                return picker_path_new(ctx, path, npath);
            }
            if (is_file) {
                DCHECK(npath >= 2, "§3.2.2 step 4 located a FILE entry whose locator's path is one item — the "
                                   "first item of a path names the root, and a root is a directory entry, so a "
                                   "file at that position is a locator this model cannot hold");
                if (id) JS_FreeCString(ctx, id);
                return picker_path_new(ctx, path, npath - 1);
            }
        }
    }
    /* STEP 5: a non-empty id that the map remembers. */
    if (id && id_len > 0) {
        out = JS_GetPropertyStr(ctx, g_recent, id);
        if (!JS_IsUndefined(out)) {
            JS_FreeCString(ctx, id);
            return out;
        }
        JS_FreeValue(ctx, out);
    }
    /* STEP 6: a WellKnownDirectory. The value has already been checked against the enumeration by the caller,
       so what arrives here is one of the six or nothing. */
    if (JS_IsString(start_in)) {
        const char *w = JS_ToCString(ctx, start_in);
        const char *items[2];

        CHECK(w != NULL, "file picker: `startIn` could not be read back after its enumeration check");
        items[0] = FS_ROOT_LOCAL;
        items[1] = w;
        out = picker_path_new(ctx, items, 2);
        JS_FreeCString(ctx, w);
        if (id) JS_FreeCString(ctx, id);
        return out;
    }
    /* STEP 7: no id, or the empty one. */
    if (!id || id_len == 0) {
        out = JS_GetPropertyStr(ctx, g_recent, "");
        if (!JS_IsUndefined(out)) {
            if (id) JS_FreeCString(ctx, id);
            return out;
        }
        JS_FreeValue(ctx, out);
    }
    if (id) JS_FreeCString(ctx, id);
    /* STEP 8: "Return a default path in a user agent specific manner." This agent's is the LOCAL FILE SYSTEM
       ROOT — the storage a picker chooses from and the one place a host puts a file — so the ordinary
       `showOpenFilePicker()` offers the device's files. */
    return picker_path_new(ctx, LOCAL_ROOT, 1);
}

/* §3.2.2's REMEMBER A PICKED DIRECTORY: "if id is not specified, let id be an empty string; set recently
   picked directory map[origin][id] to the path on the local file system corresponding to entry". The origin is
   this instance's, so the outer map is g_recent itself. `entry_path` is the path of the picked ENTRY, and what
   is remembered is the DIRECTORY it was picked from — which for a file is its parent. */
static void picker_remember(JSContext *ctx, JSValueConst id_v, JSValueConst entry_path, bool entry_is_directory)
{
    const char *id = JS_IsUndefined(id_v) ? NULL : JS_ToCString(ctx, id_v);
    const char **items;
    int n = picker_path_c(ctx, entry_path, &items);
    int keep = n;
    JSValue dir;

    if (!entry_is_directory) {
        DCHECK(n >= 2, "§3.2.2's remember a picked directory was given a FILE whose path is one item");
        keep = n - 1;
    }
    dir = picker_path_new(ctx, items, keep);
    picker_path_c_free(ctx, items, n);
    JS_SetPropertyStr(ctx, g_recent, id ? id : "", dir);
    if (id) JS_FreeCString(ctx, id);
}

/* ---- the prompt's answer ------------------------------------------------------------------------------------
 *
 * "Let entries be a list of file entries representing the selected files or directories." The list this agent
 * offers is the starting directory's children of the kind the factory selects, filtered by the accepts options
 * — which with no `types` admits everything — and bounded by `multiple` exactly as §4.10.5.1.17 bounds a file
 * control's. `*ppath` takes the picked entry's PATH (owned) for the LAST entry, which is what §3.3 step 7.10
 * remembers a directory from. Returns the list of handles (owned array), empty when nothing is selectable. */
static JSValue picker_select(JSContext *ctx, JSValueConst start_path, int magic, bool multiple, JSValue *ppath)
{
    JSValue out = JS_NewArray(ctx);
    const char **items;
    int n = picker_path_c(ctx, start_path, &items), i, count;
    JSValue dir = file_system_locate(ctx, items[0], items, n);
    bool want_directory = (magic == M_DIRECTORY);
    uint32_t taken = 0;

    CHECK(!JS_IsException(out), "file picker: a selection list could not be allocated");
    *ppath = JS_UNDEFINED;
    if (JS_IsNull(dir) || !file_system_is_directory(dir)) {
        /* The starting directory is not there — a WellKnownDirectory this device does not hold, or a remembered
           path whose directory another flow removed. Nothing is selectable, which is the same answer a real
           prompt gives a user looking at an empty folder. */
        JS_FreeValue(ctx, dir);
        picker_path_c_free(ctx, items, n);
        return out;
    }
    count = file_system_child_count(ctx, dir);
    for (i = 0; i < count && (multiple || taken == 0); i++) {
        JSValue name_v = JS_UNDEFINED;
        JSValue child = file_system_child_at(ctx, dir, i, &name_v);
        const char *name, *type;
        const char **child_path;
        bool ok;
        int k;

        if (file_system_is_directory(child) != want_directory) {
            JS_FreeValue(ctx, name_v);
            JS_FreeValue(ctx, child);
            continue;
        }
        name = file_system_name_cstr(ctx, child);
        /* A DIRECTORY HAS NO MIME TYPE and the accept filter is about files, so a directory picker offers every
           directory — which is what §3.5's prompt ("pick a directory") says and why its options carry no
           `types` at all. */
        type = want_directory ? NULL : file_system_type_cstr(ctx, child);
        ok = want_directory || file_device_accepts(NULL, 0, name, type);
        child_path = malloc(sizeof *child_path * (size_t)(n + 1));
        CHECK(child_path != NULL, "file picker: OOM building a selected entry's locator path");
        for (k = 0; k < n; k++) child_path[k] = items[k];
        child_path[n] = name;
        if (ok) {
            JS_FreeValue(ctx, *ppath);
            *ppath = picker_path_new(ctx, child_path, n + 1);
            /* §2.3/§2.4's create a new FileSystemFileHandle / FileSystemDirectoryHandle given a root and a
               path in a Realm — this realm, because a handle wears its realm's prototype. */
            JS_DefinePropertyValueUint32(ctx, out, taken++,
                                         fs_handle_new(ctx, want_directory, items[0], child_path, n + 1),
                                         JS_PROP_C_W_E);
        }
        free((void *)child_path);
        if (type) JS_FreeCString(ctx, type);
        JS_FreeCString(ctx, name);
        JS_FreeValue(ctx, name_v);
        JS_FreeValue(ctx, child);
    }
    JS_FreeValue(ctx, dir);
    picker_path_c_free(ctx, items, n);
    DCHECK(multiple || taken <= 1, "a file picker selected more than one entry for a call whose `multiple` is "
                                   "false — §3.3 step 7.5 allows no more than one file selected in that case");
    return out;
}

/* §3.4's SELECTION, which is the one that may CREATE. "Let entry be a file entry representing the selected
   file ... Set entry's binary data to an empty byte sequence." A save picker's user either picks a file that
   is there or types a name, and the only name this engine can know is the one the PAGE suggested — so a
   suggestion is taken when there is one, and otherwise an existing file is. `suggested` is the sanitized name
   or NULL. Returns the handle (owned) and its path in `*ppath`, or JS_UNINITIALIZED when nothing is
   selectable. */
static JSValue picker_select_save(JSContext *ctx, JSValueConst start_path, const char *suggested, JSValue *ppath)
{
    const char **items;
    int n = picker_path_c(ctx, start_path, &items);
    JSValue dir = file_system_locate(ctx, items[0], items, n);
    JSValue handle = JS_UNINITIALIZED;

    *ppath = JS_UNDEFINED;
    if (JS_IsNull(dir) || !file_system_is_directory(dir)) {
        JS_FreeValue(ctx, dir);
        picker_path_c_free(ctx, items, n);
        return handle;
    }
    if (suggested) {
        JSValue entry = file_system_child(ctx, dir, suggested);
        const char **path = malloc(sizeof *path * (size_t)(n + 1));
        int k;

        CHECK(path != NULL, "file picker: OOM building the saved file's locator path");
        for (k = 0; k < n; k++) path[k] = items[k];
        path[n] = suggested;
        if (JS_IsNull(entry)) {
            JS_FreeValue(ctx, entry);
            entry = file_system_create_file(ctx, dir, suggested);
        } else if (file_system_is_directory(entry)) {
            /* A directory of that name is not a file the picker can save over, and the standard's prompt would
               never have offered it. Nothing is selectable under the suggestion. */
            JS_FreeValue(ctx, entry);
            free((void *)path);
            JS_FreeValue(ctx, dir);
            picker_path_c_free(ctx, items, n);
            return handle;
        }
        /* "Set entry's binary data to an empty byte sequence" — which is the whole of what makes a save picker
           different from an open picker over an existing file, and it happens whether the entry was created
           here or was already there. */
        file_system_set_data(ctx, entry, file_system_bytes_value(ctx, "", 0));
        file_system_touch(ctx, entry);
        JS_FreeValue(ctx, entry);
        *ppath = picker_path_new(ctx, path, n + 1);
        handle = fs_handle_new(ctx, /*directory*/ false, items[0], path, n + 1);
        free((void *)path);
    } else {
        JSValue list = picker_select(ctx, start_path, M_OPEN, /*multiple*/ false, ppath);
        JSValue first = JS_GetPropertyUint32(ctx, list, 0);

        JS_FreeValue(ctx, list);
        if (JS_IsUndefined(first)) {
            JS_FreeValue(ctx, first);
        } else {
            const char **path;
            int np = picker_path_c(ctx, *ppath, &path);
            JSValue entry = file_system_locate(ctx, path[0], path, np);

            DCHECK(!JS_IsNull(entry) && file_system_is_file(entry),
                   "§3.4's selection located something that is not a file entry at the path picker_select just "
                   "took it from — the two walk the same model in the same flow");
            file_system_set_data(ctx, entry, file_system_bytes_value(ctx, "", 0));
            file_system_touch(ctx, entry);
            JS_FreeValue(ctx, entry);
            picker_path_c_free(ctx, path, np);
            handle = first;
        }
    }
    JS_FreeValue(ctx, dir);
    picker_path_c_free(ctx, items, n);
    return handle;
}

/* ---- the machine ------------------------------------------------------------------------------------------ */

/* ONE SPEC STEP PER STAGE, AND THE ENGINE IS ASKED AT EVERY ONE OF THEM.
 *
 * This was four stages, and the first of them named "§3.3/§3.4/§3.5 steps 1-3 and §3.1's verify steps 1-2" on
 * the ground that the checks in it run none of the page's code "and neither of which can therefore rest". That
 * is the reasoning core/idl_args.h and quickjs-step.h now forbid, and this member is where it was written down:
 * what makes a rest point necessary is the ENGINE — RAM pressure paging the low-value tail to the IDB cold
 * tier, a cross-session resume, a flow that outranks this one — and none of those events consult the page. A
 * span of four spec steps the engine cannot park inside is a cap, and the page being quiet across it changes
 * nothing about that.
 *
 * So the stages are the algorithm's steps, and each of them RETURNS: a machine that sets its stage and falls
 * through into the next `if` has crossed a boundary the driver never saw, so the label said "rest point" where
 * there was none. JS_STEP_YIELD is what hands the decision back — the scheduler parks the flow if something
 * outranks it and re-enters immediately if nothing does, which costs one predicted call per stage. The switch
 * is what makes the fall-through impossible to write again; there is no arm that runs two stages' bodies. */
#define FPK_STAGES(X)                                                                                          \
    X(FPK_ACCEPTS,   "File System Access §3.3/§3.4 step 2 (accepts options is the result of PROCESS ACCEPT "    \
                     "TYPES given options; §3.5 declares no `types` and its steps do not perform it)")          \
    X(FPK_STARTDIR,  "File System Access §3.3/§3.4/§3.5 step 3 (starting directory is DETERMINE THE DIRECTORY " \
                     "THE PICKER WILL START IN, given options[\"id\"], options[\"startIn\"] and environment — " \
                     "§3.2.25's union over `startIn` is that step's own input, which this member's declaration " \
                     "cannot express and which is therefore read here)")                                        \
    X(FPK_SUGGESTED, "File System Access §3.4's sanitization of `suggestedName` (\"If the suggestedName is "    \
                     "deemed too dangerous, user agents should ignore or sanitize the suggested file name\") — " \
                     "a walk of a string whose length the page chose")                                          \
    X(FPK_VERIFY,    "File System Access §3.3/§3.4/§3.5 step 5 -> §3.1's verify steps 1-2 (environment's "      \
                     "origin is not opaque, and is same origin with its top-level origin) — ONE O(1) engine "   \
                     "action, since an opaque origin is same origin with nothing and §7.2.5.1's single "        \
                     "comparison therefore decides both")                                                       \
    X(FPK_ACTIVE,    "File System Access §3.1's verify step 4 (global has TRANSIENT ACTIVATION — unknown "      \
                     "external state, so the SecurityError arm and the picker arm are two worlds and this is "  \
                     "where the flow forks)")                                                                   \
    X(FPK_DIALOG,    "File System Access §3.3/§3.5 step 7.4 and §3.4 step 7.3 (the file dialog's outcome — the " \
                     "USER'S decision, so the AbortError arm and the selection arm fork here)")                 \
    X(FPK_SELECT,    "File System Access §3.3/§3.4/§3.5 steps 7.5-7.9 (the prompt's selection over this "       \
                     "device's entries, the sensitivity judgement the standard leaves to the user agent, and "  \
                     "the handles it creates)")                                                                 \
    X(FPK_REMEMBER,  "File System Access §3.3/§3.4/§3.5 step 7.10 (REMEMBER A PICKED DIRECTORY, given "         \
                     "options[\"id\"], entries[0] and environment)")                                            \
    X(FPK_GRANT,     "File System Access §3.1's grant (\"at the time the promise ... resolves, permission "     \
                     "state for a descriptor with handle set to the returned handle, and mode set to read "     \
                     "should be granted\"), over every handle the selection produced")                          \
    X(FPK_NOTIFY,    "File System Access §3.3/§3.4/§3.5 step 7.11 (PERFORM THE ACTIVATION NOTIFICATION STEPS "  \
                     "in global's browsing context)")                                                           \
    X(FPK_SETTLE,    "File System Access §3.3/§3.4/§3.5 step 7.12's resolve, or the reject step 7.6 and §3.1 "  \
                     "reach (27.2.1.3.2 step 8's `then` read is the page's)")
enum { IDL_STEP_STAGE_BASE(FPK_STAGES) FPK_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const FPK_STEPS[] = { FPK_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    uint8_t cphase;
    uint8_t ua_phase;
    uint8_t reject;
    uint8_t started;
    JSValue promise;     /* owned */
    JSValue funcs[2];    /* the capability's [resolve, reject] (owned) */
    JSValue value;       /* what it settles with (owned) */
    JSValue start_path;  /* §3.2.2's answer, held across the dialog's fork (owned) */
    JSValue id;          /* options["id"] as read, carried for step 7.10 (owned) */
    JSValue suggested;   /* §3.4's sanitized suggestedName, or undefined (owned) */
    JSValue dismissed;   /* step 7.4's unknown (owned) */
    /* THE PICKED ENTRY'S PATH, held from step 7.9's selection to step 7.10's remember. It became a field when
       those two stopped being one stage: a value that survives a rest point cannot live in a C local, and it is
       owned here, so it is in `visit` — the one list a fork copies and a teardown discharges. */
    JSValue picked;
    JSValue cb[3];
} PickerState;

static void fpk_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    PickerState *s = st;
    int k;

    if (!s->started)
        return;
    v->val(ctx, &s->promise);
    v->val(ctx, &s->funcs[0]);
    v->val(ctx, &s->funcs[1]);
    v->val(ctx, &s->value);
    v->val(ctx, &s->start_path);
    v->val(ctx, &s->id);
    v->val(ctx, &s->suggested);
    v->val(ctx, &s->dismissed);
    v->val(ctx, &s->picked);
    STEP_CB_FOREACH(s->cb, k) v->val(ctx, &s->cb[k]);
}

static JSValue fpk_dom_error(JSContext *ctx, const char *name, const char *msg)
{
    JSValue e;

    JS_ThrowDOMException(ctx, name, "%s", msg);
    e = JS_GetException(ctx);
    CHECK(!JS_IsUndefined(e), "file picker: a DOMException was built and no exception was live");
    return e;
}

/* §3.4's "If the suggestedName is deemed too dangerous, user agents should ignore or sanitize the suggested
   file name, similar to the sanitization done when fetching something as a download." HTML §4.10.5.1.17's
   sanitization is to drop PATH COMPONENTS, which is what core/file/file_system.c does at the device's own
   edge; the same rule here, and a name that is still not a §2.1 VALID FILE NAME afterwards is IGNORED — the
   standard's own first option, and the only one that keeps this model's invariant that every entry is
   reachable by name from its parent. OWNED (JS_FreeCString), or NULL. */
static const char *picker_sanitize_suggested(JSContext *ctx, JSValueConst v)
{
    const char *raw, *base;
    size_t len;

    if (!JS_IsString(v)) return NULL;
    raw = JS_ToCStringLen(ctx, &len, v);
    CHECK(raw != NULL, "file picker: `suggestedName` could not be read back after its declared conversion");
    base = strrchr(raw, '/');
    base = base ? base + 1 : raw;
    {
        const char *b2 = strrchr(base, '\\');

        if (b2) base = b2 + 1;
    }
    if (!file_system_valid_name(base, strlen(base))) {
        JS_FreeCString(ctx, raw);
        return NULL;
    }
    if (base != raw) {
        JSValue trimmed = JS_NewString(ctx, base);
        const char *out = JS_ToCString(ctx, trimmed);

        JS_FreeValue(ctx, trimmed);
        JS_FreeCString(ctx, raw);
        return out;
    }
    return raw;
}

static int fpk_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                    JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    PickerState *s = st;
    int magic = idl_step_magic(hdr);
    JSValueConst options = argc > 0 ? argv[0] : JS_UNDEFINED;
    bool ok = false;
    int rc;

    *presult = JS_UNDEFINED;

    if (!s->started) {
        int k;

        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        s->promise = s->funcs[0] = s->funcs[1] = s->value = JS_UNDEFINED;
        s->start_path = s->id = s->suggested = s->dismissed = s->picked = JS_UNDEFINED;
        s->cphase = s->ua_phase = s->reject = 0;
        STEP_CB_FOREACH(s->cb, k) s->cb[k] = JS_UNDEFINED;
        s->started = 1;
        /* THE CAPABILITY BEFORE THE FIRST THING THAT CAN FAIL — see the file header: Web IDL §3.7.7 makes an
           abrupt completion of a promise-returning operation a rejection, so every failure below settles this
           promise rather than throwing past the `.catch` the page wrote. */
        s->promise = JS_NewPromiseCapability(ctx, s->funcs);
        if (JS_IsException(s->promise)) return -1;
    }

    /* ONE STAGE'S BODY PER ENTRY. Every arm below ends in a return, so the driver is between any two of this
       algorithm's steps — which is what makes each of them a rest point rather than a label claiming to be
       one. FPK_SETTLE is the one arm reached by an ordinary fall-out (`goto reject`), and it too is entered on
       its own invocation. */
    switch (hdr->stage) {
    case FPK_ACCEPTS:
        /* NO STAGE BELOW BUT FPK_SETTLE IS A CALL-RESUME, so an incoming result belongs to nothing in them and
           is released rather than carried to the settle — the settle has its own stage and its own entry.
           Releasing JS_UNDEFINED is a no-op, which is what this is on every entry but a re-entry after an
           abandoned request. */
        JS_FreeValue(ctx, cb_result);
        /* STEP 2 — process accept types, which showDirectoryPicker does not perform (DirectoryPickerOptions
           declares no `types` and §3.5's steps do not list it). */
        if (magic != M_DIRECTORY)
            picker_process_accept_types(ctx, options);
        STEP_GOTO(hdr->stage, FPK_STARTDIR, &s->cphase, &s->ua_phase, NULL);
        return JS_STEP_YIELD;

    case FPK_STARTDIR: {
        JSValue id_v, start_in;

        JS_FreeValue(ctx, cb_result);
        id_v = idl_dict_get(ctx, options, "id");
        start_in = idl_dict_get(ctx, options, "startIn");
        /* `StartInDirectory` is `(WellKnownDirectory or FileSystemHandle)` — a union whose two arms are an
           ENUMERATION and an INTERFACE, and core/idl_args.h declares neither shape of union, so the member
           crosses unconverted and §3.2.25's own order is applied here: a platform object implementing the
           interface crosses as itself, and everything else takes the enumeration arm, where a value outside
           the six is a TypeError. */
        if (!JS_IsUndefined(start_in) && !fs_handle_is(start_in)) {
            const char *w = JS_ToCString(ctx, start_in);
            int i;
            bool known = false;

            for (i = 0; w && WELL_KNOWN_DIRECTORY[i]; i++)
                if (!strcmp(w, WELL_KNOWN_DIRECTORY[i])) { known = true; break; }
            if (!known) {
                JS_ThrowTypeError(ctx, "'%s' is not a value of the `WellKnownDirectory` enumeration and is not "
                                       "a FileSystemHandle", w ? w : "");
                if (w) JS_FreeCString(ctx, w);
                JS_FreeValue(ctx, id_v);
                JS_FreeValue(ctx, start_in);
                s->value = JS_GetException(ctx);
                goto reject;
            }
            JS_FreeCString(ctx, w);
        }
        /* STEP 3 — determine the directory the picker will start in. */
        s->start_path = picker_starting_directory(ctx, id_v, start_in);
        JS_FreeValue(ctx, start_in);
        if (JS_IsUninitialized(s->start_path)) {
            s->start_path = JS_UNDEFINED;
            JS_FreeValue(ctx, id_v);
            s->value = JS_GetException(ctx);
            goto reject;
        }
        s->id = id_v;
        STEP_GOTO(hdr->stage, FPK_SUGGESTED, &s->cphase, &s->ua_phase, NULL);
        return JS_STEP_YIELD;
    }

    case FPK_SUGGESTED:
        JS_FreeValue(ctx, cb_result);
        if (magic == M_SAVE) {
            JSValue suggested_v = idl_dict_get(ctx, options, "suggestedName");
            const char *sug = picker_sanitize_suggested(ctx, suggested_v);

            JS_FreeValue(ctx, suggested_v);
            if (sug) {
                s->suggested = JS_NewString(ctx, sug);
                JS_FreeCString(ctx, sug);
            }
        }
        STEP_GOTO(hdr->stage, FPK_VERIFY, &s->cphase, &s->ua_phase, NULL);
        return JS_STEP_YIELD;

    case FPK_VERIFY:
        JS_FreeValue(ctx, cb_result);
        /* STEP 5 — §3.1's VERIFY, steps 1 and 2. An OPAQUE origin is same origin with NOTHING, not even with
           itself, so window_proxy_same_origin_with_top answers false for one and the two checks are one test
           through the one implementation of §7.2.5.1 — which is also why a second comparison here would be a
           second chance to read the opaque case differently. */
        if (!window_proxy_same_origin_with_top(ctx)) {
            s->value = fpk_dom_error(ctx, "SecurityError",
                                     "a file picker may be shown only by a document whose origin is not opaque "
                                     "and is same origin with its top-level origin");
            goto reject;
        }
        STEP_GOTO(hdr->stage, FPK_ACTIVE, &s->cphase, &s->ua_phase, NULL);
        return JS_STEP_YIELD;

    case FPK_ACTIVE:
        JS_FreeValue(ctx, cb_result);
        /* §3.1's VERIFY step 4. */
        rc = user_activation_transient_run(ctx, hdr, &s->ua_phase, &ok);
        if (rc) return rc;
        if (!ok) {
            s->value = fpk_dom_error(ctx, "SecurityError",
                                     "showing a file picker requires transient user activation");
            goto reject;
        }
        /* STEP 7.4's `dismissed`, minted here so the fork at the next stage has an operand the SNAPSHOT
           carries. A starting directory with NOTHING SELECTABLE is not a second unknown but a decided one:
           there is nothing for step 7.5's prompt to offer, so no selection is reachable and the dismissal is
           the only completion — forking it would park a sibling flow that does what this one does. */
        s->dismissed = concolic_source_wrap(ctx, PICKER_DIALOG_SHAPE, PICKER_DIALOG_SRC, JS_FALSE);
        CHECK(!JS_IsException(s->dismissed), "file picker: the dialog's outcome could not be allocated");
        STEP_GOTO(hdr->stage, FPK_DIALOG, &s->cphase, &s->ua_phase, NULL);
        return JS_STEP_YIELD;

    case FPK_DIALOG: {
        int arm = 0;

        JS_FreeValue(ctx, cb_result);
        if (concolic_is(s->dismissed)) {
            rc = step_fork_run(ctx, hdr, s->dismissed, PICKER_DIALOG_OP, 2, &arm);
            if (rc) return rc;
        } else {
            /* A host with no source overlay (a conformance run) gets the plain example back, so there is one
               completion and it is the one the modelled device produces. */
            arm = JS_ToBool(ctx, s->dismissed) ? 1 : 0;
        }
        if (arm != 0) {
            /* STEP 7.6 — "If dismissed is true ... reject p with an AbortError DOMException and abort." */
            s->value = fpk_dom_error(ctx, "AbortError", "the file picker was dismissed");
            goto reject;
        }
        STEP_GOTO(hdr->stage, FPK_SELECT, &s->cphase, &s->ua_phase, NULL);
        return JS_STEP_YIELD;
    }

    case FPK_SELECT: {
        JSValue result = JS_UNDEFINED;
        bool empty;

        JS_FreeValue(ctx, cb_result);
        /* STEPS 7.5-7.9. "If entry is deemed too sensitive or dangerous to be exposed to this website by the
           user agent" is a judgement this user agent makes for no entry: everything on this device was put
           there by the host through core/file/file_system.c's one edge, or written by the page itself, so
           there is no notion of sensitivity here to consult — and inventing one would suppress exactly the
           attacker-chosen file the device exists to deliver. */
        if (magic == M_SAVE) {
            const char *sug = JS_IsString(s->suggested) ? JS_ToCString(ctx, s->suggested) : NULL;

            result = picker_select_save(ctx, s->start_path, sug, &s->picked);
            if (sug) JS_FreeCString(ctx, sug);
            empty = JS_IsUninitialized(result);
            if (empty) result = JS_UNDEFINED;
        } else {
            bool multiple = (magic == M_OPEN && idl_dict_bool(ctx, options, "multiple"));
            JSValue list = picker_select(ctx, s->start_path, magic, multiple, &s->picked);
            JSValue len_v = JS_GetPropertyStr(ctx, list, "length");
            uint32_t n = 0;

            JS_ToUint32(ctx, &n, len_v);
            JS_FreeValue(ctx, len_v);
            empty = (n == 0);
            if (magic == M_DIRECTORY) {
                /* §3.5 resolves with ONE FileSystemDirectoryHandle rather than with a list. */
                result = empty ? JS_UNDEFINED : JS_GetPropertyUint32(ctx, list, 0);
                JS_FreeValue(ctx, list);
            } else {
                result = list;
            }
        }
        if (empty) {
            /* STEP 7.6's other half — "or if the user dismissed the prompt without making a selection". */
            JS_FreeValue(ctx, result);
            JS_FreeValue(ctx, s->picked);
            s->picked = JS_UNDEFINED;
            s->value = fpk_dom_error(ctx, "AbortError",
                                     "the file picker was dismissed without a selection being made");
            goto reject;
        }
        s->value = result;                                        /* step 7.12's `result`, settled at the end */
        STEP_GOTO(hdr->stage, FPK_REMEMBER, &s->cphase, &s->ua_phase, NULL);
        return JS_STEP_YIELD;
    }

    case FPK_REMEMBER:
        JS_FreeValue(ctx, cb_result);
        /* STEP 7.10 — remember a picked directory, given options["id"], entries[0] and environment. */
        DCHECK(!JS_IsUndefined(s->picked),
               "a file picker made a selection and recorded no path for it — §3.3 step 7.10 remembers the "
               "directory entries[0] was picked from, and the path is the only thing that names it");
        picker_remember(ctx, s->id, s->picked, magic == M_DIRECTORY);
        JS_FreeValue(ctx, s->picked);
        s->picked = JS_UNDEFINED;
        STEP_GOTO(hdr->stage, FPK_GRANT, &s->cphase, &s->ua_phase, NULL);
        return JS_STEP_YIELD;

    case FPK_GRANT:
        JS_FreeValue(ctx, cb_result);
        /* FILE SYSTEM ACCESS §3.1's GRANT, which is the sentence that makes the returned handle usable: "at
           the time the promise ... resolves, permission state for a descriptor with handle set to the returned
           handle, and mode set to read should be granted", and additionally readwrite for showSaveFilePicker. */
        if (magic == M_OPEN) {
            uint32_t i, n = 0;
            JSValue len_v = JS_GetPropertyStr(ctx, s->value, "length");

            JS_ToUint32(ctx, &n, len_v);
            JS_FreeValue(ctx, len_v);
            for (i = 0; i < n; i++) {
                JSValue h = JS_GetPropertyUint32(ctx, s->value, i);

                fs_access_grant(ctx, h, /*readwrite*/ false);
                JS_FreeValue(ctx, h);
            }
        } else {
            fs_access_grant(ctx, s->value, /*readwrite*/ magic == M_SAVE);
        }
        STEP_GOTO(hdr->stage, FPK_NOTIFY, &s->cphase, &s->ua_phase, NULL);
        return JS_STEP_YIELD;

    case FPK_NOTIFY:
        JS_FreeValue(ctx, cb_result);
        /* STEP 7.11 — "Perform the activation notification steps in global's browsing context." Its own note
           says what it is for: "this lets a website immediately perform operations on the returned handles
           that might require user activation, such as requesting more permissions" — which in this engine is
           §2.3.2's requestPermission, whose step 6 is the very transient-activation gate this restores. */
        user_activation_notify(ctx);
        STEP_GOTO(hdr->stage, FPK_SETTLE, &s->cphase, &s->ua_phase, NULL);
        return JS_STEP_YIELD;

    case FPK_SETTLE: {
        JSValue settled = JS_UNDEFINED;

        rc = step_call_run(ctx, &s->cphase, STEP_CB(s->cb), s->funcs[s->reject], JS_UNDEFINED, 1,
                           (JSValueConst *)&s->value, cb_result, &settled, out_cb, out_argc);
        if (rc > 0) return rc;
        JS_FreeValue(ctx, settled);
        *presult = s->promise;
        s->promise = JS_UNDEFINED;
        return JS_STEP_DONE;
    }
    }
    DFAIL("a file picker was stepped at a stage §3.3-§3.5 does not declare");
    return -1;

reject:
    s->reject = 1;
    STEP_GOTO(hdr->stage, FPK_SETTLE, &s->cphase, &s->ua_phase, NULL);
    return JS_STEP_YIELD;
}

static const IdlStepDecl FPK_DECL = {
    /* No release. This WAS fpk_visit's list a second time, ending in `started = 0` — which lowered the very
       condition the visit reads, so the discharge that runs next walked away from a state holding all of it.
       The declaration is the one list; the teardown reads it after the completion is stated. */
    fpk_step, sizeof(PickerState), fpk_visit, NULL,
    "File System Access §3.3-§3.5 the local file system handle factories", FPK_STEPS
};

/* ---- the three option dictionaries -------------------------------------------------------------------------
 *
 * WEB IDL §3.2.18's READ ORDER IS THE DECLARATION'S ORDER: a dictionary's INHERITED members are read first and
 * each dictionary's own members LEXICOGRAPHICALLY among themselves. `OpenFilePickerOptions : FilePickerOptions`
 * therefore reads excludeAcceptAllOption, id, startIn, types (the base's four, sorted) and then multiple — an
 * order no single sorted list produces, which is what the `level` column exists to express.
 *   `types` is declared IDL_ANY rather than left out, so a page that passes one is SEEN: process accept types
 * crashes naming the conversion that is missing, where an undeclared member would silently be ignored and the
 * page would get a picker with no filter at all. `startIn` is IDL_ANY for the union reason its own site
 * states. */
static const IdlDictMember OPEN_OPTIONS[] = {
    { "excludeAcceptAllOption", IDL_BOOLEAN, false, NULL, 0, NULL, IDL_DEFAULT_NONE, NULL },
    { "id",                     IDL_DOMSTRING, false, NULL, 0, NULL, IDL_DEFAULT_NONE, NULL },
    { "startIn",                IDL_ANY,      false, NULL, 0, NULL, IDL_DEFAULT_NONE, NULL },
    { "types",                  IDL_ANY,      false, NULL, 0, NULL, IDL_DEFAULT_NONE, NULL },
    { "multiple",               IDL_BOOLEAN,  false, NULL, 1, NULL, IDL_DEFAULT_NONE, NULL }
};
static const IdlDictMember SAVE_OPTIONS[] = {
    { "excludeAcceptAllOption", IDL_BOOLEAN, false, NULL, 0, NULL, IDL_DEFAULT_NONE, NULL },
    { "id",                     IDL_DOMSTRING, false, NULL, 0, NULL, IDL_DEFAULT_NONE, NULL },
    { "startIn",                IDL_ANY,      false, NULL, 0, NULL, IDL_DEFAULT_NONE, NULL },
    { "types",                  IDL_ANY,      false, NULL, 0, NULL, IDL_DEFAULT_NONE, NULL },
    { "suggestedName",          IDL_USVSTRING_NULLABLE, false, NULL, 1, NULL, IDL_DEFAULT_NONE, NULL }
};
/* DirectoryPickerOptions inherits from nothing, so its three are one lexicographic list. `mode` is read and
   converted here even though §3.5's steps never consult it: the conversion is OBSERVABLE — `mode: "bogus"` is
   a TypeError before the picker is shown — and its effect on what the returned handle may do is §3.5's own
   note that "the user agent can combine read and write permission requests on this handle into one subsequent
   prompt", which is a prompt this agent does not combine. */
static const IdlDictMember DIRECTORY_OPTIONS[] = {
    { "id",      IDL_DOMSTRING, false, NULL,           0, NULL, IDL_DEFAULT_NONE,   NULL },
    { "mode",    IDL_ENUM,      false, FS_MODE_VALUES, 0, NULL, IDL_DEFAULT_STRING, "read" },
    { "startIn", IDL_ANY,       false, NULL,           0, NULL, IDL_DEFAULT_NONE,   NULL }
};

static void file_picker_install_realm(JSContext *ctx)
{
    JSValue global = JS_GetGlobalObject(ctx);

    DCHECK(g_id_open >= 0, "§3's factories were installed before file_picker_init declared them");
    /* The whole partial interface is `[SecureContext]`, and Web IDL §3.3.13 REMOVES a member in a non-secure
       realm rather than making it throw — `'showOpenFilePicker' in window` is what a bundle feature-detects
       with, and the three answers (absent, throwing, undefined) are three different branches. It is also what
       keeps this consistent with the handles themselves, which core/file/file_system_handle.c does not build
       in such a realm at all. */
    idl_install_method_exposed(ctx, global, "showOpenFilePicker", 0, g_id_open, IDL_SECURE_CONTEXT);
    idl_install_method_exposed(ctx, global, "showSaveFilePicker", 0, g_id_save, IDL_SECURE_CONTEXT);
    idl_install_method_exposed(ctx, global, "showDirectoryPicker", 0, g_id_directory, IDL_SECURE_CONTEXT);
    JS_FreeValue(ctx, global);
}

void file_picker_init(JSContext *ctx)
{
    static const IdlArgType OPTS_ONLY[] = { IDL_DICT };

    DCHECK(g_id_open < 0, "file_picker_init ran twice — the three members' pool ids and §3.2.2's map are the "
                          "AGENT's");
    g_rt = JS_GetRuntime(ctx);
    /* §3.2.2's MAP, BUILT AT THE PRE-BOOT BASELINE. A map allocated lazily inside a flow would be that flow's
       own object and no sibling would ever see what it remembered. */
    g_recent = JS_NewObject(ctx);
    CHECK(!JS_IsException(g_recent), "§3.2.2's recently picked directory map could not be allocated");
    g_id_open = idl_method_id_step(ctx, OPTS_ONLY, 1, OPEN_OPTIONS,
                                   (int)(sizeof OPEN_OPTIONS / sizeof OPEN_OPTIONS[0]), &FPK_DECL, M_OPEN);
    idl_optional_from(0);
    g_id_save = idl_method_id_step(ctx, OPTS_ONLY, 1, SAVE_OPTIONS,
                                   (int)(sizeof SAVE_OPTIONS / sizeof SAVE_OPTIONS[0]), &FPK_DECL, M_SAVE);
    idl_optional_from(0);
    g_id_directory = idl_method_id_step(ctx, OPTS_ONLY, 1, DIRECTORY_OPTIONS,
                                        (int)(sizeof DIRECTORY_OPTIONS / sizeof DIRECTORY_OPTIONS[0]),
                                        &FPK_DECL, M_DIRECTORY);
    idl_optional_from(0);
    /* THE THREE MEMBERS ARE `partial interface Window`'s AND EVERY REALM IS A WINDOW, so they are declared
       into core/realm.h's one list rather than installed by each host: a `file_picker_install` line hand-copied
       into each host's realm builder is exactly the list that file exists to abolish, and a realm missing from
       one copy would silently have no pickers with nothing to say so. */
    realm_declare_intrinsic(file_picker_install_realm);
}

void file_picker_free(void)
{
    if (g_id_open < 0)
        return;
    DCHECK(g_rt != NULL, "§3.2.2's map was built without recording the runtime that owns it");
    JS_FreeValueRT(g_rt, g_recent);
    g_recent = JS_UNDEFINED;
    g_id_open = g_id_save = g_id_directory = -1;
    g_rt = NULL;
}
