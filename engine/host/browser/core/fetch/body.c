/* THE BODY MIXIN — WHATWG Fetch §5.3 "Body mixin", which BOTH Request and Response include.
 *
 * It lived inside response.c, which is where it was first needed and not where it belongs: `Request` includes
 * the same mixin, so the alternative was a second copy of the single-use latch, the readers and the
 * promise-settling machine — and the latch is COW-captured state, so two copies would be two places for a
 * time-travel bug to live.
 *
 * WHAT IS LEFT HERE IS WHAT IS FETCH'S. The machine that turns bytes into a settled promise moved to
 * core/byte_reader.c when File API's Blob turned out to define the same three readers: `blob.text()` is
 * `response.text()`, and Fetch is the spec that depends on File API rather than the other way round. What stays
 * is the part no other spec shares — §5.3's "consume body" latch, the Content-Type dispatch `formData()`
 * performs, and the `bodyUsed` attribute. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "solver/cow.h"
#include "core/byte_reader.h"
#include "core/encoding/text_stream.h"
#include "core/fetch/body.h"
#include "core/idl_args.h"
#include "core/file/blob.h"
#include "core/html/form_data.h"
#include "core/url/url.h"
#include "core/streams/readable_stream.h"
#include "core/url/url_search_params.h"

/* One interface that includes Body. There are two in the platform (Request and Response), so the table is
   fixed and full is a DCHECK rather than a growth path nobody exercises. */
#define BODY_IFACE_MAX 4
typedef struct {
    JSClassID    class_id;
    BodyState *(*of)(JSValueConst v);
    char      *(*mime)(JSContext *ctx, JSValueConst v);
    char      *(*source)(JSContext *ctx, JSValueConst v);
    const char  *iface;
    int          reader_handle;
} BodyIface;

static BodyIface g_body_iface[BODY_IFACE_MAX];
static int g_body_iface_n;

void body_state_mark(JSRuntime *rt, BodyState *b, JS_MarkFunc *mark_func)
{
    JS_MarkValue(rt, b->stream, mark_func);
}

void body_state_free(JSRuntime *rt, BodyState *b)
{
    JS_FreeValueRT(rt, b->stream);
    b->stream = JS_UNDEFINED;
    js_free_rt(rt, b->bytes);
    b->bytes = NULL;
    b->len = 0;
    b->has = 0;
    b->source_null = 0;
}

int body_state_set(JSContext *ctx, BodyState *b, const char *bytes, size_t len)
{
    /* THE STREAM SLOT IS SPELLED, not left zeroed. An including interface allocates its record with js_mallocz,
       and a zeroed JSValue is the INTEGER 0 — not JS_UNDEFINED — so "has a stream been built yet" read off the
       slot answers YES before one ever was. Every field of this state is set here, which is the one place a
       body is ever filled. */
    if (!b->has && !b->bytes && JS_VALUE_GET_TAG(b->stream) == JS_TAG_INT)
        b->stream = JS_UNDEFINED;
    js_free(ctx, b->bytes);
    b->bytes = NULL;
    b->len = 0;
    b->has = bytes != NULL;
    /* §5.2's `source` FOR EVERY ARM THAT NAMES ONE. This is the one place a body is filled from bytes, and
       every arm of the extraction that reaches it — Blob, byte sequence, BufferSource, FormData,
       URLSearchParams, scalar value string — sets source to something non-null; the ReadableStream arm does
       not come through here at all and states its own answer. A null body (`new Response()`) is never asked:
       §5.4 step 39 is guarded by "inputOrInitBody is non-null" before it reads this. */
    b->source_null = 0;
    if (!bytes) return 0;
    /* +1 and a NUL past the end, so the bytes can still be handed to a C string consumer; `len` is what every
       read here uses, and it is what an interior NUL no longer truncates. */
    b->bytes = js_mallocz(ctx, len + 1);
    if (!b->bytes) return -1;
    if (len) memcpy(b->bytes, bytes, len);
    b->len = len;
    return 0;
}

/* §2.2.4 "Bodies"' "clone a body" — the tee, and the reason body.h gives for it being one. */
int body_clone_run(JSContext *ctx, uint8_t *phase, JSValue *cb, int cb_cap, BodyState *dst, BodyState *src,
                   JSValue in, JSValue **out_cb, int *out_argc)
{
    JSValue out, b0, b1;
    int r;

    if (*phase == 0) {
        /* A NULL BODY IS NOT AN EMPTY ONE, and it has no stream to tee — §2.2.4's clone step is guarded by
           "if body is null, return null" and this is that guard. */
        if (!src->has) {
            JS_FreeValue(ctx, in);
            dst->has = 0;
            dst->source_null = 0;
            return 0;
        }
        /* In the spec a body IS a stream; here one is built on demand from the bytes, so the tee's operand has
           to exist before the tee can be asked for. Building it runs none of the page's code. */
        if (JS_IsUndefined(src->stream)) {
            src->stream = readable_stream_from_bytes(ctx, src->bytes ? src->bytes : "", src->len);
            if (JS_IsException(src->stream)) {
                src->stream = JS_UNDEFINED;
                JS_FreeValue(ctx, in);
                return -1;
            }
        }
    }
    {
        JSValue op = readable_stream_op(ctx, RS_OP_TEE_CLONE);   /* THIS realm's §4.9.1 tee */
        r = step_call_run(ctx, phase, cb, cb_cap, op, src->stream, 0, NULL,
                          in, &out, out_cb, out_argc);
        JS_FreeValue(ctx, op);
    }
    if (r > 0) return r;
    if (JS_IsException(out)) return -1;
    /* §4.2's `tee` answers a two-element Array this component built, so reading its indices runs no page code
       — the properties are the array's own, and no prototype getter is reached. */
    b0 = JS_GetPropertyUint32(ctx, out, 0);
    b1 = JS_GetPropertyUint32(ctx, out, 1);
    JS_FreeValue(ctx, out);
    DCHECK(readable_stream_is(b0) && readable_stream_is(b1),
           "a tee answered something other than two ReadableStreams");

    /* BOTH SIDES ARE NOW STREAM-BACKED. The bytes go, because after the tee they are no longer where the body
       is: the branch is, and a reader that took the bytes would deliver data the branch has not yielded. */
    js_free(ctx, src->bytes);
    src->bytes = NULL;
    src->len = 0;
    JS_FreeValue(ctx, src->stream);
    src->stream = b0;

    dst->bytes = NULL;
    dst->len = 0;
    dst->used = 0;   /* §2.2.4: the clone gets its OWN single-use latch, which is the whole point of cloning */
    dst->has = 1;
    /* §2.2.4 Bodies' clone-a-body: "Return a body whose stream is out2 and OTHER MEMBERS ARE COPIED FROM
       BODY" — the source is one of those members, so a clone of a byte-sourced body still has a source even
       though both sides are stream-backed after the tee. Recomputing it from the post-tee shape would say
       `null` for both and make `new Request(response.clone().body ...)` demand a duplex the spec does not. */
    dst->source_null = src->source_null;
    JS_FreeValue(ctx, dst->stream);
    dst->stream = b1;
    return 0;
}

/* Streams §9.5 Piping's "create a proxy" — "To create a proxy for a ReadableStream stream … The result will
   be a new ReadableStream object which pulls its data from stream, WHILE STREAM ITSELF BECOMES IMMEDIATELY
   LOCKED AND DISTURBED." Fetch §5.4 new Request(input, init) step 41 is the one caller: a Request built out of
   another Request takes its body, and the INPUT is unusable afterwards, which is why step 41 refuses an
   already-unusable one first.
   IT IS NOT THE TEE ABOVE, and reading the two as one is the mistake this exists to make impossible: §2.2.4's
   clone leaves BOTH sides readable, and a proxy leaves exactly one. `new Request(r); r.text()` must throw, and
   with a tee it would resolve.
   In this engine a body is BYTES with a stream built on demand, so the proxy is the bytes plus the source's
   disturbance latch — observably the same object graph, and it needs no identity transform and no park. */
int body_create_proxy(JSContext *ctx, JSValueConst src_obj, BodyState *src, BodyState *dst)
{
    DCHECK(src->has, "§5.4 step 41 asked for a proxy of a NULL body — step 41 is guarded by \"inputBody is "
                     "non-null\", so a caller reaching here with none skipped that guard");
    DCHECK(src->bytes != NULL,
           "a STREAM-BACKED body reached Fetch §5.4 step 41's create-a-proxy: §5.2's ReadableStream arm leaves "
           "the bytes in the stream, and Streams §9.5 Piping's create-a-proxy pipes it through an identity "
           "TransformStream — which this engine has no body representation for. It is the SAME unbuilt "
           "capability fetch()'s host edge names: read the stream to the end as a stage of the machine that "
           "needs the bytes, and hand those bytes on");
    if (body_state_set(ctx, dst, src->bytes, src->len) < 0)
        return -1;
    /* §5.2's `source` IS ONE OF THE MEMBERS A PROXY CARRIES OVER, and it is stated here rather than left to
       the byte fill above because the byte fill is this ENGINE's representation of a proxy and not the
       operation's answer. The DCHECK above makes the two agree today — a proxy of a stream-backed body
       aborts, so `src->source_null` is 0 at every reachable call — and the day that capability lands, step
       39 asks this question of the proxied body and must not be told a source exists because bytes were
       copied. */
    dst->source_null = src->source_null;
    /* "stream itself becomes immediately locked and disturbed" — the same latch a read sets, captured into the
       running flow's delta first, because the source Request is shared baseline state for every sibling arm. */
    cow_capture_host_state(ctx, src_obj, &src->used, sizeof src->used);
    src->used = 1;
    return 0;
}

/* §5.2'S "EXTRACT A BODY", ONCE, for both interfaces that take a BodyInit.
 *
 * It was written twice — in the Response constructor and in the Request one — and each copy knew TWO of the
 * union's six arms: a BufferSource, and everything else stringified. That is why `new Response(blob)` carried
 * the thirteen bytes of "[object Blob]" with a Content-Type of text/plain, and a FormData body carried
 * "[object FormData]". Six arms in one place is the point of a union having one rule.
 *
 * The MIME type is the extraction's second output, not the caller's guess: §5.2 gives each arm its own type or
 * none, and the constructor's only job with it is §5.5's "set Content-Type if the header list has none".
 * `*out_mime` is malloc'd, or NULL for an arm with no type. Returns -1 with a throw live. */
int body_extract(JSContext *ctx, BodyState *b, JSValueConst init, bool keepalive, char **out_mime)
{
    const UrlEncodedList *list;
    size_t len = 0;
    int r = -1;

    *out_mime = NULL;
    if (JS_IsNull(init) || JS_IsUndefined(init))
        return body_state_set(ctx, b, NULL, 0);

    {
        size_t blen = 0;
        const char *btype = NULL;
        const char *bytes = blob_bytes_of(init, &blen, &btype);
        if (bytes) {
            /* §5.2: a Blob's type IS the Content-Type, and a Blob with no type contributes none — which is why
               an empty type must give a NULL mime and not an empty header. */
            if (btype && *btype) *out_mime = strdup(btype);
            return body_state_set(ctx, b, bytes, blen);
        }
    }
    if (form_data_is(init)) {
        char boundary[FORM_DATA_BOUNDARY_MAX];
        char *body = form_data_serialize_multipart(ctx, init, boundary, &len);
        size_t n;
        if (!body) return -1;
        n = strlen(boundary) + sizeof("multipart/form-data; boundary=");
        r = body_state_set(ctx, b, body, len);
        free(body);
        *out_mime = malloc(n);
        CHECK(*out_mime, "body: OOM naming a multipart boundary");
        snprintf(*out_mime, n, "multipart/form-data; boundary=%s", boundary);
        return r;
    }
    if ((list = usp_list_of(init)) != NULL) {
        char *body = url_encoded_serialize(list, &len);
        r = body_state_set(ctx, b, body, len);
        free(body);
        *out_mime = strdup("application/x-www-form-urlencoded;charset=UTF-8");
        return r;
    }
    /* §5.2's BufferSource ARM SAYS "Set source to a copy of the bytes held by object", and "copy of the bytes"
       is a LINK: Web IDL §3.2.26 Buffer source types' `get a copy of the bytes held by the buffer source`,
       whose STEP 7 is "If IsDetachedBuffer(jsArrayBuffer) is true, then return the empty byte sequence." So
       `new Request(u, {body: detached})` carries an EMPTY body and does not throw.
       ASKED ONCE, ABOVE BOTH SHAPES, BECAUSE THE WINDOW IS WHAT CANNOT BE READ FOR A DETACHED ONE — steps 5-6
       read internal slots step 7 discards, and this engine's two routes to them both refuse: JS_GetArrayBuffer
       throws for a detached buffer, JS_GetArrayBufferView refuses an out-of-bounds view and a detached buffer
       makes every view over it out of bounds. The arm below returned -1 with that TypeError live, and the view
       arm further down aborted on a DCHECK about the UNION, which is a true sentence about a value that had
       already satisfied it.
       THE BRAND TEST IS THIS CALLER'S, not the predicate's, because this function's last arm deliberately
       accepts a plain object: `fetch(u, {body: {...}})` reads init["body"] with a raw property get and has no
       declaration to convert it (see the DCHECK there). Asking a buffer-source predicate about such a value
       would be an ALWAYS-FATAL brand CHECK in release for a page that merely wrote a wrong body. */
    if (JS_IsArrayBuffer(init) || JS_GetTypedArrayType(init) >= 0 || JS_IsDataView(init)) {
        if (JS_IsDetachedBufferSource(init))
            return body_state_set(ctx, b, NULL, 0);              /* §5.2: a BufferSource has no type */
    }
    if (JS_IsArrayBuffer(init)) {
        size_t n = 0;
        const uint8_t *base = JS_GetArrayBuffer(ctx, &n, (JSValue)init);
        if (!base) return -1;
        return body_state_set(ctx, b, (const char *)base, n);   /* §5.2: a BufferSource has no type */
    }
    if (readable_stream_is(init)) {
        /* §5.2's first arm: the body's stream IS this one. There are no bytes until it is read, and `.body`
           answers the very stream the page handed in — which is what makes `new Response(rs).body === rs`
           behave and what a page teeing a response's body depends on. A stream has no Content-Type. */
        /* THE ARM'S OWN REFUSAL, which is a step of the extraction and not a caller's guard: "If object is
           disturbed or locked, then throw a TypeError." A stream someone already holds a reader on has no
           bytes left to give this body — `const r = rs.getReader(); new Response(rs)` must throw where the
           page wrote it, and without this it succeeded and handed back a Response whose body could never be
           read, the failure arriving later at a `text()` nothing in the page connected to the constructor.
           Both slots are read through the INTERNAL operations: `locked` is "[[reader]] is not undefined" and
           `disturbed` is §4.2's flag, so a page that patched `ReadableStream.prototype.locked` cannot decide
           whether its own stream is acceptable as a body. */
        /* …AND THE ARM'S OTHER REFUSAL, which §5.2 lists FIRST and which is why this function takes the flag:
           "If keepalive is true, then throw a TypeError." A keepalive request may outlive the environment
           settings object, and Fetch §4.6's quota is stated over a body whose LENGTH is known — a stream has
           no length until it is read, so there is nothing for the quota to be computed against. Beacon §3
           step 6.1 extracts with the flag set, so `navigator.sendBeacon(u, stream)` throws where the page
           wrote it rather than composing a request out of bytes that do not exist yet. It is checked BEFORE
           disturbed/locked because that is the order §5.2 lists the two steps in, and a page that hands over
           an already-locked stream to a keepalive request must see the keepalive TypeError. */
        bool locked = false;
        if (keepalive) {
            JS_ThrowTypeError(ctx, "a ReadableStream cannot be the body of a keepalive request");
            return -1;
        }
        readable_stream_query(init, NULL, &locked);
        if (locked || readable_stream_disturbed(init)) {
            JS_ThrowTypeError(ctx, "a body cannot be extracted from a ReadableStream that is disturbed or "
                                   "locked");
            return -1;
        }
        JS_FreeValue(ctx, b->stream);
        b->stream = JS_DupValue(ctx, init);
        /* js_free, because body_state_set allocates these with js_mallocz and body_state_free releases them
           with js_free_rt — the libc `free` that stood here would have handed a quickjs allocation to the
           wrong allocator the first time this arm ran over a state that already carried bytes. */
        js_free(ctx, b->bytes);
        b->bytes = NULL;
        b->len = 0;
        b->has = 1;
        /* §5.2's SWITCH GIVES THIS ARM NO `source` LINE, and that silence is the whole of what step 39 reads.
           "Let source be null" opens the algorithm and every other arm assigns over it; a ReadableStream body
           has no bytes to name, so its source stays null and Fetch §5.4 new Request(input, init) step 39
           demands `duplex` before it will carry one. It is set HERE and not left to body_state_set, which
           this arm deliberately does not call. */
        b->source_null = 1;
        return 0;
    }
    if (JS_IsObject(init)) {
        size_t off = 0, n = 0, whole = 0;
        JSValue buf = JS_GetArrayBufferView(ctx, init, &off, &n);
        uint8_t *base;
        /* THE CLAIM THIS ASSERT USED TO MAKE IS FALSE FOR THE PATH THAT REACHES IT, and a message that names
           the wrong file is worse than no message: it sends the next reader to audit a conversion that is
           correct. It said "the declaration converts the USVString arm", which is true of every caller that
           HAS a declaration — `new Request(url, init)` and `new Response(body, init)` both take
           IDL_BODYINIT_NULLABLE, whose union rule (idl_args.c) brand-tests the seven arms and sends everything
           else to IDL_DOMSTRING before this function is ever reached.
           `fetch(url, init)` HAS NO SUCH DECLARATION FOR THIS MEMBER. It reads `init["body"]` with a raw
           property get in its FETCH_INIT_BODY stage and hands the value straight here, so a plain object —
           `fetch(u, {body: {toString(){ return "hi" }}})`, which WPT's request-init-002 checks — arrives
           unconverted, falls into the BufferSource arm above because that arm tests `JS_IsObject`, and lands
           on this line. It is the RequestInfo defect one stage down and in the same function: a union resolved
           by SHAPE where the spec resolves it by BRAND, with the USVString arm unreachable for objects.
           THE FIX IS NOT HERE AND IT IS NOT A SECOND COPY OF THE UNION. fetch()'s body member has to go
           through the same rule the declaration states — one exported brand predicate, called from both — and
           the USVString arm's ToString has to be a STAGE in js_fetch_step, because it runs the page's code
           (FETCH_INPUT_URL_STR is the shape it takes). Re-stating the arm list in fetch.c would make three
           copies of one union, which is the thing this assert exists to keep honest. */
        DCHECK(!JS_IsException(buf),
               "the BodyInit union let through an object that is none of its arms — every caller with a "
               "DECLARATION converts the USVString arm before this point, so the caller that reached here has "
               "none: fetch(input, init) reads init[\"body\"] with a raw property get and never runs the "
               "union's rule");
        base = JS_GetArrayBuffer(ctx, &whole, buf);
        if (!base) { JS_FreeValue(ctx, buf); return -1; }
        r = body_state_set(ctx, b, (const char *)base + off, n);
        JS_FreeValue(ctx, buf);
        return r;
    }
    {
        /* The USVString arm: the declaration already ran ToString, so this is the string's bytes. */
        const char *str = JS_ToCStringLen(ctx, &len, init);
        if (!str) return -1;
        r = body_state_set(ctx, b, str, len);
        JS_FreeCString(ctx, str);
        *out_mime = strdup("text/plain;charset=UTF-8");
        return r;
    }
}

/* WHICH INCLUDING INTERFACE the receiver belongs to. The reader machine finds the interface the same way and
   for the same reason; this is the Fetch-side table, which knows about the latch and the headers. */
static const BodyIface *body_iface_of(JSValueConst v)
{
    int i;
    for (i = 0; i < g_body_iface_n; i++)
        if (g_body_iface[i].of(v)) return &g_body_iface[i];
    return NULL;
}

static bool body_iface_is(JSValueConst v) { return body_iface_of(v) != NULL; }

/* A MIME type PARAMETER, which §5.3 needs exactly one of: `boundary`. Quoted or a token, and it must follow a
   `;` so `boundary` does not match inside a longer parameter name. NULL when absent. */
static const char *body_mime_param(const char *s, size_t n, const char *key, size_t *out_len)
{
    size_t klen = strlen(key), i;
    for (i = 0; i + klen + 1 <= n; i++) {
        size_t j;
        if (strncasecmp(s + i, key, klen) || s[i + klen] != '=') continue;
        j = i;
        while (j > 0 && (s[j - 1] == ' ' || s[j - 1] == '\t')) j--;
        if (j == 0 || s[j - 1] != ';') continue;
        {
            const char *v = s + i + klen + 1;
            size_t rest = n - (size_t)(v - s);
            if (rest && *v == '"') {
                const char *q = memchr(v + 1, '"', rest - 1);
                if (!q) return NULL;
                *out_len = (size_t)(q - (v + 1));
                return v + 1;
            }
            *out_len = strcspn(v, ";");
            if (*out_len > rest) *out_len = rest;
            return v;
        }
    }
    return NULL;
}

/* §5.3 "consume body": the latch is per object and the second read is a TypeError — a page's retry path tests
   exactly that, so answering the body twice would hide the branch it takes. It is the whole of what Fetch's
   read does differently from File API's, which is why the shared machine asks for it rather than holding it.
   THE LATCH IS PER FLOW, TOO. It lives in the including component's class opaque, which no property hook and no
   engine hook can see, so setting it was a write that did not time-travel: a Response created before a fork and
   read in one arm came back CONSUMED in the sibling, whose own first read then threw `body stream already
   read`. Every other kind of shared state a flow writes rides its COW delta, and so does this one — the capture
   goes immediately before the write, so the bytes the delta records are the ones this flow found. */
static int body_take(JSContext *ctx, JSValueConst this_val, const char **pbody, size_t *plen,
                     JSValue *pstream)
{
    const BodyIface *f = body_iface_of(this_val);
    BodyState *b;

    *pstream = JS_UNDEFINED;
    if (!f) {
        JS_ThrowTypeError(ctx, "not a Request or a Response");
        return -1;
    }
    b = f->of(this_val);
    *pbody = b->bytes ? b->bytes : "";
    *plen  = b->bytes ? b->len : 0;
    /* A NULL BODY IS NEVER DISTURBED. §5.3's consume returns an empty byte sequence without touching the
       stream when the body is null, so `new Response()` reads as "" as many times as it is asked and its
       `bodyUsed` stays FALSE — where `new Response("")`, whose body is EMPTY rather than null, latches on the
       first read. The two are the same zero bytes and different objects, which is exactly the distinction
       `has` exists to keep. */
    if (!b->has)
        return 0;
    if (b->used) {
        JS_ThrowTypeError(ctx, "body stream already read");
        return -1;
    }
    cow_capture_host_state(ctx, this_val, &b->used, sizeof b->used);
    b->used = 1;
    /* §5.2's FIRST union arm: a body can BE a ReadableStream, and then there are no bytes to hand back — the
       stream is fully read first. The latch is set either way, because it is set by the CONSUME and not by the
       bytes arriving. */
    if (!b->bytes && !JS_IsUndefined(b->stream) && readable_stream_is(b->stream))
        *pstream = JS_DupValue(ctx, b->stream);
    return 0;
}

/* §5.3's formData(): WHICH PARSER runs is decided by the Content-Type, and a body whose type is neither of the
   two form encodings is a TypeError rather than an empty FormData — a page that calls formData() on a JSON reply
   has a bug, and answering with no entries would hide it. Fetch's own reader, which is why it is declared here
   and not beside the four that every byte sequence answers. */
static JSValue body_read_form_data(JSContext *ctx, JSValueConst recv, const char *body, size_t len)
{
    const BodyIface *f = body_iface_of(recv);
    char *mime;
    const char *essence;
    size_t elen;
    JSValue r = JS_EXCEPTION;

    DCHECK(f != NULL, "formData() reached its reader with a receiver of no including interface");
    mime = f->mime ? f->mime(ctx, recv) : NULL;
    essence = mime ? mime : "";
    elen = strcspn(essence, ";");
    while (elen && (essence[elen - 1] == ' ' || essence[elen - 1] == '\t')) elen--;

    if (elen == (sizeof("application/x-www-form-urlencoded") - 1) &&
        !strncasecmp(essence, "application/x-www-form-urlencoded", elen)) {
        UrlEncodedList entries = { 0 };
        url_encoded_parse(&entries, body, len);
        r = form_data_new(ctx, &entries);
        url_encoded_list_free(&entries);
    } else if (elen == (sizeof("multipart/form-data") - 1) &&
               !strncasecmp(essence, "multipart/form-data", elen)) {
        /* The BOUNDARY is the Content-Type's own parameter; without one there is nothing to split on and §5.3
           says failure, which is the same TypeError a wrong type gives. */
        size_t blen = 0;
        const char *bd = body_mime_param(essence, strlen(essence), "boundary", &blen);
        if (bd)
            r = form_data_parse_multipart(ctx, body, len, bd, blen);
        else
            JS_ThrowTypeError(ctx, "the multipart Content-Type names no boundary");
    } else {
        JS_ThrowTypeError(ctx, "the body's Content-Type is not a form encoding");
    }
    free(mime);
    return r;
}

/* §5.3's READERS. `blob()` is declared here too, and is the same read as `arrayBuffer()` with a Blob around the
   bytes — its MIME type comes from the including object's Content-Type, which is why File API cannot declare it
   and Fetch can. */
static JSValue body_read_blob(JSContext *ctx, JSValueConst recv, const char *bytes, size_t len)
{
    const BodyIface *f = body_iface_of(recv);
    char *mime;
    JSValue r;

    DCHECK(f != NULL, "blob() reached its reader with a receiver of no including interface");
    mime = f->mime ? f->mime(ctx, recv) : NULL;
    r = blob_new(ctx, bytes, len, mime ? mime : "");
    free(mime);
    return r;
}

static const ByteReader BODY_READERS[] = {
    { "text",        byte_reader_text },
    { "json",        byte_reader_json },
    { "arrayBuffer", byte_reader_array_buffer },
    { "bytes",       byte_reader_bytes },
    { "blob",        body_read_blob },
    { "formData",    body_read_form_data },
};

/* WHERE A BODY'S BYTES CAME FROM — core/byte_reader.h's question, and the MIXIN's answer to it is to ask the
   INCLUDING INTERFACE, exactly as `formData()` asks it for the Content-Type. Only that interface knows: a
   Response was filled by a server at an address it holds, and a Request's body is bytes the PAGE composed and
   handed to `fetch()`, which is the NULL this returns as a statement rather than a hole.
   `false` because a reply is server-injected state and NOT a declared attacker delivery — solver/concolic.h's
   read counter is over deliveries the attacker authors, and counting a reply there would report a page that
   read no attacker source as one that read many. */
static char *body_reader_source(JSContext *ctx, JSValueConst v, bool *attacker)
{
    const BodyIface *f = body_iface_of(v);

    DCHECK(f != NULL, "a body was asked where its bytes came from with a receiver of no including interface — "
                      "the reader machine checks the receiver before it calls a reader at all, so a NULL here "
                      "is this mixin's `is` and its own table disagreeing");
    if (!f || !f->source)
        return NULL;
    *attacker = false;
    return f->source(ctx, v);
}

static const ByteReaderIface BODY_READER_IFACE = {
    body_iface_is, body_take, "Body", BODY_READERS,
    (int)(sizeof BODY_READERS / sizeof BODY_READERS[0]),
    body_reader_source
};


/* §5.3's `bodyUsed`: the body is non-null AND its stream is DISTURBED. The latch this component sets when a
   reader consumes the bytes is one way to disturb it; reading the `body` stream directly is the other, and a
   page that does the second and asks the first must be told the truth. */
static JSValue js_body_get_used(JSContext *ctx, JSValueConst this_val, int magic)
{
    const BodyIface *f = &g_body_iface[magic];
    BodyState *b = f->of(this_val);
    if (!b) return JS_ThrowTypeError(ctx, "not a %s", f->iface);
    return JS_NewBool(ctx, b->used != 0 || readable_stream_disturbed(b->stream));
}

/* THE BODY'S STREAM, BUILT ON FIRST DEMAND AND THEN THE SAME ONE FOR EVER. In the spec a body IS a stream;
   here it is bytes with a stream made when something first needs one, and "the same one" is the load-bearing
   half: a second would give the page two independent readers over one byte sequence, so `response.body ===
   response.body` and the stream `textStream()` decodes is the stream `bodyUsed` reports the disturbance of.
   ONE PLACE, because there are now two members that need it and the second copy is where they would drift.
   Returns 0, or -1 with a throw live. The caller has already established the body is non-null. */
static int body_stream_ensure(JSContext *ctx, BodyState *b)
{
    DCHECK(b->has, "a null body was asked for its stream — §5.3 answers null for one, and every caller of this "
                   "checks `has` first because that is the distinction the flag exists to keep");
    if (!JS_IsUndefined(b->stream)) return 0;
    b->stream = readable_stream_from_bytes(ctx, b->bytes ? b->bytes : "", b->len);
    if (JS_IsException(b->stream)) { b->stream = JS_UNDEFINED; return -1; }
    return 0;
}

/* §5.3's `body`. NULL for a null body — which is not an empty one — and otherwise the SAME stream every time,
   because a second would give the page two independent readers over one byte sequence. */
static JSValue js_body_get_body(JSContext *ctx, JSValueConst this_val, int magic)
{
    const BodyIface *f = &g_body_iface[magic];
    BodyState *b = f->of(this_val);
    if (!b) return JS_ThrowTypeError(ctx, "not a %s", f->iface);
    if (!b->has) return JS_NULL;
    if (body_stream_ensure(ctx, b) < 0) return JS_EXCEPTION;
    return JS_DupValue(ctx, b->stream);
}

/* §5.3's `[NewObject] ReadableStream textStream()`, for BOTH including interfaces.
 *
 * IT IS THE ONE MEMBER OF THIS MIXIN THAT THROWS RATHER THAN REJECTING, and that is not an accident of this
 * implementation: its IDL return type is `ReadableStream` and not `Promise<…>`, so Web IDL has no promise to
 * reject into and step 1's TypeError arrives where the page wrote the call. Every other reader here answers a
 * rejected promise for the same condition. A page can tell those apart with a bare `try`.
 *
 * STEPS 4-6 ARE NOT HERE. They are File API §3.3.6's steps 2-4 word for word, so they are one operation in
 * core/encoding/text_stream.c and this member performs its own steps 1-3 and then calls it.
 *
 * THE RESIDUAL THAT STOOD HERE NAMED TWO EXPORTED ENTRIES AND WAS WRONG ABOUT BOTH HALVES OF THAT, which is
 * worth keeping because its next-diff clause is the kind only the person acting on it ever reads. It said the
 * pipe was Streams §4.2.4 "Constructor, methods, and properties"' `pipeThrough`; that member converts a
 * ReadableWritablePair out of a page-supplied object and reads a StreamPipeOptions off another, and driving a
 * host's own bytes through it would let a patched accessor choose the destination. The operation is §9.5
 * Piping's "piped through", which reads `transform.[[writable]]` and `[[readable]]` as slots and whose first
 * two steps are ASSERTS. And it counted two entries where there are three: step 2's empty stream must be SET
 * UP AND CLOSED, and readable_stream_from_bytes cannot answer it — that one enqueues a chunk, so a
 * zero-length call hands back `{ value: Uint8Array(0), done: false }` where step 2's stream answers `done`. */
#define BTS_STAGES(X) \
    X(BTS_ENTRY = IDL_STEP_FIRST, \
      "Fetch §5.3 textStream() steps 1-3 (the unusable check, the null-body arm's set-up-and-closed stream, " \
      "and this's body's stream)") \
    X(BTS_DECODE, \
      "Fetch §5.3 textStream() steps 4-6 (a new TextDecoderStream set up with UTF-8, and the result of " \
      "stream piped through it)") \
    X(BTS_DONE, "Fetch §5.3 textStream() (the member's ReadableStream is its result)")
enum { BTS_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const BTS_STEPS[] = { BTS_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    uint8_t phase;
    JSValue stream;    /* §5.3 step 3's "this's body's stream" (owned) */
    JSValue cb[2];     /* step_call_run's buffer: [this, func] — the decode takes no arguments */
} JSBodyTextState;

static void js_body_text_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSBodyTextState *s = st;
    int k;
    v->val(ctx, &s->stream);
    for (k = 0; k < 2; k++) v->val(ctx, &s->cb[k]);
}

static int js_body_text_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                             JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    JSBodyTextState *s = st;
    int r;

    (void)argc; (void)argv;
    if (hdr->stage == BTS_ENTRY) {
        const BodyIface *f;
        BodyState *b;
        bool locked = false;

        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        /* Every owned slot before anything that can throw — the failure path frees what the state holds. */
        s->stream = JS_UNDEFINED;
        s->cb[0] = s->cb[1] = JS_UNDEFINED;

        f = body_iface_of(hdr->this_val);
        if (!f) return JS_ThrowTypeError(ctx, "not a Request or a Response"), -1;
        b = f->of(hdr->this_val);
        DCHECK(b != NULL, "the Body mixin's own table matched a receiver its `of` then had no state for");

        /* §5.3 step 1: "If this is unusable, then throw a TypeError." §5.3 defines unusable as "its body is
           non-null and its body's stream is disturbed or locked" — BOTH halves, where `bodyUsed` is the
           disturbed half alone, which is why this asks the question again rather than reading that getter.
           The consume latch is one way to disturb the stream and reading it directly is the other, so the
           three conditions are asked together. Both slots go through the INTERNAL operations, so a page that
           patched `ReadableStream.prototype.locked` cannot decide whether its own body is usable. */
        readable_stream_query(b->stream, NULL, &locked);
        if (b->has && (b->used || locked || readable_stream_disturbed(b->stream)))
            return JS_ThrowTypeError(ctx, "the body is unusable"), -1;

        /* §5.3 step 2: a NULL body — which is not an empty one — answers a stream that is set up and CLOSED,
           so its first read is `done` with no chunk. It returns that stream directly: no decoder is made and
           nothing is piped, because there is nothing to decode. */
        if (!b->has) {
            *presult = readable_stream_closed_empty(ctx);
            if (JS_IsException(*presult)) return -1;
            STEP_GOTO(hdr->stage, BTS_DONE, &s->phase, NULL);
            return 0;
        }

        /* §5.3 step 3: "Let stream be this's body's stream." */
        if (body_stream_ensure(ctx, b) < 0) return -1;
        s->stream = JS_DupValue(ctx, b->stream);
        STEP_GOTO(hdr->stage, BTS_DECODE, &s->phase, NULL);
    }

    if (hdr->stage == BTS_DECODE) {
        JSValue op = text_stream_decode_op(ctx);
        JSValue out;

        r = step_call_run(ctx, &s->phase, STEP_CB(s->cb), op, s->stream, 0, NULL,
                          cb_result, &out, out_cb, out_argc);
        JS_FreeValue(ctx, op);
        if (r > 0) return r;
        if (JS_IsException(out)) return -1;
        STEP_GOTO(hdr->stage, BTS_DONE, &s->phase, NULL);
        *presult = out;
        return 0;
    }

    DFAIL("Fetch §5.3's textStream() resumed at a stage it does not have");
    JS_FreeValue(ctx, cb_result);
    return -1;
}

static const IdlStepDecl js_body_text_decl = {
    js_body_text_step, sizeof(JSBodyTextState), js_body_text_visit, NULL,
    "Fetch §5.3 textStream()", BTS_STEPS,
    /* `catches_abrupt` = 0: this member PROPAGATES. Step 1's TypeError is thrown before any request has been
       made, and the decode's own abrupt — a throwing `enqueue` the page put on the controller prototype, a
       transform that errored — is the page's to see at the call it wrote. There is nothing here for a body to
       do with a throw that the epilogue does not already do. `unforkable` = NULL: every slot this state holds
       is a JSValue the visit above names, so a fork copies it whole. */
    0, NULL
};

/* ONE MACHINE FOR BOTH INCLUDING INTERFACES, like the readers above: the member finds its receiver's state
   through the mixin's own table rather than through a magic, so the two prototypes install the same
   declaration and there is one algorithm to be right about. */
static int g_body_text_stepid = -1;

int body_declare(JSContext *ctx, JSClassID class_id, BodyState *(*of)(JSValueConst v),
                 char *(*mime)(JSContext *ctx, JSValueConst v),
                 char *(*source)(JSContext *ctx, JSValueConst v), const char *iface)
{
    int handle = g_body_iface_n;
    BodyIface *f;

    DCHECK(g_body_iface_n < BODY_IFACE_MAX,
           "more interfaces included Body than this table holds — grow it, the count is fixed because the "
           "platform's is");
    f = &g_body_iface[handle];
    f->class_id = class_id;
    f->of = of;
    f->mime = mime;
    f->source = source;
    f->iface = iface;
    g_body_iface_n++;
    /* ONE reader declaration for the mixin, shared by every including interface: the readers find their
       receiver's bytes through the one `take` above, which is the mixin's own algorithm and not either
       interface's. The declaration is made on the FIRST include and reused, so the two prototypes install the
       same behaviour under their own function objects. */
    if (handle == 0) {
        f->reader_handle = byte_reader_declare(ctx, &BODY_READER_IFACE);
        /* §5.3's `textStream()` is declared on the same include and for the same reason: one algorithm, whose
           receiver decides which interface it is running for. */
        g_body_text_stepid = idl_method_id_step(ctx, NULL, 0, NULL, 0, &js_body_text_decl, 0);
    } else {
        f->reader_handle = g_body_iface[0].reader_handle;
    }
    return handle;
}

void body_install(JSContext *ctx, JSValueConst proto, int handle)
{
    DCHECK(handle >= 0 && handle < g_body_iface_n, "Body was installed with a handle nothing declared");
    DCHECK(g_body_text_stepid >= 0,
           "the Body mixin was installed into a realm before body_declare declared its textStream machine — "
           "the declaration is made on the FIRST include and every realm's install reads it");
    byte_reader_install(ctx, proto, g_body_iface[handle].reader_handle);
    /* THROUGH idl_install_method, because this member has a POOL ENTRY — see the same note in
       core/file/blob.c. `textStream()` is declared with idl_method_id_step, so its §3.7.7 `length` is derived
       from the declaration and the raw step installer (whose caller must state a number) asserts on it. */
    idl_install_method(ctx, proto, "textStream", g_body_text_stepid);
    {
        JSCFunctionListEntry e[2] = {
            JS_CGETSET_MAGIC_DEF("bodyUsed", js_body_get_used, NULL, 0),
            JS_CGETSET_MAGIC_DEF("body", js_body_get_body, NULL, 0),
        };
        e[0].magic = e[1].magic = (int16_t)handle;
        JS_SetPropertyFunctionList(ctx, (JSValue)proto, e, 2);
    }
}
