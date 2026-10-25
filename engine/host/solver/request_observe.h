/* solver/request_observe.h — a request observed leaving the engine, the proof a request fire reads.
 *
 * The observation point is engine.c's pending_park_request, at the arm whose request goes to the trusted zone
 * over the network: after Fetch §4.1's upgrade and blocking steps, so a request a policy blocks or a local
 * scheme answers never counts as having left. What is read is the request exactly as it leaves (method, URL
 * bytes, headers, body), never the page's value it was built from. */
#ifndef ENGINE_HOST_SOLVER_REQUEST_OBSERVE_H
#define ENGINE_HOST_SOLVER_REQUEST_OBSERVE_H

#include "core/fetch/fetch.h"

/* Which part of a request carries the bytes a fire looks for. HOST is the URL's host as §4.4 parses it, which
   the marker names or is a name under; ADDRESS is the URL's serialization; HEADER is one header's combined
   value; BODY is the body's bytes. */
typedef enum { REQUEST_PIECE_HOST, REQUEST_PIECE_ADDRESS, REQUEST_PIECE_HEADER, REQUEST_PIECE_BODY } RequestPiece;

/* Does `req` carry `marker` in `piece`? `method`, when not NULL, must equal the request's; `header` names the
   header for REQUEST_PIECE_HEADER and is NULL otherwise. Pure: reads only `req`. */
int request_observed(const FetchRequest *req, const char *method, RequestPiece piece, const char *header,
                     const char *marker);

/* Hands the request leaving the engine to the running flow's fire, when that flow is a candidate. Called once per
   such request by pending_park_request. */
void request_observe_park(const FetchRequest *req);

#endif
