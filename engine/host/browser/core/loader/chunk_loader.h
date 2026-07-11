#ifndef ENGINE_HOST_BROWSER_CORE_LOADER_CHUNK_LOADER_H
#define ENGINE_HOST_BROWSER_CORE_LOADER_CHUNK_LOADER_H
#include "quickjs.h"
/* CHUNK LOADER (Blink core/loader — the resource fetch/eval of a lazy <script src> / dyn-import chunk).
 * A dynamically-inserted <script src> or a static-import dep is a STATIC resource: fetched ONCE (one-per-URL,
 * never re-fetched — a re-run re-uses the cached body), then evaluated to EXTEND the page baseline. Extracted
 * from main.c so the scheduler entry owns no loader control flow: main.c's qjs_provide asks chunk_provide first
 * (a pending chunk is fetched + evaluated here), and only a NON-chunk url falls through to reply delivery. */

void chunk_pending_add(const char *url);              /* queue url for host fetch (dedup; skip if already fetched this session) */
const char *chunk_list(void);                         /* '\n'-joined pending chunk urls (qjs_chunks -> the bridge's fetch list) */
int  chunk_pending_count(void);                       /* # of chunks awaiting fetch (qjs_step NEED_FETCH gate) */
int  chunk_provide(JSContext *ctx, const char *url, const char *body);   /* 1 if url was a pending chunk (fetched + evaluated here); 0 = not a chunk (caller handles it as a reply) */
void chunk_loader_free(void);                         /* free the pending + done registries (qjs_teardown) */

#endif
