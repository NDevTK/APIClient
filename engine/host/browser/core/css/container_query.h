/* CSS Conditional 5 §5.4 "Container Queries: the @container rule"'s condition, evaluated per element. A media
 * query is a fact about the document; a container query is asked "for each element", against the query
 * container selected for it (core/css/container_selector.h), so the author cascade carries one per emitted rule
 * (core/css/css_rule.h) and the match result asks it for the element a rule's selector matched. Blink's
 * ContainerQuery and ContainerQueryEvaluator under their own names.
 *
 * Evaluation reads layout: §6.1's size features query the selected container's content box, which a size
 * container's containment keeps independent of the elements under it, so the answer for one element never
 * reads that element's own style. */
#ifndef ENGINE_HOST_BROWSER_CORE_CSS_CONTAINER_QUERY_H
#define ENGINE_HOST_BROWSER_CORE_CSS_CONTAINER_QUERY_H

#include <stdbool.h>

#include <lexbor/dom/dom.h>

#include "core/css/css_at_rule_prelude.h"

/* One `@container` rule's `<container-condition>#`, parsed, and the `@container` rules it is nested in: "Style
   rules defined on an element inside multiple nested container queries apply when all of the wrapping container
   queries are true for that element." */
typedef struct ContainerQuery ContainerQuery;

/* The rule's conditions over the enclosing `@container` rule's query `up` (NULL at none), borrowed for the
   result's life. Copies what it keeps from `conds`. OWNED: container_query_free, which frees this level only. */
ContainerQuery *container_query_new(const CssContainerConditions *conds, const ContainerQuery *up);
void container_query_free(ContainerQuery *q);

/* §5.4: whether `q` and every query it is nested in are true for `el`. "A container query list is true if any
   of its component <container-condition>s are true"; a condition that selects no container is unknown, and
   unknown applies nothing. */
bool container_query_matches(const ContainerQuery *q, lxb_dom_element_t *el);

#endif
