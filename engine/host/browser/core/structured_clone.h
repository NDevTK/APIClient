/* STRUCTURED SERIALIZATION — HTML §2.7. See structured_clone.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_STRUCTURED_CLONE_H
#define ENGINE_HOST_BROWSER_CORE_STRUCTURED_CLONE_H
#include <stdint.h>

#include "quickjs.h"

/* §2.7.10's `structuredClone` global. The DECLARATION is the agent's — `structuredClone(any value, optional
   StructuredSerializeOptions options = {})` names a dictionary whose `transfer` member is a `sequence<object>`,
   and every part of reading one is the page's code, so it is converted by the one IDL machine and not by this
   file. This used to be a bare JS_NewCFunction with no declaration at all — the only component in PLATFORM
   whose declare column was NULL — and its body read `options.transfer` with JS_GetPropertyStr from C. */
void structured_clone_init(JSContext *ctx);
void structured_clone_install(JSContext *ctx, JSValueConst global);
/* Agent teardown — core/platform.h's release column. It gives back §2.7.10's pool entry and the registry of
   transferable interfaces below, which is the AGENT's: a count carried into a second agent is a platform that
   reports interfaces registered by a runtime that no longer exists. */
void structured_clone_free(JSRuntime *rt);

/* StructuredDeserialize(StructuredSerialize(v)) — a deep, cycle-preserving copy in this realm. Every caller in
   the platform that says "a serialized copy" performs this: a MessagePort delivering a message, a window post,
   §4.9.1's tee with cloneForBranch2. Answers a new owned value, or JS_EXCEPTION with the "DataCloneError"
   DOMException live for a value §2.7 refuses (a function, a Proxy, a Promise, a Symbol, a platform object).
   THE LAST ENTRY IS THE ONES WITH NO ROW, WHICH IS THE STANDARD'S ANSWER AND NOT THIS ENGINE'S: HTML §2.7.1
   "Serializable objects" says a platform object IS serializable "if their primary interface is decorated with
   the [Serializable] IDL extended attribute", and HTML §2.7.3 step 20 refuses only the ones that are not. A
   registered interface takes step 19 instead — see the §2.7.1 block below. THIS SENTENCE USED TO READ THAT
   EVERY platform object is refused because §2.7.1 had no arm in this engine at all, and it is rewritten rather
   than dropped because a reader who finds the seam and re-derives the old reason will re-add the refusal to
   the list of things this file owns. What the file owns is which values are refused; WHICH platform objects
   are serializable is the registry's, and the registry is the INTERFACES'. */
JSValue structured_clone(JSContext *ctx, JSValueConst v);

/* §2.7's TWO OPERATIONS, SEPARATELY — which is what the standard defines and what a MessagePort needs. A post
   SERIALIZES at the moment it is called, because that is where a "DataCloneError" is observable to the caller,
   and DESERIALIZES in the delivery task, because that is where the standard puts it and because the message
   must be a copy made for the receiver. Doing both at once would move the throw to the wrong turn of the event
   loop and hand the receiver a value built in the sender's turn. */
typedef struct { uint8_t *buf; size_t len; } StructuredData;
/* Returns 0 with `*out` owned by the caller, or -1 with the "DataCloneError" DOMException live. */
int  structured_serialize(JSContext *ctx, JSValueConst v, StructuredData *out);
/* The other half, into THIS realm. A value the writer produced and the reader refuses is a should-never-happen
   and crashes; no page input can cause it. */
JSValue structured_deserialize(JSContext *ctx, const StructuredData *in);
void structured_data_free(JSContext *ctx, StructuredData *d);

/* ---- §2.7.2's TRANSFERABLE OBJECTS ---------------------------------------------------------------------------
 *
 * A transferable is not cloned, it is MOVED: §2.7.2 gives its interface two algorithms — TRANSFER STEPS that
 * empty the object into a data holder and detach the original, and TRANSFER-RECEIVING STEPS that build a new
 * object in the target realm out of that holder. The interface that owns them registers them here, because the
 * serializer must not know what a MessagePort is any more than it knows what a Response is.
 *
 * `type` IS §2.7.7's `interfaceName`, and it is the dataHolder's [[Type]] — the field §2.7.8 chooses the
 * receiving steps BY. It is not decoration: with it absent there could only ever be ONE transferable in the
 * whole engine, because a holder arriving at the other end named nothing and the deserializer had to guess.
 *
 * The HOLDER is a JS value of the registering component's own choosing — it is never seen by a page, and making
 * it a JS value rather than a C record is what lets it ride the frontier and be parked like everything else. */
typedef struct {
    const char *type;                                      /* §2.7.7's interfaceName = dataHolder.[[Type]] */
    bool    (*is)(JSValueConst v);
    JSValue (*out)(JSContext *ctx, JSValueConst v);        /* the holder, or JS_EXCEPTION with a throw live */
    JSValue (*in)(JSContext *ctx, JSValueConst holder);    /* the new object in this realm */
} StructuredTransferable;
void structured_register_transferable(const StructuredTransferable *t);

/* ---- HTML §2.7.1's SERIALIZABLE OBJECTS ---------------------------------------------------------------------
 *
 * The transferables above MOVE and detach; a serializable is COPIED. HTML §2.7.1 "Serializable objects" makes
 * the property one of an INTERFACE — "Platform objects can be serializable objects if their primary interface
 * is decorated with the [Serializable] IDL extended attribute" — and requires that interface to "define the
 * following algorithms": serialization steps and deserialization steps. So the row is the INTERFACE'S, exactly
 * as a transferable's two algorithms are, and this file knows what a CryptoKey is no more than it knows what a
 * MessagePort is.
 *
 * `name` IS HTML §2.7.3 step 19's "the identifier of the primary interface of value", and it is the key at
 * BOTH ends: step 19 writes it as serialized.[[Type]] and HTML §2.7.6 step 22 reads it back as "Let
 * interfaceName be serialized.[[Type]]" and chooses by it. IT IS NEVER A ROW INDEX, and that is not a style
 * preference — these bytes outlive the turn (a history entry, a broadcast, a routed message, the IDB cold
 * tier), so an ordinal would name a different interface the moment a registrant is added, which is one park
 * away rather than one release away.
 *
 * `out` IS THE SERIALIZATION STEPS AND `create`/`fill` ARE THE DESERIALIZATION ONES, SPLIT IN TWO BECAUSE
 * §2.7.6 SPLITS THEM. Step 22 creates the instance, step 23 is "Set memory[serialized] to value", and only
 * step 24 performs the deserialization steps — so the instance is in the reference map BEFORE any sub-value is
 * read, and HTML §2.7.1 says the steps receive a value that "will be a newly-created instance of the platform
 * object type in question, with none of its internal data set up; setting that up is the job of these steps".
 * A row is therefore TWO operations and not one.
 *
 * THE HOLDER IS ONE JS VALUE OF THE ROW'S OWN CHOOSING, WHICH IS THIS ENGINE'S CHOICE AND NOT A SPEC COUNT.
 * §2.7.3 step 26's third arm lets an interface's steps write as many fields of `serialized` as it likes and
 * perform a sub-serialization for each; a row here answers with ONE value, and a row with several fields puts
 * them in a record the same walk reaches. That is what keeps it a SUB-serialization: the holder rides the
 * writer's own work stack under the ONE `memory`, so a cycle through a platform object terminates and a graph
 * reaching one twice comes back as one object. A nested JS_WriteObject would open a second object list and do
 * neither.
 *
 * WHAT A ROW MAY ASSERT, AND WHAT IT MAY NOT. `out` reads state THIS engine wrote, so a malformed slot is this
 * codebase's own logic being wrong and is a DCHECK. `fill` reads a holder that came back through bytes, and in
 * the SAME-TURN clone those bytes are this agent's own — see the residual below for the path where they are
 * not. The value a page hands `structuredClone` is never any of this: a shape §2.7 refuses gets the
 * "DataCloneError" the standard names, which is a refusal and not an assert, because an assert on page input
 * is a page-held abort switch. */
typedef struct {
    const char *name;                                  /* §2.7.3 step 19's identifier of the primary interface */
    bool    (*is)(JSContext *ctx, JSValueConst v);     /* step 19's "a platform object that is a serializable
                                                          object", for THIS interface */
    JSValue (*out)(JSContext *ctx, JSValueConst v);    /* the serialization steps, as one holder; or
                                                          JS_EXCEPTION with a throw live */
    JSValue (*create)(JSContext *ctx);                 /* §2.7.6 step 22's new instance, no data set up */
    int     (*fill)(JSContext *ctx, JSValueConst v, JSValueConst holder);  /* the deserialization steps */
} StructuredSerializable;
void structured_register_serializable(const StructuredSerializable *s);

/* NAMED RESIDUAL — narrower than §2.7.6 and CORRECT for what it does, so there is nothing here to crash on.
 * WHAT IS NOT COVERED: a record whose bytes this agent did not write. Every refusal the READER can make —
 * this seam's unknown interface name, a Symbol tag, a transfer reference with no map — is turned into an abort
 * by structured_deserialize's DFAIL, on the argument that a graph the writer produced and the reader refused
 * is the two halves disagreeing. That argument holds for every caller this file has today and stops holding
 * the day a ROUTED message carries a body: those bytes were written by another instance, which SECURITY.md
 * grades as attacker-controlled, and §2.7.6 step 22's own answer for an interface the realm does not expose is
 * a "DataCloneError" rather than a crash.
 * WHAT THE NEXT DIFF BUILDS: a reader refusal this file can tell apart from a format disagreement — one
 * channel out of JS_ReadObject4 that says WHICH of the two it was — after which structured_deserialize
 * re-reports the first as the DOMException §2.7.6 names and keeps the DFAIL for the second.
 * HOW ITS ABSENCE WOULD SHOW: an abort in a delivery task, naming the writer/reader disagreement, on a
 * message a peer instance composed — observable as the engine dying on a document it did not itself
 * serialize. It is not shown by any row of a run that only ever clones in one agent. */

/* THE TRANSFER LIST IS A DECLARED IDL TYPE AND NOT A WALK THIS FILE PERFORMS. It used to be one — a
 * function reading `list.length` and one index per entry — and that is the ARRAY-LIKE algorithm rather than
 * §3.2.21's, so a page's iterable that is not an Array converted to nothing, and every
 * one of those reads could be an accessor or a Proxy trap run from an activation with no flow base under it.
 * IDL_SEQUENCE_OBJECT (core/idl_args.h) performs §3.2.21 on the tramp, so it rests on the element it is on;
 * IDL_SEQUENCE_OBJECT_OR_DICT performs §3.6's split for the member that has two overloads to choose between.
 *
 * SO EVERY `transfer` PARAMETER BELOW IS AN ENGINE-BUILT ARRAY, and that is what the DCHECKs assert. It has to
 * be materialized once for a reason that is not tidiness: §9.4.4's post message steps read the list three
 * times (does it contain the source port, does it contain the target port) before §2.7.7's two loops read it
 * twice more, and against a page-supplied object each of those five reads is a trap that can answer
 * DIFFERENTLY — a list read per question is a list that can be five different lists. */

/* §2.7.7's StructuredSerializeWithTransfer and §2.7.8's StructuredDeserializeWithTransfer.
 *
 * `memory` IS SEEDED WITH THE TRANSFER LIST, which is what makes these two more than "serialize, then move the
 * named objects separately". A transferable REACHED FROM INSIDE the message body is neither cloned nor refused:
 * HTML §2.7.3's step 2 finds it in `memory` and writes its dataHolder, and HTML §2.7.6's step 2 finds that
 * holder in §2.7.8's `memory` and answers with the object the transfer-receiving steps built — THE SAME object as the
 * matching entry of [[TransferredValues]], never a copy. So `port.postMessage({p: other}, [other])` delivers a
 * message whose `p` is the moved port, and `event.ports[0] === event.data.p`.
 * The seam is a PAIR OF HOOKS passed as parameters of the one serialization — JSTransferWriteHook answering an
 * index into the transfer list on the way out, JSTransferReadHook resolving that index against
 * [[TransferredValues]] on the way in — carried by JS_WriteObject3/JS_ReadObject3. A page cannot forge one: the
 * reference tag is refused outright by a read that was given no hook.
 * A transferable NOT in the transfer list is still refused, and that is the same rule rather than an exception:
 * `memory` does not hold it, so it reaches the writer as what it is — a platform object, which §2.7 does not
 * serialize. */
typedef struct { StructuredData data; JSValue holders; } StructuredWithTransfer;
/* `transfer` is the MATERIALIZED list — IDL_SEQUENCE_OBJECT's Array. Returns 0, or -1 with a throw live. */
int  structured_serialize_transfer(JSContext *ctx, JSValueConst v, JSValueConst transfer,
                                   StructuredWithTransfer *out);
/* HOW LONG ONE OF THIS FILE'S OWN ARRAYS IS — a materialized transfer list, or a record's `holders`. Both are
   engine-built, so a malformed one crashes rather than reporting; no page input can reach it. JS_UNDEFINED is
   an empty one, which is what a record carrying no transfer holds. */
uint32_t structured_transfer_len(JSContext *ctx, JSValueConst arr);
/* Answers the deserialized message; `*pvalues` receives the [[TransferredValues]] as an Array (owned), which
   is what a MessageEvent's `ports` is built from. */
JSValue structured_deserialize_transfer(JSContext *ctx, const StructuredWithTransfer *in, JSValue *pvalues);
void structured_with_transfer_free(JSContext *ctx, StructuredWithTransfer *d);

#endif
