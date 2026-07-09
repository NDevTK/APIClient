/* Worker + SharedWorker constructor — Blink core/workers. The worker SCRIPT is code with its own endpoints,
 * fetched + analyzed like a <script src> chunk; the object's onmessage handler is a scheduler flow. See worker.c. */
#ifndef ENGINE_HOST_BROWSER_WORKER_H
#define ENGINE_HOST_BROWSER_WORKER_H
#include "quickjs.h"
JSValue js_worker_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv);   /* new Worker(url) / new SharedWorker(url) */
#endif
