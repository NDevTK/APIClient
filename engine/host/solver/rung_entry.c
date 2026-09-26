#include "solver/rung_entry.h"
#include "core/json_buf.h"
#include "check.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* THE RUNGS THAT HAVE A DENOMINATOR, THEIR ENUM MEMBER AND THEIR TWO ROW NAMES, AS ONE LIST. It is a list and
   not three pairs of `#define`s for `ENDPOINT_DOORS`' reason: the slot table, the declaration's membership test
   and the census over it are ONE enumeration, so a rung added here gains its slot, its assertion arm and its
   two rows in the same expansion and a rung added anywhere else gains none of them.
   THE ROW NAMES ARE LITERALS BECAUSE `json_buf_key` TAKES ONE and the compiler enforces it (core/json_buf.h
   states why), which is the property that makes every field name this seam publishes readable off this source.
   That is also the whole reason the list exists rather than a loop over `StepUnit`: a computed key is not a
   thing this seam can express, so the rungs that may be counted are exactly the rungs written here.
   WHY THESE THREE AND NOT THE REST OF THE LADDER: they are the arms whose work a PROGRAM asks for by naming a
   global — solver/engine.c's rendering, timer and idle arms. `run-a-task`, `deliver-one-reply` and the program
   arms are asked for by the document's own structure or by a reply arriving, so there is no identifier a
   compiler could resolve for them and a row here would be a denominator of nothing. */
#define RUNG_ENTRIES(X)                                                                                       \
    /* §8.1.7.3's in-parallel half. engine.c's own arm names `requestAnimationFrame`, ResizeObserver delivery, \
       IntersectionObserver, scroll/resize/pagereveal and the Web Animations checkpoint as its work; which of  \
       those the components have BUILT is core/rendering's fact and not this file's, so this rung's row is a    \
       floor that RISES as they land rather than a fixed set. */                                               \
    X(RENDERING)                                                                                              \
    /* HTML §8.7 "Timers" — ONE task source that both `setTimeout` and `setInterval` queue on, which is why a  \
       one-name-per-rung column would have published a fraction of this rung's own denominator. */             \
    X(TIMER)                                                                                                  \
    /* Cooperative Scheduling of Background Tasks §5.1 "Start an idle period algorithm". */                    \
    X(IDLE_PERIOD)

#define RUNG_SLOT_ENUM(id) RUNG_SLOT_##id,
typedef enum { RUNG_ENTRIES(RUNG_SLOT_ENUM) RUNG_SLOT_N } RungSlot;
#undef RUNG_SLOT_ENUM

/* ONE SLOT PER COUNTED RUNG. `names` is the component's own table, borrowed; the two counts are LIFETIME counts
   of COMPILER RESOLUTIONS and are never reset, which is `g_step_unit_runs`' scope and is what makes the pair
   comparable — see the header. */
typedef struct {
    const char *const *names;
    long named;          /* an ordinary reference: a read that throws when nothing binds it */
    long named_typeof;   /* the non-throwing form, which is `typeof x` and nothing else */
} RungEntry;

static RungEntry g_rungs[RUNG_SLOT_N];

/* WHICH SLOT A `StepUnit` IS, OR -1. A SWITCH AND NOT AN ARRAY INDEXED BY THE ENUM, deliberately: an array over
   `STEP_UNIT_N` would be a table whose every other arm is a structural hole, and the hole is what a reader
   cannot tell from a declared rung standing at zero. The switch answers the membership question directly and
   the compiler checks the arms against the list above. */
static int rung_slot_of(StepUnit u) {
    switch (u) {
#define RUNG_SLOT_ARM(id) case STEP_UNIT_##id: return RUNG_SLOT_##id;
    RUNG_ENTRIES(RUNG_SLOT_ARM)
#undef RUNG_SLOT_ARM
    default: return -1;
    }
}

void rung_entry_declare(StepUnit u, const char *const *names) {
    int slot = rung_slot_of(u);

    DCHECKF(names != NULL && names[0] != NULL,
            "the %s rung declared an empty entry table — its denominator row would read zero for every document "
            "and a reader would take that for a corpus of programs none of which names this rung's work, which "
            "is the one reading this row exists to make impossible",
            step_unit_name(u));
    /* A RUNG WITH NO ROW MAY NOT DECLARE, and this is the assertion the header promises rather than a silence
       discovered later: the row names above are literals, so a rung this file has no key for would be counted
       here and published nowhere — the write-with-no-reader §A-FIELD-A-CONSUMER-DEFAULTS names, which no census
       can show because the absent row looks exactly like a row nobody added yet. */
    DCHECKF(slot >= 0,
            "the %s rung declared an entry table and this census has no row for it — every counted rung is a "
            "member of RUNG_ENTRIES, which is the one list its slot, its arm here and its two row names come "
            "off, so a rung outside it would raise a count that reaches no reader",
            step_unit_name(u));
    if (slot < 0) return;
    /* RE-DECLARED WITH THE SAME TABLE IS THE ORDINARY CASE AND COSTS NOTHING — these are per-REALM installs and
       an instance builds many realms. A DIFFERENT table is two components claiming one arm's denominator, which
       they may not: the count is read against ONE arm of `stepUnitRuns`, and two components' names summed into
       it would publish a denominator of two populations under one rung's name. */
    if (g_rungs[slot].names) {
        DCHECKF(g_rungs[slot].names == names,
                "a SECOND entry table was declared for the %s rung — this row is the denominator of ONE arm of "
                "`stepUnitRuns`, so two tables summed into it would grade one arm's runs against a population "
                "that is partly another component's",
                step_unit_name(u));
        return;
    }
    g_rungs[slot].names = names;
}

void rung_entry_compile_global_named(const char *name, int typeof_only) {
    if (!name) return;
    /* EVERY DECLARED RUNG IS OFFERED THE NAME AND NONE IS TOLD WHICH MATCHED, so a name that somehow belonged to
       two rungs would raise both rather than whichever arm ran first — the silent double-attribution no total
       reveals. The tables are disjoint today, being three task sources of three standards. */
    for (int s = 0; s < RUNG_SLOT_N; s++) {
        const char *const *n = g_rungs[s].names;
        if (!n) continue;
        for (int i = 0; n[i]; i++) {
            if (strcmp(n[i], name) != 0) continue;
            if (typeof_only) g_rungs[s].named_typeof++;
            else             g_rungs[s].named++;
            break;   /* a table may not list one name twice, and if it did this would not count it twice */
        }
    }
}

/* …the same number conversion solver/endpoint.c's `edge_num` performs, and for its reason: core/json_buf.h has
   no numeric entry, so a count reaches the buffer as the bytes of its own decimal form. */
static void rung_num(JsonBuf *b, long v) {
    char t[32];
    int n = snprintf(t, sizeof t, "%ld", v);

    DCHECK(n > 0 && (size_t)n < sizeof t,
           "a census number did not fit its own conversion buffer — the width of a long's decimal form is a "
           "property of the type, so this is a host whose long is wider than this buffer was written for and "
           "the number about to be published has lost its leading digits");
    json_buf_raw(b, t);
}

char *rung_entry_rows(void) {
    JsonBuf b = { 0 };
    int declared = 0, emitted = 0;

    for (int s = 0; s < RUNG_SLOT_N; s++) if (g_rungs[s].names) declared++;
    /* NOTHING DECLARED IS THE ABSENT FORM AND IS NOT SIX ZEROES — see the header. `json_buf_take` on an untouched
       buffer answers the empty string, which is what the splice wants, and the leading commas below are why the
       rows and their separator go together. */
    if (!declared) return json_buf_take(&b);
    /* THE KEYS ARE WRITTEN OUT AND ARE NOT GENERATED FROM `RUNG_ENTRIES`, which is solver/endpoint.c's own rule
       for its two edges and is load-bearing twice. `json_buf_key` takes a STRING LITERAL and the compiler
       enforces it, so a key composed by a macro parameter is not a thing this seam can express — and the wider
       reason is that testing/census_rows.js reads a `key`-shaped composer by matching that macro's literal
       argument, so a generated key would publish a row whose KIND no artifact states and which that gate is
       structurally unable to see. A field name this seam publishes is readable off this source or it is not
       checked by anything.
       ONLY THE DECLARED RUNGS, WHICH MAKES THE KEY SET ITSELF THE STATEMENT OF WHICH ARMS HAVE A DENOMINATOR. A
       rung whose component this host did not install has no row; a rung that IS installed and whose names the
       compiler never resolved has a row reading 0. Two different facts, told apart by presence. */
    if (g_rungs[RUNG_SLOT_RENDERING].names) {
        json_buf_raw(&b, ",");
        json_buf_key(&b, "stepNamedRenderingLife");
        rung_num(&b, g_rungs[RUNG_SLOT_RENDERING].named);
        json_buf_raw(&b, ",");
        json_buf_key(&b, "stepNamedRenderingTypeofLife");
        rung_num(&b, g_rungs[RUNG_SLOT_RENDERING].named_typeof);
        emitted++;
    }
    if (g_rungs[RUNG_SLOT_TIMER].names) {
        json_buf_raw(&b, ",");
        json_buf_key(&b, "stepNamedTimerLife");
        rung_num(&b, g_rungs[RUNG_SLOT_TIMER].named);
        json_buf_raw(&b, ",");
        json_buf_key(&b, "stepNamedTimerTypeofLife");
        rung_num(&b, g_rungs[RUNG_SLOT_TIMER].named_typeof);
        emitted++;
    }
    if (g_rungs[RUNG_SLOT_IDLE_PERIOD].names) {
        json_buf_raw(&b, ",");
        json_buf_key(&b, "stepNamedIdleLife");
        rung_num(&b, g_rungs[RUNG_SLOT_IDLE_PERIOD].named);
        json_buf_raw(&b, ",");
        json_buf_key(&b, "stepNamedIdleTypeofLife");
        rung_num(&b, g_rungs[RUNG_SLOT_IDLE_PERIOD].named_typeof);
        emitted++;
    }
    /* AND THE TWO SIDES IN ONE HAND, which is what the declaration's own membership assert cannot reach. That one
       refuses a rung outside `RUNG_ENTRIES`; this one refuses a rung INSIDE it that this function has no arm
       for — a slot that accumulates a count reaching no reader, which is §A-FIELD-A-CONSUMER-DEFAULTS' write
       with no reader and the one thing a census cannot show, since the absent row looks exactly like a row
       nobody has added yet. It is the reason the keys being written out costs nothing: the day a fourth rung
       joins the list, THIS fires rather than its denominator going quietly unpublished. */
    DCHECKF(emitted == declared,
            "%d rungs declared an entry table and this census published %d of them — every counted rung needs "
            "an arm here, because its row name is a literal and cannot be composed, so a declared rung with no "
            "arm raises a count that reaches no reader and reads as a rung nobody has instrumented yet",
            declared, emitted);
    return json_buf_take(&b);
}

/* @kinds-of rungEntry
   @kind lifetime: stepNamedRenderingLife stepNamedRenderingTypeofLife
   @kind lifetime: stepNamedTimerLife stepNamedTimerTypeofLife
   @kind lifetime: stepNamedIdleLife stepNamedIdleTypeofLife
   EVERY ROW HERE IS A LIFETIME COUNT AND NONE IS A GAUGE — they may be differenced and accumulated, they cannot
   decrease, and a sample below its predecessor is this file and not the run. THE KIND IS STATED AT THE EMITTER
   because the person who adds a row is the person who knows what may be done with it, which is the rule
   solver/result.c's own declaration block gives at length.
   AND THEY ARE NOT SITE COUNTS. A program is recompiled by every flow that replays it, so these count COMPILER
   RESOLUTIONS and are read as a BIT — zero against nonzero — never as a magnitude and never against a static
   per-bundle figure. The names say `Named` rather than `Sites` so that nobody reads them as the second.
   NO CONTAINMENT WITH `stepUnitRuns` MAY BE ASSERTED IN EITHER DIRECTION, which is why there is no identity in
   this file over the pair the header is about. A rung may run MORE times than its names were resolved (one
   `setInterval` call feeds the timer arm for ever) and it may run FEWER (the finding), and the two inequalities
   are both ordinary. */

/* A NAMED RESIDUAL — the code here is CORRECT for what it does and NARROWER than the question it answers.
   WHAT IS NOT COVERED: ONE SPELLING. This counts the free identifier only, because that is what the compiler
   resolves against the global object. A program that reaches the same work through a PROPERTY of the global —
   `window.requestAnimationFrame`, `self[n]`, a parameter a bundle shadowed the name with — is a property read or
   a local slot and reaches no global resolution at all, so it raises nothing here. The row is therefore a FLOOR
   in the direction that WITHHOLDS a finding: a zero denominator against a zero numerator reads as the correct
   silence above, on a document that may hang plenty off the rung. That population is not marginal for exactly
   these rungs — a `requestAnimationFrame` polyfill is the ordinary place a real bundle writes the property
   spelling, since the whole point of one is to assign the global it is probing.
   WHAT THE NEXT DIFF BUILDS: a MEMBER-NAME channel at the field-get emitter, which is the one seam that sees
   `window.requestAnimationFrame` and `navigator.sendBeacon` and `xhr.open` in a single place, reported through
   this same hook's sibling and consumed here by the same tables. The names would not change.
   HOW ITS ABSENCE WOULD SHOW: a reader meets a document whose row here reads zero while an independent parse of
   the same served bytes attaches work to that rung, and the census still reads as the correct silence — the two
   instruments disagree and nothing in either output says which spelling this one could not see. */
