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
 *             feature's DEFAULT ALLOWLIST of 'self', narrowed by nothing. It is computed HERE rather than by
 *             core/permissions_policy/permissions_policy.h because none of this file's features is in that
 *             component's Permissions Policy §4.1 supported-feature set — see permission_store.c's step 4,
 *             which states what that costs and how the two answers differ.
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
 * §5.2's REQUEST PERMISSION IS HERE, AND ITS CALLER IS THE FIRST POWERFUL FEATURE THIS ENGINE HOSTS. File
 * System Access §2.2 registers the "file-system" feature and §2.3.2's `handle.requestPermission()` runs its
 * permission request algorithm, whose last-but-one step is "Request permission to use desc" — this. So the
 * algorithm arrives with a call site, which is the condition this file previously stated for writing it.
 *   §5.3's PROMPT THE USER TO CHOOSE is still absent, and its absence is now a fact about the FEATURE rather
 * than about this engine: §5.3 is invoked by a feature that asks the user to pick from a SET OF OPTIONS (a
 * camera, a MIDI port), and "file-system"'s own algorithms invoke §5.2 and never §5.3. It arrives with the
 * first feature whose permission is a choice among options rather than a yes/no.
 *   §5.4's REACT TO THE USER REVOKING PERMISSION is likewise still absent, for the reason `remove a permission
 * store entry` states below: its first step runs the FEATURE's permission revocation algorithm, and
 * "file-system" declares none — File System Access §2.2 defines a descriptor type, permission state
 * constraints and a permission request algorithm, and leaves every other column of §4's registry at its
 * default. */
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
   each a single `boolean X = false`, and File System Access's `mode` is a two-valued ENUMERATION, which is the
   same one bit read through a different conversion (permission_feature_aspect_values). A feature declaring two
   MEMBERS is a second bit here and a second read in the conversion, and the registry's own assert names it.
 *
 * AND THE SUBJECT, WHICH IS THE MEMBER THAT SAYS *WHICH* INSTANCE OF THE FEATURE THIS DESCRIPTOR IS ABOUT.
 * `FileSystemPermissionDescriptor` declares `required FileSystemHandle handle`, and it is the first registered
 * descriptor type whose members are not all facts about the ORIGIN: two handles in one document can hold two
 * different permission states, and the feature's own permission state constraints are written over the ENTRY
 * the handle locates. It is a BORROWED value — the descriptor is a stack record living for the length of one
 * algorithm, exactly as §3.3's descriptor is — and it is JS_UNDEFINED for every feature whose registry row
 * names no subject member, which every other row does.
 *   IT IS NOT PART OF THE STORE KEY, and that is the FEATURE's decision rather than this file's: §3.2 keys an
 * entry by its descriptor, and "file-system"'s permission state constraints REWRITE a descriptor to the one
 * whose state it must equal before any entry is looked up (see permission_constraints_declare), so the
 * descriptors that ever reach the store are already canonical. A feature whose constraints do NOT collapse
 * this way is a feature whose subject belongs in the key, and the store's own assert is what will say so. */
typedef struct { int feature; bool aspect; JSValueConst subject; } PermissionDescriptor;

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
/* THE ASPECT MEMBER'S TYPE, as the only thing about it that differs between features: a NULL-terminated list of
   the values an ENUMERATION admits, whose FIRST entry is the IDL's own default and whose SECOND is the value
   that sets the aspect bit; or NULL where the member is a `boolean X = false` and ToBoolean is the whole
   conversion. Web IDL §3.2.18 makes a value outside the list a TypeError, which for a promise-returning
   operation is a rejection — so the list is part of the TYPE and the conversion must not fall back to
   ToBoolean, under which `{mode:"read"}` and `{mode:"readwrite"}` are the same descriptor. */
const char *const *permission_feature_aspect_values(int feature);
/* THE SUBJECT MEMBER — the `required` member whose value identifies WHICH INSTANCE of the feature a descriptor
   is about, or NULL where the descriptor type declares none. §6.2.1 step 5 reads it and a missing one is a
   TypeError (Web IDL §3.2.17: for a dictionary, `undefined` IS absent); the value itself is opaque to this
   component, which is why the feature's own component supplies the test below. */
const char *permission_feature_subject(int feature);
/* IS THIS VALUE THE SUBJECT MEMBER'S DECLARED TYPE — Web IDL §3.2.15's brand test, performed by the component
   that owns the interface because this one must not learn what a FileSystemHandle is. Declared ONCE PER AGENT
   by that component, from its own `_init`. */
typedef bool (*PermissionSubjectFn)(JSValueConst v);
void permission_subject_declare(int feature, PermissionSubjectFn is);
bool permission_subject_is(int feature, JSValueConst v);

/* §4's PERMISSION STATE CONSTRAINTS — "constraints on the values that the user agent can return as a
 * descriptor's permission state", registered by the component that owns the feature for the same reason the
 * brand test is: the constraints are written over the feature's OWN objects.
 *
 * TWO ANSWERS, because that is what the two shapes of constraint the platform writes actually are. A constraint
 * may FIX the state ("if entry represents a file system entry in a bucket file system, this descriptor's
 * permission state must always be granted") — `*fixed` takes a PERMISSION_* value and §5.1 answers it without
 * reading the store and without asking the unknown. Or it may say a descriptor's state "must be EQUAL to the
 * permission state for" ANOTHER descriptor — `*out` takes that one and §5.1 continues over it, which is what
 * makes an entry written for the canonical descriptor the answer for every descriptor constrained to equal it.
 * Returning false leaves §5.1's own steps to decide.
 *   IT RUNS BEFORE §5.1 STEP 2, because "must ALWAYS be" admits no earlier step overriding it. */
typedef bool (*PermissionConstraintFn)(JSContext *ctx, const PermissionDescriptor *d,
                                       PermissionDescriptor *out, int *fixed);
void permission_constraints_declare(int feature, PermissionConstraintFn fn);
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

/* §5.2's REQUEST PERMISSION TO USE A DESCRIPTOR — "this algorithm returns either granted or denied".
 *
 *   1. Let current state be the descriptor's permission state.
 *   2. If current state is not "prompt", return current state and abort these steps.
 *   3. Ask the user for express permission for the calling algorithm to use the powerful feature described by
 *      descriptor.
 *   4. If the user gives express permission to use the powerful feature, set current state to "granted";
 *      otherwise to "denied".
 *   5. Let settings be the current settings object.  6. Let key be the result of generating a permission key.
 *   7. Queue a task on the current settings object's responsible event loop to set a permission store entry
 *      with descriptor, key, and current state.  8. Return current state.
 *
 * STEP 3 IS THE FORK AND STEP 7 IS WHAT ENDS IT. The user's express permission is the same unknown §5.1 step 8
 * is — a decision taken in a UI this engine does not host — so it is asked through the same seam and BOTH arms
 * run: a bundle's granted path (the read, the upload, the endpoints behind it) and its denied path (the
 * fallback, the message, the re-prompt) are two worlds and a C `if` here would delete one. Outcome 0 is
 * "granted", and that numbering is this algorithm's own rather than §5.1's: step_fork_run's rule is that
 * outcome 0 is what a run with no forking policy takes, and for a REQUEST the ordinary completion is the one
 * the calling algorithm was written to continue into.
 *   AND THEN IT IS KNOWN. Step 7 writes the store entry, so every later read in this flow — §5.1 step 7,
 * another status's, a second requestPermission() — answers the decision rather than forking over it again.
 * Step 7 says "queue a task", and the store is written directly instead: the entry's only observable is a
 * later read, every later read in this flow is ordered after this algorithm's own return, and a task that
 * carried the write would have to carry the value §5.1 has already returned to the caller.
 *   `phase` IS THE CALLER'S BYTE, exactly as permission_state_run's is — this algorithm asks §5.1's chain and
 * then its own question, and which of them a parked flow is at cannot live in a C local. It starts at zero and
 * an answered request leaves it at zero. THE CALLER STILL OWES THIS ONE A STAGE OF ITS OWN: a stage holding
 * this and any other request would restart one of the two phases on every re-entry.
 *   AND THIS ALGORITHM'S OWN PHASE IS NUMBERED ABOVE §5.1's WHOLE SPACE, because it DELEGATES into that chain
 * through this same byte. Sharing a value with it does not merely confuse a label: §5.1 parks at its second
 * question, this algorithm reads that value as its own, step 1 is abandoned, the driver's answer to §5.1's
 * question is consumed by step 3's ask — a real arm, in range, recorded under the other question's key — and
 * step 2 never gets to return the decision the user had already taken, so step 7 overwrites it. A delegating
 * chain's phases begin where the delegated one's stop; see the enum in permission_store.c, and see
 * quickjs-step.h's `fork_ask_key` for the assert that now catches the general shape of this at the seam.
 *   Returns JS_STEP_FORK (the caller returns it and is re-entered at this same call site) or 0 with *out set to
 * PERMISSION_GRANTED or PERMISSION_DENIED — never PERMISSION_PROMPT, which is what "returns either granted or
 * denied" means and what the assert at the end of it states. */
int permission_request_run(JSContext *ctx, JSStepHdr *h, uint8_t *phase, const PermissionDescriptor *d,
                           int *out);

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
