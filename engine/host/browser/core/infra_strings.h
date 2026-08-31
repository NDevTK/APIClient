/* INFRA STANDARD §4.7 Strings — the string algorithms other specifications INVOKE BY NAME. See infra_strings.c.
 *
 * WHY IT IS ONE FILE. `normalize newlines` had two private copies before this one existed and a third was about
 * to be written: core/html/simple_dialogs.c ran it for HTML §8.9.1's message, and core/html/html_form.c needs
 * exactly the same algorithm for §4.10.11's API VALUE. The two copies had not drifted yet, and the reason they
 * had not is that one of them carried a comment warning the next reader that core/html/form_data.c's
 * `fd_normalize_newlines` is a DIFFERENT algorithm pointing the other way (§4.10.22.8 normalizes TOWARD CRLF) —
 * a hazard somebody had already seen and answered with prose. Prose is what a reader has to find; one
 * implementation with the spec's own name on it is what a reader cannot miss. form_data.c's copy STAYS where it
 * is and is not this: it is a step of the multipart serializer and not Infra's algorithm, which is why one
 * shared helper for the two of them would be two algorithms wearing one name.
 *
 * WHY IT LIVES HERE. `engine/build.mjs` compiles what it WALKS — `host/solver` and `host/browser` — so a shared
 * file at `host/` itself would be a translation unit no gate compiles, which is unverified rather than unused.
 * `core/` is where the engine's other cross-component helpers already sit (core/json_buf.h says the same thing
 * about the same directory for the same reason). */
#ifndef ENGINE_HOST_BROWSER_CORE_INFRA_STRINGS_H
#define ENGINE_HOST_BROWSER_CORE_INFRA_STRINGS_H
#include <stddef.h>

/* INFRA §4.7 Strings' NORMALIZE NEWLINES: "To normalize newlines in a string, replace every U+000D CR U+000A LF
   code point pair with a single U+000A LF code point, and then replace every remaining U+000D CR code point
   with a U+000A LF code point."
   THE TWO REPLACEMENTS ARE ONE PASS AND NOT TWO, which is not an optimisation — a pass that ran them in the
   order the sentence lists them would turn the LF that the first rule just produced into a second newline on
   the second sweep, so `"\r\n"` would normalize to `"\n\n"`. The single pass consumes the LF of a CRLF pair
   with the CR that introduced it, which is what "the pair" means.
   The result is at most as long as the input and is NUL-terminated. `*out_n` receives its length, because a
   string this engine normalizes may legitimately contain a NUL and strlen would then measure a prefix; pass
   NULL when the length is not wanted. Caller frees with free(). Never NULL — allocation failure is a CHECK. */
char *infra_normalize_newlines(const char *s, size_t n, size_t *out_n);

#endif
