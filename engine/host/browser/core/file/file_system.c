/* THE ONE VIRTUAL FILESYSTEM — File System Standard §2.1's data model, built as real state.
 *
 * WHY IT EXISTS AT ALL. §Headless is not valueless: the only piece a headless engine is missing is a physical
 * IO device, and the spec still defines every algorithm without one. So the device is MODELLED — entries, their
 * binary data, their children, their locks — and every one of §2.3, §2.4 and §2.5's algorithms then runs
 * exactly as written. "This engine has no filesystem" is the excuse this file deletes.
 *
 * THERE IS ONE STORE AND TWO ROOTS. HTML §4.10.5.1.17's file control chooses from the user's disk, §3's bucket
 * file system is reached with no prompt at all, and File System Access's pickers choose from the disk again;
 * those are three doors onto TWO ROOTS of ONE model, never three stores. The mock device that
 * core/html/input_picker.c already selects from is the LOCAL root of this model — it was a malloc'd array of
 * name/type/bytes triples with no directories, no writes and no isolation, and that array is DELETED here
 * rather than left beside this as a second store to drift from.
 *
 * WHY IT IS BUILT OUT OF JS VALUES, WHICH IS THE LOAD-BEARING DECISION. A filesystem is mutable state A FLOW
 * WRITES: two forked flows that write the same file must not see each other's bytes, and a flow parked at an
 * `await` in the middle of a write must resume seeing exactly what it wrote. §State isolation names the one
 * primitive for that — the per-flow heap COW delta, which captures PROPERTY WRITES — and names the failure
 * mode of the alternative: a malloc'd list captured as pointers reverts the POINTERS on a context switch and
 * leaves the nodes reachable from nothing. So an entry is an internal-slot record (core/idl_slots.h), a
 * directory's children are a record keyed by name, and a file's binary data is a value ON a record. Every
 * mutation this file performs is therefore a property write the delta already captures, the snapshot machinery
 * already carries, and the cold tier already knows how to park — with no new delta kind and nothing for a
 * component to remember.
 *
 * AND IT IS WHAT LETS A FILE'S BYTES BE ATTACKER INPUT. A file's contents are external input in the same sense
 * `location.hash` is; §Attacker sources says a virtual system exists so forced execution can reach more code,
 * and names the case — an FSA file containing XSS. A `char *` cannot hold a concolic value; a JSValue can, so
 * `entry.data` is the real bytes on a conformance run and a source-tagged concolic carrying those same bytes as
 * its EXAMPLE wherever the source overlay is installed. Everything downstream — the File a page reads, the
 * string `text()` answers with, the byte the write algorithm concatenates — is then the ordinary value the
 * interpreter already propagates through. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/file/blob.h"
#include "core/file/file_system.h"
#include "core/idl_slots.h"
#include "core/timing/event_loop.h"
#include "solver/concolic.h"

/* §2.1's FIELD NAMES, on the record an entry IS. They are spelled once here so the model and every reader of it
   cannot disagree about which name holds what. */
#define FS_KIND     "kind"
#define FS_NAME     "name"
#define FS_DATA     "data"       /* a file entry's BINARY DATA */
#define FS_MODIFIED "modified"   /* a file entry's MODIFICATION TIMESTAMP */
#define FS_LOCK     "lock"       /* a file entry's LOCK */
#define FS_SHARED   "shared"     /* a file entry's SHARED LOCK COUNT */
#define FS_CHILDREN "children"   /* a directory entry's CHILDREN */
#define FS_TYPE     "type"       /* the MIME type the device recorded — see the header */

#define FS_KIND_FILE      "file"
#define FS_KIND_DIRECTORY "directory"

/* §2.1's three lock values. "a string that may exclusively be 'open', 'taken-exclusive' or 'taken-shared'". */
#define FS_LOCK_OPEN      "open"
#define FS_LOCK_EXCLUSIVE "taken-exclusive"
#define FS_LOCK_SHARED    "taken-shared"

/* THE TWO ROOTS, HELD BY THE AGENT. §Security: an instance IS an origin-keyed agent cluster, and storage is
   keyed by origin — so the filesystem of a same-origin child navigable is THIS filesystem, not a second one,
   and it is agent state rather than realm state. The records carry NO prototype (core/idl_slots.h), so holding
   them here gives no realm's object to another realm, which is the only thing agent-wide storage could get
   wrong. */
static JSValue g_bucket_root = JS_UNDEFINED;
static JSValue g_local_root  = JS_UNDEFINED;
static int     g_ready;

/* ---- §2.1's byte sequences, as JS values ------------------------------------------------------------------
 *
 * ONE CODE UNIT PER BYTE, and the conversion is spelled out rather than borrowed from an encoding: a byte
 * sequence is not text, so decoding it as UTF-8 would replace every byte a decoder rejects with U+FFFD and
 * `getFile()` would answer with something that is not the file. The two directions below are exact inverses
 * over 0x00..0xFF and assert it, which is the whole contract. */

static JSValue fs_bytes_to_string(JSContext *ctx, const char *bytes, size_t len)
{
    /* Each byte becomes ONE code point in 0x00..0xFF, whose UTF-8 form is one byte below 0x80 and two above —
       so the buffer this builds is what JS_NewStringLen must decode to get those code points back. */
    char *utf8 = malloc(len * 2 + 1);
    size_t i, n = 0;
    JSValue s;

    CHECK(utf8 != NULL, "file system: OOM building a byte sequence's value");
    for (i = 0; i < len; i++) {
        unsigned char b = (unsigned char)bytes[i];

        if (b < 0x80) {
            utf8[n++] = (char)b;
        } else {
            utf8[n++] = (char)(0xC0 | (b >> 6));
            utf8[n++] = (char)(0x80 | (b & 0x3F));
        }
    }
    utf8[n] = 0;
    s = JS_NewStringLen(ctx, utf8, n);
    free(utf8);
    CHECK(!JS_IsException(s), "file system: a byte sequence's value could not be allocated");
    return s;
}

JSValue file_system_bytes_value(JSContext *ctx, const char *bytes, size_t len)
{
    DCHECK(bytes != NULL || len == 0, "a byte sequence with a length and no bytes");
    return fs_bytes_to_string(ctx, bytes ? bytes : "", len);
}

char *file_system_value_bytes(JSContext *ctx, JSValueConst v, size_t *plen)
{
    /* A CONCOLIC ANSWERS WITH ITS EXAMPLE, which is what makes an attacker-controlled file still produce a real
       File the page can read: the domain says the bytes are unknown and the example says what they concretely
       are right now, and every consumer that needs actual bytes — §2.3.1's File, the write algorithm's
       concatenation — takes the second half. The taint is not lost by this: it rides the VALUE, and the value
       is what reaches the page. */
    JSValue ex = concolic_is(v) ? concolic_example(ctx, v) : JS_DupValue(ctx, v);
    const char *utf8;
    size_t ulen = 0, i, n = 0;
    char *out;

    DCHECK(JS_IsString(ex), "a file entry's BINARY DATA is not a byte sequence — §2.1 makes it a byte sequence, "
                            "and this model spells one as a string with one code unit per byte, so anything "
                            "else is a writer that stored something that is not the file's contents");
    utf8 = JS_ToCStringLen(ctx, &ulen, ex);
    CHECK(utf8 != NULL, "file system: a byte sequence's value could not be read back");
    out = malloc(ulen + 1);
    CHECK(out != NULL, "file system: OOM reading a byte sequence back");
    for (i = 0; i < ulen; ) {
        unsigned char c = (unsigned char)utf8[i];

        if (c < 0x80) {
            out[n++] = (char)c;
            i++;
        } else {
            DCHECK((c == 0xC2 || c == 0xC3) && i + 1 < ulen,
                   "a file entry's BINARY DATA held a code point above U+00FF — a byte sequence has one code "
                   "unit per BYTE, so a wider one means text was stored where bytes belong (encode it at the "
                   "writer: §2.5's write algorithm UTF-8 encodes a USVString before it ever reaches the buffer)");
            out[n++] = (char)(((c & 0x03) << 6) | ((unsigned char)utf8[i + 1] & 0x3F));
            i += 2;
        }
    }
    out[n] = 0;
    JS_FreeCString(ctx, utf8);
    JS_FreeValue(ctx, ex);
    if (plen) *plen = n;
    return out;
}

/* ---- the entry record --------------------------------------------------------------------------------- */

static JSValue fs_get(JSContext *ctx, JSValueConst entry, const char *field)
{
    JSValue v;

    DCHECK(JS_IsObject(entry), "a file system entry field was read off something that is not an entry");
    v = JS_GetPropertyStr(ctx, entry, field);
    CHECK(!JS_IsException(v), "file system: an entry's field could not be read");
    return v;
}

static bool fs_kind_is(JSValueConst entry, const char *want, JSContext *ctx)
{
    JSValue k;
    const char *s;
    bool hit;

    if (!JS_IsObject(entry)) return false;
    k = fs_get(ctx, entry, FS_KIND);
    s = JS_ToCString(ctx, k);
    DCHECK(s != NULL, "a file system entry has no KIND — §2.1 makes every entry a file entry or a directory "
                      "entry, and the record is built with one or the other");
    hit = s && !strcmp(s, want);
    JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, k);
    return hit;
}

/* THE TWO KIND TESTS TAKE NO CONTEXT in the header, because every caller has one and none of them should have
   to pass it to ask what an entry IS. The context they use is the runtime's — a record's own property read
   needs no realm, since the record carries no prototype for a realm to have given it. */
static JSContext *g_fs_ctx;   /* the AGENT's context, for the two ctx-less predicates below */

bool file_system_is_directory(JSValueConst entry)
{
    DCHECK(g_fs_ctx != NULL, "a file system entry's kind was asked before file_system_init built the model");
    return fs_kind_is(entry, FS_KIND_DIRECTORY, g_fs_ctx);
}

bool file_system_is_file(JSValueConst entry)
{
    DCHECK(g_fs_ctx != NULL, "a file system entry's kind was asked before file_system_init built the model");
    return fs_kind_is(entry, FS_KIND_FILE, g_fs_ctx);
}

const char *file_system_name_cstr(JSContext *ctx, JSValueConst entry)
{
    JSValue v = fs_get(ctx, entry, FS_NAME);
    const char *s = JS_ToCString(ctx, v);

    JS_FreeValue(ctx, v);
    DCHECK(s != NULL, "a file system entry has no NAME — §2.1 gives every entry one and every constructor here "
                      "sets it");
    return s;
}

const char *file_system_type_cstr(JSContext *ctx, JSValueConst file)
{
    JSValue v = fs_get(ctx, file, FS_TYPE);
    const char *s;

    DCHECK(file_system_is_file(file), "a MIME type was read off a directory entry");
    s = JS_ToCString(ctx, v);
    JS_FreeValue(ctx, v);
    DCHECK(s != NULL, "a file entry has no recorded MIME type — every file entry is born with one, \"\" for "
                      "none, so an absent one is a constructor that skipped it");
    return s;
}

static JSValue fs_children(JSContext *ctx, JSValueConst dir)
{
    JSValue kids;

    DCHECK(file_system_is_directory(dir), "§2.1's CHILDREN were asked of an entry that is not a directory entry "
                                          "— only a directory entry has a set of children");
    kids = fs_get(ctx, dir, FS_CHILDREN);
    DCHECK(JS_IsObject(kids), "a directory entry's CHILDREN is not a set");
    return kids;
}

/* A NEW ENTRY. Both constructors go through it so the two kinds cannot come apart, and both take the NAME the
   algorithm that creates them was given. */
static JSValue fs_entry_new(JSContext *ctx, const char *kind, const char *name)
{
    JSValue e = idl_slots_new(ctx);

    CHECK(!JS_IsException(e), "file system: an entry could not be allocated");
    JS_SetPropertyStr(ctx, e, FS_KIND, JS_NewString(ctx, kind));
    JS_SetPropertyStr(ctx, e, FS_NAME, JS_NewString(ctx, name));
    if (!strcmp(kind, FS_KIND_DIRECTORY)) {
        JSValue kids = idl_slots_new(ctx);

        CHECK(!JS_IsException(kids), "file system: a directory entry's children could not be allocated");
        JS_SetPropertyStr(ctx, e, FS_CHILDREN, kids);
    } else {
        /* §2.4.2's create branch: "set child's binary data to an empty byte sequence", "set child's
           modification timestamp to the current time", and §2.1's lock, "initially open". */
        JS_SetPropertyStr(ctx, e, FS_DATA, JS_NewStringLen(ctx, "", 0));
        JS_SetPropertyStr(ctx, e, FS_MODIFIED, JS_NewFloat64(ctx, event_loop_now(ctx)));
        JS_SetPropertyStr(ctx, e, FS_LOCK, JS_NewString(ctx, FS_LOCK_OPEN));
        JS_SetPropertyStr(ctx, e, FS_SHARED, JS_NewInt32(ctx, 0));
        JS_SetPropertyStr(ctx, e, FS_TYPE, JS_NewStringLen(ctx, "", 0));
    }
    return e;
}

/* ---- §2.1's children set ------------------------------------------------------------------------------- */

JSValue file_system_child(JSContext *ctx, JSValueConst dir, const char *name)
{
    JSValue kids = fs_children(ctx, dir), child;

    child = JS_GetPropertyStr(ctx, kids, name);
    JS_FreeValue(ctx, kids);
    CHECK(!JS_IsException(child), "file system: a directory entry's child could not be read");
    /* A RECORD WITH NO PROTOTYPE ANSWERS `undefined` FOR AN ABSENT NAME, and §2.4's algorithms are stated as
       "for each child ... if child's name equals name" with a fall-through — so absent is the algorithm's own
       "no such child", reported as the JS_NULL §2.1's locate an entry uses for the same fact. */
    if (JS_IsUndefined(child)) return JS_NULL;
    DCHECK(JS_IsObject(child), "a directory entry's children held something that is not a file system entry");
    return child;
}

int file_system_child_count(JSContext *ctx, JSValueConst dir)
{
    JSValue kids = fs_children(ctx, dir);
    JSPropertyEnum *tab = NULL;
    uint32_t n = 0;
    int r;

    r = JS_GetOwnPropertyNames(ctx, &tab, &n, kids, JS_GPN_STRING_MASK);
    CHECK(r == 0, "file system: a directory entry's children could not be enumerated");
    JS_FreePropertyEnum(ctx, tab, n);
    JS_FreeValue(ctx, kids);
    return (int)n;
}

JSValue file_system_child_at(JSContext *ctx, JSValueConst dir, int i, JSValue *pname)
{
    JSValue kids = fs_children(ctx, dir), child = JS_NULL;
    JSPropertyEnum *tab = NULL;
    uint32_t n = 0;
    int r;

    r = JS_GetOwnPropertyNames(ctx, &tab, &n, kids, JS_GPN_STRING_MASK);
    CHECK(r == 0, "file system: a directory entry's children could not be enumerated");
    DCHECK(i >= 0 && (uint32_t)i < n, "§2.4.1's iteration asked for a child at an index the directory entry "
                                      "does not have");
    if (i >= 0 && (uint32_t)i < n) {
        if (pname) *pname = JS_AtomToString(ctx, tab[i].atom);
        child = JS_GetProperty(ctx, kids, tab[i].atom);
        CHECK(!JS_IsException(child), "file system: a directory entry's child could not be read");
    } else if (pname) {
        *pname = JS_UNDEFINED;
    }
    JS_FreePropertyEnum(ctx, tab, n);
    JS_FreeValue(ctx, kids);
    return child;
}

bool file_system_children_empty(JSContext *ctx, JSValueConst dir)
{
    return file_system_child_count(ctx, dir) == 0;
}

JSValue file_system_create_file(JSContext *ctx, JSValueConst dir, const char *name)
{
    JSValue kids = fs_children(ctx, dir), child;

    DCHECK(file_system_valid_name(name, strlen(name)),
           "§2.4.2's create branch was reached with a name that is not a VALID FILE NAME — step 1 rejects one "
           "with a TypeError before the entry is even located, so a name arriving here means a caller skipped "
           "its own first step");
    child = fs_entry_new(ctx, FS_KIND_FILE, name);
    /* "Append child to entry's children" — an ordinary property write, which is what the per-flow COW delta
       captures, so a flow that created a file has one and its sibling does not. */
    JS_SetPropertyStr(ctx, kids, name, JS_DupValue(ctx, child));
    JS_FreeValue(ctx, kids);
    return child;
}

JSValue file_system_create_directory(JSContext *ctx, JSValueConst dir, const char *name)
{
    JSValue kids = fs_children(ctx, dir), child;

    DCHECK(file_system_valid_name(name, strlen(name)),
           "§2.4.3's create branch was reached with a name that is not a VALID FILE NAME — step 1 rejects one "
           "with a TypeError before the entry is even located");
    child = fs_entry_new(ctx, FS_KIND_DIRECTORY, name);
    JS_SetPropertyStr(ctx, kids, name, JS_DupValue(ctx, child));
    JS_FreeValue(ctx, kids);
    return child;
}

void file_system_remove_child(JSContext *ctx, JSValueConst dir, const char *name)
{
    JSValue kids = fs_children(ctx, dir);
    JSAtom a = JS_NewAtom(ctx, name);
    int r;

    CHECK(a != JS_ATOM_NULL, "file system: a child's name could not be interned to remove it");
    r = JS_DeleteProperty(ctx, kids, a, 0);
    DCHECK(r > 0, "§2.4.4 removed a child the directory entry does not have — the algorithm finds the child by "
                  "name first and only removes what it found");
    JS_FreeAtom(ctx, a);
    JS_FreeValue(ctx, kids);
}

/* ---- a file entry's binary data and timestamp ------------------------------------------------------------ */

JSValue file_system_data(JSContext *ctx, JSValueConst file)
{
    DCHECK(file_system_is_file(file), "§2.1's BINARY DATA was read off an entry that is not a file entry");
    return fs_get(ctx, file, FS_DATA);
}

void file_system_set_data(JSContext *ctx, JSValueConst file, JSValue data)
{
    DCHECK(file_system_is_file(file), "§2.1's BINARY DATA was written to an entry that is not a file entry");
    DCHECK(JS_IsString(data) || concolic_is(data),
           "a file entry's BINARY DATA was set to something that is neither a byte sequence nor an unknown one "
           "— §2.5's close algorithm sets it to the stream's [[buffer]], which is a byte sequence, and the only "
           "other shape is the source a file's contents are when this engine did not compute them");
    JS_SetPropertyStr(ctx, file, FS_DATA, data);
}

double file_system_modified(JSContext *ctx, JSValueConst file)
{
    JSValue v = file_system_is_file(file) ? fs_get(ctx, file, FS_MODIFIED) : JS_UNDEFINED;
    double d = 0;

    DCHECK(JS_IsNumber(v), "§2.1's MODIFICATION TIMESTAMP is not a number — it is \"a number representing the "
                           "number of milliseconds since the Unix Epoch\" and every writer here sets one");
    JS_ToFloat64(ctx, &d, v);
    JS_FreeValue(ctx, v);
    return d;
}

void file_system_touch(JSContext *ctx, JSValueConst file)
{
    DCHECK(file_system_is_file(file), "a modification timestamp was written to an entry that is not a file");
    JS_SetPropertyStr(ctx, file, FS_MODIFIED, JS_NewFloat64(ctx, event_loop_now(ctx)));
}

/* ---- §2.1's locks ---------------------------------------------------------------------------------------- */

static void fs_lock_set(JSContext *ctx, JSValueConst file, const char *value)
{
    JS_SetPropertyStr(ctx, file, FS_LOCK, JS_NewString(ctx, value));
}

static const char *fs_lock_of(JSContext *ctx, JSValueConst file, JSValue *hold)
{
    const char *s;

    *hold = fs_get(ctx, file, FS_LOCK);
    s = JS_ToCString(ctx, *hold);
    DCHECK(s != NULL, "a file entry has no LOCK — §2.1 gives every file entry one, initially \"open\"");
    return s;
}

static int fs_shared_count(JSContext *ctx, JSValueConst file)
{
    JSValue v = fs_get(ctx, file, FS_SHARED);
    int32_t n = 0;

    DCHECK(JS_IsNumber(v), "§2.1's SHARED LOCK COUNT is not a number");
    JS_ToInt32(ctx, &n, v);
    JS_FreeValue(ctx, v);
    return (int)n;
}

bool file_system_take_lock(JSContext *ctx, JSValueConst file, bool exclusive)
{
    JSValue hold;
    const char *lock;
    bool ok = false;

    DCHECK(file_system_is_file(file), "§2.1's take a lock was run on an entry that is not a file entry");
    lock = fs_lock_of(ctx, file, &hold);
    if (exclusive) {
        if (lock && !strcmp(lock, FS_LOCK_OPEN)) {           /* value is "exclusive", lock is "open" */
            fs_lock_set(ctx, file, FS_LOCK_EXCLUSIVE);
            ok = true;
        }
    } else if (lock && !strcmp(lock, FS_LOCK_OPEN)) {        /* value is "shared", lock is "open" */
        fs_lock_set(ctx, file, FS_LOCK_SHARED);
        JS_SetPropertyStr(ctx, file, FS_SHARED, JS_NewInt32(ctx, 1));
        ok = true;
    } else if (lock && !strcmp(lock, FS_LOCK_SHARED)) {      /* "otherwise, if lock is taken-shared" */
        JS_SetPropertyStr(ctx, file, FS_SHARED, JS_NewInt32(ctx, fs_shared_count(ctx, file) + 1));
        ok = true;
    }
    JS_FreeCString(ctx, lock);
    JS_FreeValue(ctx, hold);
    return ok;
}

void file_system_release_lock(JSContext *ctx, JSValueConst file)
{
    JSValue hold;
    const char *lock;

    DCHECK(file_system_is_file(file), "§2.1's release a lock was run on an entry that is not a file entry");
    lock = fs_lock_of(ctx, file, &hold);
    if (lock && !strcmp(lock, FS_LOCK_SHARED)) {
        int count = fs_shared_count(ctx, file) - 1;

        DCHECK(count >= 0, "§2.1's release a lock drove a file entry's SHARED LOCK COUNT below zero — every "
                           "release answers a take, so a negative count is a stream that released twice");
        JS_SetPropertyStr(ctx, file, FS_SHARED, JS_NewInt32(ctx, count));
        if (count == 0) fs_lock_set(ctx, file, FS_LOCK_OPEN);
    } else {
        fs_lock_set(ctx, file, FS_LOCK_OPEN);
    }
    JS_FreeCString(ctx, lock);
    JS_FreeValue(ctx, hold);
}

/* ---- §2.1's valid file name ------------------------------------------------------------------------------ */

bool file_system_valid_name(const char *name, size_t len)
{
    size_t i;

    if (!name || len == 0) return false;                                  /* "is not an empty string" */
    if (len == 1 && name[0] == '.') return false;                         /* "is not equal to '.'" */
    if (len == 2 && name[0] == '.' && name[1] == '.') return false;       /* "or '..'" */
    for (i = 0; i < len; i++)
        if (name[i] == '/' || name[i] == '\\') return false;              /* the path separators */
    return true;
}

/* ---- §2.1's LOCATE AN ENTRY ------------------------------------------------------------------------------ */

JSValue file_system_root_entry(JSContext *ctx, const char *root)
{
    DCHECK(g_ready, "a file system root was asked for before file_system_init built the model");
    if (!strcmp(root, FS_ROOT_BUCKET)) return JS_DupValue(ctx, g_bucket_root);
    DCHECK(!strcmp(root, FS_ROOT_LOCAL),
           "a file system locator named a ROOT this engine does not have — §2.1's root is an opaque string "
           "whose value is implementation-defined, and this engine defines exactly two (the bucket file "
           "system's and the local file system's), so a third means a locator was built out of something that "
           "is not one of them");
    return JS_DupValue(ctx, g_local_root);
}

JSValue file_system_locate(JSContext *ctx, const char *root, const char *const *path, int npath)
{
    JSValue entry = file_system_root_entry(ctx, root);
    int i;

    DCHECK(npath >= 1, "§2.1's locate an entry was given a path with no items — a file system path is \"a list "
                       "of ONE OR MORE strings\", and its first item names the root");
    /* THE FIRST ITEM NAMES THE ROOT and the rest are the walk, which is what makes `handle.name` the last item
       of the path for the root as well as for a child. */
    for (i = 1; i < npath; i++) {
        JSValue child;

        if (!file_system_is_directory(entry)) {
            /* A path that descends THROUGH a file names nothing: §2.1's constraint is that locating an entry
               returns an entry or NULL, and a file entry has no children for the next item to be found in. */
            JS_FreeValue(ctx, entry);
            return JS_NULL;
        }
        child = file_system_child(ctx, entry, path[i]);
        JS_FreeValue(ctx, entry);
        if (JS_IsNull(child)) return JS_NULL;
        entry = child;
    }
    return entry;
}

/* ---- §2.3.1's File over an entry -------------------------------------------------------------------------- */

JSValue file_system_file_new(JSContext *ctx, JSValueConst entry)
{
    JSValue data, f;
    const char *type, *name;
    size_t len = 0;
    char *bytes;

    DCHECK(file_system_is_file(entry), "§2.3.1's File was asked for over an entry that is not a file entry");
    data = file_system_data(ctx, entry);
    type = file_system_type_cstr(ctx, entry);
    name = file_system_name_cstr(ctx, entry);
    bytes = file_system_value_bytes(ctx, data, &len);
    f = file_new(ctx, bytes, len, type, strlen(type), name, strlen(name),
                 (int64_t)file_system_modified(ctx, entry));
    CHECK(!JS_IsException(f), "file system: §2.3.1's File could not be allocated");
    /* THE SOURCE TRAVELS WITH THE BYTES. A File whose byte sequence is external input is an attacker-controlled
       path to whatever the page does with `text()`, and that identity is what an @S candidate is delivered to
       and what a flow's path constraint is keyed by. */
    if (concolic_is(data))
        blob_set_source(ctx, f, concolic_shape_c(data), concolic_src_c(data));
    free(bytes);
    JS_FreeCString(ctx, type);
    JS_FreeCString(ctx, name);
    JS_FreeValue(ctx, data);
    return f;
}

/* ---- the mock device's edge ------------------------------------------------------------------------------ */

/* A FILE'S BYTES ARE AN ATTACKER SOURCE, keyed by the file's own name so a candidate can be delivered into ONE
   file rather than into every file at once. The delivery declaration says what the browser does to the
   attacker's bytes on the way in, and for a file it does NOTHING — a file is read verbatim, with no
   percent-encoding and no leading character — which is a FACT worth declaring rather than an absence to leave
   undeclared: §S reports a parked search's delivery constraint, and "nothing is encoded" is the strongest one
   there is. Declared once per name, because a device may hold a file, lose it and hold it again. */
static void fs_declare_bytes_source(const char *src)
{
    if (concolic_source_encodes(src)) return;
    concolic_declare_source(src, "", 0);
}

void file_system_local_add(JSContext *ctx, const char *name, const char *type,
                           const char *bytes, size_t len, double last_modified)
{
    JSValue root, entry, data;
    const char *base;
    char shape[128], src[128];

    DCHECK(g_ready, "a file was put on the device before file_system_init built the model");
    DCHECK(name != NULL, "a file was put on the device with no name — §4.10.5.1.17's file is a filename, a file "
                         "type and a file body, and `files[0].name` has to answer with something");
    /* HTML §4.10.5.1.17: "Filenames must not contain path components, even in the case that a user has selected
       an entire directory hierarchy ... Path components ... are those parts of filenames that are separated by
       U+005C REVERSE SOLIDUS." Stripped HERE, at the one edge a name enters through, so no reader downstream
       has to know the rule — and so §4.10.5.4's `C:\fakepath\` prefix can never be mistaken for one. */
    base = strrchr(name, '\\');
    base = base ? base + 1 : name;
    DCHECK(file_system_valid_name(base, strlen(base)),
           "a file was put on the device under a name §2.1 does not admit — every entry in this model is "
           "reachable by name from its parent, and a name that is empty, `.`, `..` or carries a separator "
           "names something the model cannot hold");
    root = file_system_root_entry(ctx, FS_ROOT_LOCAL);
    entry = file_system_create_file(ctx, root, base);
    JS_FreeValue(ctx, root);
    JS_SetPropertyStr(ctx, entry, FS_MODIFIED, JS_NewFloat64(ctx, last_modified));
    JS_SetPropertyStr(ctx, entry, FS_TYPE, JS_NewString(ctx, type ? type : ""));
    snprintf(shape, sizeof shape, "{file:%s}", base);
    snprintf(src, sizeof src, "file:%s", base);
    fs_declare_bytes_source(src);
    /* THE BYTES, WRAPPED AT THE ONE EDGE THEY ENTER THROUGH. A conformance host installs no source overlay and
       gets the plain byte sequence back, so §2.3.1's File carries exactly what was put on the device; a solver
       host gets a concolic whose EXAMPLE is those same bytes, so `f.text()` reaches a sink carrying both the
       real contents and the fact that an attacker chose them. */
    data = concolic_source_wrap(ctx, shape, src, file_system_bytes_value(ctx, bytes, len));
    file_system_set_data(ctx, entry, data);
    JS_FreeValue(ctx, entry);
}

/* ---- the model's own lifetime ---------------------------------------------------------------------------- */

void file_system_init(JSContext *ctx)
{
    DCHECK(!g_ready, "file_system_init ran twice — the model is built once per AGENT, and a second root would "
                     "give one origin two filesystems");
    /* §3's getDirectory: "let dir be a new directory entry ... set dir's name to the empty string". §2.2's
       bucket test reads that empty first path item, so the bucket root's NAME is what makes it work. */
    g_bucket_root = fs_entry_new(ctx, FS_KIND_DIRECTORY, "");
    g_local_root  = fs_entry_new(ctx, FS_KIND_DIRECTORY, FS_ROOT_LOCAL);
    g_fs_ctx = ctx;
    g_ready = 1;
}

void file_system_free(JSContext *ctx)
{
    JS_FreeValue(ctx, g_bucket_root);
    JS_FreeValue(ctx, g_local_root);
    g_bucket_root = g_local_root = JS_UNDEFINED;
    g_fs_ctx = NULL;
    g_ready = 0;
}
