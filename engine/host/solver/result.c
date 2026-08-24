#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "solver/result.h"
#include "solver/endpoint.h"
#include "solver/solve.h"
#include "solver/engine.h"
#include "solver/flow.h"
#include "solver/world.h"   /* what the cross-instance seam materialized here — see world_segment_stats */
#include "solver/concolic.h"   /* whether this run ever acquired attacker input — see concolic_source_reads */
#include "solver/cold.h"    /* …and what it PARKED, if the host asked this engine to page out */

/* THE PAGE'S OWN UNCAUGHT ERRORS, deduped. See result.h: a script that throws is the forcing function naming an
   unbuilt capability, and it was silent. This is a plain string set — the message is the page's own, so nothing
   here interprets it. */
static char **g_errs; static int g_errs_n, g_errs_cap;

/* WHO PRINTS ONE AS IT HAPPENS — see result.h. NULL for a host whose output is a document it writes at the
   end; set by a host whose output is a stream. */
static void (*g_err_hook)(const char *msg);
void result_set_page_error_hook(void (*fn)(const char *msg)) { g_err_hook = fn; }

void result_page_error(const char *msg) {
    if (!msg || !*msg) return;
    for (int i = 0; i < g_errs_n; i++) if (!strcmp(g_errs[i], msg)) return;
    if (g_err_hook) g_err_hook(msg);
    if (g_errs_n >= g_errs_cap) {
        int c = g_errs_cap ? g_errs_cap * 2 : 8;
        char **a = realloc(g_errs, (size_t)c * sizeof(char *));
        if (!a) return;   /* a lost diagnostic is not worth failing a run over */
        g_errs = a; g_errs_cap = c;
    }
    g_errs[g_errs_n] = strdup(msg);
    if (g_errs[g_errs_n]) g_errs_n++;
}

/* Describe a thrown value WITHOUT running any of the page's code — see result.h. An own slot that is already a
   string is taken as-is; anything else is described by shape alone. */
void result_error_text(JSContext *ctx, JSValueConst err, char *out, size_t outsz) {
    char *buf = out;
    JSValue name = JS_UNDEFINED, msg = JS_UNDEFINED, stk = JS_UNDEFINED;
    const char *ns = NULL, *ms = NULL, *ss = NULL;
    JSAtom a_name, a_msg;
    int n;

    DCHECK(out != NULL && outsz >= 64,
           "a thrown value was described into no buffer, or into one too small to hold a name and a message — "
           "the description is truncated at the caller's size and every caller must give it room to be one");
    *out = 0;
    if (JS_IsString(err)) {
        const char *s = JS_ToCString(ctx, err);   /* already a string: no coercion runs */
        if (s) { snprintf(out, outsz, "%s", s); JS_FreeCString(ctx, s); }
        return;
    }
    if (!JS_IsObject(err)) { snprintf(out, outsz, "a non-object, non-string value was thrown"); return; }

    a_name = JS_NewAtom(ctx, "name");
    a_msg  = JS_NewAtom(ctx, "message");
    if (JS_GetOwnSlot(ctx, &name, err, a_name) <= 0) name = JS_UNDEFINED;
    if (JS_GetOwnSlot(ctx, &msg,  err, a_msg)  <= 0) msg  = JS_UNDEFINED;
    JS_FreeAtom(ctx, a_name);
    JS_FreeAtom(ctx, a_msg);
    /* A DOMException keeps BOTH behind accessors on its prototype, so the own-property read above finds nothing
       and the report degenerates to "an object with no own name/message" — for the single most common throw in
       a DOM engine. That cost a whole debugging cycle: an aborted sibling flow reported as an anonymous object
       when it was a NotSupportedError naming exactly what was wrong. Read the slots instead. */
    if (!JS_IsString(name)) { JS_FreeValue(ctx, name); name = JS_GetDOMExceptionName(ctx, err); }
    if (!JS_IsString(msg))  { JS_FreeValue(ctx, msg);  msg  = JS_GetDOMExceptionMessage(ctx, err); }
    /* WHERE it threw. A genuine Error keeps its stack in the [[ErrorData]] internal slot behind an accessor on
       Error.prototype, so JS_GetOwnSlot cannot see it and calling the getter would run page code (a page may
       have replaced Error.prepareStackTrace, and this runs from OUTSIDE any flow). JS_GetErrorStackString reads
       the slot directly, which is exactly the "a stored value, never an operation" rule the rest of this
       function follows. Without it a message like "not a function" names a capability and nothing else — the
       whole diagnostic is WHERE, and finding it by hand meant re-serving the library wrapped in a try/catch. */
    stk = JS_GetErrorStackString(ctx, err);
    if (JS_IsString(name)) ns = JS_ToCString(ctx, name);
    if (JS_IsString(msg))  ms = JS_ToCString(ctx, msg);
    if (JS_IsString(stk))  ss = JS_ToCString(ctx, stk);
    if (ns && ms)      n = snprintf(buf, outsz, "%s: %s", ns, ms);
    else if (ms)       n = snprintf(buf, outsz, "%s", ms);
    else if (ns)       n = snprintf(buf, outsz, "%s", ns);
    else               n = snprintf(buf, outsz, "an object with no own name/message was thrown");
    if (ss && n > 0 && (size_t)n < outsz) {
        /* the first two frames, on one line — the site and its caller, which is what identifies the call. */
        const char *l1 = ss + strspn(ss, " \t\n"), *l1e = l1 + strcspn(l1, "\n");
        const char *l2 = *l1e ? l1e + 1 : l1e, *l2e;
        l2 += strspn(l2, " \t");
        l2e = l2 + strcspn(l2, "\n");
        snprintf(buf + n, outsz - (size_t)n, "  [%.*s%s%.*s]",
                 (int)(l1e - l1), l1, (l2e > l2 ? " <- " : ""), (int)(l2e - l2), l2);
    }
    if (ns) JS_FreeCString(ctx, ns);
    if (ms) JS_FreeCString(ctx, ms);
    if (ss) JS_FreeCString(ctx, ss);
    JS_FreeValue(ctx, name);
    JS_FreeValue(ctx, msg);
    JS_FreeValue(ctx, stk);
}

void result_page_error_value(JSContext *ctx, JSValueConst err) {
    char buf[320];
    result_error_text(ctx, err, buf, sizeof buf);
    result_page_error(buf);   /* an empty description is dropped by result_page_error's own first line */
}

/* Append RAW (a delimiter this file controls) or ESCAPED (page-supplied text). Escaping the delimiters too was
   a bug: the quotes around each message came out as \" inside the JSON string. */
static void errs_raw(char **buf, size_t *cap, size_t *len, const char *s) {
    size_t k = strlen(s);
    if (*len + k + 1 >= *cap) {
        size_t nc = *cap ? *cap : 256;
        while (*len + k + 1 >= nc) nc *= 2;
        char *nb = realloc(*buf, nc);
        if (!nb) return;
        *buf = nb; *cap = nc;
    }
    memcpy(*buf + *len, s, k); *len += k; (*buf)[*len] = 0;
}

/* JSON-escape a page-supplied string (its own message text, so it can hold anything). */
static void errs_append(char **buf, size_t *cap, size_t *len, const char *s) {
    for (const char *p = s; *p; p++) {
        char esc[8]; int k;
        if (*p == '"' || *p == '\\') { esc[0] = '\\'; esc[1] = *p; k = 2; }
        else if ((unsigned char)*p < 0x20) { k = snprintf(esc, sizeof esc, "\\u%04x", (unsigned char)*p); }
        else { esc[0] = *p; k = 1; }
        if (*len + (size_t)k + 1 >= *cap) {
            size_t nc = *cap ? *cap * 2 : 256;
            while (*len + (size_t)k + 1 >= nc) nc *= 2;
            char *nb = realloc(*buf, nc);
            if (!nb) return;
            *buf = nb; *cap = nc;
        }
        memcpy(*buf + *len, esc, (size_t)k); *len += (size_t)k; (*buf)[*len] = 0;
    }
}

static char *errs_json_array(void) {
    char *b = NULL; size_t cap = 0, len = 0;
    errs_raw(&b, &cap, &len, "[");
    for (int i = 0; i < g_errs_n; i++) {
        if (i) errs_raw(&b, &cap, &len, ",");
        errs_raw(&b, &cap, &len, "\"");
        errs_append(&b, &cap, &len, g_errs[i]);
        errs_raw(&b, &cap, &len, "\"");
    }
    errs_raw(&b, &cap, &len, "]");
    return b ? b : strdup("[]");
}

/* The composition, and nothing else. Each surface serializes itself — endpoint.c walks its deduped endpoints,
   solve.c its fire-verified sinks — and this only decides that they are ONE document and what it is called.
   Keeping that decision in one place is the point: a second caller that wanted "just the endpoints" is how a
   host ends up assembling structure again. */
char *result_json(JSContext *ctx) {
    char *eps = endpoint_json_array();
    char *sinks = solve_json_array(ctx);
    char *errs = errs_json_array();
    /* NO `probeResults` SURFACE. It carried the schemas an API's own REJECTION described, and a rejection is
       the answer to a DELIBERATELY MALFORMED REQUEST — one this engine cannot make, since its only network
       edge is the pending register and the host performs a GET through safeFetch. The reader on this side was
       filing whatever rejection a GET happened to provoke under the identity of an endpoint nobody probed.
       It is extension/lib/req2proto.js, which issues the probe as the page and writes straight into
       `globalStore.probeResults`; nothing about it crosses this seam. */
    size_t n;
    char *out;

    if (!eps || !sinks || !errs) { free(eps); free(sinks); free(errs); return NULL; }
    /* THE SLACK COVERS THE WIDEST FORM, not the numbers that happen to occur. Counted rather than estimated,
       and stated so the count can be re-done: the format's fixed bytes are 467 with its conversion specifiers
       and 407 without them, and the nineteen counters' full-width decimals are 335 (five ints at 11, fourteen
       longs at 20), so the worst case is 742 against this 768. It was 192 for a shape whose widest form was
       already 197 — inside only because the real numbers are small — then 384 against a worst case the arrival
       census took to 454, then 512 against 488, then 640 against the routed-delivery pair's 566; the four ends
       of §9.3.3's delivery task are the FIFTH field to outgrow it, and the count above is it re-done rather
       than adjusted. RE-DO THE ARITHMETIC WHEN YOU ADD A FIELD; it is four counts off the format string and
       there is no way to be nearly right. The DCHECK under the snprintf is
       the second half of this, not a substitute for it: the arithmetic is what makes the buffer right, the
       assert is what catches the arithmetic being re-done wrong. */
    /* THE PARK DOCUMENT RIDES THE RESULT, because it IS a result: it is what this engine has left to say about
       a page it did not finish, and the host already does one JSON.parse of one document. "[]" — the ordinary
       case — tells the host this engine drained rather than paged out, which is what DELETES the origin's cold
       entry instead of leaving a stale residue that would be resumed forever. */
    n = strlen(eps) + strlen(sinks) + strlen(errs) + strlen(cold_park_json()) + 768;
    out = malloc(n);
    if (out) {
        /* THE THREE COST NUMBERS, together. A switch count on its own cannot say whether a run that took six
           times as long grew its frontier or grew the work inside each flow, and those need opposite fixes.
           AND WHAT THE CROSS-INSTANCE SEAM DID. A delivery arriving says nothing about whether the ancestry it
           carried was ever used, and a mechanism nobody can see run is one that has never run. */
        /* HELD AND MADE ARE TWO NUMBERS AND THIS EMITTED ONE OF THEM UNDER THE OTHER'S NAME. `_worldSegments`
           carried `world_segment_stats`'s materialized count — a CUMULATIVE history that only
           world_segment_counts_reset lowers — while every reader's prose described the LIVE table (route.mjs:
           "how many foreign worlds hold a segment here"). They agree exactly until world_release runs, which is
           the one event the number exists to make visible, so the field was at its most wrong precisely when it
           mattered — and world_release now has a caller on every sender's flow death, so the two diverge in
           every run with a peer in it rather than in none. cold.c's park hook had already worked this out and
           prints both, in the words this comment owes it: "held alone cannot say: with made beside it, held=0
           is impossible to reach, held=4/made=4 is a live peer, and a held that is far below made is a seam
           that materialized and released". So both cross the seam, each named for the number it is, and
           `world_segments_held`'s own DCHECK (a table larger than its history was grown by something that is
           not world.c) rides along with them. */
        int held = world_segments_held(), made = 0, segf = 0;
        int m;
        /* AND WHY THE SECURITY ARRAY IS THE LENGTH IT IS, WHICH AN EMPTY ONE CANNOT SAY. `securitySinks: []`
           has four readings that take opposite actions — no attacker source was ever read, none reached a
           sink, sinks ran and only the page's own strings arrived, or taint arrived and the search was
           declined because the check on it was unforgeable — and the last of those is the engine's STRONGEST
           negative result rendered as the same nothing as never having looked. These four numbers are the
           split; solver/solve.h and solver/concolic.h state which is which. They ride the result document
           rather than a log for the reason every other count here does: the renderer does not tee its stdout,
           so a number a console scrape would have to find is a number nobody reads. */
        long srcReads = concolic_source_reads(), sinkReached = 0, sinkTainted = 0, sinkSuppressed = 0;
        /* AND WHAT THIS INSTANCE DID WITH THE RECORDS A PEER SENT IT — see engine.h for why the pair travels
           together and why a host's own routed count is not comparable to a page's handler invocations. It
           rides the result document for the reason the four above it do: a zone reading this from a log would
           be reading a stream the renderer deliberately does not tee. */
        long routedDelivered = 0, routedRefused = 0;
        /* AND WHAT BECAME OF THE TASKS THOSE DELIVERIES QUEUED. `_routedDelivered` alone is the shape §@S
           forbids in a search and forbids here for the same reason: a page whose listener ran fewer times than
           the engine delivered has ONE number covering "the spec declined it" (§9.3.3 step 8.1), "there was no
           Document left to fire at" (§7.5.10 step 7) and "the scheduler lost the task", and only the last is a
           defect. All four ride the document rather than a log, for the reason the counts above them do. */
        long routedEnds[ROUTED_TASK_END_N];
        world_segment_stats(&made, &segf);
        solve_arrival_census(&sinkReached, &sinkTainted, &sinkSuppressed);
        engine_routed_census(&routedDelivered, &routedRefused);
        engine_routed_task_census(routedEnds);
        m = snprintf(out, n, "{\"fetchCallSites\":%s,\"securitySinks\":%s,\"pageErrors\":%s,"
                             "\"_switches\":%d,\"_flows\":%ld,\"_candidates\":%d,"
                             "\"_jobsQueued\":%ld,\"_jobsRun\":%ld,\"_unitsDone\":%ld,"
                             "\"_worldSegmentsHeld\":%d,\"_worldSegmentsMade\":%d,"
                             "\"_worldSegmentsForked\":%d,"
                             "\"_routedDelivered\":%ld,\"_routedRefused\":%ld,"
                             "\"_routedTasksFired\":%ld,\"_routedTasksTargetOrigin\":%ld,"
                             "\"_routedTasksTargetGone\":%ld,\"_routedTasksThrew\":%ld,"
                             "\"_sourceReads\":%ld,\"_sinkReached\":%ld,\"_sinkTainted\":%ld,"
                             "\"_sinkSuppressed\":%ld,\"_park\":%s}",
                     eps, sinks, errs, engine_switch_count(), flow_created_count(), solve_candidate_count(),
                     engine_jobs_queued(), engine_jobs_run(), engine_units_done(), held, made, segf,
                     routedDelivered, routedRefused,
                     routedEnds[ROUTED_TASK_FIRED], routedEnds[ROUTED_TASK_TARGET_ORIGIN],
                     routedEnds[ROUTED_TASK_TARGET_GONE], routedEnds[ROUTED_TASK_THREW],
                     srcReads, sinkReached, sinkTainted, sinkSuppressed, cold_park_json());
        /* THE SLACK IS ASSERTED RATHER THAN EYEBALLED. It was 192 bytes for three counters and is now carrying
           nineteen, whose widest form is 335 digits beside 407 bytes of literal — inside the slack only because
           the real numbers are small. A truncation here does not lose a digit, it loses the closing brace: the host
           gets a document that will not parse and reports NOTHING for the page, which is the loudest possible
           consequence arriving as the quietest possible bug. snprintf already told us; nothing was asking. */
        DCHECK(m > 0 && (size_t)m < n, "the result document did not fit its buffer — the host would be handed "
                                       "truncated JSON and every finding for this page would be discarded");
    }
    free(eps);
    free(sinks);
    free(errs);
    return out;
}
