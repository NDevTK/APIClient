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

/* A FIELD NAME, written as `"name":`.
 *
 * THE ARGUMENT IS A STRING LITERAL AND THE COMPILER IS WHAT SAYS SO. The macro concatenates it with the quotes
 * and the colon, and adjacent string-literal concatenation (C17 §6.4.5p5) is the only construct that
 * participates: a `const char *` in that position is a SYNTAX ERROR, not a runtime complaint. So a computed
 * field name is not something this seam can express, and every field name the engine emits is a fact readable
 * off the source — which is what makes the producer's half of the contract auditable at all.
 *
 * WHY THAT IS A SPLIT AND NOT A CONVENIENCE, and the defect shape worth keeping. ONE entry used to write both
 * halves of a JSON object — the NAME and the raw bytes around it — so at a call site the two were the same
 * construct with the same spelling, and which one a call was could be decided only by reading what its argument
 * happened to hold. That is undecidable the moment the argument is not a literal, and a reader that cannot
 * decide it must refuse EVERY non-literal: one shared entry turned every ordinary computed VALUE into an
 * unreadable construct, and the producer's field names became something an auditor had to rule out rather than
 * something it could enumerate. Splitting moves the question from "what is in this string" to "which function
 * was called", which the compiler answers and a scan can read. */
#define json_buf_key(b, name) json_buf_key_((b), "\"" name "\":")
void  json_buf_key_(JsonBuf *b, const char *quoted_name_colon);   /* json_buf_key's implementation */

/* RAW BYTES, appended verbatim. Three things arrive here and none of them is a field name: JSON structure
   (`{`, `,`, `[`), a JSON value a caller already formatted (a number out of snprintf, `null`), and — where the
   buffer is doing the growing-byte-buffer job it is, with no JSON anywhere near it — ordinary text. A field
   name reaches the buffer through json_buf_key alone, and engine/fieldgate.mjs reports a `"name":` inside a
   json_buf_raw LITERAL as a defect: the compiler forbids the computed key, the gate forbids the literal one. */
void  json_buf_raw(JsonBuf *b, const char *s);
/* One JSON STRING VALUE, quotes included: `"` and `\` escaped, the three named control characters written as
   their escapes and every other C0 byte as `\u00xx`. Bytes above 0x7F are passed through, which is correct
   because everything written here is already UTF-8 and JSON is a UTF-8 format.
   IT IS ALSO THE ONE ENTRY THAT MAY WRITE A COMPUTED KEY, and exactly once: a record whose keys are DATA — the
   endpoint surface's headers, keyed by header name — is not a contract a reader could audit and never was.
   Writing it through the value entry is what keeps it visibly a different construct from a field name. */
void  json_buf_str(JsonBuf *b, const char *s);
/* The bytes, NUL-terminated, transferred to the caller (who frees them). The buffer is empty afterwards. */
char *json_buf_take(JsonBuf *b);
void  json_buf_free(JsonBuf *b);

#endif
