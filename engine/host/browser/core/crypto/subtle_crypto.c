/* Web Cryptography API §14.3's methods and §18.4.4's normalize an algorithm — the FOUR members of §14 this
 * engine performs, each as a step machine, plus the promise capability all four are wrapped in. §14.3.5's
 * digest reaches §32.3.1's Digest; §14.3.3's sign, §14.3.4's verify and §14.3.9's importKey reach §31's HMAC
 * operations, whose algorithm is core/crypto/hmac.c. See subtle_crypto.h for why those four and no others.
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
static JSAtom    g_atom_name = JS_ATOM_NULL;
static JSAtom    g_atom_hash = JS_ATOM_NULL;
static JSAtom    g_atom_length = JS_ATOM_NULL;
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
   contributes its EXAMPLE's copy when it has a BufferSource example, and the empty sequence otherwise. Written
   once because sign has one such operand and verify has two, and because the "does this example hold bytes"
   test is the same three-way brand §14.3.5's digest asks of its own. */
static JSValue sv_operand_bytes(JSContext *ctx, JSValueConst v, bool *unknown, bool *has_example)
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
            s->sig = sv_operand_bytes(ctx, sig, &unknown, &has_example);
            CHECK(!JS_IsException(s->sig), "§14.3.4 step 4's copy of the signature could not be allocated");
        }
        if (concolic_is(data))
            s->unknown_arg = (uint8_t)(verifying ? 3 : 2);
        s->bytes = sv_operand_bytes(ctx, data, &unknown, &has_example);
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

/* ---- §14.3.9 The importKey method -------------------------------------------------------------------------- */

/* §14.3.9's sixteen steps, and §31.6.4 HMAC Import Key's sixteen, meet in one machine because the method's
 * step 9 is "Let result be the CryptoKey object that results from performing the import key operation
 * specified by normalizedAlgorithm" — so the method reads §31.3's members, hands them over, and applies its own
 * steps 10-12 to what comes back. The split between the two files is exactly that sentence: everything about
 * WHAT AN HMAC KEY IS lives in core/crypto/hmac.c, and everything about how a promise is settled and which
 * slots §14.3.9 itself writes lives here.
 *
 * §18.4.4's MEMBER WALK IS WHY THERE ARE SIX READING STAGES AND NOT ONE. Step 9 builds "a list consisting of
 * the IDL dictionary type desiredType and all of desiredType's inherited dictionaries, in order from least to
 * most derived", and step 10 walks each dictionary's members "in order" — so for `HmacImportParams : Algorithm`
 * that is Algorithm's `name`, then HmacImportParams' `hash` and `length`. EVERY ONE OF THOSE IS A READ OF THE
 * PAGE'S OBJECT, one accessor or Proxy trap away from the page's own code, so each is a REQUEST that can
 * suspend and not a JS_GetPropertyStr. The ORDER is observable in three lines of script: an algorithm object
 * whose `name` getter logs and whose `hash` getter throws tells you which ran.
 *
 * AND `hash` IS NORMALIZED RECURSIVELY, which is what makes it two stages of its own. Step 10's per-member
 * dispatch says so by type: "If member is of the type HashAlgorithmIdentifier: Set the dictionary member on
 * normalizedAlgorithm with key name key to the result of normalizing an algorithm, with the alg set to idlValue
 * and the op set to \"digest\"". So `{name:"HMAC", hash:"SHA-256"}` and `{name:"HMAC", hash:{name:"SHA-256"}}`
 * are the same algorithm, and the second reads a `name` off the page's inner object. */

static const char IK_FORK_OP[]      = "SubtleCrypto.importKey/normalizeAlgorithm";
static const char IK_FORK_OP_HASH[] = "SubtleCrypto.importKey/normalizeAlgorithm/hash";

/* §31.2 Registration's `importKey` row is the only one this engine registers for that operation — see the
   sign/verify machine's note on §18.5.1 for why an empty registry elsewhere is conformant. */
#define IK_REGISTERED_N 1
#define IK_FORK_OUTCOMES (IK_REGISTERED_N + 1)

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
IDL_ENUM_VALUES(IK_KEY_USAGES, "encrypt", "decrypt", "sign", "verify", "deriveKey", "deriveBits", "wrapKey",
                "unwrapKey");

/* §9 Terminology's "normalized value of a usages list", which is "the usage intersection of usages and the
 * list of recognized key usage values" — a sequence containing each recognized value that appears in both, in
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
static void ik_usages_normalize(JSContext *ctx, JSValueConst list, uint32_t *out)
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
        for (k = 0; IK_KEY_USAGES[k]; k++)
            if (strcmp(nm, IK_KEY_USAGES[k]) == 0) break;
        /* ALWAYS FATAL, and it is a CHECK rather than a DCHECK because `k` is LOAD-BEARING IN RELEASE: the
           shift below builds §13.3's [[usages]], which is the authorization every later operation asks (§14.3.3
           step 10's "does not contain an entry that is \"sign\""). A `k` that walked off this list would set a
           bit outside CRYPTO_KEY_USAGES_ALL in the one build where nothing checked, and a key would carry a
           permission no page asked for. */
        CHECK(IK_KEY_USAGES[k] != NULL,
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
        ik_usages_normalize(ctx, argc > 4 ? argv[4] : JS_UNDEFINED, &s->usages);
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
    } else {
        const char *nm = JS_ToCString(ctx, s->name_v);
        bool known;

        CHECK(nm != NULL, "§18.4.4's algName could not be read back as UTF-8");
        known = sd_name_matches(nm, "HMAC");
        if (!known) {
            JS_ThrowDOMException(ctx, "NotSupportedError", "'%s' is not a registered `importKey` algorithm",
                                 nm);
            JS_FreeCString(ctx, nm);
            return sc_reject(ctx, &s->p, presult);
        }
        JS_FreeCString(ctx, nm);
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
        HmacImportParams params;
        const char *format = JS_ToCString(ctx, format_v);
        JSValue key;

        CHECK(format != NULL, "§14.1's KeyFormat could not be read back as UTF-8 — the argument conversion "
                              "already checked it against the enumeration's four values");
        /* §14.3.9 STEP 4's Otherwise arm: "If the keyData parameter passed to the importKey() method is a
           JsonWebKey dictionary, throw a TypeError" — and its jwk arm's converse. The position is declared
           IDL_BUFFERSOURCE, so what arrives here is always a buffer source and never a JsonWebKey: the "jwk"
           format therefore takes the jwk arm's TypeError, which is the RIGHT answer for the input that
           reaches this engine and the wrong one for the input that cannot (see hmac.h's residual). */
        if (strcmp(format, "jwk") == 0) {
            JS_FreeCString(ctx, format);
            JS_ThrowTypeError(ctx, "%s", "importKey was given a BufferSource for the \"jwk\" format, which "
                                         "takes a JsonWebKey dictionary");
            return sc_reject(ctx, &s->p, presult);
        }
        params.hash = (SecureHashAlgorithm)s->hash;
        params.has_length = s->has_length != 0;
        params.length = s->length;
        /* §14.3.9 STEP 9, which is the whole of §31.6.4 HMAC Import Key. Steps 11 and 12 — "Set the
           [[extractable]] internal slot of result to extractable" and "Set the [[usages]] internal slot of
           result to the normalized value of usages" — are the mint's two arguments rather than two writes
           after the fact, because a CryptoKey whose slots are filled in afterwards is a CryptoKey that
           briefly exists with the wrong ones. */
        key = hmac_import_key(ctx, format, key_data, &params, s->extractable != 0, s->usages);
        JS_FreeCString(ctx, format);
        if (JS_IsException(key))
            return sc_reject(ctx, &s->p, presult);
        /* §14.3.9 STEP 10: "If the [[type]] internal slot of result is \"secret\" or \"private\" and usages is
           empty, then throw a SyntaxError." An HMAC key's type is always "secret" (§31.6.4 step 10), so this
           is the empty-usages test — and it is the METHOD's step rather than the algorithm's, which is why it
           runs on what comes back and not inside hmac_import_key. */
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
    idl_install_method_exposed(ctx, proto, "digest", g_id_digest, IDL_SECURE_CONTEXT);
    idl_install_method_exposed(ctx, proto, "sign", g_id_sign, IDL_SECURE_CONTEXT);
    idl_install_method_exposed(ctx, proto, "verify", g_id_verify, IDL_SECURE_CONTEXT);
    idl_install_method_exposed(ctx, proto, "importKey", g_id_import_key, IDL_SECURE_CONTEXT);
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
    static const IdlArgType SV_ARGS_SIGN[]   = { IDL_STRING_UNLESS_OBJECT, IDL_INTERFACE, IDL_BUFFERSOURCE };
    static const IdlArgType SV_ARGS_VERIFY[] = { IDL_STRING_UNLESS_OBJECT, IDL_INTERFACE, IDL_BUFFERSOURCE,
                                                 IDL_BUFFERSOURCE };
    /* §14's `Promise<CryptoKey> importKey(KeyFormat format, (BufferSource or JsonWebKey) keyData,
       AlgorithmIdentifier algorithm, boolean extractable, sequence<KeyUsage> keyUsages)`.
       ONE OF THESE FIVE ROWS IS NARROWER THAN THE IDL AND IT IS NAMED WHERE IT BITES: the keyData union is
       declared IDL_BUFFERSOURCE — see hmac.h's residual on §31.6.4 step 5's jwk arm, which is where the
       missing `IDL_BUFFERSOURCE_OR_DICT` row is named. THE TWO ENUMERATIONS ON THIS LINE ARE BOTH DECLARED,
       which is what one value list per declaration could not do: §3.2.18's `E` is a fact about a POSITION, so
       `format` states KeyFormat and `keyUsages` states KeyUsage as its element type. */
    static const IdlArgType IK_ARGS[] = { IDL_ENUM, IDL_BUFFERSOURCE, IDL_STRING_UNLESS_OBJECT, IDL_BOOLEAN,
                                          IDL_SEQUENCE_ENUM };
    /* §14.1 Data Types: "enum KeyFormat { \"raw\", \"spki\", \"pkcs8\", \"jwk\" };" — the value list IS the
       type, so `importKey("RAW", …)` is a TypeError from §3.2.18 before any step of §14.3.9 runs. Written with
       IDL_ENUM_VALUES because that macro SUPPLIES the terminator both readers of a value list scan for; a
       hand-written list is a list whose last element can be left off. */
    IDL_ENUM_VALUES(IK_FORMATS, "raw", "spki", "pkcs8", "jwk");

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
    /* THE THREE MEMBER NAMES §18.4.4 step 10's WALK READS, INTERNED ONCE. A keyed request holds its atom
       across a suspension, so each is agent state and not a string composed at the read. */
    g_atom_name = JS_NewAtom(ctx, "name");
    CHECK(g_atom_name != JS_ATOM_NULL, "the Algorithm dictionary's `name` could not be interned");
    g_atom_hash = JS_NewAtom(ctx, "hash");
    CHECK(g_atom_hash != JS_ATOM_NULL, "HmacImportParams' `hash` could not be interned");
    g_atom_length = JS_NewAtom(ctx, "length");
    CHECK(g_atom_length != JS_ATOM_NULL, "HmacImportParams' `length` could not be interned");
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
    g_id_import_key = idl_method_id_step(ctx, IK_ARGS, 5, NULL, 0, &IK_DECL, 0);
    idl_returns_promise();
    idl_this_iface(subtle_crypto_is, "SubtleCrypto");
    /* §3.2.18's `E` AT EACH OF THE TWO POSITIONS §14.3.9's IDL declares one at — position 0's own type, and
       position 4's ELEMENT type. */
    idl_arg_enum(0, IK_FORMATS);
    idl_arg_enum(4, IK_KEY_USAGES);
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
    agent_state_atom("crypto", &g_atom_name, "the Algorithm dictionary's `name` member name");
    agent_state_atom("crypto", &g_atom_hash, "HmacImportParams' `hash` member name");
    agent_state_atom("crypto", &g_atom_length, "HmacImportParams' `length` member name");
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
    g_atom_name = g_atom_hash = g_atom_length = JS_ATOM_NULL;
    g_obj_slot = -1;
    g_id_digest = -1;
    g_id_sign = -1;
    g_id_verify = -1;
    g_id_import_key = -1;
    g_rt = NULL;
}
