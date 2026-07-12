/* The Array interface a CONCOLIC value presents — the solver-owned policy of which Array.prototype iterators a
 * concolic (opaque, unknown-length) array routes to their SELF-HOSTED implementation (prelude.c) so iteration
 * parks per-element via {@iterdone} instead of collapsing. Installed into the fork via JS_SetArrayIterHook; the
 * vendored quickjs.c holds only the thin class_proto edge, this file holds the spec surface. */
#ifndef APICLIENT_SOLVER_CONCOLIC_ARRAY_H
#define APICLIENT_SOLVER_CONCOLIC_ARRAY_H

/* JSArrayIterHook: 1 iff a concolic array presents the self-hosted Array.prototype iterator `name`. */
int concolic_array_iter_method(const char *name);

#endif
