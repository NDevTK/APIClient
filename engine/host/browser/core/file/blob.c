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
#include "core/idl_args.h"
#include "core/idl_iter.h"

/* ONE STRUCT FOR BOTH INTERFACES, and one class id, because §4 says `interface File : Blob` — a File IS a Blob,
   so every brand test that accepts a Blob must accept it, and there is exactly one byte sequence for both to be
   about. What tells them apart is the PROTOTYPE the instance wears and `name` being non-NULL. */
typedef struct {
    char   *bytes;     /* the byte sequence; NULL only when `len` is 0 */
    size_t  len;
    char   *type;      /* §3.1's normalised MIME type, never NULL — "" is "no type" */
    char   *name;      /* §4.1's file name — NULL for a plain Blob, which is what tells the two apart */
    int64_t last_modified;
} BlobObj;

static JSClassID g_blob_class;
static JSValue   g_blob_proto = JS_UNDEFINED;
static JSValue   g_file_proto = JS_UNDEFINED;
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
    if (b) { free(b->bytes); free(b->type); free(b->name); free(b); }
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
    return blob_alloc(ctx, g_blob_proto, bytes, len, type, type_len);
}

/* §4.1's File, which is §3.1's Blob wearing File.prototype and carrying a name. `name` is NOT optional here —
   a File without one is a Blob, and this is the one place that distinction is made. */
JSValue file_new(JSContext *ctx, const char *bytes, size_t len, const char *type, size_t type_len,
                 const char *name, size_t name_len, int64_t last_modified)
{
    JSValue obj = blob_alloc(ctx, g_file_proto, bytes, len, type, type_len);
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

/* ---- §3.3's readers ---------------------------------------------------------------------------------------
 *
 * NO LATCH. §3.3 reads the Blob's byte sequence, which does not change and is not consumed — `await b.text()`
 * twice gives the text twice, where the same two calls on a Response are a TypeError. That one function is the
 * whole of what File API declares differently from Fetch. */
static int blob_take(JSContext *ctx, JSValueConst v, const char **bytes, size_t *len)
{
    BlobObj *b = JS_GetOpaque(v, g_blob_class);
    if (!b) {
        JS_ThrowTypeError(ctx, "not a Blob");
        return -1;
    }
    *bytes = b->bytes ? b->bytes : "";
    *len = b->len;
    return 0;
}

static const ByteReader BLOB_READERS[] = {
    { "text",        byte_reader_text },
    { "arrayBuffer", byte_reader_array_buffer },
    { "bytes",       byte_reader_bytes },
};

static const ByteReaderIface BLOB_READER_IFACE = {
    blob_is, blob_take, "Blob", BLOB_READERS, (int)(sizeof BLOB_READERS / sizeof BLOB_READERS[0])
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
typedef struct { uint8_t unused; } JSBlobCtorState;

static void js_blob_ctor_visit(JSContext *ctx, void *st, JSStepVisit *v) { (void)ctx; (void)st; (void)v; }
static void js_blob_ctor_release(JSContext *ctx, void *st) { (void)ctx; (void)st; }

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
        } else if (JS_IsArrayBuffer(part)) {
            bytes = JS_GetArrayBuffer(ctx, &plen, part);
            if (!bytes) { JS_FreeValue(ctx, part); goto fail; }
        } else {
            /* An ArrayBufferView. A DETACHED one contributes NOTHING rather than throwing, which is what §3.1
               says and what wpt's detached-buffer file checks — its buffer is gone, so its byte range is empty. */
            size_t whole = 0;
            uint8_t *base;
            view_buf = JS_GetArrayBufferView(ctx, part, &off, &plen);
            DCHECK(!JS_IsException(view_buf),
                   "a Blob part reached assembly as none of the union's three arms — the sequence conversion "
                   "produces exactly a string, a Blob or a BufferSource, so nothing else can be in the list");
            base = JS_GetArrayBuffer(ctx, &whole, view_buf);
            if (!base) { JS_FreeValue(ctx, view_buf); JS_FreeValue(ctx, part); goto fail; }
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
    js_blob_ctor_step, sizeof(JSBlobCtorState), js_blob_ctor_visit, js_blob_ctor_release
};

/* ---- install ---------------------------------------------------------------------------------------------- */

static const char *const BLOB_ENDINGS[] = { "transparent", "native", NULL };
/* LEXICOGRAPHIC, because §3.2.18 reads a dictionary's members in that order and not in the order the IDL writes
   them — `new Blob([], {get endings(){…}, get type(){…}})` observes which getter runs first, and BlobPropertyBag
   declares `type` before `endings`. The declaration machinery asserts the order rather than trusting it. */
static const IdlDictMember BLOB_OPTIONS[] = {
    { "endings", IDL_ENUM, false, BLOB_ENDINGS },
    { "type",    IDL_DOMSTRING },
};

/* §4's `dictionary FilePropertyBag : BlobPropertyBag`. §3.2.18 reads the INHERITED members first — endings and
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
    JS_NewClassID(rt, &g_blob_class);
    JS_NewClass(rt, g_blob_class, &def);
    g_blob_proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(g_blob_proto), "Blob.prototype could not be allocated");
    idl_interface_tag(ctx, g_blob_proto, "Blob");

    idl_install_accessor(ctx, g_blob_proto, "size", js_blob_get, BLOB_SIZE, -1);
    idl_install_accessor(ctx, g_blob_proto, "type", js_blob_get, BLOB_TYPE, -1);
    idl_install_method(ctx, g_blob_proto, "slice", 0,
                       idl_method_id(ctx, SLICE_ARGS, 3, js_blob_slice, 0));
    idl_optional_from(0);   /* §3.1: all three of slice's arguments are optional */

    g_blob_reader_handle = byte_reader_declare(ctx, &BLOB_READER_IFACE);
    byte_reader_install(ctx, g_blob_proto, g_blob_reader_handle);

    g_blob_ctor_stepid = idl_method_id_step(ctx, CTOR_ARGS, 2, BLOB_OPTIONS,
                                            (int)(sizeof BLOB_OPTIONS / sizeof BLOB_OPTIONS[0]),
                                            &js_blob_ctor_decl, 0);
    idl_optional_from(0);   /* §3.1: both constructor arguments are optional */

    /* §4: `interface File : Blob`. The prototype CHAINS to Blob.prototype, so a File's `slice`, `text` and
       `size` are the same functions rather than copies — and its instances wear the same class id, so every
       brand test that accepts a Blob accepts a File, which is what the inheritance MEANS. */
    g_file_proto = JS_NewObjectProto(ctx, g_blob_proto);
    CHECK(!JS_IsException(g_file_proto), "File.prototype could not be allocated");
    idl_interface_tag(ctx, g_file_proto, "File");
    idl_install_accessor(ctx, g_file_proto, "name", js_blob_get, FILE_NAME, -1);
    idl_install_accessor(ctx, g_file_proto, "lastModified", js_blob_get, FILE_LAST_MODIFIED, -1);
    g_file_ctor_stepid = idl_method_id_step(ctx, FILE_CTOR_ARGS, 3, FILE_OPTIONS,
                                            (int)(sizeof FILE_OPTIONS / sizeof FILE_OPTIONS[0]),
                                            &js_blob_ctor_decl, 1);
    idl_optional_from(2);   /* §4.1: `fileBits` and `fileName` are REQUIRED; only `options` is optional */
}

void blob_install(JSContext *ctx, JSValueConst global)
{
    JSValue ctor;
    DCHECK(g_blob_ctor_stepid >= 0, "Blob was installed before blob_init declared its constructor");
    ctor = idl_step_constructor(ctx, "Blob", 0, g_blob_ctor_stepid);
    CHECK(!JS_IsException(ctor), "the Blob interface object could not be allocated");
    JS_SetConstructor(ctx, ctor, g_blob_proto);
    JS_SetPropertyStr(ctx, (JSValue)global, "Blob", ctor);

    ctor = idl_step_constructor(ctx, "File", 2, g_file_ctor_stepid);
    CHECK(!JS_IsException(ctor), "the File interface object could not be allocated");
    JS_SetConstructor(ctx, ctor, g_file_proto);
    JS_SetPropertyStr(ctx, (JSValue)global, "File", ctor);
}

void blob_free(JSContext *ctx)
{
    if (!g_blob_rt)
        return;
    JS_FreeValue(ctx, g_blob_proto);
    JS_FreeValue(ctx, g_file_proto);
    g_blob_proto = g_file_proto = JS_UNDEFINED;
    g_blob_rt = NULL;
    g_blob_ctor_stepid = g_file_ctor_stepid = -1;
}
