/* HTML §8.1.3.2 "Environment settings objects"' module map, and the half of HTML §8.1.4.2 "Fetching scripts"'
 * fetch a single module script that reads and writes it — Blink core/script/module_map.cc.
 *
 * One map per realm (a settings object's), keyed by (request URL, module type). An entry is either in flight
 * (the spec's list of callbacks, here one promise every waiter reacts to) or a module script (here its source
 * text, which the importer compiles under the URL). Every fetch of a module, whether a `<link rel=modulepreload>`
 * or an import, asks this map first, so one entry has one fetch and every waiter takes its result.
 *
 * The record is per-realm and held as JS values, so a flow that writes an entry writes the record's properties
 * and the heap COW delta gives each flow its own map. */
#ifndef ENGINE_HOST_BROWSER_CORE_LOADER_MODULE_MAP_H
#define ENGINE_HOST_BROWSER_CORE_LOADER_MODULE_MAP_H
#include "quickjs.h"

/* HTML §8.1.4.2's moduleType for a fetch given no ModuleRequest (a modulepreload, a root module script). */
#define MODULE_TYPE_JAVASCRIPT_OR_WASM "javascript-or-wasm"

/* The agent's half: the per-realm slot and the intrinsic that gives every realm an empty map. */
void module_map_init(JSContext *ctx);
void module_map_free(JSRuntime *rt);

/* What fetch a single module script's steps 5-7 found for (url, module type). */
typedef enum {
    MODULE_MAP_READY,    /* step 5: a module script; the promise is already fulfilled with its source text */
    MODULE_MAP_JOINED,   /* step 6: a fetch is in flight; the promise is that fetch's */
    MODULE_MAP_FETCH,    /* step 7: the entry is new; the caller issues its one fetch */
} ModuleMapAsk;

/* Fetch a single module script's steps 4-7 over `realm`'s map. `*promise` (owned) settles with the module's
   source text, or rejects with a TypeError where the spec runs the callbacks with null. For MODULE_MAP_FETCH,
   `*fetch` (owned) names the new entry to module_map_settle; otherwise it is JS_UNDEFINED. */
ModuleMapAsk module_map_ask(JSContext *realm, const char *url, const char *module_type, JSValue *promise,
                            JSValue *fetch);

/* The entry's fetch's processResponseConsumeBody (step 13) given the reply record or a failure's null: makes the
   module script or none, writes or removes the entry, and settles its promise. Runs no page code. */
void module_map_settle(JSContext *ctx, JSValueConst fetch, JSValueConst reply);

/* module_map_settle as a function of one argument, for a park whose delivery calls a function. Owned. */
JSValue module_map_settle_function(JSContext *ctx, JSValueConst fetch);

#endif
