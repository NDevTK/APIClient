/* Web Cryptography API §10's Crypto interface: §10.2.1's `subtle`, §10.1.1's `getRandomValues` and §10.1.2's
 * `randomUUID` — the whole of the interface. See crypto.h for the ARGUMENT behind what §10.1 returns, which is
 * the only part of this file that is a decision rather than a transcription of the standard's steps. */
#include <stdint.h>
#include <stdlib.h>

#include "check.h"
#include "quickjs.h"
#include "core/agent_state.h"
#include "core/crypto/crypto.h"
#include "core/crypto/subtle_crypto.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "solver/cow.h"       /* the §10.1 draw position is shared baseline state a flow mutates */

static JSClassID g_crypto_class;
static int       g_obj_slot = -1;
static int       g_id_get_random_values = -1;
static int       g_id_random_uuid = -1;

/* ---- §10.1's randomness --------------------------------------------------------------------------------- */

/* THIS REALM'S DRAW POSITION, counted in 64-bit words. crypto.h argues why §10.1 is answered from a
   reproducible stream rather than from entropy, and why this lives on the object instead of in a static. */
typedef struct { uint64_t drawn; } CryptoStream;

/* SplitMix64's finalizer over the position (Steele, Lea and Flood, "Fast splittable pseudorandom number
   generators", OOPSLA 2014). What matters here is that it is a BIJECTION on 64 bits — the multiply is by an
   odd constant and the finalizer is invertible — so two different positions cannot yield the same word and
   distinct draws are distinct with nothing having to check. It is not a CSPRNG and crypto.h says what must
   therefore never rest on it. */
static uint64_t crypto_stream_word(uint64_t position)
{
    uint64_t z = (position + 1) * UINT64_C(0x9E3779B97F4A7C15);

    z = (z ^ (z >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94D049BB133111EB);
    return z ^ (z >> 31);
}

/* Fill `n` bytes from this realm's stream and advance it.
   THE ADVANCE IS CAPTURED BEFORE IT HAPPENS. It is a write to a C record behind a class opaque, which
   solver/cow.h gives as its own worked example of a write that does not time-travel on its own: without this,
   the first arm of a fork to draw would move the position for every sibling, and a context switch would not
   put it back. Captured, each arm draws from the position the FORK was taken at — the same bytes a rewound
   and replayed execution observes — which is the whole of what makes §10.1 answerable at all here. */
static void crypto_stream_take(JSContext *ctx, JSValueConst crypto_obj, CryptoStream *s,
                               uint8_t *out, size_t n)
{
    size_t i;

    cow_capture_host_state(ctx, crypto_obj, &s->drawn, sizeof s->drawn);
    for (i = 0; i < n; i++)
        out[i] = (uint8_t)(crypto_stream_word(s->drawn + i / 8) >> (8 * (i % 8)));
    s->drawn += (n + 7) / 8;
}

/* THE RECORD AS A COLLECTOR ENTRY SEES IT — JS_GetAnyOpaque, and never JS_GetOpaque(val, g_crypto_class).
   core/agent_state.h states the rule and the reason: crypto_free gives the class id back, and the collection
   that finalizes this agent's object graph runs AFTER core/platform.h's release column, so a lookup against
   that id would answer NULL for every realm's Crypto and each would leak its record — a malloc'd block, which
   appears in NEITHER of JS_FreeRuntime's censuses and so is silent in dev and in release alike. */
static void crypto_finalizer(JSRuntime *rt, JSValue val)
{
    JSClassID cid = 0;
    CryptoStream *s = JS_GetAnyOpaque(val, &cid);

    (void)rt;
    free(s);
}

/* §10.1.1 STEP 1'S NINE TYPES ARE A RANGE OF JSTypedArrayEnum, AND THAT IS ASSERTED AT COMPILE TIME rather
   than by a DCHECK in the member. The relationship is between two constants, so a runtime check would be
   answering at the latest possible moment a question the compiler can settle — and it would be compiled out in
   release, which is the half where an upstream reordering would silently widen the set of views this member
   accepts. The enum's first nine ARE step 1's list ("Int8Array, Uint8Array, Uint8ClampedArray, Int16Array,
   Uint16Array, Int32Array, Uint32Array, BigInt64Array, or BigUint64Array") and the three float views follow
   them, which is the whole reason the test below can be `t > JS_TYPED_ARRAY_BIG_UINT64` instead of a list. */
_Static_assert(JS_TYPED_ARRAY_UINT8C == 0 && JS_TYPED_ARRAY_BIG_UINT64 == 8 && JS_TYPED_ARRAY_FLOAT16 == 9,
               "Web Cryptography §10.1.1 step 1's nine integer types are no longer the first nine values of "
               "JSTypedArrayEnum, so getRandomValues' range test no longer states step 1's list");

/* §10.1.1 The getRandomValues method. */
static JSValue js_crypto_get_random_values(JSContext *ctx, JSValueConst this_val,
                                           int argc, JSValueConst *argv, int magic)
{
    CryptoStream *s = JS_GetOpaque(this_val, g_crypto_class);
    size_t off = 0, len = 0, size = 0;
    uint8_t *base;
    JSValue buf;
    int t;

    (void)argc; (void)magic;
    /* WEB IDL §3.7.7's brand check, which for this member IS the record lookup: JS_GetOpaque answers NULL for
       anything that is not a Crypto, so there is no second test here to keep in step with the first. */
    if (s == NULL)
        return JS_ThrowTypeError(ctx, "getRandomValues was reached on something that is not a Crypto");

    /* STEP 1: "If array is not an Int8Array, Uint8Array, Uint8ClampedArray, Int16Array, Uint16Array,
       Int32Array, Uint32Array, BigInt64Array, or BigUint64Array, then throw a TypeMismatchError". Web IDL
       §4.1's ArrayBufferView typedef LISTS the three float views and DataView among its thirteen, so the
       declared type has already admitted them and this step is exactly their refusal — a DOMException where
       the conversion's would have been a TypeError, which is a difference a page tells apart.
       IT IS ASKED BEFORE STEPS 2-3, WHICH IS THE STANDARD'S ORDER AND IS OBSERVABLE: a Float32Array longer
       than 65536 bytes is step 1's TypeMismatchError and never step 3's QuotaExceededError, and the corpus
       asks for exactly that pair (`getRandomValues.any.js`, "Float32Array (too long)"). The range is stated
       once, at the _Static_assert above. */
    t = JS_GetTypedArrayType(argv[0]);
    if (t < 0 || t > JS_TYPED_ARRAY_BIG_UINT64)
        return JS_ThrowDOMException(ctx, "TypeMismatchError",
                                    "§10.1.1 step 1 takes an integer-typed view; a float view and a DataView "
                                    "are not among the nine types it lists");

    buf = JS_GetArrayBufferView(ctx, argv[0], &off, &len);
    if (JS_IsException(buf))
        return buf;

    /* STEPS 2-3: "Let byteLength be the byte length of array. If byteLength is greater than 65536, throw a
       QuotaExceededError and terminate the algorithm." */
    if (len > 65536) {
        JS_FreeValue(ctx, buf);
        return JS_ThrowDOMException(ctx, "QuotaExceededError",
                                    "§10.1.1 step 3: this method fills at most 65536 bytes and the view is %zu",
                                    len);
    }

    base = JS_GetArrayBuffer(ctx, &size, buf);
    /* THE WINDOW IS ASSERTED AGAINST THE BUFFER'S CURRENT SIZE, in front of a WRITE through `base + off`.
       IT IS A DCHECK AND IT USED TO BE A CHECK, and the reason it stopped being one is the whole of the fix
       it is left over from. It was written when Web IDL §3.2.26 Buffer source types' [AllowResizable] refusal
       was missing from core/idl_args.c, so a length-tracking view over a RESIZED buffer really did arrive here
       claiming a window it no longer had, and this line was the only thing between that and a write past the
       allocation — a live hazard, which is what a CHECK is for. Two things now stand ahead of it: the
       conversion refuses such a view outright, and JS_GetArrayBufferView answers a length-tracking view with
       ECMAScript §10.4.5.12 TypedArrayByteLength's derived length rather than the construction-time slot. What
       is left to assert is that those two hold — that the window one engine export gave and the size another
       gave are one fact — and an assertion about the engine's own logic is a DCHECK by definition. */
    DCHECK(off <= size && len <= size - off,
           "§10.1.1's view window is outside its own buffer: JS_GetArrayBufferView answered with a window "
           "JS_GetArrayBuffer's size does not contain, and §3.2.26's conversion had already refused every "
           "view whose buffer can change size under it");

    /* STEPS 4-6: "Let bytes be a byte sequence of length byteLength. Fill bytes with cryptographically secure
       random bytes. Write bytes into array."
       THE ZERO-LENGTH VIEW TAKES NO WORD FROM THE STREAM, and that is §10.1.1 read literally rather than an
       optimisation: step 4's byte sequence is empty, so there is nothing to fill and nothing to write, and a
       realm whose page called this on an empty view must be at the position it was at before. Guarded rather
       than left to the loop because `base` is legitimately NULL for a zero-length buffer that a component
       built through JS_NewArrayBuffer with no storage, and `base + off` would be arithmetic on it. */
    if (len > 0) {
        CHECK(base != NULL,
              "§10.1.1 reached a view with bytes to fill whose ArrayBuffer has no storage — "
              "JS_GetArrayBufferView had just accepted it as neither detached nor out of bounds");
        crypto_stream_take(ctx, this_val, s, base + off, len);
    }
    JS_FreeValue(ctx, buf);

    /* STEP 7: "Return array." THE SAME OBJECT, which is what `crypto.getRandomValues(b) === b` rests on and
       the only reason this member has a return value worth reading. */
    return JS_DupValue(ctx, argv[0]);
}

/* §10.1.2 The randomUUID method. */
static JSValue js_crypto_random_uuid(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv, int magic)
{
    static const char HEX[] = "0123456789abcdef";
    CryptoStream *s = JS_GetOpaque(this_val, g_crypto_class);
    uint8_t b[16];
    char out[36];
    size_t i, n = 0;

    (void)argc; (void)argv; (void)magic;
    if (s == NULL)
        return JS_ThrowTypeError(ctx, "randomUUID was reached on something that is not a Crypto");

    /* "To generate a random UUID" STEPS 1-2: "Let bytes be a byte sequence of length 16. Fill bytes with
       cryptographically secure random bytes." */
    crypto_stream_take(ctx, this_val, s, b, sizeof b);
    /* STEP 3: "Set the 4 most significant bits of bytes[6], which represent the UUID version, to 0100."
       STEP 4: "Set the 2 most significant bits of bytes[8], which represent the UUID variant, to 10." */
    b[6] = (uint8_t)((b[6] & 0x0F) | 0x40);
    b[8] = (uint8_t)((b[8] & 0x3F) | 0x80);

    /* STEP 5's concatenation — the four "-" the step writes between bytes 3|4, 5|6, 7|8 and 9|10 — under
       §10.1.2's own definition of "the hexadecimal representation of a byte": "the two-character string
       created by expressing value in hexadecimal using ASCII lower hex digits, left-padded with '0' to reach
       two ASCII lower hex digits". */
    for (i = 0; i < sizeof b; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10)
            out[n++] = '-';
        out[n++] = HEX[b[i] >> 4];
        out[n++] = HEX[b[i] & 0x0F];
    }
    DCHECK(n == sizeof out, "§10.1.2's string representation is 36 characters — 32 hex digits and 4 hyphens");
    /* THE TWO BIT-SETS ARE ASSERTED WHERE THEY BECOME OBSERVABLE, in the STRING rather than in the byte: a
       page's UUID-shaped regex reads these two positions and nothing else does, so an off-by-one in the
       layout above would be invisible to an assert on `b`. Byte 6 is characters 14-15 and byte 8 is 19-20. */
    DCHECK(out[14] == '4', "§10.1.2 step 3's version nibble is not 4 in the string this laid out");
    DCHECK(out[19] == '8' || out[19] == '9' || out[19] == 'a' || out[19] == 'b',
           "§10.1.2 step 4's variant bits are not 10 in the string this laid out");
    return JS_NewStringLen(ctx, out, sizeof out);
}

/* §10.2.1's `[SecureContext] readonly attribute SubtleCrypto subtle`: "The subtle attribute provides an
   instance of the SubtleCrypto interface which provides low-level cryptographic primitives and algorithms."
   THE OBJECT IS THE REALM'S, asked of the component that owns it. `crypto.subtle === crypto.subtle` holds
   because that component keeps one per realm, not because this getter caches — a cache here would be a second
   statement of an identity, and the two would agree until one of them was reset. */
static JSValue crypto_get_subtle(JSContext *ctx, JSValueConst this_val, int magic)
{
    (void)magic;
    /* WEB IDL §3.7.5's BRAND CHECK on an attribute getter. `Object.getOwnPropertyDescriptor(Crypto.prototype,
       'subtle').get.call({})` is a TypeError, and the attribute's type is not a promise, so it THROWS. */
    if (JS_GetClassID(this_val) != g_crypto_class)
        return JS_ThrowTypeError(ctx, "the `subtle` getter was reached on something that is not a Crypto");
    return subtle_crypto_object(ctx);
}

/* WindowOrWorkerGlobalScope's `[SameObject] readonly attribute Crypto crypto`. */
static JSValue crypto_get_crypto(JSContext *ctx, JSValueConst this_val, int magic)
{
    (void)this_val; (void)magic;
    return realm_value_get(ctx, g_obj_slot);
}

static void crypto_install_realm(JSContext *ctx)
{
    JSValue proto, prev, global, obj;
    CryptoStream *stream;

    prev = JS_GetClassProto(ctx, g_crypto_class);
    DCHECK(JS_IsNull(prev), "crypto_install_realm ran twice in one realm");
    JS_FreeValue(ctx, prev);
    proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(proto), "Crypto.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "Crypto");
    /* THE MEMBER IS `[SecureContext]`, THE INTERFACE IS NOT. Web IDL §3.3.13 removes the member in a
       non-secure realm rather than making it throw, so `crypto.subtle` is `undefined` over plain http —
       which is exactly what real Chrome answers there and what a bundle's `crypto && crypto.subtle` guard
       is testing for. */
    idl_install_accessor_exposed(ctx, proto, "subtle", crypto_get_subtle, 0, -1, IDL_SECURE_CONTEXT);
    /* `ArrayBufferView getRandomValues(ArrayBufferView array)` — §10's one member carrying no exposure
       condition at all, so it is there over plain http exactly as it is in a real browser. */
    idl_install_method(ctx, proto, "getRandomValues", 1, g_id_get_random_values);
    /* `[SecureContext] DOMString randomUUID()` — Web IDL §3.3.13 REMOVES the member in a non-secure realm
       rather than making it throw, so a bundle's `crypto.randomUUID ? crypto.randomUUID() : fallback()` takes
       the fallback there, which is the branch it is written to take. */
    idl_install_method_exposed(ctx, proto, "randomUUID", 0, g_id_random_uuid, IDL_SECURE_CONTEXT);
    JS_SetClassProto(ctx, g_crypto_class, JS_DupValue(ctx, proto));

    global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "Crypto", idl_interface_object(ctx, "Crypto", proto));

    obj = JS_NewObjectProtoClass(ctx, proto, g_crypto_class);
    JS_FreeValue(ctx, proto);
    CHECK(!JS_IsException(obj), "this realm's Crypto could not be allocated");
    /* THE DRAW POSITION IS BUILT WITH THE REALM, never on the first read: a lazily-built record would be
       created inside whichever FLOW happened to draw first, so it would be that flow's private object and
       every sibling would get a stream of its own — the same reason core/realm.h builds this whole object
       eagerly. It starts at zero in every realm, which crypto.h argues for as a schedule-independence trade
       rather than an oversight. */
    stream = calloc(1, sizeof *stream);
    CHECK(stream != NULL, "this realm's §10.1 draw position could not be allocated");
    JS_SetOpaque(obj, stream);
    realm_value_set(ctx, g_obj_slot, obj);

    /* THE WINDOW MEMBER. The mixin's partial puts it on the global, so it goes on THIS realm's global — a
       nested navigable's `crypto` is its own, which is what `[SameObject]` means per realm. */
    idl_install_accessor(ctx, global, "crypto", crypto_get_crypto, 0, -1);
    JS_FreeValue(ctx, global);
}

void crypto_init(JSContext *ctx)
{
    /* §10.1.1's `ArrayBufferView array` — Web IDL §4.1's typedef and NOT §4.2's BufferSource, so
       `crypto.getRandomValues(new ArrayBuffer(8))` is the conversion's TypeError while a float view reaches
       the algorithm and takes step 1's TypeMismatchError. core/idl_args.h's row states the difference. */
    static const IdlArgType GRV_ARGS[] = { IDL_ARRAYBUFFERVIEW };
    /* THE FINALIZER IS WHAT THE DRAW POSITION COSTS: the record is per realm and malloc'd, so the class that
       brands the object is also what gives it back. */
    JSClassDef d = { "Crypto", .finalizer = crypto_finalizer };

    DCHECK(g_obj_slot < 0, "crypto_init ran twice — the class and the slot are declared once per AGENT");
    /* §14's INTERFACE IS THIS COMPONENT'S DEPENDENCY and is declared here rather than by each host, for the
       reason core/realm.h gives: a host that installed Crypto and not SubtleCrypto would answer `subtle` with
       an object built in some other realm, or with nothing at all. core/realm.h runs the per-realm installs in
       DECLARATION order, so SubtleCrypto's prototype exists before this realm's `subtle` can answer with an
       instance of it. */
    subtle_crypto_init(ctx);
    JS_NewClassID(JS_GetRuntime(ctx), &g_crypto_class);
    CHECK(JS_NewClass(JS_GetRuntime(ctx), g_crypto_class, &d) == 0,
          "Crypto: the per-realm prototype slot could not be declared");
    g_obj_slot = realm_value_declare(ctx, "Web Cryptography §10 this realm's Crypto object");
    g_id_get_random_values = idl_method_id(ctx, GRV_ARGS, 1, js_crypto_get_random_values, 0);
    g_id_random_uuid = idl_method_id(ctx, NULL, 0, js_crypto_random_uuid, 0);
    agent_state_id("crypto", &g_obj_slot, "§10's per-realm Crypto slot, and the declaration latch");
    agent_state_id("crypto", &g_id_get_random_values, "Web Cryptography §10.1.1's getRandomValues");
    agent_state_id("crypto", &g_id_random_uuid, "Web Cryptography §10.1.2's randomUUID");
    /* THE CLASS IS DECLARED NOW THAT IT CARRIES A FINALIZER. It was held across every successor agent this
       process could have had while it branded nothing that owned memory, which is the arm core/agent_state.h
       calls a silence agreeing with a silence; a class whose finalizer frees a malloc'd record is state this
       component must give back, and the row that gives it back is this one. */
    agent_state_class("crypto", &g_crypto_class,
                      "Web Cryptography §10 Crypto's per-realm prototype slot and brand");
    realm_declare_intrinsic(crypto_install_realm);
}

void crypto_free(void)
{
    g_obj_slot = -1;
    g_id_get_random_values = -1;
    g_id_random_uuid = -1;
    g_crypto_class = 0;
    subtle_crypto_free();
}
