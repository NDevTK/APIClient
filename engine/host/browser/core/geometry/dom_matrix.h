/* Geometry Interfaces §6 "The DOMMatrix interfaces": DOMMatrixReadOnly and DOMMatrix, the sixteen elements and the
 * is 2D flag, §6.2 "Parsing a string into an abstract matrix" through core/css/css_transform_function.h's
 * grammar, and §2.1 "Transforming a point with a matrix". Each element is a JSValue (a Number or unknown external
 * input); a result computed from an unknown is concolic_new_derived over the operands it was computed from. */
#ifndef ENGINE_HOST_BROWSER_CORE_GEOMETRY_DOM_MATRIX_H
#define ENGINE_HOST_BROWSER_CORE_GEOMETRY_DOM_MATRIX_H

#include <stdbool.h>

#include "quickjs.h"

/* Declared once per agent; registers dom_matrix_install_realm as a realm intrinsic. */
void dom_matrix_init(JSContext *ctx);
/* §6's two prototypes and interface objects for one realm, with `LegacyWindowAlias=(SVGMatrix,WebKitCSSMatrix)`. */
void dom_matrix_install_realm(JSContext *ctx);
void dom_matrix_free(void);

/* §6.1's `dictionary DOMMatrixInit : DOMMatrix2DInit`, for a member whose argument is one. */
struct IdlDictDecl;
extern const struct IdlDictDecl DOM_MATRIX_INIT_DECL;

/* §2's matrixTransform: "create a DOMMatrix from the dictionary" `init` (a converted DOMMatrixInit) and §2.1 over
   `point`, a DOMPoint or DOMPointReadOnly. A new DOMPoint, or JS_EXCEPTION with validate and fixup's TypeError. */
JSValue dom_matrix_transform_point_init(JSContext *ctx, JSValueConst init, JSValueConst point);

#endif
