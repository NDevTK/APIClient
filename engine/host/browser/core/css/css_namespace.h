/* THE `CSS` NAMESPACE — CSSOM §8.1 The CSS.escape() Method's `namespace CSS`, and CSS Conditional Rules
 * Module Level 3 §7.5 The CSS namespace, and the supports() function's `partial namespace CSS` on top of it.
 *
 * IT IS ITS OWN COMPONENT AND NOT A MEMBER OF THE CSS GROUP UNDER `element`, because what it is is a GLOBAL
 * whose two operations happen to be about CSS. §8.1's own last paragraph says so from the other side —
 * "Specifications that define operations on the CSS namespace and want to store some state should store the
 * state on the current global object's associated Document" — a namespace several standards extend, none of
 * which is CSSStyleDeclaration's. So it is a row on core/platform.h's list, where a host cannot express its
 * absence, and it has a witness there: `CSS` is a name a page reads off the global, and an install that
 * silently stopped happening is exactly what that column exists to catch.
 *
 * IT ANSWERS TWO QUESTIONS AND OWNS NEITHER OF THEM. Both members are ONE call into a component that already
 * decides the thing, which is the whole reason this file is short:
 *   - `escape(ident)` "must return the result of invoking serialize an identifier of ident", and
 *     core/css/css_serialize.h's serialize-an-identifier IS CSSOM §2.1 Common Serializing Idioms' primitive
 *     every serialize-a-CSS-rule arm is already stated over. §8.1 is that primitive under another name; a
 *     second escape table here would be a copy free to disagree, and the disagreement would be invisible
 *     because both produce a string that LOOKS like CSS.
 *   - `supports(conditionText)` is core/css/css_supports.h's `<supports-condition>`, asked TWICE (see below).
 *   - `supports(property, value)` is a DIFFERENT algorithm and not that one with a colon in it — see
 *     css_namespace.c, where §7.5's own two Notes are written out beside the code that obeys them.
 *
 * WHAT IS HONESTLY ABSENT: nothing of §8.1, and nothing of §7.5. Both of that namespace's published operations
 * are here, so a page reaching for a third — `CSS.registerProperty` is CSS Properties and Values API §2, a
 * different standard's partial namespace — gets the ReferenceError-shaped TypeError a missing operation is,
 * which is the forcing function naming the standard to implement rather than a shape-only object swallowing
 * the call. */
#ifndef ENGINE_HOST_BROWSER_CORE_CSS_CSS_NAMESPACE_H
#define ENGINE_HOST_BROWSER_CORE_CSS_CSS_NAMESPACE_H

#include "quickjs.h"

/* THE AGENT'S HALF: the two operations' pool entries. Declares the per-realm intrinsic, so every realm — the
   agent's first and every child navigable's — mints its OWN namespace object with its own two function
   objects. A namespace minted once into a static would answer every document from the realm that defined it,
   which is not a wrong identity but a wrong ANSWER: `js_call_c_function` takes the callee's realm, so the one
   `CSS.escape` would run every frame's call in the first frame's context. */
void css_namespace_init(JSContext *ctx);

/* THE AGENT'S HALF, UNDONE. The namespace object and its two function objects are the REALMS' and go with
   their contexts; what the agent holds is two pool ids. */
void css_namespace_free(void);

#endif
