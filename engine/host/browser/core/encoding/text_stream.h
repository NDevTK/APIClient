/* TextDecoderStream AND TextEncoderStream — the Encoding Standard §7.5 and §7.6. See text_stream.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_ENCODING_TEXT_STREAM_H
#define ENGINE_HOST_BROWSER_CORE_ENCODING_TEXT_STREAM_H
#include "quickjs.h"

void text_stream_init(JSContext *ctx);
void text_stream_install_protos(JSContext *ctx);   /* §7.5's and §7.6's prototypes, for ONE realm */
void text_stream_install(JSContext *ctx, JSValueConst global);
void text_stream_free(JSContext *ctx);

/* THE UTF-8 TEXT DECODE OF A ReadableStream, AS THIS REALM'S FUNCTION OBJECT — Fetch §5.3 "Body mixin"'s
 * `textStream()` steps 4-6 and File API §3.3.6 "The textStream() method"'s steps 2-4, which are the same three
 * steps and are therefore one operation rather than a copy in each caller. Called with `this` = the source
 * ReadableStream and no arguments; it answers the ReadableStream those steps return.
 *
 * WHAT IT PERFORMS, in the standards' own words: "Let decoder be a new TextDecoderStream object in this's
 * relevant realm", "Set up decoder with UTF-8", "Return the result of stream, piped through decoder". The
 * middle one is Encoding §7.5's own "set up a text decoder stream", which that standard exports for another
 * specification to perform; the last is Streams §9.5 Piping's "piped through", not §4.2.4's `pipeThrough`
 * member — see core/streams/pipe.h for why those two are not interchangeable.
 *
 * WHAT IT DOES NOT PERFORM is either caller's PROLOGUE, because the two callers differ there and only there:
 * Fetch's steps 1-2 are an unusable check and a null-body arm, File API's step 1 is "get stream on this". Each
 * member keeps its own, which is where the spec puts them.
 *
 * OWNED: the caller frees the function object. It is reached with step_call_run like any other call, which is
 * what keeps the decode and the pipe suspendable at every point §4.9.1 rests at. */
JSValue text_stream_decode_op(JSContext *ctx);

#endif
