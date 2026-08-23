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
 * THE TWO MEMBERS THAT ARE NOT HERE ARE ABSENT AND NOT STUBBED, and naming them is the point of this
 * paragraph. `getRandomValues` and `randomUUID` are real members of §10 and this engine does not install them,
 * so `crypto.randomUUID` is `undefined` and calling it is the page's own TypeError — the forcing function
 * §NO STUBS asks for, and what engine/idlgen.mjs's audit prints against this component. They are not built
 * because RANDOMNESS IS AN UNANSWERED DESIGN QUESTION FOR THIS ENGINE and not because they are hard: a
 * solver whose flows must resume byte-identically across sessions cannot draw from an entropy source, and the
 * three candidate answers — a per-flow deterministic stream keyed by the flow's identity, a concolic with no
 * example (unknown input the page did not receive from an attacker), and a fixed value — are three different
 * products. Whoever answers that question builds both members here; nothing else in this file changes.
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
