/* A layout job's record of the cascade's per-element answers, held for the job's lifetime
 * (core/layout/layout_job.h): the author origin's sheet set per root, and per element its match result
 * (core/css/css_match_result.h), its cascaded values (core/css/css_cascaded_style.h) and its computed values
 * (core/css/css_computed_style.h).
 *
 * Every answer is a function of the element and of inputs the validity argument below fixes, and layout asks
 * for each over and over: css-cascade-5 §7.2 "Inheritance" makes a read of an inherited property read the
 * parent, and css-logical-1 §4 "Flow-Relative Box Model Properties" makes `writing-mode`, `direction` and
 * `text-orientation` a prerequisite of cascading every margin, padding, border and inset. An ask made while no
 * job holds a record opens a drive on the synchronous job for its own length (css_computed_style_hold_begin),
 * so a record is held for every ask. */

/* NAMED RESIDUAL, undriven asks. Not covered: an ask that no driven job or open drive holds a record for gets
 * one for its own length, so consecutive undriven asks of one element each collect and resolve its whole
 * cascade again. Next: those layout entries driving a job (layout_job.h's synchronous-job residual). How its
 * absence shows: `_cascade.resolvedLife` near half of `asksLife` on a run with no `scrollIntoView` and no
 * paint. */

/* Validity. The record holds answers about the document the job's flow sees. Its inputs are asserted not to
 * move while it is held: tree and attribute writes at solver/dom_cow.c's write entries; every CSSOM write that
 * reaches the author origin (rules, declarations, a sheet's `disabled` flag, a sheet list, a MediaList) and
 * the custom element state `:defined` matches on, each at its own write through css_cascade_record_is_held().
 * A driven job rests only inside its own flow's member call, where that flow writes nothing and other flows'
 * writes stay in their deltas; the environment a media query reads is the realm's viewport, fixed per job.
 * It does not re-derive an answer at a hit, because that is re-running the cascade it exists to skip. */
#ifndef ENGINE_HOST_BROWSER_CORE_CSS_CSS_CASCADE_PASS_H
#define ENGINE_HOST_BROWSER_CORE_CSS_CSS_CASCADE_PASS_H

#include <stdbool.h>
#include <stddef.h>

#include <lexbor/dom/dom.h>

/* The record's storage, owned by a layout job. `elem` is a power-of-two table keyed by element, NULL with
   `elem_cap` 0 before the first entry; `release` frees everything it holds and leaves it empty. */
struct CpElement;
struct CssAuthorSheets;
struct CssMatchResult;
struct CssCascadedStyle;
struct CssComputedStyle;
typedef struct {
    struct CssAuthorSheets **author;      /* one per root resolved under, owned */
    size_t                   author_n, author_cap;
    struct CpElement        *elem;        /* one slot per element asked about, owned */
    size_t                   elem_cap, elem_used;
} CssCascadeRecord;
void css_cascade_record_release(CssCascadeRecord *r);

/* Whether the current job holds a record. Read by the write chokepoints of every cascade input, which
   assert it is not, and by the asks below, which require it. */
bool css_cascade_record_is_held(void);

/* The author origin's sheet set for `root` (core/css/css_author_sheets.h). `ask` answers the held set,
   borrowed, or false; on false the caller builds one and hands it to `hold`, which takes it. */
bool css_cascade_pass_author_ask(const lxb_dom_node_t *root, struct CssAuthorSheets **out);
void css_cascade_pass_author_hold(struct CssAuthorSheets *sheets);

/* The author origin's match result for `el` under `sheets`, the set its root resolves under. `ask` answers
   the held result, borrowed, or false; on false the caller builds one and hands it to `hold` with no return
   between the two. A held result borrows a sheet set the record holds. */
bool css_cascade_pass_match_ask(lxb_dom_element_t *el, const struct CssAuthorSheets *sheets,
                                struct CssMatchResult **out);
void css_cascade_pass_match_hold(struct CssMatchResult *m);

/* `el`'s cascaded style, borrowed, or NULL when the record holds none yet; on NULL the caller collects one,
   hands it to `hold` and then resolves it, with no return between. A held style borrows the layer order of
   a sheet set the record holds. */
struct CssCascadedStyle *css_cascade_pass_cascaded_ask(lxb_dom_element_t *el);
void css_cascade_pass_cascaded_hold(struct CssCascadedStyle *s);

/* The held computed style record of `el` (core/css/css_computed_style.h), created empty at its first ask and
   borrowed until the record is released; NULL when no record is held. */
struct CssComputedStyle *css_cascade_pass_style(lxb_dom_element_t *el);

/* The census, in solver/result.c's vocabulary. Every field is a lifetime counter of this agent and says so
   in its name. `asks_life` counts cascaded values asked for, `served_life` those an element's held cascaded
   style answered and `resolved_life` those that resolved the element's cascaded style first;
   `asks_life == served_life + resolved_life` is asserted. The `computed_*` rows are
   core/css/css_computed_style.h's asks, served and derived, over every shape. The match result's asks, served
   and built close the same way and are asserted here; they are not rows yet. */
typedef struct {
    long long asks_life;
    long long served_life;
    long long resolved_life;
    long long computed_asks_life;
    long long computed_served_life;
    long long computed_resolved_life;
} CssCascadePassCensus;
void css_cascade_pass_census(CssCascadePassCensus *out);

#endif /* ENGINE_HOST_BROWSER_CORE_CSS_CSS_CASCADE_PASS_H */
