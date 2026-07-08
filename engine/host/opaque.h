/* The OPAQUE sentinel + the minimal host-edge stubs — the shared concolic-value foundation.
 *
 * g_opaque is "external input the tool must not concretely decide": the default opaque value (shape "{}")
 * that generic propagation dups. Nearly every component returns it for a read it can't concretize, so it (and
 * the trivial stubs that return it) live in one place every TU includes rather than re-externing piecemeal. */
#ifndef ENGINE_HOST_OPAQUE_H
#define ENGINE_HOST_OPAQUE_H

#include "quickjs.h"

extern JSValue g_opaque;            /* the OPAQUE sentinel (created by opaque_init) */
void opaque_init(JSContext *ctx);   /* create g_opaque (qjs_init) */
void opaque_free(JSContext *ctx);   /* free g_opaque (teardown) */

/* Minimal host-edge stubs for a browser bundle: a no-op (addEventListener etc — the handler stays a
   never-fired function, so orphan-invoke drives it), and opaque-returning reads (DOM/response reads that are
   external input the tool must not concretely decide). A missing capability is a missing stub, never a
   parallel resolver — the real Lexbor DOM replaces these when the host is wired. */
JSValue js_noop(JSContext *ctx, JSValueConst t, int c, JSValueConst *v);
JSValue js_opaque(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
JSValue js_opaque_stub(JSContext *ctx, JSValueConst t, int c, JSValueConst *v);

#endif
