/* A JSON WRITER FOR STRINGS THE ENGINE ITSELF OWNS — see json_buf.c.
 *
 * WHY THIS IS NOT `JS_JSONStringify`. quickjs.h deletes that entry point on purpose: serializing a JS VALUE
 * runs the page's code (`toJSON`, the replacer, every member read, a Proxy's ownKeys trap), so it is a step
 * machine reached through the flow machinery and a C entry beside it would be a second implementation of the
 * same algorithm. This is the other thing: a writer over C strings the engine BUILT, where there is no value to
 * read and nothing of the page's to run. The @H endpoint surface and the @S sink report are both written with
 * it, and so is the request record XHR hands the trusted zone.
 *
 * WHY IT LIVES HERE. `engine/build.mjs` compiles what it WALKS — `host/solver` and `host/browser` — so a
 * shared file at `host/` itself would be a translation unit no gate compiles, which this file's own standard
 * calls unverified rather than unused. Both halves reach it: the include path carries `host/browser`, which is
 * how solver/engine.h already reaches core/fetch/fetch.h.
 *
 * WHY IT IS ONE FILE. It was written TWICE — `solver/endpoint.c` and `solver/solve.c` each carried a private
 * `Buf`/`buf_puts`/`buf_json_str` — and the two had already drifted in their OOM message while agreeing on
 * everything that matters. A third copy is what this deletes rather than adds. */
#ifndef ENGINE_HOST_BROWSER_CORE_JSON_BUF_H
#define ENGINE_HOST_BROWSER_CORE_JSON_BUF_H
#include <stddef.h>

/* A growing byte buffer. Zero-initialise one (`JsonBuf b = { 0 };`) and release it with json_buf_free, or take
   its bytes with json_buf_take. */
typedef struct { char *b; size_t n, cap; } JsonBuf;

void  json_buf_puts(JsonBuf *b, const char *s);
/* One JSON STRING literal, quotes included: `"` and `\` escaped, the three named control characters written as
   their escapes and every other C0 byte as `\u00xx`. Bytes above 0x7F are passed through, which is correct
   because everything written here is already UTF-8 and JSON is a UTF-8 format. */
void  json_buf_str(JsonBuf *b, const char *s);
/* The bytes, NUL-terminated, transferred to the caller (who frees them). The buffer is empty afterwards. */
char *json_buf_take(JsonBuf *b);
void  json_buf_free(JsonBuf *b);

#endif
