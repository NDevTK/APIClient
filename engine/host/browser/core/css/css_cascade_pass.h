/* css-cascade-5 §4.2 "Cascaded Values"' answer for one (element, property), held by a layout job
 * (core/layout/layout_job.h) for the job's lifetime.
 *
 * css-cascade-5 §6 "Cascading" is a function of the element and the property and of nothing about who asks,
 * and layout asks for the same pair over and over for two reasons that compound: css-cascade-5 §7.2
 * "Inheritance" makes a read of an inherited property climb to the parent, and css-logical-1 §4
 * "Flow-Relative Box Model Properties" makes `writing-mode`, `direction` and `text-orientation`, all inherited,
 * a prerequisite of cascading every margin, padding, border and inset. Both are one answer asked by many
 * callers, so the record is keyed on the question and sits at the one function every asker reaches.
 * `node engine/layout_cost.mjs <native binary>` measures the shapes this collapses. Beside the answers it holds
 * two inputs of the cascade that do not depend on the property: the author origin's sheet set per root and its
 * match result per element, so every miss after an element's first sorts declarations and matches no selector.
 * Both are functions of the inputs the validity argument below fixes. */

/* Validity. The record holds answers about the document the job's flow sees. Its inputs are asserted not to
 * move while it is held: tree and attribute writes at solver/dom_cow.c's write entries; every CSSOM write that
 * reaches the author origin (rules, declarations, a sheet's `disabled` flag, a sheet list, a MediaList) and
 * the custom element state `:defined` matches on, each at its own write through css_cascade_record_is_held().
 * A driven job rests only inside its own flow's member call, where that flow writes nothing and other flows'
 * writes stay in their deltas; the environment a media query reads is the realm's viewport, fixed per job.
 * It does not re-derive an answer at a hit, because that is re-running the cascade it exists to skip; the
 * shared probe is what keeps a slot answering only its own pair. */

/* The computed value (css_computed_value's text answer) is held beside the cascaded one under the same key,
 * which ends css-cascade-5 §7.2 "Inheritance"'s climb at the first answered ancestor. Beside the string entries
 * the record holds each element's typed computed values (core/css/css_computed_style.h), which every length-,
 * line-height- and border-spacing-shaped ask reads; its asks open the synchronous job's record when none is
 * held, so they are driven whoever asks.
 * NAMED RESIDUAL, undriven asks. Not covered: outside a driven job the record is held only while the
 * synchronous job has a drive open, so most asks of a layout entry that drives no job resolve the cascade.
 * Next: those entries driving a job (layout_job.h's synchronous-job residual). How its absence shows:
 * `_cascade.servedLife` far below `asksLife` on a run with no `scrollIntoView` and no paint. */

/* RETIREMENT: this component goes when the cascade produces a computed-style object that layout reads. */
#ifndef ENGINE_HOST_BROWSER_CORE_CSS_CSS_CASCADE_PASS_H
#define ENGINE_HOST_BROWSER_CORE_CSS_CSS_CASCADE_PASS_H

#include <stdbool.h>
#include <stddef.h>

#include <lexbor/dom/dom.h>

/* The record's storage, owned by a layout job. `tab` is a power-of-two table, NULL with `cap` 0 before the
   first entry; `release` frees every string it owns and leaves it empty. */
struct CpEntry;
struct CssAuthorSheets;
struct CssMatchResult;
struct CssComputedStyle;
typedef struct {
    struct CpEntry          *tab;
    size_t                   cap, used;
    struct CssAuthorSheets **author;      /* one per root resolved under, owned */
    size_t                   author_n, author_cap;
    struct CssMatchResult  **match;       /* power-of-two table keyed by element, NULL slot empty; owned */
    size_t                   match_cap, match_used;
    struct CssComputedStyle **style;      /* power-of-two table keyed by element, NULL slot empty; owned */
    size_t                   style_cap, style_used;
} CssCascadeRecord;
void css_cascade_record_release(CssCascadeRecord *r);

/* Whether the current job holds a record. Read by the write chokepoints of every cascade input, which
   assert it is not, and by nothing that decides what to compute. */
bool css_cascade_record_is_held(void);

/* The ask, counted whether or not a record is held. Answers true and writes `*out` when the record holds the
   pair; the value is an owned copy the caller frees as it frees the cascade's own answer, and NULL is an
   answer ("if the output of the cascade is an empty list, there is no cascaded value"). False means the
   caller owes the cascade and reports its answer with `css_cascade_pass_record`. */
bool css_cascade_pass_ask(lxb_dom_element_t *el, const char *name, char **out);

/* The cascade that ask cost, reported by the one caller on its false arm with no return between the two.
   `value` is borrowed; the record copies it. Counted whether or not a record is held. */
void css_cascade_pass_record(lxb_dom_element_t *el, const char *name, const char *value);

/* The same pair for css_computed_value's answer: `ask` answers an owned copy or false, and its one caller
   computes and reports with `record` on the false arm. Both are counted whether or not a record is held. */
bool css_cascade_pass_computed_ask(lxb_dom_element_t *el, const char *name, char **out);
void css_cascade_pass_computed_record(lxb_dom_element_t *el, const char *name, const char *value);

/* The author origin's sheet set for `root` (core/css/css_author_sheets.h). `ask` answers the held set,
   borrowed, or false. On false the caller builds one and offers it to `hold`, which takes it and answers true
   when a record is held; on false the caller still owns it and frees it after its one resolution. */
bool css_cascade_pass_author_ask(const lxb_dom_node_t *root, struct CssAuthorSheets **out);
bool css_cascade_pass_author_hold(struct CssAuthorSheets *sheets);

/* The author origin's match result for `el` (core/css/css_match_result.h) under `sheets`, the set its root
   resolves under. `ask` answers the held result, borrowed, or false; on false the caller builds one and
   offers it to `hold` with no return between the two, which takes it and answers true when a record is held
   and on false leaves it with the caller, who frees it after its one resolution. A held result borrows a
   sheet set the record holds. Both are counted whether or not a record is held. */
bool css_cascade_pass_match_ask(lxb_dom_element_t *el, const struct CssAuthorSheets *sheets,
                                struct CssMatchResult **out);
bool css_cascade_pass_match_hold(struct CssMatchResult *m);

/* The held computed style record of `el` (core/css/css_computed_style.h), created empty at its first ask and
   borrowed until the record is released; NULL when no record is held. */
struct CssComputedStyle *css_cascade_pass_style(lxb_dom_element_t *el);

/* The census, in solver/result.c's vocabulary. Every field is a lifetime counter of this agent and says so
   in its name. `asks_life` counts cascaded values asked for, `served_life` those the record answered and
   `resolved_life` those that ran the cascade; `asks_life == served_life + resolved_life` is asserted. The
   `computed_*` rows are the same three for the computed value and close the same way. The match result's
   asks, served and built close the same way and are asserted here; they are not rows yet. */
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
