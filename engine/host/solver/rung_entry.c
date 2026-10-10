#include "solver/rung_entry.h"
#include "solver/compose.h"   /* the splice's leading comma — see rung_entry_rows */
#include "solver/metrics.h"   /* the rows, declared in solver/metrics/rung_entry.def */
#include "check.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* THE RUNGS THAT HAVE A DENOMINATOR, THEIR ENUM MEMBER AND THEIR TWO ROW NAMES, AS ONE LIST. It is a list and
   not three pairs of `#define`s for `ENDPOINT_DOORS`' reason: the slot table, the declaration's membership test
   and the census over it are ONE enumeration, so a rung added here gains its slot, its assertion arm and its
   two rows in the same expansion and a rung added anywhere else gains none of them.
   The row names are declared in solver/metrics/rung_entry.def, each a literal, so a rung added here also needs
   its rows there and an arm in rung_entry_rows, which asserts it has one for every declared rung.
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
/* THE RECEIVER TEST IS NO LONGER HERE AND THE `base` NO LONGER ARRIVES, which is a deletion and not a loss. A
   wrapper's own member, a bundler's `(0,o.requestIdleCallback)` re-export shim and an `api.fetch` each read a
   property of a receiver that is NOT the global, and every one of them would raise a denominator this document
   does not owe — that refusal still happens, ONCE, at solver/concolic.c's dispatch, which is the file that owns
   the hook table and reaches both of its consumers. What this file held was a SECOND statement of which names a
   realm binds to its own global, and the residual below records that it was already the second of three; a
   property channel for solver/endpoint.c's doors would have made it four, so the set moved to the one place both
   consumers pass through instead. Their SHARE of the property reads of these names is a fact about real bundles
   and is measured by `node testing/static_surface.mjs`, never asserted anywhere. */
void rung_entry_compile_global_member(const char *member) {
    if (!member) return;
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

/* One declared rung's three rows, or their absence: a rung whose component this host did not install has no
   rows, and a rung that is installed and whose names the compiler never resolved has rows reading 0. Two
   different facts, told apart by presence. */
#define RUNG_SAMPLE(slot, named_id, typeof_id, prop_id)            \
    do {                                                            \
        if (g_rungs[slot].names) {                                  \
            METRIC_SET(named_id, g_rungs[slot].named);              \
            METRIC_SET(typeof_id, g_rungs[slot].named_typeof);      \
            METRIC_SET(prop_id, g_rungs[slot].named_prop);          \
            emitted++;                                              \
        } else {                                                    \
            METRIC_ABSENT(named_id);                                \
            METRIC_ABSENT(typeof_id);                               \
            METRIC_ABSENT(prop_id);                                 \
        }                                                           \
    } while (0)

/* No containment with `stepUnitRuns` holds in either direction, so none is asserted: a rung may run more times
   than its names were resolved (one `setInterval` call feeds the timer arm for ever) or fewer, and running fewer
   is two states — reached and declined, or never given a turn — which solver/engine.h's `clock_render_asks`
   triple, published beside `stepUnitRuns`, separates. */
char *rung_entry_rows(void) {
    int declared = 0, emitted = 0;
    char *rows, *out;

    for (int s = 0; s < RUNG_SLOT_N; s++) if (g_rungs[s].names) declared++;
    RUNG_SAMPLE(RUNG_SLOT_RENDERING, rung_named_rendering, rung_named_rendering_typeof, rung_named_rendering_prop);
    RUNG_SAMPLE(RUNG_SLOT_TIMER, rung_named_timer, rung_named_timer_typeof, rung_named_timer_prop);
    RUNG_SAMPLE(RUNG_SLOT_IDLE_PERIOD, rung_named_idle, rung_named_idle_typeof, rung_named_idle_prop);
    /* A slot of `RUNG_ENTRIES` with no arm above would accumulate a count that reaches no reader, which a census
       cannot show: the absent row looks exactly like a row nobody has added yet. */
    DCHECKF(emitted == declared,
            "%d rungs declared an entry table and this census published %d of them — every counted rung needs "
            "an arm here and rows in solver/metrics/rung_entry.def, or its count reaches no reader",
            declared, emitted);
    rows = metrics_family_members_text(METRIC_FAMILY_RUNG_ENTRY);
    if (!rows || !rows[0]) return rows;
    out = composef(",%s", rows);
    free(rows);
    return out;
}
#undef RUNG_SAMPLE

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

   AND A SECOND ONE, ABOUT THE RECEIVER TEST RATHER THAN THE MEMBER — AND ITS FIRST CLAUSE IS NOW ABOUT ANOTHER
   FILE, which is why it is rewritten here rather than deleted: the hazard is unchanged and only its address
   moved. WHAT IS NOT COVERED: the receiver set is still a SECOND STATEMENT of which names a realm binds to its
   own global — testing/static_surface.mjs states the same set as `GLOBAL_OBJECTS` for the same purpose, and the
   two already differ, so it is a copy and not merely a risk of one. What changed is that this file no longer
   holds one: `GLOBAL_SELF_NAMES` lives at solver/concolic.c's dispatch, which both consumers of
   `.global_member_named` are reached through, so a door channel added for solver/endpoint.c did not make it a
   third. The names are installed in browser/core/frame/window.c — which binds `window`, `self` and `frames` to
   the global BY VALUE — and `globalThis` by the interpreter's own intrinsics, which is where the fact is, and
   neither of those tells either remaining copy anything.
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
