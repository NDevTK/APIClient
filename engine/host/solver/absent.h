/* Absent state: which read of a name the page cannot resolve is unknown input, and which is a component this
 * engine owes. Two bases ask it: the global object, and a record the document published onto it. */
#ifndef ENGINE_HOST_SOLVER_ABSENT_H
#define ENGINE_HOST_SOLVER_ABSENT_H
#include "quickjs.h"

/* Install as JSConcolicHooks.absent. `obj` is the base the read missed on — the global object, or a published
   record (the engine has already established it is one of the two). `answer` is what the standard answers
   there, which the minted unknown records as this session's own reading. */
JSValue absent_read_hook(JSContext *ctx, JSValueConst obj, JSAtom name, JSConcolicAbsentAnswer answer);

/* Install as JSConcolicHooks.absent_unresolved: the same question reached by an operation that performs no
   [[Get]], of which `op` says which. `typeof X` on a name nothing binds is answered at the opcode by
   ECMAScript §13.5.3 The typeof Operator's §13.5.3.1 Runtime Semantics: Evaluation step 2.a; `"X" in window`
   by §13.10.1 Runtime Semantics: Evaluation's `RelationalExpression : RelationalExpression in
   ShiftExpression`, whose last step is "Return ? HasProperty(rightValue, ? ToPropertyKey(leftValue))"
   (§7.3.11 HasProperty ( obj, propertyKey )); and `Reflect.has(window, "X")` by §28.1.8 "Reflect.has (
   target, key )" step 3's "Return ? target.[[HasProperty]](propertyKey)". The read hook is asked by none of
   them, so this records the read into the same population and row and decides nothing; the return is void
   because no algorithm here asks a host for a value. `op` is stated by the caller, the party that performed
   the operation, and its population is `JSConcolicAbsentOp`. */
void absent_unresolved_note(JSContext *ctx, JSAtom name, JSConcolicAbsentOp op);

/* Install as JSConcolicHooks.present: the same question for a member the record holds. A published record's
   extent was chosen against this visitor's credentials, so `__FLAGS.admin === false` is a fact about this
   session and not about the program. This mints the derivation the read hook mints, with the held value as
   its example, so a loaded `features.admin:false` does not concretize the gate. `holder` is the record the
   slot was found on and `value` is borrowed. */
JSValue absent_present_hook(JSContext *ctx, JSValueConst holder, JSAtom name, JSValueConst value);

/* Install as JSConcolicHooks.publish. The engine has found `value` reachable from the global object through
   records the document's own inline scripts built, and states the parent it hangs off and the name it hangs
   under; this files the path the record's members are then read by. Parents arrive before their children, so
   a child whose parent is not filed is the engine and this file disagreeing about what is published, and it
   crashes. */
void absent_publish_hook(JSContext *ctx, JSValueConst parent, JSAtom name, JSValueConst value);

/* Give back the published-namespace registry. Called on the agent's release column, beside the source
   registry: a path names a record of a document that is gone. */
void absent_free(void);

/* Which standard owns a global name. `ABSENT_VOCAB_N` is the count and never a vocabulary; per-vocabulary
   arrays are sized by it, so a further standard adds one enum member and one row to each array. */
typedef enum { ABSENT_VOCAB_WEBIDL, ABSENT_VOCAB_ECMASCRIPT, ABSENT_VOCAB_ECMA402, ABSENT_VOCAB_N } AbsentVocab;

/* Which standard owns this global name, and the table's own spelling of it. Answered from the generated
   vocabularies: browser/platform_names.h, every name Web IDL exposes on Window (engine/idlgen.mjs);
   browser/language_names.h, ECMAScript §19 The Global Object's own subclauses (engine/esglobalgen.mjs); and
   browser/i18n_names.h, the one name ECMA-402 §8 The Intl Object puts there (engine/i18nglobalgen.mjs). The
   tables are searched separately and the answer states which held the name, since a `@WHY` has to say which
   standard owes it. The entry is the table's static string, so a caller may key on the pointer and no page
   byte enters anything keyed that way; absent_json writes its row names unescaped on that ground. Returns
   NULL when no standard owns the name, and `*vocab` is then untouched. */
const char *absent_standard_name(const char *name, AbsentVocab *vocab);

/* The names this document asked for and this engine did not answer: solver/result.c's `_absent`, as one JSON
   object on the heap. The caller frees it; NULL only on allocation failure, which every composer on that seam
   treats as the census being absent. absent.c states the population, the denominator, the kind of every row,
   the identities, and, as named residuals, the feature-detect spellings the census does not see. */
char *absent_json(void);

#endif
