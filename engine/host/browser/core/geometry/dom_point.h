/* Geometry Interfaces §2 "The DOMPoint interfaces": DOMPointReadOnly and DOMPoint, a point's four internal member
 * variables (x coordinate, y coordinate, z coordinate, w perspective) and §2.1 "Transforming a point with a
 * matrix", which core/geometry/dom_matrix.h performs. Each variable is a JSValue (a Number or unknown external
 * input), for the reason core/geometry/dom_rect.c states for a rectangle's. */
#ifndef ENGINE_HOST_BROWSER_CORE_GEOMETRY_DOM_POINT_H
#define ENGINE_HOST_BROWSER_CORE_GEOMETRY_DOM_POINT_H

#include <stdbool.h>

#include "quickjs.h"

/* Declared once per agent; registers dom_point_install_realm as a realm intrinsic. */
void dom_point_init(JSContext *ctx);
/* §2's two prototypes and interface objects for one realm, and the `LegacyWindowAlias=SVGPoint` reference. */
void dom_point_install_realm(JSContext *ctx);
void dom_point_free(void);

/* A new DOMPoint (`mutable`) or DOMPointReadOnly in `ctx` with the four variables set. CONSUMES the four, each a
   Number or unknown external input. */
JSValue dom_point_new(JSContext *ctx, bool mutable_point, JSValue x, JSValue y, JSValue z, JSValue w);

/* Whether `v` is a DOMPoint or a DOMPointReadOnly. */
bool dom_point_is(JSValueConst v);

/* The four variables of the point `v`, borrowed from it, in x, y, z, w order. `v` must be a point (asserted). */
void dom_point_values(JSValueConst v, JSValueConst out[4]);

/* §2's `dictionary DOMPointInit`, for a member whose argument or dictionary member is one. */
struct IdlDictDecl;
extern const struct IdlDictDecl DOM_POINT_INIT_DECL;

/* "Create a DOMPoint from the dictionary" over a converted DOMPointInit `init`. */
JSValue dom_point_from_init(JSContext *ctx, JSValueConst init);

#endif
