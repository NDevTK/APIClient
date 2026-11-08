/* Geometry Interfaces §5 "The DOMQuad interface": four DOMPoint objects, which the author can modify and which
 * directly affect the quadrilateral. */
#ifndef ENGINE_HOST_BROWSER_CORE_GEOMETRY_DOM_QUAD_H
#define ENGINE_HOST_BROWSER_CORE_GEOMETRY_DOM_QUAD_H

#include "quickjs.h"

/* Declared once per agent; registers dom_quad_install_realm as a realm intrinsic. */
void dom_quad_init(JSContext *ctx);
/* §5's prototype and interface object for one realm. */
void dom_quad_install_realm(JSContext *ctx);
void dom_quad_free(void);

#endif
