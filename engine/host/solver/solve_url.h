/* The request class's breakout: an address whose scheme and host the attacker's bytes decide, derived from
 * the request a real run handed the engine, with a locator standing where those bytes landed.
 *
 * The context is a URL §4.4 "URL parsing" state: the state the basic URL parser is in when its pointer reaches
 * the locator, read with core/url's own parser (url_prefix_state) over the bytes before it, resolved against
 * the document's base. The breakout is the shortest input that state's transitions carry to a host the
 * attacker owns (concolic_attacker_site(), or a name under it). A state past the authority decides the host, so it
 * has no breakout, and the search parks.
 *
 * The witness carries the base beside the request's input, because a relative input's parse depends on it and
 * the derivation runs where no realm is at hand. */
#ifndef ENGINE_HOST_SOLVER_SOLVE_URL_H
#define ENGINE_HOST_SOLVER_SOLVE_URL_H

#include "solver/solve_filter.h"

/* The context probe injected at the source. ASCII alphanumeric, so it moves no §4.4 state and survives every
   percent-encode set unchanged. */
#define SOLVE_URL_LOCATOR "apiclientaddress"

/* The witness for one observed request: `base` (the document base URL the input resolves against) and
   `input` (the address bytes the page handed the request, before parsing), composed into one owned string. */
char *solve_url_witness(const char *base, const char *input);

typedef void (*SolveUrlEmit)(void *user, const char *breakout);
/* Derive the breakouts for one witness and hand each one `d` delivers to `emit`. Returns how many were
   emitted. A witness without the locator emits nothing (the probe did not reach this request). */
int solve_url_breakouts(const char *witness, const SolveDelivered *d, SolveUrlEmit emit, void *user);

#endif
