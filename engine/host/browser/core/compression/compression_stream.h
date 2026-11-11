/* CompressionStream — the Compression Standard §4 "Interface CompressionStream". See compression_stream.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_COMPRESSION_COMPRESSION_STREAM_H
#define ENGINE_HOST_BROWSER_CORE_COMPRESSION_COMPRESSION_STREAM_H

#include "quickjs.h"

/* The agent's half: the class, the step machines and the realm intrinsic. */
void compression_stream_init(JSContext *ctx);
/* §4's interface prototype object and interface object, for one realm (`[Exposed=*]`). */
void compression_stream_install_realm(JSContext *ctx);
/* core/platform.h's release column: the class id and step ids go back; the prototypes are the realms'. */
void compression_stream_free(void);

#endif
