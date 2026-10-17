/* WORK THE THREAD DOES OUTSIDE THE INTERPRETER'S DISPATCH — the spans the event loop's clock (event_loop.h's
 * work mover) does not see because they retire no opcode: decoding bytes, tokenizing HTML, parsing CSS, and
 * parsing script (the last counted inside quickjs, JS_FlowParseBytesLife).
 *
 * A census: each span is counted with the bytes it consumed and the thread CPU it took (quantum_thread_us),
 * beside the opcode work the clock banked and the work it discarded, so the share of a run's thread time the
 * clock does not charge is readable against `stepUs`. CPU is a report here and never a clock input: the clock
 * must be a function of the flow's path.
 *
 * The charge. Each kind's bytes move the clock in the opcode unit, at a fixed rate: what one byte of the kind
 * costs this engine's thread, over what one retired opcode costs it, both read off this census at one build
 * (6a224bbe, dev): an opcode loop page retired 66,000,021 units in 14,663,592 us of `stepUs` (0.222 us each),
 * and grafana's census gave script parse 1,415,828 bytes in 296,358 us, decode 1,450,457 in 13,032, HTML
 * 15,069 in 260, CSS 29,841 in 3,156. A rate is a calibration like EVENT_LOOP_WORK_PER_MS, not a measurement
 * the clock takes: the clock stays a function of the bytes a flow's path consumed. */
#ifndef ENGINE_HOST_BROWSER_CORE_TIMING_THREAD_WORK_H
#define ENGINE_HOST_BROWSER_CORE_TIMING_THREAD_WORK_H

#define THREAD_WORK_UNITS_PER_BYTE_SCRIPT 1   /* 0.209 us per byte over 0.222 us per opcode */
#define THREAD_WORK_UNITS_PER_KIB_DECODE 41
#define THREAD_WORK_UNITS_PER_KIB_HTML 78
#define THREAD_WORK_UNITS_PER_KIB_CSS 490

#include <stddef.h>
#include <stdint.h>

typedef enum {
    THREAD_WORK_SCRIPT_PARSE,   /* a classic compile stint (solver/engine.c's start site) */
    THREAD_WORK_DECODE,         /* Encoding §6.1 decode (core/encoding/encoding.c's encoding_decode) */
    THREAD_WORK_HTML_PARSE,     /* bytes fed to an HTML parser (core/html/html_parse.c, fragment_parser.c) */
    THREAD_WORK_CSS_PARSE,      /* a style sheet's rules parsed from text (core/css/css_style_sheet.c) */
    THREAD_WORK_N
} ThreadWorkKind;

/* One span, on the caller's stack. Spans nest (a decode inside an HTML load); only the outermost is counted, so
   no thread time is counted twice. */
typedef struct {
    int64_t t0;
    int outer;
} ThreadWorkSpan;

void thread_work_begin(ThreadWorkSpan *s);
/* Ends the span begun with `s`, counting `bytes` and its CPU under `kind` when it is the outermost. */
void thread_work_end(ThreadWorkSpan *s, ThreadWorkKind kind, uint64_t bytes);

/* Charge `bytes` of `kind` to the running flow's clock at the kind's rate (rounded down), through the
   interpreter's retired-work count (quickjs.h's JS_FlowRetireWork). Script parse is charged by quickjs's
   tokenizer itself and is not a kind this takes. With no flow running the charge is discarded at the next
   switch-in, as the baseline's work is. */
void thread_work_charge(ThreadWorkKind kind, uint64_t bytes);

/* Retired work the scheduler discarded where it switched a flow in (solver/engine.c's flow_switch_in). */
void thread_work_note_discarded(uint64_t units);

/* The census, copied out. */
typedef struct {
    uint64_t opcode_units, discarded_units, script_parse_bytes_all;
    long spans[THREAD_WORK_N];
    uint64_t bytes[THREAD_WORK_N];
    int64_t cpu_us[THREAD_WORK_N];
} ThreadWorkCensus;
void thread_work_census(ThreadWorkCensus *out);

#endif
