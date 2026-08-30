/* Web Cryptography API §14.3.5's digest method, §18.4.4's normalize an algorithm, and §32.3.1's Digest
 * operation — see subtle_crypto.h for why this is the one method of §14 that exists.
 *
 * §14.3.5 The digest method, verbatim, because every stage below is one of its steps:
 *   1. Let algorithm be the algorithm parameter passed to the digest() method.
 *   2. Let normalizedAlgorithm be the result of normalizing an algorithm, with alg set to algorithm and op set
 *      to "digest".
 *   3. If an error occurred, return a Promise rejected with normalizedAlgorithm.
 *   4. Let data be the result of getting a copy of the bytes held by the data parameter passed to the digest()
 *      method.
 *   5. Let realm be the relevant realm of this.
 *   6. Let promise be a new Promise.
 *   7. Return promise and perform the remaining steps in parallel.
 *   8. If the following steps or referenced procedures say to throw an error, queue a global task on the
 *      crypto task source, given realm's global object, to reject promise with the returned error; and then
 *      terminate the algorithm.
 *   9. Let digest be the result of performing the digest operation specified by normalizedAlgorithm using
 *      algorithm, with data as message.
 *  10. Queue a global task on the crypto task source, given realm's global object, to perform the remaining
 *      steps.
 *  11. Let result be the result of creating an ArrayBuffer in realm, containing digest.
 *  12. Resolve promise with result.
 *
 * STEP 4 IS AN OBSERVABLE AND IT IS THE REASON THIS COPIES. The bytes are copied BEFORE the promise is
 * returned, so a page that calls digest() and then writes into the same Uint8Array gets the digest of what it
 * passed and not of what it later wrote. Three lines of a bundle can tell the difference.
 *
 * THE PROMISE IS CREATED BEFORE STEP 6's NUMBER, AND THAT IS NOT A REORDERING. Steps 2-3 must REJECT, which
 * needs a promise; the standard writes step 3 as "return a Promise rejected with normalizedAlgorithm", i.e. a
 * promise created at that moment. Creating the capability once, at the top, gives the same object graph to
 * every exit and nothing can observe a promise the member has not returned yet. It is the same argument
 * core/permissions/permissions.c states for §6.2.1's step 7.
 *
 * WHY THE MESSAGE IS WALKED ONE BLOCK PER TURN. A message is of the PAGE'S size, so hashing one is not an O(1)
 * engine action and quickjs-step.h's rule is explicit about what that means: "a span over anything of the
 * PAGE'S SIZE … is a stage per step, and the stage that walks returns JS_STEP_YIELD at every turn, so the
 * scheduler is ASKED at each one". One turn is one FIPS 180-4 §6.x message block, which is the standard's own
 * unit of work and the smallest thing this can rest between.
 *
 * §18.4.4's REGISTRY LOOKUP IS A BRANCH ON THE PAGE'S VALUE, AND WHEN THAT VALUE IS UNKNOWN IT FORKS. "If
 * registeredAlgorithms contains a key that is a case-insensitive string match for algName … Otherwise: Return
 * a new NotSupportedError". Deciding that with a strcmp against a concolic's SHAPE would prune every arm but
 * one, silently, and the arm it kept would be the failing one — so the machine declares the fork instead and
 * every registered name is explored. Outcome 0 is SHA-256, because step_fork_run's rule is that outcome 0 is
 * the one a run with no forking policy takes and a candidate re-fire must not be diverted onto an error arm.
 *
 * AND A MESSAGE THAT IS UNKNOWN EXTERNAL INPUT PRODUCES AN UNKNOWN DIGEST, never a fabricated one. A concolic
 * crosses a BufferSource position as itself (core/idl_args.c's pass-through), and the digest of bytes nobody
 * has is a value nobody has: the promise resolves with the operation-named unknown concolic_builtin_hook
 * derives, carrying the REAL digest of the concolic's own example when it has one that is a BufferSource. A
 * placeholder digest would be a fabricated observation, and a bare opaque would drop the taint. */
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/agent_state.h"
#include "core/crypto/crypto_key.h"
#include "core/crypto/secure_hash.h"
#include "core/crypto/subtle_crypto.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "solver/concolic.h"

static JSClassID g_subtle_class;
static int       g_obj_slot = -1;
static int       g_id_digest = -1;
static JSAtom    g_atom_name = JS_ATOM_NULL;
/* THE RUNTIME THE ATOM BELONGS TO. An interned name is agent state and is freed against the runtime it was
   interned in; a release that cannot name one leaks a JSAtomStruct, which JS_FreeRuntime's atom walk reports
   by description and nothing else would have shown. */
static JSRuntime *g_rt;

/* WEB IDL §3.7.7 Operations' BRAND CHECK — "If jsValue does not implement the interface target, throw a
   TypeError", so `SubtleCrypto.prototype.digest.call({}, …)` is one. Because the operation's return type is a
   promise, §3.7.7 makes that a REJECTION rather than a throw, which core/idl_args performs for every member
   that declares one. This only has to state the TypeError.
   THE NUMBER USED TO READ §3.7.5, WHICH IS "CONSTANTS" — a real section with no brand check in it at all, so
   the citation resolved and said nothing the code claims. */
static bool sd_brand(JSContext *ctx, JSValueConst this_val)
{
    DCHECK(g_subtle_class != 0, "a SubtleCrypto member ran before subtle_crypto_init declared the class");
    if (JS_GetClassID(this_val) == g_subtle_class) return true;
    JS_ThrowTypeError(ctx, "a SubtleCrypto member was reached on something that is not a SubtleCrypto");
    return false;
}

/* §32.2's REGISTRY ROWS FOR THE "digest" OPERATION: "The recognized algorithm names are "SHA-1", "SHA-256",
   "SHA-384", and "SHA-512" for the respective SHA algorithms."
   THE ORDER IS THE FORK'S NUMBERING and not the standard's list order — see the fork note above. */
static const struct { const char *name; SecureHashAlgorithm alg; } SD_REGISTERED[] = {
    { "SHA-256", SECURE_HASH_SHA256 },
    { "SHA-384", SECURE_HASH_SHA384 },
    { "SHA-512", SECURE_HASH_SHA512 },
    { "SHA-1",   SECURE_HASH_SHA1   },
};
#define SD_REGISTERED_N ((int)(sizeof SD_REGISTERED / sizeof SD_REGISTERED[0]))
/* The one outcome past the registered rows: §18.4.4's "Otherwise: Return a new NotSupportedError". */
#define SD_FORK_OUTCOMES (SD_REGISTERED_N + 1)

/* step_fork_run keeps a BORROWED pointer to the operation string on the header, so it must outlive the ask. */
static const char SD_FORK_OP[] = "SubtleCrypto.digest/normalizeAlgorithm";

/* §18.4.4's "a key that is a case-insensitive string match for algName". ASCII case-insensitivity, which is
   what Web IDL and Infra mean by it everywhere; the registry's keys are ASCII by construction. */
static bool sd_name_matches(const char *a, const char *b)
{
    size_t i;

    for (i = 0; a[i] && b[i]; i++) {
        char x = a[i], y = b[i];

        if (x >= 'a' && x <= 'z') x = (char)(x - 'a' + 'A');
        if (y >= 'a' && y <= 'z') y = (char)(y - 'a' + 'A');
        if (x != y) return false;
    }
    return a[i] == b[i];
}

#define SD_STAGES(X)                                                                                          \
    X(SD_NAME, "Web Cryptography §18.4.4 normalizing an algorithm step 2 (Get(alg, \"name\") — the required "  \
              "member of the Algorithm dictionary the object arm is converted to)")                           \
    X(SD_NAME_STR, "Web Cryptography §18.4.4 normalizing an algorithm step 2 (converting alg[\"name\"] to "    \
                   "its DOMString)")                                                                          \
    X(SD_SELECT, "Web Cryptography §18.4.4 normalizing an algorithm step 5 (the case-insensitive lookup of "   \
                 "algName in the \"digest\" operation's registeredAlgorithms, and §14.3.5 step 3's rejection " \
                 "for a name no row registers)")                                                              \
    X(SD_COPY, "Web Cryptography §14.3.5 step 4 (get a copy of the bytes held by the data parameter)")         \
    X(SD_BLOCK, "FIPS 180-4 §6.1.2 / §6.2.2 / §6.4.2 step 1-4 for ONE message block M(i), which is §32.3.1's " \
                "digest operation performed one block at a time")                                             \
    X(SD_FINISH, "Web Cryptography §14.3.5 steps 10-12 (queue a global task on the crypto task source to "     \
                 "create an ArrayBuffer in realm containing digest and resolve promise with it)")
enum { IDL_STEP_STAGE_BASE(SD_STAGES) SD_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const SD_STEPS[] = { SD_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    JSValue    promise;    /* owned */
    JSValue    funcs[2];   /* the capability's [resolve, reject] (owned) */
    JSValue    name_v;     /* alg["name"] as read, then as its DOMString, or the concolic itself (owned) */
    JSValue    bytes;      /* §14.3.5 step 4's copy, as an ArrayBuffer (owned) */
    SecureHash hash;       /* POD, and that is load-bearing — it rides forks, parks and resumes as bytes */
    uint32_t   off;        /* how much of `bytes` §6.x has already consumed */
    uint8_t    alg;        /* the SecureHashAlgorithm §18.4.4 step 5 selected */
    uint8_t    unknown;    /* the message is unknown external input, so the digest is too */
    uint8_t    started;
} SdState;

static void sd_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    SdState *s = st;

    /* THE GUARD IS `started` AND NOT A STAGE, because a fork can land here before the prologue has run: a
       zeroed step state's JSValue is the INTEGER 0, which every visitor would take for a live value. */
    if (!s->started)
        return;
    v->val(ctx, &s->promise);
    v->val(ctx, &s->funcs[0]);
    v->val(ctx, &s->funcs[1]);
    v->val(ctx, &s->name_v);
    v->val(ctx, &s->bytes);
}

/* §14.3.5 step 3's REJECTION, and step 8's — the one operation both are: settle the promise this algorithm
   already created with the exception that is live, and hand the promise back. */
static int sd_reject(JSContext *ctx, SdState *s, JSValue *presult)
{
    JSValue exc = JS_GetException(ctx);

    DCHECK(s->started, "§14.3.5 rejected before it created its promise — every failure step of this algorithm "
                       "settles the promise step 6 creates, so the capability is built before the first thing "
                       "that can fail rather than at the step the standard numbers it");
    if (JS_CallAsFlow(ctx, s->funcs[1], exc) < 0)
        JS_FreeValue(ctx, JS_GetException(ctx));
    JS_FreeValue(ctx, exc);
    *presult = s->promise;
    s->promise = JS_UNDEFINED;
    return JS_STEP_DONE;
}

/* WEB IDL §3.2.26 Buffer source types' `get a copy of the bytes held by the buffer source`, whose two shapes
 * are the two arms of §4.2's BufferSource. Steps 1-4 read the WINDOW — the buffer itself for an ArrayBuffer,
 * and [[ViewedArrayBuffer]] with [[ByteOffset]] and [[ByteLength]] for a view — and step 5 is a POSITIVE
 * STATEMENT: "If IsDetachedBuffer(jsArrayBuffer) is true, then return the empty byte sequence."
 *
 * STEP 5 IS ASKED FIRST HERE, AND THAT IS NOT A REORDERING OF THE OBSERVABLE ALGORITHM — steps 1-4 are pure
 * reads of internal slots whose results step 5 discards, so the two orders answer identically. It is asked
 * first because THE WINDOW IS WHAT CANNOT BE READ FOR A DETACHED VIEW: an embedder's only route from a view to
 * its buffer is JS_GetArrayBufferView, which refuses an OUT-OF-BOUNDS view, and a detached buffer makes every
 * view over it out of bounds. So the algorithm met an exception at step 3 for exactly the input step 5 defines
 * the answer to.
 *
 * AND THE ARRAYBUFFER ARM IS WHY THAT WENT UNSEEN FOR SO LONG. This function used to lean on JS_GetBufferBytes
 * answering NULL with a zero length for a detached buffer, which is true and is the whole of the buffer arm —
 * so `digest(alg, detached)` hashed the empty message correctly while `digest(alg, viewOntoDetached)` aborted,
 * and the abort named the BRAND TEST ("neither an ArrayBuffer nor an ArrayBufferView"), which is a true
 * sentence about a value that had passed that very test. One algorithm, two arms, one of them answering a
 * different question: the defect shape that reads as a diagnosis. JS_IsDetachedBufferSource is the third
 * member of the pair quickjs-step.h already declares for §3.2.26's other two questions of the same buffer. */
static JSValue sd_copy_buffer_source(JSContext *ctx, JSValueConst src)
{
    JSValue view_buf = JS_UNDEFINED;
    JSValueConst ab = src;
    size_t off = 0, len = 0;
    uint32_t whole = 0;
    const uint8_t *base;
    JSValue copy;

    /* STEP 7, over §3.2.26's "underlying buffer of a buffer source type instance" — V itself for an
       ArrayBuffer, V.[[ViewedArrayBuffer]] for a view, which is the one question the predicate answers for
       both arms. The empty byte sequence IS the message, so the digest is SHA-256("") and not an error: a
       page that detaches its input and then hashes it gets the same answer a browser gives it. */
    if (JS_IsDetachedBufferSource(src))
        return JS_NewArrayBufferCopy(ctx, (const uint8_t *)"", 0);

    /* STEPS 1-6: the window. Everything past step 7 has a live, fixed-length, non-shared buffer under it —
       §3.2.26's conversion at the IDL_BUFFERSOURCE position refused the shared and resizable arms, and the
       detach is the line above — so JS_GetArrayBufferView's out-of-bounds refusal is now unreachable, which
       is what this DCHECK asserts rather than the brand test it used to name. */
    if (!JS_IsArrayBuffer(src)) {
        view_buf = JS_GetArrayBufferView(ctx, src, &off, &len);
        DCHECK(!JS_IsException(view_buf),
               "§3.2.26's step 5 refused a view whose buffer is neither detached, nor shared, nor resizable — "
               "the IDL_BUFFERSOURCE conversion performs the brand test and both refusals once and step 7 is "
               "asked above, so the only remaining cause of an out-of-bounds view has grown a fourth case");
        ab = view_buf;
    }
    base = JS_GetBufferBytes(ab, &whole);
    if (JS_IsArrayBuffer(src)) {
        off = 0;
        len = whole;
    }
    DCHECK(base != NULL || whole == 0,
           "§3.2.26 reached a live buffer with no storage — step 7 answered for the detached case above, so a "
           "NULL here is a buffer that is neither detached nor allocated");
    DCHECK(off + len <= (size_t)whole,
           "a BufferSource's byte range reaches past its own buffer — the offset and the length come from the "
           "view and the size from the buffer, and the three are one fact");
    /* STEPS 6-7: the copy. A zero-length buffer legitimately has NULL storage, and `base + off` would be
       arithmetic on it, so the empty case names its own bytes. */
    copy = JS_NewArrayBufferCopy(ctx, base ? base + off : (const uint8_t *)"", len);
    JS_FreeValue(ctx, view_buf);
    return copy;
}

/* §32.3.1's Digest over the concolic's own EXAMPLE, when it has one and it is a BufferSource. This is the
   "run the real op on the concrete" half of a concolic: the domain stays unknown and the example is a real
   digest of real bytes, never a value this file invented. */
static JSValue sd_example_digest(JSContext *ctx, JSValueConst unknown_data, SecureHashAlgorithm alg)
{
    JSValue ex = concolic_example(ctx, unknown_data);
    JSValue copy, out = JS_UNDEFINED;
    uint8_t digest[SECURE_HASH_MAX_DIGEST];
    SecureHash h;
    const uint8_t *p;
    uint32_t len = 0;

    if (!JS_IsArrayBuffer(ex) && JS_GetTypedArrayType(ex) < 0 && !JS_IsDataView(ex)) {
        JS_FreeValue(ctx, ex);
        return JS_UNDEFINED;   /* no example, or one that holds no bytes: example-free, which is honest */
    }
    copy = sd_copy_buffer_source(ctx, ex);
    JS_FreeValue(ctx, ex);
    if (JS_IsException(copy))
        return JS_UNDEFINED;
    p = JS_GetBufferBytes(copy, &len);
    secure_hash_init(&h, alg);
    secure_hash_update(&h, p ? p : (const uint8_t *)"", p ? len : 0);
    secure_hash_finish(&h, digest, sizeof digest);
    out = JS_NewArrayBufferCopy(ctx, digest, secure_hash_digest_size(alg));
    JS_FreeValue(ctx, copy);
    return out;
}

static int sd_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                   JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    SdState *s = st;
    JSValueConst alg  = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSValueConst data = argc > 1 ? argv[1] : JS_UNDEFINED;
    int r;

    *presult = JS_UNDEFINED;

    /* THE ONE-TIME PROLOGUE IS GUARDED BY `started` AND NOT BY THE STAGE. A request that SUSPENDS re-enters
       this function at the SAME stage, so everything a stage does before its request runs again — and here
       that would mint a second capability over the first and hand the page a promise nothing settles.
       EVERY SLOT IS SPELLED, because a zeroed step state's JSValue is the INTEGER 0 and not JS_UNDEFINED. */
    if (!s->started) {
        JSValue funcs[2];

        s->promise = s->funcs[0] = s->funcs[1] = s->name_v = s->bytes = JS_UNDEFINED;
        s->off = 0;
        s->alg = (uint8_t)SECURE_HASH_SHA256;
        s->unknown = 0;
        /* STEP 6's PROMISE, CREATED FIRST — see the file comment. */
        s->promise = JS_NewPromiseCapability(ctx, funcs);
        CHECK(!JS_IsException(s->promise),
              "§14.3.5's promise capability could not be allocated — a digest that answers with neither a "
              "promise nor a throw is a call a page can only hang on");
        s->funcs[0] = funcs[0];
        s->funcs[1] = funcs[1];
        s->started = 1;
        DCHECK(argc >= 2, "§14.3.5's digest ran with fewer than its two declared arguments — Web IDL §3.6 "
                          "step 5 refuses that in the prologue and §3.7.7 turns the refusal into a rejection, "
                          "so the body is only ever entered with both");
        if (!sd_brand(ctx, hdr->this_val)) {
            JS_FreeValue(ctx, cb_result);
            return sd_reject(ctx, s, presult);
        }
    }
    /* AN ABRUPT REQUEST RESULT ARRIVES AT THE HELPER THAT PARKED, AS ITS OWN -1 — and it is taken THERE, at
       SD_NAME's and SD_NAME_STR's `if (r < 0) return sd_reject(...)`, never by a test at the top of this
       function. A blanket `if (JS_IsException(cb_result)) return sd_reject(...)` stood here and is deleted.
       It was a workaround for a driver-side rewind (`step_hdr_request_abandon`) that no longer exists:
       quickjs-step.h's request contract now says an abrupt KEYED or COERCION completion arrives as the
       helper's own -1, having ENDED the request FIRST — `step_keyed_abrupt` rewinds the cursor and frees the
       atom before it tests, exactly as a normal completion ends them. Consuming the delivery ahead of the
       helper leaves the request recorded as IN FLIGHT, so `step_getprop_done`'s key check and
       `step_keyed_answered`'s stage check — the two asserts that exist to name a stage collecting another
       stage's answer — never ran on the one path they were written for, and this machine then walked out of
       SD_NAME with `get_phase` at GET_PH_GOT and `name`'s atom live on the header. It also made both `r < 0`
       branches below unreachable: two sites that read as the contract while a third quietly decided instead
       of them.
       THE DELIVERY IS NOT LOST. step_getprop_run reports -1 with the throw live in the context;
       step_tostring_run reaches JS_ToStringFree(ctx, JS_EXCEPTION), which quickjs answers with JS_EXCEPTION
       untouched, and returns -1 the same way — after resetting `str_phase` and releasing its held coercion.
       sd_reject picks up the identical exception at each site.
       AND NO OTHER STAGE CAN BE HANDED ONE, which is asserted rather than argued: SD_SELECT parks on
       step_fork_run, whose contract is JS_STEP_FORK or 0 and which runs none of the page's code, and SD_COPY,
       SD_BLOCK and SD_FINISH rest on JS_STEP_YIELD, which the driver re-enters with JS_UNDEFINED. Each of
       those stages FREES cb_result and carries on, so a throw arriving at one would ride live into step 12's
       resolve. */
    DCHECK(!JS_IsException(cb_result) || hdr->stage == SD_NAME || hdr->stage == SD_NAME_STR,
           "§14.3.5 was delivered an abrupt completion at a stage that parks on no request able to throw — "
           "only the `name` read and its ToString run the page's code, so this machine has grown a request "
           "kind it does not answer for, and the stage it arrived at frees the delivery and continues with "
           "the throw still live in the context");

    STEP_DISPATCH(SD_STAGES, hdr->stage, "Web Cryptography §14.3.5 digest(algorithm, data)", JS_STEP_ABRUPT);

    STEP_ARM(SD_NAME);
    /* §18.4.4: "If alg is an instance of a DOMString: Return the result of running the normalize an algorithm
       algorithm, with the alg set to a new Algorithm dictionary whose name attribute is alg" — so the string
       arm IS the name and reads nothing. The object arm reads `name`, which is one accessor or Proxy trap away
       from the page's own code and is therefore a request rather than a JS_GetPropertyStr. */
    if (JS_IsString(alg)) {
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        s->name_v = JS_DupValue(ctx, alg);
    } else if (concolic_is(alg)) {
        /* UNKNOWN EXTERNAL INPUT IS NOT AN OBJECT TO READ A MEMBER OFF — it stands for whatever the page was
           given, so the name it denotes is unknown and §18.4.4 step 5's lookup is the fork at SD_SELECT. */
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        s->name_v = JS_DupValue(ctx, alg);
    } else {
        r = step_getprop_run(ctx, hdr, alg, g_atom_name, cb_result, &s->name_v, out_cb, out_argc);
        if (r > 0) return r;
        if (r < 0) return sd_reject(ctx, s, presult);
        cb_result = JS_UNDEFINED;
    }
    STEP_GOTO(hdr->stage, SD_NAME_STR, &hdr->get_phase, &hdr->str_phase, NULL);

    STEP_ARM(SD_NAME_STR);
    /* WEB IDL §3.2.17: a REQUIRED dictionary member the object does not have is a TypeError, and `undefined`
       counts as not having it — asked before the ToString rather than after, where it would become the nine
       characters "undefined" and then an unsupported name. §3.7.7 makes the TypeError a rejection. */
    if (JS_IsUndefined(s->name_v)) {
        JS_FreeValue(ctx, cb_result);
        JS_ThrowTypeError(ctx, "the algorithm passed to SubtleCrypto.digest has no `name`, which the Algorithm "
                               "dictionary declares as a required member");
        return sd_reject(ctx, s, presult);
    }
    if (!JS_IsString(s->name_v) && !concolic_is(s->name_v)) {
        JSValue str;

        r = step_tostring_run(ctx, hdr, s->name_v, cb_result, &str, out_cb, out_argc);
        if (r > 0) return r;
        if (r < 0) return sd_reject(ctx, s, presult);
        JS_FreeValue(ctx, s->name_v);
        s->name_v = str;
    } else {
        JS_FreeValue(ctx, cb_result);
    }
    cb_result = JS_UNDEFINED;
    STEP_GOTO(hdr->stage, SD_SELECT, &hdr->get_phase, &hdr->str_phase, NULL);

    STEP_ARM(SD_SELECT);
    JS_FreeValue(ctx, cb_result);
    cb_result = JS_UNDEFINED;
    if (concolic_is(s->name_v)) {
        int arm = 0;

        /* §18.4.4 step 5 OVER A NAME NOBODY KNOWS. Every registered row is feasible and so is the
           NotSupportedError, so the machine declares the fork and the driver snapshots the flow for the arms
           it does not take. Deciding it here with a comparison would delete every arm but one. */
        r = step_fork_run(ctx, hdr, s->name_v, SD_FORK_OP, SD_FORK_OUTCOMES, JS_OUTCOME_REAL_UNSTATED, &arm);
        if (r > 0) return r;
        DCHECK(arm >= 0 && arm < SD_FORK_OUTCOMES,
               "§18.4.4's registry fork answered with an outcome it did not declare");
        if (arm == SD_REGISTERED_N) {
            JS_ThrowDOMException(ctx, "NotSupportedError", "%s",
                                 "the algorithm named is not a registered `digest` algorithm");
            return sd_reject(ctx, s, presult);
        }
        s->alg = (uint8_t)SD_REGISTERED[arm].alg;
    } else {
        const char *nm = JS_ToCString(ctx, s->name_v);
        int i;

        CHECK(nm != NULL, "§18.4.4's algName could not be read back as UTF-8 after its own ToString produced "
                          "it — the string exists and this cannot fail for a reason the algorithm has an "
                          "answer for");
        for (i = 0; i < SD_REGISTERED_N; i++)
            if (sd_name_matches(nm, SD_REGISTERED[i].name)) break;
        if (i == SD_REGISTERED_N) {
            /* §18.4.4: "Otherwise: Return a new NotSupportedError and terminate this algorithm", which
               §14.3.5 step 3 turns into a rejected promise. */
            JS_ThrowDOMException(ctx, "NotSupportedError", "'%s' is not a registered `digest` algorithm", nm);
            JS_FreeCString(ctx, nm);
            return sd_reject(ctx, s, presult);
        }
        JS_FreeCString(ctx, nm);
        s->alg = (uint8_t)SD_REGISTERED[i].alg;
    }
    STEP_GOTO(hdr->stage, SD_COPY, &hdr->get_phase, &hdr->str_phase, NULL);

    STEP_ARM(SD_COPY);
    JS_FreeValue(ctx, cb_result);
    cb_result = JS_UNDEFINED;
    if (concolic_is(data)) {
        /* THE MESSAGE IS UNKNOWN, SO THE DIGEST IS. There is nothing to copy and nothing to walk; SD_FINISH
           resolves with the operation-named unknown instead. */
        s->unknown = 1;
        STEP_GOTO(hdr->stage, SD_FINISH, &hdr->get_phase, &hdr->str_phase, NULL);
        return JS_STEP_YIELD;
    }
    /* STEP 4: "Let data be the result of getting a copy of the bytes held by the data parameter." */
    s->bytes = sd_copy_buffer_source(ctx, data);
    CHECK(!JS_IsException(s->bytes), "§14.3.5 step 4's copy of the message could not be allocated");
    secure_hash_init(&s->hash, (SecureHashAlgorithm)s->alg);
    s->off = 0;
    STEP_GOTO(hdr->stage, SD_BLOCK, &hdr->get_phase, &hdr->str_phase, NULL);

    STEP_ARM(SD_BLOCK);
    JS_FreeValue(ctx, cb_result);
    cb_result = JS_UNDEFINED;
    {
        uint32_t len = 0;
        const uint8_t *p = JS_GetBufferBytes(s->bytes, &len);
        size_t block = secure_hash_block_size((SecureHashAlgorithm)s->alg);
        size_t take;

        DCHECK(p != NULL || len == 0,
               "§14.3.5 step 4's own copy of the message is detached — nothing but this machine holds it, and "
               "the whole reason step 4 copies is that the page cannot reach these bytes");
        DCHECK(s->off <= len, "the message walk is past the end of its own copy");
        take = (size_t)len - s->off < block ? (size_t)len - s->off : block;
        if (take > 0) {
            secure_hash_update(&s->hash, p + s->off, take);
            s->off += (uint32_t)take;
            /* ONE BLOCK, THEN ASK. The scheduler answers from the frontier; when nobody is waiting the
               re-entry costs one predicted call, which is what makes this cheap enough to ask every block. */
            return JS_STEP_YIELD;
        }
        DCHECK(s->off == len, "the message walk stopped short of its own copy with a whole block still in it");
    }
    STEP_GOTO(hdr->stage, SD_FINISH, &hdr->get_phase, &hdr->str_phase, NULL);

    STEP_ARM(SD_FINISH);
    JS_FreeValue(ctx, cb_result);
    {
        uint8_t digest[SECURE_HASH_MAX_DIGEST];
        JSValue result;
        JSValueConst args[1];

        if (s->unknown) {
            result = concolic_builtin_hook(ctx, data, "digest",
                                           sd_example_digest(ctx, data, (SecureHashAlgorithm)s->alg));
            DCHECK(!JS_IsUninitialized(result),
                   "the message was recorded as unknown external input and the derivation declined it — the "
                   "two are one fact read at two stages, and they have come apart");
        } else {
            /* STEP 9's digest, finished, and STEP 11's "creating an ArrayBuffer in realm, containing digest".
               The realm is THIS one: a C member runs in the realm that defined it, which is the realm whose
               prototype carries this member, which is §14.3.5 step 5's relevant realm of `this`. */
            secure_hash_finish(&s->hash, digest, sizeof digest);
            result = JS_NewArrayBufferCopy(ctx, digest,
                                           secure_hash_digest_size((SecureHashAlgorithm)s->alg));
            CHECK(!JS_IsException(result), "§14.3.5 step 11's ArrayBuffer could not be allocated");
        }
        /* STEP 10 and STEP 12: "Queue a global task on the crypto task source … to resolve promise with
           result." A JOB, not a call — §14.2's whole purpose is that the settle happens in a later task, so a
           `Promise.resolve().then(…)` written after the digest call runs FIRST, exactly as it does in a real
           browser. */
        args[0] = result;
        JS_EnqueueCallJob(ctx, s->funcs[0], 1, args);
        JS_FreeValue(ctx, result);
    }
    *presult = s->promise;
    s->promise = JS_UNDEFINED;
    return JS_STEP_DONE;
}

static const IdlStepDecl SD_DECL = {
    sd_step, sizeof(SdState), sd_visit, NULL,
    "Web Cryptography §14.3.5 digest(algorithm, data)", SD_STEPS,
    /* catches_abrupt: §14.3.5 step 3 REJECTS for every error normalizing an algorithm produced, and a `name`
       getter that throws after suspending is one of them. Without it the throw would tear this machine down
       and propagate past the `.catch` the page wrote. */
    1
};

/* ---- the per-realm install ------------------------------------------------------------------------------ */

JSValue subtle_crypto_object(JSContext *ctx)
{
    return realm_value_get(ctx, g_obj_slot);
}

static void subtle_crypto_install_realm(JSContext *ctx)
{
    JSValue proto, prev, global, obj;

    prev = JS_GetClassProto(ctx, g_subtle_class);
    DCHECK(JS_IsNull(prev), "subtle_crypto_install_realm ran twice in one realm");
    JS_FreeValue(ctx, prev);
    proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(proto), "SubtleCrypto.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "SubtleCrypto");
    /* §14's interface is `[SecureContext]` as a whole, and Web IDL §3.3.13 REMOVES a member in a non-secure
       realm rather than making it throw — `'digest' in crypto.subtle` is what a bundle feature-detects with,
       and absent, throwing and undefined are three different branches. */
    idl_install_method_exposed(ctx, proto, "digest", g_id_digest, IDL_SECURE_CONTEXT);
    JS_SetClassProto(ctx, g_subtle_class, JS_DupValue(ctx, proto));

    global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "SubtleCrypto", idl_interface_object(ctx, "SubtleCrypto", proto));
    JS_FreeValue(ctx, global);

    obj = JS_NewObjectProtoClass(ctx, proto, g_subtle_class);
    JS_FreeValue(ctx, proto);
    CHECK(!JS_IsException(obj), "this realm's SubtleCrypto could not be allocated");
    realm_value_set(ctx, g_obj_slot, obj);
}

void subtle_crypto_init(JSContext *ctx)
{
    JSClassDef d = { "SubtleCrypto" };
    static const IdlArgType SD_ARGS[] = { IDL_STRING_UNLESS_OBJECT, IDL_BUFFERSOURCE };

    DCHECK(g_obj_slot < 0, "subtle_crypto_init ran twice — the class, the slot and the member's pool id are "
                           "the AGENT's");
    /* §13's INTERFACE IS THIS COMPONENT'S DEPENDENCY and is declared here, for the reason core/crypto/crypto.c
       gives about this one: every absent method of §14.3 takes a CryptoKey or mints one, so the component that
       will call the mint is the component that declares it. core/realm.h runs the per-realm installs in
       DECLARATION order, so CryptoKey.prototype exists before anything of §14's can hand a key back. */
    crypto_key_init(ctx);
    g_rt = JS_GetRuntime(ctx);
    JS_NewClassID(JS_GetRuntime(ctx), &g_subtle_class);
    CHECK(JS_NewClass(JS_GetRuntime(ctx), g_subtle_class, &d) == 0,
          "SubtleCrypto: the per-realm prototype slot could not be declared");
    g_obj_slot = realm_value_declare(ctx, "Web Cryptography §10.2.1 this realm's SubtleCrypto");
    g_atom_name = JS_NewAtom(ctx, "name");
    CHECK(g_atom_name != JS_ATOM_NULL, "the Algorithm dictionary's `name` could not be interned");
    g_id_digest = idl_method_id_step(ctx, SD_ARGS, 2, NULL, 0, &SD_DECL, 0);
    /* §14's `Promise<ArrayBuffer> digest(...)`: Web IDL §3.7.7 makes EVERY throw of this member — the brand
       check, the arity, both argument conversions and the algorithm itself — a rejected promise. */
    idl_returns_promise();
    /* DECLARED UNDER THE ROW THAT RELEASES IT, which is `crypto` — §10's component declares this one and its
       release reaches this one's, so core/platform.c's two-sided check ("a row with a release that declared no
       agent state cannot be asserted to have undone anything") is asking about the pair. Naming a component
       with no row of its own would leave these three slots on the registry with nothing on the release column
       to be the inverse of. */
    agent_state_id("crypto", &g_obj_slot, "§10.2.1's per-realm SubtleCrypto slot, and the declaration latch");
    agent_state_id("crypto", &g_id_digest, "§14.3.5's digest machine");
    agent_state_atom("crypto", &g_atom_name, "the Algorithm dictionary's `name` member name");
    agent_state_ptr("crypto", &g_rt, "the runtime that `name` was interned in");
    realm_declare_intrinsic(subtle_crypto_install_realm);
}

void subtle_crypto_free(void)
{
    crypto_key_free();
    if (g_obj_slot < 0)
        return;
    DCHECK(g_rt != NULL, "SubtleCrypto was declared without recording the runtime its atom belongs to");
    JS_FreeAtomRT(g_rt, g_atom_name);
    g_atom_name = JS_ATOM_NULL;
    g_obj_slot = -1;
    g_id_digest = -1;
    g_rt = NULL;
}
