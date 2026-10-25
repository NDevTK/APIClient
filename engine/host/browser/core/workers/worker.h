/* HTML §10.2.6.3 "Dedicated workers and the Worker interface". See worker.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_WORKERS_WORKER_H
#define ENGINE_HOST_BROWSER_CORE_WORKERS_WORKER_H

#include "quickjs.h"

/* Declared once per runtime from core/platform.c's `worker` row; released by its release column. */
void worker_init(JSContext *ctx);
void worker_free(JSRuntime *rt);

#endif
