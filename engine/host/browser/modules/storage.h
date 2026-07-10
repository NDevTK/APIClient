/* Web storage (localStorage / sessionStorage) as a CONCOLIC round-trip.
 *
 * setItem(k,v) records v; getItem(k) recovers it — but stored data is EXTERNAL INPUT (attacker-tamperable
 * across sessions/tabs), so getItem returns an opaque-for-control-flow value that CARRIES the stored value as
 * its concrete @H example when the bundle set it this run (a URL/id round-tripped through storage), and a bare
 * opaque {ls} otherwise. Self-contained: owns the key->value table; the only shared symbol is the opaque
 * sentinel. main.c registers these C-functions on the storage objects. */
#ifndef ENGINE_HOST_STORAGE_H
#define ENGINE_HOST_STORAGE_H

#include "quickjs.h"
#include "opaque.h"   /* g_opaque — getItem returns it when nothing was stored for the key this run */

JSValue js_storage_get(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);   /* getItem / key */
JSValue js_storage_set(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);   /* setItem */
void storage_free(JSContext *ctx);   /* teardown: drop this run's stored values */

#endif
