/* FULLSCREEN — the Fullscreen API Standard's §2 "Model" and §3 "API". See fullscreen.c.
 *
 * WHAT IS HERE. FULLSCREEN §2 "Model"'s "Fullscreen is supported", and FULLSCREEN §3 "API"'s
 * `fullscreenEnabled` getter on Document, which is the one member of that standard whose whole answer is
 * computable without the model's element stack: "The
 * fullscreenEnabled getter steps are to return true if this is allowed to use the "fullscreen" feature and
 * fullscreen is supported, and false otherwise."
 *
 * ITS FIRST CONJUNCT IS THE REASON THE COMPONENT EXISTS AT ALL RATHER THAN THE MEMBER BEING ONE MORE LINE OF
 * document.c. "Allowed to use the "fullscreen" feature" is HTML §4.8.5 "The `iframe` element"'s allowed-to-use
 * over a POLICY-CONTROLLED FEATURE that Fullscreen itself defines — §7 "Permissions Policy Integration": "This
 * specification defines a policy-controlled feature identified by the string "fullscreen". Its default
 * allowlist is 'self'." So landing this member is landing a feature into Permissions Policy §4.1's supported
 * set, and that in turn is what makes §9.4 "Process permissions policy attributes" step 3 — the legacy
 * `allowfullscreen` attribute of `iframe` — an assignment instead of a step over a feature nothing recognises.
 * The three facts are one diff because none of them is observable without the other two.
 *
 * SO THE MEMBER IS NOT A CONSTANT AND ITS TWO ANSWERS ARE BOTH REACHABLE. A top-level document is allowed to
 * use a 'self' feature (§9.7 step 1 gives its navigable an `Enabled` inherited value, and §9.9 falls through to
 * the default allowlist, which 'self' satisfies against the document's own origin), so it reads true. A
 * CROSS-ORIGIN child navigable whose container carries neither `allowfullscreen` nor an `allow` attribute
 * naming the feature reads FALSE — §9.7 step 7 compares against the CONTAINER DOCUMENT's origin — and the same
 * child with `<iframe allowfullscreen>` reads true again, because §9.4 step 3.1 puts §4.7's special value `*`
 * in its container policy. That is three different answers from three real documents, which is what a member
 * has to have before it is worth installing.
 *
 * ══ "FULLSCREEN IS SUPPORTED" IS TRUE HERE, AND THAT IS A DECISION WITH AN ARGUMENT UNDER IT ═════════════════
 *
 * §2: "Fullscreen is supported if there is no previously-established user preference, security risk, or
 * platform limitation." Three disjuncts, and the tempting reading is that this build has the third one — it
 * cannot yet put an element in the top layer, so it cannot display anything fullscreen, so surely that is a
 * platform limitation and the whole predicate is false.
 *
 * THAT READING IS WRONG AND IT IS WRONG IN THE DIRECTION THAT NEVER GETS FIXED. §2's three disjuncts are facts
 * about the ENVIRONMENT and the USER — a preference somebody set, a risk the platform poses, a limitation the
 * platform imposes — and not about how much of the standard an implementation has written. An implementation
 * that has not built fullscreen does not report `fullscreenEnabled === false`; it has no `fullscreenEnabled` at
 * all, which is exactly what this build did until this component and exactly what it still does for
 * `requestFullscreen`. Spelling an unbuilt algorithm as a "platform limitation" would convert a gap this
 * engine must close into a property of the user agent that reads as settled, and it would make the permissions
 * policy conjunct above unobservable behind a constant — CLAUDE.md's two-questions-one-bit defect, performed
 * with the spec's own vocabulary as cover.
 *
 * AND THE STANDARD ITSELF FORBIDS THE READING, in the one place it discusses a missing device. §8 "Security and
 * Privacy Considerations", on keyboard locking: "Similarly, some platforms might not have a keyboard, where
 * user agents may ignore the keyboard lock state. However, this should not affect the web-observable behavior
 * of the requestFullscreen() method or be exposed in other ways, to avoid fingerprinting." A missing device is
 * expressly not allowed to move what the API answers. That is CLAUDE.md's §Headless-is-not-valueless written
 * by the standard, and it settles all three disjuncts for this agent: there is no preference store for a
 * preference to have been established in; §8's security concern is entirely about what the END USER SEES ("User
 * agents should ensure, e.g. by means of an overlay, that the end user is aware something is displayed
 * fullscreen"), and an agent that displays nothing spoofs nobody; and the model — an ordered element stack,
 * per-element flags and two events — is defined with no output device, the pixels being §5 "Rendering", which
 * is presentation.
 *
 * WHAT THAT COSTS, STATED RATHER THAN HIDDEN: a page that reads `document.fullscreenEnabled` now gets `true`
 * where it used to get `undefined`, so `if (document.fullscreenEnabled) el.requestFullscreen()` now ENTERS its
 * branch and throws on the absent member instead of silently skipping it. That is the trade this file is
 * making on purpose. The gate's true arm is code the solver could not reach at all before, and a throw on a
 * member that is not there is CLAUDE.md's forcing function, while a `false` that made the branch vanish would
 * have hidden both the reachable code and the missing member behind one wrong answer.
 *
 * ── NAMED RESIDUAL: THE MODEL'S ELEMENT STACK, AND EVERY MEMBER DEFINED OVER IT ──────────────────────────────
 *   — WHAT IS NOT COVERED: all of §2 except "fullscreen is supported" — the per-element fullscreen flag, iframe
 *     fullscreen flag and keyboard lock flag; the document's FULLSCREEN ELEMENT ("the topmost element in the
 *     document's top layer whose fullscreen flag is set, if any, and null otherwise"); the list of pending
 *     fullscreen events and the RUN THE FULLSCREEN STEPS that drains it; fullscreen/unfullscreen an element,
 *     unfullscreen a document, fully exit fullscreen, and the removing-steps and unloading-cleanup hooks. And
 *     with them all of §3 except this member: `requestFullscreen(options)`, `exitFullscreen()`, `fullscreen`,
 *     `fullscreenElement`, and the `onfullscreenchange`/`onfullscreenerror` event handler IDL attributes on
 *     Element and Document, which have nothing to be dispatched to while nothing runs the fullscreen steps.
 *   — WHAT THE NEXT DIFF BUILDS: the PER-ELEMENT FULLSCREEN FLAG, and an ORDERED READ of the top layer to be
 *     the topmost set element of. The layer itself is NOT part of it and must not be built a second time —
 *     core/css/top_layer.h is css-position-4 §3 "Top Layer" and §3.3 "Top Layer Manipulation", with both
 *     ordered sets and all three manipulation algorithms, and HTML §8.1.7.3 "Processing model"
 *     update-the-rendering step 23 already drains it. What that component deliberately does NOT export yet is
 *     a WALK: it publishes `top_layer_contains` and withholds every derived concept until a caller asks for
 *     one, on the ground that an exported predicate nobody asks is the read-with-no-writer defect wearing the
 *     other costume. So §2's fullscreen element — "the TOPMOST element
 *     in the document's top layer whose fullscreen flag is set" — is the caller that must arrive WITH the
 *     ordered accessor it needs, in the same diff, rather than finding one waiting. Over those two: §2's list
 *     of pending fullscreen events, RUN THE FULLSCREEN STEPS as a step machine (it fires events, so it runs
 *     the page's listeners and must park), and update-the-rendering STEP 12, which is where those steps are
 *     invoked and which rendering.c still asserts is unwritten — its `realm_awaits` there is keyed on
 *     `Document.prototype.exitFullscreen`, so step 12 and that member land together or the probe is a liar.
 *     `requestFullscreen`'s steps 8 to 14 and `exit fullscreen` follow.
 *   — HOW ITS ABSENCE WOULD SHOW: `node engine/idlgen.mjs` reports `requestFullscreen`, `onfullscreenchange`
 *     and `onfullscreenerror` ABSENT on Element, and `fullscreen`, `exitFullscreen`, `fullscreenElement`,
 *     `onfullscreenchange` and `onfullscreenerror` ABSENT on Document. Behaviourally: a page that calls
 *     `el.requestFullscreen()` gets a TypeError for a member that is not installed rather than a fullscreen
 *     session, and NOTHING in this engine can make `fullscreenEnabled`'s answer differ from `allowed to use`,
 *     because the second conjunct has no other value to take.
 *
 * ── NAMED RESIDUAL: [LegacyLenientSetter] ────────────────────────────────────────────────────────────────────
 *   — WHAT IS NOT COVERED: the IDL declares `[LegacyLenientSetter] readonly attribute boolean
 *     fullscreenEnabled`, and this installs a plain readonly accessor. Web IDL §3.4.2 "[LegacyLenientSetter]":
 *     "it indicates that a no-op setter will be generated for the attribute's accessor property. This results
 *     in erroneous assignments to the property in strict mode to be ignored rather than causing an exception to
 *     be thrown." A readonly accessor has no setter at all, so the exception is thrown.
 *   — WHAT THE NEXT DIFF BUILDS: an installer form that gives an attribute §3.7.6's no-op setter, driven off
 *     the extended attribute rather than restated per member — three members of Fullscreen alone carry it, and
 *     it is the kind of §3.4 annotation that is one mechanism for the whole platform or N copies.
 *   — HOW ITS ABSENCE WOULD SHOW: `"use strict"; document.fullscreenEnabled = 1` throws a TypeError here and
 *     is silently ignored in a browser. Sloppy-mode assignment is already silent in both.
 */
#ifndef ENGINE_HOST_BROWSER_CORE_FULLSCREEN_FULLSCREEN_H
#define ENGINE_HOST_BROWSER_CORE_FULLSCREEN_FULLSCREEN_H

#include "quickjs.h"

/* FULLSCREEN §3 "API"'s `partial interface Document`, for one realm. Called from document.c's
   document_install_proto, beside
   the other components that add members to that one prototype — a member of Document is installed where
   Document's prototype is built, never from a host's own list, which is the hand-picked list CLAUDE.md warns
   about. There is no agent-level declaration to pair with it: this member is a readonly accessor with no
   setter id, no per-realm slot and no class of its own, so an init would be an empty function standing for a
   convention rather than doing anything. */
void fullscreen_install_document_members(JSContext *ctx, JSValueConst document_proto);

#endif
