/* STRUCTURED SERIALIZATION — HTML §2.7. See structured_clone.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_STRUCTURED_CLONE_H
#define ENGINE_HOST_BROWSER_CORE_STRUCTURED_CLONE_H
#include "quickjs.h"

/* §2.7.3's `structuredClone` global. There is no init: the operation holds no state of its own. */
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

/* ---- §2.7's TRANSFERABLE OBJECTS -----------------------------------------------------------------------------
 *
 * A transferable is not cloned, it is MOVED: Web IDL gives its interface two algorithms — TRANSFER STEPS that
 * empty the object into a data holder and detach the original, and TRANSFER-RECEIVING STEPS that build a new
 * object in the target realm out of that holder. The interface that owns them registers them here, because the
 * serializer must not know what a MessagePort is any more than it knows what a Response is.
 *
 * The HOLDER is a JS value of the registering component's own choosing — it is never seen by a page, and making
 * it a JS value rather than a C record is what lets it ride the frontier and be parked like everything else. */
typedef struct {
    bool    (*is)(JSValueConst v);
    JSValue (*out)(JSContext *ctx, JSValueConst v);        /* the holder, or JS_EXCEPTION with a throw live */
    JSValue (*in)(JSContext *ctx, JSValueConst holder);    /* the new object in this realm */
} StructuredTransferable;
void structured_register_transferable(const StructuredTransferable *t);

/* §2.7.1's StructuredSerializeWithTransfer and §2.7.2's StructuredDeserializeWithTransfer.
 *
 * ONE THING IS NOT BUILT AND IS NAMED RATHER THAN APPROXIMATED: the standard's `memory` map is pre-seeded with
 * the transfer list, so a transferred object REFERENCED FROM INSIDE the message body deserializes to the new
 * object. This engine's serializer has no seam to seed, so a port inside the body is refused — which is the
 * right answer for a port that is not in the transfer list (a platform object is not serializable) and the
 * wrong one for a port that is. A page that does `port.postMessage({p: other}, [other])` gets a DataCloneError
 * instead of a message carrying the moved port. */
typedef struct { StructuredData data; JSValue holders; } StructuredWithTransfer;
/* `list` is the transfer list (a sequence) or JS_UNDEFINED. Returns 0, or -1 with a throw live. */
int  structured_serialize_transfer(JSContext *ctx, JSValueConst v, JSValueConst list,
                                   StructuredWithTransfer *out);
/* Answers the deserialized message; `*pvalues` receives the [[TransferredValues]] as an Array (owned), which
   is what a MessageEvent's `ports` is built from. */
JSValue structured_deserialize_transfer(JSContext *ctx, const StructuredWithTransfer *in, JSValue *pvalues);
void structured_with_transfer_free(JSContext *ctx, StructuredWithTransfer *d);

#endif
