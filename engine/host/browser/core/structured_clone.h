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

#endif
