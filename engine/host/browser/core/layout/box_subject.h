/* The subject of a layout crash: the box, node or computed value an aborting message is about, as opposed to
 * the function that noticed (the symbolized frames name that). One member walk asks per box, so the possible
 * subjects number the document's boxes, and naming the subject is part of the assert.
 *
 * Every function here is total and box_subject.c does not include `check.h`: an assert firing while a
 * `DFAILF` composes its message would replace the defect being reported. Each arm answers a distinct sentence
 * ("(no element)", "(no local name)", "(no computed value)"); the invariant those name keeps its own crash at
 * the site that depends on the value. So a message names a box through this component, never through a
 * caller's cascade predicate that DCHECKs, and nothing here asks whether a box is replaced (that entry aborts
 * by name for the element kinds a layout message often reports).
 *
 * Results are copied into the caller's buffer; the cascade's allocation is freed inside, so no crash site
 * frees anything. A local name is printed with `%.*s` and Lexbor's length, since it has no terminator promise
 * (C99 §7.19.6.1 "The fprintf function": with a precision the array need not "contain a null character"). */
#ifndef ENGINE_HOST_BROWSER_CORE_LAYOUT_BOX_SUBJECT_H
#define ENGINE_HOST_BROWSER_CORE_LAYOUT_BOX_SUBJECT_H
/* Named residual: `css_computed_value` aborts outside the keyword-valued properties it answers (a length,
   an unmodelled name, `line-height`, a non-`none` `transform`), so totality here is this component's own, not
   its callee's. Not covered: a property name outside that set. Next: `css_computed_value.h` exports that
   answering set as a predicate (`css_computed_models` admits `line-height` and `transform`, so it cannot
   serve). Its absence shows as an abort inside message composition reporting the cascade's invariant
   instead of the layout defect. */
#include <stddef.h>

#include <lexbor/dom/dom.h>

/* `<div> (display `block`)` — the element and the one computed value every layout remedy is stated against.
   Answers a sentence without touching `buf` for a NULL element; an element with no local name is composed into
   `buf` as `(no local name) (display ...)`. */
const char *box_subject(lxb_dom_element_t *el, char *buf, size_t cap);

/* The same for a node that may not be an element (a run over a child list, a parent deciding a containing
   block, a descendant walk through text): it names the node kind where "(no element)" would be wrong. */
const char *box_subject_node(lxb_dom_node_t *n, char *buf, size_t cap);

/* One computed value, copied into the caller's buffer and the cascade's string released here. See the residual
   above for which property names the callee answers. */
const char *box_subject_computed(lxb_dom_element_t *el, const char *name, char *buf, size_t cap);

#endif
