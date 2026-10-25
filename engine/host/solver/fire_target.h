/* What a candidate run's fire proves: the observation that settles it, one per candidate flow. See
   fire_target.c. */
#ifndef ENGINE_HOST_SOLVER_FIRE_TARGET_H
#define ENGINE_HOST_SOLVER_FIRE_TARGET_H

/* FIRE_MARKER: an @S search's sink fired the breakout's marker (solve.c's js_x9). FIRE_REQUEST: the request
   an endpoint record witnesses left the engine (endpoint.c's verification observation). FIRE_NONE is an
   exploring flow, which carries no candidate. */
typedef enum { FIRE_NONE = 0, FIRE_MARKER, FIRE_REQUEST } FireKind;

/* `sink` is solve.c's static sink-class name for FIRE_MARKER and NULL otherwise; `key` is the witness
   verification key (solver/witness_verify.h) for FIRE_REQUEST, owned, and NULL otherwise. Held by value on a
   Flow, zero for FIRE_NONE. */
typedef struct {
    FireKind kind;
    const char *sink;
    char *key;
} FireTarget;

/* Make `t` (which is FIRE_NONE) a marker target for `sink`, or a request target for a copy of `key`. */
void fire_target_marker(FireTarget *t, const char *sink);
void fire_target_request(FireTarget *t, const char *key);
/* `dst` (FIRE_NONE) takes a copy of `src`, as a fork's arm continues its parent's candidate. */
void fire_target_copy(FireTarget *dst, const FireTarget *src);
/* Back to FIRE_NONE, releasing the key. */
void fire_target_clear(FireTarget *t);
/* The marker target's sink and the request target's key: each asserts the kind it is asked of. */
const char *fire_target_sink(const FireTarget *t);
const char *fire_target_key(const FireTarget *t);

#endif
