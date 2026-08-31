/* HTML §8.1.8.1 "Event handlers" — THE ATTRIBUTE CHANGE STEPS, the half of that section that turns markup into
 * a handler. See event_handler_attribute.c.
 *
 * WHY THIS IS A FILE AND NOT A BRANCH IN core/events/event_target.c. §8.1.8.1 is written as one section and
 * implemented as two, and the seam is not arbitrary: everything the IDL attribute half needs is a handler map
 * and a name, and everything THIS half needs is an ELEMENT — step 1's "the name of an event handler content
 * attribute ON ELEMENT" is a question about which element, and step 5.1's "Should element's inline behavior be
 * blocked by Content Security Policy?" is a question about that element's document's policy container.
 * core/events/event_target.c may name neither: it deliberately depends on no DOM at all, because a `#include`
 * of the tree there made every host that installs events link lexbor with it, which is why §2.9's propagation
 * path is registered rather than called. So the five steps live here, where the element is, with all five
 * numbers visible in one function, and the three algorithms §8.1.8.1 names in their own right — determine the
 * target of an event handler, deactivate an event handler, and the map write that activate follows — are
 * called across the seam.
 *
 * WHAT IT IS FOR, WHICH IS NOT CONFORMANCE. `<button onclick="doAdminThing()">` is a population of code a
 * forced-execution run cannot enter while the attribute is inert, and every sink behind an inline handler is
 * unreachable — the two halves of one section, one built and one not, was the largest reachability gap in the
 * engine. It is also how ordinary pages start: `<body onload="init()">`.
 *
 * WHAT IS NOT BUILT AND WHERE IT CRASHES. §8.1.8.1's "get the current value of the event handler" step 3 — the
 * COMPILE — turns the internal raw uncompiled handler this file stores into a function, and it does not exist.
 * A handler these steps register is therefore registered, positioned in the listener list, and ABORTS when the
 * dispatch walk or the IDL getter reads it, naming the compile. That is the state this file is FOR: silence was
 * the previous one. */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_EVENT_HANDLER_ATTRIBUTE_H
#define ENGINE_HOST_BROWSER_CORE_HTML_EVENT_HANDLER_ATTRIBUTE_H
#include <stddef.h>

#include <lexbor/dom/dom.h>

#include "quickjs.h"

/* HTML §8.1.8.1's five ATTRIBUTE CHANGE STEPS, as one of DOM §4.9's — registered on core/dom/element.c's
 * element_attr_changed beside html_link_attr_changed and custom_elements_attribute_changed, and there rather
 * than in an IDL setter for the reason every one of its neighbours is: a content attribute has more than one
 * spelling (`el.setAttribute("onclick", s)`, `el.attributes.onclick.value = s`, a parser's write, an
 * `innerHTML` reparse) and an IDL setter answers for exactly one of them — and for this attribute family the
 * IDL setter is a DIFFERENT ALGORITHM, storing a callback object where these steps store an uncompiled body.
 *
 * `value` is NULL for a REMOVED attribute, which is step 4's own condition, and is otherwise BORROWED and NOT
 * NUL-terminated: it is the (pointer, length) DOM §4.9 carries across the write, and an event handler body may
 * legitimately contain a U+0000 (ECMAScript §11.1 "Source Text" permits every code point from U+0000). Reading
 * it to the first NUL would compile a different program from the one the page wrote. */
void event_handler_attribute_changed(JSContext *ctx, lxb_dom_element_t *el, const char *ns, const char *local,
                                     const char *value, size_t value_len);

/* …AND THE SAME FIVE STEPS FOR THE TREE A PARSE BUILT, which is not an optimisation of the entry above but the
 * only route the markup case has. A lexbor parse writes its attributes through `lxb_dom_attr_set_value_wo_copy`
 * and reaches none of DOM §4.9's mutation chokepoint, so a document's OWN `<body onload="init()">` — the single
 * commonest way a bundle starts — fires nothing above. It is the same absence core/dom/element.c's named
 * residual describes for the whole parsed-walk family (html_base_element_parsed, html_script_parsed,
 * media_element_parsed, html_image_parsed, html_link_parsed, html_style_element_parsed), and it is answered the
 * same way: the steps the parse did not run are run over the FINISHED tree, so the order within the parse is
 * lost while the effects are not. Losing the order costs nothing here — §8.1.8.1's steps run no script, and a
 * handler cannot be dispatched at before the parse is over.
 *
 * SHADOW-INCLUDING, for media_element_parsed's reason: by the time this runs, a `<template shadowrootmode>` has
 * been converted, so an `<svg onload=…>` that was inside it is in a shadow tree and a plain descendant walk
 * would not find it. */
void event_handler_attribute_parsed(JSContext *ctx, lxb_dom_node_t *root);

#endif
