/* DOM §4.3.3 "Interface MutationRecord" — the value object §4.3.2 enqueues. See mutation_record.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_DOM_MUTATION_RECORD_H
#define ENGINE_HOST_BROWSER_CORE_DOM_MUTATION_RECORD_H
#include <lexbor/dom/dom.h>
#include "quickjs.h"

/* §4.3.2's RECORD TYPE, as the three strings the IDL's `type` attribute answers with. An enum and not a string
   at the call sites because "queue a mutation record of type" is switched on in three places — the interested
   observer test, the old-value test and the record itself — and three spellings of "characterData" is one typo
   away from an observer that never fires. */
enum { MR_TYPE_ATTRIBUTES = 0, MR_TYPE_CHARACTER_DATA = 1, MR_TYPE_CHILD_LIST = 2 };

void mutation_record_init(JSContext *ctx);
void mutation_record_install_proto(JSContext *ctx);   /* §3.7: this realm's MutationRecord.prototype */
void mutation_record_install(JSContext *ctx, JSValueConst global);
void mutation_record_free(JSRuntime *rt);

/* §4.3.2 step 3.1's "a new MutationRecord object with its type set to …". Every field is passed because the
   record is IMMUTABLE once made — the spec constructs it in one step and nothing writes to it afterwards, so
   there is no setter to get wrong and no state for a flow to observe another flow through.
 *
 * `added`/`removed` are ARRAYS OF NODE WRAPPERS, CONSUMED, which this turns into the two [SameObject] static
 * NodeLists the IDL declares — built HERE and not by the caller, because [SameObject] means the object handed
 * to the page must be the SAME one on every read, and the only way to guarantee that is for the record to hold
 * one it made. `prev`/`next` may be NULL (the IDL's `Node?`). `name`/`ns`/`old` may be NULL (`DOMString?`).
 * OWNED: the caller frees. */
JSValue mutation_record_new(JSContext *ctx, int type, lxb_dom_node_t *target,
                            JSValue added, JSValue removed,
                            lxb_dom_node_t *prev, lxb_dom_node_t *next,
                            const char *name, const char *ns, const char *old, size_t old_len);

#endif
