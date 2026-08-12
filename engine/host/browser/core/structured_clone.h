/* STRUCTURED SERIALIZATION — HTML §2.7. See structured_clone.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_STRUCTURED_CLONE_H
#define ENGINE_HOST_BROWSER_CORE_STRUCTURED_CLONE_H
#include <stdint.h>

#include "quickjs.h"

/* §2.7.10's `structuredClone` global. There is no init: the operation holds no state of its own. */
void structured_clone_install(JSContext *ctx, JSValueConst global);

/* StructuredDeserialize(StructuredSerialize(v)) — a deep, cycle-preserving copy in this realm. Every caller in
   the platform that says "a serialized copy" performs this: a MessagePort delivering a message, a window post,
   §4.9.7's tee with cloneForBranch2. Answers a new owned value, or JS_EXCEPTION with the "DataCloneError"
   DOMException live for a value §2.7 refuses (a function, a Proxy, a Promise, a platform object). */
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

/* §3.2.21's `sequence<object>` — the transfer list, MATERIALIZED ONCE into an engine-built Array.
 *
 * IT IS ONE CALL BECAUSE IT IS THE ONE PLACE THE PAGE'S CODE RUNS. §9.4.2's post message steps read the list
 * three times (does it contain the source port, does it contain the target port, and then §2.7.7's two loops),
 * and against a page-supplied object each of those reads is an accessor or a Proxy trap that can answer
 * DIFFERENTLY every time — so a list read per question is a list that can be four different lists. Everything
 * downstream of this call walks an Array the engine built, and reads nothing of the page's at all.
 *
 * WHAT IS STILL MISSING AND IS NAMED RATHER THAN APPROXIMATED: §3.2.21 converts a sequence through the ITERATOR
 * protocol (GetMethod(@@iterator), its call, one `next()` per element, a `done` and a `value` read), and every
 * one of those is a rest point a declared member parks on — which is why IDL_SEQUENCE_DOMSTRING and
 * IDL_SEQUENCE_INTERFACE are DECLARED TYPES in idl_args.h rather than walks a body performs. `sequence<object>`
 * has no such type yet, so this reads `length` and indices instead: right for the Array every real page passes,
 * and wrong for an iterable that is not one. The fix is an IDL_SEQUENCE_OBJECT beside those two, declared on
 * `postMessage`'s options member, after which this function's callers receive the converted list and this
 * function goes away.
 *
 * Returns 0 with `*out` an owned Array (empty for an absent list), or -1 with a throw live. */
int structured_transfer_list(JSContext *ctx, JSValueConst list, JSValue *out);

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
/* `transfer` is the MATERIALIZED list from structured_transfer_list. Returns 0, or -1 with a throw live. */
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
