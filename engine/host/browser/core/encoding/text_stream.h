/* TextDecoderStream AND TextEncoderStream — the Encoding Standard §7.5 and §7.6. See text_stream.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_ENCODING_TEXT_STREAM_H
#define ENGINE_HOST_BROWSER_CORE_ENCODING_TEXT_STREAM_H
#include "quickjs.h"

void text_stream_init(JSContext *ctx);
/* Encoding §7.5 Interface TextDecoderStream's and §7.6 Interface TextEncoderStream's prototypes, their Web
   IDL §3.7.1 Interface object's interface objects, and the Web IDL §3.8 property references for them — for
   ONE realm, declared into core/realm.h's list. ONE entry because Web IDL §3.8 Platform objects implementing
   interfaces' `define the global property references` is given "target" and "a realm realm" and its step 1
   population is "every interface that is exposed in realm": no Document appears in it. Both interfaces
   declare `[Exposed=*]`, so EVERY realm owes both names — and while the interface objects were installed from
   core/platform.c's per-document column, a worker realm, which reaches no platform_document_install, received
   neither. */
void text_stream_install_realm(JSContext *ctx);
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
