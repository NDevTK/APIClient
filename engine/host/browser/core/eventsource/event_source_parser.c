/* HTML §9.2.5 Parsing an event stream and §9.2.6 Interpreting an event stream. See event_source_parser.h for
 * why the format is its own component and what the sink's three entries are.
 *
 * WHY THE DECODE IS ROUTED AND NOT WRITTEN. §9.2.6's first sentence is "Streams must be decoded using the UTF-8
 * decode algorithm", which is Encoding's operation and already exists here as
 * core/encoding/encoding.h's `encoding_utf8_decode` — the same entry HTML §7.4.2.3.2's `javascript:` URL uses.
 * Writing a second one would be two answers to one question, and the one that drifts is the copy nobody runs
 * against the standard. It matters that it is THAT hook and not the without-BOM one: §9.2.6 states the BOM rule
 * as part of the decode — "The UTF-8 decode algorithm strips one leading UTF-8 Byte Order Mark (BOM), if any" —
 * and §9.2.5's own ABNF says the same thing structurally, its `stream` production being `[ bom ] *event`. Only
 * the FIRST is stripped; a second U+FEFF is an ordinary code point and becomes part of a field name, which is
 * what makes such a line fall through §9.2.6's arms to "the field is ignored".
 *
 * WHY THE PARSE IS OVER THE DECODED BYTES AND WHY THAT IS NOT MERELY CONVENIENT. The decoder answers WELL-FORMED
 * UTF-8 with every malformed sequence already replaced by U+FFFD, and UTF-8 is self-synchronising: no byte of a
 * multi-byte sequence is ever an ASCII byte. Every character §9.2.6 tests for — LF, CR, colon, space, NULL, and
 * the ASCII digits — is ASCII, so scanning the decoded BYTES for them finds exactly the code points the
 * standard means and never a fragment of a character. That is why there is no code-point iterator here, and it
 * is a property of the decoder's output rather than an assumption about the server's input, which is why the
 * one thing asserted about that buffer is that it is a scalar value string.
 *
 * NOTHING HERE ASSERTS ANYTHING ABOUT THE STREAM. The bytes are a stranger's — the server the page addressed
 * chose every one of them — so a stream that is empty, truncated mid-character, all colons, carrying a NULL in
 * a field name, or naming a `retry` of thirty digits is INPUT and takes §9.2.6's own arms: "Ignore the line",
 * "the field is ignored", "any pending data must be discarded". CLAUDE.md's §WHOSE-BYTES-STATE-THE-VALUE is the
 * rule and the failure it names is concrete: an assert over a field value is an abort switch this engine would
 * be handing to every origin a page fetches from. The asserts below are all over values this codebase computed
 * — the caller's sink, the decoder's output, and this file's own re-entrancy.
 *
 * WHY THE BUFFERS ARE PLAIN C. CLAUDE.md requires platform data a flow QUEUES to be a JS value, because it must
 * park to the cold tier and fork per flow. §9.2.6's three buffers are not that: they are created, filled and
 * released inside one call of `event_source_parser_interpret`, they are never reachable from a flow, and the
 * only thing that outlives the call is what the sink COPIES. What the sink does with it — §9.2.6 step 8 queues
 * a task — is queued data and is the caller's to hold as a JS value. That reasoning is exactly as durable as
 * the one-shot entry it rests on, which is the residual at the foot of this file. */
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "core/encoding/encoding.h"
#include "core/eventsource/event_source_parser.h"

/* A GROWING BYTE RUN, AND IT IS NOT core/json_buf.h's. That buffer's append takes a NUL-TERMINATED STRING and
   measures it with strlen, which is right for a JSON writer and wrong for exactly this: HTML §9.2.5's `any-char`
   production is "a scalar value other than U+000A LINE FEED (LF) or U+000D CARRIAGE RETURN (CR)", so a field
   value may contain U+0000, and §9.2.6's `id` arm is written around that possibility — "If the field value does
   not contain U+0000 NULL, then set the last event ID buffer to the field value". Routing these through a
   strlen-shaped append would truncate at precisely the byte the standard names. */
typedef struct { char *b; size_t n, cap; } EsBuf;

static void es_buf_append(EsBuf *b, const char *p, size_t n)
{
    char *nb;
    size_t cap;

    DCHECK(p != NULL || n == 0, "an event stream buffer was appended a null run with a non-zero length");
    if (n == 0)
        return;
    /* A FLOOR AND NOT A BOUND: an event stream has no length limit and this engine caps nothing, so what is
       checked here is the address space, which is the physical floor CLAUDE.md reserves CHECK for. */
    CHECK(n <= (size_t)-1 - b->n, "eventsource: an event stream buffer would exceed the address space");
    if (b->n + n > b->cap) {
        cap = b->cap ? b->cap : 64;
        while (cap < b->n + n) {
            CHECK(cap <= ((size_t)-1) / 2, "eventsource: an event stream buffer would exceed the address space");
            cap *= 2;
        }
        nb = realloc(b->b, cap);
        CHECK(nb != NULL, "eventsource: OOM growing an event stream buffer");
        b->b = nb;
        b->cap = cap;
    }
    memcpy(b->b + b->n, p, n);
    b->n += n;
}

/* THE BYTES, FOR A SINK THAT READS A LENGTH. Never NULL: an empty buffer has allocated nothing, and the sink's
   contract is that the pointer is dereferenceable and the LENGTH is the fact. */
static const char *es_buf_bytes(const EsBuf *b) { return b->b ? b->b : ""; }

/* §9.2.6's THREE BUFFERS — "a data buffer, an event type buffer, and a last event ID buffer must be associated
   with it. They must be initialized to the empty string" — plus the seam they are emptied through.
   `in_sink` is the one invariant this file owns about its own callers: the sink's three entries queue work and
   must not feed this parse, because dispatch the event's step 7 is performed AFTER step 8 below and the two
   orders are indistinguishable only while nothing re-enters. */
typedef struct {
    EsBuf data, type, last_id;
    const EventSourceParserSink *sink;
    void *user;
    bool  in_sink;
} EsParse;

/* §9.2.6's DISPATCH THE EVENT, for web browsers — the eight steps, of which 4, 5 and part of 6 belong to the
   caller (see the header for why).
   THE RESET IS WRITTEN AFTER THE QUEUE AND THE STANDARD WRITES IT BEFORE. Step 7 is "Set the data buffer and the
   event type buffer to the empty string" and step 8 queues the task; these buffers are this component's private
   state, no sink can observe them and the re-entrancy assert is what says so, so the two orders differ in
   nothing an implementation can distinguish — and this one is what lets step 8 carry the bytes the buffers hold
   rather than a copy taken to survive an early reset. */
static void es_dispatch_event(EsParse *s)
{
    const char *type;
    size_t type_n;

    /* Step 1. It runs before step 2's return, which is why a blank-data event still moves the event source's
       last event ID string: `id: 7` followed by a blank line fires nothing and sets the field. */
    DCHECK(!s->in_sink, "an event stream sink re-entered the parse that called it — §9.2.6's dispatch the event "
                        "queues a task, and a sink that instead fed more of the stream would observe the data "
                        "and event type buffers between steps 7 and 8, which this file's order relies on being "
                        "unobservable");
    s->in_sink = true;
    s->sink->set_last_event_id(s->user, es_buf_bytes(&s->last_id), s->last_id.n);
    s->in_sink = false;
    /* Step 2. "If the data buffer is an empty string, set the data buffer and the event type buffer to the
       empty string and return." The data buffer is already empty; the event type buffer need not be, and a
       stream of `event: ping` followed by a blank line is exactly the case that reaches this. */
    if (s->data.n == 0) {
        s->type.n = 0;
        return;
    }
    /* Step 3. "If the data buffer's last character is a U+000A LINE FEED (LF) character, then remove the last
       character from the data buffer." It is one character and not a run: `data:` twice leaves "\n\n", and the
       standard's own example says that block "fires an event with the data set to a single newline character". */
    if (s->data.b[s->data.n - 1] == '\n')
        s->data.n--;
    /* Steps 5 and 6 are one decision. Step 5 initializes the type to "message" and step 6 says "If the event
       type buffer has a value other than the empty string, change the type of the newly created event to equal
       the value of the event type buffer", so the answer is the buffer where it has one. */
    if (s->type.n) {
        type = s->type.b;
        type_n = s->type.n;
    } else {
        type = "message";
        type_n = sizeof "message" - 1;
    }
    /* Step 8, then step 7 — see the block comment above. */
    DCHECK(!s->in_sink, "an event stream sink re-entered the parse that called it");
    s->in_sink = true;
    s->sink->dispatch_event(s->user, type, type_n, s->data.b, s->data.n);
    s->in_sink = false;
    s->data.n = 0;
    s->type.n = 0;
}

/* INFRA'S ASCII DIGIT, over the run §9.2.6's `retry` arm tests: "If the field value consists of only ASCII
   digits". THE EMPTY VALUE ANSWERS FALSE, and that is a reading rather than an oversight: a bare `retry:` line
   is vacuously all-digits and the arm's next clause is "interpret the field value as an integer in base ten",
   which no empty string denotes — so the value does not name an integer and the arm's own alternative,
   "Otherwise, ignore the field", is the only answer that leaves the reconnection time a number. */
static bool es_all_ascii_digits(const char *p, size_t n)
{
    size_t i;

    if (n == 0)
        return false;
    for (i = 0; i < n; i++)
        if (p[i] < '0' || p[i] > '9')
            return false;
    return true;
}

/* "interpret the field value as an integer in base ten".
   A DIGIT RUN LONGER THAN THIS MACHINE CAN HOLD SATURATES, AND IT IS NOT A CLAMP PAST A BROKEN INVARIANT —
   the value is a stranger's and is legitimate input, so there is nothing here to assert and nothing to refuse.
   Of the two defined answers available, saturating is the one the standard's own sentence chooses: "ignore the
   field" is the arm §9.2.6 reserves for a value that is NOT all digits, and taking it here would leave the
   reconnection time at its previous value for a stream that plainly asked for a longer one. UINT64_MAX
   milliseconds is 5.8e8 years, so what the two answers actually differ about is whether a reconnection that
   will not happen in any session happens at the old delay instead — and the standard says set it. */
static uint64_t es_base_ten(const char *p, size_t n)
{
    uint64_t v = 0;
    size_t i;
    unsigned d;

    for (i = 0; i < n; i++) {
        d = (unsigned)(p[i] - '0');
        if (v > (UINT64_MAX - d) / 10)
            return UINT64_MAX;
        v = v * 10 + d;
    }
    return v;
}

/* §9.2.6's "steps to process the field given a field name and a field value" — the five arms, in the
   standard's order. "Field names must be compared literally, with no case folding performed", which is what
   makes every comparison here a length plus a memcmp and never a case-insensitive one. */
#define ES_NAME_IS(lit) (name_n == sizeof (lit) - 1 && memcmp(name, (lit), sizeof (lit) - 1) == 0)
static void es_process_field(EsParse *s, const char *name, size_t name_n,
                             const char *value, size_t value_n)
{
    if (ES_NAME_IS("event")) {
        /* "Set the event type buffer to the field value." A SET and not an append, so the buffer is emptied
           first — two `event:` lines in one block leave the second's value. */
        s->type.n = 0;
        es_buf_append(&s->type, value, value_n);
    } else if (ES_NAME_IS("data")) {
        /* "Append the field value to the data buffer, then append a single U+000A LINE FEED (LF) character to
           the data buffer." The LF is unconditional, which is why dispatch the event's step 3 exists. */
        es_buf_append(&s->data, value, value_n);
        es_buf_append(&s->data, "\n", 1);
    } else if (ES_NAME_IS("id")) {
        /* "If the field value does not contain U+0000 NULL, then set the last event ID buffer to the field
           value. Otherwise, ignore the field." memchr and not strlen: the NULL this tests for is a byte in the
           middle of a run, which is the whole reason the buffers here are length-carrying. */
        if (memchr(value, '\0', value_n) == NULL) {
            s->last_id.n = 0;
            es_buf_append(&s->last_id, value, value_n);
        }
    } else if (ES_NAME_IS("retry")) {
        if (es_all_ascii_digits(value, value_n)) {
            DCHECK(!s->in_sink, "an event stream sink re-entered the parse that called it");
            s->in_sink = true;
            s->sink->set_reconnection_time(s->user, es_base_ten(value, value_n));
            s->in_sink = false;
        }
    }
    /* "Otherwise — The field is ignored." Including the empty field name a bare `:`-less blank-ish line
       cannot produce (§9.2.6's first arm takes the empty line) and the one a line of `:` alone cannot produce
       either (its second arm takes a leading colon), so what reaches here is a name the server chose. */
}
#undef ES_NAME_IS

/* §9.2.6's "Lines must be processed, in the order they are received, as follows" — the four arms, in order.
   THE ORDER OF THE FIRST TWO IS LOAD-BEARING: a line of `:` alone contains a colon, so the leading-colon arm
   must be asked before the contains-a-colon one or every comment would become a field with an empty name. */
static void es_process_line(EsParse *s, const char *line, size_t line_n)
{
    const char *colon;
    const char *value;
    size_t value_n;

    if (line_n == 0) {
        /* "If the line is empty (a blank line) — Dispatch the event, as defined below." */
        es_dispatch_event(s);
        return;
    }
    if (line[0] == ':')
        /* "If the line starts with a U+003A COLON character (:) — Ignore the line." */
        return;
    colon = memchr(line, ':', line_n);
    if (colon != NULL) {
        /* "Collect the characters on the line before the first U+003A COLON character (:), and let field be
           that string. Collect the characters on the line after the first U+003A COLON character (:), and let
           value be that string. If value starts with a U+0020 SPACE character, remove it from value." ONE
           space: the standard's own example says two `data` lines whose values differ by that one space "fires
           two identical events". */
        value = colon + 1;
        value_n = line_n - (size_t)(colon - line) - 1;
        if (value_n > 0 && value[0] == ' ') {
            value++;
            value_n--;
        }
        es_process_field(s, line, (size_t)(colon - line), value, value_n);
        return;
    }
    /* "Otherwise, the string is not empty but does not contain a U+003A COLON character (:) — Process the field
       using the steps described below, using the whole line as the field name, and the empty string as the
       field value." */
    es_process_field(s, line, line_n, "", 0);
}

void event_source_parser_interpret(const char *bytes, size_t n,
                                   const EventSourceParserSink *sink, void *user)
{
    EsParse s;
    char *decoded;
    size_t dn = 0, i, line_start;

    DCHECK(bytes != NULL || n == 0,
           "§9.2.6 was handed a null event stream body with a non-zero length");
    DCHECK(sink != NULL && sink->set_last_event_id != NULL && sink->dispatch_event != NULL &&
           sink->set_reconnection_time != NULL,
           "an event stream sink is missing one of §9.2.6's three steps that need the EventSource object — all "
           "three are reachable from an ordinary stream and a sink that supplies only dispatch_event loses the "
           "last event ID of every block whose data buffer is empty, silently");

    /* §9.2.6 step 1, routed — see the banner. The hook never answers NULL (its own contract is a malloc'd,
       NUL-terminated, well-formed sequence and it CHECKs its allocation), so a NULL here would be that contract
       having changed under this file. */
    decoded = encoding_utf8_decode(bytes, n, &dn);
    DCHECK(decoded != NULL, "Encoding's UTF-8 decode answered nothing for an event stream body");
    DCHECK(encoding_is_scalar_value_string(decoded, dn),
           "Encoding's UTF-8 decode answered a sequence that is not a scalar value string — this parse scans "
           "the decoded BYTES for LF, CR, colon, space, NULL and the ASCII digits and is only equivalent to "
           "scanning §9.2.6's code points while that sequence is well-formed UTF-8");

    memset(&s, 0, sizeof s);
    s.sink = sink;
    s.user = user;

    /* §9.2.6's line split: "a U+000D CARRIAGE RETURN U+000A LINE FEED (CRLF) character pair, a single U+000A
       LINE FEED (LF) character not preceded by a U+000D CARRIAGE RETURN (CR) character, and a single U+000D
       CARRIAGE RETURN (CR) character not followed by a U+000A LINE FEED (LF) character being the ways in which
       a line can end". Taking CR first and swallowing an LF behind it answers all three in one pass, and the
       swallow is what stops a CRLF ending two lines. */
    for (i = 0, line_start = 0; i < dn; ) {
        if (decoded[i] == '\r') {
            es_process_line(&s, decoded + line_start, i - line_start);
            i++;
            if (i < dn && decoded[i] == '\n')
                i++;
            line_start = i;
        } else if (decoded[i] == '\n') {
            es_process_line(&s, decoded + line_start, i - line_start);
            i++;
            line_start = i;
        } else {
            i++;
        }
    }
    /* "Once the end of the file is reached, any pending data must be discarded. If the file ends in the middle
       of an event (before the final empty line), the incomplete event is not dispatched." So the run from
       `line_start` to the end is NOT a line — nothing terminated it — and the three buffers are dropped rather
       than dispatched. The standard's own example says so of a whole block: the last one "is discarded because
       it is not followed by a blank line". */
    DCHECK(line_start <= dn, "the event stream line split walked past the end of the decoded body");
    free(s.data.b);
    free(s.type.b);
    free(s.last_id.b);
    free(decoded);
}

/* NAMED RESIDUAL — INCREMENTAL DELIVERY.
 *
 * WHAT IS NOT COVERED: this entry interprets a COMPLETE body, so no event of a stream is dispatched before the
 * whole reply has arrived. A server that holds the connection open — which is what an event stream is for, and
 * what HTML §9.2.5 means by "connections established to remote servers for such resources are expected to be
 * long-lived" — delivers nothing at all through this path, because the reply never completes.
 *
 * WHAT THE NEXT DIFF BUILDS: an incremental body delivery across the one chokepoint SECURITY.md names, and a
 * feed entry beside this one that keeps §9.2.6's three buffers plus a pending-line run across calls. Its state
 * then CROSSES A SUSPEND POINT, so by CLAUDE.md's §PLATFORM-DATA-A-FLOW-QUEUES-IS-A-JS-VALUE those four
 * buffers become JS values on the EventSource object's own record rather than the plain C they correctly are
 * here, and the decode becomes the STREAMING one — core/encoding/encoding.h's `enc_decoder_decode` with
 * `stream` true — because a character split across two chunks decodes to two U+FFFDs otherwise.
 *
 * AND THE TRUSTED HALF OF THAT DELIVERY IS BUILT, WHICH THIS CLAUSE SAID IT WAS NOT. The words that stood here
 * were `extension/lib/safe-fetch.js reads a body with one await resp.arrayBuffer() and has no chunk seam`, and
 * that is the one failure mode a next-diff clause has: it is a claim about a tree that moves, read once, by
 * somebody who has already decided to do the work — so a wrong one is not caught, it is executed. RE-DERIVED
 * rather than repeated: `_readBody` takes the response through `resp.body.getReader()` and RELEASES each chunk
 * to an `opts.onChunk` sink as it arrives, with no chunk cap, no size cap and no timeout, sitting BELOW every
 * gate that reads the response head and ABOVE the one that reads the body. An event stream is on the
 * streamable side of that boundary and does not have to argue for it: HTML §9.2.2's step 8 creates its request
 * "given urlRecord, the empty string, and corsAttributeState", and the empty destination is not one
 * `_isScriptLike` enumerates, so `_bodyGated` is false and the entry's `blocked-stream-body-gated:` refusal
 * cannot reach it. That sink has NO CALLER anywhere in this tree, which is what it was waiting for.
 *
 * SO THE HALF THAT IS ACTUALLY MISSING IS THE ENGINE'S, AND IT IS TWO THINGS RATHER THAN ONE. First, a
 * NON-SETTLING DELIVERY: `engine/host/qjs_abi.h` declares `qjs_provide` and `qjs_decline` and nothing else
 * that carries bytes for a park, and `engine/host/solver/pending.h`'s field list holds one `body` beside one
 * `haveValue`, so the only thing the zone can say about a request is its whole answer, once — there is no
 * state in which a park has received bytes and is still owed more. Second, a HEAD DELIVERED BEFORE ITS BODY:
 * `_readBody`'s own banner states that consequence, that a sink is handed bytes while its caller still holds
 * no head and that the computed type is the sniff's, so a head cannot carry one — "enough for a consumer that
 * parses a byte stream, and not enough for one that must announce a connection before it may dispatch".
 * HTML §9.2.2's step 15 is exactly that consumer: its third arm refuses a response whose "status is not 200" or
 * whose "`Content-Type` is not `text/event-stream`", and its fourth announces the connection, and BOTH run
 * before a byte of this file's input is read.
 *
 * HOW ITS ABSENCE WOULD SHOW: a page whose `EventSource` addresses a real streaming endpoint fires no `message`
 * event ever, while its flow stays parked on a reply that never completes — the flow is outranked and paged,
 * which is correct scheduling and is indistinguishable, from outside, from a server that said nothing. Against
 * a FINITE `text/event-stream` reply — which is what a fixture and most probes serve — this path is exact, and
 * that is the difference to watch: events for a body that ends, silence for one that does not. */
