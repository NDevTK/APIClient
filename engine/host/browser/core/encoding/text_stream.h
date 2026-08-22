/* TextDecoderStream AND TextEncoderStream — the Encoding Standard §7.5 and §7.6. See text_stream.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_ENCODING_TEXT_STREAM_H
#define ENGINE_HOST_BROWSER_CORE_ENCODING_TEXT_STREAM_H
#include "quickjs.h"

void text_stream_init(JSContext *ctx);
void text_stream_install_protos(JSContext *ctx);   /* §7.5's and §7.6's prototypes, for ONE realm */
void text_stream_install(JSContext *ctx, JSValueConst global);
void text_stream_free(JSContext *ctx);

#endif
