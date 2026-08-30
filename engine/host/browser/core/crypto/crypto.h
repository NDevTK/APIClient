/* Web Cryptography API §10 Crypto interface, and the Window member that answers with one.
 *
 *   partial interface mixin WindowOrWorkerGlobalScope {
 *     [SameObject] readonly attribute Crypto crypto;
 *   };
 *
 *   [Exposed=(Window,Worker)]
 *   interface Crypto {
 *     [SecureContext] readonly attribute SubtleCrypto subtle;
 *     ArrayBufferView getRandomValues(ArrayBufferView array);
 *     [SecureContext] DOMString randomUUID();
 *   };
 *
 * §10.2.1 The subtle attribute: "The subtle attribute provides an instance of the SubtleCrypto interface which
 * provides low-level cryptographic primitives and algorithms."
 *
 * WHY THIS IS ITS OWN FILE AND NOT PART OF THE ALGORITHM SURFACE. §10 is a two-member door and §14 is the
 * surface behind it; they are separate interfaces with separate exposure conditions (`Crypto` is exposed
 * everywhere and `subtle` is `[SecureContext]`), and the whole content of this component is object identity —
 * which object a realm answers `crypto` with, and which object that one answers `subtle` with. Folding it into
 * the algorithms would put a realm's identity question inside a file about hashing.
 *
 * WHAT §10.1 RETURNS, AND WHY IT IS A REPRODUCIBLE STREAM RATHER THAN ENTROPY. This paragraph used to say the
 * question was open and name three candidate answers — a deterministic stream, a concolic with no example, and
 * a fixed value. They are not three tastes; two of them are refuted by rules this project already holds, and
 * the argument is written out here because a value a solver INVENTS is exactly the thing §RUN, DON'T MATCH
 * exists to stop, so an unargued choice would be indistinguishable from one.
 *
 * NOT A CONCOLIC WITH NO EXAMPLE. §Headless is not valueless reserves opacity for the UNKNOWABLE — attacker
 * input and server-injected absent state — and says in as many words that it is never for "there is no
 * device". Random bytes are neither: no attacker chooses them and no server injects them, and what is missing
 * here is an entropy DEVICE, which is that excluded case exactly. The cost is not only fidelity. A
 * domain-carrying value FORKS control flow, so `if (buf[0] & 1)` would fork a world per bit of a nonce nobody
 * can steer — unbounded forking that reaches no attacker-reachable code while outranking work that would. And
 * an endpoint key built from an opaque nonce degrades to a SHAPE, losing the @H example for a value the page
 * COMPUTED, which is the one case §@H says must stay concrete.
 *
 * NOT A FIXED VALUE. Two failures, and both LOSE code paths rather than merely being unfaithful. §10.1.2's
 * UUID is an IDENTITY — bundles use one as a Map key, a request id, a list key — so a constant collapses N
 * distinct objects into one, and the flows that would have explored the Nth never exist. That is a
 * TRUNCATION, which §NO BOUNDS forbids by name. And a zero fill makes the ordinary rejection-sampling loop
 * `do { crypto.getRandomValues(b) } while (!b[0])` non-terminating — which this engine has no watchdog to cut
 * and must not grow one.
 *
 * SO: A REPRODUCIBLE STREAM, and the reason is the SCHEDULER rather than taste. §Time-travel resume's razor
 * says a resume that is not byte-identical is a CAP; an entropy source makes byte-identical resume impossible
 * BY CONSTRUCTION, because the same parked flow resumed tomorrow draws different bytes and takes a different
 * arm. Determinism is not a convenience here — it is what makes a snapshot a continuation. §Testing's solver
 * differential then wants one document under several schedules to emit the SAME findings, and a real RNG fails
 * that gate at random, which would destroy the only oracle the solver's own semantics have. What comes back is
 * still a REAL value in §Headless's sense: well-formed, distinct per call, carrying §10.1.2's version and
 * variant bits, so a bundle's UUID-shaped regex matches here exactly as it does in Chrome.
 *
 * THESE BYTES ARE NOT SECRET AND NO FINDING MAY REST ON THEIR UNPREDICTABILITY. A PoC that depends on
 * PREDICTING a value from here does not reproduce — the real page gets real entropy — so a draw from this
 * stream is never a solved value for an attacker-supplied slot. The generator is a counter mixer standing in
 * for a device, and it is NOT §Bind before build's banned hand-rolled crypto for that reason: it is not
 * cryptography, and it is not offered as any.
 *
 * THE POSITION IS PER REALM AND IT TIME-TRAVELS. It is a C record behind a class opaque, which is precisely
 * where solver/cow.h says a write is invisible to every property hook and every engine hook, so it is captured
 * at its mutation point: two arms of a fork each draw from the position the FORK was taken at — which is what
 * a rewound-and-replayed execution must observe — and a context switch restores it. Held in a module static it
 * would be the §per-realm-fact defect as well, one counter answering every document's question.
 *
 * EVERY REALM'S STREAM STARTS AT THE SAME PLACE, AND THAT IS A CHOSEN TRADE RATHER THAN AN OVERSIGHT. Seeding
 * realms apart needs something realm-distinct, and the only such thing available is a creation ORDINAL — but
 * realms are created per FLOW, so that ordinal is a function of the SCHEDULE, and the finding set would become
 * schedule-dependent, failing the differential gate this engine's semantics are checked by. Schedule
 * independence is worth more than cross-realm distinctness: what it gives up is two DOCUMENTS drawing the same
 * first UUID, and what it keeps is distinctness WITHIN one document, which is where a page's own Map lives.
 *
 * IDENTITY IS PER REALM, and that is what `[SameObject]` means here: §3.7 gives every realm its own interface
 * prototype objects, a C member answers in the realm that DEFINED it (js_call_c_function takes `ctx` from the
 * function object), so a Crypto held in a module static would answer every document's `crypto === crypto` out
 * of whichever realm built it first. It is held in core/realm.h's per-realm value slot instead, built eagerly
 * with the realm rather than on the first read — a lazy build would place the object inside whichever FLOW
 * touched it first and each arm of a fork would get a private `crypto`. */
#ifndef ENGINE_HOST_BROWSER_CORE_CRYPTO_CRYPTO_H
#define ENGINE_HOST_BROWSER_CORE_CRYPTO_CRYPTO_H

#include "quickjs.h"

/* Declared ONCE PER AGENT; the per-realm install registers itself through core/realm.h. */
void crypto_init(JSContext *ctx);
void crypto_free(void);

#endif
