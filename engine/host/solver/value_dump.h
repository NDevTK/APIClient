/* A JSValue DUMPED AS JSON, WITH EVERY CONCOLIC STANDING ON ITS OWN EXAMPLE — see value_dump.c.
 *
 * WHAT THIS IS NOT, AND THE DELETION IT MUST NOT UNDO. It is NOT `JSON.stringify`. quickjs.h deletes the C
 * entry point for that algorithm by name — "serialization runs the page's code — toJSON, the replacer, every
 * element and member read, and a Proxy's ownKeys/getOwnPropertyDescriptor traps — so it is a step machine
 * reached through the flow machinery, and a C entry beside it would be a second implementation of the same
 * algorithm" — and quickjs.c's own deletion note names `JS_JSONStringify`, `js_json_check`, `js_json_to_str`
 * and `JSONStringifyContext` as the dual-system seam that went with it. A C walker that ran any of that would
 * be that system re-added under a new name.
 *   SO THE DISTINCTION IS NOT THE OUTPUT FORMAT, IT IS THAT THIS RUNS NO PAGE CODE AT ALL. It reads SLOTS
 * through `JS_GetOwnSlot` — the fork's own no-user-code own-property read, which REFUSES an accessor rather
 * than calling it — it consults no `toJSON`, it takes no replacer, it performs no [[Get]] and no prototype
 * walk, and it ABORTS on a Proxy rather than reaching a trap. Everything it cannot express that way is a
 * `DFAIL` naming the kind, never a value it invents. That is the family §Architecture names: quickjs's own
 * `JS_DumpValue`/`JS_DumpGCObject` dump infra, which inspects a value without being observable to the page.
 *
 * WHY IT RESOLVES EXAMPLES. A value a page stored is concolic wherever its used value depends on something
 * this engine models as unknown (solver/absent.h), and the whole of §Solver-half's claim is that the CONCRETE
 * EXAMPLE exists beside the unknown. `concolic_is(v) ? concolic_example(ctx, v) : JS_DupValue(ctx, v)` is this
 * engine's routine spelling of taking it, and this is that spelling applied at every leaf of a walk. How
 * widespread it is is a fact about the tree and therefore a DERIVATION rather than a digit:
 * `git grep -aoE "concolic_is\([^)]*\) \? concolic_example" -- '*.c' '*.h' | wc -l` counts the occurrences
 * and `git grep -lE` the same pattern counts the files. (`grep -c` would count LINES, which is the same number
 * only while no line holds two.)
 *
 * AND AN EXAMPLELESS CONCOLIC IS A STATED ABSENCE AND NEVER A ZERO. Where the unknown carries no example the
 * dump writes its DISPLAY SHAPE as a JSON string (`concolic_shape_c`), which is a POSITIVE statement that the
 * producer had no value and says which source it was waiting on. A consumer that tests for a Number therefore
 * sorts it into its own column with a reason, which is the property testing/render_diff.js's comparator is
 * built on; writing `0`, or `null`, or omitting the member, are the three spellings that would make a value
 * nobody produced indistinguishable from one that was measured.
 *
 * WHY IT IS IN `solver/`. What it knows that a JSON writer does not is the concolic triple, which is this
 * half of the project's own primitive — and `engine/build.mjs` walks `host/solver` and `host/browser`, so a
 * file here is compiled by both programs with nothing to add to any list. */
#ifndef ENGINE_HOST_SOLVER_VALUE_DUMP_H
#define ENGINE_HOST_SOLVER_VALUE_DUMP_H
#include "quickjs.h"

/* `v` DUMPED AS ONE JSON TEXT, NUL-terminated, malloc'd, TRANSFERRED to the caller.
 *
 * NEVER NULL: an allocation this cannot make is CLAUDE.md's always-fatal case (the artifact the caller is
 * composing is lost at the one line that would report it), so it aborts rather than answering a status a
 * caller could default past.
 *
 * NEVER CONTAINS A TAB OR A NEWLINE. Every string value goes through `json_buf_str`, which writes the three
 * named control characters as their escapes and every other C0 byte as `\u00xx`, so the result is one LINE and
 * may be carried as a field of a tab-delimited, newline-separated record without an encoding step. A caller
 * relying on that is relying on json_buf.c's own loop and not on this sentence. */
char *value_dump_json(JSContext *ctx, JSValueConst v);

#endif
