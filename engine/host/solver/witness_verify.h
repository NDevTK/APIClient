/* solver/witness_verify.h — verifying an endpoint witness by firing it.
 *
 * An @H sighting's witness (solver/endpoint.h's `witness`) says what its flow assumed. A witness whose every
 * assumption is a pin on one attacker-deliverable source is a value an attacker can hand the page, as is a marked
 * sighting's delivery, so it is checked the way @S checks a PoC: an ordinary candidate flow on the one frontier
 * re-runs the document from the baseline with every value the delivery sets delivered through its source's real
 * transform (concolic_deliver for the first, concolic_set_gates for the others). If such a run
 * reaches the witnessed request assuming nothing of its own, the witness is verified. A run that never reaches
 * it proves nothing: the witness stays unverified as a parked search, never "infeasible".
 *
 * This file owns the session's verification queue and the minting of those flows. solver/endpoint.c decides
 * which witnesses are eligible, composes the key, and makes the observation. */
#ifndef ENGINE_HOST_SOLVER_WITNESS_VERIFY_H
#define ENGINE_HOST_SOLVER_WITNESS_VERIFY_H

#include "quickjs.h"

/* The verification key: the witnessed request's method and path and the witness's canonical identity, composed
   with concolic_ident_compose's length-prefixed encoding. Owned by the caller. */
char *witness_verify_key(const char *method, const char *path, const char *witness_canon);
/* …split back into its three fields, each owned by the caller. Asserts the key is one this file composed. */
void  witness_verify_key_split(const char *key, char **method, char **path, char **witness_canon);

/* One further source a verification sets beside its substitution: the declared source, the value its read
   answers (concolic_gate_beside) and its base's head (concolic_url_head, or NULL). */
typedef struct { const char *src; const char *val; const char *head; } WitnessVerifyGate;
/* Queues one verification for this session: substitute `payload` for the source `src`, behind `head` (the URL
   component's head concolic_url_head composed from the queuing flow's facets, or NULL), set the `ngate` further
   sources `gates`, and watch for `key`'s request. One per key per session: a key already queued, seeded or
   rebuilt from the cold tier is not queued again. Every string is copied. */
void  witness_verify_queue(const char *key, const char *src, const char *payload, const char *head,
                           const WitnessVerifyGate *gates, int ngate);
/* The head `key`'s verification is delivered behind, or NULL for none; asserts the key is queued. Borrowed. */
const char *witness_verify_head(const char *key);
/* `key`'s further sources as concolic_set_gates takes them (a head NULL for none); answers how many and asserts
   the key is queued. Borrowed. */
int   witness_verify_gates(const char *key, char *const **srcs, char *const **vals, char *const **heads);
/* `key`'s further sources as one text witness_verify_resumed reads back (the cold tier's field); owned. */
char *witness_verify_gates_text(const char *key);
/* Mints a flow for every queued verification not yet seeded. Called at the pick beside solve_seed_candidates,
   so a verification is a member of the one frontier. Returns how many flows it added. */
int   witness_verify_seed(JSContext *ctx);
/* Whether witness_verify_seed would mint a flow. */
int   witness_verify_seeds_owed(void);
/* A verification flow rebuilt from the cold tier, with the head (or NULL) and the further sources' text
   (witness_verify_gates_text) its record carried: records its key as seeded so this session does not mint a
   second run for the same witness. */
void  witness_verify_resumed(const char *key, const char *head, const char *gates_text);
/* Per-session teardown of the queue. */
void  witness_verify_free(void);

#endif
