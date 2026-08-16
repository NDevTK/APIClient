/* ACTIVE DISCOVERY — the engine fetches an API's own PUBLISHED description and reads the endpoints out of it.
 *
 * WHY IT IS THE ENGINE'S. CLAUDE.md §Attacker sources: "Active discovery is REQUIRED (lazy scripts + discovery
 * docs + Google API req2proto error-probes) — passive learning is too thin." It was `extension/lib/discovery.js`
 * (the candidate URL set) driven by `extension/lib/discovery-probe.js` (the fetch loop), and both were LOGIC in
 * the untrusted-adjacent host — `extension/jsaudit.mjs`'s row for them names this file as what they become.
 *
 * WHAT MADE THE MOVE POSSIBLE, AND WHY IT WAS BLOCKED UNTIL NOW. The engine's only fetch edge is the PER-FLOW
 * pending seam: a request is parked on the flow that made it, and `engine_provide` fills the register of the
 * flows that named the URL. A component with no flow has nowhere to park, which is why "port discovery.js" was
 * not the task — the task was a FLOW THE ENGINE SEEDS ITSELF. That is what this file is:
 *
 *   ONE CANDIDATE URL IS ONE FLOW. `discovery_seed_origin` mints a flow per candidate (solver/flow.h's
 *   `disc_url`), each an ordinary member of the ONE WFQ frontier — ranked, preempted, parked and resumed like
 *   the boot fork, a branch arm or an @S candidate session. Its ONE unit of work is: park on its URL
 *   (`engine_pending_discovery_url`), report FLOW_STEP_OWED so the scheduler runs its siblings, and — when the
 *   trusted zone has answered — read the document and register what it names. There is no loop, no cursor and
 *   no driver: the scheduler is the driver, which is the whole of §scheduler's "THERE IS NO GRIND".
 *
 *   SIBLINGS, NOT A SEQUENCE. The JS version walked its candidates in a `for` loop and returned at the first
 *   one that answered, which is a drive-to-completion holding N network round trips. Here the candidates are
 *   siblings on the frontier, so no candidate's answer gates another's — and there is no "found" or "not_found"
 *   latch to write down, which is the shape §NO BOUNDS bans by name (the JS had exactly that, plus a
 *   300-second cooldown, both since deleted from it).
 *
 *   IT PARKS TO THE COLD TIER AND RESUMES IN A LATER SESSION. A discovery flow's whole identity is the URL it
 *   probes, so its recipe is that URL (solver/cold.h's `d` record) and the resumed flow re-issues the GET and
 *   reads TODAY's document — §Time-travel-resume's "a resumed flow re-derives example VALUES from CURRENT
 *   sources" applied to the one flow for which the fetch IS the work.
 *
 * IT ISSUES NOTHING BUT A GET, STRUCTURALLY. There is no method parameter anywhere in this file and none on the
 * park it uses (`engine_pending_discovery_url` takes a URL and nothing else), so §Attacker sources' "a
 * state-mutating request is NEVER fired to learn" is a property of the shape rather than a check someone
 * remembers to write — the same thing `req2proto.c` achieved by having no issuing entry at all, and the same
 * defect `pageContextGet` closed on the JS side by removing the method parameter. A discovery document DESCRIBES
 * POST methods, and those are RECORDED as endpoints (recording is not firing); nothing here can call one.
 *
 * WHAT IS NOT HERE, BY NAME. The JS candidate set also carried API-KEY variants (`?key=…` and the
 * `X-Goog-Api-Key` header), because some documents only load for a caller that holds a key. The engine has no
 * key surface yet — keys are learned by `extension/lib/keys.js`, still queued in jsaudit for moat.c — so those
 * candidates are not built here rather than being built from a key this file would have to invent. When keys.js
 * moves, the key-bearing candidates are minted from the engine's own learned keys, as more siblings.
 *
 * THE SCHEMA HALF OF A DISCOVERY DOCUMENT IS STILL THE POPUP'S. `findDiscoveryMethod` / `resolveDiscoverySchema`
 * in `extension/lib/discovery.js` resolve a request-body schema for the Send panel out of a document the host
 * holds; this component reads the same documents for their ENDPOINTS and emits them through solver/endpoint.c.
 * The two halves are different consumers of one file, which is why the file did not move in one piece. */
#ifndef ENGINE_HOST_SOLVER_DISCOVERY_H
#define ENGINE_HOST_SOLVER_DISCOVERY_H

#include "quickjs.h"

/* SEED THE PROBE FLOWS FOR ONE ORIGIN — `origin` is scheme + host + optional port and NOTHING else
 * (`https://people-pa.googleapis.com`), because a published document's address is built from the origin and a
 * well-known path. The SCHEME is carried rather than assumed: an `http://localhost:8080` API's document is at
 * http, and inventing https for it probes a host that was never learned.
 *
 * THE EVENT IS A HOST ENTERING THE ENDPOINT SURFACE (solver/endpoint.c calls this at the one point a new
 * endpoint is created for an origin no endpoint had), which is why there is no set of "origins already asked"
 * anywhere: the surface's own identity dedup IS that fact, and a second memo beside it would be a seen-set the
 * engine maintained for itself. */
void discovery_seed_origin(JSContext *ctx, const char *origin);

/* READ ONE ANSWERED CANDIDATE. `url` is the candidate that was fetched and `reply` is the host's reply record
 * (`{status, statusText, headers, body, urlList}`) or JS_NULL for a network error — handed over exactly as it
 * arrived, because "there is no document at this address" is a positive answer and not an engine invariant.
 * Every method the document names is registered through solver/endpoint.c, so a discovery document's endpoints
 * are the SAME @H surface as the ones forced execution reached, deduped by the same identity. */
void discovery_reply(JSContext *ctx, const char *url, JSValueConst reply);

#endif
