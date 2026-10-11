/* solver/witness_verify.h — verifying an endpoint witness by firing it.
 *
 * An @H sighting's witness (solver/endpoint.h's `witness`) says what its flow assumed. A witness whose every
 * assumption is a pin on one attacker-deliverable source is a value an attacker can hand the page, so it is
 * checked the way @S checks a PoC: an ordinary candidate flow on the one frontier re-runs the document from the
 * baseline with that value delivered through the source's real transform (concolic_deliver). If such a run
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

/* Queues one verification for this session: substitute `payload` for the source `src` and watch for `key`'s
   request. One per key per session: a key already queued, seeded or rebuilt from the cold tier is not queued
   again. All three strings are copied. */
void  witness_verify_queue(const char *key, const char *src, const char *payload);
/* Mints a flow for every queued verification not yet seeded. Called at the pick beside solve_seed_candidates,
   so a verification is a member of the one frontier. Returns how many flows it added. */
int   witness_verify_seed(JSContext *ctx);
/* A verification flow rebuilt from the cold tier: records its key as seeded so this session does not mint a
   second run for the same witness. */
void  witness_verify_resumed(const char *key);
/* Per-session teardown of the queue. */
void  witness_verify_free(void);

#endif
