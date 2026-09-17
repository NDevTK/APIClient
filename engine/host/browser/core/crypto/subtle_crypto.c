/* Web Cryptography API §14.3's methods and §18.4.4's normalize an algorithm — the FOUR members of §14 this
 * engine performs, each as a step machine, plus the promise capability all four are wrapped in. §14.3.5's
 * digest reaches §32.3.1's Digest; §14.3.3's sign and §14.3.4's verify reach §31's HMAC operations, whose
 * algorithm is core/crypto/hmac.c; §14.3.9's importKey reaches EITHER §31.6.4 or §29.4.4, whose algorithm is
 * core/crypto/aes_gcm_key.c, because the operation it performs is the one §18.4.4 step 5's registry lookup
 * selected. See subtle_crypto.h for why those four members and no others.
 *
 * THE FILE IS ORDERED digest, then sign/verify, then importKey, and each machine's own banner states what it
 * is. What they SHARE is stated once, at the top: the promise capability, §32.2's registry order, and §18.4.4's
 * case-insensitive name match.
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
#include <math.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/agent_state.h"
#include "core/crypto/aes_gcm.h"
#include "core/crypto/aes_gcm_key.h"
#include "core/crypto/crypto_key.h"
#include "core/crypto/hmac.h"
#include "core/crypto/secure_hash.h"
#include "core/crypto/subtle_crypto.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "solver/concolic.h"

static JSClassID g_subtle_class;
static int       g_obj_slot = -1;
static int       g_id_digest = -1;
static int       g_id_sign = -1;
static int       g_id_verify = -1;
static int       g_id_import_key = -1;
static int       g_id_export_key = -1;
static int       g_id_generate_key = -1;
static int       g_id_encrypt = -1;
static int       g_id_decrypt = -1;
static JSAtom    g_atom_name = JS_ATOM_NULL;
static JSAtom    g_atom_hash = JS_ATOM_NULL;
static JSAtom    g_atom_length = JS_ATOM_NULL;
static JSAtom    g_atom_iv = JS_ATOM_NULL;
static JSAtom    g_atom_additional_data = JS_ATOM_NULL;
static JSAtom    g_atom_tag_length = JS_ATOM_NULL;
/* THE RUNTIME THE ATOM BELONGS TO. An interned name is agent state and is freed against the runtime it was
   interned in; a release that cannot name one leaks a JSAtomStruct, which JS_FreeRuntime's atom walk reports
   by description and nothing else would have shown. */
static JSRuntime *g_rt;

/* WEB IDL §3.7 Interfaces' implementation-check an object, step 3's `interface` — "If object does not
 * implement interface, then throw a TypeError" — as the PREDICATE core/idl_args' idl_this_iface takes. §14's
 * SubtleCrypto is exactly one class and nothing inherits from it, so implementing it IS carrying that class
 * id; the component that owns the interface is the only thing that can answer that, which is why the
 * declaration names a predicate rather than restating the test.
 *
 * IT IS A DECLARATION AND NO LONGER A CALL IN EACH BODY, AND THAT IS AN ORDER RATHER THAN A TIDY-UP. §3.7.7
 * Operations' create an operation function asks the receiver in STEP 2's TRY-LIST at its step 2.1.2.3 — "If
 * jsValue does not implement the interface target, throw a TypeError" — and only reaches "compute the
 * effective overload set" at 2.1.4 and §3.6 Overload resolution algorithm at 2.1.5, so EVERY argument
 * conversion runs after it. A body runs after all of them, so the test written there let
 * `SubtleCrypto.prototype.importKey.call({}, {toString(){ window.ran = true; return "raw"; }}, …)` run the
 * page's `toString` and refuse afterwards, where a browser refuses with `window.ran` still undefined. It also
 * put §3.6 step 5's ARITY refusal ahead of the receiver's, which is the same difference one step earlier.
 *
 * STEP 2 HOLDS TWO SIBLING LISTS — the try-list those sub-numbers belong to, and the list under "And then, if
 * an exception E was thrown" that restarts at 1 — so a bare `2.1` names two different steps and the list is
 * named here rather than left to the sub-number alone.
 *
 * BOTH REFUSALS ARE A REJECTION AND NOT A THROW, which is why the order is observable through a `.catch` and
 * not only through a `try`: the four members declare idl_returns_promise, and step 2's exception-list returns
 * `! Call(%Promise.reject%, %Promise%, «E»)` for a promise-typed operation.
 *
 * THE NUMBER USED TO READ §3.7.5, WHICH IS "CONSTANTS" — a real section with no brand check in it at all, so
 * the citation resolved and said nothing the code claims. */
static bool subtle_crypto_is(JSValueConst v)
{
    DCHECK(g_subtle_class != 0, "a SubtleCrypto member ran before subtle_crypto_init declared the class");
    return JS_GetClassID(v) == g_subtle_class;
}

/* §32.2's REGISTRY ROWS FOR THE "digest" OPERATION: "The recognized algorithm names are "SHA-1", "SHA-256",
   "SHA-384", and "SHA-512" for the respective SHA algorithms."
   THE ORDER IS THE FORK'S NUMBERING and not the standard's list order — see the fork note above. THE NAMES ARE
   NOT HERE: secure_hash_name is the one statement of what each of the four is called, declared beside the enum
   whose membership that same sentence of §32.2 decides, so this table says only what ORDER the fork numbers
   them in. A second column of strings here is the copy that drifts. */
static const SecureHashAlgorithm SD_REGISTERED[] = {
    SECURE_HASH_SHA256, SECURE_HASH_SHA384, SECURE_HASH_SHA512, SECURE_HASH_SHA1
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

/* ---- what every §14.3 method is wrapped in ----------------------------------------------------------------- */

/* THE PROMISE CAPABILITY, ONCE, BECAUSE FOUR METHODS OWN THE SAME THREE FIELDS AND THE SAME TWO EXITS. Every
 * operation of §14.3 is written to the same frame: "Let promise be a new Promise", "Return promise and perform
 * the remaining steps in parallel", and an error step that says to reject it. Written out per machine that is
 * three JSValues, one `visit`, one reject and one resolve copied four times — the second copy of a fact, which
 * is the one that drifts. It is a struct rather than a convention so that a state which forgets it does not
 * compile.
 *
 * `started` GUARDS THE PROLOGUE AND IT IS A FLAG RATHER THAN A STAGE, for the reason each machine's own comment
 * gives: a request that SUSPENDS re-enters at the SAME stage, so a prologue keyed on the stage would mint a
 * second capability over the first and hand the page a promise nothing settles. It is also what `visit` tests,
 * because a fork can land on a state the prologue has not run over yet and a zeroed JSValue is the INTEGER 0,
 * which every visitor would take for a live value. */
typedef struct {
    JSValue promise;    /* owned */
    JSValue funcs[2];   /* the capability's [resolve, reject] (owned) */
    uint8_t started;
} ScPromise;

static void sc_promise_visit(JSContext *ctx, ScPromise *p, JSStepVisit *v)
{
    if (!p->started)
        return;
    v->val(ctx, &p->promise);
    v->val(ctx, &p->funcs[0]);
    v->val(ctx, &p->funcs[1]);
}

/* "Let promise be a new Promise" — created at the TOP of the machine rather than at the step the standard
   numbers it. Steps 2-3 of every one of these methods must REJECT, which needs a promise, and the standard
   writes that as "return a Promise rejected with normalizedAlgorithm", i.e. a promise created at that moment.
   Building the capability once gives the same object graph to every exit, and nothing can observe a promise the
   member has not returned yet. core/permissions/permissions.c states the same argument for §6.2.1's step 7. */
static void sc_promise_begin(JSContext *ctx, ScPromise *p)
{
    JSValue funcs[2];

    DCHECK(!p->started, "a §14.3 method minted a second promise capability over its first — the prologue is "
                        "guarded by `started` and not by the stage, because a suspended request re-enters at "
                        "the stage it parked on");
    p->promise = p->funcs[0] = p->funcs[1] = JS_UNDEFINED;
    p->promise = JS_NewPromiseCapability(ctx, funcs);
    CHECK(!JS_IsException(p->promise),
          "a §14.3 method's promise capability could not be allocated — a member that answers with neither a "
          "promise nor a throw is a call a page can only hang on");
    p->funcs[0] = funcs[0];
    p->funcs[1] = funcs[1];
    p->started = 1;
}

/* THE REJECT EXIT — step 3's "return a Promise rejected with normalizedAlgorithm" and the error step's "queue a
   global task on the crypto task source … to reject promise with the returned error", which are one operation:
   settle the promise this method already created with the exception that is live, and hand the promise back. */
static int sc_reject(JSContext *ctx, ScPromise *p, JSValue *presult)
{
    JSValue exc = JS_GetException(ctx);

    DCHECK(p->started, "a §14.3 method rejected before it created its promise — every failure step of these "
                       "algorithms settles the promise the method creates, so the capability is built before "
                       "the first thing that can fail rather than at the step the standard numbers it");
    if (JS_CallAsFlow(ctx, p->funcs[1], exc) < 0)
        JS_FreeValue(ctx, JS_GetException(ctx));
    JS_FreeValue(ctx, exc);
    *presult = p->promise;
    p->promise = JS_UNDEFINED;
    return JS_STEP_DONE;
}

/* THE RESOLVE EXIT — "Queue a global task on the crypto task source, given realm's global object, to perform
   the remaining steps" followed by "Resolve promise with result". A JOB, not a call: §14.2's whole purpose is
   that the settle happens in a LATER TASK, so a `Promise.resolve().then(…)` written after the call runs FIRST,
   exactly as it does in a real browser. `result` is CONSUMED. */
static int sc_resolve(JSContext *ctx, ScPromise *p, JSValue result, JSValue *presult)
{
    JSValueConst args[1];

    DCHECK(p->started, "a §14.3 method resolved before it created its promise");
    args[0] = result;
    JS_EnqueueCallJob(ctx, p->funcs[0], 1, args);
    JS_FreeValue(ctx, result);
    *presult = p->promise;
    p->promise = JS_UNDEFINED;
    return JS_STEP_DONE;
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
    ScPromise  p;          /* step 6's promise and its capability — see sc_promise_begin */
    JSValue    name_v;     /* alg["name"] as read, then as its DOMString, or the concolic itself (owned) */
    JSValue    bytes;      /* §14.3.5 step 4's copy, as an ArrayBuffer (owned) */
    SecureHash hash;       /* POD, and that is load-bearing — it rides forks, parks and resumes as bytes */
    uint32_t   off;        /* how much of `bytes` §6.x has already consumed */
    uint8_t    alg;        /* the SecureHashAlgorithm §18.4.4 step 5 selected */
    uint8_t    unknown;    /* the message is unknown external input, so the digest is too */
} SdState;

static void sd_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    SdState *s = st;

    /* THE GUARD IS `started` AND NOT A STAGE, because a fork can land here before the prologue has run: a
       zeroed step state's JSValue is the INTEGER 0, which every visitor would take for a live value. */
    sc_promise_visit(ctx, &s->p, v);
    if (!s->p.started)
        return;
    v->val(ctx, &s->name_v);
    v->val(ctx, &s->bytes);
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
static JSValue sc_copy_buffer_source(JSContext *ctx, JSValueConst src)
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
    copy = sc_copy_buffer_source(ctx, ex);
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
    if (!s->p.started) {
        s->name_v = s->bytes = JS_UNDEFINED;
        s->off = 0;
        s->alg = (uint8_t)SECURE_HASH_SHA256;
        s->unknown = 0;
        /* STEP 6's PROMISE, CREATED FIRST — see the file comment. */
        sc_promise_begin(ctx, &s->p);
        DCHECK(argc >= 2, "§14.3.5's digest ran with fewer than its two declared arguments — Web IDL §3.6 "
                          "step 5 refuses that in the prologue and §3.7.7 turns the refusal into a rejection, "
                          "so the body is only ever entered with both");
    }
    /* AN ABRUPT REQUEST RESULT ARRIVES AT THE HELPER THAT PARKED, AS ITS OWN -1 — and it is taken THERE, at
       SD_NAME's and SD_NAME_STR's `if (r < 0) return sc_reject(...)`, never by a test at the top of this
       function. A blanket `if (JS_IsException(cb_result)) return sc_reject(...)` stood here and is deleted.
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
        if (r < 0) return sc_reject(ctx, &s->p, presult);
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
        return sc_reject(ctx, &s->p, presult);
    }
    if (!JS_IsString(s->name_v) && !concolic_is(s->name_v)) {
        JSValue str;

        r = step_tostring_run(ctx, hdr, s->name_v, cb_result, &str, out_cb, out_argc);
        if (r > 0) return r;
        if (r < 0) return sc_reject(ctx, &s->p, presult);
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
            return sc_reject(ctx, &s->p, presult);
        }
        s->alg = (uint8_t)SD_REGISTERED[arm];
    } else {
        const char *nm = JS_ToCString(ctx, s->name_v);
        int i;

        CHECK(nm != NULL, "§18.4.4's algName could not be read back as UTF-8 after its own ToString produced "
                          "it — the string exists and this cannot fail for a reason the algorithm has an "
                          "answer for");
        for (i = 0; i < SD_REGISTERED_N; i++)
            if (sd_name_matches(nm, secure_hash_name(SD_REGISTERED[i]))) break;
        if (i == SD_REGISTERED_N) {
            /* §18.4.4: "Otherwise: Return a new NotSupportedError and terminate this algorithm", which
               §14.3.5 step 3 turns into a rejected promise. */
            JS_ThrowDOMException(ctx, "NotSupportedError", "'%s' is not a registered `digest` algorithm", nm);
            JS_FreeCString(ctx, nm);
            return sc_reject(ctx, &s->p, presult);
        }
        JS_FreeCString(ctx, nm);
        s->alg = (uint8_t)SD_REGISTERED[i];
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
    s->bytes = sc_copy_buffer_source(ctx, data);
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
           result." A JOB, not a call — see sc_resolve, which is that pair for every method of §14.3. */
        return sc_resolve(ctx, &s->p, result, presult);
    }
}

static const IdlStepDecl SD_DECL = {
    sd_step, sizeof(SdState), sd_visit, NULL,
    "Web Cryptography §14.3.5 digest(algorithm, data)", SD_STEPS,
    /* catches_abrupt: §14.3.5 step 3 REJECTS for every error normalizing an algorithm produced, and a `name`
       getter that throws after suspending is one of them. Without it the throw would tear this machine down
       and propagate past the `.catch` the page wrote. */
    1
};

/* ---- §14.3.3 The sign method and §14.3.4 The verify method ------------------------------------------------- */

/* ONE MACHINE FOR BOTH, WITH A MAGIC, BECAUSE THE TWO ALGORITHMS ARE ONE ALGORITHM WITH ONE EXTRA READ. Compare
 * §14.3.3 and §14.3.4 side by side: fourteen steps each, identical from the normalization through step 8, then
 * §14.3.3 step 9 and §14.3.4 step 10 are the same sentence ("If the name member of normalizedAlgorithm is not
 * equal to the name attribute of the [[algorithm]] internal slot of key then throw an InvalidAccessError"),
 * step 10 and step 11 are the same sentence over a different usage, and the tails differ only in that verify
 * takes a `signature` copy at its step 4 and resolves with a boolean rather than an ArrayBuffer. The same
 * shape is true one level down: §31.6.1 Sign and §31.6.2 Verify state the SAME first step, word for word, and
 * differ only in what they do with the mac. Two machines would be one algorithm written twice, which is the
 * copy this codebase keeps paying for; idl_args.h's magic is exactly the mechanism for it (innerHTML and
 * outerHTML are its worked example).
 *
 * WHAT IS NOT SHARED IS THE ARGUMENT LIST, and that is a DECLARATION rather than a body: §14.3.3 takes
 * (algorithm, key, data) and §14.3.4 takes (algorithm, key, signature, data), so the two members declare two
 * type lists over one step definition and the magic says which is which.
 *
 * BOTH WALKS PARK, AND THERE ARE THREE OF THEM RATHER THAN ONE. The message is obviously of the page's size.
 * So is the KEY — FIPS 198-1 Table 1 step 2 hashes it whenever it is longer than a block, and
 * `importKey("raw", new Uint8Array(1<<24), …)` is a legal call — which is why SV_KEY is a stage of its own that
 * yields per block rather than a line inside SV_CHECK. hmac.h states the invariant and hmac_key_update asserts
 * it, so a caller that fed either walk in one go aborts at the site instead of freezing the frontier.
 *
 * AND AN UNKNOWN MESSAGE PRODUCES AN UNKNOWN MAC, never a fabricated one — the same rule §14.3.5's digest
 * follows, for the same reason. A concolic crosses a BufferSource position as itself; the MAC of bytes nobody
 * has is a value nobody has, so the promise resolves with the operation-named unknown concolic_builtin_hook
 * derives. Its EXAMPLE is the REAL MAC of the concolic's own example when it has one that is a BufferSource,
 * and it is computed by walking that example through THESE SAME STAGES rather than by a one-shot beside them:
 * an example is of the page's size too, so a second un-parkable spelling of this walk would be the thing
 * hmac.h refuses to offer. For §14.3.4 the unknown is a BOOLEAN, so `if (await verify(…))` forks. */

enum { SC_M_SIGN = 0, SC_M_VERIFY };

/* step_fork_run keeps a BORROWED pointer to the operation string, so it must outlive the ask. Two strings and
   not one composed at the ask: a fork's operation is its cross-session NAME, and a name assembled at run time
   is one a replay cannot match. */
static const char SV_FORK_OP_SIGN[]   = "SubtleCrypto.sign/normalizeAlgorithm";
static const char SV_FORK_OP_VERIFY[] = "SubtleCrypto.verify/normalizeAlgorithm";

/* §31.2 Registration: "The recognized algorithm name for this algorithm is \"HMAC\"", and its rows include
   `sign` and `verify`. So the associative container §18.4.4 step 1 stores at the "sign" key of
   supportedAlgorithms has, in THIS engine, exactly one entry — §20.9.1, §21.4.1, §23.7.1 and §25.3.1 are the
   other four algorithms' rows and none of them is built. §18.5.1 is explicit that this is conformant: "there
   are no algorithms that conforming user agents are required to implement". */
#define SV_REGISTERED_N 1
#define SV_FORK_OUTCOMES (SV_REGISTERED_N + 1)

#define SV_STAGES(X)                                                                                          \
    X(SV_NAME, "Web Cryptography §18.4.4 normalizing an algorithm step 2 (Get(alg, \"name\") for the sign or "  \
               "verify operation)")                                                                            \
    X(SV_NAME_STR, "Web Cryptography §18.4.4 normalizing an algorithm step 2 (converting alg[\"name\"] to its " \
                   "DOMString)")                                                                               \
    X(SV_SELECT, "Web Cryptography §18.4.4 normalizing an algorithm step 5 (the case-insensitive lookup of "    \
                 "algName in the sign or verify operation's registeredAlgorithms)")                            \
    X(SV_CHECK, "Web Cryptography §14.3.3 steps 4 and 9-10 / §14.3.4 steps 4-5 and 10-11 (the copies of the "   \
                "message and the signature, and the two InvalidAccessError refusals over the key)")            \
    X(SV_KEY, "FIPS 198-1 §4 Table 1 steps 1-3 for ONE block of K, which is §31.6.1's MAC Generation reading "  \
              "the key represented by the [[handle]] internal slot one block at a time")                       \
    X(SV_TEXT, "FIPS 198-1 §4 Table 1 step 5 for ONE block of `text`, which is §31.6.1's MAC Generation "       \
               "appending the message one block at a time")                                                    \
    X(SV_FINISH, "Web Cryptography §14.3.3 steps 12-14 / §14.3.4 steps 13-14 (queue a global task on the "      \
                 "crypto task source and resolve promise)")
enum { IDL_STEP_STAGE_BASE(SV_STAGES) SV_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const SV_STEPS[] = { SV_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    ScPromise p;
    JSValue   name_v;      /* alg["name"] as read, then as its DOMString, or the concolic itself (owned) */
    JSValue   handle;      /* §13.3's [[handle]], the key's own bytes (owned) */
    JSValue   bytes;       /* the message copy, or the concolic's example's (owned) */
    JSValue   sig;         /* §14.3.4 step 4's signature copy; JS_UNDEFINED for sign (owned) */
    Hmac      mac;         /* POD, and that is load-bearing — it rides forks, parks and resumes as bytes */
    uint64_t  key_off;     /* how much of [[handle]] Table 1 steps 1-3 have consumed */
    uint32_t  off;         /* how much of `bytes` Table 1 step 5 has consumed */
    uint8_t   alg;         /* the SecureHashAlgorithm §31.6.1 step 1's "hash attribute" identified */
    uint8_t   unknown;     /* the message (or, for verify, the signature) is unknown external input */
    uint8_t   unknown_arg; /* WHICH argument that was — the concolic the result is derived from */
    uint8_t   has_example; /* EVERY unknown operand supplied a BufferSource example, so the result below is a
                              real observation of them and can be carried as the derived unknown's example */
} SvState;

/* THE BYTES AN OPERAND CONTRIBUTES TO THE WALK. A known BufferSource contributes its own copy; an unknown one
   contributes its EXAMPLE's copy when it has a BufferSource example, and the empty sequence otherwise.
   THE NAME IS `sc_` AND NOT `sv_` BECAUSE THREE MACHINES ASK IT AND NOT ONE. It was written for sign's single
   operand and verify's two; §14.3.1 and §14.3.2 ask it of `data` and of §29.3 AesGcmParams' two BufferSource
   members, which is the same three-way brand §14.3.5's digest asks of its own. A helper named for one caller
   is the shape that grows a second copy the day a second caller cannot see itself in the name. */
static JSValue sc_operand_bytes(JSContext *ctx, JSValueConst v, bool *unknown, bool *has_example)
{
    JSValue ex, copy;

    if (!concolic_is(v))
        return sc_copy_buffer_source(ctx, v);
    *unknown = true;
    ex = concolic_example(ctx, v);
    if (!JS_IsArrayBuffer(ex) && JS_GetTypedArrayType(ex) < 0 && !JS_IsDataView(ex)) {
        /* No example, or one that holds no bytes: the walk runs over the empty sequence and the result is
           reported EXAMPLE-FREE, which is honest. A placeholder would be a fabricated observation. */
        JS_FreeValue(ctx, ex);
        *has_example = false;
        return JS_NewArrayBufferCopy(ctx, (const uint8_t *)"", 0);
    }
    copy = sc_copy_buffer_source(ctx, ex);
    JS_FreeValue(ctx, ex);
    return copy;
}

static void sv_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    SvState *s = st;

    sc_promise_visit(ctx, &s->p, v);
    if (!s->p.started)
        return;
    v->val(ctx, &s->name_v);
    v->val(ctx, &s->handle);
    v->val(ctx, &s->bytes);
    v->val(ctx, &s->sig);
}

/* §14.3.3 step 9 / §14.3.4 step 10: "If the name member of normalizedAlgorithm is not equal to the name
   attribute of the [[algorithm]] internal slot of key then throw an InvalidAccessError."
   THE COMPARISON IS EXACT AND NOT §18.4.4 step 5's CASE-INSENSITIVE ONE, because both operands are names THIS
   ENGINE wrote: step 5's own sub-step 1 says "Set algName to the value of the MATCHING KEY", so by step 9 the
   normalized name is the registry's spelling, and the key's is what §31.6.4 step 12 stored. A case-insensitive
   comparison here would be answering a question neither operand can ask. */
static bool sv_key_algorithm_is(JSContext *ctx, JSValueConst key, const char *name)
{
    JSValue slot = crypto_key_algorithm(ctx, key), v;
    const char *nm;
    bool same;

    v = JS_GetPropertyStr(ctx, slot, "name");
    JS_FreeValue(ctx, slot);
    nm = JS_ToCString(ctx, v);
    JS_FreeValue(ctx, v);
    CHECK(nm != NULL, "a CryptoKey's [[algorithm]].name could not be read back as UTF-8 — this engine wrote it "
                      "from a registered algorithm name");
    same = strcmp(nm, name) == 0;
    JS_FreeCString(ctx, nm);
    return same;
}

static int sv_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                   JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    SvState *s = st;
    const int magic = idl_step_magic(hdr);
    const bool verifying = magic == SC_M_VERIFY;
    /* §14.3.3 step 1 / §14.3.4 step 1's "the algorithm and key parameters", and the two BufferSource positions
       whose ORDER is the only thing the two IDLs disagree about. */
    JSValueConst alg  = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSValueConst key  = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValueConst sig  = verifying ? (argc > 2 ? argv[2] : JS_UNDEFINED) : JS_UNDEFINED;
    JSValueConst data = verifying ? (argc > 3 ? argv[3] : JS_UNDEFINED) : (argc > 2 ? argv[2] : JS_UNDEFINED);
    int r;

    *presult = JS_UNDEFINED;

    if (!s->p.started) {
        s->name_v = s->handle = s->bytes = s->sig = JS_UNDEFINED;
        s->key_off = 0;
        s->off = 0;
        s->alg = (uint8_t)SECURE_HASH_SHA256;
        s->unknown = 0;
        s->unknown_arg = 0;
        s->has_example = 0;
        sc_promise_begin(ctx, &s->p);
        DCHECK(argc >= (verifying ? 4 : 3),
               "§14.3.3 or §14.3.4 ran with fewer than its declared arguments — Web IDL §3.6 step 5 refuses "
               "that in the prologue and §3.7.7 turns the refusal into a rejection, so the body is only ever "
               "entered with all of them");
        DCHECK(magic == SC_M_SIGN || magic == SC_M_VERIFY,
               "the sign/verify machine ran under a magic neither member declares");
    }
    /* Only SV_NAME and SV_NAME_STR park on a request able to throw — the same claim §14.3.5's machine makes
       and for the same reason: SV_SELECT parks on step_fork_run, and the three walking stages rest on
       JS_STEP_YIELD, which the driver re-enters with JS_UNDEFINED. */
    DCHECK(!JS_IsException(cb_result) || hdr->stage == SV_NAME || hdr->stage == SV_NAME_STR,
           "§14.3.3/§14.3.4 was delivered an abrupt completion at a stage that parks on no request able to "
           "throw — only the `name` read and its ToString run the page's code");

    STEP_DISPATCH(SV_STAGES, hdr->stage, "Web Cryptography §14.3.3 sign / §14.3.4 verify", JS_STEP_ABRUPT);

    STEP_ARM(SV_NAME);
    if (JS_IsString(alg) || concolic_is(alg)) {
        /* §18.4.4's DOMString arm IS the name, and unknown external input stands for whatever the page was
           given, so neither reads a member. */
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        s->name_v = JS_DupValue(ctx, alg);
    } else {
        r = step_getprop_run(ctx, hdr, alg, g_atom_name, cb_result, &s->name_v, out_cb, out_argc);
        if (r > 0) return r;
        if (r < 0) return sc_reject(ctx, &s->p, presult);
        cb_result = JS_UNDEFINED;
    }
    STEP_GOTO(hdr->stage, SV_NAME_STR, &hdr->get_phase, &hdr->str_phase, &hdr->num_phase, NULL);

    STEP_ARM(SV_NAME_STR);
    if (JS_IsUndefined(s->name_v)) {
        JS_FreeValue(ctx, cb_result);
        JS_ThrowTypeError(ctx, "the algorithm passed to SubtleCrypto.%s has no `name`, which the Algorithm "
                               "dictionary declares as a required member", verifying ? "verify" : "sign");
        return sc_reject(ctx, &s->p, presult);
    }
    if (!JS_IsString(s->name_v) && !concolic_is(s->name_v)) {
        JSValue str;

        r = step_tostring_run(ctx, hdr, s->name_v, cb_result, &str, out_cb, out_argc);
        if (r > 0) return r;
        if (r < 0) return sc_reject(ctx, &s->p, presult);
        JS_FreeValue(ctx, s->name_v);
        s->name_v = str;
    } else {
        JS_FreeValue(ctx, cb_result);
    }
    cb_result = JS_UNDEFINED;
    STEP_GOTO(hdr->stage, SV_SELECT, &hdr->get_phase, &hdr->str_phase, &hdr->num_phase, NULL);

    STEP_ARM(SV_SELECT);
    JS_FreeValue(ctx, cb_result);
    cb_result = JS_UNDEFINED;
    if (concolic_is(s->name_v)) {
        int arm = 0;

        /* §18.4.4 step 5 OVER A NAME NOBODY KNOWS — declared as a fork for §14.3.5's reason: deciding it with
           a comparison against a shape would delete every arm but one, and the arm it kept would be the
           failing one. Outcome 0 is the registered row, because step_fork_run's rule is that outcome 0 is the
           one a run with no forking policy takes and a candidate re-fire must not be diverted onto an error. */
        r = step_fork_run(ctx, hdr, s->name_v, verifying ? SV_FORK_OP_VERIFY : SV_FORK_OP_SIGN,
                          SV_FORK_OUTCOMES, JS_OUTCOME_REAL_UNSTATED, &arm);
        if (r > 0) return r;
        DCHECK(arm >= 0 && arm < SV_FORK_OUTCOMES,
               "§18.4.4's registry fork answered with an outcome it did not declare");
        if (arm == SV_REGISTERED_N) {
            JS_ThrowDOMException(ctx, "NotSupportedError", "the algorithm named is not a registered `%s` "
                                 "algorithm", verifying ? "verify" : "sign");
            return sc_reject(ctx, &s->p, presult);
        }
    } else {
        const char *nm = JS_ToCString(ctx, s->name_v);
        bool known;

        CHECK(nm != NULL, "§18.4.4's algName could not be read back as UTF-8 after its own ToString produced "
                          "it");
        known = sd_name_matches(nm, "HMAC");
        if (!known) {
            JS_ThrowDOMException(ctx, "NotSupportedError", "'%s' is not a registered `%s` algorithm", nm,
                                 verifying ? "verify" : "sign");
            JS_FreeCString(ctx, nm);
            return sc_reject(ctx, &s->p, presult);
        }
        JS_FreeCString(ctx, nm);
    }
    STEP_GOTO(hdr->stage, SV_CHECK, &hdr->get_phase, &hdr->str_phase, &hdr->num_phase, NULL);

    STEP_ARM(SV_CHECK);
    JS_FreeValue(ctx, cb_result);
    cb_result = JS_UNDEFINED;
    {
        bool unknown = false, has_example = true;
        uint32_t handle_len = 0;

        /* §14.3.4 step 4 and §14.3.3 step 4 / §14.3.4 step 5: "getting a copy of the bytes held by the
           signature parameter" and "…by the data parameter", in the standard's own order, BEFORE the two
           refusals below. Neither copy runs the page's code, so the order is not observable here — it is the
           spec's order because there is no reason for it to be anything else.
           AN UNKNOWN OPERAND MAKES THE WHOLE RESULT UNKNOWN, and the LAST such operand is the one the derived
           unknown names as its source: for verify that is `data`, whose bytes are what an @S candidate would
           inject at, and for sign there is only one. */
        if (verifying) {
            if (concolic_is(sig))
                s->unknown_arg = 2;
            s->sig = sc_operand_bytes(ctx, sig, &unknown, &has_example);
            CHECK(!JS_IsException(s->sig), "§14.3.4 step 4's copy of the signature could not be allocated");
        }
        if (concolic_is(data))
            s->unknown_arg = (uint8_t)(verifying ? 3 : 2);
        s->bytes = sc_operand_bytes(ctx, data, &unknown, &has_example);
        CHECK(!JS_IsException(s->bytes), "the copy of the message could not be allocated");
        s->unknown = unknown ? 1 : 0;
        s->has_example = (unknown && has_example) ? 1 : 0;

        /* §14.3.3 step 9 / §14.3.4 step 10. */
        if (!sv_key_algorithm_is(ctx, key, "HMAC")) {
            JS_ThrowDOMException(ctx, "InvalidAccessError", "%s",
                                 "the key was not created for the algorithm this call names");
            return sc_reject(ctx, &s->p, presult);
        }
        /* §14.3.3 step 10 / §14.3.4 step 11: "If the [[usages]] internal slot of key does not contain an entry
           that is \"sign\"" (respectively "verify"), "then throw an InvalidAccessError." */
        if ((crypto_key_usages(ctx, key) &
             (uint32_t)(verifying ? CRYPTO_KEY_USAGE_VERIFY : CRYPTO_KEY_USAGE_SIGN)) == 0) {
            JS_ThrowDOMException(ctx, "InvalidAccessError", "the key's usages do not include '%s'",
                                 verifying ? "verify" : "sign");
            return sc_reject(ctx, &s->p, presult);
        }
        /* §31.6.1 step 1 / §31.6.2 step 1's opening clause, which is one sentence in both: "using the key
           represented by the [[handle]] internal slot of key, the hash function identified by the hash
           attribute of the [[algorithm]] internal slot of key". */
        s->handle = crypto_key_handle(ctx, key);
        (void)JS_GetBufferBytes(s->handle, &handle_len);
        s->alg = (uint8_t)hmac_key_hash(ctx, key);
        hmac_begin(&s->mac, (SecureHashAlgorithm)s->alg, handle_len);
        s->key_off = 0;
        s->off = 0;
    }
    STEP_GOTO(hdr->stage, SV_KEY, &hdr->get_phase, &hdr->str_phase, &hdr->num_phase, NULL);

    STEP_ARM(SV_KEY);
    JS_FreeValue(ctx, cb_result);
    cb_result = JS_UNDEFINED;
    {
        uint32_t len = 0;
        const uint8_t *p = JS_GetBufferBytes(s->handle, &len);
        size_t block = hmac_block(&s->mac);
        size_t take;

        DCHECK(p != NULL || len == 0,
               "a CryptoKey's [[handle]] is detached — nothing but this engine holds it, and §31.6.4 step 9 "
               "copied the bytes into it precisely so no page can reach them");
        DCHECK(s->key_off <= (uint64_t)len, "the key walk is past the end of the [[handle]]");
        take = (size_t)((uint64_t)len - s->key_off) < block ? (size_t)((uint64_t)len - s->key_off) : block;
        if (take > 0) {
            hmac_key_update(&s->mac, p + s->key_off, take);
            s->key_off += (uint64_t)take;
            /* ONE BLOCK, THEN ASK — FIPS 198-1 Table 1 step 2 is unbounded in the page's own key. */
            return JS_STEP_YIELD;
        }
        DCHECK(s->key_off == (uint64_t)len, "the key walk stopped short with a whole block still in it");
        hmac_key_end(&s->mac);
    }
    STEP_GOTO(hdr->stage, SV_TEXT, &hdr->get_phase, &hdr->str_phase, &hdr->num_phase, NULL);

    STEP_ARM(SV_TEXT);
    JS_FreeValue(ctx, cb_result);
    cb_result = JS_UNDEFINED;
    {
        uint32_t len = 0;
        const uint8_t *p = JS_GetBufferBytes(s->bytes, &len);
        size_t block = hmac_block(&s->mac);
        size_t take;

        DCHECK(p != NULL || len == 0, "this algorithm's own copy of the message is detached");
        DCHECK(s->off <= len, "the message walk is past the end of its own copy");
        take = (size_t)len - s->off < block ? (size_t)len - s->off : block;
        if (take > 0) {
            hmac_text_update(&s->mac, p + s->off, take);
            s->off += (uint32_t)take;
            return JS_STEP_YIELD;
        }
        DCHECK(s->off == len, "the message walk stopped short with a whole block still in it");
    }
    STEP_GOTO(hdr->stage, SV_FINISH, &hdr->get_phase, &hdr->str_phase, &hdr->num_phase, NULL);

    STEP_ARM(SV_FINISH);
    JS_FreeValue(ctx, cb_result);
    {
        uint8_t mac[SECURE_HASH_MAX_DIGEST];
        size_t mac_len = hmac_mac_size((SecureHashAlgorithm)s->alg);
        JSValue result;

        /* FIPS 198-1 §4 Table 1 steps 6-9, which is §31.6.1 step 1's whole "MAC Generation operation". */
        hmac_finish(&s->mac, mac, sizeof mac);
        if (verifying) {
            /* §31.6.2 step 2: "Return true if mac is equal to signature and false otherwise." */
            uint32_t sig_len = 0;
            const uint8_t *sp = JS_GetBufferBytes(s->sig, &sig_len);
            bool ok = hmac_mac_equal(mac, mac_len, sp ? sp : (const uint8_t *)"", sig_len);

            result = JS_NewBool(ctx, ok);
        } else {
            /* §14.3.3 step 13: "Let result be the result of creating an ArrayBuffer in realm, containing
               signature." The realm is THIS one — a C member runs in the realm that defined it, which is the
               realm whose prototype carries this member, which is step 5's relevant realm of `this`. */
            result = JS_NewArrayBufferCopy(ctx, mac, mac_len);
            CHECK(!JS_IsException(result), "§14.3.3 step 13's ArrayBuffer could not be allocated");
        }
        if (s->unknown) {
            JSValueConst src = argv[s->unknown_arg];
            JSValue example = s->has_example ? result : JS_UNDEFINED;

            if (!s->has_example)
                JS_FreeValue(ctx, result);
            result = concolic_builtin_hook(ctx, src, verifying ? "verify" : "sign", example);
            DCHECK(!JS_IsUninitialized(result),
                   "an argument was recorded as unknown external input and the derivation declined it — the "
                   "two are one fact read at two stages, and they have come apart");
        }
        return sc_resolve(ctx, &s->p, result, presult);
    }
}

static const IdlStepDecl SV_DECL = {
    sv_step, sizeof(SvState), sv_visit, NULL,
    "Web Cryptography §14.3.3 sign / §14.3.4 verify", SV_STEPS,
    /* catches_abrupt: step 3 REJECTS for every error normalizing an algorithm produced, and a `name` getter
       that throws after suspending is one of them. */
    1
};

/* ---- §14.3.1 The encrypt method / §14.3.2 The decrypt method --------------------------------------------- */

/* ONE MACHINE FOR BOTH, which is the same statement §14.3.3/§14.3.4 make one section over and for a stronger
 * reason: sign and verify differ in their ARGUMENT LISTS, and these two do not. Their fourteen steps are
 * word-for-word identical but for the `op` step 2 normalizes with, the usage step 10 demands, and which of
 * §29.4.1 / §29.4.2 step 11 performs — so two machines would be one algorithm written twice, and the seam
 * between them is where the two would drift.
 *
 * WHY AES-GCM ALONE IS THE REGISTRY. §29.2 Registration gives BOTH `encrypt` and `decrypt` the Parameters
 * `AesGcmParams` and the Result `byte sequence`; §22.2, §24.2 and §26.2-§28.2 are the other chapters' rows and
 * none of them is built, because each needs a bignum, a curve or a block mode this engine does not have.
 * §18.5.1 Recommendations is explicit that an engine may register nothing at all: "there are no algorithms
 * that conforming user agents are required to implement".
 *
 * THE MODE IS core/crypto/aes_gcm.c AND THE OPERATION IS HERE, which is the split §31's HMAC already stands on
 * the other side of: core/crypto/hmac.c is FIPS 198-1's MAC and §14.3.3's promise, its two InvalidAccessErrors
 * and its walk are in this file. NIST SP 800-38D's Algorithm 4 and Algorithm 5 are likewise a pure C primitive
 * that knows no JSContext, and §29.4.1 steps 1-8 / §29.4.2 steps 1-9 are Web IDL algorithm steps over page
 * values — they read a dictionary, refuse lengths and settle a promise, none of which belongs in a cipher.
 * core/crypto/aes_gcm_key.c is the third piece and is neither: it is what an AES-GCM CryptoKey IS.
 *
 * IT IS BLOCK-AT-A-TIME FOR §Every-runtime-job's REASON. Three of this algorithm's inputs are of the PAGE'S
 * size — §29.3's `iv` ("May be up to 2^64-1 bytes long"), its `additionalData`, and `data` — so each walk
 * rests on JS_STEP_YIELD every AES_BLOCK bytes exactly as §14.3.3's message walk does, and the whole of what
 * rides the park is the POD AesGcm beside the two buffers.
 *
 * NAMED RESIDUAL — §29.3's `tagLength` IS CONVERTED HERE AND NOT AT ITS TYPE. WHAT IS NOT COVERED: Web IDL
 * declares it `[EnforceRange] octet`, and core/idl_args.h has no row for `octet` at all — its integer list runs
 * long, unsigned long, unsigned short, long long and unsigned long long, each with its [EnforceRange] twin — so
 * §3.2.4.9 ConvertToInt's four steps at bounds 0..255 are spelled at EN_TAGLEN_NUM instead of once at the type,
 * exactly as §31.3's `length` is spelled at IK_LENGTH_NUM. WHAT THE NEXT DIFF BUILDS: an IDL_OCTET_ENFORCE row
 * beside IDL_UNSIGNED_LONG_ENFORCE, admitted by idl_is_integer and given its bounds in idl_num_of, after which
 * this member is declared rather than coerced. HOW ITS ABSENCE WOULD SHOW: an integer member of some other
 * dictionary declared `octet` has to reach for a wider row, and the value a page may write to it is then a
 * range this engine chose rather than the one its IDL states.
 *
 * NAMED RESIDUAL — AN EXAMPLE-FREE UNKNOWN `data` CONTRIBUTES THE EMPTY BYTE SEQUENCE. WHAT IS NOT COVERED:
 * sc_operand_bytes answers the empty sequence for unknown external input carrying no BufferSource example,
 * which is §14.3.5's own honest answer for a digest and is a DIFFERENT fact here — for §14.3.2 an empty
 * ciphertext is refused by §29.4.2 step 4, so the promise rejects with an OperationError whose REASON is this
 * engine's missing bytes rather than the page's short argument. The observable matches a browser handed an
 * empty ciphertext, which is why this is narrower than the spec and not wrong. WHAT THE NEXT DIFF BUILDS: the
 * bytes of an unknown BufferSource, the world core/idl_args.h's IDL_BUFFERSOURCE_OR_DICT row names at its
 * outcome 1 — after which an unknown ciphertext is walked and the rejection, if any, is the tag's. HOW ITS
 * ABSENCE WOULD SHOW: a §14.3.2 call over a ciphertext this run never made concrete rejects at the LENGTH
 * refusal, so no fork of the tag comparison is ever recorded for it and the derived unknown carries no example.
 *
 * NAMED RESIDUAL — THE PRESENCE OF AN OPTIONAL MEMBER READ OFF AN UNKNOWN ALGORITHM OBJECT IS NOT FORKED.
 * WHAT IS NOT COVERED: where `algorithm` is itself unknown external input, each member read answers the
 * unknown (§14.3.9's machine does the same for §31.3's `length`), so §3.2.17 step 4.1.4's "If jsMemberValue is
 * not undefined" is DECIDED present for `additionalData` — the absent world, in which §29.4.1 step 5 supplies
 * the empty byte sequence, is never explored. WHAT THE NEXT DIFF BUILDS: that presence question as a
 * step_fork_run at this seam, which is the ask core/idl_args.h's IdlDictWalk already reserves its `ask` buffer
 * for and names in its own words. HOW ITS ABSENCE WOULD SHOW: two AES-GCM ciphertexts over one unknown
 * algorithm object differ only in whether additional data was authenticated, and a run over such an object
 * records exactly one of the two.
 *
 * WHOSE BYTES DECIDE WHAT. Every value this algorithm branches on is the PAGE'S — the `iv`'s length, the
 * `tagLength`, the ciphertext's length, and the authentication tag itself — so each of them is a REFUSAL and
 * never an assert: an `OperationError` for §29.4.1 step 3's Otherwise, for §29.4.2 step 4's short ciphertext,
 * for an empty IV, and for §29.4.2 step 8's "FAIL". Asserting on any of them would hand a page an abort
 * switch over the whole engine, which CLAUDE.md §WHOSE-BYTES-STATE-THE-VALUE forbids by name. What this file
 * DOES assert is what IT computed: that the walks stop where their own lengths say, and that the tag length it
 * hands the mode is one of the seven the step above it already refused every other value for. */

enum { SC_M_ENCRYPT = 0, SC_M_DECRYPT };

/* step_fork_run keeps a BORROWED pointer to the operation string, so each must outlive the ask; and a fork's
   operation is its CROSS-SESSION NAME, so it is a static and never a string composed at the ask. */
static const char EN_FORK_OP_ENC[]     = "SubtleCrypto.encrypt/normalizeAlgorithm";
static const char EN_FORK_OP_DEC[]     = "SubtleCrypto.decrypt/normalizeAlgorithm";
static const char EN_FORK_OP_TAG_ENC[] = "SubtleCrypto.encrypt/AesGcmParams.tagLength";
static const char EN_FORK_OP_TAG_DEC[] = "SubtleCrypto.decrypt/AesGcmParams.tagLength";

#define EN_REGISTERED_N 1
#define EN_FORK_OUTCOMES (EN_REGISTERED_N + 1)

/* §29.4.1 Encrypt step 3 / §29.4.2 Decrypt step 1's SEVEN, IN BITS: "If the tagLength member of
   normalizedAlgorithm is one of 32, 64, 96, 104, 112, 120 or 128".
   128 IS FIRST AND THE STANDARD'S LIST IS NOT. step_fork_run's one rule on the numbering is that outcome 0 is
   the arm a run with no forking policy takes, and for an unknown `tagLength` that must be the arm an absent
   member takes — step 3's "If the tagLength member of normalizedAlgorithm is not present: Let tagLength be
   128" — so a recorded recipe and a candidate re-fire both land on the value every page that omits the member
   already gets. The two arms past the seven are the two REFUSALS this member has, and they are separate
   because they are different exceptions from different documents: outcome 7 is §29.4.1 step 3's "Otherwise:
   throw an OperationError" (a value the octet accepted and this list does not), and outcome 8 is Web IDL
   §3.3.6 [EnforceRange]'s TypeError, which fires at §18.4.4 step 6's CONVERSION and therefore before any step
   of §29.4 runs at all. A fork that dropped either would delete a world the page can observe through a
   `.catch` that reads `e.name`. */
static const uint16_t EN_TAG_BITS[] = { 128, 32, 64, 96, 104, 112, 120 };
#define EN_TAG_N ((int)COUNTOF(EN_TAG_BITS))
#define EN_TAG_OUTCOMES (EN_TAG_N + 2)

#define EN_STAGES(X)                                                                                          \
    X(EN_NAME, "Web Cryptography §18.4.4 normalizing an algorithm step 2 (Get(alg, \"name\") for the encrypt " \
               "or decrypt operation)")                                                                       \
    X(EN_NAME_STR, "Web Cryptography §18.4.4 normalizing an algorithm step 2 (converting alg[\"name\"] to its "\
                   "DOMString)")                                                                              \
    X(EN_SELECT, "Web Cryptography §18.4.4 normalizing an algorithm step 5 (the case-insensitive lookup of "   \
                 "algName in the encrypt or decrypt operation's registeredAlgorithms)")                        \
    X(EN_AAD, "Web Cryptography §18.4.4 normalizing an algorithm steps 6 and 10 (§29.3 AesGcmParams' "         \
              "`additionalData`, first of that dictionary's own members in Web IDL §3.2.17's read order)")      \
    X(EN_IV, "Web Cryptography §18.4.4 normalizing an algorithm steps 6 and 10 (§29.3 AesGcmParams' required " \
             "`iv`)")                                                                                          \
    X(EN_TAGLEN, "Web Cryptography §18.4.4 normalizing an algorithm step 6 (§29.3 AesGcmParams' `tagLength`, " \
                 "last of that dictionary's own members in Web IDL §3.2.17's read order)")                      \
    X(EN_TAGLEN_NUM, "Web IDL §3.2.4.9 Abstract operations' ConvertToInt under §3.3.6 [EnforceRange], over "   \
                     "§29.3's `tagLength` member, whose declared type is `octet`")                              \
    X(EN_CHECK, "Web Cryptography §14.3.1 steps 4 and 9-10 / §14.3.2 steps 4 and 9-10, then §29.4.1 steps "    \
                "1-5 / §29.4.2 steps 1-7 (the copy of `data`, the two InvalidAccessError refusals over the "    \
                "key, and this chapter's own refusals over the page's lengths)")                                \
    X(EN_IVWALK, "NIST SP 800-38D §7.1 step 2 / §7.2 step 3 for ONE block of the IV — J_0 being formed by "     \
                 "whichever of its two arms len(IV) selects")                                                   \
    X(EN_AADWALK, "NIST SP 800-38D §7.1 step 5 / §7.2 step 6 for ONE block of the A region")                     \
    X(EN_TEXT, "NIST SP 800-38D §7.1 steps 3 and 5 / §7.2 steps 4 and 6 for ONE block of the text region, "     \
               "interleaved under §7's own licence that \"equivalent sets of steps that produce the correct "   \
               "output are permitted\"")                                                                        \
    X(EN_FINISH, "Web Cryptography §14.3.1 steps 12-14 / §14.3.2 steps 12-14 (queue a global task on the "      \
                 "crypto task source, create the ArrayBuffer and resolve promise), over §7.1 step 6's T or "    \
                 "§7.2 step 8's comparison")
enum { IDL_STEP_STAGE_BASE(EN_STAGES) EN_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const EN_STEPS[] = { EN_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    ScPromise p;
    JSValue   name_v;      /* alg["name"] as read, then as its DOMString, or the concolic itself (owned) */
    JSValue   iv;          /* §18.4.4 step 10's copy of `iv`'s bytes (owned) */
    JSValue   aad;         /* the same of `additionalData`; JS_UNDEFINED when the member is ABSENT (owned) */
    JSValue   tag_v;       /* alg["tagLength"] as read, before its conversion (owned) */
    JSValue   bytes;       /* §14.3.1 step 4's copy of `data`, or the concolic's example's (owned) */
    JSValue   out;         /* §14.3.1 step 13's ArrayBuffer, filled block by block (owned) */
    /* THE CONCOLIC THE RESULT NAMES AS ITS SOURCE, HELD RATHER THAN INDEXED. §14.3.3's machine keeps an index
       into argv because every operand it can derive from IS an argument; here an unknown can arrive as a
       MEMBER of the algorithm object, and argv[0] is then the object and not the unknown —
       concolic_builtin_hook answers JS_UNINITIALIZED for a known operand, so an index would have named a value
       the derivation declines. It is the LAST unknown in §3.2.17's read order for §14.3.4's reason: `data` is
       the operand an @S candidate injects at, and it is read last. (owned) */
    JSValue   src;
    AesGcm    gcm;         /* POD, and that is load-bearing — it rides forks, parks and resumes as bytes */
    uint64_t  iv_off;      /* how much of `iv` §7.1 step 2 has absorbed */
    uint32_t  aad_off;     /* how much of `aad` §7.1 step 5 has absorbed */
    uint32_t  off;         /* how much of the text region §7.1 step 3 has enciphered */
    uint32_t  text_len;    /* §29.4.2 step 6's actualCiphertext length; for encrypt, all of `bytes` */
    uint16_t  tag_bits;    /* §7.1's `t`, in BITS — the unit §29.4.x states it in */
    uint8_t   unknown;     /* an operand was unknown external input, so the result is */
    uint8_t   has_example; /* EVERY unknown operand supplied bytes, so the result is a real observation */
} EnState;

static void en_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    EnState *s = st;

    sc_promise_visit(ctx, &s->p, v);
    if (!s->p.started)
        return;
    v->val(ctx, &s->name_v);
    v->val(ctx, &s->iv);
    v->val(ctx, &s->aad);
    v->val(ctx, &s->tag_v);
    v->val(ctx, &s->bytes);
    v->val(ctx, &s->out);
    v->val(ctx, &s->src);
}

/* §18.4.4 step 10's BufferSource ARM, WHICH IS NOT THE ARGUMENT MACHINE'S AND SAYS SO HERE. §3.2.17's member
 * loop in core/idl_args.c has arms for the numeric types, the strings, the enumerations, the booleans, the
 * interfaces and the types that push a level, and NONE for IDL_BUFFERSOURCE — a member declared it falls past
 * every arm and is PLACED UNCONVERTED, so §3.2.26 Buffer source types' brand test and its two refusals would
 * not be performed at all. §29.3 AesGcmParams is the first dictionary in this engine to declare one.
 *
 * WHAT IS NOT COVERED: this is §3.2.26's conversion written at ONE site rather than at the type, so a second
 * dictionary declaring a BufferSource member gets no conversion from it and none from the member loop either.
 * WHAT THE NEXT DIFF BUILDS is an IDL_BUFFERSOURCE arm in idl_level_run's member loop, beside the
 * `idl_is_numeric` one, performing the brand test and then idl_buffer_source_refuse's [AllowShared] and
 * [AllowResizable] refusals — that file already holds both halves. HOW ITS ABSENCE WOULD SHOW: a page passing
 * a SharedArrayBuffer or a length-tracking view over a resizable buffer to a BufferSource-typed member of some
 * OTHER dictionary is accepted where a browser throws a TypeError at the conversion, and the algorithm behind
 * it then reads a window that no longer describes its allocation.
 *
 * Returns -1 with a TypeError live, or 0 with `*out` owning the copy (JS_UNDEFINED for an absent OPTIONAL
 * member — `required` is the caller's to state, because the two members differ in exactly that). */
static int en_member_bytes(JSContext *ctx, EnState *s, JSValueConst v, const char *member, bool required,
                           JSValue *out)
{
    *out = JS_UNDEFINED;
    /* §3.2.17 step 4.1.4: "If jsMemberValue is not undefined" — an undefined member is an ABSENT one, and for
       `required BufferSource iv` step 4.1.6 is "if member is a required dictionary member, then throw a
       TypeError". */
    if (JS_IsUndefined(v)) {
        if (!required)
            return 0;
        JS_ThrowTypeError(ctx, "the algorithm passed to SubtleCrypto has no `%s`, which §29.3 AesGcmParams "
                               "declares as a required member", member);
        return -1;
    }
    if (concolic_is(v)) {
        JSValue ex = concolic_example(ctx, v);
        bool holds = JS_IsArrayBuffer(ex) || JS_GetTypedArrayType(ex) >= 0 || JS_IsDataView(ex);

        /* §3.2.26's "get a copy of the bytes held by the buffer source" HAS NO ANSWER OVER AN UNKNOWN, which is
           the same sentence core/idl_args.h's IDL_BUFFERSOURCE_OR_DICT row writes at its own buffer arm. An
           EXAMPLE that holds bytes is a real observation of them and is walked; one that does not is a world
           this engine cannot execute, and an empty byte sequence here would not be a narrower answer — it is a
           zero-length IV, which §5.2.1.1 Input Data forbids outright, so the page would be told its own
           argument was empty when the truth is that this engine had no bytes to give the mode. */
        if (!holds) {
            JS_FreeValue(ctx, ex);
            DFAILF("§29.3 AesGcmParams' `%s` is unknown external input carrying no BufferSource example, and "
                   "§3.2.26 Buffer source types' \"get a copy of the bytes held by the buffer source\" has no "
                   "answer over an unknown. Build the bytes of an unknown BufferSource — the same world "
                   "core/idl_args.h's IDL_BUFFERSOURCE_OR_DICT row names at its outcome 1 — rather than "
                   "letting an empty sequence stand in for them, which would render as the page's own empty "
                   "`%s`", member, member);
            /* THE RELEASE ARM, WHICH IS A THROW AND NOT A QUIET RETURN. In release the DFAILF above vanishes,
               so this line is the shipped answer: an OperationError ENDS the algorithm, where returning the
               empty sequence would hand §29.4's steps a zero-length IV and report the engine's own gap as the
               page's argument. An arm beneath a DFAIL that returns successfully is what leaves a sibling
               component a state it will never test for. */
            JS_ThrowDOMException(ctx, "OperationError",
                                 "AES-GCM's `%s` is unknown external input whose bytes this engine cannot "
                                 "supply", member);
            return -1;
        }
        s->unknown = 1;
        JS_FreeValue(ctx, s->src);
        s->src = JS_DupValue(ctx, v);
        *out = sc_copy_buffer_source(ctx, ex);
        JS_FreeValue(ctx, ex);
        CHECK(!JS_IsException(*out), "§18.4.4 step 10's copy of an unknown member's example could not be "
                                     "allocated");
        return 0;
    }
    /* §3.2.26's BRAND, which the member loop would have performed had it an arm for this type: "an ArrayBuffer,
       a typed array or a DataView crosses as itself and anything else is a TypeError". It is asked BEFORE the
       two refusals below, which is the order §3.2.26's own four algorithms state. */
    if (!JS_IsArrayBuffer(v) && JS_GetTypedArrayType(v) < 0 && !JS_IsDataView(v)) {
        JS_ThrowTypeError(ctx, "§29.3 AesGcmParams' `%s` is declared BufferSource, and Web IDL §3.2.26 Buffer "
                               "source types admits only an ArrayBuffer, a typed array or a DataView", member);
        return -1;
    }
    /* §3.2.26's TWO REFUSALS. §4.2 BufferSource carries neither [AllowShared] nor [AllowResizable] — its own
       note says [AllowShared] "cannot be used with BufferSource as ArrayBuffer does not support it" — so both
       are unconditional at this position. The resizable one is the memory-safety boundary and not pedantry: a
       length-tracking view reports a byte length recomputed at every read, and this algorithm lets the page's
       code run between the read and the walk at every one of its rest points. */
    if (JS_IsSharedBufferSource(v)) {
        JS_ThrowTypeError(ctx, "§3.2.26 Buffer source types refuses a SharedArrayBuffer to §29.3 AesGcmParams' "
                               "`%s`: §4.2 BufferSource carries no [AllowShared] extended attribute and cannot "
                               "carry one", member);
        return -1;
    }
    if (!JS_IsFixedLengthBufferSource(v)) {
        JS_ThrowTypeError(ctx, "§3.2.26 Buffer source types refuses a resizable buffer to §29.3 AesGcmParams' "
                               "`%s`: the position carries no §3.3.1 [AllowResizable] extended attribute", member);
        return -1;
    }
    *out = sc_copy_buffer_source(ctx, v);
    CHECK(!JS_IsException(*out), "§18.4.4 step 10's copy of a BufferSource member could not be allocated");
    return 0;
}

/* §29.4.1 steps 1-2 and 4 / §29.4.2 steps 2-3, WHICH THIS ENGINE'S REPRESENTATION DISCHARGES RATHER THAN
   TESTS. Every one of them refuses a length ("greater than 2^64 - 1 bytes", "greater than 2^39 - 256 bytes")
   that a buffer in this engine cannot reach: JS_GetBufferBytes reports a uint32_t, so the comparison would be
   an assert whose two sides cannot disagree — a NON-check wearing the syntax of one. What is asserted instead
   is the PREMISE, at compile time, so the day that type widens the steps stop being discharged and this line
   is what says so. */
_Static_assert((uint64_t)UINT32_MAX < UINT64_MAX,
               "§29.4.1 steps 1-2's \"greater than 2^64 - 1 bytes\" is discharged by the buffer length type, "
               "and that type is now wide enough to reach the bound — the two steps need real comparisons");
_Static_assert((uint64_t)UINT32_MAX < 549755813632ull,
               "§29.4.1 step 4's \"greater than 2^39 - 256 bytes\" is discharged by the buffer length type, "
               "and that type is now wide enough to reach the bound — the step needs a real comparison");

static int en_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                   JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    EnState *s = st;
    const int magic = idl_step_magic(hdr);
    const bool decrypting = magic == SC_M_DECRYPT;
    const char *const op = decrypting ? "decrypt" : "encrypt";
    /* §14.3.1 step 1 / §14.3.2 step 1's "the algorithm and key parameters", and step 4's `data`. The two IDLs
       are identical, which is why one argument list serves both. */
    JSValueConst alg  = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSValueConst key  = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValueConst data = argc > 2 ? argv[2] : JS_UNDEFINED;
    int r;

    *presult = JS_UNDEFINED;

    if (!s->p.started) {
        s->name_v = s->iv = s->aad = s->tag_v = s->bytes = s->out = s->src = JS_UNDEFINED;
        memset(&s->gcm, 0, sizeof s->gcm);
        s->iv_off = 0;
        s->aad_off = 0;
        s->off = 0;
        s->text_len = 0;
        s->tag_bits = 0;
        s->unknown = 0;
        s->has_example = 1;
        sc_promise_begin(ctx, &s->p);
        DCHECK(argc >= 3,
               "§14.3.1 or §14.3.2 ran with fewer than its three declared arguments — Web IDL §3.6 step 5 "
               "refuses that in the prologue and §3.7.7 turns the refusal into a rejection, so the body is "
               "only ever entered with all of them");
        DCHECK(magic == SC_M_ENCRYPT || magic == SC_M_DECRYPT,
               "the encrypt/decrypt machine ran under a magic neither member declares");
    }
    /* THE STAGES THAT PARK ON A REQUEST ABLE TO THROW ARE THE ONES THAT RUN THE PAGE'S CODE, and the condition
       below is the list rather than a count of it: EN_NAME, its ToString, the three §29.3 member reads and
       `tagLength`'s own coercion. EN_SELECT parks on step_fork_run alone, EN_TAGLEN_NUM parks on either, and
       EN_IVWALK / EN_AADWALK / EN_TEXT rest on JS_STEP_YIELD, which the driver re-enters with JS_UNDEFINED. */
    DCHECK(!JS_IsException(cb_result) || hdr->stage == EN_NAME || hdr->stage == EN_NAME_STR ||
               hdr->stage == EN_AAD || hdr->stage == EN_IV || hdr->stage == EN_TAGLEN ||
               hdr->stage == EN_TAGLEN_NUM,
           "§14.3.1/§14.3.2 was delivered an abrupt completion at a stage that parks on no request able to "
           "throw — only the `name` read, its ToString, the three §29.3 member reads and `tagLength`'s own "
           "coercion run the page's code");

    STEP_DISPATCH(EN_STAGES, hdr->stage, "Web Cryptography §14.3.1 encrypt / §14.3.2 decrypt", JS_STEP_ABRUPT);

    STEP_ARM(EN_NAME);
    if (JS_IsString(alg) || concolic_is(alg)) {
        /* §18.4.4's DOMString arm IS the name, and unknown external input stands for whatever the page was
           given, so neither reads a member. */
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        s->name_v = JS_DupValue(ctx, alg);
    } else {
        r = step_getprop_run(ctx, hdr, alg, g_atom_name, cb_result, &s->name_v, out_cb, out_argc);
        if (r > 0) return r;
        if (r < 0) return sc_reject(ctx, &s->p, presult);
        cb_result = JS_UNDEFINED;
    }
    STEP_GOTO(hdr->stage, EN_NAME_STR, &hdr->get_phase, &hdr->str_phase, &hdr->num_phase, NULL);

    STEP_ARM(EN_NAME_STR);
    if (JS_IsUndefined(s->name_v)) {
        JS_FreeValue(ctx, cb_result);
        JS_ThrowTypeError(ctx, "the algorithm passed to SubtleCrypto.%s has no `name`, which the Algorithm "
                               "dictionary declares as a required member", op);
        return sc_reject(ctx, &s->p, presult);
    }
    if (!JS_IsString(s->name_v) && !concolic_is(s->name_v)) {
        JSValue str;

        r = step_tostring_run(ctx, hdr, s->name_v, cb_result, &str, out_cb, out_argc);
        if (r > 0) return r;
        if (r < 0) return sc_reject(ctx, &s->p, presult);
        JS_FreeValue(ctx, s->name_v);
        s->name_v = str;
    } else {
        JS_FreeValue(ctx, cb_result);
    }
    cb_result = JS_UNDEFINED;
    STEP_GOTO(hdr->stage, EN_SELECT, &hdr->get_phase, &hdr->str_phase, &hdr->num_phase, NULL);

    STEP_ARM(EN_SELECT);
    JS_FreeValue(ctx, cb_result);
    cb_result = JS_UNDEFINED;
    if (concolic_is(s->name_v)) {
        int arm = 0;

        /* §18.4.4 step 5 OVER A NAME NOBODY KNOWS — a fork for §14.3.3's reason: deciding it with a comparison
           against a shape would delete every arm but one, and the arm it kept would be the failing one. */
        r = step_fork_run(ctx, hdr, s->name_v, decrypting ? EN_FORK_OP_DEC : EN_FORK_OP_ENC,
                          EN_FORK_OUTCOMES, JS_OUTCOME_REAL_UNSTATED, &arm);
        if (r > 0) return r;
        DCHECK(arm >= 0 && arm < EN_FORK_OUTCOMES,
               "§18.4.4's registry fork answered with an outcome it did not declare");
        if (arm == EN_REGISTERED_N) {
            JS_ThrowDOMException(ctx, "NotSupportedError", "the algorithm named is not a registered `%s` "
                                 "algorithm", op);
            return sc_reject(ctx, &s->p, presult);
        }
    } else {
        const char *nm = JS_ToCString(ctx, s->name_v);
        bool known;

        CHECK(nm != NULL, "§18.4.4's algName could not be read back as UTF-8 after its own ToString produced "
                          "it");
        known = sd_name_matches(nm, "AES-GCM");
        if (!known) {
            JS_ThrowDOMException(ctx, "NotSupportedError", "'%s' is not a registered `%s` algorithm", nm, op);
            JS_FreeCString(ctx, nm);
            return sc_reject(ctx, &s->p, presult);
        }
        JS_FreeCString(ctx, nm);
    }
    STEP_GOTO(hdr->stage, EN_AAD, &hdr->get_phase, &hdr->str_phase, &hdr->num_phase, NULL);

    /* THE THREE MEMBERS ARE READ IN WEB IDL §3.2.17's ORDER AND NOT IN §29.3's DECLARATION ORDER, which is the
       one thing about this dictionary a page can observe with three getters and a log. §3.2.17 reads the
       inherited dictionaries first ("in order from least to most derived" — Algorithm's `name`, already read)
       and then each level's own members in LEXICOGRAPHIC order, so `additionalData` precedes `iv` precedes
       `tagLength` where the IDL declares iv, additionalData, tagLength. */
    STEP_ARM(EN_AAD);
    {
        JSValue raw = JS_UNDEFINED;

        if (concolic_is(alg)) {
            /* A MEMBER READ OFF AN UNKNOWN OBJECT IS ITSELF UNKNOWN, which is what §14.3.9's machine already
               says of HmacImportParams' `length`: unknown external input stands for whatever the page was
               given, and that includes whatever `additionalData` it held. */
            JS_FreeValue(ctx, cb_result);
            cb_result = JS_UNDEFINED;
            raw = JS_DupValue(ctx, alg);
        } else if (JS_IsString(alg)) {
            /* §18.4.4's DOMString arm builds "a new Algorithm dictionary whose name attribute is alg" and
               nothing else — so there is no member to read at all, and `iv` being REQUIRED makes that a
               TypeError at EN_IV. */
            JS_FreeValue(ctx, cb_result);
            cb_result = JS_UNDEFINED;
        } else {
            r = step_getprop_run(ctx, hdr, alg, g_atom_additional_data, cb_result, &raw, out_cb, out_argc);
            if (r > 0) return r;
            if (r < 0) return sc_reject(ctx, &s->p, presult);
            cb_result = JS_UNDEFINED;
        }
        r = en_member_bytes(ctx, s, raw, "additionalData", false, &s->aad);
        JS_FreeValue(ctx, raw);
        if (r < 0) return sc_reject(ctx, &s->p, presult);
    }
    STEP_GOTO(hdr->stage, EN_IV, &hdr->get_phase, &hdr->str_phase, &hdr->num_phase, NULL);

    STEP_ARM(EN_IV);
    {
        JSValue raw = JS_UNDEFINED;

        if (concolic_is(alg)) {
            JS_FreeValue(ctx, cb_result);
            cb_result = JS_UNDEFINED;
            raw = JS_DupValue(ctx, alg);
        } else if (JS_IsString(alg)) {
            JS_FreeValue(ctx, cb_result);
            cb_result = JS_UNDEFINED;
        } else {
            r = step_getprop_run(ctx, hdr, alg, g_atom_iv, cb_result, &raw, out_cb, out_argc);
            if (r > 0) return r;
            if (r < 0) return sc_reject(ctx, &s->p, presult);
            cb_result = JS_UNDEFINED;
        }
        r = en_member_bytes(ctx, s, raw, "iv", true, &s->iv);
        JS_FreeValue(ctx, raw);
        if (r < 0) return sc_reject(ctx, &s->p, presult);
    }
    STEP_GOTO(hdr->stage, EN_TAGLEN, &hdr->get_phase, &hdr->str_phase, &hdr->num_phase, NULL);

    STEP_ARM(EN_TAGLEN);
    if (concolic_is(alg)) {
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        s->tag_v = JS_DupValue(ctx, alg);
    } else if (JS_IsString(alg)) {
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        s->tag_v = JS_UNDEFINED;
    } else {
        r = step_getprop_run(ctx, hdr, alg, g_atom_tag_length, cb_result, &s->tag_v, out_cb, out_argc);
        if (r > 0) return r;
        if (r < 0) return sc_reject(ctx, &s->p, presult);
        cb_result = JS_UNDEFINED;
    }
    STEP_GOTO(hdr->stage, EN_TAGLEN_NUM, &hdr->get_phase, &hdr->str_phase, &hdr->num_phase, NULL);

    STEP_ARM(EN_TAGLEN_NUM);
    /* §3.2.17: for a dictionary member `undefined` IS absence, and `tagLength` carries no `= …`, so an absent
       one does not exist on normalizedAlgorithm at all — which is the POSITIVE statement §29.4.1 step 3's
       first arm reads ("If the tagLength member of normalizedAlgorithm is not present: Let tagLength be
       128"). */
    if (JS_IsUndefined(s->tag_v)) {
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        s->tag_bits = 128;
    } else if (concolic_is(s->tag_v)) {
        int arm = 0;

        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        r = step_fork_run(ctx, hdr, s->tag_v, decrypting ? EN_FORK_OP_TAG_DEC : EN_FORK_OP_TAG_ENC,
                          EN_TAG_OUTCOMES, JS_OUTCOME_REAL_UNSTATED, &arm);
        if (r > 0) return r;
        DCHECK(arm >= 0 && arm < EN_TAG_OUTCOMES,
               "§29.4.1 step 3's tag-length fork answered with an outcome it did not declare");
        if (arm == EN_TAG_N) {
            /* §29.4.1 step 3 / §29.4.2 step 1's "Otherwise: throw an OperationError" — a value the octet
               accepted and this list does not. */
            JS_ThrowDOMException(ctx, "OperationError", "%s",
                                 "AES-GCM's `tagLength` must be one of 32, 64, 96, 104, 112, 120 or 128");
            return sc_reject(ctx, &s->p, presult);
        }
        if (arm == EN_TAG_N + 1) {
            /* Web IDL §3.3.6 [EnforceRange]'s TypeError, which §18.4.4 step 6's conversion raises BEFORE any
               step of §29.4 runs — a different exception from a different document, which is why it is its own
               outcome rather than folded into the one above. */
            JS_ThrowTypeError(ctx, "%s", "the `tagLength` of the algorithm passed to SubtleCrypto is not a "
                                         "finite number in the range of an octet, and its member enforces a "
                                         "range");
            return sc_reject(ctx, &s->p, presult);
        }
        s->tag_bits = EN_TAG_BITS[arm];
    } else {
        double d = 0.0;

        r = step_todouble_run(ctx, hdr, s->tag_v, cb_result, &d, out_cb, out_argc);
        if (r > 0) return r;
        if (r < 0) return sc_reject(ctx, &s->p, presult);
        cb_result = JS_UNDEFINED;
        /* §3.3.6 [EnforceRange]'s ARM of §3.2.4.9 Abstract operations' ConvertToInt, at the bounds `octet`
           states: "If x is NaN, +∞, or −∞, then throw a TypeError"; "Set x to IntegerPart(x)"; "If x <
           lowerBound or x > upperBound, then throw a TypeError". THE BOUNDS ARE THE TYPE'S AND NOT §29.4.1's:
           an octet is 0..255, so 200 converts here and is refused by step 3 with an OperationError while 300
           never reaches step 3 at all. A page reading `e.name` in a `.catch` distinguishes them. */
        if (!isfinite(d)) {
            JS_ThrowTypeError(ctx, "%s", "the `tagLength` of the algorithm passed to SubtleCrypto is not a "
                                         "finite number");
            return sc_reject(ctx, &s->p, presult);
        }
        d = (d < 0 ? -1.0 : 1.0) * floor(fabs(d));
        if (d < 0 || d > 255.0) {
            JS_ThrowTypeError(ctx, "%s", "the `tagLength` of the algorithm passed to SubtleCrypto is outside "
                                         "the range of an octet");
            return sc_reject(ctx, &s->p, presult);
        }
        {
            int i, found = -1;

            for (i = 0; i < EN_TAG_N; i++)
                if ((double)EN_TAG_BITS[i] == d) { found = i; break; }
            if (found < 0) {
                JS_ThrowDOMException(ctx, "OperationError", "%s",
                                     "AES-GCM's `tagLength` must be one of 32, 64, 96, 104, 112, 120 or 128");
                return sc_reject(ctx, &s->p, presult);
            }
            s->tag_bits = EN_TAG_BITS[found];
        }
    }
    STEP_GOTO(hdr->stage, EN_CHECK, &hdr->get_phase, &hdr->str_phase, &hdr->num_phase, NULL);

    STEP_ARM(EN_CHECK);
    JS_FreeValue(ctx, cb_result);
    cb_result = JS_UNDEFINED;
    {
        bool unknown = false, has_example = true;
        uint32_t data_len = 0, iv_len = 0, handle_len = 0;
        const uint8_t *kp;
        JSValue handle;
        size_t tag_bytes = (size_t)(s->tag_bits / 8u);
        uint64_t out_len;

        /* §14.3.1 step 4 / §14.3.2 step 4: "getting a copy of the bytes held by the data parameter", which the
           standard numbers BEFORE the two refusals below. The copy runs none of the page's code, so the order
           is not observable here — it is the spec's order because there is no reason for it to be anything
           else. */
        s->bytes = sc_operand_bytes(ctx, data, &unknown, &has_example);
        CHECK(!JS_IsException(s->bytes), "§14.3.1 step 4's copy of `data` could not be allocated");
        if (unknown) {
            s->unknown = 1;
            JS_FreeValue(ctx, s->src);
            s->src = JS_DupValue(ctx, data);
        }
        if (!has_example)
            s->has_example = 0;
        /* §14.3.1 step 9 / §14.3.2 step 9: "If the name member of normalizedAlgorithm is not equal to the name
           attribute of the [[algorithm]] internal slot of key then throw an InvalidAccessError." The
           comparison is EXACT and not §18.4.4 step 5's case-insensitive one, because both operands are names
           THIS ENGINE wrote — step 5's own sub-step 1 sets algName to "the value of the matching key", and the
           key's is what §29.4.4 step 6 stored. */
        if (!sv_key_algorithm_is(ctx, key, "AES-GCM")) {
            JS_ThrowDOMException(ctx, "InvalidAccessError", "%s",
                                 "the key was not created for the algorithm this call names");
            return sc_reject(ctx, &s->p, presult);
        }
        /* §14.3.1 step 10 / §14.3.2 step 10: "If the [[usages]] internal slot of key does not contain an entry
           that is \"encrypt\"" (respectively "decrypt"), "then throw an InvalidAccessError." */
        if ((crypto_key_usages(ctx, key) &
             (uint32_t)(decrypting ? CRYPTO_KEY_USAGE_DECRYPT : CRYPTO_KEY_USAGE_ENCRYPT)) == 0) {
            JS_ThrowDOMException(ctx, "InvalidAccessError", "the key's usages do not include '%s'", op);
            return sc_reject(ctx, &s->p, presult);
        }
        (void)JS_GetBufferBytes(s->bytes, &data_len);
        (void)JS_GetBufferBytes(s->iv, &iv_len);
        /* §29.4.2 step 4: "If ciphertext has a length in bits less than tagLength, then throw an
           OperationError." IN BITS, which is the unit the whole of §29.4 states this member in, so the
           comparison is written that way rather than in the bytes this engine holds — the seven admissible
           values are all multiples of eight and a bytes-only reading would be right by that accident. */
        if (decrypting && (uint64_t)data_len * 8u < (uint64_t)s->tag_bits) {
            JS_ThrowDOMException(ctx, "OperationError", "%s",
                                 "the ciphertext is shorter than the authentication tag it must end with");
            return sc_reject(ctx, &s->p, presult);
        }
        /* NIST SP 800-38D §5.2.1.1 Input Data's "1 <= len(IV) <= 2^64-1", WHICH §29.4 DOES NOT RESTATE AND
           WHICH ITS step 6 INHERITS by performing that function. §29.4.1 states only the upper bound, so the
           lower one arrives as a prerequisite of the algorithm the step names — and a prerequisite a page can
           violate is a REFUSAL, not an assert. core/crypto/aes_gcm.h says in its own words that this method
           owes the OperationError before the mode is begun, and aes_gcm_begin's DCHECK is the other half of
           that one sentence. */
        if (iv_len == 0) {
            JS_ThrowDOMException(ctx, "OperationError", "%s",
                                 "AES-GCM's `iv` must hold at least one byte");
            return sc_reject(ctx, &s->p, presult);
        }
        /* §29.4.1 step 7's "ciphertext be equal to C | T" sizes the encrypt output, and §29.4.2 step 6's
           "removing the last tagLength bits from ciphertext" sizes the decrypt one. */
        s->text_len = decrypting ? (uint32_t)(data_len - (uint32_t)tag_bytes) : data_len;
        out_len = decrypting ? (uint64_t)s->text_len : (uint64_t)data_len + (uint64_t)tag_bytes;
        s->out = JS_NewArrayBufferCopy(ctx, NULL, (size_t)out_len);
        CHECK(!JS_IsException(s->out), "§14.3.1 step 13's ArrayBuffer could not be allocated");
        /* §31.6.1's opening clause one chapter over, said of this algorithm: the key material is what §13.3's
           [[handle]] internal slot holds, and §29.4.4 step 9 put FIPS 197 §6.1's 16, 24 or 32 bytes there. */
        handle = crypto_key_handle(ctx, key);
        kp = JS_GetBufferBytes(handle, &handle_len);
        DCHECK(kp != NULL && (handle_len == 16u || handle_len == 24u || handle_len == 32u),
               "an AES-GCM CryptoKey's [[handle]] is detached or is not one of FIPS 197 §6.1's three key "
               "lengths — §29.4.4 Import Key refuses every other length, so this key was not minted by it");
        aes_gcm_begin(&s->gcm, kp, handle_len, (uint64_t)iv_len, tag_bytes, decrypting);
        JS_FreeValue(ctx, handle);
        s->iv_off = 0;
        s->aad_off = 0;
        s->off = 0;
    }
    STEP_GOTO(hdr->stage, EN_IVWALK, &hdr->get_phase, &hdr->str_phase, &hdr->num_phase, NULL);

    STEP_ARM(EN_IVWALK);
    JS_FreeValue(ctx, cb_result);
    cb_result = JS_UNDEFINED;
    {
        uint32_t len = 0;
        const uint8_t *p = JS_GetBufferBytes(s->iv, &len);
        uint64_t left = aes_gcm_iv_left(&s->gcm);
        size_t take;

        DCHECK(p != NULL, "this algorithm's own copy of the `iv` is detached");
        DCHECK(left == (uint64_t)len - s->iv_off, "the IV walk and the mode disagree about how much is left");
        take = left < (uint64_t)AES_BLOCK ? (size_t)left : (size_t)AES_BLOCK;
        if (take > 0) {
            aes_gcm_iv_update(&s->gcm, p + s->iv_off, take);
            s->iv_off += (uint64_t)take;
            /* ONE BLOCK, THEN ASK — §29.3's `iv` "May be up to 2^64-1 bytes long". */
            return JS_STEP_YIELD;
        }
        aes_gcm_iv_end(&s->gcm);
    }
    STEP_GOTO(hdr->stage, EN_AADWALK, &hdr->get_phase, &hdr->str_phase, &hdr->num_phase, NULL);

    STEP_ARM(EN_AADWALK);
    JS_FreeValue(ctx, cb_result);
    cb_result = JS_UNDEFINED;
    {
        uint32_t len = 0;
        const uint8_t *p = JS_IsUndefined(s->aad) ? NULL : JS_GetBufferBytes(s->aad, &len);
        size_t take;

        /* §29.4.1 step 5: "Let additionalData be the additionalData member of normalizedAlgorithm if present
           or an empty byte sequence otherwise" — which is this walk running zero times, and not a special
           case. */
        /* `p` IS NULL FOR A PRESENT-BUT-EMPTY MEMBER AS WELL AS FOR AN ABSENT ONE, and the difference is the
           page's: `additionalData: new Uint8Array(0)` is a value a page really writes and §29.4.1 step 5 gives
           it the same empty byte sequence an absent member gets. Asserting `p != NULL` for a present member
           would have made that call abort the engine — a refusal over the PAGE'S bytes, which is exactly what
           CLAUDE.md §WHOSE-BYTES-STATE-THE-VALUE forbids. What is asserted is what this engine computed: a
           copy it made is either readable or empty. */
        DCHECK(p != NULL || len == 0, "this algorithm's own copy of `additionalData` is detached");
        DCHECK(s->aad_off <= len, "the additional-data walk is past the end of its own copy");
        take = (size_t)len - s->aad_off < AES_BLOCK ? (size_t)len - s->aad_off : (size_t)AES_BLOCK;
        if (take > 0) {
            aes_gcm_aad_update(&s->gcm, p + s->aad_off, take);
            s->aad_off += (uint32_t)take;
            return JS_STEP_YIELD;
        }
        aes_gcm_aad_end(&s->gcm);
    }
    STEP_GOTO(hdr->stage, EN_TEXT, &hdr->get_phase, &hdr->str_phase, &hdr->num_phase, NULL);

    STEP_ARM(EN_TEXT);
    JS_FreeValue(ctx, cb_result);
    cb_result = JS_UNDEFINED;
    {
        uint32_t len = 0;
        const uint8_t *in = JS_GetBufferBytes(s->bytes, &len);
        size_t out_n = 0;
        uint8_t *out = JS_GetArrayBuffer(ctx, &out_n, s->out);
        size_t take;

        DCHECK(in != NULL || len == 0, "this algorithm's own copy of `data` is detached");
        /* `out` IS NULL FOR A ZERO-LENGTH BUFFER, which §29.4.2 reaches on a page value rather than on a
           defect: a ciphertext that is exactly its own tag decrypts to the empty plaintext, and step 4 admits
           it ("a length in bits LESS than tagLength" is what it refuses). */
        DCHECK(out != NULL || out_n == 0,
               "§14.3.1 step 13's ArrayBuffer is detached — nothing but this algorithm holds it");
        DCHECK(s->off <= s->text_len && s->text_len <= len,
               "the text walk is past the end of the region §29.4.1 step 6 / §29.4.2 step 8 was given");
        take = (size_t)(s->text_len - s->off) < AES_BLOCK ? (size_t)(s->text_len - s->off)
                                                          : (size_t)AES_BLOCK;
        if (take > 0) {
            aes_gcm_text_update(&s->gcm, in + s->off, out + s->off, take);
            s->off += (uint32_t)take;
            return JS_STEP_YIELD;
        }
        DCHECK(s->off == s->text_len, "the text walk stopped short with a whole block still in it");
    }
    STEP_GOTO(hdr->stage, EN_FINISH, &hdr->get_phase, &hdr->str_phase, &hdr->num_phase, NULL);

    STEP_ARM(EN_FINISH);
    JS_FreeValue(ctx, cb_result);
    {
        size_t tag_bytes = (size_t)(s->tag_bits / 8u);
        JSValue result;

        if (decrypting) {
            uint32_t len = 0;
            const uint8_t *in = JS_GetBufferBytes(s->bytes, &len);

            DCHECK(in != NULL, "this algorithm's own copy of the ciphertext is detached");
            /* §29.4.2 step 5's "Let tag be the last tagLength bits of ciphertext", then step 8: "If the result
               of the algorithm is the indication of inauthenticity, \"FAIL\": throw an OperationError."
               A TAG MISMATCH IS THE PAGE'S DATA AND NEVER THIS ENGINE'S LOGIC, so it is a refusal. The
               plaintext already written into `out` is DISCARDED with it — §7.2 step 8 makes P the output only
               once T' = T, and handing back the bytes of a rejected decryption is a decryption oracle. */
            if (!aes_gcm_decrypt_verify(&s->gcm, in + s->text_len, tag_bytes)) {
                JS_ThrowDOMException(ctx, "OperationError", "%s",
                                     "the ciphertext did not authenticate under this key, iv and "
                                     "additionalData");
                return sc_reject(ctx, &s->p, presult);
            }
        } else {
            size_t out_n = 0;
            uint8_t *out = JS_GetArrayBuffer(ctx, &out_n, s->out);

            DCHECK(out != NULL && out_n == (size_t)s->text_len + tag_bytes,
                   "§29.4.1 step 7's C | T does not fill the buffer this algorithm sized for it");
            /* NOT `out != NULL || out_n == 0` HERE, which is the sibling walk's admission and would be wrong
               at this site: §7.1's `t` is at least four bytes, so an encrypt output is never empty and a NULL
               here is this engine's defect rather than a page's value. The two asserts differ because the two
               quantities do, not by oversight. */
            /* §7.1 steps 4-6 produce T, written straight after C — which IS step 7's "ciphertext be equal to
               C | T, where '|' denotes concatenation". */
            aes_gcm_encrypt_finish(&s->gcm, out + s->text_len, tag_bytes);
        }
        /* §14.3.1 step 13: "Let result be the result of creating an ArrayBuffer in realm, containing
           ciphertext." The realm is THIS one — a C member runs in the realm that defined it, which is the realm
           whose prototype carries this member, which is step 5's relevant realm of `this`. */
        result = s->out;
        s->out = JS_UNDEFINED;
        if (s->unknown) {
            JSValue example = s->has_example ? result : JS_UNDEFINED;

            DCHECK(concolic_is(s->src),
                   "the result was recorded as derived from unknown external input and the source held is not "
                   "a concolic — concolic_builtin_hook declines a known operand, so the two halves of that one "
                   "fact have come apart");
            if (!s->has_example)
                JS_FreeValue(ctx, result);
            result = concolic_builtin_hook(ctx, s->src, op, example);
            DCHECK(!JS_IsUninitialized(result),
                   "an argument was recorded as unknown external input and the derivation declined it — the "
                   "two are one fact read at two stages, and they have come apart");
        }
        return sc_resolve(ctx, &s->p, result, presult);
    }
}

static const IdlStepDecl EN_DECL = {
    en_step, sizeof(EnState), en_visit, NULL,
    "Web Cryptography §14.3.1 encrypt / §14.3.2 decrypt", EN_STEPS,
    /* catches_abrupt: step 3 REJECTS for every error normalizing an algorithm produced, and a `name`, `iv`,
       `additionalData` or `tagLength` accessor that throws after suspending is one of them. */
    1
};

/* ---- §14.3.9 The importKey method -------------------------------------------------------------------------- */

/* §14.3.9's steps and the registered operation's meet in one machine because the method's step 9 is "Let
 * result be the CryptoKey object that results from performing the import key operation specified by
 * normalizedAlgorithm" — so the method normalizes, hands over, and applies its own steps 10-12 to what comes
 * back. The split between the files is exactly that sentence: everything about WHAT A KEY OF ONE ALGORITHM IS
 * lives in that algorithm's own component — core/crypto/hmac.c for §31, core/crypto/aes_gcm_key.c for §29 —
 * and everything about how a promise is settled and which slots §14.3.9 itself writes lives here.
 *
 * §18.4.4's MEMBER WALK IS WHY THERE ARE SIX READING STAGES AND NOT ONE, AND WHY FIVE OF THEM ARE ONE ROW'S.
 * Step 9 builds "a list consisting of the IDL dictionary type desiredType and all of desiredType's inherited
 * dictionaries, in order from least to most derived", and step 10 walks each dictionary's members "in order" —
 * so for `HmacImportParams : Algorithm` that is Algorithm's `name`, then HmacImportParams' `hash` and
 * `length`. EVERY ONE OF THOSE IS A READ OF THE PAGE'S OBJECT, one accessor or Proxy trap away from the page's
 * own code, so each is a REQUEST that can suspend and not a JS_GetPropertyStr. The ORDER is observable in three
 * lines of script: an algorithm object whose `name` getter logs and whose `hash` getter throws tells you which
 * ran.
 *
 * WHICH DICTIONARY THAT IS COMES FROM THE REGISTRATION AND NOT FROM THIS METHOD, so the walk is a different
 * LENGTH per row rather than the same six stages with some of them idle. §31.2 gives HMAC's importKey row the
 * Parameters `HmacImportParams`; §29.2 gives AES-GCM's the Parameters `None`, and §18.3 Specification
 * Conventions says that column "will contain the IDL type to use for algorithm normalization for that
 * operation" — so an AES-GCM import's desiredType is the base Algorithm, its member walk is the `name` step 2
 * already read, and IK_SELECT transfers straight to IK_DONE. The five stages between them are HmacImportParams'
 * and are named for it.
 *
 * AND `hash` IS NORMALIZED RECURSIVELY, which is what makes it two stages of its own. Step 10's per-member
 * dispatch says so by type: "If member is of the type HashAlgorithmIdentifier: Set the dictionary member on
 * normalizedAlgorithm with key name key to the result of normalizing an algorithm, with the alg set to idlValue
 * and the op set to \"digest\"". So `{name:"HMAC", hash:"SHA-256"}` and `{name:"HMAC", hash:{name:"SHA-256"}}`
 * are the same algorithm, and the second reads a `name` off the page's inner object. */

static const char IK_FORK_OP[]      = "SubtleCrypto.importKey/normalizeAlgorithm";
static const char IK_FORK_OP_HASH[] = "SubtleCrypto.importKey/normalizeAlgorithm/hash";

/* §18.4.4's `registeredAlgorithms` FOR THE "importKey" OPERATION — the name each chapter's Registration
   section states: §31.2's "The recognized algorithm name for this algorithm is "HMAC"." and §29.2's, which
   says the same of "AES-GCM". See the sign/verify machine's note on §18.5.1 for why an empty registry
   elsewhere is conformant.
   THE ORDER IS THE FORK'S NUMBERING AND NOT THE STANDARD'S CHAPTER ORDER, exactly as SD_REGISTERED's is:
   step_fork_run's rule is that outcome 0 is the one a run with no forking policy takes, and HMAC is outcome 0
   because it was the only row before AES-GCM joined it — so a recipe recorded against this operation still
   names the arm it named when it was written, and the arm that MOVED is the NotSupportedError one, which a
   candidate re-fire must not be diverted onto in either numbering.
   THE NAMES ARE HERE, WHICH IS THE OPPOSITE OF SD_REGISTERED's ARRANGEMENT AND IS THE SAME RULE READ AGAINST A
   DIFFERENT TREE. secure_hash_name exists because the four members of that enum are named in three places and
   one statement of the four is what stops them drifting. These two are named nowhere a host can reach, and the
   other sites that write them are writing a DIFFERENT SENTENCE of the standard: hmac.c puts "HMAC" in §31.4's
   `name` attribute at §31.6.4's step 12 and aes_gcm_key.c puts "AES-GCM" in §27.4's at §29.4.4's step 6. A
   second column of strings here would be a copy; those are not. */
typedef enum { IK_ALG_HMAC = 0, IK_ALG_AES_GCM } IkAlgorithm;
static const char *const IK_REGISTERED[] = { "HMAC", "AES-GCM" };
#define IK_REGISTERED_N ((int)COUNTOF(IK_REGISTERED))
/* The one outcome past the registered rows: §18.4.4's "Otherwise: Return a new NotSupportedError". */
#define IK_FORK_OUTCOMES (IK_REGISTERED_N + 1)
/* THE TABLE AND THE ENUM ARE ONE FACT, because the fork answers with a POSITION IN THIS TABLE and every branch
   below reads that position as an IkAlgorithm. A row added without its enumerator would leave the new arm
   reading as the row before it — a real registered algorithm performing another one's import — which is the
   one way these two can disagree and the one thing no runtime check would see. */
_Static_assert(IK_REGISTERED_N == (int)IK_ALG_AES_GCM + 1,
               "IK_REGISTERED and IkAlgorithm have come apart — the fork's arm index is read as an "
               "IkAlgorithm, so every row of the table needs its enumerator");

#define IK_STAGES(X)                                                                                          \
    X(IK_NAME, "Web Cryptography §18.4.4 normalizing an algorithm step 2 (Get(alg, \"name\") for the "         \
               "importKey operation)")                                                                        \
    X(IK_NAME_STR, "Web Cryptography §18.4.4 normalizing an algorithm step 2 (converting alg[\"name\"] to its " \
                   "DOMString)")                                                                              \
    X(IK_SELECT, "Web Cryptography §18.4.4 normalizing an algorithm step 5 (the case-insensitive lookup of "   \
                 "algName in the importKey operation's registeredAlgorithms)")                                 \
    X(IK_HASH, "Web Cryptography §18.4.4 normalizing an algorithm step 10 (the per-member walk reaching "      \
               "§31.3 HmacImportParams' required `hash`, whose HashAlgorithmIdentifier arm normalizes with "   \
               "op set to \"digest\")")                                                                        \
    X(IK_HASH_NAME, "Web Cryptography §18.4.4 normalizing an algorithm step 2, recursively (Get(hash, "        \
                    "\"name\") when §31.3's `hash` member is an object rather than a DOMString)")              \
    X(IK_HASH_SELECT, "Web Cryptography §18.4.4 normalizing an algorithm step 5, recursively (the lookup of "  \
                      "the inner hash's name in the digest operation's registeredAlgorithms)")                 \
    X(IK_LENGTH, "Web Cryptography §18.4.4 normalizing an algorithm step 10 (the per-member walk reaching "    \
                 "§31.3 HmacImportParams' optional `length`)")                                                 \
    X(IK_LENGTH_NUM, "Web IDL §3.2.4.9 Abstract operations' ConvertToInt under §3.3.6 [EnforceRange], over "   \
                     "§31.3's `length` member")                                                               \
    X(IK_DONE, "Web Cryptography §14.3.9 steps 9-15 (perform §31.6.4 HMAC Import Key, apply steps 10-12's "    \
               "slots, and resolve promise with the key)")
enum { IDL_STEP_STAGE_BASE(IK_STAGES) IK_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const IK_STEPS[] = { IK_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    ScPromise p;
    JSValue   name_v;      /* alg["name"], then its DOMString, or the concolic itself (owned) */
    JSValue   hash_v;      /* §31.3's `hash` as read, then the inner name (owned) */
    JSValue   len_v;       /* §31.3's `length` as read (owned) */
    uint32_t  usages;      /* §9's normalized value of the usages list, as a CryptoKeyUsage mask */
    uint32_t  length;      /* §31.3's `length` after §3.2.4.9's conversion, in bits */
    uint8_t   hash;        /* the SecureHashAlgorithm the inner normalization selected */
    uint8_t   alg;         /* the IkAlgorithm §18.4.4 step 5's lookup selected */
    uint8_t   has_length;
    uint8_t   extractable;
} IkState;

static void ik_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    IkState *s = st;

    sc_promise_visit(ctx, &s->p, v);
    if (!s->p.started)
        return;
    v->val(ctx, &s->name_v);
    v->val(ctx, &s->hash_v);
    v->val(ctx, &s->len_v);
}

/* §14.1 Data Types' `enum KeyUsage`, IN THE ORDER crypto_key.h's CryptoKeyUsage BITS ARE DECLARED — the index
 * into this list IS the bit, which is what lets ONE list be both the §3.2.18 Enumeration types value list the
 * `sequence<KeyUsage>` position declares and the §9 Terminology mapping the walk below performs. It was two
 * lists, in two files, that had to stay in the same order with nothing saying so.
 * §14.1's own text writes the values in this order, so the list is the IDL's and the bit assignment reads off
 * it rather than the other way round. */
/* §14.1's `enum KeyUsage`, which core/crypto/crypto_key.h now states beside the BITS it indexes — see there
   for why one list serves the §3.2.18 conversion and §31.6.4 the jwk arm's step 8's `key_ops` comparison. */
IDL_ENUM_VALUES_EXTERN(CRYPTO_KEY_USAGE_NAMES, "encrypt", "decrypt", "sign", "verify", "deriveKey",
                       "deriveBits", "wrapKey", "unwrapKey");

/* Web Cryptography API §9 Terminology's "normalized value of a usages list usages", whose result "shall be
 * the usage intersection of usages and a sequence containing all recognized key usage values" — and §9's
 * usage intersection of two sequences is "a sequence containing each recognized key usage value that appears
 * in both a and b, in the order listed in the list of recognized key usage values". The second half used to
 * stand here as "the list of recognized key usage values", which is the name of the ORDERING §9 intersects
 * in, not the sequence it intersects WITH — one word apart and a different operand. In this engine that is
 * a mask over each recognized value that appears in both, in
 * the order that list gives them. As a mask, that is exactly a set of bits, which is why §13.3's [[usages]] is
 * one (crypto_key.h states the argument in full).
 *
 * THE INTERSECTION IS AN IDENTITY HERE AND THAT IS A FACT ABOUT THE TYPE, NOT A SHORTCUT. §14.3.9's
 * `sequence<KeyUsage> keyUsages` is declared IDL_SEQUENCE_ENUM over this very list, so Web IDL §3.2.18 step 2
 * has already refused every string that is not a recognized usage — with a TypeError, at the position, before
 * this member's body exists. Every element that arrives is therefore in both operands, and §9's intersection
 * has nothing to drop. THIS WALK USED TO PERFORM THAT REFUSAL, which was the right answer at the wrong layer:
 * the declared types had no row for a sequence whose element type is an enumeration, so the position was
 * IDL_SEQUENCE_DOMSTRING and the check was a body's private copy of a rule every enumeration has. The
 * observable difference the row bought is ORDER — §3.2.21.1 Creating a sequence from an iterable converts each
 * element inside the repeat loop, so a bogus usage at index 0 now throws before index 1 is pulled from the
 * page's iterator, where this walk pulled the whole list first and judged it afterwards.
 *
 * SO THE ONLY THING THAT CAN FAIL HERE IS THIS ENGINE'S OWN INVARIANT, and it is asserted rather than
 * reported: a name this list does not hold means the declaration and this list have drifted apart, which is
 * impossible while they ARE one list. */
static void sc_usages_normalize(JSContext *ctx, JSValueConst list, uint32_t *out)
{
    uint32_t n = 0, i;
    JSValue len_v;

    *out = 0;
    DCHECK(JS_IsArray(list), "§3.2.21's sequence conversion did not hand this member an Array — the position "
                             "is declared IDL_SEQUENCE_ENUM and that conversion builds one");
    len_v = JS_GetPropertyStr(ctx, list, "length");
    JS_ToUint32(ctx, &n, len_v);
    JS_FreeValue(ctx, len_v);
    for (i = 0; i < n; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, list, i);
        const char *nm = JS_ToCString(ctx, e);
        int k;

        JS_FreeValue(ctx, e);
        CHECK(nm != NULL, "an element of a keyUsages sequence could not be read back as UTF-8 — §3.2.21's "
                          "conversion already produced a DOMString for every element");
        for (k = 0; CRYPTO_KEY_USAGE_NAMES[k]; k++)
            if (strcmp(nm, CRYPTO_KEY_USAGE_NAMES[k]) == 0) break;
        /* ALWAYS FATAL, and it is a CHECK rather than a DCHECK because `k` is LOAD-BEARING IN RELEASE: the
           shift below builds §13.3's [[usages]], which is the authorization every later operation asks (§14.3.3
           step 10's "does not contain an entry that is \"sign\""). A `k` that walked off this list would set a
           bit outside CRYPTO_KEY_USAGES_ALL in the one build where nothing checked, and a key would carry a
           permission no page asked for. */
        CHECK(CRYPTO_KEY_USAGE_NAMES[k] != NULL,
              "a keyUsages element is not one of §14.1's KeyUsage values — the position is declared "
              "IDL_SEQUENCE_ENUM over THIS list, so Web IDL §3.2.18 step 2 refused every other string before "
              "this body ran");
        JS_FreeCString(ctx, nm);
        *out |= 1u << k;
    }
}

static int ik_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                   JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    IkState *s = st;
    /* §14.3.9 step 1: "Let format, algorithm, extractable and usages, be the format, algorithm, extractable
       and keyUsages parameters passed to the importKey() method, respectively." */
    JSValueConst format_v = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSValueConst key_data = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValueConst alg      = argc > 2 ? argv[2] : JS_UNDEFINED;
    int r;

    *presult = JS_UNDEFINED;

    if (!s->p.started) {
        s->name_v = s->hash_v = s->len_v = JS_UNDEFINED;
        s->usages = 0;
        s->length = 0;
        s->hash = (uint8_t)SECURE_HASH_SHA256;
        s->alg = (uint8_t)IK_ALG_HMAC;
        s->has_length = 0;
        s->extractable = 0;
        sc_promise_begin(ctx, &s->p);
        DCHECK(argc >= 5, "§14.3.9's importKey ran with fewer than its five declared arguments — Web IDL §3.6 "
                          "step 5 refuses that in the prologue and §3.7.7 turns the refusal into a rejection");
        s->extractable = (uint8_t)(JS_ToBool(ctx, argc > 3 ? argv[3] : JS_UNDEFINED) != 0);
        /* §9's NORMALIZED VALUE, AS A MASK. It cannot fail: the position is declared IDL_SEQUENCE_ENUM, so Web
           IDL §3.2.18 refused every string that is not a KeyUsage during the argument conversion — which §3.6
           runs LEFT TO RIGHT and finishes before this body's step 1, so a bogus usage is a TypeError with none
           of the algorithm object's getters having run. */
        sc_usages_normalize(ctx, argc > 4 ? argv[4] : JS_UNDEFINED, &s->usages);
    }
    DCHECK(!JS_IsException(cb_result) || hdr->stage == IK_NAME || hdr->stage == IK_NAME_STR ||
               hdr->stage == IK_HASH || hdr->stage == IK_HASH_NAME || hdr->stage == IK_LENGTH ||
               hdr->stage == IK_LENGTH_NUM,
           "§14.3.9 was delivered an abrupt completion at a stage that parks on no request able to throw — the "
           "member reads and their coercions are the only stages that run the page's code");

    STEP_DISPATCH(IK_STAGES, hdr->stage, "Web Cryptography §14.3.9 importKey(format, keyData, algorithm, "
                                         "extractable, keyUsages)", JS_STEP_ABRUPT);

    STEP_ARM(IK_NAME);
    if (JS_IsString(alg) || concolic_is(alg)) {
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        s->name_v = JS_DupValue(ctx, alg);
    } else {
        r = step_getprop_run(ctx, hdr, alg, g_atom_name, cb_result, &s->name_v, out_cb, out_argc);
        if (r > 0) return r;
        if (r < 0) return sc_reject(ctx, &s->p, presult);
        cb_result = JS_UNDEFINED;
    }
    STEP_GOTO(hdr->stage, IK_NAME_STR, &hdr->get_phase, &hdr->str_phase, &hdr->num_phase, NULL);

    STEP_ARM(IK_NAME_STR);
    if (JS_IsUndefined(s->name_v)) {
        JS_FreeValue(ctx, cb_result);
        JS_ThrowTypeError(ctx, "%s", "the algorithm passed to SubtleCrypto.importKey has no `name`, which the "
                                     "Algorithm dictionary declares as a required member");
        return sc_reject(ctx, &s->p, presult);
    }
    if (!JS_IsString(s->name_v) && !concolic_is(s->name_v)) {
        JSValue str;

        r = step_tostring_run(ctx, hdr, s->name_v, cb_result, &str, out_cb, out_argc);
        if (r > 0) return r;
        if (r < 0) return sc_reject(ctx, &s->p, presult);
        JS_FreeValue(ctx, s->name_v);
        s->name_v = str;
    } else {
        JS_FreeValue(ctx, cb_result);
    }
    cb_result = JS_UNDEFINED;
    STEP_GOTO(hdr->stage, IK_SELECT, &hdr->get_phase, &hdr->str_phase, &hdr->num_phase, NULL);

    STEP_ARM(IK_SELECT);
    JS_FreeValue(ctx, cb_result);
    cb_result = JS_UNDEFINED;
    if (concolic_is(s->name_v)) {
        int arm = 0;

        r = step_fork_run(ctx, hdr, s->name_v, IK_FORK_OP, IK_FORK_OUTCOMES, JS_OUTCOME_REAL_UNSTATED, &arm);
        if (r > 0) return r;
        DCHECK(arm >= 0 && arm < IK_FORK_OUTCOMES,
               "§18.4.4's registry fork answered with an outcome it did not declare");
        if (arm == IK_REGISTERED_N) {
            JS_ThrowDOMException(ctx, "NotSupportedError", "%s",
                                 "the algorithm named is not a registered `importKey` algorithm");
            return sc_reject(ctx, &s->p, presult);
        }
        s->alg = (uint8_t)arm;
    } else {
        const char *nm = JS_ToCString(ctx, s->name_v);
        int i;

        CHECK(nm != NULL, "§18.4.4's algName could not be read back as UTF-8");
        for (i = 0; i < IK_REGISTERED_N; i++)
            if (sd_name_matches(nm, IK_REGISTERED[i])) break;
        if (i == IK_REGISTERED_N) {
            JS_ThrowDOMException(ctx, "NotSupportedError", "'%s' is not a registered `importKey` algorithm",
                                 nm);
            JS_FreeCString(ctx, nm);
            return sc_reject(ctx, &s->p, presult);
        }
        JS_FreeCString(ctx, nm);
        s->alg = (uint8_t)i;
    }
    /* §18.4.4 step 9's `dictionaries` — "a list consisting of the IDL dictionary type desiredType and all of
       desiredType's inherited dictionaries, in order from least to most derived" — AND WHICH DICTIONARY THAT
       IS, IS WHAT THE ROW JUST SELECTED DECIDES. §31.2 gives HMAC's importKey row the type HmacImportParams,
       so the five stages below walk its `hash` and its `length`; §29.2 gives AES-GCM's the type `None`, whose
       desiredType is the base Algorithm and whose only member is the `name` step 2 has already read.
       SO THE SKIP IS THE MEMBER WALK BEING SHORTER AND NOT AN OPTIMISATION, AND IT IS OBSERVABLE: an algorithm
       object whose `hash` getter logs tells a page which of the two rows it named, and `importKey("raw", buf,
       "AES-GCM", true, ["encrypt"])` — §18.4.4's DOMString arm, an Algorithm with a name and nothing else —
       resolves where the same call naming HMAC is a TypeError for a required member it has no object to find.
       STEP_JUMP RATHER THAN A FALL-THROUGH, because the stage list is in ALGORITHM order and the arms this row
       does not take lie between IK_SELECT and IK_DONE. It crosses no work at all — those stages are not steps
       of this algorithm — which is the one condition quickjs-step.h puts on the transfer. */
    if ((IkAlgorithm)s->alg != IK_ALG_HMAC) {
        STEP_GOTO(hdr->stage, IK_DONE, &hdr->get_phase, &hdr->str_phase, &hdr->num_phase, NULL);
        STEP_JUMP(IK_DONE);
    }
    STEP_GOTO(hdr->stage, IK_HASH, &hdr->get_phase, &hdr->str_phase, &hdr->num_phase, NULL);

    STEP_ARM(IK_HASH);
    /* §31.3's `hash` is `required`, so §3.2.17's dictionary conversion is what refuses an absent one — asked
       below, after the read, because `undefined` IS absent for a dictionary member and the read is what tells
       them apart. The algorithm may itself be a bare DOMString, in which case §18.4.4's string arm made an
       Algorithm dictionary with only a `name` and there is no `hash` to find. */
    if (concolic_is(alg)) {
        /* UNKNOWN EXTERNAL INPUT IS NOT AN OBJECT TO READ A MEMBER OFF — it stands for whatever the page was
           given, so every member of it is unknown too and the inner normalization's own fork is what decides
           which hash it names. Answering `undefined` here would be the concrete-undefined that buries a
           branch, which is the defect §Attacker-sources names by that name. */
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        s->hash_v = JS_DupValue(ctx, alg);
    } else if (JS_IsString(alg)) {
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        s->hash_v = JS_UNDEFINED;
    } else {
        r = step_getprop_run(ctx, hdr, alg, g_atom_hash, cb_result, &s->hash_v, out_cb, out_argc);
        if (r > 0) return r;
        if (r < 0) return sc_reject(ctx, &s->p, presult);
        cb_result = JS_UNDEFINED;
    }
    if (JS_IsUndefined(s->hash_v)) {
        JS_ThrowTypeError(ctx, "%s", "the algorithm passed to SubtleCrypto.importKey has no `hash`, which "
                                     "HmacImportParams declares as a required member");
        return sc_reject(ctx, &s->p, presult);
    }
    STEP_GOTO(hdr->stage, IK_HASH_NAME, &hdr->get_phase, &hdr->str_phase, &hdr->num_phase, NULL);

    STEP_ARM(IK_HASH_NAME);
    /* THE RECURSIVE NORMALIZATION, whose DOMString arm is the name itself and whose object arm reads a `name`
       off the page's inner object — the same two arms as the outer one, one level down. */
    if (JS_IsString(s->hash_v) || concolic_is(s->hash_v)) {
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
    } else if (JS_IsObject(s->hash_v)) {
        JSValue inner = JS_UNDEFINED;

        r = step_getprop_run(ctx, hdr, s->hash_v, g_atom_name, cb_result, &inner, out_cb, out_argc);
        if (r > 0) return r;
        if (r < 0) return sc_reject(ctx, &s->p, presult);
        cb_result = JS_UNDEFINED;
        JS_FreeValue(ctx, s->hash_v);
        s->hash_v = inner;
        if (JS_IsUndefined(s->hash_v)) {
            JS_ThrowTypeError(ctx, "%s", "the `hash` of the algorithm passed to SubtleCrypto.importKey has no "
                                         "`name`, which the Algorithm dictionary declares as a required "
                                         "member");
            return sc_reject(ctx, &s->p, presult);
        }
    } else {
        /* A primitive that is neither a string nor unknown — §18.4.4's object arm converts it to an Algorithm
           dictionary, and §3.2.17 says a non-object that is not null or undefined is a TypeError. */
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        JS_ThrowTypeError(ctx, "%s", "the `hash` of the algorithm passed to SubtleCrypto.importKey is neither "
                                     "a string nor an object");
        return sc_reject(ctx, &s->p, presult);
    }
    if (!JS_IsString(s->hash_v) && !concolic_is(s->hash_v)) {
        JSValue str;

        r = step_tostring_run(ctx, hdr, s->hash_v, cb_result, &str, out_cb, out_argc);
        if (r > 0) return r;
        if (r < 0) return sc_reject(ctx, &s->p, presult);
        JS_FreeValue(ctx, s->hash_v);
        s->hash_v = str;
    }
    cb_result = JS_UNDEFINED;
    STEP_GOTO(hdr->stage, IK_HASH_SELECT, &hdr->get_phase, &hdr->str_phase, &hdr->num_phase, NULL);

    STEP_ARM(IK_HASH_SELECT);
    JS_FreeValue(ctx, cb_result);
    cb_result = JS_UNDEFINED;
    if (concolic_is(s->hash_v)) {
        int arm = 0;

        /* THE INNER LOOKUP IS THE DIGEST OPERATION'S REGISTRY, which is §32.2's four rows plus the
           NotSupportedError — the identical fork §14.3.5's SD_SELECT declares, over a different operation
           string so a replay can tell the two asks apart. */
        r = step_fork_run(ctx, hdr, s->hash_v, IK_FORK_OP_HASH, SD_FORK_OUTCOMES, JS_OUTCOME_REAL_UNSTATED,
                          &arm);
        if (r > 0) return r;
        DCHECK(arm >= 0 && arm < SD_FORK_OUTCOMES,
               "§18.4.4's registry fork answered with an outcome it did not declare");
        if (arm == SD_REGISTERED_N) {
            JS_ThrowDOMException(ctx, "NotSupportedError", "%s",
                                 "the inner hash named is not a registered `digest` algorithm");
            return sc_reject(ctx, &s->p, presult);
        }
        s->hash = (uint8_t)SD_REGISTERED[arm];
    } else {
        const char *nm = JS_ToCString(ctx, s->hash_v);
        int i;

        CHECK(nm != NULL, "the inner hash's algName could not be read back as UTF-8");
        for (i = 0; i < SD_REGISTERED_N; i++)
            if (sd_name_matches(nm, secure_hash_name(SD_REGISTERED[i]))) break;
        if (i == SD_REGISTERED_N) {
            JS_ThrowDOMException(ctx, "NotSupportedError", "'%s' is not a registered `digest` algorithm", nm);
            JS_FreeCString(ctx, nm);
            return sc_reject(ctx, &s->p, presult);
        }
        JS_FreeCString(ctx, nm);
        s->hash = (uint8_t)SD_REGISTERED[i];
    }
    STEP_GOTO(hdr->stage, IK_LENGTH, &hdr->get_phase, &hdr->str_phase, &hdr->num_phase, NULL);

    STEP_ARM(IK_LENGTH);
    if (concolic_is(alg)) {
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        s->len_v = JS_DupValue(ctx, alg);
    } else if (JS_IsString(alg)) {
        /* §18.4.4's DOMString arm builds "a new Algorithm dictionary whose name attribute is alg" and nothing
           else, so there is no `length` member to read and the optional one is ABSENT. */
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        s->len_v = JS_UNDEFINED;
    } else {
        r = step_getprop_run(ctx, hdr, alg, g_atom_length, cb_result, &s->len_v, out_cb, out_argc);
        if (r > 0) return r;
        if (r < 0) return sc_reject(ctx, &s->p, presult);
        cb_result = JS_UNDEFINED;
    }
    STEP_GOTO(hdr->stage, IK_LENGTH_NUM, &hdr->get_phase, &hdr->str_phase, &hdr->num_phase, NULL);

    STEP_ARM(IK_LENGTH_NUM);
    /* §3.2.17: for a dictionary member, `undefined` IS absence — and `length` carries no `= …`, so an absent
       one does not exist on normalizedAlgorithm at all. That is the POSITIVE statement §31.6.4 steps 1 and 8
       both read, which is why `has_length` is a field rather than a zero. */
    if (JS_IsUndefined(s->len_v)) {
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        s->has_length = 0;
    } else if (concolic_is(s->len_v)) {
        JS_FreeValue(ctx, cb_result);
        DFAIL("§31.6.4 step 1 and step 8 BRANCH on HmacImportParams' `length`, and this one is unknown external "
              "input — the zero test, the greater-than test and the eight-below test are three feasible "
              "outcomes plus the arm that sets length, and deciding them against a shape would delete every "
              "arm but one. Declare a step_fork_run over those outcomes here, named "
              "\"SubtleCrypto.importKey/HmacImportParams.length\", exactly as IK_SELECT declares §18.4.4 step "
              "5's; a comparison in C is what must NOT appear at this site");
        return JS_STEP_ABRUPT;
    } else {
        double d = 0.0;

        r = step_todouble_run(ctx, hdr, s->len_v, cb_result, &d, out_cb, out_argc);
        if (r > 0) return r;
        if (r < 0) return sc_reject(ctx, &s->p, presult);
        cb_result = JS_UNDEFINED;
        /* §3.3.6 [EnforceRange]'s ARM of §3.2.4.9 Abstract operations' ConvertToInt: a non-finite value, or one
           whose integer part falls outside the type's range, is a TypeError rather than the modulo an
           unadorned `unsigned long` would take. `length` is 32-bit and unsigned. */
        if (!isfinite(d)) {
            JS_ThrowTypeError(ctx, "%s", "the `length` of the algorithm passed to SubtleCrypto.importKey is "
                                         "not a finite number");
            return sc_reject(ctx, &s->p, presult);
        }
        d = trunc(d);   /* §3.2.4.9's IntegerPart: the value truncated toward zero */
        if (d < 0 || d > 4294967295.0) {
            JS_ThrowTypeError(ctx, "%s", "the `length` of the algorithm passed to SubtleCrypto.importKey is "
                                         "outside the range of an unsigned long");
            return sc_reject(ctx, &s->p, presult);
        }
        s->length = (uint32_t)d;
        s->has_length = 1;
    }
    STEP_GOTO(hdr->stage, IK_DONE, &hdr->get_phase, &hdr->str_phase, &hdr->num_phase, NULL);

    STEP_ARM(IK_DONE);
    JS_FreeValue(ctx, cb_result);
    {
        const char *format = JS_ToCString(ctx, format_v);
        JSValue key;

        CHECK(format != NULL, "§14.1's KeyFormat could not be read back as UTF-8 — the argument conversion "
                              "already checked it against the enumeration's four values");
        /* §14.3.9 STEP 4, BOTH ARMS, over the union the position now declares. The step is a pair of
           converses — jwk: "If the keyData parameter passed to the importKey() method is not a JsonWebKey
           dictionary, throw a TypeError"; otherwise: "If the keyData parameter passed to the importKey()
           method is a JsonWebKey dictionary, throw a TypeError" — so what it needs is which arm §3.2.25 took,
           and that is a fact the CONVERSION decided rather than one this body re-derives from a shape.
           WHICH IS WHY THE TEST IS THE BUFFER BRAND AND NOT `JS_IsObject`. §3.2.25's dictionary arm produces
           an engine-built plain object, so both arms arrive as objects and object-ness tells them apart from
           nothing; the buffer arm's value is the page's own ArrayBuffer, DataView or typed array, which is
           exactly what steps 6, 8 and 9 selected on. The same three clauses, read back.
           THIS BLOCK USED TO BE A ONE-WAY TYPEERROR and it was a WRONG ANSWER rather than a narrower one: the
           position was declared IDL_BUFFERSOURCE, so a JWK never survived the conversion to reach step 4 at
           all and every `importKey("jwk", {kty:"oct",k:"…"}, …)` was refused where a browser resolves. */
        {
            bool is_buffer = JS_IsArrayBuffer(key_data) || JS_IsDataView(key_data) ||
                             JS_GetTypedArrayType(key_data) >= 0;

            if ((strcmp(format, "jwk") == 0) == is_buffer) {
                JS_FreeCString(ctx, format);
                JS_ThrowTypeError(ctx, "%s", is_buffer
                                  ? "importKey was given a BufferSource for the \"jwk\" format, which takes "
                                    "a JsonWebKey dictionary"
                                  : "importKey was given a JsonWebKey dictionary for a format that takes a "
                                    "BufferSource");
                return sc_reject(ctx, &s->p, presult);
            }
        }
        /* §14.3.9 STEP 9: "Let result be the CryptoKey object that results from performing the import key
           operation specified by normalizedAlgorithm using keyData, algorithm, format, extractable and
           usages" — which is the whole of §31.6.4 HMAC Import Key or the whole of §29.4.4 AES-GCM Import Key,
           and the row §18.4.4 step 5 selected is what says which. Steps 11 and 12 — "Set the [[extractable]]
           internal slot of result to extractable" and "Set the [[usages]] internal slot of result to the
           normalized value of usages" — are the mint's two arguments rather than two writes after the fact,
           because a CryptoKey whose slots are filled in afterwards is a CryptoKey that briefly exists with the
           wrong ones. THE PARAMETERS ARE ONE ROW'S AND NOT THE METHOD'S: §29.2 registers AES-GCM's importKey
           with `None`, so there is no dictionary to hand it and the five stages that would have read one never
           ran. */
        switch ((IkAlgorithm)s->alg) {
        case IK_ALG_HMAC: {
            HmacImportParams params;

            params.hash = (SecureHashAlgorithm)s->hash;
            params.has_length = s->has_length != 0;
            params.length = s->length;
            key = hmac_import_key(ctx, format, key_data, &params, s->extractable != 0, s->usages);
            break;
        }
        case IK_ALG_AES_GCM:
            key = aes_gcm_import_key(ctx, format, key_data, s->extractable != 0, s->usages);
            break;
        default:
            /* UNREACHABLE BY CONSTRUCTION and asserted rather than defended: IK_REGISTERED is this engine's
               own table, the fork's range check refused an arm outside it and the concrete arm returned
               before assigning, so a value here is those three having come apart rather than anything a page
               said. */
            JS_FreeCString(ctx, format);
            DFAILF("§18.4.4 step 5 selected registry row %u, which IkAlgorithm does not name — the fork "
                   "declared IK_FORK_OUTCOMES over IK_REGISTERED and this row is in neither",
                   (unsigned)s->alg);
            /* RELEASE: the row cannot be performed, which is what §18.4.4's own Otherwise answers for a name
               it cannot resolve. Stated rather than left to whatever happened to be pending, because
               sc_reject settles with the live exception and an arm that throws nothing would settle with
               none — a rejected promise carrying no reason, which is the quiet return §Offensive-programming
               forbids wearing a rejection. */
            JS_ThrowDOMException(ctx, "NotSupportedError", "%s",
                                 "the algorithm named is not a registered `importKey` algorithm");
            return sc_reject(ctx, &s->p, presult);
        }
        JS_FreeCString(ctx, format);
        if (JS_IsException(key))
            return sc_reject(ctx, &s->p, presult);
        /* §14.3.9 STEP 10: "If the [[type]] internal slot of result is \"secret\" or \"private\" and usages is
           empty, then throw a SyntaxError." BOTH rows mint a "secret" key — §31.6.4 step 10 and §29.4.4 step
           4 each say so outright — so this is the empty-usages test for either, and it is the METHOD's step
           rather than the algorithm's, which is why it runs on what comes back and not inside the operation.
           IT READS THE MASK AND NOT THE KEY'S OWN SLOT, which is sound only while that is true of every row:
           the day a row mints a "public" key, this test is the one that has to read [[type]] back. */
        if (s->usages == 0) {
            JS_FreeValue(ctx, key);
            JS_ThrowDOMException(ctx, "SyntaxError", "%s",
                                 "a secret key must be imported with at least one usage");
            return sc_reject(ctx, &s->p, presult);
        }
        /* STEPS 13-15: queue the task, convert to an ECMAScript object in realm (a CryptoKey already is one),
           and resolve. */
        return sc_resolve(ctx, &s->p, key, presult);
    }
}

static const IdlStepDecl IK_DECL = {
    ik_step, sizeof(IkState), ik_visit, NULL,
    "Web Cryptography §14.3.9 importKey(format, keyData, algorithm, extractable, keyUsages)", IK_STEPS,
    /* catches_abrupt: §14.3.9 step 3 REJECTS for every error normalizing an algorithm produced, and each of
       the three member getters can throw after suspending. */
    1
};

/* ---- §14.3.10 The exportKey method ------------------------------------------------------------------------
 *
 * THE ONE METHOD OF §14.3 WITH NO ALGORITHM ARGUMENT, AND EVERY DIFFERENCE BELOW FOLLOWS FROM THAT. `Promise<
 * (ArrayBuffer or JsonWebKey)> exportKey(KeyFormat format, CryptoKey key)` declares an enumeration and an
 * interface, so §3.6's conversion settles BOTH operands before step 1 — there is no `Get(alg, "name")`, no
 * ToString, and no §18.4.4 member walk. The five reading stages the importKey machine needs do not exist here
 * and neither does a sixth: NOTHING IN STEPS 6-11 RUNS THE PAGE'S CODE, so the machine cannot suspend and has
 * exactly one stage.
 *
 * WHICH IS ALSO WHY STEP 6 IS NOT A FORK. §14.3.9's registry lookup is `step_fork_run` over a name the PAGE
 * supplied, which may be concolic and whose arms are worlds the solver must keep. §14.3.10 step 6 reads "the
 * name member of the [[algorithm]] internal slot of key" — a string THIS ENGINE wrote at §31.6.4 step 12, off
 * a record in an own slot the page cannot reach. There is one world, so a fork here would mint arms over a
 * value that is already concrete, which §Solver-half's concretize-on-pin forbids in the other direction.
 *
 * THE REGISTRY IS ONE ROW AND THAT IS CONFORMANT, on the same sentence the sign/verify machine rests on:
 * §18.5.1 states "there are no algorithms that conforming user agents are required to implement". §29.4.5
 * AES-GCM Export Key is the named next diff and is a ROW plus its own `alg` sub-step, not a change here — the
 * shared run of its jwk arm is already in core/crypto/jwk.c, diffed against §31.6.5's rather than assumed to
 * match it. WHAT ITS ABSENCE LOOKS LIKE: `exportKey("raw", aesGcmKey)` rejects with NotSupportedError from
 * step 6 where a browser resolves with the key's octets, and WebCryptoAPI/import_export/symmetric_importKey's
 * `runTests("AES-GCM")` round trip reports it while `runTests("HMAC")` passes.
 *
 * THE COMPARISON IS sd_name_matches AND NOT strcmp, which says something slightly stronger than this operand
 * needs and is deliberate: §18.4.4's identification is ASCII case-insensitive everywhere, and using the one
 * comparator that states it keeps step 6 reading as the same identification the other three methods perform.
 * The operand is engine-written, so the two answer alike for every key this engine can mint. */

#define XK_STAGES(X)                                                                                          \
    X(XK_DONE, "Web Cryptography §14.3.10 steps 6-11 (the registered-algorithm lookup over the key's own "     \
               "[[algorithm]], the [[extractable]] refusal, the export key operation, and the resolve)")
enum { IDL_STEP_STAGE_BASE(XK_STAGES) XK_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const XK_STEPS[] = { XK_STAGES(JS_STEP_STAGE_LABEL) NULL };

/* THE STATE IS THE PROMISE AND NOTHING ELSE, because every value this algorithm touches is read and spent
   inside one stage — there is no suspension point for anything to have to survive. */
typedef struct {
    ScPromise p;
} XkState;

static void xk_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    XkState *s = st;

    /* THE GUARD IS `started`, for the reason every sibling's visit states: a fork can land on a state whose
       prologue has not run, and a zeroed JSValue is the INTEGER 0 rather than a live value. */
    sc_promise_visit(ctx, &s->p, v);
}

static int xk_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                   JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    XkState *s = st;
    /* §14.3.10 step 1: "Let format and key be the format and key parameters passed to the exportKey() method,
       respectively." */
    JSValueConst format_v = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSValueConst key      = argc > 1 ? argv[1] : JS_UNDEFINED;

    *presult = JS_UNDEFINED;

    if (!s->p.started) {
        /* STEPS 3-4: "Let promise be a new Promise" and "Return promise and perform the remaining steps in
           parallel". Step 2's realm is THIS one — a C member runs in the realm that defined it. */
        sc_promise_begin(ctx, &s->p);
        DCHECK(argc >= 2, "§14.3.10's exportKey ran with fewer than its two declared arguments — Web IDL §3.6 "
                          "step 5 refuses that in the prologue and §3.7.7 turns the refusal into a rejection");
    }
    DCHECK(!JS_IsException(cb_result),
           "§14.3.10 was delivered an abrupt completion, and it parks on no request at all — the method takes "
           "an enumeration and an interface, both settled by §3.6's conversion before step 1, so there is "
           "nothing between the prologue and the resolve that can run a page's code");

    STEP_DISPATCH(XK_STAGES, hdr->stage, "Web Cryptography §14.3.10 exportKey(format, key)", JS_STEP_ABRUPT);

    STEP_ARM(XK_DONE);
    JS_FreeValue(ctx, cb_result);
    {
        const char *format = JS_ToCString(ctx, format_v);
        JSValue algorithm, name;
        const char *nm;
        bool known;
        JSValue result;

        CHECK(format != NULL, "§14.1's KeyFormat could not be read back as UTF-8 — the argument conversion "
                              "already checked it against the enumeration's four values");

        /* STEP 6: "If the name member of the [[algorithm]] internal slot of key does not identify a registered
           algorithm that supports the export key operation, then throw a NotSupportedError." */
        algorithm = crypto_key_algorithm(ctx, key);
        DCHECK(JS_IsObject(algorithm),
               "§14.3.10 step 6 read a key whose [[algorithm]] slot is not a dictionary — §13.3 declares the "
               "slot on every key and crypto_key_new is its only writer");
        name = JS_GetPropertyStr(ctx, algorithm, "name");
        JS_FreeValue(ctx, algorithm);
        nm = JS_ToCString(ctx, name);
        JS_FreeValue(ctx, name);
        CHECK(nm != NULL, "a CryptoKey's [[algorithm]] `name` could not be read back as UTF-8 — this engine "
                          "wrote it from the registration section of the chapter that minted the key");
        known = sd_name_matches(nm, "HMAC");
        if (!known) {
            /* THE REFUSAL IS THE STANDARD'S AND NOT AN ASSERT. A key of an algorithm this engine registers for
               `importKey` and not for `exportKey` is a state a PAGE reaches with two ordinary calls, so it
               takes step 6's own error — and the message names the key's algorithm rather than the format,
               because that is the operand this step refused. */
            JS_ThrowDOMException(ctx, "NotSupportedError",
                                 "'%s' is not a registered `exportKey` algorithm", nm);
            JS_FreeCString(ctx, nm);
            JS_FreeCString(ctx, format);
            return sc_reject(ctx, &s->p, presult);
        }
        JS_FreeCString(ctx, nm);

        /* STEP 7: "If the [[extractable]] internal slot of key is false, then throw an InvalidAccessError."
           AFTER STEP 6 AND NOT BEFORE IT, which is observable: a non-extractable key of an unregistered
           algorithm rejects with NotSupportedError and not InvalidAccessError, and a page that exports one
           reads which of the two it got. */
        if (!crypto_key_extractable(ctx, key)) {
            JS_ThrowDOMException(ctx, "InvalidAccessError", "%s",
                                 "this key was created non-extractable, so its key material cannot be "
                                 "exported");
            JS_FreeCString(ctx, format);
            return sc_reject(ctx, &s->p, presult);
        }

        /* STEP 8: "Let result be the result of performing the export key operation specified by the
           [[algorithm]] internal slot of key using key and format" — the whole of §31.6.5 HMAC Export Key,
           step 4's format dispatch and its NotSupportedError arm included. The split between the files is that
           sentence, exactly as it is for §14.3.9: what a key of one algorithm IS lives in that algorithm's own
           component, and what §14.3.10 itself does lives here. */
        result = hmac_export_key(ctx, format, key);
        JS_FreeCString(ctx, format);
        if (JS_IsException(result))
            return sc_reject(ctx, &s->p, presult);
        /* STEPS 9-11: queue the task, perform step 10's conversion, and resolve. BOTH OF STEP 10's ARMS ARE
           THE IDENTITY ON WHAT CAME BACK — see hmac.h: the engine's carrier for the raw arm's byte sequence is
           the ArrayBuffer the operation built, and for the jwk arm's dictionary the object core/crypto/jwk.c
           built, both in this realm. sc_resolve is the queue. */
        return sc_resolve(ctx, &s->p, result, presult);
    }
}

static const IdlStepDecl XK_DECL = {
    xk_step, sizeof(XkState), xk_visit, NULL,
    "Web Cryptography §14.3.10 exportKey(format, key)", XK_STEPS,
    /* catches_abrupt: ZERO, and it is the only one of these four that is. The flag exists for a machine that
       PARKS on a request the page can complete abruptly; this one parks on nothing, so there is no abrupt
       completion for it to catch and claiming otherwise would assert a rest point it does not have. */
    0
};

/* ---- §14.3.6 The generateKey method ------------------------------------------------------------------------
 *
 * THE ONLY METHOD OF §14.3 THAT MINTS A KEY OUT OF NOTHING, and every difference below follows from that. It
 * takes no CryptoKey and no BufferSource, so its whole input is `AlgorithmIdentifier algorithm`, a boolean and
 * a `sequence<KeyUsage>` — which means the six stages here are §18.4.4's normalization and nothing else, and
 * the algorithm's own work is one call.
 *
 * ITS SOURCE IS §10.1's STREAM AND THAT IS WHAT MADE IT WAIT. §29.4.3 step 3 says to "Generate an AES key of
 * length equal to the length member of normalizedAlgorithm", and this interface has no randomness of its own:
 * core/crypto/crypto.h's `crypto_random_bytes` is the route, drawing through the SAME object this realm's
 * `crypto` getter latches its draw position on, so a forked pair of flows above a generateKey draws from the
 * position the FORK was taken at rather than minting identical material twice. That header argues the whole of
 * it; a second stream is invisible downstream of the key, which is why the route is an entry and not a helper
 * each caller could reproduce.
 *
 * THE REGISTRY IS ONE ROW, AND THE ROW IS THE ONE REAL PAGES CALL. §18.5.1 states "there are no algorithms
 * that conforming user agents are required to implement", so a short registry is conformant; which row is here
 * was decided by reading the CALL SITES rather than §14.3's member list. Of this corpus's `crypto.subtle
 * .generateKey` calls, four name AES-GCM (two of them through a constant — `W.ALGORITHM="AES-GCM"` and
 * `ns="AES-GCM"` — which a literal grep does not see), three name ECDSA over P-256 and one names
 * RSASSA-PKCS1-v1_5; ZERO name HMAC. §31.6.3's HMAC row is therefore not the next diff even though HMAC is the
 * algorithm this component is deepest in: it would install an arm no site calls.
 *
 * WHAT EACH SITE DOES NEXT IS THE OTHER HALF OF THAT READING AND IT IS NOT UNIFORM. One AES-GCM site
 * (helixapp's) goes straight from the key to §14.3.1's encrypt, which is built, and COMPLETES. The other three
 * call §14.3.10's exportKey on what comes back, whose registry §14.3.10 step 6 answers over is one row and
 * that row is HMAC — so they reach a "NotSupportedError" one call later. That is a REFUSAL THE STANDARD
 * DEFINES rather than a gap wearing a resolved promise, and it is strictly further than those sites got
 * before, where the member's absence was a TypeError on the call itself. §29.4.5 AES-GCM Export Key is what
 * moves them, and the exportKey machine's own comment already names it.
 *
 * THE FEATURE DETECT THIS FLIPS, PRICED BEFORE IT WAS LANDED. meticulous.js — served into this corpus's
 * grafana mirror — carries `["decrypt","digest","encrypt","exportKey","generateKey","importKey","sign",
 * "verify"].every(n => typeof subtle[n] === "function")`, so installing this member takes it from 7/8 to 8/8
 * and flips a guard that was false. §NO-STUBS is explicit that an all-or-nothing guard flipped into an
 * incompletable branch is worse in BOTH arms, so what the branch CALLS was derived rather than assumed: its
 * two consumers are an SHA-1 wrapper and an SHA-256 wrapper, and the calls behind them are `digest`
 * ({name:"SHA-1"} / {name:"SHA-256"}), `importKey("raw", …, {name:"HMAC",hash:{name:"SHA-…"}}, false,
 * ["sign"])` and `sign({name:"HMAC",hash:…}, key, data)` — the keyed pair being what the AWS SigV4 signer's
 * `hmac(ctor, key, data)` reaches. All three are installed and all three are registered for those algorithms,
 * so the branch completes. The SHA-1 consumer is the sharper half: its false arm is `throw new
 * Error("SHA1 not supported")` with no fallback at all, so for that wrapper the guard being false was already
 * the dead end and any completion is an advance. */

static const char GK_FORK_OP[] = "SubtleCrypto.generateKey/normalizeAlgorithm";

/* §18.4.4's `registeredAlgorithms` FOR THE "generateKey" OPERATION — §29.2's "The recognized algorithm name
   for this algorithm is "AES-GCM"." The table and the enum are ONE fact for IK_REGISTERED's reason: the fork
   answers with a POSITION and every branch reads it as a GkAlgorithm. */
typedef enum { GK_ALG_AES_GCM = 0 } GkAlgorithm;
static const char *const GK_REGISTERED[] = { "AES-GCM" };
#define GK_REGISTERED_N ((int)COUNTOF(GK_REGISTERED))
/* The one outcome past the registered rows: §18.4.4's "Otherwise: Return a new NotSupportedError". */
#define GK_FORK_OUTCOMES (GK_REGISTERED_N + 1)
_Static_assert(GK_REGISTERED_N == (int)GK_ALG_AES_GCM + 1,
               "GK_REGISTERED and GkAlgorithm have come apart — the fork's arm index is read as a "
               "GkAlgorithm, so every row of the table needs its enumerator");

#define GK_STAGES(X)                                                                                          \
    X(GK_NAME, "Web Cryptography §18.4.4 normalizing an algorithm step 2 (Get(alg, \"name\") for the "        \
               "generateKey operation)")                                                                      \
    X(GK_NAME_STR, "Web Cryptography §18.4.4 normalizing an algorithm step 2 (converting alg[\"name\"] to "    \
                   "its DOMString)")                                                                          \
    X(GK_SELECT, "Web Cryptography §18.4.4 normalizing an algorithm step 5 (the case-insensitive lookup of "   \
                 "algName in the generateKey operation's registeredAlgorithms)")                               \
    X(GK_LENGTH, "Web Cryptography §18.4.4 normalizing an algorithm step 10 (the per-member walk reaching "    \
                 "§27.5 AesKeyGenParams' required `length`)")                                                  \
    X(GK_LENGTH_NUM, "Web IDL §3.2.4.9 Abstract operations' ConvertToInt under §3.3.6 [EnforceRange], over "   \
                     "§27.5's `length` member, whose declared type is `unsigned short`")                       \
    X(GK_DONE, "Web Cryptography §14.3.6 steps 8-12 (perform §29.4.3 AES-GCM Generate Key, apply step 9's "    \
               "empty-usages test and resolve promise with the key)")
enum { IDL_STEP_STAGE_BASE(GK_STAGES) GK_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const GK_STEPS[] = { GK_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    ScPromise p;
    JSValue   name_v;      /* alg["name"], then its DOMString, or the concolic itself (owned) */
    JSValue   len_v;       /* §27.5's `length` as read (owned) */
    uint32_t  usages;      /* §9's normalized value of the usages list, as a CryptoKeyUsage mask */
    uint32_t  length;      /* §27.5's `length` after §3.2.4.9's conversion, in bits */
    uint8_t   alg;         /* the GkAlgorithm §18.4.4 step 5's lookup selected */
    uint8_t   extractable;
} GkState;

static void gk_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    GkState *s = st;

    sc_promise_visit(ctx, &s->p, v);
    if (!s->p.started)
        return;
    v->val(ctx, &s->name_v);
    v->val(ctx, &s->len_v);
}

static int gk_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                   JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    GkState *s = st;
    /* §14.3.6 step 1: "Let algorithm, extractable and usages be the algorithm, extractable and keyUsages
       parameters passed to the generateKey() method, respectively." */
    JSValueConst alg = argc > 0 ? argv[0] : JS_UNDEFINED;
    int r;

    *presult = JS_UNDEFINED;

    if (!s->p.started) {
        s->name_v = s->len_v = JS_UNDEFINED;
        s->usages = 0;
        s->length = 0;
        s->alg = (uint8_t)GK_ALG_AES_GCM;
        s->extractable = 0;
        sc_promise_begin(ctx, &s->p);
        DCHECK(argc >= 3, "§14.3.6's generateKey ran with fewer than its three declared arguments — Web IDL "
                          "§3.6 step 5 refuses that in the prologue and §3.7.7 turns the refusal into a "
                          "rejection");
        s->extractable = (uint8_t)(JS_ToBool(ctx, argc > 1 ? argv[1] : JS_UNDEFINED) != 0);
        /* §9's NORMALIZED VALUE, AS A MASK — the same operation §14.3.9's step 12 names, which is why the
           walk is sc_ and not ik_. It cannot fail: the position is declared IDL_SEQUENCE_ENUM, so Web IDL
           §3.2.18 refused every string that is not a KeyUsage during the argument conversion. */
        sc_usages_normalize(ctx, argc > 2 ? argv[2] : JS_UNDEFINED, &s->usages);
    }
    DCHECK(!JS_IsException(cb_result) || hdr->stage == GK_NAME || hdr->stage == GK_NAME_STR ||
               hdr->stage == GK_LENGTH || hdr->stage == GK_LENGTH_NUM,
           "§14.3.6 was delivered an abrupt completion at a stage that parks on no request able to throw — the "
           "member reads and their coercions are the only stages that run the page's code");

    STEP_DISPATCH(GK_STAGES, hdr->stage, "Web Cryptography §14.3.6 generateKey(algorithm, extractable, "
                                         "keyUsages)", JS_STEP_ABRUPT);

    STEP_ARM(GK_NAME);
    if (JS_IsString(alg) || concolic_is(alg)) {
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        s->name_v = JS_DupValue(ctx, alg);
    } else {
        r = step_getprop_run(ctx, hdr, alg, g_atom_name, cb_result, &s->name_v, out_cb, out_argc);
        if (r > 0) return r;
        if (r < 0) return sc_reject(ctx, &s->p, presult);
        cb_result = JS_UNDEFINED;
    }
    STEP_GOTO(hdr->stage, GK_NAME_STR, &hdr->get_phase, &hdr->str_phase, &hdr->num_phase, NULL);

    STEP_ARM(GK_NAME_STR);
    if (JS_IsUndefined(s->name_v)) {
        JS_FreeValue(ctx, cb_result);
        JS_ThrowTypeError(ctx, "%s", "the algorithm passed to SubtleCrypto.generateKey has no `name`, which "
                                     "the Algorithm dictionary declares as a required member");
        return sc_reject(ctx, &s->p, presult);
    }
    if (!JS_IsString(s->name_v) && !concolic_is(s->name_v)) {
        JSValue str;

        r = step_tostring_run(ctx, hdr, s->name_v, cb_result, &str, out_cb, out_argc);
        if (r > 0) return r;
        if (r < 0) return sc_reject(ctx, &s->p, presult);
        JS_FreeValue(ctx, s->name_v);
        s->name_v = str;
    } else {
        JS_FreeValue(ctx, cb_result);
    }
    cb_result = JS_UNDEFINED;
    STEP_GOTO(hdr->stage, GK_SELECT, &hdr->get_phase, &hdr->str_phase, &hdr->num_phase, NULL);

    STEP_ARM(GK_SELECT);
    JS_FreeValue(ctx, cb_result);
    cb_result = JS_UNDEFINED;
    if (concolic_is(s->name_v)) {
        int arm = 0;

        r = step_fork_run(ctx, hdr, s->name_v, GK_FORK_OP, GK_FORK_OUTCOMES, JS_OUTCOME_REAL_UNSTATED, &arm);
        if (r > 0) return r;
        DCHECK(arm >= 0 && arm < GK_FORK_OUTCOMES,
               "§18.4.4's registry fork answered with an outcome it did not declare");
        if (arm == GK_REGISTERED_N) {
            JS_ThrowDOMException(ctx, "NotSupportedError", "%s",
                                 "the algorithm named is not a registered `generateKey` algorithm");
            return sc_reject(ctx, &s->p, presult);
        }
        s->alg = (uint8_t)arm;
    } else {
        const char *nm = JS_ToCString(ctx, s->name_v);
        int i;

        CHECK(nm != NULL, "§18.4.4's algName could not be read back as UTF-8");
        for (i = 0; i < GK_REGISTERED_N; i++)
            if (sd_name_matches(nm, GK_REGISTERED[i])) break;
        if (i == GK_REGISTERED_N) {
            JS_ThrowDOMException(ctx, "NotSupportedError", "'%s' is not a registered `generateKey` algorithm",
                                 nm);
            JS_FreeCString(ctx, nm);
            return sc_reject(ctx, &s->p, presult);
        }
        JS_FreeCString(ctx, nm);
        s->alg = (uint8_t)i;
    }
    STEP_GOTO(hdr->stage, GK_LENGTH, &hdr->get_phase, &hdr->str_phase, &hdr->num_phase, NULL);

    STEP_ARM(GK_LENGTH);
    /* §18.4.4 step 9's `dictionaries` for the row step 5 selected: §29.2 gives AES-GCM's generateKey row the
       Parameters type `AesKeyGenParams`, whose ONE member is `required [EnforceRange] unsigned short length`.
       `required`, so §3.2.17's dictionary conversion is what refuses an absent one — asked below, after the
       read, because `undefined` IS absence for a dictionary member and the read is what tells them apart. */
    if (concolic_is(alg)) {
        /* UNKNOWN EXTERNAL INPUT IS NOT AN OBJECT TO READ A MEMBER OFF — it stands for whatever the page was
           given, so `length` is unknown too and GK_LENGTH_NUM's own arm is where that is answered. */
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        s->len_v = JS_DupValue(ctx, alg);
    } else if (JS_IsString(alg)) {
        /* §18.4.4's DOMString arm builds "a new Algorithm dictionary whose name attribute is alg" and nothing
           else, so there is no `length` member to find — and `length` is REQUIRED, so `generateKey("AES-GCM",
           …)` is the required member's TypeError where the same spelling resolves for an importKey whose row
           takes `None`. The two members answer that string differently and both answers are their own row's. */
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        s->len_v = JS_UNDEFINED;
    } else {
        r = step_getprop_run(ctx, hdr, alg, g_atom_length, cb_result, &s->len_v, out_cb, out_argc);
        if (r > 0) return r;
        if (r < 0) return sc_reject(ctx, &s->p, presult);
        cb_result = JS_UNDEFINED;
    }
    if (JS_IsUndefined(s->len_v)) {
        JS_ThrowTypeError(ctx, "%s", "the algorithm passed to SubtleCrypto.generateKey has no `length`, which "
                                     "AesKeyGenParams declares as a required member");
        return sc_reject(ctx, &s->p, presult);
    }
    STEP_GOTO(hdr->stage, GK_LENGTH_NUM, &hdr->get_phase, &hdr->str_phase, &hdr->num_phase, NULL);

    STEP_ARM(GK_LENGTH_NUM);
    if (concolic_is(s->len_v)) {
        JS_FreeValue(ctx, cb_result);
        /* THE THREE-VALUE TEST §29.4.3 STEP 2 MAKES IS A BRANCH ON THIS OPERAND, and this one is unknown
           external input — 128, 192, 256 and the OperationError are four feasible outcomes, and deciding them
           against a shape would delete every world but one. Declare a step_fork_run over those outcomes here,
           named "SubtleCrypto.generateKey/AesKeyGenParams.length", exactly as GK_SELECT declares §18.4.4 step
           5's; a comparison in C is what must NOT appear at this site. */
        DFAIL("§29.4.3 step 2 BRANCHES on AesKeyGenParams' `length`, and this one is unknown external input — "
              "the three admitted lengths and the OperationError are four feasible outcomes, and deciding "
              "them against a shape would delete every arm but one. Declare a step_fork_run over those "
              "outcomes here, named \"SubtleCrypto.generateKey/AesKeyGenParams.length\", exactly as GK_SELECT "
              "declares §18.4.4 step 5's; a comparison in C is what must NOT appear at this site");
        return JS_STEP_ABRUPT;
    } else {
        double d = 0.0;

        r = step_todouble_run(ctx, hdr, s->len_v, cb_result, &d, out_cb, out_argc);
        if (r > 0) return r;
        if (r < 0) return sc_reject(ctx, &s->p, presult);
        cb_result = JS_UNDEFINED;
        /* §3.3.6 [EnforceRange]'s ARM of §3.2.4.9 Abstract operations' ConvertToInt: a non-finite value, or
           one whose integer part falls outside the type's range, is a TypeError rather than the modulo an
           unadorned `unsigned short` would take. §27.5's `length` is 16-bit and unsigned, which is NARROWER
           than §31.3's `unsigned long` one atom over — the same member name at two declared widths, so 70000
           is a TypeError here and a number there. */
        if (!isfinite(d)) {
            JS_ThrowTypeError(ctx, "%s", "the `length` of the algorithm passed to SubtleCrypto.generateKey is "
                                         "not a finite number");
            return sc_reject(ctx, &s->p, presult);
        }
        d = trunc(d);   /* §3.2.4.9's IntegerPart: the value truncated toward zero */
        if (d < 0 || d > 65535.0) {
            JS_ThrowTypeError(ctx, "%s", "the `length` of the algorithm passed to SubtleCrypto.generateKey is "
                                         "outside the range of an unsigned short");
            return sc_reject(ctx, &s->p, presult);
        }
        s->length = (uint32_t)d;
    }
    STEP_GOTO(hdr->stage, GK_DONE, &hdr->get_phase, &hdr->str_phase, &hdr->num_phase, NULL);

    STEP_ARM(GK_DONE);
    JS_FreeValue(ctx, cb_result);
    {
        JSValue key;

        /* §14.3.6 STEP 8: "Let result be the result of performing the generate key operation specified by
           normalizedAlgorithm using algorithm, extractable and usages" — the whole of §29.4.3. `extractable`
           and `usages` are the mint's arguments rather than writes after the fact, for the reason §14.3.9's
           own step 11 and 12 are: a CryptoKey whose slots are filled in afterwards is a CryptoKey that
           briefly exists with the wrong ones. */
        switch ((GkAlgorithm)s->alg) {
        case GK_ALG_AES_GCM:
            key = aes_gcm_generate_key(ctx, s->length, s->extractable != 0, s->usages);
            break;
        default:
            /* UNREACHABLE BY CONSTRUCTION and asserted rather than defended: GK_REGISTERED is this engine's
               own table, the fork's range check refused an arm outside it and the concrete arm returned
               before assigning, so a value here is those three having come apart rather than anything a page
               said. */
            DFAILF("§18.4.4 step 5 selected registry row %u, which GkAlgorithm does not name — the fork "
                   "declared GK_FORK_OUTCOMES over GK_REGISTERED and this row is in neither",
                   (unsigned)s->alg);
            /* RELEASE: the row cannot be performed, which is what §18.4.4's own Otherwise answers for a name
               it cannot resolve. Stated rather than left to whatever happened to be pending, because
               sc_reject settles with the live exception and an arm that throws nothing would settle with
               none. */
            JS_ThrowDOMException(ctx, "NotSupportedError", "%s",
                                 "the algorithm named is not a registered `generateKey` algorithm");
            return sc_reject(ctx, &s->p, presult);
        }
        if (JS_IsException(key))
            return sc_reject(ctx, &s->p, presult);
        /* §14.3.6 STEP 9's FIRST ARM: "If result is a CryptoKey object: If the [[type]] internal slot of
           result is "secret" or "private" and usages is empty, then throw a SyntaxError." §29.4.3 step 9 mints
           a "secret" key outright, so this is the empty-usages test for the one registered row, and it is the
           METHOD's step rather than the algorithm's — which is why it runs on what comes back.
           STEP 9's SECOND ARM IS A WORLD NO REGISTERED ROW REACHES, and that is §29.2's Result column
           ("generateKey … CryptoKey") rather than a narrowing this file chose: an AES key is symmetric, so
           there is no CryptoKeyPair for its [[usages]]-of-privateKey test to be about. A row whose Result is a
           pair is what builds that arm, and it builds CryptoKeyPair with it — the interface does not exist in
           this engine at all, so there is nothing here that could answer the test wrongly.
           IT READS THE MASK AND NOT THE KEY'S OWN SLOT, which is sound only while every row mints a "secret"
           key: the day a row mints a "public" one, this test is what has to read [[type]] back. */
        if (s->usages == 0) {
            JS_FreeValue(ctx, key);
            JS_ThrowDOMException(ctx, "SyntaxError", "%s",
                                 "a secret key must be generated with at least one usage");
            return sc_reject(ctx, &s->p, presult);
        }
        /* STEPS 10-12: queue the task, convert to an ECMAScript object in realm (a CryptoKey already is one),
           and resolve. */
        return sc_resolve(ctx, &s->p, key, presult);
    }
}

static const IdlStepDecl GK_DECL = {
    gk_step, sizeof(GkState), gk_visit, NULL,
    "Web Cryptography §14.3.6 generateKey(algorithm, extractable, keyUsages)", GK_STEPS,
    /* catches_abrupt: §14.3.6 step 3 REJECTS for every error normalizing an algorithm produced, and both of
       the member getters can throw after suspending. */
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
    /* §14's interface is `[SecureContext]` as a whole, and Web IDL §3.3.13 [SecureContext] REMOVES a member in
       a non-secure realm rather than making it throw — `'digest' in crypto.subtle` is what a bundle
       feature-detects with, and absent, throwing and undefined are three different branches. */
    idl_install_method_exposed(ctx, proto, "encrypt", g_id_encrypt, IDL_SECURE_CONTEXT);
    idl_install_method_exposed(ctx, proto, "decrypt", g_id_decrypt, IDL_SECURE_CONTEXT);
    idl_install_method_exposed(ctx, proto, "digest", g_id_digest, IDL_SECURE_CONTEXT);
    idl_install_method_exposed(ctx, proto, "sign", g_id_sign, IDL_SECURE_CONTEXT);
    idl_install_method_exposed(ctx, proto, "verify", g_id_verify, IDL_SECURE_CONTEXT);
    idl_install_method_exposed(ctx, proto, "importKey", g_id_import_key, IDL_SECURE_CONTEXT);
    idl_install_method_exposed(ctx, proto, "exportKey", g_id_export_key, IDL_SECURE_CONTEXT);
    idl_install_method_exposed(ctx, proto, "generateKey", g_id_generate_key, IDL_SECURE_CONTEXT);
    JS_SetClassProto(ctx, g_subtle_class, JS_DupValue(ctx, proto));

    global = JS_GetGlobalObject(ctx);
    /* THE OTHER HALF OF THAT ONE ANNOTATION — §3.3.13's example states it in the same sentence as the members
       ("In such a context, there will be no \"HeartbeatSensor\" property on Window") — so the interface object
       carries the SAME IdlExposure the member above does, and `'SubtleCrypto' in window` is false over plain
       http exactly as `crypto.subtle` is undefined there. */
    idl_install_interface_object_exposed(ctx, global, "SubtleCrypto", proto, IDL_SECURE_CONTEXT);
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
    /* §14's `Promise<ArrayBuffer> sign(AlgorithmIdentifier algorithm, CryptoKey key, BufferSource data)` and
       `Promise<boolean> verify(AlgorithmIdentifier algorithm, CryptoKey key, BufferSource signature,
       BufferSource data)` — two lists over ONE step definition, which is what the magic is for. */
    /* §14's `Promise<ArrayBuffer> encrypt(AlgorithmIdentifier algorithm, CryptoKey key, BufferSource data)`
       and the IDENTICAL list `decrypt` declares — which is why one declaration serves both members and one
       machine serves both algorithms. */
    static const IdlArgType EN_ARGS[] = { IDL_STRING_UNLESS_OBJECT, IDL_INTERFACE, IDL_BUFFERSOURCE };
    static const IdlArgType SV_ARGS_SIGN[]   = { IDL_STRING_UNLESS_OBJECT, IDL_INTERFACE, IDL_BUFFERSOURCE };
    static const IdlArgType SV_ARGS_VERIFY[] = { IDL_STRING_UNLESS_OBJECT, IDL_INTERFACE, IDL_BUFFERSOURCE,
                                                 IDL_BUFFERSOURCE };
    /* §14's `Promise<CryptoKey> importKey(KeyFormat format, (BufferSource or JsonWebKey) keyData,
       AlgorithmIdentifier algorithm, boolean extractable, sequence<KeyUsage> keyUsages)`.
       THE keyData UNION IS DECLARED WHOLE, and it is the only position in the platform's whole IDL with this
       shape — `(BufferSource or D)` where D is a dictionary occurs here and nowhere else, which is why
       core/idl_args.h's row for it says what it serves rather than what it might. It used to be declared
       IDL_BUFFERSOURCE, and that was not a narrowing but a WRONG ANSWER: a JWK is an ordinary object, so
       `importKey("jwk", {kty:"oct",k:"…"}, …)` was refused by the ARGUMENT CONVERSION with a TypeError before
       step 1 of §14.3.9 ran, where a browser resolves with a CryptoKey. THE TWO ENUMERATIONS ON THIS LINE ARE BOTH DECLARED,
       which is what one value list per declaration could not do: §3.2.18's `E` is a fact about a POSITION, so
       `format` states KeyFormat and `keyUsages` states KeyUsage as its element type. */
    static const IdlArgType IK_ARGS[] = { IDL_ENUM, IDL_BUFFERSOURCE_OR_DICT, IDL_STRING_UNLESS_OBJECT,
                                          IDL_BOOLEAN, IDL_SEQUENCE_ENUM };
    /* §14's `Promise<(ArrayBuffer or JsonWebKey)> exportKey(KeyFormat format, CryptoKey key)`. BOTH POSITIONS
       ARE SETTLED BY §3.6's CONVERSION and neither can suspend, which is the whole reason §14.3.10's machine
       has one stage: an enumeration is a membership test over a string and an interface is a brand test over a
       class, and there is no dictionary here for a page to hang a getter on. */
    static const IdlArgType XK_ARGS[] = { IDL_ENUM, IDL_INTERFACE };
    /* ---- §15 "JsonWebKey dictionary", AS THE IDL DECLARES IT ---------------------------------------------
       `dictionary RsaOtherPrimesInfo { DOMString r; DOMString d; DOMString t; };` — the element type of
       JsonWebKey's `oth`, and a dictionary NO OTHER declaration in this engine reaches. It is declared here
       rather than beside HMAC because a dictionary's member table belongs to the component that RECEIVES the
       argument, which is this member's declaration; §31.6.4's algorithm is what READS a few of the members.
       §3.2.17's READ ORDER IS LEXICOGRAPHIC WITHIN A LEVEL, which is what these arrays are sorted by and what
       a page with a getter on each member observes. Neither dictionary inherits, so every member is level 0. */
    static const IdlDictMember RSA_OTHER_PRIMES_MEMBERS[] = {
        { "d", IDL_DOMSTRING, false, NULL, 0, NULL, IDL_DEFAULT_NONE, NULL },
        { "r", IDL_DOMSTRING, false, NULL, 0, NULL, IDL_DEFAULT_NONE, NULL },
        { "t", IDL_DOMSTRING, false, NULL, 0, NULL, IDL_DEFAULT_NONE, NULL },
    };
    static const IdlDictDecl RSA_OTHER_PRIMES_DECL = {
        "RsaOtherPrimesInfo", RSA_OTHER_PRIMES_MEMBERS,
        (int)(sizeof RSA_OTHER_PRIMES_MEMBERS / sizeof *RSA_OTHER_PRIMES_MEMBERS)
    };
    /* `dictionary JsonWebKey { … };` — EIGHTEEN members in §15 plus the TWO a `partial dictionary JsonWebKey`
       adds (`pub` and `priv`, which that partial's own comment attributes to RFC 9964). A partial is not
       inheritance: its members are the same dictionary's at the same level, so all twenty sort together.
       `ext` IS IDL_BOOLEAN_NO_DEFAULT AND THAT IS NOT A NICETY. §15 writes `boolean ext;` with no default, and
       §31.6.4 Import Key's jwk arm, step 9, branches on PRESENCE — "If the ext field of jwk is present and has the value
       false and extractable is true, then throw a DataError" — so absent and present-and-false are two states
       the algorithm tells apart, which IDL_BOOLEAN's ToBoolean(undefined) folds into one.
       `oth` IS THE FIRST MEMBER IN THIS ENGINE TO NEED `sequence<D>`, and core/idl_args.h's IDL_SEQUENCE_DICT
       is that row: the corpus declares eighty-five dictionary members of that shape and until it existed not
       one of them could be declared at all. */
    static const IdlDictMember JWK_MEMBERS[] = {
        { "alg",     IDL_DOMSTRING,           false, NULL, 0, NULL,                   IDL_DEFAULT_NONE, NULL },
        { "crv",     IDL_DOMSTRING,           false, NULL, 0, NULL,                   IDL_DEFAULT_NONE, NULL },
        { "d",       IDL_DOMSTRING,           false, NULL, 0, NULL,                   IDL_DEFAULT_NONE, NULL },
        { "dp",      IDL_DOMSTRING,           false, NULL, 0, NULL,                   IDL_DEFAULT_NONE, NULL },
        { "dq",      IDL_DOMSTRING,           false, NULL, 0, NULL,                   IDL_DEFAULT_NONE, NULL },
        { "e",       IDL_DOMSTRING,           false, NULL, 0, NULL,                   IDL_DEFAULT_NONE, NULL },
        { "ext",     IDL_BOOLEAN_NO_DEFAULT,  false, NULL, 0, NULL,                   IDL_DEFAULT_NONE, NULL },
        { "k",       IDL_DOMSTRING,           false, NULL, 0, NULL,                   IDL_DEFAULT_NONE, NULL },
        { "key_ops", IDL_SEQUENCE_DOMSTRING,  false, NULL, 0, NULL,                   IDL_DEFAULT_NONE, NULL },
        { "kty",     IDL_DOMSTRING,           false, NULL, 0, NULL,                   IDL_DEFAULT_NONE, NULL },
        { "n",       IDL_DOMSTRING,           false, NULL, 0, NULL,                   IDL_DEFAULT_NONE, NULL },
        { "oth",     IDL_SEQUENCE_DICT,       false, NULL, 0, &RSA_OTHER_PRIMES_DECL, IDL_DEFAULT_NONE, NULL },
        { "p",       IDL_DOMSTRING,           false, NULL, 0, NULL,                   IDL_DEFAULT_NONE, NULL },
        { "priv",    IDL_DOMSTRING,           false, NULL, 0, NULL,                   IDL_DEFAULT_NONE, NULL },
        { "pub",     IDL_DOMSTRING,           false, NULL, 0, NULL,                   IDL_DEFAULT_NONE, NULL },
        { "q",       IDL_DOMSTRING,           false, NULL, 0, NULL,                   IDL_DEFAULT_NONE, NULL },
        { "qi",      IDL_DOMSTRING,           false, NULL, 0, NULL,                   IDL_DEFAULT_NONE, NULL },
        { "use",     IDL_DOMSTRING,           false, NULL, 0, NULL,                   IDL_DEFAULT_NONE, NULL },
        { "x",       IDL_DOMSTRING,           false, NULL, 0, NULL,                   IDL_DEFAULT_NONE, NULL },
        { "y",       IDL_DOMSTRING,           false, NULL, 0, NULL,                   IDL_DEFAULT_NONE, NULL },
    };
    /* §14.1 Data Types: "enum KeyFormat { \"raw\", \"spki\", \"pkcs8\", \"jwk\" };" — the value list IS the
       type, so `importKey("RAW", …)` is a TypeError from §3.2.18 before any step of §14.3.9 runs. Written with
       IDL_ENUM_VALUES because that macro SUPPLIES the terminator both readers of a value list scan for; a
       hand-written list is a list whose last element can be left off.
       IT IS NAMED FOR THE TYPE AND NOT FOR A MEMBER because TWO declarations state it — §14.3.9's position 0
       and §14.3.10's — and §3.2.18's `E` is a fact about the TYPE that each position restates. A second
       four-string list beside the second declaration would be the copy that drifts, and the day §14.1 grows a
       value it would be the copy nobody updates. */
    IDL_ENUM_VALUES(KEY_FORMATS, "raw", "spki", "pkcs8", "jwk");

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
    /* THE MEMBER NAMES §18.4.4 step 6's CONVERSION AND step 10's WALK READ, INTERNED ONCE. A keyed request
       holds its atom across a suspension, so each is agent state and not a string composed at the read. The
       count is deliberately not written here: it was "THREE" while HmacImportParams was the only declared
       dictionary, and §29.3 AesGcmParams' three members made that sentence wrong without touching a line of
       it. What the reader needs is the RULE, and the list below is the list. */
    g_atom_name = JS_NewAtom(ctx, "name");
    CHECK(g_atom_name != JS_ATOM_NULL, "the Algorithm dictionary's `name` could not be interned");
    g_atom_hash = JS_NewAtom(ctx, "hash");
    CHECK(g_atom_hash != JS_ATOM_NULL, "HmacImportParams' `hash` could not be interned");
    g_atom_length = JS_NewAtom(ctx, "length");
    CHECK(g_atom_length != JS_ATOM_NULL, "HmacImportParams' `length` could not be interned");
    g_atom_iv = JS_NewAtom(ctx, "iv");
    CHECK(g_atom_iv != JS_ATOM_NULL, "AesGcmParams' `iv` could not be interned");
    g_atom_additional_data = JS_NewAtom(ctx, "additionalData");
    CHECK(g_atom_additional_data != JS_ATOM_NULL, "AesGcmParams' `additionalData` could not be interned");
    g_atom_tag_length = JS_NewAtom(ctx, "tagLength");
    CHECK(g_atom_tag_length != JS_ATOM_NULL, "AesGcmParams' `tagLength` could not be interned");
    g_id_digest = idl_method_id_step(ctx, SD_ARGS, 2, NULL, 0, &SD_DECL, 0);
    /* §14's `Promise<ArrayBuffer> digest(...)`: Web IDL §3.7.7 makes EVERY throw of this member — the brand
       check, the arity, both argument conversions and the algorithm itself — a rejected promise. */
    idl_returns_promise();
    /* §3.7's implementation-check an object, step 3, STATED RATHER THAN PERFORMED — see subtle_crypto_is for
       why the position of this statement is the whole point of it. Every one of §14's members is a REGULAR
       operation on the interface prototype object, so every one of them makes it. */
    idl_this_iface(subtle_crypto_is, "SubtleCrypto");
    g_id_sign = idl_method_id_step(ctx, SV_ARGS_SIGN, 3, NULL, 0, &SV_DECL, SC_M_SIGN);
    idl_returns_promise();
    idl_this_iface(subtle_crypto_is, "SubtleCrypto");
    /* §3.2.15's `I` for the `CryptoKey key` position — the class, which is what cannot be forged. THE TWO
       BRANDS ARE ANSWERED AT OPPOSITE ENDS OF THE MEMBER: this one is part of §3.6's conversion at 2.1.5, the
       receiver's is 2.1.2.3, so `sign.call({}, alg, notAKey, data)` names the RECEIVER and not the key. */
    idl_iface_brand(crypto_key_class());
    g_id_verify = idl_method_id_step(ctx, SV_ARGS_VERIFY, 4, NULL, 0, &SV_DECL, SC_M_VERIFY);
    idl_returns_promise();
    idl_this_iface(subtle_crypto_is, "SubtleCrypto");
    idl_iface_brand(crypto_key_class());
    g_id_import_key = idl_method_id_step(ctx, IK_ARGS, 5, JWK_MEMBERS,
                                        (int)(sizeof JWK_MEMBERS / sizeof *JWK_MEMBERS), &IK_DECL, 0);
    idl_returns_promise();
    idl_this_iface(subtle_crypto_is, "SubtleCrypto");
    /* §3.2.18's `E` AT EACH OF THE TWO POSITIONS §14.3.9's IDL declares one at — position 0's own type, and
       position 4's ELEMENT type. */
    idl_arg_enum(0, KEY_FORMATS);
    idl_arg_enum(4, CRYPTO_KEY_USAGE_NAMES);
    /* §14.3.1 and §14.3.2, TWO MEMBERS OVER ONE STEP DEFINITION — the same magic the sign/verify pair uses,
       and here the two argument lists are identical as well, so the magic is the whole of the difference. */
    g_id_encrypt = idl_method_id_step(ctx, EN_ARGS, 3, NULL, 0, &EN_DECL, SC_M_ENCRYPT);
    idl_returns_promise();
    idl_this_iface(subtle_crypto_is, "SubtleCrypto");
    idl_iface_brand(crypto_key_class());
    g_id_decrypt = idl_method_id_step(ctx, EN_ARGS, 3, NULL, 0, &EN_DECL, SC_M_DECRYPT);
    idl_returns_promise();
    idl_this_iface(subtle_crypto_is, "SubtleCrypto");
    idl_iface_brand(crypto_key_class());
    /* §14's `Promise<(CryptoKey or CryptoKeyPair)> generateKey(AlgorithmIdentifier algorithm, boolean
       extractable, sequence<KeyUsage> keyUsages)` — §14.3.9's LAST THREE POSITIONS EXACTLY, with its `format`
       and its `keyData` union removed from the front. The three rows are therefore the same three rows that
       list ends in, written the same way for the same reasons: a bare `AlgorithmIdentifier` is §18.4.4's
       `(object or DOMString)`, `boolean` is §3.2.2's total ToBoolean, and the sequence's element type is
       stated by idl_arg_enum below rather than by the row. */
    static const IdlArgType GK_ARGS[] = { IDL_STRING_UNLESS_OBJECT, IDL_BOOLEAN, IDL_SEQUENCE_ENUM };
    g_id_export_key = idl_method_id_step(ctx, XK_ARGS, 2, NULL, 0, &XK_DECL, 0);
    idl_returns_promise();
    idl_this_iface(subtle_crypto_is, "SubtleCrypto");
    /* §3.2.15's `I` for the `CryptoKey key` position and §3.2.18's `E` for the `KeyFormat format` one — the
       same two statements sign and importKey make, over the same class and the same value list. */
    idl_iface_brand(crypto_key_class());
    idl_arg_enum(0, KEY_FORMATS);
    g_id_generate_key = idl_method_id_step(ctx, GK_ARGS, 3, NULL, 0, &GK_DECL, 0);
    idl_returns_promise();
    idl_this_iface(subtle_crypto_is, "SubtleCrypto");
    /* §3.2.18's `E` for the ELEMENT type of position 2's `sequence<KeyUsage>` — the same list §14.3.9's
       position 4 declares, and the reason sc_usages_normalize cannot fail. */
    idl_arg_enum(2, CRYPTO_KEY_USAGE_NAMES);
    /* DECLARED UNDER THE ROW THAT RELEASES IT, which is `crypto` — §10's component declares this one and its
       release reaches this one's, so core/platform.c's two-sided check ("a row with a release that declared no
       agent state cannot be asserted to have undone anything") is asking about the pair. Naming a component
       with no row of its own would leave these slots on the registry with nothing on the release column to be
       the inverse of. */
    agent_state_id("crypto", &g_obj_slot, "§10.2.1's per-realm SubtleCrypto slot, and the declaration latch");
    agent_state_id("crypto", &g_id_digest, "§14.3.5's digest machine");
    agent_state_id("crypto", &g_id_sign, "§14.3.3's sign machine");
    agent_state_id("crypto", &g_id_verify, "§14.3.4's verify machine");
    agent_state_id("crypto", &g_id_import_key, "§14.3.9's importKey machine");
    agent_state_id("crypto", &g_id_export_key, "§14.3.10's exportKey machine");
    agent_state_id("crypto", &g_id_generate_key, "§14.3.6's generateKey machine");
    agent_state_id("crypto", &g_id_encrypt, "§14.3.1's encrypt machine");
    agent_state_id("crypto", &g_id_decrypt, "§14.3.2's decrypt machine");
    agent_state_atom("crypto", &g_atom_name, "the Algorithm dictionary's `name` member name");
    agent_state_atom("crypto", &g_atom_hash, "HmacImportParams' `hash` member name");
    agent_state_atom("crypto", &g_atom_length, "the `length` member name of §31.3's HmacImportParams and "
                                               "of §27.5's AesKeyGenParams — ONE atom because it is one "
                                               "string, and TWO dictionaries because the declared types "
                                               "differ: §31.3's is `unsigned long` and §27.5's is `unsigned "
                                               "short`, so their [EnforceRange] ranges are not the same and "
                                               "neither machine may read the other's");
    agent_state_atom("crypto", &g_atom_iv, "AesGcmParams' `iv` member name");
    agent_state_atom("crypto", &g_atom_additional_data, "AesGcmParams' `additionalData` member name");
    agent_state_atom("crypto", &g_atom_tag_length, "AesGcmParams' `tagLength` member name");
    agent_state_ptr("crypto", &g_rt, "the runtime that `name` was interned in");
    realm_declare_intrinsic(subtle_crypto_install_realm);
}

void subtle_crypto_free(void)
{
    crypto_key_free();
    if (g_obj_slot < 0)
        return;
    DCHECK(g_rt != NULL, "SubtleCrypto was declared without recording the runtime its atoms belong to");
    JS_FreeAtomRT(g_rt, g_atom_name);
    JS_FreeAtomRT(g_rt, g_atom_hash);
    JS_FreeAtomRT(g_rt, g_atom_length);
    JS_FreeAtomRT(g_rt, g_atom_iv);
    JS_FreeAtomRT(g_rt, g_atom_additional_data);
    JS_FreeAtomRT(g_rt, g_atom_tag_length);
    g_atom_name = g_atom_hash = g_atom_length = JS_ATOM_NULL;
    g_atom_iv = g_atom_additional_data = g_atom_tag_length = JS_ATOM_NULL;
    g_obj_slot = -1;
    g_id_digest = -1;
    g_id_sign = -1;
    g_id_verify = -1;
    g_id_import_key = -1;
    g_id_export_key = -1;
    g_id_generate_key = -1;
    g_id_encrypt = -1;
    g_id_decrypt = -1;
    g_rt = NULL;
}
