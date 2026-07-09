/* DOMParser + Range: parse attacker-influenced HTML into a {parsedhtml}-tainted node.  See domparser.c.
 *
 * new DOMParser().parseFromString(html) and range.createContextualFragment(html) both take an HTML STRING
 * and return a node. Under forced-exec the returned node is a {parsedhtml} OPAQUE carrying the input's
 * concolic example, so a later `el.appendChild(parsed)` / innerHTML of it is detected as an @S sink and a
 * candidate flow's concrete payload rides through. */
#ifndef ENGINE_HOST_DOMPARSER_H
#define ENGINE_HOST_DOMPARSER_H
#include "quickjs.h"

JSValue js_domparser_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv);        /* new DOMParser() */
JSValue js_doc_createrange(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv); /* document.createRange() */

#endif
