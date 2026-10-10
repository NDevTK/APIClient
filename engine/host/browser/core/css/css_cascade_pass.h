/* css-cascade-5 §4.2 "Cascaded Values"' answer for one (element, property), held by a layout job
 * (core/layout/layout_job.h) for the job's lifetime.
 *
 * css-cascade-5 §6 "Cascading" is a function of the element and the property and of nothing about who asks,
 * and layout asks for the same pair over and over for two reasons that compound: css-cascade-5 §7.2
 * "Inheritance" makes a read of an inherited property climb to the parent, and css-logical-1 §4
 * "Flow-Relative Box Model Properties" makes `writing-mode`, `direction` and `text-orientation`, all inherited,
 * a prerequisite of cascading every margin, padding, border and inset. Both are one answer asked by many
 * callers, so the record is keyed on the question and sits at the one function every asker reaches.
 * `node engine/layout_cost.mjs <native binary>` measures the shapes this collapses. */

/* Validity. The record holds answers about the document the job's flow sees. Its inputs are asserted not to
 * move while it is held: tree and attribute writes at solver/dom_cow.c's write entries; every CSSOM write that
 * reaches the author origin (rules, declarations, a sheet's `disabled` flag, a sheet list, a MediaList) and
 * the custom element state `:defined` matches on, each at its own write through css_cascade_record_is_held().
 * A driven job rests only inside its own flow's member call, where that flow writes nothing and other flows'
 * writes stay in their deltas; the environment a media query reads is the realm's viewport, fixed per job.
 * It does not re-derive an answer at a hit, because that is re-running the cascade it exists to skip; the
 * shared probe is what keeps a slot answering only its own pair. */

/* NAMED RESIDUAL, the computed value. Not covered: this is the cascaded value, so css-cascade-5 §7's
 * defaulting still runs per ask and §7.2's inheritance still climbs once per descendant, each frame now a
 * probe instead of a sheet walk. Next: the computed value beside it under the same key, which ends the climb
 * at the first answered ancestor. How its absence shows: layout_cost.mjs's per-ancestor frame count staying
 * superlinear in the deep shape.
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
typedef struct {
    struct CpEntry *tab;
    size_t          cap, used;
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

/* The census, in solver/result.c's vocabulary. Every field is a lifetime counter of this agent and says so
   in its name. `asks_life` counts cascaded values asked for, `served_life` those the record answered and
   `resolved_life` those that ran the cascade; `asks_life == served_life + resolved_life` is asserted. */
typedef struct {
    long long asks_life;
    long long served_life;
    long long resolved_life;
} CssCascadePassCensus;
void css_cascade_pass_census(CssCascadePassCensus *out);

#endif /* ENGINE_HOST_BROWSER_CORE_CSS_CSS_CASCADE_PASS_H */
