/* HTML §4.12.5.1.7 "Path2D objects" — the interface a page constructs directly, and the first includer of
 * §4.12.5.1.6's `CanvasPath` mixin this engine builds.
 *
 * IT IS A COMPLETE COMPONENT WITH NO RENDERING CONTEXT ANYWHERE, which is the whole reason it can be built
 * now. §4.12.5.1.7 introduces the interface as "Path2D objects can be used to declare paths that are then
 * later used on objects implementing the CanvasDrawPath interface" — DECLARE here, USE there — and every one
 * of its steps is a list operation over subpaths: the constructor copies subpaths or parses SVG path data,
 * and `addPath` copies and transforms them. Nothing reads a bitmap, and the interface has NO ATTRIBUTES AT
 * ALL, so there is no observable this component could answer wrongly by being built before the contexts that
 * paint what it holds.
 *
 * THAT ALSO SETTLES WHY §NO-STUBS' PARTIAL-INTERFACE HAZARD DOES NOT REACH IT, and the reasoning matters more
 * than the conclusion. That hazard is about a FEATURE-DETECTED name: installing the object flips a page's
 * `if (window.X)` TRUE and abandons a fallback branch that was working. `Path2D` is not detected anywhere in
 * the corpus this product is measured against — every use is an unguarded `new Path2D(…)` or an
 * `x instanceof Path2D`, which is why engine/absentrank.mjs ranks it in its THROWS band rather than its
 * detect-only one. There is no guard to flip. The day a page writes `if (window.Path2D)`, absence is the
 * answer a browser without it gives and the argument would have to be made again.
 *
 * A RECEIVER IS PAGE-SUPPLIED INPUT. Every member below brand-checks `this` and throws a Web IDL §3.7.7
 * TypeError when it is not a Path2D — never a DCHECK, which would hand any page an abort switch for the whole
 * engine by writing `Path2D.prototype.moveTo.call(null)`. A forcing solver reaches exactly that constantly. */
#ifndef ENGINE_HOST_BROWSER_CORE_CANVAS_PATH_2D_H
#define ENGINE_HOST_BROWSER_CORE_CANVAS_PATH_2D_H

#include "quickjs.h"

/* Declared once per AGENT: the class, the constructor and the members. REGISTERS the per-realm install below,
   which is the whole of what a realm owes this component, so no host has a line to remember — see
   core/realm.h for why that list may not be hand-copied. */
void path_2d_init(JSContext *ctx);

/* §4.12.5.1.7's interface prototype object for ONE realm, its Web IDL §3.7.1 interface object, and §3.8's
   property reference for the one name this component owes a realm. `Path2D` is `[Exposed=(Window,Worker)]`,
   and §3.8's "define the global property references on target, given realm realm" names no Document — so the
   name is placed from the realm intrinsic rather than from a per-document column, which a worker realm never
   reaches. */
void path_2d_install_realm(JSContext *ctx);
void path_2d_free(void);

#endif /* ENGINE_HOST_BROWSER_CORE_CANVAS_PATH_2D_H */
