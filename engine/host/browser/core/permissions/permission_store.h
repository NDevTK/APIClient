/* PERMISSIONS §3's MODEL AND §5.1's READ — the powerful features, the permission store, and what a
 * descriptor's permission state IS. See permission_store.c.
 *
 * THE ONE DECISION THIS COMPONENT EXISTS TO GET RIGHT: a permission state is UNKNOWN EXTERNAL STATE, and it is
 * unknown in exactly the sense core/dom/abort.c draws the line at. §3.1: "A permission represents a USER'S
 * DECISION to allow a web application to use a powerful feature." The decision is the user's, taken in a
 * browser UI this engine does not host, at a moment this engine cannot observe — so an engine that answers
 * "prompt" is not reporting a state it read, it is reporting that it has no user, which is a fact about its
 * INPUTS written down as a fact about the WORLD. That is the same defect core/html/user_activation.c was
 * written to delete, one specification over.
 *   And BOTH answers reach code worth running. `navigator.permissions.query({name:"notifications"})` then
 * `status.state === "granted"` routes a real bundle into its subscribe/registration path — endpoints, keys,
 * a service-worker registration — while "prompt" routes it into a banner and "denied" into a fallback. A
 * concrete answer here deletes two of those three worlds and everything they reach, which is precisely the
 * failure CLAUDE.md names for a loaded `features.admin:false`.
 *
 * SO THE UNKNOWN IS A SOURCE AND THE STORE IS NOT. §5.1 is a sequence of decisions and only its LAST step is
 * ignorant:
 *   step 2  — a non-secure context is "denied". COMPUTED: core/frame/secure_context.c knows this realm's
 *             environment, so there is nothing unknown and nothing to fork.
 *   step 4  — a document not allowed to use the feature is "denied". COMPUTED from the policy-controlled
 *             feature's DEFAULT ALLOWLIST, which this engine does not narrow (it parses no Permissions-Policy
 *             header and no `allow` attribute), exactly as core/html/focus.c computes the same question.
 *   step 7  — a permission STORE ENTRY is returned as it stands. KNOWN: an entry exists only because this
 *             engine WROTE it, and a value the engine wrote is a value it knows — `new AbortController()
 *             .signal.aborted` is false for the identical reason. A concolic store entry would fork a world
 *             its own writer contradicts.
 *   step 8  — "the permission state of feature, taking into account any permission state constraints". THE
 *             USER'S INTENT. Nothing in this engine has observed it, so this is the concolic: the domain is
 *             §3.1's three states and the EXAMPLE is the feature's own DEFAULT PERMISSION STATE, which §4 says
 *             is "prompt" unless the feature says otherwise.
 *
 * THE UNKNOWN IS PER FEATURE AND PER AGENT, WHICH IS ALSO HOW THE ASPECT ORDER IS DISCHARGED. §3.2's store is
 * the USER AGENT's ("The user agent maintains a single permission store"), and the user's decision is about an
 * ORIGIN rather than about a document — so one source per feature serves every realm of this agent, and a flow
 * that pins `camera` in one document has pinned it in every other. That also answers §4's PARTIAL ORDER
 * without a constraint solver: `{name:"camera", panTiltZoom:true}` is stronger than `{name:"camera",
 * panTiltZoom:false}`, so the strong one being granted forces the weak one, and reading BOTH out of one source
 * makes them equal — which satisfies the implication in every world rather than fabricating one where a user
 * granted pan-tilt-zoom and refused the camera. The order is discharged by IDENTITY, exactly as §6.4.1's
 * history-action question is when the two timestamps hold the same value. Where the two are separately KNOWN —
 * two store entries — the implication is evaluated instead, because there the answers are facts and can
 * disagree.
 *
 * WHAT THIS FILE DOES NOT DECIDE: §5.2's request permission and §5.3's prompt the user to choose. Both are
 * algorithms a POWERFUL FEATURE invokes ("ask the user for express permission for the CALLING ALGORITHM"), and
 * this engine has no powerful feature's API — no Geolocation, no Notification, no getUserMedia. Writing them
 * with no caller is a design that has never run; they arrive with the first feature that asks. */
#ifndef ENGINE_HOST_BROWSER_CORE_PERMISSIONS_PERMISSION_STORE_H
#define ENGINE_HOST_BROWSER_CORE_PERMISSIONS_PERMISSION_STORE_H

#include <stdbool.h>
#include <stdint.h>

#include "quickjs.h"
#include "quickjs-step.h"

/* §6.3's `enum PermissionState { "granted", "denied", "prompt" }`, in the IDL's own order — the order is the
   enum's identity, not a convenience, because it is what a serialized store entry and a parked flow's decision
   both mean by an integer. */
enum { PERMISSION_GRANTED = 0, PERMISSION_DENIED, PERMISSION_PROMPT, PERMISSION_STATE_N };
/* The enum value's string, and its inverse. `permission_state_of` answers -1 for a string that is not one of
   §3.1's three, which is the only honest answer and the one every caller has to handle. */
const char *permission_state_str(int state);
int         permission_state_of(const char *s);

/* §3.3's POWERFUL FEATURE and §3.3.1's ASPECT, as the pair a descriptor is. `feature` indexes the registry
   below; `aspect` is the value of the feature's ONE declared aspect member and is false for a feature whose
   permission descriptor type is the default `PermissionDescriptor`.
     ONE aspect member, because that is what every registered feature's descriptor type declares — Push's
   `userVisibleOnly`, Web MIDI's `sysex`, Media Capture's `panTiltZoom`, Clipboard's `allowWithoutGesture` are
   each a single `boolean X = false`. A feature declaring two is a second bit here and a second read in the
   conversion, and the registry's own assert names it. */
typedef struct { int feature; bool aspect; } PermissionDescriptor;

/* Declared ONCE PER AGENT. The store, the per-feature sources and the key are the agent's, not a realm's —
   §3.2 says "the user agent maintains a SINGLE permission store", and SECURITY.md makes an instance one
   origin, so one store answers every realm this agent builds. */
void permission_store_init(JSContext *ctx);
/* Released with the AGENT. It takes no context because the free list it is reached from does not have one at
   that point — the values it owns are runtime-lifetime and JS_FreeValueRT is what releases those. */
void permission_store_free(void);

/* §6.2.1 step 4's "is not supported", answered from §4's registry: the index of the feature this name
   identifies, or -1. A name this user agent does not support is a TypeError at the query, which is the spec's
   own answer and what a feature-detecting bundle reads. */
int         permission_feature_of(const char *name);
const char *permission_feature_name(int feature);
/* The feature's ASPECT MEMBER — the one member its permission descriptor type declares beyond
   `PermissionDescriptor`'s `name`, or NULL where the type is the default one. The query's conversion reads it
   off the page's object; nothing else needs to know it exists. */
const char *permission_feature_aspect(int feature);
/* §4's DEFAULT PERMISSION STATE for the feature — "an PermissionState value that serves as a permission's
   default state ... If not specified, the permission's default state is prompt". It is the EXAMPLE the
   feature's source carries and the first arm of every chain asked over it, so both places read it from here
   rather than assuming which one it is. */
int         permission_feature_default(int feature);

/* §5.1 "a descriptor's permission state", given the current settings object — which is `ctx`'s realm.
   OWNED. The answer is a `PermissionState` STRING, because that is what §6.3.3's attribute returns and what a
   page compares against; it is CONCRETE where §5.1 decided (a non-secure context, a policy-denied document, a
   store entry) and the feature's CONCOLIC source where §5.1's last step reaches the user's intent. */
JSValue permission_state(JSContext *ctx, const PermissionDescriptor *d);

/* THE SAME READ FOR A C CALLER THAT MUST BRANCH ON IT — and the reason it cannot be the function above.
 *
 * A powerful feature's own algorithm asks "is this granted" and then does one of two entirely different things
 * (Geolocation acquires a position or fires a PERMISSION_DENIED PositionError; the Screen Wake Lock request
 * resolves or rejects with NotAllowedError). Over an unknown state a C `if` picks one of those and DELETES the
 * other, which is the single failure the solver exists to prevent — so the question is asked through
 * step_fork_run, the caller is a STEP MACHINE, and the sibling arm is snapshotted as its own flow.
 *   THREE STATES ARE TWO CHAINED QUESTIONS, not one three-armed fork, and the chaining is what keeps the
 * worlds consistent. The outcome seam prepares ONE sibling per ask — it says so at its own assert — so a
 * machine declaring three completions aborts rather than losing an arm silently. The chain asks first "is the
 * user's decision the feature's DEFAULT permission state" and only inside that question's false arm "which of
 * the other two is it", which produces exactly three worlds and no fourth that contradicts itself. Outcome 0
 * of the first is the default, which is step_fork_run's one numbering rule: outcome 0 is the arm a run with no
 * forking policy takes, and for a permission that is "the user has not decided", never "granted".
 *   `phase` IS THE CALLER'S BYTE, exactly as core/html/user_activation.h's is and for the identical reason: a
 * chain of two asks cannot remember which of them is outstanding in a C local, because the machine returns
 * between them. It starts at zero, and an answered chain leaves it at zero, so one byte serves any number of
 * successive questions. Each SUCCESSIVE question still needs its own stage on the calling machine.
 *   Returns JS_STEP_FORK (the caller returns it and is re-entered at this same call site) or 0 with *out set
 * to one of the PERMISSION_* values. */
int permission_state_run(JSContext *ctx, JSStepHdr *h, uint8_t *phase, const PermissionDescriptor *d, int *out);

/* §3.2's STORE OPERATIONS, over the one store. `get` answers -1 where there is NO ENTRY — a different thing
   from an entry whose state is "prompt", and §5.1 step 7 branches on exactly that difference: one is the
   user's decision, the other is its absence.
     `set` is §3.2's "set a permission store entry", and its caller is §6.3.4's update steps: the moment the
   user agent BECOMES AWARE of a state it knows that state, which is what §5.2 step 7 writes and what §5.1
   step 7 reads back.
     §3.2's "REMOVE a permission store entry" is not here, and its absence is not an oversight. Its only caller
   in the standard is §5.4's react to the user revoking permission, whose first step runs the FEATURE's
   permission revocation algorithm — and this engine has no powerful feature's API to revoke. It arrives with
   the first one that does, together with the revocation algorithm that is the rest of §5.4. */
int  permission_store_get(JSContext *ctx, const PermissionDescriptor *d);
void permission_store_set(JSContext *ctx, const PermissionDescriptor *d, int state);

/* THE FEATURE'S SOURCE — the value §5.1 step 8 answers with when nothing is known, handed out so that
   §6.3.4's "the user agent is aware that the state has changed" can be asked OVER IT. It is the same object
   every read of that feature returns, so a flow's constraint over it is one fact however many statuses ask.
   OWNED. */
JSValue permission_unknown(JSContext *ctx, int feature);

#endif
