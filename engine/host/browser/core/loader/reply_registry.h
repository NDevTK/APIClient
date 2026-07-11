#ifndef ENGINE_HOST_BROWSER_CORE_LOADER_REPLY_REGISTRY_H
#define ENGINE_HOST_BROWSER_CORE_LOADER_REPLY_REGISTRY_H
/* REPLY FETCH REGISTRY (Blink core/loader) — the set of same-origin resource URLs a consumed reply
 * (r.json()/r.text()) asked the trusted offscreen to fetch. The host relays the list to safeFetch and calls
 * qjs_provide with each body; the concolic reply value is parked + delivered in solver/reply.c. This registry
 * is JUST the pending-fetch list (dedup + one-per-url), extracted from main.c so the scheduler entry owns no
 * loader bookkeeping — parallel to chunk_loader (chunks) for reply fetches. */

void reply_fetch_register(const char *url, int is_json);   /* a consumed reply's url -> queue a host fetch (dedup); is_json is the parse tag reply.c parks with */
const char *reply_pending_list(void);                      /* '\n'-joined pending reply urls (qjs_pending -> the bridge's fetch list) */
int  reply_pending_count(void);                            /* # of replies awaiting fetch (qjs_step NEED_FETCH gate) */
void reply_pending_drop(const char *url);                  /* url delivered (its body cached + parked consumers resolved) -> drop the registration */
void reply_registry_free(void);                            /* free the pending registry (session reset / finalize / teardown) */

#endif
