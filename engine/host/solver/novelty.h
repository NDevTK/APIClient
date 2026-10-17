/* Novelty against the product: what an emission is worth to the WFQ ledger.
 *
 * Every credit site names what it emitted as an identity, one digest per emission kind, computed here and only
 * here. The trusted zone keeps the union of the identities every instance of a cluster has emitted, in every
 * session, and hands it back (novelty_learn); a site pays 1 only for an identity absent from both its own table
 * and that set. A known fact is still recorded and merged; it is paid 0. The zone never computes an identity: it
 * stores the strings this file emits, so the two sides cannot disagree about a spelling.
 *
 * The set is scheduler state, not page state: it is not COW-captured, and a resumed flow reads the current set.
 */
#ifndef ENGINE_HOST_SOLVER_NOVELTY_H
#define ENGINE_HOST_SOLVER_NOVELTY_H

#include <stdint.h>

/* The emission kinds, each the identity of what one credit site pays for. The token is the wire spelling of the
   kind on a set line and is part of the digested spelling. */
#define NOVELTY_KINDS(X)                                                                                         \
    X(NOVELTY_ENDPOINT, "endpoint")   /* solver/endpoint.c's record identity, scoped */                          \
    X(NOVELTY_HEADER,   "header")     /* a header name on one endpoint identity */                               \
    X(NOVELTY_SINK,     "sink")       /* solver/solve.c's search identity: sink class and injection source */    \
    X(NOVELTY_FINDING,  "finding")    /* a fire-verified PoC for that same (class, source) */
typedef enum {
#define NOVELTY_KIND_ENUM(id, token) id,
    NOVELTY_KINDS(NOVELTY_KIND_ENUM)
#undef NOVELTY_KIND_ENUM
    NOVELTY_KIND_N
} NoveltyKind;

/* The first 128 bits of SHA-256 over the canonical spelling. Wide enough that a collision is not a practical
   concern at any population this product can hold: a collision makes a novel emission read as known and costs
   it its point, and the birthday bound n^2 / 2^(w+1) at ten million identities is about 1.5e-25 for w = 128
   and 2.7e-6 for w = 64, a lost reward nothing could detect. */
typedef struct { uint64_t hi, lo; } NoveltyId;

/* The text a set line starts with, and the first field of every digested spelling: a change to any kind's
   fields is a new spelling, so a set persisted under an older one matches nothing rather than the wrong thing. */
#define NOVELTY_SPELLING "novelty/1"

/* The scope every identity of this instance is qualified by: the serialization of the root document address's
   origin (URL §4 "URL serializing" over its tuple origin), set once per agent before any credit site runs. An
   instance is one agent cluster, so a relative address spelled the same way in two of its documents names one
   resource only within the scope. Borrowed and copied. */
void novelty_set_scope(const char *scope);

/* One identity: `kind`, the scope, then `n` fields, each length-prefixed so that no byte a field holds can
   impersonate a separator. The fields are the kind's own table identity, in that table's order. */
NoveltyId novelty_id(NoveltyKind kind, const char *const *fields, int n);

/* Files `id` as one this instance has emitted and answers whether it is novel: absent from the known set. The
   caller's own table has just answered "new" for it, so an identity already filed here means that table's
   identity and this one disagree, which is asserted at `file`/`line`, the caller's. */
int  novelty_note_at(NoveltyKind kind, NoveltyId id, const char *file, int line);
#define novelty_note(kind, id) novelty_note_at((kind), (id), __FILE__, __LINE__)

/* The credit site's one ask: pays the running flow 1.0 through flow_credit_emit when `novel`, and counts the
   ask as paid or known-unpaid under `kind` either way. */
void novelty_credit(NoveltyKind kind, int novel);

/* The zone's known identities, as serialized typed text: the line NOVELTY_SPELLING, then one line per identity,
   `<kind token> <32 lowercase hex digits>`, newline-terminated. Additive; may be called any number of times,
   including before the agent's document exists. A text under another spelling is refused whole and counted. */
void novelty_learn(const char *text);

/* The `_novelty` member of the result document (owned): the spelling, the scope, the per-kind census of asks
   (paid + knownUnpaid) and the set sizes, and `identities`, every identity this instance filed, as set lines
   without their newline. */
char *novelty_json(void);

/* Releases both sets and the census; the agent's release column calls it beside endpoint_free. */
void novelty_free(void);

#endif
