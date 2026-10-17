/* Which bytes a program name was compiled from, by content digest.
 *
 * A function's cross-session name (quickjs's JS_OrphanHash) folds the name of the script its body was compiled
 * from; this answers which bytes that name stood for in this instance, so a drive of uncalled code is keyed by
 * the bundle's content and the function's name together (solver/novelty.h's NOVELTY_DRIVE). Fed at the one row
 * where a document's program is compiled. A name compiled from two different byte sequences (every inline
 * script of a document shares its base URL) names no single content, and says so.
 */
#ifndef ENGINE_HOST_SOLVER_SCRIPT_DIGEST_H
#define ENGINE_HOST_SOLVER_SCRIPT_DIGEST_H

#include "solver/dyn_body.h"

/* `name` (borrowed, copied) was compiled from `body` (borrowed). */
void script_digest_note(const char *name, const DynBody *body);

/* What `name`'s bytes are called, owned: `sha256 <64 hex>` when exactly one byte sequence was compiled under
   it, `address <name>` when none was or more than one was. */
char *script_digest_identity(const char *name);

/* Every name forgotten; the agent's release column calls it beside novelty_free. */
void script_digest_free(void);

#endif
