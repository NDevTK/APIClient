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
   THE LAST ENTRY IS THIS ENGINE'S ANSWER AND NOT THE STANDARD'S, which is why it is spelled out below rather
   than left reading as a fact about §2.7: HTML §2.7.1 "Serializable objects" says a platform object IS
   serializable "if their primary interface is decorated with the [Serializable] IDL extended attribute", and
   §2.7.3 step 20 refuses only the ones that are not. Every platform object is refused HERE because §2.7.1 has
   no arm in this engine at all — see the §2.7.1 block below. */
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

/* ---- §2.7.1's SERIALIZABLE OBJECTS — THE MISSING HALF, AND WHAT ITS SEAM HAS TO BE ------------------------
 *
 * THIS REGISTRY'S SIBLING DOES NOT EXIST. §2.7.2's transferables MOVE and detach and register above; §2.7.1's
 * serializables are COPIED, and nothing registers one — so every [Serializable] interface in this tree
 * (CryptoKey, Blob, FileList, the File System Access handles) is refused by engine/qjs/quickjs.c's per-class
 * dispatch falling through to its default arm, which this file re-reports as the "DataCloneError" above.
 *
 * AND THE ABSENCE IS ALREADY COSTING SOMETHING THAT IS NOT A PLATFORM OBJECT, WHICH IS THE PART WORTH KNOWING
 * BEFORE ANYBODY PRICES IT. §2.7.3's per-value steps are steps of a RECURSIVE algorithm and this file has no
 * per-value arm to ask them in — its ONE per-value decision is the transfer map's index_of — so a step that is
 * not about the whole value has to be asked in the writer or asked once. Step 5's Symbol refusal was asked
 * once, at the top level, and `structuredClone({s: Symbol()})` therefore came back with an `s`. That is now
 * fixed in the writer; it is recorded here because it is the same gap wearing a different §2.7.3 step, and
 * because it is the evidence that the seam below is owed today rather than when the first [Serializable]
 * interface wants it.
 *
 * THE SEAM IS NOT THE ONE §2.7.2's IS, AND IT IS NOT BCR_TA's EITHER. Three facts decide its shape and each is
 * read off the standard rather than off the nearest thing in this engine that looks like it:
 *
 *   (1) THE WIRE CARRIES AN IDENTIFIER AND THEN A SUB-SERIALIZATION. §2.7.3 step 19 is "Otherwise, if value is
 *       a platform object that is a serializable object:", whose sub-steps are "Let typeString be the
 *       identifier of the primary interface of value", "Set serialized to { [[Type]]: typeString }" and "Set
 *       deep to true"; step 26's third arm then runs the interface's serialization steps, which "may need to
 *       perform a sub-serialization". A sub-serialization is the SAME walk under the SAME `memory` — so it is
 *       one more value pushed on the writer's own work stack, never a nested JS_WriteObject, which would open
 *       a second object_list and neither terminate a cycle nor preserve `===`.
 *
 *   (2) THE READER CREATES BEFORE IT FILLS, AND THAT IS §2.7.6's OWN ORDER RATHER THAN A PREFERENCE. Step 22
 *       is "Otherwise:" — "Let interfaceName be serialized.[[Type]]", "If the interface identified by
 *       interfaceName is not exposed in targetRealm, then throw a \"DataCloneError\" DOMException", "Set value
 *       to a new instance of the interface identified by interfaceName, created in targetRealm", "Set deep to
 *       true". Step 23 is "Set memory[serialized] to value". Step 24 is "If deep is true:", whose last arm
 *       performs the deserialization steps. So the instance is IN `memory` before any sub-value is read, and
 *       §2.7.1 says in as many words that it arrives "with none of its internal data set up; setting that up
 *       is the job of these steps". A ROW IS THEREFORE TWO OPERATIONS, create and fill, and not one.
 *       THE RESERVE-AND-PATCH IDIOM IS THE WRONG ONE TO COPY, AND IT IS THE OBVIOUS ONE. quickjs's reader has
 *       exactly this shape already, at BC_TAG_TYPED_ARRAY: `idx = s->objects_count; BC_add_object_ref1(s,
 *       NULL);`, a frame, one sub-value, then `s->objects[f->i] = ...`. It reserves because a typed array
 *       CANNOT exist before its buffer — the constructor needs it. A serializable can, and §2.7.6 step 23
 *       requires it, so copying that idiom imports a narrowing the standard does not have: the slot is NULL
 *       while the sub-value is read, so a serializable whose own data reaches back to it resolves against
 *       `!s->objects[val]` and is refused as a corrupt stream. The line number is right and the ROLE is not.
 *
 *   (3) THE KEY IS THE IDENTIFIER AND NEVER A ROW INDEX. §2.7.6 step 22 chooses BY interfaceName, exactly as
 *       §2.7.8 chooses the receiving steps by a holder's [[Type]] above — and these bytes outlive the turn:
 *       a history entry, a broadcast, a routed message, the IDB cold tier. A row ordinal names a different
 *       interface the moment a registrant is added, which is one park away rather than one release away.
 *
 * WHAT IT IS NOT YET IS SCOREABLE, AND THAT IS WHY IT IS RECORDED AND NOT BUILT. The corpus's own oracle for
 * the first consumer is WebCryptoAPI/serialization/, nineteen collected documents that share one META script;
 * every one of them calls crypto.subtle.generateKey and crypto.subtle.exportKey, and neither is installed —
 * `git grep -c '"generateKey"' -- '*.c' '*.h'` answers nothing at all, against two for "importKey". So those
 * nineteen fail before they reach structuredClone, and a seam landed today would move their verdict by zero:
 * an absent crash is not a correct value. core/crypto/crypto_key.h states the resulting landing order. */

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
 * §2.7.1's first step finds it in `memory` and writes its dataHolder, and §2.7.2's first step finds that holder
 * in §2.7.8's `memory` and answers with the object the transfer-receiving steps built — THE SAME object as the
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
