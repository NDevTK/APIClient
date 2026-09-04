/* PERFORMANCE TIMELINE §3 The PerformanceEntry interface — THE BASE EVERY TIMED THING IN THE PLATFORM IS.
 *
 * WHAT IT IS. One record of one measurement: a name, the kind of measurement it is, when it started and how
 * long it lasted. Every timing standard in the platform derives from it — User Timing's mark and measure,
 * Navigation Timing's navigation, Resource Timing's resource, Event Timing's event — which is why it is a
 * component of its own rather than a struct inside whichever of those landed first.
 *
 * WHY IT IS SEPARATE FROM core/timing/performance.c. §7 of HIGH RESOLUTION TIME owns the Performance OBJECT,
 * whose two members answer questions about the ENVIRONMENT (the clock and the time origin). An entry answers
 * questions about ONE PAST EVENT and holds no environment at all; the only thing the two share is that a
 * PARTIAL interface in some third standard puts a member on the first that mints the second. Folding them
 * would give the environment object a record it never reads.
 *
 * ---- WHAT IS BUILT AND WHAT IS NOT, WHICH IS THE WHOLE OF THIS COMPONENT'S HONESTY -------------------------
 *
 * BUILT: the interface, its class, its per-realm prototype and interface object, and the four attributes §3
 * states GETTER PROSE for — `name`, `entryType`, `startTime`, `duration` — plus Web IDL §3.7.7.1.1's default
 * `toJSON` over them. Also §3's "initialize a PerformanceEntry" operation, which is the ONE door a derived
 * interface mints through.
 *
 * NOT BUILT, AND NAMED RATHER THAN SHAPED — §3's `id` AND `navigationId`:
 *   WHAT IS NOT COVERED. Both are declared in §3's IDL and neither is installed, so `entry.id` and
 *     `entry.navigationId` are ABSENT rather than answering a number.
 *   WHY, AND IT IS NOT THE SAME REASON FOR THE TWO. `navigationId` has getter prose ("This attribute MUST
 *     return the value it is initialized to") and its ONE writer is §5.1 Queue a PerformanceEntry steps 5 and
 *     6, which this build does not run — see the residual on that below. `id` IS DIFFERENT AND THE DIFFERENCE
 *     IS A SPEC GAP: §3 declares `readonly attribute unsigned long long id` in its IDL, gives it NO getter
 *     prose at all, and does NOT initialize it in "initialize a PerformanceEntry" — its only writer is §5.1
 *     step 1, which is itself guarded on "If newEntry's id is unset". So for an entry that has never been
 *     queued the standard states no value for `id` whatever, and installing a getter would mean CHOOSING one.
 *     CLAUDE.md's §Attacker-sources rule that a value known only to satisfy a gate is invented rather than
 *     computed is the same rule one level out: a zero here would be a number this engine made up, and a page
 *     could not tell it from a number it measured.
 *   WHAT THE NEXT DIFF BUILDS. §5.1 Queue a PerformanceEntry, at which point step 1 gives `id` a value through
 *     §5.7 Generate a Performance Entry id (a per-global "last performance entry id" this component would
 *     hold) and steps 5-6 give `navigationId` one; both getters are then installed here, reading fields §5.1
 *     wrote. Note that §5.1 step 6's "set newEntry's navigationId to null" cannot be represented at the IDL's
 *     non-nullable `unsigned long long` — a second gap in the same pair of attributes, to be resolved against
 *     the standard rather than papered over at the getter.
 *   HOW ITS ABSENCE WOULD SHOW. `'id' in performance.mark('x')` is false where a browser that has shipped
 *     these answers true, and `entry.toJSON()` has four keys where §3.7.7.1.1 over the full IDL gives six.
 *
 * NOT BUILT — §5.1 Queue a PerformanceEntry AND THE PERFORMANCE ENTRY BUFFER. There is deliberately no buffer
 * in this component, and the reason is a rule rather than a shortage of effort. The buffer's only readers in
 * the platform are §2.1.1 getEntries(), §2.1.2 getEntriesByType(), §2.1.3 getEntriesByName() and §4's
 * PerformanceObserver, and every one of them is ABSENT from this build — so a buffer added now would be a
 * write with no reader, which CLAUDE.md's §A-FIELD-A-CONSUMER-DEFAULTS rates as a broken contract in its own
 * right. AND ADDING THE READERS WITH IT WOULD BE WORSE, not better: this build mints exactly one entry type,
 * so `performance.getEntries()` would hand a page a timeline containing marks and NOTHING ELSE, and a page
 * cannot tell that from a page on which nothing else happened. core/timing/performance.h already states the
 * shape of that defect for this exact surface, in its own words: an empty list is the plausible datum that
 * makes a page conclude there was no navigation to time. It is the same sentence here with entries in it.
 *   WHAT THE NEXT DIFF BUILDS: §5.1 and the per-global performance entry buffer map it appends to, TOGETHER
 *     WITH at least one non-mark entry type (Navigation Timing §5 Creating a navigation timing entry is the
 *     one every page has), so the first reader installed answers about a timeline rather than about marks.
 *   HOW ITS ABSENCE WOULD SHOW: `performance.mark('a')` returns a real PerformanceMark and
 *     `performance.getEntriesByName('a')` is a TypeError naming the absent operation — which is the forcing
 *     function, and is what a page that stores marks and reads them back trips over.
 */
#ifndef ENGINE_HOST_BROWSER_CORE_TIMING_PERFORMANCE_ENTRY_H
#define ENGINE_HOST_BROWSER_CORE_TIMING_PERFORMANCE_ENTRY_H

#include <stdbool.h>

#include "quickjs.h"
#include "solver/cow.h"

/* ---- THE DERIVED INTERFACE'S HALF OF ONE ENTRY ------------------------------------------------------------
 *
 * §3 is a BASE INTERFACE and every entry a page ever sees is an instance of something derived from it, so this
 * component owns §3's four fields and knows nothing about the fields the deriving standard adds — User Timing
 * §2.2's `detail`, Resource Timing's dozen. The deriving component states those in a struct whose FIRST member
 * is the PerfEntry below, and states THIS record beside it: its IDL identifier, the COW layout of the WHOLE
 * derived struct, and how to release and to mark the fields §3 does not know about.
 *
 * IT IS ALSO THE BRAND, AND THAT IS WHY `name` IS COMPARED BY POINTER. Every entry in the platform wears ONE
 * class id — the base's — because the opaque is one allocation and a JSValue carries one class; a derived
 * class id is a per-realm PROTOTYPE SLOT and nothing else (the same split core/file/file_system_handle.c
 * makes). So `JS_GetClassID` cannot tell a mark from a measure, and Web IDL §3.7.6 Attributes' brand for
 * `PerformanceMark.prototype.detail` has to be asked of something the page cannot forge: this pointer, which
 * is a static in the component that owns the interface and can only have been written by its own mint. */
typedef struct PerfEntryClass {
    const char      *name;   /* the most-derived interface's IDL identifier — the brand, compared by POINTER */
    const CowRecord *rec;    /* the DERIVED struct's layout: its size, and EVERY owned JSValue in it */
    /* The derived struct's owned JSValues, released and marked. The FOUR of §3 are this component's and are
       not passed here — a deriving component that freed them would double-free, and one that forgot to mark
       its own would have gc_scan read them as rooted from outside the heap. NULL is legitimate for a derived
       interface that adds no owned value; it is not a hole, because `rec` states the same fact independently
       and performance_entry_new asserts the two agree. */
    void (*release)(JSRuntime *rt, void *derived);
    void (*mark)(JSRuntime *rt, void *derived, JS_MarkFunc *mark_func);
} PerfEntryClass;

/* PERFORMANCE TIMELINE §3's OWN FIELDS — "initialize a PerformanceEntry entry given a DOMHighResTimeStamp
   startTime, a DOMString entryType, a DOMString name, and an optional DOMHighResTimeStamp endTime (default 0)",
   plus PERFORMANCE TIMELINE §3's "A PerformanceEntry has a DOMHighResTimeStamp end time, initially 0" that
   operation's last step writes.
   `name` AND `start_time` ARE JSValues AND NOT A C STRING AND A DOUBLE, and that is the solver half of this
   component rather than a convenience. Both can be UNKNOWN EXTERNAL INPUT — `performance.mark(location.hash)`
   is the ordinary spelling of a name a page did not compute — and PERFORMANCE TIMELINE §3 says each attribute
   "must return the value it is initialized to", so holding the value ITSELF is both the literal reading and
   what keeps a page's `sink(entry.name)` carrying the taint it arrived with. A C string would launder it. */
typedef struct PerfEntry {
    const PerfEntryClass *cls;
    JSValue name;         /* §3's name — a DOMString, or the unknown that denotes one */
    JSValue entry_type;   /* §3's entryType — a DOMString from the entry type registry */
    JSValue start_time;   /* §3's startTime — a DOMHighResTimeStamp, or the unknown that denotes one */
    double  end_time;     /* §3's end time, initially 0 — the operand `duration`'s getter steps branch on */
} PerfEntry;

/* Declared ONCE PER AGENT — the class, and the per-realm prototype and interface object it declares. */
void performance_entry_init(JSContext *ctx);
void performance_entry_free(void);

/* §3's "initialize a PerformanceEntry", as the ONE mint. The caller has already allocated its DERIVED struct
   (whose first member is a PerfEntry) and filled the fields §3 does not own; this performs §3's five steps
   over the base, wraps it in an object of THIS realm wearing `proto`, and takes ownership of the three values.
   `proto` is the DERIVED interface's prototype and is BORROWED. */
JSValue performance_entry_new(JSContext *ctx, JSValueConst proto, const PerfEntryClass *cls, void *derived,
                              JSValue start_time, JSValue entry_type, JSValue name, double end_time);

/* THE BASE OF AN ENTRY, or NULL for anything that is not one — and it CAPTURES, so every derived component's
   accessor reaches its own fields through this rather than through JS_GetOpaque. That is not a convenience:
   the capture must happen where a flow REACHES the record, and routing it here is what makes it impossible for
   a derived interface to add an accessor and forget one. */
PerfEntry *performance_entry_of(JSValueConst v);
/* Web IDL §3.7 Interfaces' implementation-check, for §3 itself and for one derived interface. `cls` is the
   deriving component's own static — see PerfEntryClass. */
bool performance_entry_is(JSValueConst v);
bool performance_entry_is_a(JSValueConst v, const PerfEntryClass *cls);

/* §3's interface prototype object FOR THIS REALM — what a derived interface's prototype chains to. */
JSValue performance_entry_proto(JSContext *ctx);

#endif
