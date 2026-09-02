/* §4.9.1's ReadableStreamPipeTo, and the two ReadableStream members over it. See pipe.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_STREAMS_PIPE_H
#define ENGINE_HOST_BROWSER_CORE_STREAMS_PIPE_H

#include "quickjs.h"

void pipe_init(JSContext *ctx);
void pipe_free(JSContext *ctx);

/* Install `pipeTo` and `pipeThrough` on ReadableStream.prototype. Called by readable_stream_install with the
   prototype it has just built: the two members belong to §4.2's interface, and the ALGORITHM behind them is
   this file's, so the declaration is here and the placement is §4's. */
void pipe_install(JSContext *ctx, JSValueConst stream_proto);

/* STREAMS §9.5 Piping — "The result of a ReadableStream readable piped through a TransformStream transform" —
 * AS THIS REALM'S FUNCTION OBJECT. Called with `this` = the source ReadableStream and one argument, the
 * TransformStream; it answers that transform's readable half.
 *
 * IT IS NOT `pipeThrough`, AND THE DIFFERENCE IS NOT A SHORTCUT. §4.2.4's member exists to serve a PAGE: its
 * steps 1-4 run Web IDL §3.2.17 over whatever object was passed to build a ReadableWritablePair, then read a
 * StreamPipeOptions off a second one, and both of those are the page's own getters. §9.5 is what ANOTHER
 * STANDARD performs, and it reads `transform.[[writable]]` and `transform.[[readable]]` as internal slots —
 * so a host component that took the member instead would hand a page's patched accessors a say in where its
 * own bytes go. §9.5's steps 1-2 are ASSERTS for the same reason the member's are throws.
 *
 * BORROWED-STYLE OWNERSHIP MATCHES readable_stream_op's: the value is OWNED and the caller frees it. The
 * caller reaches it with step_call_run, which is what keeps the pipe suspendable at every point §4.9.1 rests
 * at — a host that called it from C would be the drive-to-completion the engine aborts on. */
JSValue pipe_through_op(JSContext *ctx);

#endif
