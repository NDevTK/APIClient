/* FULLSCREEN — the Fullscreen API Standard's §2 "Model" and §3 "API". See fullscreen.c.
 *
 * WHAT IS HERE. FULLSCREEN §2 "Model"'s "Fullscreen is supported", its per-element FULLSCREEN FLAG and its
 * per-document FULLSCREEN ELEMENT, and the three members of §3 "API" that are the whole of those three:
 * `fullscreenEnabled` on Document, `fullscreen` on Document, and `fullscreenElement` on
 * DocumentOrShadowRoot — which is to say on Document AND on ShadowRoot.
 *
 * ══ §2's FULLSCREEN ELEMENT, AND THE ORDERED READ IT IS THE FIRST CALLER OF ═══════════════════════════════════
 *
 * FULLSCREEN §2 "Model": "The fullscreen element is the topmost element in the document's top layer whose
 * fullscreen flag is set, if any, and null otherwise." Two facts, in two components, and neither belongs in
 * the other.
 *
 * THE FLAG IS THIS FILE'S. FULLSCREEN §2 "Model": "All elements have an associated fullscreen flag. Unless
 * stated otherwise it is unset." It is an own slot under a private Symbol on the ELEMENT'S WRAPPER, which is
 * where per-flow per-node state lives in this engine — core/html/popover.c holds §6.12's five per-element
 * facts the same way, and the reason is the same: a slot written as a property write is captured by the heap
 * COW delta, so a forked arm that fullscreened an element and one that did not each read back their own flag
 * and a parked flow resumes with the one it had. On the underlying Lexbor node it would be ONE answer for
 * every flow. An ABSENT slot IS "unset", which is the initial value FULLSCREEN §2 "Model" states rather than
 * a hole a default fills.
 *
 * THE ORDER IS THE TOP LAYER'S. "Topmost" is an ORDER over a set the page mutates, and css-position-4 §3 "Top
 * Layer" is the component that owns it — so this file does NOT index that set. core/css/top_layer.h now
 * exports `top_layer_topmost`, an ordered read that answers with the MEMBER and never with its rank, and §2's
 * fullscreen element is the caller it arrived with. That header carries the argument in full: css-position-4
 * §3's own "The top layer (and the pending top layer removals) should not be interacted with directly by
 * specification algorithms", why the last member is the topmost one, and why a recorded POSITION over a set
 * `showPopover()` and `showModal()` mutate renames its referent with every arm still in range.
 *
 * NOTHING SETS THE FLAG YET, AND THAT IS THE ORDER OF §2 RATHER THAN A HOLE IN IT. The only algorithm that sets
 * it is §2's FULLSCREEN AN ELEMENT, whose step 1 runs HTML §6.12 The popover attribute's TOPMOST POPOVER
 * ANCESTOR — which core/html/popover.c DFAILs on by name, because §6.12's Auto/Hint stack is not built — and
 * whose only caller is `requestFullscreen`, which needs §2's list of pending fullscreen events and RUN THE
 * FULLSCREEN STEPS under it. So every document's fullscreen element is null today, and `fullscreen` and
 * `fullscreenElement` answer false and null CORRECTLY: no element in this agent can have the flag set, which is
 * a different statement from a member that cannot say. The residual below names the writer as the next diff.
 *
 * ── The fullscreenEnabled member, whose answer needs none of the above ───────────────────────────────────────
 * FULLSCREEN §3 "API": "The
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
 * FULLSCREEN §2 "Model": "Fullscreen is supported if there is no previously-established user preference,
 * security risk, or platform limitation." Three disjuncts, and the tempting reading is that this build has
 * the third one — it cannot yet put an element in the top layer, so it cannot display anything fullscreen,
 * so surely that is a platform limitation and the whole predicate is false.
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
 *   `fullscreen` and `fullscreenElement` make the SAME trade in a quieter form. They answered `undefined` and
 * now answer `false` and `null`, so no `if` changes direction — both were falsy and both still are. What
 * changes is the question a fullscreen shim actually asks: `'fullscreenElement' in document` and
 * `document.fullscreenElement !== undefined` are how one decides the unprefixed API is present and it need not
 * install a `webkit`-prefixed path, so the shim now proceeds to `requestFullscreen` and throws there. That is
 * the same forcing function one member later, and it is the honest place for it: the member that is missing is
 * the one that throws.
 *
 * ── NAMED RESIDUAL: WHAT SETS THE FLAG, AND EVERYTHING DOWNSTREAM OF IT ──────────────────────────────────────
 *   — WHAT IS NOT COVERED: §2's iframe fullscreen flag and keyboard lock flag (and so "keyboard lock active");
 *     its list of pending fullscreen events and the RUN THE FULLSCREEN STEPS that drains it; FULLSCREEN AN
 *     ELEMENT, UNFULLSCREEN AN ELEMENT, UNFULLSCREEN A DOCUMENT, FULLY EXIT FULLSCREEN, and the removing-steps
 *     and unloading-document-cleanup hooks that run them. With them, all of FULLSCREEN §3 "API" except the
 *     three members here: `requestFullscreen(options)`, `exitFullscreen()`, and the
 *     `onfullscreenchange`/`onfullscreenerror` event handler IDL attributes on Element and Document, which
 *     have nothing to be dispatched to while nothing runs the fullscreen steps. The FLAG THEREFORE HAS A
 *     READER AND NO WRITER — which is a broken contract when it is an accident and is the spec's own order
 *     when it is not: §2's one setter is fullscreen an element, and that is genuinely blocked rather than
 *     merely unwritten (below).
 *   — WHAT THE NEXT DIFF BUILDS: §2's FULLSCREEN AN ELEMENT and UNFULLSCREEN AN ELEMENT, which are what put the
 *     flag's writer in the tree. Fullscreen an element's step 1 is "Let hideUntil be the result of running
 *     topmost popover ancestor given element, null, and false" and its step 2 runs hide popovers until.
 *     core/html/popover.c HAS THE FIRST NOW — topmost popover ancestor is built there over the two showing
 *     popover lists it derives from css-position-4 §3's top layer, and it is static because this caller is not
 *     in the tree yet, so building fullscreen an element is also what EXPORTS it. What popover.c still lacks is
 *     HIDE POPOVER STACK UNTIL, which §6.12's hide popovers until is two calls to; its show popover step 15.3
 *     DFAILs there and names what it needs first (a re-entrant hide a popover, because that algorithm fires
 *     `beforetoggle` at the page). So the popover half is still a real prerequisite and not a citation, and it
 *     is now ONE algorithm rather than two. Over those: §2's list
 *     of pending fullscreen events, RUN THE FULLSCREEN STEPS as a step machine (it fires events, so it runs the
 *     page's listeners and must park), and update-the-rendering STEP 12, which is where those steps are invoked
 *     and which core/rendering/rendering.c still asserts is unwritten — its `realm_awaits` there is keyed on
 *     `Document.prototype.exitFullscreen`, so step 12 and that member land together or the probe is a liar.
 *     `requestFullscreen`'s steps 8 to 14 and `exit fullscreen` follow.
 *   — HOW ITS ABSENCE WOULD SHOW: `node engine/idlgen.mjs` reports `requestFullscreen`, `onfullscreenchange`
 *     and `onfullscreenerror` ABSENT on Element, and `exitFullscreen`, `onfullscreenchange` and
 *     `onfullscreenerror` ABSENT on Document. Behaviourally: `el.requestFullscreen()` is a TypeError for a
 *     member that is not installed rather than a fullscreen session, and `document.fullscreenElement` is null
 *     for EVERY document however the page tries to enter fullscreen — so the day it is not null is the day the
 *     writer exists, which is the one observation that distinguishes this residual from a walk that is wrong.
 *
 * ── NAMED RESIDUAL: [LegacyLenientSetter] ────────────────────────────────────────────────────────────────────
 *   — WHAT IS NOT COVERED: the IDL declares `[LegacyLenientSetter] readonly attribute boolean
 *     fullscreenEnabled`, `[LegacyLenientSetter, Unscopable] readonly attribute boolean fullscreen` and
 *     `[LegacyLenientSetter] readonly attribute Element? fullscreenElement`, and this installs three plain
 *     readonly accessors. Web IDL §3.4.2 "[LegacyLenientSetter]":
 *     "it indicates that a no-op setter will be generated for the attribute's accessor property. This results
 *     in erroneous assignments to the property in strict mode to be ignored rather than causing an exception to
 *     be thrown." A readonly accessor has no setter at all, so the exception is thrown.
 *   — WHAT THE NEXT DIFF BUILDS: an installer form that gives an attribute §3.7.6's no-op setter, driven off
 *     the extended attribute rather than restated per member — all three members of Fullscreen carry it, and
 *     it is the kind of §3.4 annotation that is one mechanism for the whole platform or N copies.
 *   — HOW ITS ABSENCE WOULD SHOW: `"use strict"; document.fullscreenEnabled = 1` throws a TypeError here and
 *     is silently ignored in a browser. Sloppy-mode assignment is already silent in both.
 *
 * ── NAMED RESIDUAL: [Unscopable] ─────────────────────────────────────────────────────────────────────────────
 *   — WHAT IS NOT COVERED: `fullscreen` alone of the three carries `[Unscopable]`, and this engine has no
 *     mechanism for that annotation at all — DOM's `[CEReactions, Unscopable]` `prepend`, `append`,
 *     `replaceChildren` and `slot` are installed here without it too, so this member joins a platform-wide
 *     absence rather than opening one.
 *   — WHAT THE NEXT DIFF BUILDS: the `@@unscopables` object on an interface prototype, populated from the
 *     extended attribute by the same installer the residual above describes — one mechanism for the platform,
 *     since five members across two standards already want it.
 *   — HOW ITS ABSENCE WOULD SHOW: `with (document) { fullscreen }` reads the member here and reads the outer
 *     scope's binding in a browser. It is the same one-line test for `slot` and `append`.
 */
#ifndef ENGINE_HOST_BROWSER_CORE_FULLSCREEN_FULLSCREEN_H
#define ENGINE_HOST_BROWSER_CORE_FULLSCREEN_FULLSCREEN_H

#include <lexbor/dom/dom.h>

#include "quickjs.h"

/* Declared ONCE PER AGENT — §2's fullscreen-flag slot key, which identifies the same flag on every element in
   every realm. It is called from core/dom/document.c's document_init and given back from document_agent_free,
   which is the convention core/html/focus.h and core/html/autofocus.h state from their own side: a
   sub-component whose INSTALL runs from document_install_proto is DECLARED by document_init, so that the
   declaration is paired with the install rather than copied into each host's own list. */
void fullscreen_init(JSContext *ctx);
void fullscreen_free(JSRuntime *rt);

/* FULLSCREEN §2 "Model"'s FULLSCREEN ELEMENT of `document`: "the topmost element in the document's top layer
   whose fullscreen flag is set, if any, and null otherwise". OWNED, and JS_NULL when there is none — which is
   every document in this build, for the reason the first named residual gives.
   IT IS EXPORTED FOR A CALLER OUTSIDE FULLSCREEN §3 "API", and that caller is the reason the concept is a
   component's rather than a getter's private helper: HTML §6.10.1 "Close requests"' CLOSE REQUEST STEPS
   step 1 is "If document's fullscreen element is not null", whose two sub-steps fully exit fullscreen and
   RETURN — so a fullscreen document short-circuits the whole algorithm and never reaches step 7's process
   close watchers. Fullscreen therefore establishes no close watcher and registers nothing with
   core/html/close_watcher.c; it is a
   higher-priority arm of the same nine steps, and whichever component owns those nine steps asks THIS at its
   step 1 and close_watcher at its step 7. Neither half may grow a private copy of the other's question. */
JSValue fullscreen_element(JSContext *ctx, lxb_dom_node_t *document);

/* FULLSCREEN §3 "API"'s `partial interface Document`, for one realm. Called from document.c's
   document_install_proto, beside
   the other components that add members to that one prototype — a member of Document is installed where
   Document's prototype is built, never from a host's own list, which is the hand-picked list CLAUDE.md warns
   about. */
void fullscreen_install_document_members(JSContext *ctx, JSValueConst document_proto);

/* FULLSCREEN §3 "API"'s `partial interface mixin DocumentOrShadowRoot`, whose one member `fullscreenElement`
   Document carries above and ShadowRoot carries here — ONE call per interface that includes the mixin, the shape
   core/css/style_sheet_list.h and core/html/focus.h already use for the same mixin. It is installed rather
   than inherited because DocumentFragment does not include DocumentOrShadowRoot, so nothing on ShadowRoot's
   prototype chain could have supplied it. */
void fullscreen_install_shadow_root_members(JSContext *ctx, JSValueConst shadow_root_proto);

#endif
