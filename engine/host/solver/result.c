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
#include "solver/dom_cow.h"   /* the DOM half of the swap census — see result_swap_json */
#include "solver/decide.h"    /* …and which predicate grew the frontier — see decide_fork_json */
/* …and what the ORDER above them was denominated in, which is the fact that decides whether two of these
   documents may be compared at all. Composed by the component that owns it — see result.h and quantum.h. */
#include "solver/quantum.h"
/* THE REALM COUNT IS THE ONE ROW OF THE HEAP CENSUS THAT quickjs CANNOT ANSWER, and it is a BROWSER fact: a
   child realm is built per flow that creates a navigable with an address, so the component that holds the list
   is the one that answers. §A-CAPABILITY-MATERIALIZED-PER-FLOW makes it a ceiling, and navigable.c's own OOM
   CHECK sends its reader to this number by name. */
#include "core/frame/navigable.h"
/* §8.1.4.6 "Runtime script errors"'s throw site — one component derives it, this one reports it. */
#include "core/events/report_exception.h"

/* THE PAGE'S OWN UNCAUGHT ERRORS, deduped. See result.h: a script that throws is the forcing function naming an
   unbuilt capability, and it was silent. The message is the page's own, so nothing here interprets it.
   EACH ENTRY IS A PAIR — the message and §8.1.4.6 "Runtime script errors"'s throw site — and the DEDUPE KEY IS
   THE PAIR. Keyed on the message alone, two different scripts raising one error are one entry, which is the
   one collapse a reader cannot afford: a document that stages an uncaught error on purpose and a regression
   raising the same message somewhere else become a single indistinguishable line. `at` is "" for a thrown
   value with no backtrace, which is §8.1.4.6's own answer and not an absent field.
   AND THE ROW COUNTS ITS OCCURRENCES, which is what makes a report revocable — see result.h. `standing` is
   how many occurrences of this pair have been reported and NOT taken back; `retracted` is how many were.
   Their SUM is how many times the pair was reported, and neither is derivable from the other: a row at
   standing 0 with retracted 3 is a pair this engine named three times and withdrew three times, which is a
   different fact from a pair that was never recorded (no row at all) and must not read like it anywhere.
   ONE ARRAY OF ROWS AND NOT FOUR PARALLEL COLUMNS. The message and the throw site were two `char **` grown by
   two reallocs, and the comment on that growth had to say out loud that a half-grown pair misfiles a row the
   dedupe then reads across. Two more columns would be two more chances at exactly that; one row is one
   allocation and the failure has nowhere to be partial. */
typedef struct {
    char *msg;
    char *at;
    int   standing;
    int   retracted;
} PageErrorRow;
static PageErrorRow *g_errs; static int g_errs_n, g_errs_cap;

/* The row for this pair, or NULL. The ONE place the (message, throw site) key is spelled, so the report and
   the retraction cannot come to disagree about what identifies a row. */
static PageErrorRow *errs_find(const char *msg, const char *filename) {
    for (int i = 0; i < g_errs_n; i++)
        if (!strcmp(g_errs[i].msg, msg) && !strcmp(g_errs[i].at, filename)) return &g_errs[i];
    return NULL;
}

/* WHO PRINTS ONE AS IT HAPPENS, AND THE FACT THAT A HOST ANSWERED THE QUESTION AT ALL — see result.h. The
   second is not bookkeeping for the first: a NULL hook USED to mean "this host publishes the document", so a
   host that had considered where an uncaught page error is read and a host that never had made the identical
   call, and the one that never had was the fixture whose whole job is naming unbuilt capabilities. */
static void (*g_err_hook)(const char *msg, const char *filename, ResultPageErrorEdge edge);
static int g_err_route_declared;
void result_set_page_error_hook(void (*fn)(const char *msg, const char *filename, ResultPageErrorEdge edge)) {
    DCHECK(fn != NULL,
           "a host declared a page-error STREAM and handed it no printer. Clearing the hook is not how a host "
           "says it publishes the document — result_page_errors_ride_the_document is — so this would restore "
           "the silent default that declaration exists to end");
    g_err_hook = fn;
    g_err_route_declared = 1;
}
void result_page_errors_ride_the_document(void) { g_err_route_declared = 1; }

void result_page_error(const char *msg, const char *filename) {
    if (!msg || !*msg) return;
    /* NEVER NULL, AND "" IS THE ANSWER RATHER THAN THE ABSENCE OF ONE — result.h states why. A NULL here would
       be a caller that never asked §8.1.4.6 where the throw was, which reads downstream exactly like a value
       that carried no backtrace, and those are different facts about different runs. */
    DCHECK(filename != NULL,
           "a page error was recorded with no throw-site field at all — §8.1.4.6's `filename` is \"\" for a "
           "thrown value carrying no backtrace and that is a positive answer, so a null one is a caller that "
           "did not ask rather than a value that had nothing to say");
    /* AT THE ORIGIN — the FIRST uncaught page error, which is the last moment at which this host's silence is
       still recoverable. A page's throw is the forcing function that names an unbuilt capability, so a host
       that reaches one having declared neither route is a host in which that name cannot be read, and the
       failure is silent in the one direction nobody checks: the run continues and reports the surface it
       happened to reach. The message is recorded either way — what is undeclared is whether anything ever
       says so. */
    DCHECK(g_err_route_declared,
           "the page threw and this host has never said where an uncaught page error is READ — call "
           "result_set_page_error_hook (this host's output is a stream of lines, so it must print the error "
           "when it occurs) or result_page_errors_ride_the_document (this host publishes result_json "
           "unconditionally and `pageErrors` is in it). A host that renders the document only at the END of a "
           "run is the FIRST of those, not the second: a run that is killed before it drains publishes "
           "nothing, and the throw that ended a <script> is then the one fact its report cannot state");
    {
        /* AN OCCURRENCE, NOT A DUPLICATE TO DROP. The pair is still the dedupe key for what a reader SEES —
           the document is a console and the stream is one line per pair — but the row now counts what it
           stands for, so a later retraction has one occurrence to take back rather than a whole row to
           erase. This early return used to lose the second occurrence entirely. */
        PageErrorRow *row = errs_find(msg, filename);
        if (row) {
            /* THE PAIR'S LATCH, RISING — see result.h. Silent while it already stands, because the line
               announcing it is still true; announced again after a correction, because that correction was
               the stream's last word on this pair and it has stopped being true. */
            int was_standing = row->standing;
            row->standing++;
            if (!was_standing && g_err_hook) g_err_hook(msg, filename, RESULT_PAGE_ERROR_STANDS);
            return;
        }
    }
    /* routing between the two declared answers, never a default past one. BEFORE the row is committed, so an
       allocation failure below loses the row and not the announcement: the line is true either way, and the
       stream is the route whose whole purpose is saying so at the moment it happens. */
    if (g_err_hook) g_err_hook(msg, filename, RESULT_PAGE_ERROR_STANDS);
    if (g_errs_n >= g_errs_cap) {
        int c = g_errs_cap ? g_errs_cap * 2 : 8;
        PageErrorRow *a = realloc(g_errs, (size_t)c * sizeof(*g_errs));
        /* ONE ALLOCATION, so there is no half-grown row to misfile — the two `char **` columns this replaced
           needed a paragraph here saying a MISFILED row is worse than a lost one. A lost diagnostic is still
           not worth failing a run over. */
        if (!a) return;
        g_errs = a;
        g_errs_cap = c;
    }
    g_errs[g_errs_n].msg = strdup(msg);
    g_errs[g_errs_n].at = strdup(filename);
    g_errs[g_errs_n].standing = 1;
    g_errs[g_errs_n].retracted = 0;
    if (g_errs[g_errs_n].msg && g_errs[g_errs_n].at) { g_errs_n++; return; }
    free(g_errs[g_errs_n].msg); free(g_errs[g_errs_n].at);
    g_errs[g_errs_n].msg = NULL; g_errs[g_errs_n].at = NULL;
}

/* TAKING ONE BACK — result.h states the algorithm this serves and why a no-op is a positive answer here. */
void result_page_error_retract(const char *msg, const char *filename) {
    PageErrorRow *row;
    if (!msg || !*msg) return;   /* the same description `result_page_error` declines to record */
    DCHECK(filename != NULL,
           "a page error was retracted with no throw-site field at all — the retraction keys on the same "
           "(message, throw site) pair the report does, so a null one cannot name the row it means to take "
           "back and would silently retract nothing");
    row = errs_find(msg, filename);
    /* NO ROW, OR A ROW WITH NOTHING STANDING: §8.1.4.7 step 4.1.4 appends promises step 4.1.3 declined to
       report (the append is gated on [[PromiseIsHandled]], the report on notCanceled), so a page that cancels
       `unhandledrejection` is still owed a `rejectionhandled` this console never reported. There is nothing
       to take back and nothing to record — a row minted here would say this engine had named an error it
       never named, which is the fabrication the retraction exists to remove. */
    if (!row || !row->standing) return;
    row->standing--;
    row->retracted++;
    /* THE PAIR'S LATCH, FALLING. A correction printed while another occurrence still stands would withdraw a
       line that is still true of this run. */
    if (!row->standing && g_err_hook) g_err_hook(msg, filename, RESULT_PAGE_ERROR_RETRACTED);
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

/* THE PAIR A THROWN VALUE KEYS ON, DERIVED ONCE FOR BOTH EDGES. The report and its retraction must compose the
   identical (message, throw site) or no retraction would ever find the row it means to take back, and two
   copies of this derivation is exactly how they would come to differ. */
static void page_error_key(JSContext *ctx, JSValueConst err, char *buf, size_t bufsz, char **at) {
    /* §8.1.4.6 "Runtime script errors"'s THROW SITE, asked of the component that owns the derivation
       (core/events/report_exception.h) rather than re-parsed out of the rendered frame `result_error_text`
       appends. Two parsers of one backtrace disagree the first time either is corrected, and the answer here
       is the one a reader partitions a run's errors by. */
    uint32_t line = 0, col = 0;
    *at = report_exception_position(ctx, err, &line, &col);
    result_error_text(ctx, err, buf, bufsz);
}

void result_page_error_value(JSContext *ctx, JSValueConst err) {
    char buf[320];
    char *at;
    page_error_key(ctx, err, buf, sizeof buf, &at);
    result_page_error(buf, at);   /* an empty description is dropped by result_page_error's own first line */
    free(at);
}

/* §8.1.6.4 step 7.4's edge, keyed by the same derivation as the report — result.h states why a page that
   mutated the reason between the two events is a legitimate miss rather than something to assert on. */
void result_page_error_value_retract(JSContext *ctx, JSValueConst reason) {
    char buf[320];
    char *at;
    page_error_key(ctx, reason, buf, sizeof buf, &at);
    result_page_error_retract(buf, at);
    free(at);
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

/* ONE ENTRY PER DISTINCT MESSAGE, which is what `pageErrors` has always been and what the readers of the
   document expect — core/events/report_exception.c calls this a developer console, and a console is a list of
   what went wrong rather than a list of occurrences. The RECORD is keyed on (message, throw site) now, so this
   is the one place the two shapes part: a message raised from two scripts is two rows there and one line
   here, and the per-script fact is carried by the STREAM (result.h), which is where a reader that needs it
   reads. The skip is written out rather than folded into the record because collapsing it at the record would
   put the document's shape back into the dedupe and lose the pair again.
   TWO ARRAYS AND THEY ARE DISJOINT, WHICH IS WHAT MAKES AN EMPTY ONE READABLE. A message with any STANDING
   occurrence is in `pageErrors` — the page raised it and nothing took it back. A message with no standing
   occurrence anywhere and at least one retracted one is in `pageErrorsRetracted`: this engine named it and
   then withdrew it, because HTML §8.1.6.4 step 7.4 told it the page had handled the rejection after all. A
   message in NEITHER was never recorded. Those are three different facts about a page and, before the second
   array, the last two were the same absence — which is precisely §Testing's "an absent count and a zero count
   are different facts" wearing a name instead of a number.
   DISJOINT AND NOT OVERLAPPING, BECAUSE THE OVERLAP WOULD CONTRADICT ITSELF ON ONE SCREEN. A message that
   still stands at one script and was retracted at another DID go wrong, so it belongs in the console; listing
   it in both would render one message twice under two opposite claims. The per-site half of that fact is the
   STREAM's, exactly as the per-site half of the report is — the routes differ in form, never in what they
   know.
   A RETRACTED ROW IS STILL A CAPABILITY THE PAGE REACHED FOR, which is why the second array carries the
   MESSAGE rather than merely a count of withdrawals. `Element.matches is not a function` names an unbuilt
   engine capability whether or not the bundle caught the rejection it arrived in — the retraction says the
   page did nothing wrong, never that the engine has nothing to build. */
static char *errs_json_array_where(int want_standing) {
    char *b = NULL; size_t cap = 0, len = 0;
    int emitted = 0;
    errs_raw(&b, &cap, &len, "[");
    for (int i = 0; i < g_errs_n; i++) {
        int seen = 0, stands_somewhere = 0, retracted_somewhere = 0;
        /* BY MESSAGE, ACROSS EVERY ROW THAT CARRIES IT, because the two arrays are decided per MESSAGE while
           the rows are per pair: a message standing at one throw site is not retracted just because another
           site withdrew it. */
        for (int j = 0; j < g_errs_n; j++) {
            if (strcmp(g_errs[j].msg, g_errs[i].msg)) continue;
            if (j < i) seen = 1;
            if (g_errs[j].standing) stands_somewhere = 1;
            if (g_errs[j].retracted) retracted_somewhere = 1;
        }
        if (seen) continue;
        if (want_standing ? !stands_somewhere : (stands_somewhere || !retracted_somewhere)) continue;
        if (emitted++) errs_raw(&b, &cap, &len, ",");
        errs_raw(&b, &cap, &len, "\"");
        errs_append(&b, &cap, &len, g_errs[i].msg);
        errs_raw(&b, &cap, &len, "\"");
    }
    errs_raw(&b, &cap, &len, "]");
    return b ? b : strdup("[]");
}

/* THE ORDERING, COMPOSED — see result.h for why this lives here rather than in a host's printf, and for what
   the two shapes below mean. This function DECIDES NOTHING: it reads flow_wfq_census and renders it.

   HOW TO READ THE FULL SHAPE, kept beside the composer because a reader who has the bytes is the one who needs
   it. `valMax - valMin` against 1.0 is the reward spread against the optimism term's ENTIRE range: wider than
   that and the bonus can no longer reorder the frontier's ends, so the order is the reward's and the bottom
   waits on the aging term alone. `valZero` names who is down there (a from-baseline flow enters at reward 0 —
   every candidate session and every joined document's boot flow), `selfEmit` says whether anything has emitted
   since it was born, `svcMin` beside `svcMax` says whether the aging term is measuring one flow or the whole
   frontier, and `families` is what turns `svcFamMin == svcFamMax` from an ambiguity into a reading (1 is an
   identity of the structure and the family half can never order anything; more than 1 is a term that orders
   and is momentarily level). solver/flow.h states each row in full.

   AND HOW TO READ THE JOB ROWS, which are the ones that say what the ORDER is costing. `jobsOwed`,
   `jobsFramed` and `jobsReady` are the cold line's `jobs` total split by what each job waits on — the host,
   the member finishing its own program (HTML §8.1.4.4 "Calling scripts", clean up after running script step
   3), or RANK. Only the last is the WFQ's to move, and `jobWGap` says how far behind the front of the queue it
   stands, in the order's own points: 0 with `jobsReady > 0` is the top of the queue holding a runnable job and
   no ordering problem at all; a gap on the scale of `valMax - valMin` is the reward spread burying the backlog
   where the aging term — FLOW_AGE_QUANTUM per quantum of silence — cannot reach it inside a session. `jobsReady`
   at 0 makes `jobWGap` 0 too, which is why the pair is read together and neither alone. `visZero` is the count
   `visMin` cannot give: how many members have completed NO unit of work, which is both the population
   `jobsFramed` belongs to and the population whose optimism bonus can never decay.

   AND `neverPicked`/`neverPickedGap` ARE THE SAME PAIR ASKED OF THE ONE WORD IN §scheduler'S RAZOR THAT NO
   OTHER ROW HERE CAN ANSWER. The razor forbids a resume that "drops, starves, skips, reorders, or forgets ANY
   flow"; every row above that looks like it names the starved population is a TERM OF THE WEIGHT, and
   flow_credit_emit resets the SILENCE ones — `unrun` is zero own silence and an emission by ANY arm of the
   member's family writes that zero for the whole family at once, and flow_pick's own `unrun` needs all three
   at zero, which its own comment says is non-empty only within one quantum of an emission. So each of those
   counts a member that has just PRODUCED something — and every arm standing beside it — as one that has never
   run, and on a frontier that has gone quiet neither can name anybody. `visZero` is the exception and is not
   the answer either: an emission used to write `visits = 0` on the emitter and no longer does, so the row now
   means exactly "completed no unit of work" — which a member can read for a whole run BECAUSE it was
   dispatched into a program that never ends, a resume-seam defect wearing an ordering row's shape.
   `neverPicked` counts members the scheduler has never handed the thread — the one
   quantity nothing resets, because the member did nothing to reset it — and `neverPickedGap` is how far the
   best of them stands behind the weight the pick actually returned, in the order's own points. Read them the
   way `jobsReady`/`jobWGap` are read: a large gap is the ordering WORKING (those members are outranked, and
   the aging term is what reaches them). Neither half is a reading alone.

   AND A GAP AT ZERO IS NOT THE OPPOSITE VERDICT — THIS PARAGRAPH USED TO SAY IT WAS, AND THE SENTENCE IS
   RETIRED HERE RATHER THAN DELETED, because the reading it licensed is the one a reader re-derives. It said a
   gap at or near ZERO beside a non-zero count was, in this file's own words and no standard's, a member
   standing at the front of the order that the pick is not returning — starvation rather than ordering, a
   defect in the dispatch rather than in the weight. That
   does not follow, for a reason visible in flow_pick: the comparison is STRICT, so on a frontier carrying a
   large EQUAL-WEIGHT cohort — which is the ordinary state of a one-family page, since every member of a family
   reads one reward and an emission zeroes that family's silence at every arm at once — the pick returns ONE of
   N tied maxima and the other N-1 are, at that instant, never-picked members standing exactly at the front.
   Zero is then the EXPECTED reading of a healthy sweep, not evidence of anything, and the row cannot tell that
   state from the one the retired sentence named.
   MEASURED, which is why the sentence is going rather than being softened: six runs of the native fixture at
   3ca1e281, 212 `@WFQ` censuses, `neverPickedGap` min 0.000 / median 0.476 / max 5.563 with SIXTY-THREE samples
   at exactly 0.000 — spread across every run, including runs whose ladder drained all the way to the orphan
   seed. A verdict that fires on 30% of samples of a frontier that is working is not a verdict.
   WHAT THE PAIR HONESTLY SAYS is the count and the distance, and the reading that needs neither guessed is a
   SERIES: `neverPicked` climbing across consecutive censuses while the frontier grows is the tail not being
   reached, and that is a throughput statement. A single sample of this row — of any row here — characterises
   an instant and never a run, which is the same rule §Testing states for every other number in this tree.

   AND `topSvc`/`topSvcFam`/`nonrewardMax` ARE WHAT TURN `valTop` FROM A DIGIT INTO A STATEMENT ABOUT THE
   LEADER. `valTop` is the front flow's fork FAMILY's ledger, and a ledger only climbs — so a reward that has
   not moved between two censuses reads exactly like one being earned slowly, and "is the leading account still
   producing" is precisely the question the row cannot answer alone. The silence is the half an emission RESETS
   (flow_credit_emit zeroes the account's `fam_us`), so `topSvcFam` climbing IS the leading account being
   silent, and `topSvcFam` at or near zero is its aging being forgiven. READ `topSvc` FOR ANYTHING ABOUT A GAP
   AND NEVER `topSvcFam`: every arm of one family reads one `fam_us`, so on a `families: 1` frontier that half
   is charged to the leader and to every member behind it in the same instant and cancels out of
   `neverPickedGap` entirely, while the OWN half is charged only to the flow being dispatched. A leader
   genuinely monopolising the thread shows `topSvc` climbing monotonically and the gaps closing behind it; a
   front being REFILLED by freshly-minted arms shows `topSvc` low or sawtoothing with the same gaps standing,
   and no amount of waiting closes those because the flow being charged is never the flow at the front. Those
   two take opposite work and no other row here separates them — `svcMax` and `svcMin` are the frontier's ends
   and the leader need be neither. `nonrewardMax` is flow.c's FLOW_NONREWARD_MAX carried out so a reader
   DERIVES the bound it judges the gaps by instead of restating it: a weight is an account's reward plus a
   non-reward sum that function bounds, so `(valTop - valMin) + nonrewardMax` is the largest gap a NON-NEGATIVE
   non-reward sum can produce, and a gap above it says the trailing member's own terms are already net negative
   — behind by AGING, which nothing bounded reaches, rather than behind by LIFT, which one term reading
   differently would close. On a one-family frontier the reward half of that is identically zero.

   THE ARITHMETIC, DONE RATHER THAN ESTIMATED, and it is the reason the buffer is this size: the format's fixed
   bytes are 384 without its conversion specifiers, and the thirty-three numbers' widest forms are 3594
   (fourteen longs and nine int64s at 20, three `%.1f` doubles at 312 and seven `%.3f` at 314 — a double's
   widest decimal form is 309 integer digits plus sign, point and fraction, which is what makes this buffer two
   orders larger than the counter documents in this file). 384 + 3594 + 1 = 3979 against this 4096. RE-DO IT
   WHEN YOU ADD A ROW; the DCHECK below catches the arithmetic being re-done wrong, and is not a substitute for
   doing it. The starvation pair added 33 fixed bytes (`,"neverPicked":` at 15 and `,"neverPickedGap":` at 18)
   and 334 of conversion (one more long, one more `%.3f`), which is where 312/2906/3328 became 345/3240/3586;
   the leader triple then added 39 fixed (`"topSvc":` at 9, `,"topSvcFam":` at 13 and `,"nonrewardMax":` at 16,
   the leading comma of the group being the one the previous row already ended on) and 354 of conversion (two
   more int64s at 20 and one more `%.3f` at 314), which is where those became these. */
char *result_wfq_json(void) {
    WfqCensus w;
    char *out;
    size_t n = 4096;
    int m;

    flow_wfq_census(&w);
    out = malloc(n);
    if (!out) return NULL;
    /* AN EMPTY FRONTIER SAYS SO AND SAYS NOTHING ELSE — result.h states why the term rows are absent rather
       than zero. This is the shape `qjs_result` composes, because a session answers DONE by draining or by
       parking and both leave no members standing. */
    if (w.members == 0)
        m = snprintf(out, n, "{\"members\":0}");
    else
        m = snprintf(out, n,
                     "{\"members\":%ld,\"valMin\":%.1f,\"valMax\":%.1f,\"valTop\":%.1f,"
                     "\"valZero\":%ld,\"selfEmit\":%ld,\"unrun\":%ld,"
                     "\"neverPicked\":%ld,\"neverPickedGap\":%.3f,"
                     "\"svcMax\":%lld,\"svcMin\":%lld,\"svcFamMax\":%lld,\"svcFamMin\":%lld,\"families\":%ld,"
                     "\"visMin\":%lld,\"visMax\":%lld,\"visZero\":%ld,"
                     "\"cands\":%ld,\"candUnrun\":%ld,\"candSvcMax\":%lld,\"candDecMax\":%ld,\"decMax\":%ld,"
                     "\"distMax\":%.3f,\"wTop\":%.3f,\"wMin\":%.3f,\"candWMax\":%.3f,"
                     "\"topSvc\":%lld,\"topSvcFam\":%lld,\"nonrewardMax\":%.3f,"
                     "\"jobsReady\":%ld,\"jobsFramed\":%ld,\"jobsOwed\":%ld,\"jobWGap\":%.3f}",
                     w.members, w.val_min, w.val_max, w.val_top,
                     w.val_zero, w.self_emit, w.unrun,
                     w.never_picked, w.never_picked_gap,
                     (long long)w.svc_max, (long long)w.svc_min,
                     (long long)w.svc_fam_max, (long long)w.svc_fam_min, w.families,
                     (long long)w.vis_min, (long long)w.vis_max, w.vis_zero,
                     w.cand_members, w.cand_unrun, (long long)w.cand_svc_max, w.cand_dec_max, w.dec_max,
                     w.dist_max, w.w_top, w.w_min, w.cand_w_max,
                     (long long)w.top_svc, (long long)w.top_svc_fam, w.nonreward_max,
                     w.jobs_ready, w.jobs_framed, w.jobs_owed, w.job_w_gap);
    DCHECK(m > 0 && (size_t)m < n,
           "the WFQ census did not fit its buffer — a truncation here does not lose a digit, it loses the "
           "closing brace, so the document that embeds it will not parse and every finding for this page is "
           "discarded. Re-do the byte count above rather than widening it by eye");
    return out;
}

/* WHAT A CONTEXT SWITCH COSTS, AND WHAT THE TWO CHAINS ARE STILL HOLDING — see result.h for why this composes
   here rather than in a host's printf. It DECIDES NOTHING: it reads cow.c's and dom_cow.c's own stats and
   renders them.

   THE TWO HALVES ANSWER DIFFERENT QUESTIONS AND THAT IS WHY THEY ARE ON ONE LINE. `installs`/`entries`/`worst`
   are the COST of a switch — how many the scheduler made and how many delta slots it had to unapply and
   re-apply, with `mean` the per-switch figure a reader actually compares. `heapSegs`/`domSegs` and their entry
   counts are RETENTION, which is the other thing a delta can get wrong and which the cost rows are blind to: a
   frontier of four flows whose chains hold tens of thousands of frozen segments is a lifetime bug that reads
   exactly like a healthy run in the first three numbers.

   THE ARITHMETIC, DONE FROM THE FORMAT STRING RATHER THAN ESTIMATED: its fixed bytes are 99 without the
   conversion specifiers, and the eight numbers' widest forms are 452 (seven longs at 20 and one `%.1f` double
   at 312 — 309 integer digits plus sign, point and fraction). 99 + 452 + 1 = 552 against this 576. RE-DO IT
   WHEN YOU ADD A ROW; the DCHECK below catches the arithmetic being re-done wrong and is not a substitute for
   doing it. */
char *result_swap_json(void) {
    long sc = 0, st = 0, sm = 0, hs = 0, he = 0, ds = 0, de = 0;
    size_t n = 576;
    char *out;
    int m;

    cow_swap_stats(&sc, &st, &sm);
    cow_chain_stats(&hs, &he);
    dom_cow_chain_stats(&ds, &de);
    out = malloc(n);
    if (!out) return NULL;
    m = snprintf(out, n,
                 "{\"installs\":%ld,\"entries\":%ld,\"worst\":%ld,\"mean\":%.1f,"
                 "\"heapSegs\":%ld,\"heapSegEntries\":%ld,\"domSegs\":%ld,\"domSegEntries\":%ld}",
                 sc, st, sm, sc ? (double)st / (double)sc : 0.0, hs, he, ds, de);
    DCHECK(m > 0 && (size_t)m < n,
           "the swap census did not fit its buffer — a truncation here does not lose a digit, it loses the "
           "closing brace, so the document that embeds it will not parse and every finding for this page is "
           "discarded. Re-do the byte count above rather than widening it by eye");
    return out;
}

/* WHAT THE FRONTIER IS MADE OF AND WHAT ITS PARKED SNAPSHOTS WEIGH — solver/cold.h's ColdCensus, this
   instance's own totals (solver/engine.h's EngineFrontierCensus) and what a resume rebuilt out of a residue.
   See result.h for why it composes here.

   THE PER-FLOW ROWS ARE WHAT MULTIPLY BY THE FRONTIER'S SIZE and the SHARED rows are counted once for the
   whole frontier, because a frozen segment is referenced by every flow forked below it — so `perFlowKiB` and
   `sharedKiB` are the two totals a pager actually trades against each other, and each is a SUM of the rows
   named beside it rather than a separate measurement. `dynKiB` is priced with the shared half and not the
   per-flow one: a program's text is ONE buffer however many timelines hold that program (solver/dyn_body.h),
   and summing it per flow would report the sharing as if it did not exist.

   `owed` BESIDE `blocked`, because the two answer different questions and the GAP between them is the
   diagnostic. `blocked` asks each flow's REGISTER whether the host owes it anything; `owed` counts the flows
   that have told the SCHEDULER they cannot progress, which is what the pick actually reads. A fully blocked
   frontier reporting `blocked: 512, owed: 59` is one whose marks are being cleared faster than the sweep can
   lay them down. On a healthy stall the two agree.

   `live` IS NAMED AFTER WHAT IT COUNTS, WHICH IT WAS NOT. It was emitted as `flows` while the run's created
   count was ALSO called `flows` one line away, so the frontier's current size and the number of flows ever
   made were one word — and those two are opposite verdicts on the same shape: a frontier that stops growing
   has either RETIRED its flows or PAGED them out, and `finished` and `sold` beside `live` are what tells those
   apart. The created total keeps its own name on the document (`_flows`), where it is a TOTAL among totals.

   AND `finished` AND `sold` EACH CARRY THE TWO POPULATIONS THEY ARE THE SUM OF, because one counter over a
   frontier that is mostly @S candidate sessions answers neither question a reader has. An exploration flow
   retiring is coverage this document gained; a candidate session retiring is one derived payload that ran and
   did NOT fire, which is the search discarding it — and on a frontier where the candidates are the great
   majority of the members, "the engine retired N flows" IS "the search discarded essentially nothing" with
   nothing in the row to say so. The label is `Flow.cand_src` and it is a binary partition for the whole of a
   member's life (solver/engine.h says why it is two rows and not three), the totals STAY so the parts have
   something to be checked against, and engine_frontier_census asserts the identity at the one place all six
   are in one hand — the same discipline `stepUnits` keeps against `live` at the composition below.
   THE PARTS ARE ROWS AND NOT A SUBTRACTION, deliberately. Emitting the candidate half alone would leave the
   other half to be derived by a consumer, and a derived half cannot be checked: `finished - finishedCands`
   is a number for every pair of inputs, including the pair where one of them stopped being written. Both
   arms are emitted, always, zeroes included, and engine/build.mjs's `COLD_FIELDS` throws on either going
   absent exactly as it does for every other row here.

   `live` IS NOT SPLIT HERE AND THAT IS NOT AN OVERSIGHT. It is compound in the same way — `cold_census` and
   `flow_wfq_census` walk the SAME registry — and the `_wfq` census on this same document already carries the
   candidate half of it as `cands` (solver/flow.h: "members carrying a payload substitution"). A `liveCands`
   row would be the second spelling of one number in one document, which is precisely the drift the
   record-field contract exists to catch and which `resumedOrphans` is refused for six paragraphs down.
   `framed` and `blocked` are compound too and are deliberately whole: the first is the park's re-execution
   COST and the second is what the host owes, and a pager and a host each pay for a member whichever
   population it belongs to, so a split of either would be a row no consumer could state anything new from.

   THE THREE ORPHAN-CLAIM ROWS ARE THE COLD ROUND TRIP'S VERDICT. `orphanClaims` is how many inherited drives a
   resume rebuilt out of the residue, `orphanClaimsMet` how many of those waits a take satisfied, and
   `orphanClaimsUnmet` how many waiting flows FINISHED never having been handed a body. THE LAST IS THE VERDICT
   AND THE FIRST TWO ARE CONTEXT: met can legitimately EXCEED the records, because a waiting drive forks arms
   while it replays and every arm is the same drive of the same body, so met-minus-claims is not a loss.
   Unmet is the loss, exactly — on a document whose bytes did not change between two sessions it is ZERO.

   AND ZERO IS ALSO WHAT A SESSION THAT NEVER RESUMED WROTE THERE, WHICH MADE THE VERDICT UNREADABLE. All three
   are derived from a rebuild: `cold_resumed` reports the LAST one and its record is memset at the top of
   `cold_resume`, and the claim counters are reset with the session — so a session handed NO RESIDUE AT ALL
   reports the identical 0/0/0 as a resume that rebuilt a frontier and lost nothing. Two states, one number, on
   the row this comment calls THE VERDICT: a reader taking `orphanClaimsUnmet: 0` for a pass was reading a pass
   out of a session that never resumed. §Testing — an absent count and a zero count are DIFFERENT facts and
   must never be averaged — and the defaulted-field rule, one hop earlier: here the producer emitted the hole
   itself, so no consumer had to fill one.

   `resumed` IS THE POSITIVE STATEMENT AND IT IS ALWAYS PRESENT, WHICH IS THE WHOLE OF WHY IT IS A ROW OF ITS
   OWN RATHER THAN A SHAPE A READER INFERS. `resumed: 0` says THIS SESSION WAS HANDED NO RESIDUE, so the three
   zeroes beside it are not a verdict at all; `resumed: 1` says a rebuild ran, and only under it is
   `orphanClaimsUnmet: 0` the pass this comment claims. It is answerable from the record because `cold_resume`
   ends by asserting it landed at least one flow (`DCHECK(flows > 0)`) and memsets its census on entry, so
   `flows + cands == 0` is producible only by a session that never called it — a READING, not an inference, and
   the DCHECK at the composition below is what keeps it one. It is not a count and cannot be averaged with one,
   and there is no absent value for a consumer to fill: the two states are two values of one always-emitted
   number, which is the shape the defaulted-field rule asks for and the shape a nested object or an omitted row
   would each have missed — the first because this census's readers assert every row is a finite number
   (extension/bridge.js) and render it generically (extension/popup.js), the second because an absent number is
   exactly what `|| 0` turns back into a zero.

   AND THE DECOMPOSITION RIDES BESIDE IT — `resumedSegs`/`resumedFlows`/`resumedCands`/`resumedWorlds`, which
   cold.h calls "the observable that says which ARMS of the grammar ran". A residue of nothing but 'f' records
   exercised neither park_unhex nor solve_resume_candidate nor the foreign-world rebuild, and `@RESUMED 4`
   looks identical either way, so this is what tells an EXERCISED round trip from an unexercised one — the
   difference between a rebuild that proved the read half of the cold tier and one that proved a quarter of it.
   ALWAYS PRESENT, ZEROES INCLUDED, and that is not the defect this row exists to fix — it is the reason
   `resumed` is a row of its own. result.h's contract for these three censuses is that EVERY ROW IS ALWAYS
   PRESENT because they read subsystems that exist at every instant a document can be composed at, so omitting
   four of them under one condition would trade a two-states-one-number defect for a broken shape contract, and
   an omitted number is in any case exactly what `|| 0` turns back into a zero. Under `resumed: 0` the four
   zeroes are the true decomposition of a rebuild that did not happen; under `resumed: 1` they are a reading.
   One row says which, and it cannot be absent.
   THIS WAS READABLE AT THE POPUP ALONE AND NOWHERE ELSE. The JS half of this round trip rides the per-run LOG
   ROW, which happens to carry `cold` beside it; the ANALYSIS DOCUMENT — what test_forced.c, the ABI entry and
   every other in-process host holds — carried no statement of the kind at all, so every consumer but one read
   a resumed session and a session that never resumed as the same three zeroes.
   `resumedOrphans` IS NOT A ROW: `orphanClaims` IS that number, and two spellings of one number in one
   document is the drift the record-field contract exists to catch — the same sentence the paragraph below
   makes about `_orphansDriven`.
   ALL FIVE ARE IN engine/build.mjs's `COLD_FIELDS`, which is the guard that a row this composer stops emitting
   or renames fails there instead of being compared as `undefined` — and both of that list's readers throw on a
   non-numeric row, so dropping one of these five is caught rather than reported as a verdict nobody can read.

   NO `orphans` ROW: the count of drives this session STARTED is `_orphansDriven` on the document already, and
   two spellings of one number in one document is the drift the record-field contract exists to catch.

   `steps` AND `stepUnitRuns` ARE NOT A SECOND SPELLING OF `stepUnits`, WHICH IS THE ONE THING A READER MUST NOT
   TAKE THEM FOR. `stepUnits` is a GAUGE — the members standing in each arm at the instant this document is
   composed — so its zero says nobody is sitting there now. `stepUnitRuns` is a LIFETIME COUNT of the steps this
   instance has run through each arm, so its zero says the ladder has never once reached that arm, and `steps`
   is the total those arms partition. The pair is the axis the census had no instrument for: with the gauge
   alone, "the ladder is never entered below rung N" and "it is entered constantly and no member is resting
   there when a census happens to be taken" are ONE zero, and they are opposite diagnoses with opposite fixes.
   Both rows are therefore emitted and neither is derived from the other — a derived half is a half that cannot
   fail, which is the same argument `coldPartition` makes about `finished - finishedCands` in engine/build.mjs.
   THEY ARE A REPORT AND NEVER A BOUND (§NO BOUNDS). Nothing in the engine reads them to decide anything, and
   the counters say so at their declaration in solver/engine.c; it is worth saying here too, because the census
   is where a reader MEETS the numbers and a lifetime per-arm total is exactly the shape someone reaches for
   when they want a no-progress detector.

   `hostAnswersExtra` IS BESIDE `hostAnswered` AND IS NOT PART OF IT. One rendezvous has one answer per peer
   TIMELINE and every one of them is true, but only the FIRST settles the ask; the rest each fork an arm and
   unblock nothing. They were being added into `hostAnswered`, which made a peer holding four timelines read as
   four payments for one ask and inverted the census's own `answered <= asked`.

   THE ARITHMETIC IS THE EXPRESSION THAT SIZES THE BUFFER AND NOT A SENTENCE BESIDE IT, because the sentence
   beside it was WRONG and the buffer it justified was TOO SMALL. It read "fixed bytes 503 … the thirty-nine
   numbers' widest forms are 753 … 503 + 753 + 1 = 1257 against this 1280", and the format string it described
   measured 521 fixed bytes over FORTY numbers — thirty-seven longs and three ints, whose widest forms are
   773 — so the honest sum was 521 + 773 + 1 = 1295 against a 1280 buffer that was ALREADY 15 BYTES SHORT of
   its own worst case. Every one of those figures was wrong in the safe-looking direction, which is the exact
   history `result_document`'s comment records fifty lines down: an arithmetic ADJUSTED to a new row rather
   than re-derived from the string. So the counts stop being prose. They are still hand-derived — there is no
   portable way to ask a format string its widest expansion — but they are now TERMS THE COMPILER ADDS, so the
   stated sum and the allocated size cannot disagree with each other, and the way a forgotten row surfaces is
   the DCHECK under the snprintf rather than a paragraph nobody re-checks.
   WHERE IT IS STILL FRAGILE, SAID PLAINLY. The counts are a HAND CENSUS of one string — that part did not go
   away, it only stopped being able to disagree with the malloc. The widths are `long` at 20 and `int` at 11,
   which is the 64-bit host's `long`; on WASM32 A `long` IS 32 BITS and every long term is generous by nine
   bytes, so the assert cannot fire on the shipping host FIRST — a miscount is found where the numbers are
   widest, which is not where this runs in production. There is also NO SLACK by construction: the terms ARE
   the worst case, so the widest possible census fills the buffer exactly and any miscount at all is a
   truncation rather than a near miss. That is the intent. RE-DO THE COUNTS WHEN YOU ADD A ROW: they are three
   integers and there is no way to be nearly right. */
/* ONE ROW-COMPOSER FOR THE TWO PER-ARM HISTOGRAMS THIS CENSUS CARRIES. They differ in exactly one thing — the
   POPULATION they are counts of — and not at all in how a row is spelled, so a second copy of the loop would be
   a second speller of solver/step_unit.h's row format, which is the drift that file's own "THE ONE LIST"
   paragraph exists to prevent one level down.
   `what` NAMES THE HISTOGRAM IN THE WIDTH ASSERT, and it is a parameter rather than a sentence because a
   DCHECK stamps the line it is WRITTEN at: a shared helper's message reports this function for both callers, so
   "the histogram did not fit" would name an action with no object (CLAUDE.md §AN-ASSERT-THAT-NAMES-A-REMEDY).
   With two callers that is cheap to fix and it is fixed rather than deferred, because the third caller is the
   one who would have paid for it.
   IT RETURNS THE SUM AND ASSERTS NOTHING ABOUT IT. The two callers' identities are DIFFERENT — one sums to the
   frontier's live members, the other to the scheduler's step count — so the comparison belongs at each caller
   where its own other side is in hand, and a shared assert would have to be given the answer it is checking. */
static long cold_hist_json(char *buf, size_t cap, const long *counts, const char *what) {
    int hi = 0, k;
    long seen = 0;

    DCHECK(buf != NULL && counts != NULL, "a step-unit histogram was composed from nothing or into nothing");
    buf[hi++] = '{';
    for (k = 0; k < STEP_UNIT_N; k++) {
        int w = snprintf(buf + hi, cap - (size_t)hi, "%s\"%s\":%ld",
                         k ? "," : "", step_unit_name((StepUnit)k), counts[k]);
        DCHECKF(w > 0 && (size_t)w < cap - (size_t)hi,
                "the `%s` step-unit histogram did not fit the width its own list derives — "
                "STEP_UNITS_JSON_MAX is computed from solver/step_unit.h's names, so a row that does not fit "
                "means a count wider than a `long`'s 20 digits or a name that reached this buffer from "
                "somewhere else", what);
        hi += w;
        seen += counts[k];
    }
    buf[hi++] = '}';
    buf[hi] = 0;
    return seen;
}

char *result_cold_json(void) {
    /* The format string's widest expansion, as terms rather than as a sum somebody typed. `COLD_LITERAL` is the
       string with every conversion specifier removed. */
    enum { COLD_LITERAL = 737, COLD_LONGS = 49, COLD_INTS = 4 };
    ColdCensus c;
    /* THE TWO PER-ARM HISTOGRAMS, EACH COMPOSED INTO ITS OWN BUFFER AND SPLICED AS ONE `%s`. Their width is
       STEP_UNITS_JSON_MAX, an expansion of solver/step_unit.h's list, so an arm ADDED there widens both
       automatically — which is the one thing the hand-counted terms above must not be asked to do.
       THEY ARE TWO ROWS BECAUSE THEY ARE TWO QUESTIONS, and this is the whole reason the second exists.
       `hist` is a census of the MEMBERS STANDING at this instant, so a zero there says nobody is sitting in
       that arm right now; `runs` is a count of the STEPS this instance has run, so a zero there says the
       ladder has never once reached that arm. A reader holding only the first cannot tell "the arm is never
       entered" from "it is entered constantly and left again before every census" — opposite diagnoses with
       opposite fixes — and a reader holding only the second cannot tell where the frontier is parked. Neither
       is derivable from the other and neither may be defaulted into the other.
       EVERY ROW IS EMITTED, INCLUDING THE ZEROES, and that is the contract rather than a courtesy: an ABSENT
       row and a ZERO row are different facts (this composer changed, against this frontier had nobody in that
       arm), and a reader that cannot tell them apart is the defect that made `@RESUMED` read 0 for every
       session there has ever been. The consumer asserts the row's presence and reads its value; neither side
       may default the other's hole. */
    char hist[STEP_UNITS_JSON_MAX];
    char runs[STEP_UNITS_JSON_MAX];
    ColdResumed resumed;
    EngineFrontierCensus e;
    EngineStepUnitRuns r;
    /* …plus the two histograms' own width, which is the ONE term here that is not a hand count: it is an
       expansion of solver/step_unit.h's list, so adding an arm cannot silently truncate this document. */
    size_t n = COLD_LITERAL + COLD_LONGS * 20 + COLD_INTS * 11 + 2 * STEP_UNITS_JSON_MAX + 1;
    char *out;
    int ran;
    int m;

    cold_census(&c);
    engine_step_unit_runs(&r);
    {
        /* THE SUMS ARE TAKEN OUTSIDE THE ASSERTS AND NOT INSIDE THEM, because the composition is the WORK and a
           DCHECK's condition is compiled out in release: a `DCHECK(cold_hist_json(...) == x)` would leave both
           buffers unwritten in exactly the build that ships, and the document would carry whatever the stack
           held. A DCHECK condition must be side-effect-free, so the effect happens here and only the
           comparison is asserted. */
        long standing = cold_hist_json(hist, sizeof hist, c.step_units, "stepUnits");
        long stepped  = cold_hist_json(runs, sizeof runs, r.arms, "stepUnitRuns");
        /* THE PARTITION IS THE POINT, SO IT IS ASSERTED. Every live member carries exactly one arm, so these
           counts SUM to the frontier — an inequality is the walk having missed a member or a member having
           been counted twice, and either makes every reading composed from this row a statement about a
           frontier that is not the one standing. */
        DCHECK(standing == c.flows,
               "the step-unit histogram does not account for every member of the frontier — each live flow "
               "carries exactly one arm, so a total that is not `flows` means the census walk and the "
               "histogram disagree about who is standing");
        /* AND THE LIFETIME HISTOGRAM'S OWN IDENTITY, WHICH IS A DIFFERENT ONE AND IS ASKED TWICE ON PURPOSE.
           engine.c asserts it at the scheduler's convergence point, where it is exact and where the step that
           broke it has just returned; this is the other side of the same contract, at the boundary the number
           CROSSES — the census composer. A difference visible here and not there is a row lost between the
           accessor and this document rather than a step that failed to record itself, and those are two
           different files to open. */
        DCHECK(stepped == r.steps,
               "the lifetime step histogram does not account for every scheduler step — the arms are counted "
               "at the convergence point and the steps at flow_step's entry, so a total that is not `steps` "
               "means one of the two stopped being written, and every reading of which rung the ladder stops "
               "at is then about a ladder this document did not climb");
    }
    cold_resumed(&resumed);
    engine_frontier_census(&e);
    ran = resumed.flows + resumed.cands > 0;
    /* A REBUILD IS ALL OF ITSELF OR NONE OF IT, asserted here because `resumed: 0` is a POSITIVE claim that this
       session was handed no residue and this is the one place both halves of that claim are in one hand.
       cold_resume ends with `DCHECK(flows > 0)` and memsets its census on entry, so `flows + cands == 0` can
       only be the never-called state — unless a census arrives holding segments, foreign worlds or orphan
       locators under no flow at all, which is neither state and would publish as the claim that nothing came
       back. */
    DCHECK(ran || (resumed.segs == 0 && resumed.worlds == 0 && resumed.orphans == 0),
           "the cold tier reported a rebuild that landed no flow and yet rebuilt segments, foreign worlds or "
           "orphan locators — `resumed` is about to be emitted as 0, which STATES that this session was handed "
           "no residue, and that would be a lie about a residue that was read back. cold_resume's own "
           "`flows > 0` is the other side of this pair");
    /* AND A CLAIM IS EVIDENCE OF A DRIVE THAT WAS INHERITED, never of one this session started. `orphan_want` is
       written at exactly one site (cold.c's 'o' record) and spreads only by fork, so met-or-lost claims under a
       zero `resumed.orphans` mean the two counters have stopped describing one population and the round trip's
       verdict is being read off the wrong session. The comparison is legitimate at all only because
       engine_sched_begin calls cold_resume AT MOST ONCE per session — `cold_resumed` describes the last rebuild
       while the claim totals describe the whole session — so a second call site is precisely what this fires
       on, and it should. Met and unmet may both EXCEED `orphans` (a waiting drive forks arms and every arm is
       the same drive), which is why the implication runs one way only. */
    DCHECK(resumed.orphans > 0 || (e.claims_met == 0 && e.claims_unmet == 0),
           "an inherited-drive claim was met or lost in a session whose rebuild carried no orphan locator — the "
           "three orphanClaims rows are about to describe a round trip that this document also says did not "
           "happen");
    out = malloc(n);
    if (!out) return NULL;
    m = snprintf(out, n,
                 "{\"live\":%ld,\"framed\":%ld,\"blocked\":%ld,\"owed\":%d,"
                 "\"finished\":%ld,\"finishedFlows\":%ld,\"finishedCands\":%ld,"
                 "\"deepest\":%d,\"completed\":%d,"
                 "\"sold\":%ld,\"soldFlows\":%ld,\"soldCands\":%ld,\"forks\":%ld,"
                 "\"resumed\":%d,\"resumedSegs\":%ld,\"resumedFlows\":%ld,\"resumedCands\":%ld,"
                 "\"resumedWorlds\":%ld,"
                 "\"orphanClaims\":%ld,\"orphanClaimsMet\":%ld,\"orphanClaimsUnmet\":%ld,"
                 "\"hostAsked\":%ld,\"hostAnswered\":%ld,\"hostAnswersExtra\":%ld,"
                 "\"hostAnswersLate\":%ld,\"hostTerminated\":%ld,\"pagedReqs\":%ld,"
                 "\"pagedAsks\":%ld,\"pagedUnarmed\":%ld,\"pagedFloor\":%ld,"
                 "\"decEntries\":%ld,\"decKiB\":%ld,\"headEntries\":%ld,\"headKiB\":%ld,"
                 "\"domHeadEntries\":%ld,\"domHeadKiB\":%ld,\"jobs\":%ld,\"pend\":%ld,\"pendKiB\":%ld,"
                 "\"miscKiB\":%ld,\"perFlowKiB\":%ld,"
                 "\"segKiB\":%ld,\"domSegKiB\":%ld,\"pinSegs\":%ld,\"pinSegEntries\":%ld,"
                 "\"pinSegKiB\":%ld,\"decSegs\":%ld,\"decSegEntries\":%ld,\"decSegKiB\":%ld,"
                 "\"dynBodies\":%ld,\"dynKiB\":%ld,\"sharedKiB\":%ld,"
                 "\"steps\":%ld,\"stepUnitRuns\":%s,"
                 "\"stepUnits\":%s}",
                 c.flows, c.framed, c.blocked, flow_host_owed_count(),
                 e.finished, e.finished_flows, e.finished_cands,
                 e.deepest, e.completed,
                 e.sold, e.sold_flows, e.sold_cands, e.forks,
                 ran, resumed.segs, resumed.flows, resumed.cands, resumed.worlds,
                 resumed.orphans, e.claims_met, e.claims_unmet,
                 e.host_asked, e.host_answered, e.host_answers_extra, e.host_answers_late, e.host_terminated,
                 e.paged_reqs,
                 e.paged_asks, e.paged_unarmed, e.paged_floor,
                 c.dec_entries, c.dec_bytes / 1024, c.head_entries, c.head_bytes / 1024,
                 c.dom_head_entries, c.dom_head_bytes / 1024, c.job_count, c.pend_count,
                 c.pend_bytes / 1024, c.misc_bytes / 1024,
                 (c.dec_bytes + c.head_bytes + c.dom_head_bytes + c.pend_bytes + c.misc_bytes) / 1024,
                 c.seg_bytes / 1024, c.dom_seg_bytes / 1024,
                 c.pin_seg_count, c.pin_seg_entries, c.pin_seg_bytes / 1024,
                 c.dec_seg_count, c.dec_seg_entries, c.dec_seg_bytes / 1024,
                 c.dyn_count, c.dyn_bytes / 1024,
                 (c.seg_bytes + c.dom_seg_bytes + c.pin_seg_bytes + c.dec_seg_bytes + c.dyn_bytes) / 1024,
                 r.steps, runs,
                 hist);
    DCHECK(m > 0 && (size_t)m < n,
           "the cold/frontier census did not fit its buffer — a truncation here loses the closing brace, so "
           "the document that embeds it will not parse and every finding for this page is discarded. Re-do "
           "the byte count above rather than widening it by eye");
    return out;
}

/* WHAT THE RUNTIME AND THE C ALLOCATOR UNDER IT HOLD — quickjs's own JS_ComputeMemoryUsage walk, the child
   realms navigable.c built, and mallinfo. See result.h for why it composes here.

   "IT GREW" NAMES NOTHING TO FIX, which is why the kinds are here and not just a total. A climbing
   `allocations` with a flat `objects` is memory no GC object owns — an atom, a string, a property table, a
   bytecode function — and each of those has a different owner and a different place the owner forgot to let go.

   `miscBytes`/`miscParts` ARE NAMED AFTER WHAT THEY COUNT AND WERE NOT. They were emitted as
   `realmBytes`/`realmParts` on the claim that quickjs's `memory_used_*` is a walk of the CONTEXT LIST, so a
   reader asking "is the growth child realms?" read them and got an answer about something else:
   JS_ComputeMemoryUsage adds two entries per realm and then adds EVERY object's property array, every fast
   array's element vector, every var_ref, bound function, C-closure record and module entry to the same pair.
   It is the MISCELLANEOUS bucket. The realm question is answered by the component that knows —
   `navigable_realm_count`, which is the working set §A-CAPABILITY-MATERIALIZED-PER-FLOW names as a ceiling and
   which navigable.c's own OOM CHECK sends its reader to by name.

   AND `childRealms` ALONE ANSWERS TWO QUESTIONS WITH ONE NUMBER, which is why `childRealmsMade` and
   `childRealmsPeak` ride beside it. The live count is small for a run that built no child realm and small for
   a run that built a great many and reclaimed every one — opposite facts about the ceiling, and the second is
   what HTML §7.5.10 "Destroying documents" step 9's reference drop exists to produce. `made` is monotone and
   `peak` is the high-water live, so `made == peak` says every realm this run built was live at one instant and
   NOT ONE was reclaimed, while `made > peak` says the reclamation ran. An absent count and a zero count are
   different facts; so are a zero that means "none built" and a zero that means "all given back".

   `unattributed` IS ONE SUBTRACTION AND NOT A SUM OF ROWS. JS_ComputeMemoryUsage's last statements add atoms,
   strings, objects, properties, shapes, function bytecode and pc2line INTO `memory_used_size`, and every fast
   array's elements were already added to it in the object walk — so summing those rows again beside it counts
   the named heap TWICE and subtracts it twice from malloc_size, leaving a residual that reads healthy while
   the named part of the heap is the thing growing. The residual is: what the allocator holds, minus everything
   the runtime can name. `stepMachines` and `trampFrames` are what decompose it — a suspended continuation-
   holding builtin and a heap call frame are the two largest things quickjs cannot name, and a frontier of
   parked flows holds one of each per parked call and per suspended activation.

   THE ARITHMETIC, from the format string: fixed bytes 342 without the conversion specifiers, and the
   twenty-six numbers' widest forms are 475 (twenty-one int64s at 20 and five ints at 11).
   342 + 475 + 1 = 818 against this 832. RE-DO IT WHEN YOU ADD A ROW. */
char *result_heap_json(JSContext *ctx) {
    JSMemoryUsage mem;
    JSRuntime *rt;
    long long attributed;
    size_t n = 832;
    char *out;
    int m;

    DCHECK(ctx != NULL, "the heap census was asked for against no realm — every row of it is a walk of ONE "
                        "runtime, so a census with no runtime to walk is a reading of nothing");
    rt = JS_GetRuntime(ctx);
    JS_ComputeMemoryUsage(rt, &mem);
    attributed = (long long)mem.memory_used_size;
    out = malloc(n);
    if (!out) return NULL;
    m = snprintf(out, n,
                 "{\"allocations\":%lld,\"atoms\":%lld,\"strings\":%lld,\"objects\":%lld,"
                 "\"shapes\":%lld,\"props\":%lld,\"funcs\":%lld,\"funcCode\":%lld,\"arrays\":%lld,"
                 "\"miscBytes\":%lld,\"miscParts\":%lld,\"childRealms\":%d,"
                 "\"childRealmsMade\":%d,\"childRealmsPeak\":%d,"
                 "\"objBytes\":%lld,\"propBytes\":%lld,\"shapeBytes\":%lld,\"strBytes\":%lld,"
                 "\"atomBytes\":%lld,\"funcBytes\":%lld,\"arrayElemBytes\":%lld,"
                 "\"unattributed\":%lld,\"stepMachines\":%d,\"trampFrames\":%d,"
                 "\"cLiveKiB\":%lld,\"arenaKiB\":%lld}",
                 (long long)mem.malloc_count, (long long)mem.atom_count, (long long)mem.str_count,
                 (long long)mem.obj_count, (long long)mem.shape_count, (long long)mem.prop_count,
                 (long long)mem.js_func_count, (long long)mem.js_func_code_size, (long long)mem.array_count,
                 (long long)mem.memory_used_size, (long long)mem.memory_used_count,
                 navigable_realm_count(), navigable_realm_made(), navigable_realm_peak(),
                 (long long)mem.obj_size, (long long)mem.prop_size, (long long)mem.shape_size,
                 (long long)mem.str_size, (long long)mem.atom_size, (long long)mem.js_func_size,
                 (long long)mem.fast_array_elements * (long long)sizeof(JSValue),
                 (long long)mem.malloc_size - attributed,
                 JS_StepMachineCount(rt), JS_TrampFrameCount(rt),
                 (long long)engine_c_alloc_live() / 1024, (long long)engine_c_alloc_arena() / 1024);
    DCHECK(m > 0 && (size_t)m < n,
           "the heap census did not fit its buffer — a truncation here loses the closing brace, so the "
           "document that embeds it will not parse and every finding for this page is discarded. Re-do the "
           "byte count above rather than widening it by eye");
    return out;
}

/* The composition, and nothing else. Each surface serializes itself — endpoint.c walks its deduped endpoints,
   solve.c its fire-verified sinks — and this only decides that they are ONE document and what it is called.
   Keeping that decision in one place is the point: a second caller that wanted "just the endpoints" is how a
   host ends up assembling structure again. */
char *result_json(JSContext *ctx) {
    char *eps = endpoint_json_array();
    char *sinks = solve_json_array(ctx);
    char *errs = errs_json_array_where(/*want_standing*/ 1);
    /* AND THE ONES THIS ENGINE TOOK BACK — see errs_json_array_where. Composed beside `pageErrors` and never
       folded into it: a run in which the page raised nothing and a run in which it raised errors and handled
       every one of them are two different pages, and one empty array was the evidence for both. */
    char *errsRetracted = errs_json_array_where(/*want_standing*/ 0);
    /* THE ORDERING, ON THE ONE SURFACE THAT CROSSES THE ABI — see result.h. Composed here rather than by a
       host, because the host that had it is a driver the production entry does not call. */
    char *wfq = result_wfq_json();
    /* AND THE THREE CENSUSES THAT RODE NOTHING AT ALL, for the same reason and on the same surface: what the
       FRONTIER is made of, what the RUNTIME holds, and what a context SWITCH costs. result.h states the
       argument; `decide.c`'s table joins them because "which predicate is growing the frontier" is the same
       question one level down and had the same single unreachable emission site. */
    char *cold = result_cold_json();
    char *heap = result_heap_json(ctx);
    char *swap = result_swap_json();
    char *forkAt = decide_fork_json();
    /* AND WHAT THE ORDER THOSE FIVE SIT UNDER WAS DENOMINATED IN — solver/quantum.h's composer, not a sixth
       one of ours. It is the only field on this document that is neither a total over the run nor a reading of
       an instant: it is a property of the HOST, and it is here because without it two `_wfq` orderings taken
       from one revision on one page are not comparable and nothing on either document says so. result.h states
       the argument in full. */
    char *quantum = quantum_json();
    /* NO `probeResults` SURFACE. It carried the schemas an API's own REJECTION described, and a rejection is
       the answer to a DELIBERATELY MALFORMED REQUEST — one this engine cannot make, since its only network
       edge is the pending register and the host performs a GET through safeFetch. The reader on this side was
       filing whatever rejection a GET happened to provoke under the identity of an endpoint nobody probed.
       It is extension/lib/req2proto.js, which issues the probe as the page and writes straight into
       `globalStore.probeResults`; nothing about it crosses this seam. */
    size_t n;
    char *out;

    if (!eps || !sinks || !errs || !errsRetracted || !wfq || !cold || !heap || !swap || !forkAt || !quantum) {
        free(eps); free(sinks); free(errs); free(errsRetracted); free(wfq);
        free(cold); free(heap); free(swap); free(forkAt); free(quantum);
        return NULL;
    }
    /* THE SLACK COVERS THE WIDEST FORM, not the numbers that happen to occur. Counted rather than estimated,
       and stated so the count can be re-done: the format's fixed bytes are 603 with its conversion specifiers
       and 523 without them, and the twenty-one counters' full-width decimals are 375 (five ints at 11, sixteen
       longs at 20), so the worst case is 899 (523 + 375 + the NUL) against this 1024. The ELEVEN `%s`
       contribute nothing to that
       figure and everything to the sum above it — each is a composed surface whose real length is added by
       `strlen`, which is why a census joining the document costs a term in `n` and 11 bytes of literal here.
       `pageErrorsRetracted` is the eleventh: 25 bytes of literal, a `strlen` term, and nothing added to the
       widest form. THE MARGIN IS RAISED RATHER THAN SPENT, and the reason is not comfort: the DCHECK below is
       a DCHECK, so it is compiled out at `-DAPICLIENT_DEV=0` and a release build has the arithmetic and
       nothing else standing between it and a lost closing brace. It was 192 for a shape whose widest form was
       already 197 — inside only because the real numbers are small — then 384 against a worst case the arrival
       census took to 454, then 512 against 488, then 640 against the routed-delivery pair's 566, then 768.
       THAT 768 WAS ALREADY 50 BYTES SHORT WHEN THIS COUNT WAS RE-DONE, and the prose above it was the reason
       nobody noticed: it said "467 with its conversion specifiers and 407 without" and "nineteen counters …
       335", against a format string that measured 508/442 with twenty-one counters at 375. Every one of those
       five numbers was wrong in the safe-looking direction, so the stated worst case (742) sat comfortably
       inside a buffer the real worst case (818) did not fit — an arithmetic that had been ADJUSTED to the new
       field rather than re-done from the string, which is exactly what the sentence below tells you not to do.
       RE-DO THE ARITHMETIC WHEN YOU ADD A FIELD, FROM THE FORMAT STRING ITSELF; it is four counts and there is
       no way to be nearly right. The DCHECK under the snprintf is the second half of this, not a substitute for
       it: the arithmetic is what makes the buffer right, the assert is what catches it being re-done wrong —
       and here it was the only thing standing between the shipped document and a lost closing brace. */
    /* THE PARK DOCUMENT RIDES THE RESULT, because it IS a result: it is what this engine has left to say about
       a page it did not finish, and the host already does one JSON.parse of one document. "[]" — the ordinary
       case — tells the host this engine drained rather than paged out, which is what DELETES the origin's cold
       entry instead of leaving a stale residue that would be resumed forever. */
    n = strlen(eps) + strlen(sinks) + strlen(errs) + strlen(errsRetracted) + strlen(wfq) +
        strlen(cold_park_json()) +
        strlen(cold) + strlen(heap) + strlen(swap) + strlen(forkAt) + strlen(quantum) + 1024;
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
        /* AND WHETHER THE HEADLINE SURFACE RAN AT ALL — solver/engine.h's orphan census, which is the ORPHAN
           side of the four numbers above it. `_orphansDriven` existed and reached only the heap/progress line,
           which §Testing says nobody can read; `_orphansAsked` is what tells "this bundle ships no uncalled
           code" from "no flow ever reached the end of its own work". They ride the result document for the
           same reason every count here does. */
        long orphansDriven = 0, orphansAsked = 0;
        /* AND WHAT BECAME OF THE TASKS THOSE DELIVERIES QUEUED. `_routedDelivered` alone is the shape §@S
           forbids in a search and forbids here for the same reason: a page whose listener ran fewer times than
           the engine delivered has ONE number covering "the spec declined it" (§9.3.3 step 8.1), "there was no
           Document left to fire at" (§7.5.10 step 7) and "the scheduler lost the task", and only the last is a
           defect. All four ride the document rather than a log, for the reason the counts above them do. */
        long routedEnds[ROUTED_TASK_END_N];
        world_segment_stats(&made, &segf);
        solve_arrival_census(&sinkReached, &sinkTainted, &sinkSuppressed);
        engine_routed_census(&routedDelivered, &routedRefused);
        engine_orphan_census(&orphansDriven, &orphansAsked);
        engine_routed_task_census(routedEnds);
        m = snprintf(out, n, "{\"fetchCallSites\":%s,\"securitySinks\":%s,\"pageErrors\":%s,"
                             /* THE ONES THIS ENGINE NAMED AND THEN TOOK BACK — beside `pageErrors` because
                                the two are read together and disjoint: neither array can say on its own
                                whether an empty console means the page raised nothing or handled everything
                                it raised. errs_json_array_where states the three facts they keep apart. */
                             "\"pageErrorsRetracted\":%s,"
                             "\"_switches\":%d,\"_flows\":%ld,\"_candidates\":%d,"
                             "\"_jobsQueued\":%ld,\"_jobsRun\":%ld,\"_unitsDone\":%ld,"
                             "\"_worldSegmentsHeld\":%d,\"_worldSegmentsMade\":%d,"
                             "\"_worldSegmentsForked\":%d,"
                             "\"_routedDelivered\":%ld,\"_routedRefused\":%ld,"
                             "\"_routedTasksFired\":%ld,\"_routedTasksTargetOrigin\":%ld,"
                             "\"_routedTasksTargetGone\":%ld,\"_routedTasksThrew\":%ld,"
                             "\"_sourceReads\":%ld,\"_sinkReached\":%ld,\"_sinkTainted\":%ld,"
                             "\"_sinkSuppressed\":%ld,"
                             /* AND THE ORDER THE FRONTIER WAS IN WHEN THIS DOCUMENT WAS COMPOSED — result.h
                                says why it rides here and what its two shapes mean. Every counter above is a
                                TOTAL over the run; this one is a READING OF AN INSTANT, which is why it is one
                                nested object and not twenty-three more `_`-prefixed siblings: a consumer that
                                mixed them into the same row would be showing an instantaneous spread beside a
                                cumulative switch count and calling both "so far". */
                             "\"_orphansDriven\":%ld,\"_orphansAsked\":%ld,\"_wfq\":%s,"
                             /* THE THREE SUBSYSTEM CENSUSES, EACH ONE NESTED OBJECT, for the reason `_wfq` is
                                one: every `_`-prefixed sibling above is a TOTAL over the run and every row
                                inside these is a READING OF AN INSTANT, so spreading them into siblings would
                                put a cumulative switch count beside a momentary byte figure and call both "so
                                far". Three objects and not one, because a reader compares WITHIN a census and
                                never across — see result.h. */
                             "\"_cold\":%s,\"_heap\":%s,\"_swap\":%s,\"_forkAt\":%s,"
                             /* AND WHAT ALL OF THE ABOVE WERE DENOMINATED IN — the one nested object here that
                                is neither a total nor a reading of an instant, but a property of the HOST that
                                decides whether two of these documents may be compared at all. result.h and
                                solver/quantum.h state the argument; nothing in this file composes it. */
                             "\"_quantum\":%s,\"_park\":%s}",
                     eps, sinks, errs, errsRetracted,
                     engine_switch_count(), flow_created_count(), solve_candidate_count(),
                     engine_jobs_queued(), engine_jobs_run(), engine_units_done(), held, made, segf,
                     routedDelivered, routedRefused,
                     routedEnds[ROUTED_TASK_FIRED], routedEnds[ROUTED_TASK_TARGET_ORIGIN],
                     routedEnds[ROUTED_TASK_TARGET_GONE], routedEnds[ROUTED_TASK_THREW],
                     srcReads, sinkReached, sinkTainted, sinkSuppressed,
                     orphansDriven, orphansAsked, wfq, cold, heap, swap, forkAt, quantum,
                     cold_park_json());
        /* THE SLACK IS ASSERTED RATHER THAN EYEBALLED, AND IT IS THE ONLY THING THAT WAS STILL RIGHT. It was
           192 bytes for three counters and now carries twenty-one, whose widest form is 375 digits beside 523
           bytes of literal — and a previous 768 did not cover that, which nothing noticed because the prose
           stating the count had been adjusted instead of re-derived (see the arithmetic above). A truncation
           here does not lose a digit, it loses the closing brace: the host gets a document that will not parse
           and reports NOTHING for the page, which is the loudest possible consequence arriving as the quietest
           possible bug. snprintf already told us; this is what asks. */
        DCHECK(m > 0 && (size_t)m < n, "the result document did not fit its buffer — the host would be handed "
                                       "truncated JSON and every finding for this page would be discarded");
    }
    free(eps);
    free(sinks);
    free(errs);
    free(errsRetracted);
    free(wfq);
    free(cold);
    free(heap);
    free(swap);
    free(forkAt);
    free(quantum);
    return out;
}
