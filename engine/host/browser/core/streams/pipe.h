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

#endif
