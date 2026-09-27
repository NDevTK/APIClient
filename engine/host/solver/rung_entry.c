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
    /* THE PROPERTY SPELLING OF THE SAME NAME, ON A RECEIVER THE SOURCE SPELLS AS THE GLOBAL — and a THIRD
       count rather than a contribution to either above, because the guard the pair above is split on does not
       exist here: `typeof window.x` and `window.x` are the same field get (see the header). A reader who wants
       the union adds two of these; nobody is handed a sum somebody else took. */
    long named_prop;
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

/* THE NAMES THIS ENGINE BINDS TO A REALM'S OWN GLOBAL OBJECT, which is the whole of the receiver test the
   property spelling gets and is a SOURCE-TEXT test. `window`, `self` and `frames` are HTML §7.2.2 "The Window
   object"'s and browser/core/frame/window.c installs all three with the global itself as the value;
   `globalThis` is ECMAScript §19.1 "Value Properties of the Global Object"'s and the interpreter installs it.
   WHAT IS DELIBERATELY NOT HERE AND WHY EACH ABSENCE IS THE WITHHOLDING DIRECTION. `top` and `parent` are the
   NAVIGABLE'S — §7.2.2.4 walks the parent chain, so a child realm's `parent.requestIdleCallback` is a read of
   ANOTHER agent's global and is not this document naming its own rung's work. `global` is Node's and this
   engine binds nothing to it, so `global.x` is an absent global and solver/absent.c's question rather than
   this one. A page may reassign `self`, `frames` and `globalThis` — all three are writable — so this test
   presumes the standard bindings; a bundle that rebinds one raises a row by one on a name it does not name,
   which is the only direction in which this table can be wrong and is the reason the set is the narrow one.
   AND IT IS A SECOND STATEMENT OF ONE SET, WHICH IS RECORDED RATHER THAN HIDDEN: testing/static_surface.mjs
   holds `GLOBAL_OBJECTS` for the same purpose over a mirrored corpus, and the two differ today — it carries
   `global` and not `frames`, which is correct for a pass that must refuse to guess and wrong for an engine
   that knows what it installed. The residual at the foot of this file names what removes the copy. */
static const char *const GLOBAL_SELF_NAMES[] = { "window", "self", "globalThis", "frames", NULL };

static int base_is_global(const char *base) {
    for (int i = 0; GLOBAL_SELF_NAMES[i]; i++)
        if (strcmp(GLOBAL_SELF_NAMES[i], base) == 0) return 1;
    return 0;
}

void rung_entry_compile_global_member(const char *base, const char *member) {
    if (!base || !member) return;
    /* THE RECEIVER FIRST, BECAUSE IT IS WHAT MAKES THIS A REFINEMENT RATHER THAN A LOUDER ROW. A wrapper's own
       member, a bundler's `(0,o.requestIdleCallback)` re-export shim and an `api.fetch` each read a property of
       a receiver that is NOT the global, and every one of them would raise a denominator this document does not
       owe — the direction that manufactures a finding out of a correctly-silent rung. Their SHARE of the
       property reads of these names is a fact about real bundles and is measured by
       `node testing/static_surface.mjs`, never asserted here. */
    if (!base_is_global(base)) return;
    /* …and then exactly the walk its sibling performs, over the SAME tables, for the same reason: every
       declared rung is offered the name and none is told which matched. */
    for (int s = 0; s < RUNG_SLOT_N; s++) {
        const char *const *n = g_rungs[s].names;
        if (!n) continue;
        for (int i = 0; n[i]; i++) {
            if (strcmp(n[i], member) != 0) continue;
            g_rungs[s].named_prop++;
            break;
        }
    }
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
        json_buf_raw(&b, ",");
        json_buf_key(&b, "stepNamedRenderingPropLife");
        rung_num(&b, g_rungs[RUNG_SLOT_RENDERING].named_prop);
        emitted++;
    }
    if (g_rungs[RUNG_SLOT_TIMER].names) {
        json_buf_raw(&b, ",");
        json_buf_key(&b, "stepNamedTimerLife");
        rung_num(&b, g_rungs[RUNG_SLOT_TIMER].named);
        json_buf_raw(&b, ",");
        json_buf_key(&b, "stepNamedTimerTypeofLife");
        rung_num(&b, g_rungs[RUNG_SLOT_TIMER].named_typeof);
        json_buf_raw(&b, ",");
        json_buf_key(&b, "stepNamedTimerPropLife");
        rung_num(&b, g_rungs[RUNG_SLOT_TIMER].named_prop);
        emitted++;
    }
    if (g_rungs[RUNG_SLOT_IDLE_PERIOD].names) {
        json_buf_raw(&b, ",");
        json_buf_key(&b, "stepNamedIdleLife");
        rung_num(&b, g_rungs[RUNG_SLOT_IDLE_PERIOD].named);
        json_buf_raw(&b, ",");
        json_buf_key(&b, "stepNamedIdleTypeofLife");
        rung_num(&b, g_rungs[RUNG_SLOT_IDLE_PERIOD].named_typeof);
        json_buf_raw(&b, ",");
        json_buf_key(&b, "stepNamedIdlePropLife");
        rung_num(&b, g_rungs[RUNG_SLOT_IDLE_PERIOD].named_prop);
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
   @kind lifetime: stepNamedRenderingLife stepNamedRenderingTypeofLife stepNamedRenderingPropLife
   @kind lifetime: stepNamedTimerLife stepNamedTimerTypeofLife stepNamedTimerPropLife
   @kind lifetime: stepNamedIdleLife stepNamedIdleTypeofLife stepNamedIdlePropLife
   EVERY ROW HERE IS A LIFETIME COUNT AND NONE IS A GAUGE — they may be differenced and accumulated, they cannot
   decrease, and a sample below its predecessor is this file and not the run. THE KIND IS STATED AT THE EMITTER
   because the person who adds a row is the person who knows what may be done with it, which is the rule
   solver/result.c's own declaration block gives at length.
   AND THEY ARE NOT SITE COUNTS. A program is recompiled by every flow that replays it, so these count COMPILER
   RESOLUTIONS and are read as a BIT — zero against nonzero — never as a magnitude and never against a static
   per-bundle figure. The names say `Named` rather than `Sites` so that nobody reads them as the second.
   NO CONTAINMENT WITH `stepUnitRuns` MAY BE ASSERTED IN EITHER DIRECTION, which is why there is no identity in
   this file over the pair the header is about. A rung may run MORE times than its names were resolved (one
   `setInterval` call feeds the timer arm for ever) and it may run FEWER, and the two inequalities are both
   ordinary.
   THAT SENTENCE READ `and it may run FEWER (the finding)`, AND THE PARENTHESIS IS RETIRED RATHER THAN DELETED
   BECAUSE IT IS THE LABEL A READER RE-DERIVES FROM THIS PAIR ALONE. Running fewer is TWO states and not one:
   the rung was REACHED and declined every time, or the ladder never gave it a turn — and only the first is a
   fact about the rung. solver/engine.h's `clock_render_asks` triple is the arrival count that separates them,
   published beside `stepUnitRuns`, and this file's own header now carries the three-row ladder. THE SENTENCE
   AROUND IT IS UNCHANGED AND WAS NEVER THE DEFECT: it is about MAGNITUDES, where no containment holds in
   either direction, and the claim that went wrong was the one-word LABEL on one of its two inequalities. */

/* THE RESIDUAL THAT ASKED FOR THE `…PropLife` ROWS IS RETIRED, AND IT IS REWRITTEN RATHER THAN DELETED BECAUSE
   ITS REMEDY CLAUSE WAS WRONG IN TWO WAYS A READER WILL RE-DERIVE FROM ITS OWN REASONING. Its NOT-COVERED half
   was exact and is what located the work: this file counted ONE SPELLING, the free identifier, so
   `window.requestAnimationFrame` reached no global resolution and a zero denominator against a zero numerator
   read as the correct silence on a document that hangs plenty off the rung — and a polyfill is the ordinary
   place a bundle writes the property spelling, since the whole point of one is to assign the global it probes.
   Its remedy half named `a MEMBER-NAME channel at the field-get emitter … consumed here by the same tables. The
   names would not change`, and BOTH of those are false.
   THE SEAM IS NOT THE FIELD-GET EMITTER. A property read is where the RECEIVER exists, and testing it there is
   what a reader reaches for — but that is the interpreter, downstream of REACH, and reach is the one arm
   §AN-INVARIANT-OVER-A-GATED-OPERATION says the ask must be recorded upstream of. A row raised there would
   answer `a flow got to a property-spelled read`, which is the three-state zero the rows above exist to end,
   arriving in the diff that was supposed to refine them. The seam is the SAME funnel the bare spelling uses:
   at `<free identifier>.<member>` the compiler has already established that nothing binds the base, and the
   member is the atom of the field get adjacent to it.
   AND THE NAMES HAD TO CHANGE, WHICH IS THE HALF NO AMOUNT OF CARE ABOUT THE SEAM WOULD HAVE REACHED. The pair
   above is split on a guard that does not exist for a property: ECMAScript §13.5.3 step 2.a needs a
   non-throwing read only for an unresolvable REFERENCE, and a property of an object is `undefined` when absent,
   so the unary parser patches nothing and `typeof window.x` emits the same field get as `window.x`. Reporting
   into `…NamedIdleLife` would have merged a population that is largely FEATURE DETECTION into the row read as
   uses — inverting the one distinction that split exists for, on the rung where it matters most, since a name a
   bundle probes is by construction a name that is not universally present.

   A NAMED RESIDUAL — the code here is CORRECT for what it does and NARROWER than the question it answers.
   WHAT IS NOT COVERED: A COMPUTED MEMBER OF THE GLOBAL BELONGS TO NO NAME. `self[n]` and `window[k]` are an
   array element and not a field get, and the key is a value rather than an atom, so no channel keyed on a
   property NAME can attribute one — neither this one nor the bare-spelling rows, which makes it a floor under
   BOTH columns rather than under one. The same holds for `window?.x`, whose optional-chain test stands between
   the base and the field get so the two are no longer adjacent, and for a base that is not a bare identifier at
   all (`Mi().requestIdleCallback`, `(0,o.requestIdleCallback)`) — a receiver a source spells as an expression is
   refused here BY DESIGN, since admitting it is what would make this a louder instrument instead of a
   refinement, and a receiver that a call RETURNS the global from is the one member of that set the refusal is
   wrong about.
   WHAT THE NEXT DIFF BUILDS: nothing in this file. The computed-key half is answerable only where the key's own
   VALUE is in hand, which is the interpreter, so it is a row of a DIFFERENT kind on a different census and it
   must not be summed into these — a reader who wants it asked upstream of reach is asking for something no
   instrument can hold, because the key does not exist until something runs.
   HOW ITS ABSENCE WOULD SHOW: a document whose three rows for one rung all read zero while that rung's runs are
   nonzero — the pair the header calls THE FINDING, inverted, since a rung cannot run work nothing named. Read
   as a defect in the rung it sends a reader to the rung's component; read as this residual it sends them to
   whether the document reaches its entry through a computed key, and only the second is the question.
   RETIREMENT: this residual goes when a row on this census states how many reads of a global self-name took a
   COMPUTED key, because the floor is then a number a reader can weigh rather than a sentence they must believe.

   AND A SECOND ONE, ABOUT THE RECEIVER TEST RATHER THAN THE MEMBER. WHAT IS NOT COVERED: `GLOBAL_SELF_NAMES` is
   a SECOND STATEMENT of which names a realm binds to its own global — testing/static_surface.mjs states the same
   set as `GLOBAL_OBJECTS` for the same purpose, and the two already differ, so this is a copy and not merely a
   risk of one. The names are installed in browser/core/frame/window.c and by the interpreter's own intrinsics,
   which is where the fact is, and neither of those tells either copy anything.
   WHAT THE NEXT DIFF BUILDS: the set declared where it is installed, borrowed and never copied, exactly as a
   rung's entry table is — a realm's global installer lending its own table at its per-realm install, so a realm
   that binds a fourth self-reference gains the receiver test for it with no edit here and the mirrored-corpus
   pass derives the set from that declaration the way it already derives the rung tables from theirs.
   HOW ITS ABSENCE WOULD SHOW: a reader compares this channel's rows against a mirrored-corpus count of
   property-spelled sites and finds this one lower by exactly the sites whose receiver is spelled with a name one
   copy holds and the other does not — a disagreement whose size is a property of two lists rather than of any
   program, and which neither output attributes to the lists.
   RETIREMENT: this record goes when the receiver set reaching this file is borrowed from the component that
   installs it, so no list here can be the one that drops a name. */
