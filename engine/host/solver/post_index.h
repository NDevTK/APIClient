/* THE ROUTED-POST INDEX: one record per cross-instance `postMessage` whose message held unknowns.
 *
 * A routed message crosses as its examples (core/frame/window_message.c), and the receiver mints its whole
 * `data` as the per-delivery source `message.data:<id>`, so a receiver's finding names the delivery and a
 * member path. Which of the SENDER's own sources sits at that path is a fact only the sender holds. This
 * index is where the sender states it, keyed by the same record identity the receiver hashes
 * (window_message_record_id), so the trusted zone can join a receiver's parked finding to the producer that
 * would discharge it. It is an emission: written once per distinct record, never retracted, read by
 * result.c into the result document's `routedPosts`. */
#ifndef ENGINE_HOST_SOLVER_POST_INDEX_H
#define ENGINE_HOST_SOLVER_POST_INDEX_H
#include <stddef.h>

/* One routed post. Every field is borrowed for the call. `relation` is the target's relation to the
   sender's navigable: "parent", "child", "opener", "opened" or "other". */
typedef struct {
    const char *id;
    const char *target_doc;
    const char *target_origin;
    const char *relation;
} PostIndexHead;

/* One occurrence of one of the sender's unknowns in the message body: its source identity, its display
   shape, and the member path from `data` ("" for `data` itself, ".a.0" for `data.a[0]`). Borrowed. */
typedef struct {
    const char *src;
    const char *shape;
    const char *path;
} PostIndexSymbol;

/* Records the post. A second call for one `id` is the same record emitted again and adds nothing. */
void  post_index_record(const PostIndexHead *h, const PostIndexSymbol *syms, size_t n);
/* The records as a JSON array (caller frees). */
char *post_index_json(void);
void  post_index_free(void);

#endif
