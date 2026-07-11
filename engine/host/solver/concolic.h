/* The maximally-unknown CONCOLIC value + the minimal host-edge stubs — the shared concolic-value foundation.
 *
 * g_concolic is "external input the tool must not concretely decide": the most-general concolic value (shape
 * "{}", no source, no example yet) that generic propagation dups. Nearly every component returns it for a read
 * it can't concretize, so it (and the trivial stubs that return it) live in one place every TU includes rather
 * than re-externing piecemeal. (It is the concolic-spectrum's unknown end, not a "know-nothing" sentinel.) */
#ifndef ENGINE_HOST_CONCOLIC_H
#define ENGINE_HOST_CONCOLIC_H

#include "quickjs.h"

extern JSValue g_concolic;            /* the maximally-unknown concolic value (created by concolic_init) */
void concolic_init(JSContext *ctx);   /* create g_concolic (qjs_init) */
void concolic_free(JSContext *ctx);   /* free g_concolic (teardown) */

/* Minimal host-edge stubs for a browser bundle: a no-op (addEventListener etc — the handler stays a
   never-fired function, so orphan-invoke drives it), and concolic-returning reads (DOM/response reads that are
   external input the tool must not concretely decide). A missing capability is a missing stub, never a
   parallel resolver — the real Lexbor DOM replaces these when the host is wired. */
JSValue js_noop(JSContext *ctx, JSValueConst t, int c, JSValueConst *v);
JSValue js_concolic_read(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
JSValue js_concolic_stub(JSContext *ctx, JSValueConst t, int c, JSValueConst *v);

/* A CONCOLIC CONSTANT: a value tagged `shape` so a branch on it still FORKS (explore both worlds — more
   logic, you don't know which arm ships an endpoint), carrying `example` as its concrete value (model, never
   lost). The shared primitive for a modelable-but-branch-relevant environment value (navigator/screen/media).
   CONSUMES `example`. Bare-concrete would delete the fork; a bare unknown would drop the value; this is both. */
JSValue js_concolic(JSContext *ctx, const char *shape, JSValue example);

/* Leaf intrinsics the self-hosted builtins need (the JS-visible __isOpaque / __opaqueExample names): a CONCRETE
   bool "is this value still symbolic (unresolved concolic)?" (sort collapses a meaningless symbolic order
   without forking) and the concrete EXAMPLE it carries (stringify serializes a config value to its real value,
   else undefined). Leaves — hold no continuation. */
JSValue js_is_concolic(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
JSValue js_concolic_example(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);

#endif
