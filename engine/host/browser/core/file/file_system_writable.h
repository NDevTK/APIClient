/* FileSystemWritableFileStream — File System Standard §2.5. See file_system_writable.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_FILE_FILE_SYSTEM_WRITABLE_H
#define ENGINE_HOST_BROWSER_CORE_FILE_FILE_SYSTEM_WRITABLE_H
#include <stdbool.h>

#include "quickjs.h"
#include "quickjs-step.h"
#include "core/streams/stream_work.h"

void fs_writable_init(JSContext *ctx);
void fs_writable_free(JSRuntime *rt);

/* §2.5's CREATE A NEW FileSystemWritableFileStream given a file entry, as a SUB-SEQUENCE.
 *
 * It is a sub-sequence and not a call because §2.5's step 8, "Set up stream", reaches Streams §5.5.4 Default
 * controllers' SetUpWritableStreamDefaultController, which builds the controller a START PROMISE — and building
 * one means RESOLVING a capability, where ECMAScript §27.5.1.3 CreateResolvingFunctions step 2.f reads `then`
 * off whatever it is resolved with, which is the page's code and therefore a request. Streams §6 Transform
 * streams' TransformStream starts its two halves through exactly this seam (core/streams/transform_stream.c),
 * so this is the same operation and not a second one.
 *
 * `w` is the CALLER's StreamWork — the caller holds it across the suspension and its `visit` carries it.
 * `keep_existing_data` is File System §2.3.2 The createWritable() method's `options["keepExistingData"]`: the
 * stream's [[buffer]] starts as a copy of the entry's binary data rather than empty — its step 5.7.3.1, "Set
 * stream's [[buffer]] to a copy of entry's binary data".
 * `entry` is BORROWED. Returns >0 (the caller returns it), 0 once
 * `*pstream` is the stream (OWNED), or -1 with a throw live. */
int fs_writable_new_run(JSContext *ctx, StreamWork *w, JSValueConst entry, bool keep_existing_data,
                        JSValue in, JSValue *pstream, JSValue **out_cb, int *out_argc);

/* §2.5's THREE CONVENIENCE METHODS on this realm's FileSystemWritableFileStream.prototype, chained to
   WritableStream.prototype — installed by the per-realm intrinsic this component declares, so there is nothing
   for a host to call. This is the prototype itself, for the mint above. OWNED. */
JSValue fs_writable_proto(JSContext *ctx);

#endif
