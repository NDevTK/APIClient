/* THE Blob INTERFACE — W3C File API §3.
 *
 * A Blob is an IMMUTABLE byte sequence with a MIME type, and that immutability is the whole of what makes it
 * different from a body: it re-reads, it slices into another Blob that shares nothing, and nothing about it
 * changes after the constructor returns. So it is not the Body mixin with a flag — it declares the same three
 * readers into core/byte_reader.c with a `take` that has no latch, and that is the entire difference stated
 * once.
 *
 * WHY IT IS BUILT NOW. It is what four wpt files abort naming, it is `response.blob()`, it is the second arm of
 * FormData's `append` overload and the value a multipart part with a filename must carry, and it is what
 * `URL.createObjectURL` takes. Every one of those was a DFAIL naming this interface.
 *
 * `stream()` and `textStream()` are ABSENT, honestly, until there is a ReadableStream — a shape-only object with
 * a noop `getReader` would be a stub of exactly the kind the IDL audit exists to expose, and the audit names
 * both. */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "cutils.h"
#include "core/byte_reader.h"
#include "core/file/blob.h"
#include "core/file/file_list.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "core/idl_iter.h"
#include "core/streams/readable_stream.h"
#include "core/frame/location.h"
#include "core/dom/document.h"
#include "core/url/url.h"
#include <stdio.h>

/* ONE STRUCT FOR BOTH INTERFACES, and one class id, because §4 says `interface File : Blob` — a File IS a Blob,
   so every brand test that accepts a Blob must accept it, and there is exactly one byte sequence for both to be
   about. What tells them apart is the PROTOTYPE the instance wears and `name` being non-NULL. */
typedef struct {
    char   *bytes;     /* the byte sequence; NULL only when `len` is 0 */
    size_t  len;
    char   *type;      /* §3.1's normalised MIME type, never NULL — "" is "no type" */
    char   *name;      /* §4.1's file name — NULL for a plain Blob, which is what tells the two apart */
    int64_t last_modified;
    /* WHERE THE BYTE SEQUENCE CAME FROM, when this engine did not compute it — NULL for every Blob a page
       built out of its own strings and buffers, and the source identity of the external input otherwise.
       A FILE'S CONTENTS ARE EXTERNAL INPUT IN THE SAME SENSE `location.hash` IS: the bytes are chosen by
       whoever put the file on the device, not by the page and not by this engine, so a page that reads one and
       puts it in a sink has an attacker-controlled path there. Carrying the identity on the Blob rather than
       minting a concolic for the bytes is what keeps the two halves separate: the BYTES stay real bytes (a
       `slice`, a multipart body and a `blob:` fetch all need them), and the READ that turns them into a value
       the page computes with is where the source is minted — per read, never once, because a candidate run
       substitutes a source at MINT time and a value minted once could never receive one (core/frame/location.c
       states the same rule for the same reason). */
    char   *src;
    char   *shape;
} BlobObj;

static JSClassID g_blob_class;
/* §4's File HAS A CLASS ID AND NO INSTANCES OF ITS OWN: a File wears the Blob class (every brand test that
   accepts a Blob accepts a File, which is what the inheritance MEANS), so this id exists for the per-realm
   PROTOTYPE SLOT alone — the same store quickjs keeps for Blob, so the two are found the same way. */
static JSClassID g_file_class;
static int       g_blob_id_stream = -1, g_blob_id_slice = -1;
static int       g_file_ctor_stepid = -1;
static JSRuntime *g_blob_rt;
static int       g_blob_ctor_stepid = -1;
static int       g_blob_reader_handle = -1;

static JSValue blob_alloc(JSContext *ctx, JSValueConst proto, const char *bytes, size_t len,
                          const char *type, size_t type_len);

static void blob_finalizer(JSRuntime *rt, JSValue val)
{
    BlobObj *b = JS_GetOpaque(val, g_blob_class);
    (void)rt;
    if (b) { free(b->bytes); free(b->type); free(b->name); free(b->src); free(b->shape); free(b); }
}

bool blob_is(JSValueConst v)
{
    return g_blob_class != 0 && JS_GetOpaque(v, g_blob_class) != NULL;
}

const char *blob_bytes_of(JSValueConst v, size_t *plen, const char **ptype)
{
    BlobObj *b = g_blob_class ? JS_GetOpaque(v, g_blob_class) : NULL;
    if (!b) return NULL;
    if (plen)  *plen  = b->len;
    if (ptype) *ptype = b->type;
    return b->bytes ? b->bytes : "";
}

const char *blob_file_name_of(JSValueConst v)
{
    BlobObj *b = g_blob_class ? JS_GetOpaque(v, g_blob_class) : NULL;
    return b ? b->name : NULL;
}

int64_t blob_last_modified_of(JSValueConst v)
{
    BlobObj *b = g_blob_class ? JS_GetOpaque(v, g_blob_class) : NULL;
    return b ? b->last_modified : 0;
}

JSClassID blob_class_id(void)
{
    DCHECK(g_blob_class != 0,
           "a `Blob` IDL position was declared before blob_init built the class it brands against — the "
           "declaration would then brand against class id 0, which every object fails");
    return g_blob_class;
}

bool blob_source_of(JSValueConst v, const char **shape, const char **src)
{
    BlobObj *b = g_blob_class ? JS_GetOpaque(v, g_blob_class) : NULL;

    DCHECK(shape != NULL && src != NULL,
           "a byte sequence's SOURCE was asked for with nowhere to put half of it — the shape is what an @H "
           "record displays and the src is what a candidate delivery is keyed by, and a caller needs both");
    if (!b || !b->src)
        return false;
    DCHECK(b->shape != NULL,
           "a Blob carries a source identity with no display shape — blob_set_source records the pair or "
           "neither, so one without the other is a record something else wrote");
    *shape = b->shape;
    *src = b->src;
    return true;
}

void blob_set_source(JSContext *ctx, JSValueConst v, const char *shape, const char *src)
{
    BlobObj *b = g_blob_class ? JS_GetOpaque(v, g_blob_class) : NULL;

    (void)ctx;
    DCHECK(b != NULL, "a byte sequence's SOURCE was recorded on something that is not a Blob");
    DCHECK(shape != NULL && src != NULL, "a byte sequence's SOURCE was recorded with no identity — the shape is "
                                         "what an @H record displays and the src is what the flow's constraint "
                                         "and a candidate delivery are keyed by, and neither is optional");
    DCHECK(b == NULL || b->src == NULL,
           "a Blob's byte sequence was given a SECOND source identity — a byte sequence comes from one place, "
           "and a slice of a sourced Blob is a new Blob that must be given the source at its own mint");
    if (!b) return;
    b->shape = strdup(shape);
    b->src = strdup(src);
    CHECK(b->shape && b->src, "blob: OOM recording a byte sequence's source identity");
}

/* §3.1's TYPE NORMALISATION, which both the constructor and `slice` perform: a type carrying any character
   outside U+0020..U+007E is DROPPED entirely rather than sanitised, and what remains is ASCII-lowercased.
   Caller frees. */
static char *blob_normalize_type(const char *type, size_t len)
{
    char *out;
    size_t i;

    for (i = 0; i < len; i++)
        if ((unsigned char)type[i] < 0x20 || (unsigned char)type[i] > 0x7E) { len = 0; break; }
    out = malloc(len + 1);
    CHECK(out, "blob: OOM normalising a MIME type");
    for (i = 0; i < len; i++)
        out[i] = (type[i] >= 'A' && type[i] <= 'Z') ? (char)(type[i] - 'A' + 'a') : type[i];
    out[len] = 0;
    return out;
}

JSValue blob_new(JSContext *ctx, const char *bytes, size_t len, const char *type)
{
    return blob_new_len(ctx, bytes, len, type ? type : "", type ? strlen(type) : 0);
}

/* THE SAME, WITH THE TYPE'S LENGTH. §3.1 drops a type carrying any character outside U+0020..U+007E, and NUL is
   one of them — a caller that has the type as a JS string must not lose the difference between `image/gif` and
   `image/gif\0` by handing over a C string. */
JSValue blob_new_len(JSContext *ctx, const char *bytes, size_t len, const char *type, size_t type_len)
{
    {
        JSValue proto = JS_GetClassProto(ctx, g_blob_class);
        JSValue obj;
        DCHECK(!JS_IsNull(proto), "a Blob was minted in a realm that never ran its install");
        obj = blob_alloc(ctx, proto, bytes, len, type, type_len);
        JS_FreeValue(ctx, proto);
        return obj;
    }
}

/* §4.1's File, which is §3.1's Blob wearing File.prototype and carrying a name. `name` is NOT optional here —
   a File without one is a Blob, and this is the one place that distinction is made. */
JSValue file_new(JSContext *ctx, const char *bytes, size_t len, const char *type, size_t type_len,
                 const char *name, size_t name_len, int64_t last_modified)
{
    JSValue proto = JS_GetClassProto(ctx, g_file_class);
    JSValue obj;

    DCHECK(!JS_IsNull(proto), "a File was minted in a realm that never ran its install");
    obj = blob_alloc(ctx, proto, bytes, len, type, type_len);
    JS_FreeValue(ctx, proto);
    BlobObj *b;

    if (JS_IsException(obj))
        return obj;
    b = JS_GetOpaque(obj, g_blob_class);
    b->name = malloc(name_len + 1);
    CHECK(b->name, "blob: OOM naming a File");
    memcpy(b->name, name, name_len);
    b->name[name_len] = 0;
    b->last_modified = last_modified;
    return obj;
}

static JSValue blob_alloc(JSContext *ctx, JSValueConst proto, const char *bytes, size_t len,
                          const char *type, size_t type_len)
{
    BlobObj *b;
    JSValue obj;

    DCHECK(g_blob_class != 0, "a Blob was built before the class existed — blob_init runs at install");
    obj = JS_NewObjectProtoClass(ctx, proto, g_blob_class);
    if (JS_IsException(obj))
        return obj;
    b = calloc(1, sizeof *b);
    CHECK(b, "blob: OOM building a Blob");
    if (len) {
        /* +1 and a NUL past the end so the bytes can still be handed to a C string consumer; `len` is what
           every read uses, and it is what an interior NUL no longer truncates. */
        b->bytes = malloc(len + 1);
        CHECK(b->bytes, "blob: OOM copying a Blob's bytes");
        memcpy(b->bytes, bytes, len);
        b->bytes[len] = 0;
        b->len = len;
    }
    b->type = blob_normalize_type(type, type_len);
    JS_SetOpaque(obj, b);
    return obj;
}

/* ---- File API §8's BLOB URL STORE -------------------------------------------------------------------------
 *
 * §8.1 mints an entry and §8.2 removes one. The store holds a REFERENCE to each Blob, which is the whole of
 * what the URL is for: `URL.createObjectURL(blob)` is how a page keeps bytes alive under a name it can hand to
 * an <img> or a fetch, and revoking is how it stops.
 *
 * THE URL IS DERIVED, NOT DRAWN AT RANDOM. §8.1 says the path is a UUID, and a browser generates one; this
 * engine is deterministic on purpose — a time-travel resume must produce the byte-identical URL, or a flow that
 * stored one and a flow that resumes to read it disagree about which entry they mean. A counter is unique for
 * exactly as long as a random UUID is, and it is unique by construction rather than with high probability.
 *
 * AND THE STORE THAT SENTENCE IS ABOUT IS THE PART THAT COULD NOT RESUME. It was a `realloc`'d array of
 * `{ char *url; JSValue blob; }` with the counter beside it as a `static unsigned`, which is the shape
 * CLAUDE.md names in as many words: platform data a flow queues is a JS value, never malloc'd C. Every
 * consequence was live. `URL.createObjectURL` in one arm registered for EVERY arm and `revokeObjectURL` in one
 * revoked for every arm, so an arm that revoked eagerly broke a sibling's `<img src>`. The counter was one for
 * the agent, so two arms of a fork minting a URL at the SAME source line got DIFFERENT URLs — which is exactly
 * what the determinism above exists to prevent, stated and then defeated one line below. And the entries were
 * C memory reachable only from a static: parked to the cold tier they went nowhere, and freed with a delta
 * they would have been a leak the runtime's own GC walk cannot see.
 *
 * SO IT IS A HEAP OBJECT: an Array of `[url, blob, mint]` and the counter beside it, built at agent init so it
 * is BASELINE, and every mutation is a property write the per-flow COW delta captures. A revoked entry leaves
 * a hole the next mint reuses, so a page that creates and revokes in a loop does not grow the Array. */
static JSValue g_blob_url_store = JS_UNDEFINED;
static JSAtom  g_atom_urls = JS_ATOM_NULL, g_atom_url_next = JS_ATOM_NULL;

enum { BU_URL = 0, BU_BLOB, BU_MINT };

/* The store's entry list, OWNED. */
static JSValue blob_url_list(JSContext *ctx)
{
    JSValue q;

    DCHECK(JS_IsObject(g_blob_url_store),
           "File API §8's blob URL store was reached in an agent that never ran blob_init — the store has to "
           "be built there, which is pre-boot, or the first flow to mint a URL creates it inside its own delta "
           "and every sibling then looks in an object that is not there");
    q = JS_GetProperty(ctx, g_blob_url_store, g_atom_urls);
    DCHECK(JS_IsArray(q), "File API §8's blob URL store lost its entry list");
    return q;
}

static uint32_t blob_url_count(JSContext *ctx, JSValueConst q)
{
    JSValue len = JS_GetPropertyStr(ctx, q, "length");
    uint32_t n = 0;

    JS_ToUint32(ctx, &n, len);
    JS_FreeValue(ctx, len);
    return n;
}

/* WHICH ENTRY THIS URL NAMES, or -1. `q` is the list; the caller holds it.
 *
 * AND THIS IS WHERE THE ISOLATION IS ASSERTED, because it is the one place every entry a flow can see is
 * looked at. The list and the mint counter ride the SAME per-flow COW delta, so an entry a flow can see must
 * carry a mint number its OWN counter has already handed out; an entry from a sibling's timeline carries one
 * this flow never allocated. That is the exact statement of "a URL minted by one flow is not visible to
 * another", and it fires if either half of the pair is ever answered from outside the delta. */
static int blob_url_find(JSContext *ctx, JSValueConst q, const char *url, size_t len)
{
    uint32_t i, n = blob_url_count(ctx, q);
    double next = 0;
    JSValue nv = JS_GetProperty(ctx, g_blob_url_store, g_atom_url_next);
    int hit = -1;

    JS_ToFloat64(ctx, &next, nv);
    JS_FreeValue(ctx, nv);
    for (i = 0; i < n && hit < 0; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, q, i), u, m;
        const char *s;
        size_t slen = 0;
        double mint = 0;

        if (!JS_IsObject(e)) { JS_FreeValue(ctx, e); continue; }   /* a revoked entry's slot */
        m = JS_GetPropertyUint32(ctx, e, BU_MINT);
        JS_ToFloat64(ctx, &mint, m);
        JS_FreeValue(ctx, m);
        DCHECK(mint < next,
               "a flow reached a blob URL entry minted with a counter value this flow never allocated — "
               "File API §8's store and its mint counter ride the SAME per-flow COW delta, so an entry another "
               "flow's timeline created cannot be visible here. One of the two is being answered from outside "
               "the delta, and the effect is one arm's createObjectURL registering for every arm");
        u = JS_GetPropertyUint32(ctx, e, BU_URL);
        s = JS_ToCStringLen(ctx, &slen, u);
        if (s && slen == len && !memcmp(s, url, len))
            hit = (int)i;
        JS_FreeCString(ctx, s);
        JS_FreeValue(ctx, u);
        JS_FreeValue(ctx, e);
    }
    return hit;
}

char *blob_url_create(JSContext *ctx, JSValueConst obj)
{
    UrlRecord base;
    const char *base_str = document_url(ctx);
    char *origin = NULL, *url;
    size_t n;
    uint32_t mint = 0, slot, have;
    JSValue q, entry, nv;

    if (!blob_is(obj)) {
        JS_ThrowTypeError(ctx, "createObjectURL requires a Blob");
        return NULL;
    }
    /* §8.1: the URL's path is the ORIGIN of the settings object followed by the id. A document with no address
       has an opaque origin, which serializes to "null" — the same answer §4.7 gives, and the same one a page
       reading `location.origin` would see. */
    if (base_str) {
        url_record_init(&base);
        if (url_parse(&base, base_str, strlen(base_str), NULL))
            origin = url_serialize_origin(&base);
        url_record_free(&base);
    }
    q = blob_url_list(ctx);
    nv = JS_GetProperty(ctx, g_blob_url_store, g_atom_url_next);
    JS_ToUint32(ctx, &mint, nv);
    JS_FreeValue(ctx, nv);
    JS_SetProperty(ctx, g_blob_url_store, g_atom_url_next, JS_NewUint32(ctx, mint + 1));

    /* `blob:` + origin + `/` + a 36-character UUID + the NUL. It was 32 — the length of a UUID's HEX DIGITS
       rather than of a UUID, which truncated the last group and made the path fail the shape the spec gives
       it. */
    n = strlen("blob:") + strlen(origin ? origin : "null") + 1 + 36 + 1;
    url = malloc(n);
    CHECK(url, "blob: OOM minting an object URL");
    snprintf(url, n, "blob:%s/%08x-0000-4000-8000-%012x", origin ? origin : "null", mint, mint);
    free(origin);

    entry = JS_NewArray(ctx);
    CHECK(!JS_IsException(entry), "blob: OOM recording an object URL");
    JS_SetPropertyUint32(ctx, entry, BU_URL, JS_NewString(ctx, url));
    JS_SetPropertyUint32(ctx, entry, BU_BLOB, JS_DupValue(ctx, obj));
    JS_SetPropertyUint32(ctx, entry, BU_MINT, JS_NewUint32(ctx, mint));
    have = blob_url_count(ctx, q);
    for (slot = 0; slot < have; slot++) {   /* reuse a revoked entry's slot before growing the list */
        JSValue e = JS_GetPropertyUint32(ctx, q, slot);
        int free_slot = !JS_IsObject(e);
        JS_FreeValue(ctx, e);
        if (free_slot) break;
    }
    JS_SetPropertyUint32(ctx, q, slot, entry);
    JS_FreeValue(ctx, q);
    return url;
}

void blob_url_revoke(JSContext *ctx, const char *url, size_t len)
{
    JSValue q = blob_url_list(ctx);
    /* §8.2: a URL that names no entry is a NO-OP, not an error — a page revoking twice is ordinary cleanup. */
    int i = blob_url_find(ctx, q, url, len);

    if (i >= 0)
        JS_SetPropertyUint32(ctx, q, (uint32_t)i, JS_UNDEFINED);
    JS_FreeValue(ctx, q);
}

/* OWNED, and it has to be: the entry lives in a heap Array now, so a borrowed reference would be one the next
   revoke or the next context switch can free under the caller. */
JSValue blob_url_lookup(JSContext *ctx, const char *url, size_t len)
{
    JSValue q = blob_url_list(ctx), blob = JS_UNDEFINED;
    int i = blob_url_find(ctx, q, url, len);

    if (i >= 0) {
        JSValue e = JS_GetPropertyUint32(ctx, q, (uint32_t)i);
        blob = JS_GetPropertyUint32(ctx, e, BU_BLOB);
        JS_FreeValue(ctx, e);
    }
    JS_FreeValue(ctx, q);
    return blob;
}

/* ---- §3.3's readers ---------------------------------------------------------------------------------------
 *
 * NO LATCH. §3.3 reads the Blob's byte sequence, which does not change and is not consumed — `await b.text()`
 * twice gives the text twice, where the same two calls on a Response are a TypeError. That one function is the
 * whole of what File API declares differently from Fetch. */
static int blob_take(JSContext *ctx, JSValueConst v, const char **bytes, size_t *len, JSValue *pstream)
{
    /* A Blob's bytes are always here: §3 makes it an immutable byte sequence, never a stream. */
    *pstream = JS_UNDEFINED;
    BlobObj *b = JS_GetOpaque(v, g_blob_class);
    if (!b) {
        JS_ThrowTypeError(ctx, "not a Blob");
        return -1;
    }
    *bytes = b->bytes ? b->bytes : "";
    *len = b->len;
    return 0;
}

/* §3.3's `stream()`. NOT a byte reader: those answer a PROMISE, and `stream()` answers the stream ITSELF,
   synchronously — a page writes `blob.stream().getReader()` on one line. A Blob's bytes are all present, so the
   stream carries them as one chunk and is closed, which is what a stream over an immutable byte sequence IS
   rather than a simplification of one. */
static JSValue js_blob_stream(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    size_t len = 0;
    const char *bytes = blob_bytes_of(this_val, &len, NULL);
    (void)argc; (void)argv; (void)magic;
    if (!bytes) return JS_ThrowTypeError(ctx, "not a Blob");
    return readable_stream_from_bytes(ctx, bytes, len);
}

/* WHERE A BLOB'S BYTES CAME FROM — the question the shared reader machine asks of every interface that holds
   a byte sequence, and this is Blob's answer. A READER OF THIS INTERFACE'S OWN THAT CALLED THE SHARED ONE AND
   WRAPPED WHAT CAME BACK IS ONE DOOR TOO MANY, and the cost is not this interface's: `text()` and `json()` are
   both readers whose value IS the content, File API declares only the first, and an interface that declares
   both then has to remember to wrap at two of its own copies — an interface that forgets hands the page a
   plain record, and `if (cfg.admin)` over a member the server did not send answers `undefined` with nothing to
   say a gate went unexplored. One question, asked at both content readers by the machine that owns them; each
   interface states its own answer and writes no reader for it.
 *
 * `true` because THESE bytes are attacker input: blob_set_source's only caller is the File System Access
 * component, whose source rows are declared deliveries (a file the user chose), and those are exactly the
 * values solver/concolic.h's read counter exists to count. A Blob the page BUILT carries no source, and NULL
 * says so — see ByteReaderIface.source for why that is a statement and not a hole. */
static char *blob_reader_source(JSContext *ctx, JSValueConst recv, bool *attacker)
{
    BlobObj *b = g_blob_class ? JS_GetOpaque(recv, g_blob_class) : NULL;
    char *src;

    (void)ctx;
    DCHECK(b != NULL, "a Blob was asked where its bytes came from and is not a Blob — the reader machine's own "
                      "receiver check has already answered for that, so a NULL here is this interface's `is` "
                      "and its opaque disagreeing");
    if (!b || !b->src)
        return NULL;
    DCHECK(b->shape != NULL,
           "a Blob carries a source identity with no display shape — blob_set_source records the pair or "
           "neither, so one without the other is that pair having come apart");
    *attacker = true;
    src = strdup(b->src);
    CHECK(src != NULL, "blob: OOM stating where a byte sequence came from");
    return src;
}

/* §3.3's THREE READERS. `text()` is the one that turns a byte sequence into a VALUE the page computes with — a
   string it concatenates, tests, and hands to a sink — so it is the one the provenance above rides, and the
   shared implementation is what asks for it. The two below answer with the real bytes: an ArrayBuffer is a
   buffer of bytes and not a value the solver's domain is over, and handing back a concolic in its place would
   break `new Uint8Array(await f.arrayBuffer())`, which is the ordinary way a page reads one. */
static const ByteReader BLOB_READERS[] = {
    { "text",        byte_reader_text },
    { "arrayBuffer", byte_reader_array_buffer },
    { "bytes",       byte_reader_bytes },
};

static const ByteReaderIface BLOB_READER_IFACE = {
    blob_is, blob_take, "Blob", BLOB_READERS, (int)(sizeof BLOB_READERS / sizeof BLOB_READERS[0]),
    blob_reader_source
};

/* ---- §3.1's attributes and slice() ------------------------------------------------------------------------ */

enum { BLOB_SIZE = 0, BLOB_TYPE, FILE_NAME, FILE_LAST_MODIFIED };

static JSValue js_blob_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    BlobObj *b = JS_GetOpaque(this_val, g_blob_class);
    if (!b) return JS_ThrowTypeError(ctx, "not a Blob");
    switch (magic) {
    case BLOB_SIZE: return JS_NewInt64(ctx, (int64_t)b->len);
    case BLOB_TYPE: return JS_NewString(ctx, b->type);
    /* §4's two attributes are on File.prototype, so a plain Blob cannot reach them through the chain — but
       `File.prototype.name` read off a Blob can, and that is the receiver check every IDL attribute makes. */
    case FILE_NAME:
        if (!b->name) return JS_ThrowTypeError(ctx, "not a File");
        return JS_NewString(ctx, b->name);
    default:
        DCHECK(magic == FILE_LAST_MODIFIED,
               "a Blob attribute was declared with a magic this component does not answer");
        if (!b->name) return JS_ThrowTypeError(ctx, "not a File");
        return JS_NewInt64(ctx, b->last_modified);
    }
}

/* §3.1's slice(). The relative-index arithmetic is the spec's, and it is over the CONVERTED arguments — the
   `[Clamp] long long` conversion has already run in the args machine, so `slice(1.5)` arrives here as 2. */
static JSValue js_blob_slice(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    BlobObj *b = JS_GetOpaque(this_val, g_blob_class);
    int64_t size, start = 0, end, span;
    const char *ctype = "";
    size_t ctype_len = 0;
    JSValue r;

    (void)magic;
    if (!b) return JS_ThrowTypeError(ctx, "not a Blob");
    size = (int64_t)b->len;
    end = size;
    if (argc > 0 && !JS_IsUndefined(argv[0])) {
        if (JS_ToInt64(ctx, &start, argv[0]) < 0) return JS_EXCEPTION;
        start = start < 0 ? (size + start > 0 ? size + start : 0) : (start < size ? start : size);
    }
    if (argc > 1 && !JS_IsUndefined(argv[1])) {
        if (JS_ToInt64(ctx, &end, argv[1]) < 0) return JS_EXCEPTION;
        end = end < 0 ? (size + end > 0 ? size + end : 0) : (end < size ? end : size);
    }
    span = end - start > 0 ? end - start : 0;
    if (argc > 2 && !JS_IsUndefined(argv[2])) {
        /* WITH ITS LENGTH, for the reason the constructor reads its type that way: §3.1 drops a content type
           carrying any character outside U+0020..U+007E, and NUL is one — read as a C string, `"te\0xt/plain"`
           is `te` and passes the check the spec fails it on. */
        ctype = JS_ToCStringLen(ctx, &ctype_len, argv[2]);
        if (!ctype) return JS_EXCEPTION;
    }
    /* The slice's bytes are COPIED, because a Blob owns its byte sequence: sharing the parent's storage would
       make one object's lifetime decide another's, and §3.1 gives no way to observe that they came from the
       same place. */
    r = blob_new_len(ctx, (b->bytes ? b->bytes : "") + start, (size_t)span, ctype, ctype_len);
    if (argc > 2 && !JS_IsUndefined(argv[2])) JS_FreeCString(ctx, ctype);
    return r;
}

/* ---- §3.1's constructor -----------------------------------------------------------------------------------
 *
 * `constructor(optional sequence<BlobPart> blobParts, optional BlobPropertyBag options = {})`.
 *
 * BOTH ARGUMENTS ARRIVE CONVERTED. The sequence walk is `sequence<BlobPart>`'s own conversion and happens in the
 * args machine, in argument order, because that is where Web IDL puts it — driven from here it ran AFTER the
 * options dictionary was read, and `new Blob(throwingIterable, {get type(){…}})` called a getter the spec never
 * reaches. What is left is the part that is §3.1's and not Web IDL's: assembling the parts' bytes.
 *
 * STILL A MACHINE rather than a plain body, because it is the shape a member takes when the args machine cannot
 * finish its work — and because the `endings` transform below is over data of the page's size. */
/* WHERE THIS MACHINE RESTS. §3.1's constructor is three steps and §4.1's File constructor adds two — and none
   of them can reach the page's code, because the declaration has already driven the `sequence<BlobPart>`
   iterator and the BlobPropertyBag's members before the body is entered. So the assembly is one stage and the
   machine never returns to it. */
#define BLOB_CTOR_STAGES(X) \
    X(BLOB_CTOR_ASSEMBLE = IDL_STEP_FIRST, \
      "File API §3.1 new Blob(blobParts, options) steps 1-3 and §4.1 new File(fileBits, fileName, options) " \
      "steps 1-5 (the byte sequence the parts assemble to, the normalized type, and a File's name and " \
      "lastModified)")
enum { BLOB_CTOR_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const BLOB_CTOR_STEPS[] = { BLOB_CTOR_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct { int unused; } JSBlobCtorState;

static void js_blob_ctor_visit(JSContext *ctx, void *st, JSStepVisit *v) { (void)ctx; (void)st; (void)v; }

/* §3.1's "convert line endings to native", which `endings: "native"` asks for. The native ending on this host is
   LF, so every CRLF, lone CR and lone LF becomes one LF — which is a real transform and not a no-op, because a
   `\r\n` in the input is TWO bytes and comes out as one. Caller frees; `*out_len` is the result's length. */
static char *blob_native_endings(const char *s, size_t n, size_t *out_len)
{
    char *out = malloc(n + 1);
    size_t i, w = 0;

    CHECK(out, "blob: OOM converting line endings");
    for (i = 0; i < n; i++) {
        if (s[i] == '\r') {
            out[w++] = '\n';
            if (i + 1 < n && s[i + 1] == '\n') i++;
        } else {
            out[w++] = s[i];
        }
    }
    out[w] = 0;
    *out_len = w;
    return out;
}

static int js_blob_ctor_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                             JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    /* ONE MACHINE FOR BOTH CONSTRUCTORS. §4.1's File constructor IS §3.1's Blob constructor with a name
       argument in the middle and one more dictionary member — the parts walk, the endings transform and the
       type normalisation are the same steps, so they are the same code and the magic says which shape the
       arguments are in. */
    bool is_file = idl_step_magic(hdr) != 0;
    JSValueConst parts = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSValueConst options = argc > (is_file ? 2 : 1) ? argv[is_file ? 2 : 1] : JS_UNDEFINED;
    JSValueConst fname = is_file && argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValue type_v, endings_v;
    const char *type = NULL, *endings = NULL;
    size_t type_len = 0;
    bool native;
    char *buf = NULL;
    size_t total = 0;
    uint32_t i, n = 0;

    (void)st; (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);
    DCHECK(hdr->stage == BLOB_CTOR_ASSEMBLE,
           "the Blob/File constructor resumed at a stage §3.1 and §4.1 do not have between them");
    if (JS_IsUndefined(hdr->this_val)) {
        JS_ThrowTypeError(ctx, "constructor %s requires 'new'", is_file ? "File" : "Blob");
        return -1;
    }
    /* NOTHING BELOW RUNS THE PAGE'S CODE: the parts list holds only strings, Blobs and BufferSources the args
       machine converted, and the options object is the engine-built one it filled. */
    type_v = idl_dict_get(ctx, options, "type");
    endings_v = idl_dict_get(ctx, options, "endings");
    if (!JS_IsUndefined(type_v)) {
        /* WITH ITS LENGTH. §3.1 drops a type carrying any character outside U+0020..U+007E, and NUL is one of
           them — read as a C string, `"image/gif\0"` is `image/gif` and passes the check the spec fails it on. */
        type = JS_ToCStringLen(ctx, &type_len, type_v);
        if (!type) { JS_FreeValue(ctx, type_v); JS_FreeValue(ctx, endings_v); return -1; }
    }
    if (!JS_IsUndefined(endings_v)) {
        endings = JS_ToCString(ctx, endings_v);
        if (!endings) {
            JS_FreeCString(ctx, type);
            JS_FreeValue(ctx, type_v); JS_FreeValue(ctx, endings_v);
            return -1;
        }
    }
    /* The args machine has already refused anything but the two values the enumeration lists. */
    native = endings && !strcmp(endings, "native");

    if (!JS_IsUndefined(parts)) {
        JSValue len_v = JS_GetPropertyStr(ctx, parts, "length");
        uint32_t count = 0;
        if (JS_ToUint32(ctx, &count, len_v) < 0) {
            JS_FreeValue(ctx, len_v);
            JS_FreeCString(ctx, type); JS_FreeCString(ctx, endings);
            JS_FreeValue(ctx, type_v); JS_FreeValue(ctx, endings_v);
            return -1;
        }
        JS_FreeValue(ctx, len_v);
        n = count;
    }
    for (i = 0; i < n; i++) {
        JSValue part = JS_GetPropertyUint32(ctx, parts, i);
        const uint8_t *bytes = NULL;
        size_t plen = 0, off = 0;
        char *owned = NULL;
        const char *cstr = NULL;
        JSValue view_buf = JS_UNDEFINED;
        char *grown;

        if (JS_IsString(part)) {
            cstr = JS_ToCStringLen(ctx, &plen, part);
            if (!cstr) { JS_FreeValue(ctx, part); goto fail; }
            if (native) {
                owned = blob_native_endings(cstr, plen, &plen);
                bytes = (const uint8_t *)owned;
                JS_FreeCString(ctx, cstr);
                cstr = NULL;
            } else {
                bytes = (const uint8_t *)cstr;
            }
        } else if (blob_is(part)) {
            bytes = (const uint8_t *)blob_bytes_of(part, &plen, NULL);
        } else if (JS_IsDetachedBufferSource(part)) {
            /* WEB IDL §3.2.26 Buffer source types' `get a copy of the bytes held by the buffer source` STEP 7:
               "If IsDetachedBuffer(jsArrayBuffer) is true, then return the empty byte sequence." File API
               §3.1.1 Constructor Parameters' `process blob parts` reaches this algorithm by name — "If element
               is a BufferSource, get a copy of the bytes held by the buffer source, and append those bytes to
               bytes" — so a detached part contributes NOTHING and the constructor still succeeds.
               IT IS ASKED HERE, ABOVE BOTH ARMS, BECAUSE NEITHER ARM CAN READ THE WINDOW FOR ONE. Steps 5-6
               read internal slots whose values step 7 discards, so asking step 7 first is not a reordering of
               anything observable; asking it second is unreachable, because an embedder's only routes to those
               slots are JS_GetArrayBuffer, which THROWS for a detached buffer, and JS_GetArrayBufferView, which
               refuses an out-of-bounds view — and a detached buffer makes every view over it out of bounds. So
               `new Blob([detachedBuffer])` threw a TypeError and `new Blob([viewOntoDetached])` aborted on the
               brand DCHECK below, which was a true sentence about a value that had passed that very test.
               The predicate answers for BOTH arms — §3.2.26's "underlying buffer" is V for an ArrayBuffer and
               V.[[ViewedArrayBuffer]] for a view — which is why one branch replaces two.
               THE BRAND HOLDS BEFORE IT IS ASKED: idl_args.c's `BlobPart` rule is a brand test that sends
               everything which is neither a BufferSource nor a Blob down the USVString arm, and the string and
               Blob arms are taken above, so the predicate's own always-fatal brand CHECK is this file's
               two-sided statement of that and replaces the DCHECK that used to stand below. */
            bytes = (const uint8_t *)"";
            plen = 0;
        } else if (JS_IsArrayBuffer(part)) {
            bytes = JS_GetArrayBuffer(ctx, &plen, part);
            DCHECK(bytes != NULL,
                   "§3.2.26's step 6 refused a buffer that is neither detached nor a non-buffer — the detach is "
                   "answered above and the brand test is the predicate's own, so JS_GetArrayBuffer has grown a "
                   "third refusal this arm does not know about");
            if (!bytes) { JS_FreeValue(ctx, part); goto fail; }
        } else {
            /* An ArrayBufferView, whose buffer is live: §3.2.26 step 7 is answered above. */
            size_t whole = 0;
            uint8_t *base;
            view_buf = JS_GetArrayBufferView(ctx, part, &off, &plen);
            DCHECK(!JS_IsException(view_buf),
                   "§3.2.26's step 5 refused a view whose buffer is not detached — step 7 is asked above and "
                   "the BlobPart conversion refuses a shared or resizable buffer, so the only remaining cause "
                   "of an out-of-bounds view has grown a fourth case");
            base = JS_GetArrayBuffer(ctx, &whole, view_buf);
            if (!base) { JS_FreeValue(ctx, view_buf); JS_FreeValue(ctx, part); goto fail; }
            /* THE `memcpy` BELOW READS `plen` BYTES FROM `base + off`, and those two numbers come from two
               different engine exports — the window from JS_GetArrayBufferView, the size from
               JS_GetArrayBuffer. They must describe one allocation, and there was a view for which they did
               not: a length-tracking one over a resized buffer, whose window JS_GetArrayBufferView answered
               out of the construction-time [[ByteLength]] slot, so `new Blob([v])` after `rab.resize(8)`
               reported `size: 64` and read 56 bytes past the allocation into the Blob's own bytes. */
            DCHECK(off <= whole && plen <= whole - off,
                   "a Blob part's view window is outside its own buffer — JS_GetArrayBufferView and "
                   "JS_GetArrayBuffer disagree about one allocation, and the copy below trusts the first");
            bytes = base + off;
        }
        grown = realloc(buf, total + plen + 1);
        CHECK(grown, "blob: OOM assembling a Blob's bytes");
        buf = grown;
        if (plen) memcpy(buf + total, bytes, plen);
        total += plen;
        free(owned);
        if (cstr) JS_FreeCString(ctx, cstr);
        JS_FreeValue(ctx, view_buf);
        JS_FreeValue(ctx, part);
    }
    if (is_file) {
        /* §4.1: `lastModified` defaults to NOW. It is a real clock read — a File the page builds and then
           inspects has a real timestamp, and answering 0 would be a value this engine invented. */
        JSValue lm_v = idl_dict_get(ctx, options, "lastModified");
        int64_t lm = js__gettimeofday_us() / 1000;
        size_t nlen = 0;
        const char *n_str;

        if (!JS_IsUndefined(lm_v) && JS_ToInt64(ctx, &lm, lm_v) < 0) {
            JS_FreeValue(ctx, lm_v);
            goto fail;
        }
        JS_FreeValue(ctx, lm_v);
        /* The declaration converted `fileName` to a USVString, so this is its bytes — with the LENGTH, because
           a name may carry an interior NUL exactly as a type may. */
        n_str = JS_ToCStringLen(ctx, &nlen, fname);
        if (!n_str) goto fail;
        *presult = file_new(ctx, buf ? buf : "", total, type ? type : "", type_len, n_str, nlen, lm);
        JS_FreeCString(ctx, n_str);
    } else {
        *presult = blob_new_len(ctx, buf ? buf : "", total, type ? type : "", type_len);
    }
    free(buf);
    JS_FreeCString(ctx, type);
    JS_FreeCString(ctx, endings);
    JS_FreeValue(ctx, type_v);
    JS_FreeValue(ctx, endings_v);
    return JS_IsException(*presult) ? -1 : 0;
fail:
    free(buf);
    JS_FreeCString(ctx, type);
    JS_FreeCString(ctx, endings);
    JS_FreeValue(ctx, type_v);
    JS_FreeValue(ctx, endings_v);
    return -1;
}

static const IdlStepDecl js_blob_ctor_decl = {
    js_blob_ctor_step, sizeof(JSBlobCtorState), js_blob_ctor_visit, NULL,
    "File API §3.1 new Blob(blobParts, options) / §4.1 new File(fileBits, fileName, options)", BLOB_CTOR_STEPS
};

/* ---- install ---------------------------------------------------------------------------------------------- */

static const char *const BLOB_ENDINGS[] = { "transparent", "native", NULL };
/* LEXICOGRAPHIC, because Web IDL §3.2.17 reads a dictionary's members in that order and not in the order the IDL writes
   them — `new Blob([], {get endings(){…}, get type(){…}})` observes which getter runs first, and BlobPropertyBag
   declares `type` before `endings`. The declaration machinery asserts the order rather than trusting it. */
static const IdlDictMember BLOB_OPTIONS[] = {
    { "endings", IDL_ENUM, false, BLOB_ENDINGS },
    { "type",    IDL_DOMSTRING },
};

/* §4's `dictionary FilePropertyBag : BlobPropertyBag`. Web IDL §3.2.17 reads the INHERITED members first — endings and
   type, sorted among themselves — and only then the derived dictionary's own, which is why `lastModified` comes
   last despite sorting before `type`. The level is what states that. */
static const IdlDictMember FILE_OPTIONS[] = {
    { "endings",      IDL_ENUM, false, BLOB_ENDINGS, 0 },
    { "type",         IDL_DOMSTRING, false, NULL, 0 },
    { "lastModified", IDL_LONG_LONG, false, NULL, 1 },
};

void blob_init(JSContext *ctx)
{
    JSClassDef def = { "Blob", .finalizer = blob_finalizer };
    JSRuntime *rt = JS_GetRuntime(ctx);
    static const IdlArgType CTOR_ARGS[2] = { IDL_SEQUENCE_BLOBPART, IDL_DICT };
    static const IdlArgType FILE_CTOR_ARGS[3] = { IDL_SEQUENCE_BLOBPART, IDL_USVSTRING, IDL_DICT };
    static const IdlArgType SLICE_ARGS[3] = { IDL_LONG_LONG_CLAMP, IDL_LONG_LONG_CLAMP, IDL_DOMSTRING };

    DCHECK(g_blob_rt == NULL || g_blob_rt == rt,
           "Blob was installed into a second runtime — its class id and step ids belong to the first, and one "
           "WASM instance is one document");
    if (g_blob_rt == rt)
        return;
    g_blob_rt = rt;
    /* §8's STORE, BUILT AT AGENT INIT, which is pre-boot and therefore BASELINE — the same rule §8.7 Timers's map of
       active timers and §8.12 Animation frames's map of animation frame callbacks are built under. Built lazily on the first
       `createObjectURL` instead, it would belong to whichever FLOW minted the first URL and every sibling
       would be registering into an object created inside another flow's delta. */
    g_atom_urls = JS_NewAtom(ctx, "blobUrlStore");
    g_atom_url_next = JS_NewAtom(ctx, "blobUrlCounter");
    CHECK(g_atom_urls != JS_ATOM_NULL && g_atom_url_next != JS_ATOM_NULL,
          "blob: the URL store's own keys could not be interned");
    g_blob_url_store = JS_NewObjectProto(ctx, JS_NULL);
    CHECK(!JS_IsException(g_blob_url_store), "blob: File API §8's URL store could not be allocated");
    {
        JSValue q = JS_NewArray(ctx);
        CHECK(!JS_IsException(q), "blob: File API §8's URL store could not be allocated");
        JS_SetProperty(ctx, g_blob_url_store, g_atom_urls, q);
    }
    JS_SetProperty(ctx, g_blob_url_store, g_atom_url_next, JS_NewUint32(ctx, 0));
    JS_NewClassID(rt, &g_blob_class);
    JS_NewClass(rt, g_blob_class, &def);
    {
        JSClassDef fdef = { "File" };
        JS_NewClassID(rt, &g_file_class);
        JS_NewClass(rt, g_file_class, &fdef);
    }
    g_blob_id_stream = idl_method_id(ctx, SLICE_ARGS, 0, js_blob_stream, 0);
    g_blob_id_slice  = idl_method_id(ctx, SLICE_ARGS, 3, js_blob_slice, 0);
    idl_optional_from(0);   /* §3.1: all three of slice's arguments are optional */

    g_blob_reader_handle = byte_reader_declare(ctx, &BLOB_READER_IFACE);

    g_blob_ctor_stepid = idl_method_id_step(ctx, CTOR_ARGS, 2, BLOB_OPTIONS,
                                            (int)(sizeof BLOB_OPTIONS / sizeof BLOB_OPTIONS[0]),
                                            &js_blob_ctor_decl, 0);
    idl_optional_from(0);   /* §3.1: both constructor arguments are optional */

    g_file_ctor_stepid = idl_method_id_step(ctx, FILE_CTOR_ARGS, 3, FILE_OPTIONS,
                                            (int)(sizeof FILE_OPTIONS / sizeof FILE_OPTIONS[0]),
                                            &js_blob_ctor_decl, 1);
    idl_optional_from(2);   /* §4.1: `fileBits` and `fileName` are REQUIRED; only `options` is optional */
    realm_declare_intrinsic(blob_install_protos);
    /* §5's FileList, declared from here because §3, §4 and §5 are ONE component and this is its declaration
       point — the same reason html_form_declare declares §4.10.22.10's SubmitEvent. It declares its own
       per-realm intrinsic, so its prototype and interface object are built in every realm this one is. */
    file_list_init(ctx);
}

/* §3.1's AND §4's INTERFACE PROTOTYPE OBJECTS, FOR ONE REALM. */
void blob_install_protos(JSContext *ctx)
{
    JSValue blob_p, file_p, prev;

    DCHECK(g_blob_class != 0, "a realm asked for Blob.prototype before the class was declared");
    prev = JS_GetClassProto(ctx, g_blob_class);
    DCHECK(JS_IsNull(prev), "blob_install_protos ran twice in one realm");
    JS_FreeValue(ctx, prev);

    blob_p = JS_NewObject(ctx);
    CHECK(!JS_IsException(blob_p), "Blob.prototype could not be allocated");
    idl_interface_tag(ctx, blob_p, "Blob");
    idl_install_accessor(ctx, blob_p, "size", js_blob_get, BLOB_SIZE, -1);
    idl_install_accessor(ctx, blob_p, "type", js_blob_get, BLOB_TYPE, -1);
    idl_install_method(ctx, blob_p, "stream", 0, g_blob_id_stream);
    idl_install_method(ctx, blob_p, "slice", 0, g_blob_id_slice);
    byte_reader_install(ctx, blob_p, g_blob_reader_handle);
    JS_SetClassProto(ctx, g_blob_class, blob_p);

    /* §4: `interface File : Blob`. The prototype CHAINS to Blob.prototype, so a File's `slice`, `text` and
       `size` are the same functions rather than copies — and its instances wear the same class id, so every
       brand test that accepts a Blob accepts a File, which is what the inheritance MEANS. */
    file_p = JS_NewObjectProto(ctx, blob_p);
    CHECK(!JS_IsException(file_p), "File.prototype could not be allocated");
    idl_interface_tag(ctx, file_p, "File");
    idl_install_accessor(ctx, file_p, "name", js_blob_get, FILE_NAME, -1);
    idl_install_accessor(ctx, file_p, "lastModified", js_blob_get, FILE_LAST_MODIFIED, -1);
    JS_SetClassProto(ctx, g_file_class, file_p);
}

void blob_install(JSContext *ctx, JSValueConst global)
{
    JSValue ctor;
    DCHECK(g_blob_ctor_stepid >= 0, "Blob was installed before blob_init declared its constructor");
    ctor = idl_step_constructor(ctx, "Blob", 0, g_blob_ctor_stepid);
    CHECK(!JS_IsException(ctor), "the Blob interface object could not be allocated");
    {
        JSValue proto = JS_GetClassProto(ctx, g_blob_class);
        DCHECK(!JS_IsNull(proto), "Blob was installed into a realm that never ran its proto build");
        JS_SetConstructor(ctx, ctor, proto);
        JS_FreeValue(ctx, proto);
    }
    JS_SetPropertyStr(ctx, (JSValue)global, "Blob", ctor);

    ctor = idl_step_constructor(ctx, "File", 2, g_file_ctor_stepid);
    CHECK(!JS_IsException(ctor), "the File interface object could not be allocated");
    {
        JSValue proto = JS_GetClassProto(ctx, g_file_class);
        DCHECK(!JS_IsNull(proto), "File was installed into a realm that never ran its proto build");
        JS_SetConstructor(ctx, ctor, proto);
        JS_FreeValue(ctx, proto);
    }
    JS_SetPropertyStr(ctx, (JSValue)global, "File", ctor);
}

void blob_free(JSContext *ctx)
{
    if (!g_blob_rt)
        return;
    /* The ENTRIES are the flows' — each arm's are in the delta that captured them, and the collector owns the
       bytes. What the agent owns is the record itself and the two keys it is read by. */
    JS_FreeValue(ctx, g_blob_url_store);
    g_blob_url_store = JS_UNDEFINED;
    JS_FreeAtom(ctx, g_atom_urls);
    JS_FreeAtom(ctx, g_atom_url_next);
    g_atom_urls = g_atom_url_next = JS_ATOM_NULL;
    file_list_free(ctx);
    /* the prototypes are the REALMS' — released with their contexts */
    g_blob_rt = NULL;
    g_blob_ctor_stepid = g_file_ctor_stepid = -1;
}
