/* Work the thread does outside the interpreter's dispatch, which retires no opcode: decoding bytes, tokenizing
 * HTML, parsing CSS, and parsing script (the last counted and charged inside quickjs, JS_FlowParseBytesLife).
 *
 * A census: each span is counted with the bytes it consumed and the thread CPU it took (quantum_thread_us,
 * the pricing clock), beside all the work the clock (event_loop.h's work mover) banked, the host charges
 * retired, and the work discarded, so a rate can be re-read against `stepUs`. CPU prices a kind here and is
 * never an input: the clock, the slice and the aging are functions of the flow's path.
 *
 * The charge. Each kind's bytes move the clock in the opcode unit, at a fixed rate: what one byte of the kind
 * costs this engine's thread, over what one retired opcode costs it, both read off this census at one build
 * (6a224bbe, dev): an opcode loop page retired 66,000,021 units in 14,663,592 us of `stepUs` (0.222 us each),
 * and grafana's census gave script parse 1,415,828 bytes in 296,358 us, decode 1,450,457 in 13,032, HTML
 * 15,069 in 260, CSS 29,841 in 3,156. A rate is a calibration like EVENT_LOOP_WORK_PER_MS, not a measurement
 * the clock takes: the clock stays a function of the bytes a flow's path consumed. */
#ifndef ENGINE_HOST_BROWSER_CORE_TIMING_THREAD_WORK_H
#define ENGINE_HOST_BROWSER_CORE_TIMING_THREAD_WORK_H

/* Named residual. Not covered: a fork's snapshot and C builtins over their input (JSON, codecs, sorts without a
   callback) charge nothing, so the slice and the aging do not see them. Next diff builds: a kind per such
   component, charged by its input at a rate read off this census, as the kinds below are. Absence shows as: a
   step whose `stepUs` is large against its `stepWork` beside the census's per-opcode price. */
#define THREAD_WORK_UNITS_PER_BYTE_SCRIPT 1   /* 0.209 us per byte over 0.222 us per opcode */
#define THREAD_WORK_UNITS_PER_KIB_DECODE 41
#define THREAD_WORK_UNITS_PER_KIB_HTML 78
#define THREAD_WORK_UNITS_PER_KIB_CSS 490
/* A constraint-store probe hashes its whole key (solver/concolic.c's cons_hash): 1.25 ns per byte at -O1 on the
   calibration box, so 6 units per KiB. A key is the predicate's spelling and grows with the path that built
   it, which is why a probe is priced by its bytes and not per call. */
#define THREAD_WORK_UNITS_PER_KIB_CONSTRAINT 6
/* The orphan take walks the runtime's whole object list (quickjs.c's JS_OrphanTakeOne), so it is priced per 1024
   heap entries visited: 112 ns an entry on the live `platform` fixture at load ~35 (scratchpad
   level1w_walkrate.sh: 59876243 entries over 6.72 s of thread CPU), read as about half that unloaded. */
#define THREAD_WORK_UNITS_PER_KI_ORPHAN_ENTRIES 256
/* An Intl host question (core/intl/intl_host.h), priced by its argument and answer bytes: 20,000 time zone offset
   questions took 20,314 us over 380,000 bytes at load ~16 (scratchpad si-tzsite, 1d05ecec, dev), against 0.13 us
   a unit of stepUs over stepWork in the same census, so 415 units per KiB. */
#define THREAD_WORK_UNITS_PER_KIB_INTL 415
/* Layout, per 1024 items of each kind's input: a CSS 2.1 §9.4.1 question answered (core/layout/layout_job.h's
   BF_BOX and BF_WALK memo entries), a line box item taken (a character collected with its advance measured, so
   a glyph run is priced by its characters, a css-sizing-3 §2.1 walk position or a §9.4.2 fill position, with a
   box's §10.8.1 metrics counted as REST_UNIT_LAYOUT_LINE_METRICS' hundred items), and a style rule's selector
   tested against an element by the author origin's match, an element itself counting as
   THREAD_WORK_SELECTOR_TESTS_PER_ELEMENT tests. A relative least-squares fit of each kind's own span CPU over
   its inputs on five fixture documents (dev) gave 47.6 us a question with 0.259 us an item, and 0.885 us an
   element with 0.161 us a test, each document within a factor of 1.7 of the fit; over 0.222 us an opcode
   that is 214, 1.17, 4.0 and 0.72 units. A line item is one unit, which a charge of one item rounds to at any
   rate up to 1024. */
#define THREAD_WORK_UNITS_PER_KI_LAYOUT_BOXES (214 * 1024)
#define THREAD_WORK_UNITS_PER_KI_LINE_ITEMS 1024
#define THREAD_WORK_UNITS_PER_KI_SELECTOR_TESTS 740
#define THREAD_WORK_SELECTOR_TESTS_PER_ELEMENT 6
/* What one box metric derivation is in line items (solver/rest_unit.h's LINE_METRICS: "one costs what a
   hundred of those do"). */
#define THREAD_WORK_LINE_ITEMS_PER_METRIC 100

#include <stddef.h>
#include <stdint.h>

typedef enum {
    THREAD_WORK_SCRIPT_PARSE,   /* a classic compile stint (solver/engine.c's start site) */
    THREAD_WORK_DECODE,         /* Encoding §6.1 decode (core/encoding/encoding.c's encoding_decode) */
    THREAD_WORK_HTML_PARSE,     /* bytes fed to an HTML parser (core/html/html_parse.c, fragment_parser.c) */
    THREAD_WORK_CSS_PARSE,      /* a style sheet's rules parsed from text (core/css/css_style_sheet.c) */
    THREAD_WORK_CONSTRAINT,     /* a path-constraint key hashed for a probe (solver/concolic.c's cons_hash) */
    THREAD_WORK_ORPHAN_WALK,    /* heap entries an orphan take visited (solver/engine.c's engine_orphan_seed) */
    THREAD_WORK_INTL,           /* an Intl host question's bytes (core/intl/intl_host.h) */
    THREAD_WORK_LAYOUT_BOX,     /* a §9.4.1 question body run, charged per question answered (layout_job.c) */
    THREAD_WORK_LAYOUT_LINE,    /* a line box fill or intrinsic run's items taken (layout_job.c's spends) */
    THREAD_WORK_SELECTOR_MATCH, /* style rules tested by one element's author match (css_style_declaration.c) */
    THREAD_WORK_N
} ThreadWorkKind;

/* One span, on the caller's stack. Spans nest (a decode inside an HTML load, a box inside its container's
   body); each counts its own time less its inner spans', so no thread time is counted twice and a kind's CPU is
   the work of its own input. */
typedef struct {
    int64_t t0;
    int depth;
} ThreadWorkSpan;

void thread_work_begin(ThreadWorkSpan *s);
/* Ends the span begun with `s`, counting `bytes` and its own CPU under `kind`. Spans end in reverse order. */
void thread_work_end(ThreadWorkSpan *s, ThreadWorkKind kind, uint64_t bytes);

/* Charge `bytes` of `kind` (heap entries for THREAD_WORK_ORPHAN_WALK) to the running flow's clock at the kind's
   rate per 1024 (rounded down), through the
   interpreter's retired-work count (quickjs.h's JS_FlowRetireWork). Script parse is charged by quickjs's
   tokenizer itself and is not a kind this takes. With no flow running the charge is discarded at the next
   switch-in, as the baseline's work is. */
void thread_work_charge(ThreadWorkKind kind, uint64_t bytes);

/* Retired work the scheduler discarded where it switched a flow in (solver/engine.c's flow_switch_in). */
void thread_work_note_discarded(uint64_t units);

/* The census, copied out. */
typedef struct {
    uint64_t banked_units;      /* every unit the clock banked: opcodes, script parse and the host charges */
    uint64_t charged_units;     /* units thread_work_charge retired, banked or later discarded */
    uint64_t discarded_units, script_parse_bytes_all;
    long spans[THREAD_WORK_N];
    uint64_t bytes[THREAD_WORK_N];
    int64_t cpu_us[THREAD_WORK_N];
    uint64_t charged_input[THREAD_WORK_N];   /* the input thread_work_charge priced, per kind */
} ThreadWorkCensus;
void thread_work_census(ThreadWorkCensus *out);

#endif
