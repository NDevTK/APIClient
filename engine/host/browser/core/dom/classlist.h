/* element.classList — contains/add/remove/toggle over the REAL class attribute (COW-captured per flow), so
 * `classList.add('admin'); if(classList.contains('admin')) fetch(...)` reaches the class-gated endpoint. */
#ifndef ENGINE_HOST_CLASSLIST_H
#define ENGINE_HOST_CLASSLIST_H

#include "quickjs.h"

/* el.classList getter -> an object with contains/add/remove/toggle bound to the element. */
JSValue js_el_classlist_get(JSContext *ctx, JSValueConst this_val);

#endif
