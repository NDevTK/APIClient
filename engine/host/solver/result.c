#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "solver/compose.h"   /* every census below is sized by what it writes — see composef */
#include "solver/result.h"
#include "solver/endpoint.h"
#include "solver/rung_entry.h"  /* the invoker rungs' denominator, read beside `stepUnitRuns` */
#include "solver/solve.h"
#include "solver/engine.h"
#include "solver/flow.h"
#include "solver/net_reach.h"   /* how far the frontier stands from the request sites it has not run */
#include "solver/world.h"   /* what the cross-instance seam materialized here — see world_segment_stats */
#include "solver/concolic.h"
#include "solver/metrics.h"    /* the @S arrival census and the schema it is declared in */
#include "solver/cold.h"    /* …and what it parked, if the host asked this engine to page out */
/* The reply door's rate, asked of the component that owns both ends of its membership rather than recomputed. */
#include "solver/pending_index.h"
#include "solver/dom_cow.h"   /* the DOM half of the swap census — see result_swap_json */
#include "solver/decide.h"    /* …and which predicate grew the frontier — see decide_fork_json */
#include "solver/absent.h"    /* …and which names a standard owns this realm answered with silence */
/* …and what the order above them was denominated in, which decides whether two of these documents may be
   compared at all. Composed by its owner — see result.h and quantum.h. */
#include "solver/quantum.h"
/* The realm count is the one heap-census row QuickJS cannot answer, and it is a browser fact: a child realm is
   built per flow that creates a navigable with an address, so the component holding the list answers. A
   per-flow capability must be reclaimed, so this count is a ceiling; navigable.c's OOM CHECK names it. */
#include "core/frame/navigable.h"
#include "core/css/css_cascade_pass.h"   /* what the run spent on css-cascade-5 §4.2's values — see the `_cascade` block */
#include "core/layout/flow_placement.h"   /* what the render spent on CSS 2.1 §9.4.1's positions — see the `_layout` block */
#include "core/frame/window_proxy.h"   /* the ask behind the realm census — see window_proxy_destroy_releases */
/* §8.1.4.6 "Runtime script errors"'s throw site — one component derives it, this one reports it. */
#include "core/events/report_exception.h"
#include "core/timing/event_loop.h"   /* whether the clock's licence was wanted and refused — see event_loop_advance_census */

/* The page's own uncaught errors, deduped (see result.h). The message is the page's own and is not interpreted.
   The dedupe key is the pair (message, HTML §8.1.4.6 "Runtime script errors" throw site), so two scripts raising
   one message stay two rows. `at` is "" for a thrown value with no backtrace, which is §8.1.4.6's own answer.
   A row counts occurrences so a report is revocable: `standing` were reported and not taken back, `retracted`
   were taken back, and their sum is how many were reported. A row at standing 0 is a pair named and withdrawn,
   a different fact from a pair never recorded (no row).
   One array of rows rather than parallel columns, so a growth failure cannot leave a half-grown row. */
typedef struct {
    char *msg;
    char *at;
    int   standing;
    int   retracted;
} PageErrorRow;
static PageErrorRow *g_errs; static int g_errs_n, g_errs_cap;

/* The row for this pair, or NULL. The one place the (message, throw site) key is spelled, so the report and
   the retraction agree on what identifies a row. */
static PageErrorRow *errs_find(const char *msg, const char *filename) {
    for (int i = 0; i < g_errs_n; i++)
        if (!strcmp(g_errs[i].msg, msg) && !strcmp(g_errs[i].at, filename)) return &g_errs[i];
    return NULL;
}

/* The pairs this engine declared its own exploration (see result.h; only the raising site can declare one).
   Keyed on the same (message, throw site) pair as a row, through the same derivation, so a mark answers about
   the row it was filed for. Not a field on PageErrorRow: the declaration is made at the raise, before §8.1.4.6
   reports anything, so a row field would only be a second copy of this table.
   Process-lifetime, like the rows; a pair raised twice is one declaration. */
typedef struct { char *msg; char *at; } ExploredPair;
static ExploredPair *g_expl; static int g_expl_n, g_expl_cap;

int result_page_error_explored(const char *msg, const char *filename) {
    DCHECK(msg != NULL && filename != NULL,
           "the exploration question was asked with half a key — a page-error row is identified by (message, "
           "throw site) and a null half would answer about whichever row happened to carry the other, which is "
           "a wrong classification rather than a missing one");
    for (int i = 0; i < g_expl_n; i++)
        if (!strcmp(g_expl[i].msg, msg) && !strcmp(g_expl[i].at, filename)) return 1;
    return 0;
}

static void explored_add(const char *msg, const char *filename) {
    if (result_page_error_explored(msg, filename)) return;
    if (g_expl_n >= g_expl_cap) {
        int c = g_expl_cap ? g_expl_cap * 2 : 8;
        ExploredPair *a = realloc(g_expl, (size_t)c * sizeof(*g_expl));
        /* A lost declaration is not worth failing a run over: the line then lands among the errors a reader
           reads by hand rather than among the engine's own. Nothing here is a verdict. */
        if (!a) return;
        g_expl = a;
        g_expl_cap = c;
    }
    g_expl[g_expl_n].msg = strdup(msg);
    g_expl[g_expl_n].at = strdup(filename);
    if (g_expl[g_expl_n].msg && g_expl[g_expl_n].at) { g_expl_n++; return; }
    /* Half a pair is worse than none — it would match on the half that allocated — so it is discarded. */
    free(g_expl[g_expl_n].msg); free(g_expl[g_expl_n].at);
    g_expl[g_expl_n].msg = NULL; g_expl[g_expl_n].at = NULL;
}

/* Who prints a page error as it happens, and whether the host declared a route at all (see result.h). The
   latch is separate from the hook because "publishes the document" is a declaration of its own, and a NULL
   hook alone cannot tell it from a host that never considered the question. */
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
    /* Never NULL: "" is §8.1.4.6's answer for no backtrace, and NULL would be a caller that never asked. */
    DCHECK(filename != NULL,
           "a page error was recorded with no throw-site field at all — §8.1.4.6's `filename` is \"\" for a "
           "thrown value carrying no backtrace and that is a positive answer, so a null one is a caller that "
           "did not ask rather than a value that had nothing to say");
    /* Asserted at the first uncaught page error: a page's throw names an unbuilt capability, and a host that
       declared neither route has no place that name can be read. The message is recorded either way. */
    DCHECK(g_err_route_declared,
           "the page threw and this host has never said where an uncaught page error is READ — call "
           "result_set_page_error_hook (this host's output is a stream of lines, so it must print the error "
           "when it occurs) or result_page_errors_ride_the_document (this host publishes result_json "
           "unconditionally and `pageErrors` is in it). A host that renders the document only at the END of a "
           "run is the FIRST of those, not the second: a run that is killed before it drains publishes "
           "nothing, and the throw that ended a <script> is then the one fact its report cannot state");
    {
        /* A repeat is an occurrence, not a duplicate to drop. The pair is still the dedupe key for what a reader
           sees (one line per pair), but the row counts occurrences so a retraction takes back exactly one. */
        PageErrorRow *row = errs_find(msg, filename);
        if (row) {
            /* The pair's latch, rising (see result.h): silent while it already stands, announced again after a
               retraction, because the retraction was the stream's last word on this pair. */
            int was_standing = row->standing;
            row->standing++;
            if (!was_standing && g_err_hook) g_err_hook(msg, filename, RESULT_PAGE_ERROR_STANDS);
            return;
        }
    }
    /* Routes between the two declared answers. Announced before the row is committed, so an allocation failure
       below loses the row and not the announcement. */
    if (g_err_hook) g_err_hook(msg, filename, RESULT_PAGE_ERROR_STANDS);
    if (g_errs_n >= g_errs_cap) {
        int c = g_errs_cap ? g_errs_cap * 2 : 8;
        PageErrorRow *a = realloc(g_errs, (size_t)c * sizeof(*g_errs));
        /* One allocation, so there is no half-grown row; a lost diagnostic is not worth failing a run over. */
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

/* Takes one occurrence back — result.h states the algorithm this serves and why a no-op is a positive answer. */
void result_page_error_retract(const char *msg, const char *filename) {
    PageErrorRow *row;
    if (!msg || !*msg) return;   /* the same description `result_page_error` declines to record */
    DCHECK(filename != NULL,
           "a page error was retracted with no throw-site field at all — the retraction keys on the same "
           "(message, throw site) pair the report does, so a null one cannot name the row it means to take "
           "back and would silently retract nothing");
    row = errs_find(msg, filename);
    /* No row, or nothing standing: §8.1.4.7 step 4.1.4 appends promises step 4.1.3 declined to report (the
       append is gated on [[PromiseIsHandled]], the report on notCanceled), so a page that cancels
       `unhandledrejection` is still owed a `rejectionhandled` this console never reported. Nothing to take
       back, and a row minted here would claim an error this engine never named. */
    if (!row || !row->standing) return;
    row->standing--;
    row->retracted++;
    /* The pair's latch, falling: a correction printed while another occurrence stands withdraws a true line. */
    if (!row->standing && g_err_hook) g_err_hook(msg, filename, RESULT_PAGE_ERROR_RETRACTED);
}

/* Describes a thrown value without running any page code (see result.h). Every slot on a thrown value is the
   page's to define, so it routes to the engine's describer `JS_DiagCString` (also used by §8.1.4.6's
   `extract_error_information`), which walks the prototype chain for data properties, falls back to the
   constructor's name, and answers `[object Class]` — never invoking an accessor or a Proxy trap. This function
   adds the concolic form, DOMException's internal slots and the throw site's frames.
   HTML §8.1.4.6 "Runtime script errors" extract error information leaves message and position
   "implementation-defined values derived from exception", so the choice is argued from what a reader can use. */
void result_error_text(JSContext *ctx, JSValueConst err, char *out, size_t outsz) {
    char *buf = out;
    JSValue name, msg, stk;
    const char *ns = NULL, *ms = NULL, *ss = NULL, *what = NULL;
    char *owned = NULL;
    int n;

    DCHECK(out != NULL && outsz >= 64,
           "a thrown value was described into no buffer, or into one too small to hold a name and a message — "
           "the description is truncated at the caller's size and every caller must give it room to be one");
    *out = 0;
    /* A thrown concolic is named by its shape and source: the engine's describer would call it
       `[object Function]`, since its class is callable. Answered here and not in JS_DiagCString because that
       string is also §8.1.4.6's page-visible ErrorEvent `message`, which a solver shape must never reach.
       Both fields are the record's stored strings, so nothing runs. */
    if (concolic_is(err)) {
        const char *shape = concolic_shape_c(err), *src = concolic_src_c(err);
        /* The record's one writer stores `shape ? shape : "{}"` and `src ? strdup(src) : NULL`, so a shape is
           never NULL and a source legitimately is — a value derived from no minted source. */
        DCHECK(shape != NULL,
               "a concolic reached the page-error describer with no stored shape — its one writer defaults the "
               "shape to \"{}\", so a NULL here is a record built outside it");
        if (src) snprintf(buf, outsz, "an unknown value was thrown: %s (source %s)", shape, src);
        else     snprintf(buf, outsz, "an unknown value was thrown: %s (no source)", shape);
        return;
    }
    /* A DOMException is asked first: its `name` and `message` are Web IDL accessors on the prototype, so the
       describer stops at the getter and answers only "DOMException", while the internal slots hold the name
       (e.g. `SyntaxError`) and the message. A stored value, never an operation. */
    name = JS_GetDOMExceptionName(ctx, err);
    msg  = JS_GetDOMExceptionMessage(ctx, err);
    if (JS_IsString(name)) ns = JS_ToCString(ctx, name);
    if (JS_IsString(msg))  ms = JS_ToCString(ctx, msg);
    if (ns && ms && *ms) n = snprintf(buf, outsz, "%s: %s", ns, ms);
    else if (ns)         n = snprintf(buf, outsz, "%s", ns);
    else {
        what = JS_DiagCString(ctx, err, &owned);
        /* A NULL gets a positive statement, never an empty one, because `result_page_error` drops an empty
           description. A Symbol is the one cause nameable here (the only primitive whose ToString throws); an
           allocation failure or a revoked Proxy is indistinguishable without reading the context's exception,
           which is not this function's to read or clear, so the text claims neither. */
        if (what)                  n = snprintf(buf, outsz, "%s", what);
        else if (JS_IsSymbol(err)) n = snprintf(buf, outsz, "a Symbol was thrown");
        else                       n = snprintf(buf, outsz, "a thrown value whose description this engine "
                                                            "could not compose");
    }
    /* Where it threw. An Error keeps its stack in the [[ErrorData]] slot behind an accessor on Error.prototype,
       and calling the getter would run page code (Error.prepareStackTrace) outside any flow, so
       JS_GetErrorStackString reads the slot directly. */
    stk = JS_GetErrorStackString(ctx, err);
    if (JS_IsString(stk)) ss = JS_ToCString(ctx, stk);
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
    if (what) JS_DiagFreeCString(ctx, what, owned);
    JS_FreeValue(ctx, name);
    JS_FreeValue(ctx, msg);
    JS_FreeValue(ctx, stk);
}
/* Named residual: a thrown object is described by its kind, never its shape.
   Not covered: only `name`, `message` and `constructor` reach the description, so `{status,body}` and
   `{code,detail}` both read `Object` (or `[object Object]` with no prototype).
   Next diff: `JS_DiagCString` appends the own property keys — keys, never values, since describing a value is
   a coercion that runs page code; in the engine, so every C embedder shares one describer.
   Absence shows as an `enginePageErrors` row whose whole message is a kind (no colon, message or frames) at a
   count above one, naming no property. */

/* The (message, throw site) pair a thrown value keys on, derived once for the report, the retraction and the
   exploration mark so all three find the same row. */
static void page_error_key(JSContext *ctx, JSValueConst err, char *buf, size_t bufsz, char **at) {
    /* §8.1.4.6 "Runtime script errors"'s throw site, asked of its owner (core/events/report_exception.h)
       rather than re-parsed from the frames `result_error_text` appends: one parser of one backtrace. */
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

/* Marks the pending exception as this engine's own exploration (see result.h). Runs at the raise and keys
   through `page_error_key` on the very value §8.1.4.6 will later be handed, whose backtrace is already
   captured, so the recorded pair is the pair its row will carry. Runs no page code. */
JSValue result_explored_throw(JSContext *ctx) {
    char buf[320];
    char *at;
    JSValue e;

    DCHECK(JS_HasException(ctx),
           "a site declared its throw to be this engine's own exploration with NO exception pending — the "
           "declaration is about the value the caller has just raised, so with nothing raised there is nothing "
           "to classify and the next page error to arrive would be the one this had marked as the engine's");
    e = JS_GetException(ctx);
    page_error_key(ctx, e, buf, sizeof buf, &at);
    explored_add(buf, at);
    free(at);
    /* Re-raised unchanged: JS_Throw takes this reference and returns JS_EXCEPTION, so the page sees the identical
       value and backtrace. */
    return JS_Throw(ctx, e);
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

/* Releases this component's eight slots; solver/engine.c's solver_agent_free calls it. A malloc'd row
 * appears in neither of JS_FreeRuntime's censuses, so nothing else would report a leak here.
 * `g_err_route_declared` is reset too: carried past its agent it would let the next host pass
 * `result_page_error`'s route DCHECK without ever declaring a route.
 * Not declared to core/agent_state.h: `platform_check_agent_state` requires a row of core/platform.c's list,
 * which this component has none of, and `agent_state_check_released` runs at the end of `platform_agent_free`,
 * before `solver_agent_free` releases these slots. This half's release column is `solver_agent_free`.
 * All eight or none: `errs_json_array` walks `g_errs[0 .. g_errs_n)` and dereferences both halves of every
 * row, so freeing the array but keeping the count is a use-after-free. Free, assert, then reset the handles. */
void result_free(void)
{
    /* Every host declares the route in its agent init, unconditionally, beside `platform_agent_init`. */
    DCHECK(g_err_route_declared,
           "the page-error console was released in an agent that never declared where an uncaught page error "
           "is READ. A host states that once, at its agent bring-up, so this is either a SECOND release of one "
           "agent's console — the first reset this latch, and the rows it would free are already given back — "
           "or a teardown reached without the bring-up that answers for this component at all");

    for (int i = 0; i < g_errs_n; i++) {
        DCHECK(g_errs[i].msg != NULL && g_errs[i].at != NULL,
               "a page-error row reached the release owning only half its key — `result_page_error` frees and "
               "ABANDONS a half-allocated pair rather than committing one, so a row visible here with a null "
               "half was committed by some other path, and it is the row the dedupe compares equal on "
               "whichever half did allocate");
        free(g_errs[i].msg);
        free(g_errs[i].at);
    }
    free(g_errs); g_errs = NULL; g_errs_n = g_errs_cap = 0;

    for (int i = 0; i < g_expl_n; i++) {
        DCHECK(g_expl[i].msg != NULL && g_expl[i].at != NULL,
               "an exploration declaration reached the release owning only half its key — `explored_add` frees "
               "and ABANDONS a half-allocated pair for the reason the row beside it does, so a null half here "
               "is a declaration filed under a key nobody made");
        free(g_expl[i].msg);
        free(g_expl[i].at);
    }
    free(g_expl); g_expl = NULL; g_expl_n = g_expl_cap = 0;

    /* The handles last. Resetting the latch makes a report after release loud: `result_page_error` would
       otherwise `realloc(NULL, …)` a console for an ended agent; instead its route DCHECK fires. */
    g_err_hook = NULL;
    g_err_route_declared = 0;
}

/* Appends raw text (a delimiter this file controls); page-supplied text goes through errs_append. */
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

/* JSON-escapes a page-supplied string (its own message text, so it can hold anything). */
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

/* One entry per distinct message, the console shape readers expect (core/events/report_exception.c calls it
   a developer console). Rows are keyed per (message, throw site), so this is where the two shapes part: the
   per-script fact is carried by the stream (result.h).
   Three arrays, each per message across every row carrying it. `pageErrors`: some occurrence stands.
   `pageErrorsRetracted`: none stands and at least one was withdrawn because HTML §8.1.6.4 step 7.4 reported
   the rejection handled; disjoint from the first, since a message still standing somewhere did go wrong. A
   message in neither was never recorded. A retracted message is still a capability the page reached for, so
   the message is kept, not a count.
   `pageErrorsExplored` is orthogonal: messages this engine minted on its own exploration (a browser component
   forking completions over unknown input and reaching a spec step whose answer is a throw), asked of
   `result_page_error_explored` rather than a row field. Evidence, still reported, but not a page error, so a
   consumer renders it apart. A declared pair with no row (an exploration throw the page caught) is in no
   array, since this walks what was reported. */
typedef enum {
    /* the page raised it and nothing took it back */
    ERRS_STANDING,
    /* named and then withdrawn, standing nowhere — disjoint from the above */
    ERRS_RETRACTED,
    /* whose throw it was — orthogonal to both, so a message may be in this and in one of them */
    ERRS_EXPLORED
} ErrsArray;

static char *errs_json_array(ErrsArray which) {
    char *b = NULL; size_t cap = 0, len = 0;
    int emitted = 0;
    errs_raw(&b, &cap, &len, "[");
    for (int i = 0; i < g_errs_n; i++) {
        int seen = 0, stands_somewhere = 0, retracted_somewhere = 0, explored_somewhere = 0;
        int want = 0;   /* the answer for a value outside ErrsArray — see the switch below */
        /* Per message, across every row carrying it: a message standing at one site is not retracted because
           another site withdrew it. */
        for (int j = 0; j < g_errs_n; j++) {
            if (strcmp(g_errs[j].msg, g_errs[i].msg)) continue;
            if (j < i) seen = 1;
            if (g_errs[j].standing) stands_somewhere = 1;
            if (g_errs[j].retracted) retracted_somewhere = 1;
            /* Asked with the row's own pair, since the declaration is keyed per (message, throw site). */
            if (result_page_error_explored(g_errs[j].msg, g_errs[j].at)) explored_somewhere = 1;
        }
        if (seen) continue;
        /* Every row stands for at least one occurrence: minted at standing 1, and a retraction moves an occurrence
           between the counters, so their sum is invariant and the two arrays above stay a partition. */
        DCHECK(g_errs[i].standing || g_errs[i].retracted,
               "a page-error row stands for no occurrence at all — the row is minted at standing 1 and the "
               "retraction only MOVES an occurrence to the retracted counter, so a row at zero on both was "
               "created by a path that never reported anything, and the two arrays this file calls a partition "
               "would silently stop being one");
        /* No `default:` arm, so `-Wswitch` (in `-Wall`) names a missing arm when ErrsArray grows. `want = 0` above
           covers an out-of-range cast, unreachable here (the enum is static and callers pass literals), by
           emitting an empty array rather than every message. */
        switch (which) {
        case ERRS_STANDING:  want = stands_somewhere; break;
        case ERRS_RETRACTED: want = !stands_somewhere && retracted_somewhere; break;
        case ERRS_EXPLORED:  want = explored_somewhere; break;
        }
        if (!want) continue;
        if (emitted++) errs_raw(&b, &cap, &len, ",");
        errs_raw(&b, &cap, &len, "\"");
        errs_append(&b, &cap, &len, g_errs[i].msg);
        errs_raw(&b, &cap, &len, "\"");
    }
    errs_raw(&b, &cap, &len, "]");
    return b ? b : strdup("[]");
}

/* Every composer below is sized by what it writes: solver/compose.h's `composef` measures and allocates, so no
   call site here has a buffer or a byte count. A fit assert only fires on a document wide enough to reach the
   end of a buffer, so a hand count hides inside its own slack. */

/* The ordering, composed (see result.h). It decides nothing: it reads flow_wfq_census and renders it;
   solver/flow.h states each row's reading, and the format string notes the readings that span rows.
   `valMax - valMin` is read against 1.0, the optimism term's whole range: wider, and the bonus can no longer
   reorder the frontier's ends. `families` 1 makes `svcFamMin == svcFamMax` an identity of the structure.
   The job rows split the cold line's `jobs` total by what each job waits on — the host, the member finishing
   its own program (HTML §8.1.4.4 "Calling scripts" clean up after running script step 3), or rank — and only
   rank is the WFQ's to move, so a zero job count is charged to the ordering only through `jobsReady`. They
   count jobs and a fork byte-copies its parent's queue, so `(jobsReady + jobsFramed) / members` is the
   per-member depth; `_jobsQueued` on the work line counts queue operations.
   `jobs == jobsOwed + jobsFramed + jobsReady` is arithmetic within one sample, not asserted: cold_census sums
   `flow_job_pending` over `flow_at` (`g_flows[i]`) and flow_wfq_census partitions the same accessor over the
   same range, and result_json composes the two adjacently, so the assert could not fail — and a second
   census would raise `g_scan_runs[FLOW_SCAN_CENSUS]`. `framed == members - memUnframed` likewise. It becomes
   assertable when cold_census stops reaching its total that way. */
/* The kind of every row this composer publishes, read by testing/census_rows.js from source; the four kinds
   and the rules the reader enforces are stated above result_cold_json. Rows folded by this census's own walk
   are gauges at one instant; each free function beside them (scan pair, preempt ask, rival-miss partition,
   member-key and index checks, epoch pair, starvation and plateau counters, arrivals and departures, credit
   triple, work total, rank changes) states lifetime at its declaration.
   `vt` is a high-water mark: flow.c keeps the larger value, asserting it is the maximum coordinate any account
   stands at, so it plateaus and is not a ceiling.
   Keys spelling `Life` on the branch rows are gauges: the suffix names the horizon of the per-bucket quantity,
   but the row is an extremum or sum over whichever buckets the walk reached, so it may fall (solver/flow.h).
   The burn split follows the asserted identity `brUsLifeSum + brRetiredUsLife == chargedUsLife`: a bucket whose
   subtree wholly departs is freed and its receipt folded into the retired total, so `chargedUsLife` and
   `brRetiredUsLife` only grow, while `brUsLifeSum`, `brHeldUsLife` and `brEmptyUsLife` fall by a departed
   bucket's receipt; a live arm's share is taken against `brHeldUsLife`.

   @kinds-of wfq
   @kind gauge: members jobsReady jobsFramed jobsOwed jobWGap jobsReadyTask jobsReadyMicro memUnframed visZero
   @kind lifetime: picksLifetime picksDeparted unframedPicksLifetime readyPicksLifetime
   @kind lifetime: taskHeldDelivLifetime taskHeldSeqLifetime taskArmOlderLifetime taskArmNoRowLifetime
   @kind maximum: vt
   @kind gauge: valMin valMax valTop valZero valArrived valUnplaced selfEmit unrun
   @kind gauge: neverPicked neverPickedGap neverPickedAtTop picksLive picksMax
   @kind gauge: svcMax svcMin svcFamMax svcFamMin families silPhases silCarry
   @kind gauge: visMin visMax topSvc topSvcFam topForgiven nonrewardMax
   @kind gauge: branches brLiveMax brLiveMin brLiveSum brDepthMax brFanMax brFanSum brFanDepth
   @kind gauge: brBornLifeMax brBornLifeMin brCrowdLive brCrowdBornLife brCrowdUsLife
   @kind gauge: brMinterLive brMinterGoneLife brMinterUsLife brUsLifeMax brUsLifeMin
   @kind gauge: brUsLifeSum brHeldUsLife brEmptyUsLife
   @kind gauge: cands candUnrun candSvcMax candDecMax decMax distMax wTop wMin candWMax
   @kind gauge: delivReady delivFramed delivOwed delivWGap delivWGapVis wTopVis
   @kind gauge: curDeep curDeepLive curDeepWGap epochAwayLive epochAwayWalk
   @kind lifetime: brRetiredUsLife chargedUsLife
   @kind lifetime: scanNextRuns scanNextWeights scanRivalRuns scanRivalWeights
   @kind lifetime: scanOtherRuns scanOtherWeights scanCensusRuns scanCensusWeights
   @kind lifetime: preemptAsksLifetime rivalMissGen rivalMissCur rivalMissBoth
   @kind lifetime: keyArmedLifetime keyStaleGenLifetime keyFirstSeenLifetime keyRunningLifetime
   @kind lifetime: epochRebuildLifetime epochResetsLifetime starvedPicks starvedPicksIdle
   @kind lifetime: plateauAsked plateauHeld plateauRuns plateauHeldIdle
   @kind lifetime: arrivals departures creditsOfferedLifetime creditsPaidLifetime creditsDroppedLifetime
   @kind lifetime: workDone rankChanges
   @kind lifetime: keyIndexAskedLifetime keyIndexDifferedLifetime keyIndexDifferedTieLifetime
   @kind lifetime: keyIndexDifferedStrictLifetime keyIndexBandMembersLifetime keyIndexBandWeighedLifetime
   @kind gauge: reachBodies reachBodiesRan reachSites reachSitesHit reachSitesSkipped reachSitesUnrun
   @kind gauge: reachFramed reachAheadTop reachAheadStack reachAheadTopMin reachAheadTopMax reachAheadTopSum
   @kind lifetime: reachSitesCompiledLifetime reachSitesHitLifetime
*/
char *result_wfq_json(void) {
    WfqCensus w;
    NetReachCensus nr;

    flow_wfq_census(&w);
    /* The four arming buckets in one read, since they are a partition (solver/flow.h). After the census,
       because flow_wfq_census calls flow_best — flow_pick, which classifies every member it weighs — so an
       earlier read would be one walk older than the scan rows containing it. Nothing below steps anything. */
    FlowKeyChecks kc = flow_key_checks();
    FlowIndexChecks ic = flow_index_checks();
    /* …and the partition of the preempt hook's cache misses, in one read (solver/engine.h). */
    EngineRivalMiss rm = engine_rival_miss();
    /* …and which arm of flow_step took the step of a member holding a runnable task, in one read
       (solver/engine.h states the four rows and their identity with `run-a-task`); `readyPicksLifetime` says
       the dispatch reaches a job holder, these what the ladder did there. */
    EngineLadderTaskCensus lt;

    engine_ladder_task_census(&lt);
    /* The containment that holds in both builds: every bucket is raised on the statement after a scan's own
       `g_scan_weights[why]++`, so the four never sum above the weighings. Equality is not asserted here — the
       scan totals also carry flow_pick's seed fold and the census walk — but inside flow_pick, over one loop.
       In release all four are zero and this passes vacuously. Both sides are 64-bit (solver/flow.h's
       FlowKeyChecks static assertion): a `long` wraps past 2^31 on wasm32. If it fires, read the magnitudes
       first; near 2^31 with a negative right-hand side is a width, not the identity. */
    DCHECK(kc.armed + kc.stale_gen + kc.first_seen + kc.running
               <= flow_scan_weights(FLOW_SCAN_NEXT) + flow_scan_weights(FLOW_SCAN_RIVAL)
                + flow_scan_weights(FLOW_SCAN_OTHER) + flow_scan_weights(FLOW_SCAN_CENSUS),
           "the member-key check classified more members than this instance's scans ever weighed — every "
           "bucket is raised on the statement after the scan's own weight counter, so the four are a SUBSET "
           "of those weighings by construction. A count above them is a classification reached without a "
           "weighing, which means the block has been moved off the line it is about and `keyArmedLifetime` is "
           "no longer a count of comparisons this order made");
    /* `picksLifetime == _switches` (solver/flow.h), checked within one composition: `g_picks_total` and
       `g_switches` are raised in engine_sched_step's one `best != cur` block (flow_credit_pick's only caller),
       neither is reset with an agent, and composition steps nothing. Across two documents they may differ. A
       break is a second writer of one of them. */
    DCHECK(w.picks_lifetime == (int64_t)engine_switch_count(),
           "the scheduler's lifetime dispatch count and its context-switch count disagree WITHIN ONE "
           "COMPOSITION — flow_credit_pick has exactly one caller and engine_sched_step raises `g_switches` in "
           "the same straight-line block, so these are one event counted twice and nothing between the two "
           "reads can step the engine. One of them has acquired a writer that is not that block, and `_wfq`'s "
           "`picksLifetime` is about to be published beside a `_switches` it is defined to equal");
    /* `starvedPicksIdle` is a subset of `starvedPicks`: flow_pick raises both under one condition at one line,
       the subset behind a further predicate. Asserted because `starvedPicks - starvedPicksIdle` is published as
       a population; a break is a second writer. */
    DCHECK(flow_starved_picks_idle() <= flow_starved_picks(),
           "the scheduler reports MORE starved dispatches in which the re-dispatched member had nothing to "
           "continue than starved dispatches altogether — these are raised under one condition at one line in "
           "flow_pick, the second behind a further predicate, so the subset has acquired a writer that is not "
           "that line and `starvedPicks - starvedPicksIdle` is about to be published as a negative population");
    /* `starvedPicks / picksLifetime` is a share of dispatches: flow_pick raises it only where `best != seed`,
       the displacement engine.c credits flow_credit_pick for, so the containment holds by construction. A
       per-scan raise would count the value yield retaining an incumbent as starvation. */
    DCHECK(flow_starved_picks() <= w.picks_lifetime,
           "the scheduler reports MORE dispatches that passed over a never-run member than dispatches — "
           "flow_pick raises the starved count only where the pick DISPLACES the incumbent, which is exactly "
           "the branch engine.c credits flow_credit_pick in, so this cannot exceed unless one of the two has "
           "acquired a second writer; `starvedPicks / picksLifetime` is about to be published as a share "
           "above 1, which is the reading that says the two rows are counting different events again");
    /* The plateau identities: flow_pick raises the four in one evaluation, each subset inside its population's
       own `if`. Asserted because every reading of these rows is a quotient (`plateauHeld / plateauRuns` is a
       depth that cannot be below one); a break is a second writer. */
    DCHECK(flow_plateau_held() <= flow_plateau_asked(),
           "the scheduler reports MORE dispatch scans on which the incumbent kept the thread over a level "
           "never-run member than scans on which that comparison was makeable at all — the two are raised in "
           "one evaluation in flow_pick, the second inside the first's own condition, so the population has "
           "acquired a writer that is not that line and `plateauHeld / plateauAsked` is about to be published "
           "as a share above 1");
    DCHECK(flow_plateau_runs() <= flow_plateau_held(),
           "the scheduler reports MORE maximal plateau stretches than the retentions they are made of — a run "
           "is counted only on a scan that also raised a retention, so this is a second writer of one of them, "
           "and `plateauHeld / plateauRuns` is about to be published as a mean DEPTH below one, which would "
           "read as a queue rotating faster than it holds");
    DCHECK(flow_plateau_held_idle() <= flow_plateau_held(),
           "the scheduler reports MORE retentions in which the incumbent had nothing to continue than "
           "retentions altogether — the subset is raised behind a further predicate inside the superset's own "
           "`if`, so this is a second writer, and the remainder `plateauHeld - plateauHeldIdle` is about to be "
           "published as a negative population of incumbents that were finishing work");
    /* `scanRivalRuns` has a denominator: engine.c raises `preemptAsksLifetime` at the top of its preempt policy
       and calls flow_rival_of (its only caller) from the rescan branch below, so rescans are a subset of
       consultations. This spans two files, so it is the one check here that sees a writer on the other side of
       a header. */
    DCHECK((long long)flow_scan_runs(FLOW_SCAN_RIVAL) <= (long long)engine_preempt_asks(),
           "the scheduler reports MORE preempt-hook rescans than consultations of the preempt policy — the "
           "rescan branch is inside that policy and runs after it raises its own count, and flow_rival_of has "
           "no other caller, so one of the two has acquired a writer that is not that hook; "
           "`scanRivalRuns / preemptAsksLifetime` is about to be published as a cache miss rate above 1");
    /* …and the three miss arms partition those rescans: each is raised inside the rescan branch under the same
       `cur` test that decides whether flow_rival_of runs, so the sum is the walk counted twice. Asserted here,
       where the partition (engine.c) and the total (flow.c) are in one hand. All four counters are raised in
       every build, so this holds in release too. */
    DCHECK(rm.gen + rm.cur + rm.both == (uint64_t)flow_scan_runs(FLOW_SCAN_RIVAL),
           "the preempt hook's cache-miss partition does not sum to the rescans those misses bought — the "
           "three arms are raised inside the rescan branch under the same `cur` test that decides whether "
           "flow_rival_of is called, so they are that walk counted a second time. A sum BELOW the total is an "
           "arm that has been moved off the line that walks, or a second caller of flow_rival_of; a sum ABOVE "
           "it is a miss counted where no walk followed. Either way `rivalMissGen`, `rivalMissCur` and "
           "`rivalMissBoth` are about to be published as a partition of a number they are not a partition of, "
           "and the reading they exist for — which invalidator a rescan would have to lose to not happen — "
           "is no longer a question these rows can answer");
    /* An empty frontier says so and nothing else (result.h states why the term rows are absent rather than
       zero). `qjs_result` composes this shape, since a session that drains or parks leaves no members. */
    if (w.members == 0)
        return composef("{\"members\":0}");
    /* A member exists, so flow_new named the register's context, whose runtime holds every member's bodies. */
    net_reach_census(pending_ctx(), &nr);
    DCHECKF(nr.members == w.members, "the reach census walked %ld members and the WFQ census %ld, in one "
            "composition that steps nothing", nr.members, w.members);
    DCHECKF(nr.framed <= (long)(w.members - w.mem_unframed), "the reach census read a body frame in %ld "
            "members and the WFQ census found a frame in only %ld", nr.framed, (long)(w.members - w.mem_unframed));
    return composef(
                     "{\"members\":%ld,\"valMin\":%.1f,\"valMax\":%.1f,\"valTop\":%.1f,"
                     /* …and the clock those three are positions on. `vt` is the frontier's virtual time
                        (solver/flow.h): the queue coordinate of the item in service, where every never-served
                        account stands. A `valMin` far below `vt` is accounts left behind by a clock that moved on;
                        a `vt` above the whole band is a clock no member stands at. */
                     "\"vt\":%.1f,"
                     /* `valZero` is the ceiling population; `valArrived` the members that emitted nothing, which
                        since a from-baseline flow enters at the frontier's virtual time includes @S candidate
                        sessions holding what the leader held. A large `valArrived` at the floor of the band is the
                        arrival coordinate being left behind. `valUnplaced` is the subset never given the thread,
                        whose coordinate is still a reading of `vt`: large and far below `vt` is a placement
                        defect; zero with the same floor is served accounts out-earned, the bandit working.
                        `valArrived - valUnplaced` is served and still carrying nothing. `selfEmit` asks the same
                        of one member rather than its account. */
                     "\"valZero\":%ld,\"valArrived\":%ld,\"valUnplaced\":%ld,\"selfEmit\":%ld,"
                     "\"unrun\":%ld,"
                     "\"neverPicked\":%ld,\"neverPickedGap\":%.3f,"
                     /* …and how wide the plateau is. `neverPickedGap` reads 0.0 for the best of a wide tie and for
                        a lone near-miss, and 0.0 is also the expected reading of a healthy sweep (the pick returns
                        one of N tied maxima), so read `neverPicked` as a series. This is N. A gauge, `<=
                        neverPicked` and nonzero exactly when `neverPickedGap` is 0.000, both asserted in
                        flow_wfq_census. Beside `visZero` and `jobsFramed` it says which within-family separator
                        flattened the order: the optimism bonus (frozen while a member is inside a program, since
                        flow_credit_visit asserts `frame == NULL`) or the own silence (forgiven account-wide at
                        any arm's emission). */
                     "\"neverPickedAtTop\":%ld,"
                     /* …and where the dispatches went (solver/flow.h has the three states). `picksLive / (members
                        - neverPicked)` near 1: the thread reached a fresh member nearly every time and the
                        frontier is outgrowing one thread; well above 1: the order re-serves members ahead of
                        ones it never served; `picksMax` near `picksLive`: one member holds the thread.
                        `picksLive` and `picksMax` are gauges and may fall; `picksLifetime` is the only counter
                        and must equal `_switches` (asserted above). */
                     "\"picksLive\":%lld,\"picksMax\":%lld,\"picksLifetime\":%lld,"
                     /* …and the half of that counter the frontier no longer holds, so `picksLive + picksDeparted
                        == picksLifetime` is a partition (asserted at the end of flow_wfq_census) rather than a
                        gauge subtracted from a counter. It also separates retiring from selling in `departures`
                        (engine.c asserts `finished + sold + teardown == departures`): flow_finish is reached only
                        from the `FLOW_STEP_DONE` arm, whose member was switched in by the block that credits
                        flow_credit_pick, so a finish costs a dispatch. `departures > 0` with this row at zero
                        proves nothing retired; a nonzero here proves nothing. */
                     "\"picksDeparted\":%lld,"
                     /* The seven notch rows (`svcMax`, `svcMin`, `svcFamMax`, `svcFamMin`, `candSvcMax`, `topSvc`,
                        `topSvcFam`) are quotients `<thread time> / FLOW_SERVICE_US` (flow.c's flow_service_notch
                        and flow_family_notch), so one increment is one quantum of unforgiven silence — never a
                        dispatch, step or microsecond; multiply by FLOW_AGE_QUANTUM for the points the weight
                        charges. Dispatch counts are the `picks*` rows. Gauges: silence since the account's last
                        forgiveness, which flow_credit_emit zeroes family-wide; `topForgiven` counts those. */
                     "\"svcMax\":%lld,\"svcMin\":%lld,\"svcFamMax\":%lld,\"svcFamMin\":%lld,\"families\":%ld,"
                     /* …and what asking the order costs. The notches drop their remainder, but the aging term
                        divides the sum of two, whose quotient carries a carry bit. `silPhases` is how many
                        distinct remainders the frontier stands on and `silCarry` how many members are past the
                        carry boundary; between two generation bumps only that carry moves, and members sharing a
                        remainder flip together. `silPhases: 1` would mean nothing reorders between bumps, but in
                        the archived corpus it occurs only at `members == 1` (solver/flow.h). A statement about
                        what a sub-linear ask must index; no term of flow_weight reads either. `silPhases` counts
                        keys and `silCarry` is a gauge; neither may be differenced. */
                     "\"silPhases\":%ld,\"silCarry\":%ld,"
                     /* The fork-subtree scope between member and family. A bucket is a top-level arm (forked
                        directly off a family root); deeper branches sum into it (flow.c's residual at FlowAcct
                        `up`). `branches`, `brLive*` and `brDepthMax` are gauges. `brLiveMax / members` is how
                        concentrated the frontier is; `brLiveMin` is 0 or 1 while a family root stands.
                        `brLiveSum == members` is asserted in flow_wfq_census. */
                     "\"branches\":%ld,\"brLiveMax\":%ld,\"brLiveMin\":%ld,\"brLiveSum\":%ld,"
                     /* The branch term is `1.0 / sub_born`, so `1/brBornLifeMin - 1/brBornLifeMax` is its span.
                        `brBornLifeMax` beside `brLiveMax` separates a bucket that mints unboundedly from one
                        holding a lot now. Mint extrema are over buckets with a live member. */
                     "\"brBornLifeMax\":%ld,\"brBornLifeMin\":%ld,"
                     /* `brCrowd*` are the bucket owning `brLiveMax`; the other maxima may belong to different
                        buckets (the burn maximum is often a departed root holding boot's burn). Compare
                        `brCrowdLive / members` with `brCrowdUsLife / brHeldUsLife`: at par, branching turned into
                        thread one for one; near zero, an arm already demoted that nothing retires; above, an
                        ordinary monopolist. `brCrowdUsLife` 0 with `brCrowdLive > 0` is starvation, not an
                        unobserved bucket. `brCrowdLive == brLiveMax` is asserted in flow_wfq_census. */
                     "\"brCrowdLive\":%ld,\"brCrowdBornLife\":%ld,\"brCrowdUsLife\":%lld,"
                     /* …and the same three for the arm that has taken the most arms (selected by `brBornLifeMax`,
                        where the crowd is selected by `brLiveMax`; `sub_born = live + sub_gone` makes them one
                        bucket only while nothing has departed). An arm forking at every position of an unknown
                        length and letting each finish mints unboundedly and stands narrow. It carries the smallest
                        branch bonus; compare `brMinterUsLife / brHeldUsLife` with `brMinterLive / members` as for
                        the crowd. `brMinterGoneLife` (shed count) is published nowhere else. The live count is a
                        gauge; the shed count and burn are per-bucket lifetime counts read at one instant, since
                        the bucket selected moves. `brMinterLive + brMinterGoneLife == brBornLifeMax` is asserted
                        in flow_wfq_census; `brMinterUsLife == brCrowdUsLife` says both selectors name one arm. */
                     "\"brMinterLive\":%ld,\"brMinterGoneLife\":%ld,\"brMinterUsLife\":%lld,"
                     /* Burn rows are over every bucket, in the quantum's own measure (@QUANTUM's `isCpu`), so a
                        raw total is quoted with that line; `brUsLifeMax / chargedUsLife` is how concentrated the
                        thread is. Asserted in flow_wfq_census: `brHeldUsLife + brEmptyUsLife == brUsLifeSum` and
                        `brUsLifeSum + brRetiredUsLife == chargedUsLife`. */
                     "\"brUsLifeMax\":%lld,\"brUsLifeMin\":%lld,"
                     "\"brUsLifeSum\":%lld,\"brHeldUsLife\":%lld,\"brEmptyUsLife\":%lld,"
                     "\"brRetiredUsLife\":%lld,\"chargedUsLife\":%lld,"
                     "\"brDepthMax\":%d,"
                     /* …and which fork inside a bucket did the minting. A fan is the live members forked directly
                        off one non-root node (solver/flow.h); `brFanDepth` 1 is the top-level arm minting the
                        crowd itself, depth D is minting D-1 levels below it. Gauges. `brFanMax / brFanSum` is how
                        concentrated the deep forking is; `brFanMax: 0` says every fork is top-level.
                        `brFanMax <= brLiveMax`, asserted in flow_wfq_census. */
                     "\"brFanMax\":%ld,\"brFanSum\":%ld,\"brFanDepth\":%d,"
                     "\"visMin\":%lld,\"visMax\":%lld,\"visZero\":%ld,"
                     "\"cands\":%ld,\"candUnrun\":%ld,\"candSvcMax\":%lld,\"candDecMax\":%ld,\"decMax\":%ld,"
                     "\"distMax\":%.3f,\"wTop\":%.3f,\"wMin\":%.3f,\"candWMax\":%.3f,"
                     /* …and the event the two leader notches are a reading between: `topForgiven` counts
                        forgivenesses of the front account's silence. An emission zeroes both aging halves for
                        every arm of the account, collapsing the frontier into exactly tied visit tiers that
                        flow_pick sweeps one member per quantum; if so, `picksMax` tracks this and `picksLive /
                        picksMax` is the sweep depth. It belongs to whichever account leads, so it may fall on a
                        change of leader (`valTop` falling beside it). `valTop / topForgiven` is points per
                        finding. Read `topSvc`, never `topSvcFam`, for a gap: every arm of a family reads one
                        `fam_us`, which cancels out of `neverPickedGap`. A monopolising leader shows `topSvc`
                        climbing with gaps closing; a front refilled by fresh arms shows `topSvc` low or
                        sawtoothing with gaps standing. `nonrewardMax` is flow.c's FLOW_NONREWARD_MAX: `(valTop -
                        valMin) + nonrewardMax` is the largest gap a non-negative non-reward sum can produce, so a
                        larger gap is aging, not lift. */
                     "\"topSvc\":%lld,\"topSvcFam\":%lld,\"topForgiven\":%lld,\"nonrewardMax\":%.3f,"
                     "\"jobsReady\":%ld,\"jobsFramed\":%ld,\"jobsOwed\":%ld,\"jobWGap\":%.3f,"
                     /* …and the rank-ready row split by which arm of flow_step can dispatch the job: a microtask
                        is taken by the checkpoint arm above the program sequence, a task by the arm below it (the
                        `else` of `a program starts on this step`). With `jobsReady > 0` and `_jobsRun` flat, all
                        TASK says the sequence arm's exclusion holds the backlog; any `jobsReadyMicro` refutes that
                        for the jobs it counts. Neither says the jobs would have run (arms above the checkpoint, or
                        the pick). The two sum to `jobsReady`, asserted in flow_wfq_census and re-asserted by
                        engine/build.mjs for release (solver/flow.h). */
                     "\"jobsReadyTask\":%ld,\"jobsReadyMicro\":%ld,"
                     /* …and `jobsReady`'s denominator: members holding no frame, on the same walk as `members`.
                        With `jobsReady: 0`, `memUnframed: 0` sends the reader to flow_step (frames not ending),
                        and `memUnframed > 0` to where jobs are queued. */
                     "\"memUnframed\":%ld,"
                     /* …and its lifetime half: dispatches the unframed state ever received, raised in
                        flow_credit_pick beside `picksLifetime` on the same flow_stack_empty, so contained in it
                        (asserted in flow_wfq_census and again by engine/build.mjs for release). With `jobWGap: 0`,
                        zero here with `picksLifetime` large is the order ranking ready holders first while the
                        dispatch does not take them; above zero is `wTop` not being what the dispatch compares.
                        With no dispatch at all it is 0 for a third reason (solver/flow.h). */
                     "\"unframedPicksLifetime\":%lld,"
                     /* …and the subset of those dispatches reaching a member with a rank-ready job, which turns
                        that bound into a measurement. With `jobsRun` flat: 0 here and the row above nonzero sends
                        the reader to flow_pick; above 0 says flow_step declines the job at a higher arm, and
                        `jobsReadyTask`/`jobsReadyMicro` are next. Raised inside the unframed count's `if` in
                        flow_credit_pick under the ready arm's three conjuncts; contained by construction and
                        asserted in flow_wfq_census (solver/flow.h). */
                     "\"readyPicksLifetime\":%lld,"
                     /* …and what the ladder did with those dispatches. The first two count the arms above the task
                        arm reached with a task runnable; the last two partition the task arm's own step count
                        (asserted in solver/engine.c). Read against `run-a-task` on @COLD; solver/engine.h states
                        what each sizes and what none reaches (the three arms above delivery, and the depth of the
                        member's own queue). Lifetime counts beside gauges: one of these falling is a second
                        writer. */
                     "\"taskHeldDelivLifetime\":%ld,\"taskHeldSeqLifetime\":%ld,"
                     "\"taskArmOlderLifetime\":%ld,\"taskArmNoRowLifetime\":%ld,"
                     "\"delivReady\":%ld,\"delivFramed\":%ld,\"delivOwed\":%ld,\"delivWGap\":%.3f,"
                     /* …and which term that gap is at the two members it is between: the optimism operand, which
                        has no row elsewhere, read only where `delivReady` is nonzero (solver/flow.h). Placed after
                        the gap, not beside `visMin`/`visMax`, which are extrema over the whole frontier. */
                     "\"delivWGapVis\":%lld,\"wTopVis\":%lld,"
                     /* …and the same difference against the members furthest through the program table, joining
                        this line to `programCursors` on @COLD (solver/flow.h). `curDeepWGap` 0.000: a member at
                        the deepest row is the front of the order, so an unreached tail wants dispatches; positive:
                        the order ranks less-advanced members ahead, in the same points as `neverPickedGap` and
                        `nonrewardMax`. `curDeepLive` is the gap's population. Gauges (unlike the monotone
                        `deepest`/ `deepestLeft`). `curDeep` and `curDeepLive` repeat `programCursors`' top bucket,
                        computed by a different walk over `Flow.script_i`, so they cross-check when both share
                        `workDone`. */
                     "\"curDeep\":%d,\"curDeepLive\":%ld,\"curDeepWGap\":%.3f,"
                     /* …and what asking the order cost (solver/flow.h's FLOW_SCANS: three entries, counted apart,
                        in counts not time). Two causes of an unreached tail: too little thread for the members
                        standing, or thread spent asking. Read `scanNextWeights / steps` against `members` and
                        `scanRivalRuns` against `forks`: the dispatch loop asks once per step, the preempt hook
                        once per frontier generation. Spelled out, not nested: two quantities over three entries
                        partition no total. */
                     "\"scanNextRuns\":%ld,\"scanNextWeights\":%lld,"
                     "\"scanRivalRuns\":%ld,\"scanRivalWeights\":%lld,"
                     "\"scanOtherRuns\":%ld,\"scanOtherWeights\":%lld,"
                     /* …and what this census itself costs: flow_wfq_census weighs the frontier twice per sample,
                        once in its own walk (counted here) and once in the flow_best it calls (in `scanOther*`);
                        the engine asserts the two equal, so a sample costs twice `scanCensusWeights`. Read as
                        fractions: `scanCensusWeights / scanCensusRuns` is the mean frontier a sample paid for, and
                        against `scanNextWeights` the share of weighing spent reporting. */
                     "\"scanCensusRuns\":%ld,\"scanCensusWeights\":%lld,"
                     /* …and how often the hook was asked. The rival rescan is close to half of all frontier
                        weighing; this tells a cache that absorbs nothing (repair the cache) from a generation
                        moving as fast as the hook is consulted (the page branching). `scanRivalRuns /
                        preemptAsksLifetime` is the miss rate, distinct from rescans per rank change and from
                        per-step cost; the containment is asserted above. A lifetime counter (never reset in
                        engine.c), so it may be differenced. */
                     "\"preemptAsksLifetime\":%llu,"
                     /* …and which half of the hook's key had moved when it missed. The cache is keyed on a
                        disjunction — the frontier generation or the incumbent — and raises inside one C call
                        collapse into one miss at the next poll, so a rate alone cannot say which. A partition, not
                        three rates: `rivalMissGen` is the order having changed; `rivalMissCur` a walk for a
                        frontier whose generation stood still, which is not a frontier where nothing moved
                        (solver/engine.h; `silPhases` prices that repair); `rivalMissBoth` prices either repair,
                        since removing one invalidator buys nothing where both moved. Lifetime counts in every
                        build; their sum is asserted equal to `scanRivalRuns` above. */
                     "\"rivalMissGen\":%llu,\"rivalMissCur\":%llu,\"rivalMissBoth\":%llu,"
                     /* …and whether the dispatch walk's one assertion was ever asked. flow_pick's member-key
                        invariant (solver/flow.h's FlowKeyChecks) is a predicted absence, exempting a member on
                        three arms before comparing. `keyArmedLifetime` is the score and the other three, which
                        partition the classifications with it, are why: `keyRunningLifetime` is bounded by one
                        member per scan, `keyFirstSeenLifetime` by one per member created, and
                        `keyStaleGenLifetime` near the total means the generation moves faster than members are
                        re-weighed — vacuous rather than held. Lifetime counts of comparisons, never read against
                        `members`. All four zero beside a nonzero `scan<Entry>Weights` is a release build: the
                        check is dev-only, and the row is emitted in both builds because engine/build.mjs derives
                        its required set from this format string. */
                     "\"keyArmedLifetime\":%lld,\"keyStaleGenLifetime\":%lld,"
                     "\"keyFirstSeenLifetime\":%lld,\"keyRunningLifetime\":%lld,"
                     /* …and whether an index over that key would have answered what the comparator answered: the
                        four rows above score whether the key stands still, these whether it orders, which a
                        candidate set rests on. `keyIndexAskedLifetime` counts scans that folded the surrogate
                        against a maximum (the reachability witness); `keyIndexDifferedLifetime` the subset naming
                        a different member while the assert held, ordinary when two members tie. A zero there
                        with a large ask means the surrogate picked the same member every time. The
                        disagreement that matters is not an abort, since flow.c's flow_index_margin declares
                        the two spellings a derived distance apart; it is the partition on the next row.
                        Lifetime counts raised under APICLIENT_DEV, so two zeros first question the build. */
                     "\"keyIndexAskedLifetime\":%ld,\"keyIndexDifferedLifetime\":%ld,"
                     /* …and which mode each disagreement was. `keyIndexDifferedTieLifetime`: the surrogate read
                        the comparator's own member at its extremum, losing a distinction (flow.c shows a
                        margin-carrying candidate set answers it with flow_weight untouched).
                        `keyIndexDifferedStrictLifetime`: the member on the losing side, a real disagreement and
                        the tie-identity decision solver/flow.h reserves for the project owner. The mode compares
                        `sur_w` with the surrogate's reading of the returned member, never that member's weight.
                        They partition the row above (asserted in flow.c), so a strict count is read as a share.
                        Lifetime counts under APICLIENT_DEV; `keyIndexAskedLifetime` is the witness. */
                     "\"keyIndexDifferedTieLifetime\":%ld,\"keyIndexDifferedStrictLifetime\":%ld,"
                     /* …and what answering it would cost. The design that answers a tie edits flow_weight not at
                        all: a candidate set of every member within a derived margin of the surrogate's extremum,
                        re-compared through flow_weight, which flow.c proves contains the comparator's extremum
                        and asserts returns the same member. `keyIndexBandMembersLifetime` is how many members
                        the set admitted and `keyIndexBandWeighedLifetime` how many were tested, on the same
                        walk: their quotient is the share an index would still re-compare (near one saves
                        nothing). A large share is expected — the band holds every member tied with the maximum
                        whatever the margin (solver/flow.h) — so it is the order being tied, not a wide margin.
                        Lifetime counts summed over asks, under APICLIENT_DEV; `keyIndexAskedLifetime` is the
                        witness. */
                     "\"keyIndexBandMembersLifetime\":%lld,\"keyIndexBandWeighedLifetime\":%lld,"
                     /* The away-from-base population and what it costs an index. The `Live` rows are gauges, the
                        `Lifetime` rows counters. `epochAwayLive` is maintained incrementally at four sites in
                        solver/flow.c and summed per family at the census's family door; `epochAwayWalk` asks
                        flow_own_silence of every member. Two maintainers at one instant, so equality is asserted
                        in flow_wfq_census and checkable here; a difference is a fifth transition site.
                        `epochRebuildLifetime` is the reading: flow_credit_emit returns a family to base by moving
                        a generation, so an index over `flow_index_key` rebuilds exactly the members standing away,
                        and this sums that at each emission (a gauge sample between emissions is a lottery). Read
                        it against `scanNextWeights`, the walk an index would replace; `epochResetsLifetime` gives
                        the mean rebuild per emission. Zero resets is a run that never emitted, so read a zero
                        rebuild against it first. */
                     "\"epochAwayLive\":%ld,\"epochAwayWalk\":%ld,"
                     "\"epochRebuildLifetime\":%ld,\"epochResetsLifetime\":%ld,"
                     /* Whether a pick ever passed over a never-run member, which no gauge can answer. This counts
                        dispatches where the scan returned an already-dispatched member while a never-dispatched
                        one stood at exactly the same weight, raised only where `best != seed`, the
                        displacement engine.c credits flow_credit_pick for. A lifetime counter, read as a
                        fraction of `picksLifetime`: a handful is flow_pick's strict comparison letting the
                        incumbent keep a tie, a figure near the dispatches is the order no longer separating
                        members. On a forking page a newborn arm stands at its parent's weight, so this also
                        counts every quantum of a multi-quantum program; `starvedPicksIdle` separates those. */
                     "\"starvedPicks\":%ld,"
                     /* …and the subset in which the re-dispatched member had nothing to continue — no live frame
                        and no checkpoint owed, standing at the unit boundary of HTML §8.1.4.4 "Calling scripts"
                        clean up after running script step 3 — a pass-over with nothing necessary in it. Read this,
                        not `starvedPicks`, against `picksLifetime`. Raised at the same line under the same
                        condition, so the subset relation is one evaluation. An upper bound: the thread holder's
                        parked continuations live in the runtime during its turn (solver/flow.c). */
                     "\"starvedPicksIdle\":%ld,"
                     /* …and how deep the plateau is: the pick that keeps the incumbent over a level never-run
                        member, where the pair above counts the one that displaces it. `plateauHeld / plateauRuns`
                        is the mean consecutive scans an incumbent held the thread with someone untouched level;
                        bounded is the queue rotating as the aging term is priced for (flow.h: a tied flow hands
                        over after one quantum), while `plateauRuns` 1 beside a large `plateauHeld` is one unbroken
                        hold. `plateauAsked` is the reachability witness; zero beside nonzero `picksLifetime` is a
                        frontier that drains, and `plateauHeld / plateauAsked` is how tied the frontier is at the
                        dispatching line. Lifetime counters. `plateauHeldIdle` is an upper bound: an incumbent's
                        parked continuations are not visible from the pick, and the microtask clause is a queue
                        walk too costly here in release (flow.c's residual); read it against `jobsQueued`. */
                     "\"plateauAsked\":%ld,\"plateauHeld\":%ld,"
                     "\"plateauRuns\":%ld,\"plateauHeldIdle\":%ld,"
                     /* …and whether the order is deciding anything: the frontier's arrival and departure
                        processes. `arrivals / picksLifetime` is members minted per dispatch: below 1 the frontier
                        drains and a `neverPickedAtTop` plateau is the order failing to separate reachable members
                        (repair flow_weight); above 1 no ordering drains it. Do not read `rankChanges` for this:
                        frontier_rank_changed has many callers, most of which do not change the membership.
                        Lifetime counters; `arrivals - departures == members` (a gauge) is asserted in
                        flow_wfq_census and checkable here. */
                     "\"arrivals\":%lld,\"departures\":%lld,"
                     /* …and whether the order was ever offered anything to order by. `valTop`, `topForgiven` and
                        `selfEmit` read zero both when no detector fired and when every detection happened on host
                        time with no flow to pay (the root markup inventoried by `qjs_init`'s parse before
                        `qjs_begin` seeds the frontier). `creditsOfferedLifetime: 0` is the first;
                        `creditsDroppedLifetime > 0` with `creditsPaidLifetime: 0` the second. Lifetime counters;
                        offered == paid + dropped is asserted in flow_wfq_census. */
                     "\"creditsOfferedLifetime\":%lld,\"creditsPaidLifetime\":%lld,"
                     "\"creditsDroppedLifetime\":%lld,"
                     /* `workDone` (`engine_work_done()`) dates this census in the clock every other stream of the
                        run is cadenced by (`fixture_have_answers` samples on it, run_scheduler gates censuses on
                        it), so rows of two lines are compared only within one sample: the @H sampler fires every
                        PROBE_SAMPLE_EVERY units from zero, so its first table can be composed at `workDone` 1
                        while @COLD continues to the end. A lifetime counter: `picksLifetime` and
                        `scanCensusWeights` against it are per-unit-of-work rates. `rankChanges` is the hook's
                        rescan denominator: `scanRivalRuns / scanNextRuns` is scan work per step, not cadence,
                        since a rescan fires on a rank change or an incumbent switch (solver/flow.h). */
                     "\"workDone\":%ld,\"rankChanges\":%ld,"
                     /* Request-site reach (solver/net_reach.h; quickjs.h's JS_NetSiteCensus defines a site and
                        what it cannot see). Sites are bytecode reads of a door's entry name; `reachSites ==
                        reachSitesHit + reachSitesSkipped + reachSitesUnrun` over live bodies: `Skipped` sits in
                        a body some flow ran and `Unrun` in a body none ran.
                        The lifetime pair counts every compiled copy of a body. The member rows: `reachFramed`
                        members execute a body, `reachAheadTop` of them stand in one holding an unreached site
                        ahead of the deepest pc (`reachAheadTopMin/Max/Sum` in bytecode bytes over those; 0 when
                        none), `reachAheadStack` in any frame. Measurement only: nothing orders on it. */
                     "\"reachBodies\":%lld,\"reachBodiesRan\":%lld,\"reachSites\":%lld,"
                     "\"reachSitesHit\":%lld,\"reachSitesSkipped\":%lld,\"reachSitesUnrun\":%lld,"
                     "\"reachSitesCompiledLifetime\":%llu,\"reachSitesHitLifetime\":%llu,"
                     "\"reachFramed\":%ld,\"reachAheadTop\":%ld,\"reachAheadStack\":%ld,"
                     "\"reachAheadTopMin\":%ld,\"reachAheadTopMax\":%ld,\"reachAheadTopSum\":%lld}",
                     w.members, w.val_min, w.val_max, w.val_top, w.vt,
                     w.val_zero, w.val_arrived, w.val_unplaced, w.self_emit, w.unrun,
                     w.never_picked, w.never_picked_gap, w.never_picked_at_top,
                     (long long)w.picks_live, (long long)w.picks_max, (long long)w.picks_lifetime,
                     (long long)w.picks_departed,
                     (long long)w.svc_max, (long long)w.svc_min,
                     (long long)w.svc_fam_max, (long long)w.svc_fam_min, w.families,
                     w.sil_phases, w.sil_carry,
                     w.branches, w.br_live_max, w.br_live_min, w.br_live_sum,
                     w.br_born_max, w.br_born_min,
                     w.br_crowd_live, w.br_crowd_born, (long long)w.br_crowd_us,
                     w.br_minter_live, w.br_minter_gone, (long long)w.br_minter_us,
                     (long long)w.br_us_max, (long long)w.br_us_min,
                     (long long)w.br_us_sum, (long long)w.br_held_us, (long long)w.br_empty_us,
                     (long long)w.br_retired_us, (long long)w.charged_us,
                     w.br_depth_max,
                     w.br_fan_max, w.br_fan_sum, w.br_fan_depth,
                     (long long)w.vis_min, (long long)w.vis_max, w.vis_zero,
                     w.cand_members, w.cand_unrun, (long long)w.cand_svc_max, w.cand_dec_max, w.dec_max,
                     w.dist_max, w.w_top, w.w_min, w.cand_w_max,
                     (long long)w.top_svc, (long long)w.top_svc_fam, (long long)w.top_forgiven,
                     w.nonreward_max,
                     w.jobs_ready, w.jobs_framed, w.jobs_owed, w.job_w_gap,
                     w.jobs_ready_task, w.jobs_ready_micro,
                     w.mem_unframed, (long long)w.unframed_picks_lifetime,
                     (long long)w.ready_picks_lifetime,
                     lt.task_held_deliv, lt.task_held_seq, lt.task_arm_older, lt.task_arm_no_row,
                     w.deliv_ready, w.deliv_framed, w.deliv_owed, w.deliv_w_gap,
                     (long long)w.deliv_w_gap_vis, (long long)w.w_top_vis,
                     w.cur_deep, w.cur_deep_live, w.cur_deep_w_gap,
                     /* Weight counters are 64-bit and run counters are not: a run is raised once per scan, a
                        weight once per member per scan, and only the latter reaches 2^31. Cast rather than a PRI
                        macro, since int64_t is `long` natively and `long long` on wasm32. */
                     flow_scan_runs(FLOW_SCAN_NEXT),  (long long)flow_scan_weights(FLOW_SCAN_NEXT),
                     flow_scan_runs(FLOW_SCAN_RIVAL), (long long)flow_scan_weights(FLOW_SCAN_RIVAL),
                     flow_scan_runs(FLOW_SCAN_OTHER), (long long)flow_scan_weights(FLOW_SCAN_OTHER),
                     flow_scan_runs(FLOW_SCAN_CENSUS), (long long)flow_scan_weights(FLOW_SCAN_CENSUS),
                     (unsigned long long)engine_preempt_asks(),
                     (unsigned long long)rm.gen, (unsigned long long)rm.cur, (unsigned long long)rm.both,
                     (long long)kc.armed, (long long)kc.stale_gen,
                     (long long)kc.first_seen, (long long)kc.running,
                     ic.index_asked, ic.index_differed,
                     ic.differed_tie, ic.differed_strict,
                     (long long)ic.band_members, (long long)ic.band_weighed,
                     w.epoch_away_live, w.epoch_away_walk,
                     flow_epoch_rebuild(), flow_epoch_resets(),
                     flow_starved_picks(), flow_starved_picks_idle(),
                     flow_plateau_asked(), flow_plateau_held(),
                     flow_plateau_runs(), flow_plateau_held_idle(),
                     (long long)w.arrivals, (long long)w.departures,
                     (long long)w.credit_calls, (long long)w.credit_paid, (long long)w.credit_dropped,
                     engine_work_done(), flow_rank_changes(),
                     (long long)nr.sites.bodies, (long long)nr.sites.bodies_ran, (long long)nr.sites.sites,
                     (long long)nr.sites.hit, (long long)nr.sites.skipped, (long long)nr.sites.unrun,
                     (unsigned long long)nr.sites.compiled, (unsigned long long)nr.sites.hit_ever,
                     nr.framed, nr.ahead_top, nr.ahead_stack, nr.top_min, nr.top_max, nr.top_sum);
}

/* One row composer for the two state-kind histograms, which differ only in the side of the pair counted.
   It holds no list of its own: the bound is cow_state_kind_count() and the names cow_state_kind_name(), both
   expansions of solver/cow.h's COW_STATE_KINDS, so a capture unit added there renders with no edit here. A
   selector rather than an array, so a kind's index means something in one place only. `what` names the
   histogram in the width assert. Asserts no sum: the pair's identity is per kind (`made <= asks`), asserted
   in cow.c. */
static void cow_state_hist_json(char *buf, size_t cap, int want_made, const char *what) {
    int hi = 0, k, n = cow_state_kind_count();

    DCHECK(buf != NULL, "a COW state-kind histogram was composed into nothing");
    buf[hi++] = '{';
    for (k = 0; k < n; k++) {
        long asks = 0, made = 0;
        int w;
        cow_state_kind_stats(k, &asks, &made);
        w = snprintf(buf + hi, cap - (size_t)hi, "%s\"%s\":%ld",
                     k ? "," : "", cow_state_kind_name(k), want_made ? made : asks);
        DCHECKF(w > 0 && (size_t)w < cap - (size_t)hi,
                "the `%s` COW state-kind histogram did not fit the width its own list derives — "
                "COW_STATE_KINDS_JSON_MAX is computed from solver/cow.h's names, so a row that does not fit "
                "means a count wider than a `long`'s 20 digits or a name that reached this buffer from "
                "somewhere else", what);
        hi += w;
    }
    buf[hi++] = '}';
    buf[hi] = 0;
}

/* Which component asked: the per-site split of the `hostRec` row of `cowStateAsks`, a population only a run
   can know, so it is composed on the heap and sized from its own rows (key plus 36: quotes, colon, comma, an
   `int` line and a `long`'s widest twenty). The keys are `__FILE__` at call sites in this repository, made
   repo-relative by the build's `-ffile-prefix-map`, so they are not escaped; the DCHECK states that. A site
   never reached is absent, not 0 — an ask count makes no claim about reachability — and `{}` is the positive
   statement that no component record was reached. NULL on allocation failure, which result_swap_json passes
   on as an absent census (solver/compose.h). */
static char *cow_site_hist_json(void) {
    const CowHostRecSite *head = cow_host_rec_sites(), *s;
    size_t n = 3;            /* "{}" and the NUL */
    size_t len = 0;
    char *out;

    for (s = head; s; s = s->next) n += strlen(s->file) + 36;
    out = malloc(n);
    if (!out) return NULL;
    out[len++] = '{';
    for (s = head; s; s = s->next) {
        int w;
        DCHECKF(strcspn(s->file, "\"\\") == strlen(s->file),
                "a component-record capture site is named by a source path carrying a quote or a backslash — "
                "`%s` at line %d. These keys are written with no escaping pass because they are this "
                "repository's own paths and not a page's bytes; one that needs escaping does not make a wrong "
                "row, it makes a census that will not parse and a page that reports nothing at all",
                s->file, s->line);
        if (len > 1) out[len++] = ',';
        w = snprintf(out + len, n - len, "\"%s:%d\":%ld", s->file, s->line, s->asks);
        DCHECKF(w > 0 && (size_t)w < n - len,
                "the per-site component-record census overran the size counted from its own rows at %s:%d — "
                "the count above walks the same list this loop walks, so a row that does not fit is a key "
                "that grew between the two passes or a count wider than the twenty digits priced for it",
                s->file, s->line);
        len += (size_t)w;
    }
    out[len++] = '}';
    out[len] = 0;
    DCHECK(len < n, "the per-site component-record census overran its buffer at the closing brace — a "
                    "truncation here does not lose a row, it loses the brace, so the document that embeds it "
                    "will not parse and every finding for this page is discarded");
    return out;
}

/* What a context switch costs and what the two chains still hold, rendered from cow.c's and dom_cow.c's own
   stats (see result.h); it decides nothing. Rows are grouped below at their format lines.
   `installs`/`entries`/`worst`/`mean` are the cost of a switch: lifetime counts `installs` and `entries`
   (cow.c's `g_swap_count`/`g_swap_entries`), `worst` a high-water mark, and `mean` the division
   `entries / installs`, which is not a count and sums with nothing. `entries >= worst` whenever
   `installs > 0`. Sized by composef (solver/compose.h). */
char *result_swap_json(void) {
    long sc = 0, st = 0, sm = 0, hs = 0, he = 0, ds = 0, de = 0;
    long gc = 0, gm = 0, ac = 0, am = 0;
    long dw = 0, dsi = 0, du = 0, dn = 0;
    long df = 0, dr = 0, dss = 0;
    long roa = 0, rog = 0, ros = 0, rod = 0;
    char asks[COW_STATE_KINDS_JSON_MAX], made[COW_STATE_KINDS_JSON_MAX];
    char *sites, *out;

    cow_swap_stats(&sc, &st, &sm);
    cow_coro_swap_stats(&gc, &gm, &ac, &am);
    cow_chain_stats(&hs, &he);
    dom_cow_chain_stats(&ds, &de);
    dom_cow_site_stats(&dw, &dsi, &du, &dn);
    dom_cow_site_set_stats(&df, &dr, &dss);
    dom_cow_rendering_stats(&roa, &rog, &ros, &rod);
    cow_state_hist_json(asks, sizeof asks, 0, "cowStateAsks");
    cow_state_hist_json(made, sizeof made, 1, "cowStateMade");
    sites = cow_site_hist_json();
    if (!sites) return NULL;   /* this census is absent — composef's own arm for the same failure */
    out = composef(
                 "{\"installs\":%ld,\"entries\":%ld,\"worst\":%ld,\"mean\":%.1f,"
                 /* Retention, which the cost rows are blind to: gauges of the live frozen chains
                    (`g_seg_live`/`g_seg_entries_live`), falling whenever a segment is released — a few flows
                    holding tens of thousands of segments is a lifetime bug that reads healthy above. */
                 "\"heapSegs\":%ld,\"heapSegEntries\":%ld,\"domSegs\":%ld,\"domSegEntries\":%ld,"
                 /* Which of the page's own lines changed the document, which a delta cannot say of its author:
                    one `innerHTML` assignment and four hundred `appendChild`s are one `domSegEntries`. Lifetime
                    counts from solver/dom_cow.h's `dom_cow_site_stats` (identities asserted in dom_cow.c):
                    `domWrites == domWritesSited + domWritesUnsited` and `domSites <= domWritesSited`. A
                    `domSites` 0 is no page code writing (traffic in `domWritesUnsited`) or no writes at all.
                    `domSites` is also a high-water mark — a complete alphabet plateaus — and counts names over
                    the session, the alphabet a surface's per-flow site set would be drawn from, not surfaces.
                    It is keyed by QuickJS's JS_RunningSiteHash over page JavaScript and shares nothing with
                    `cowHostRecAsksBySite`, which names this engine's C call sites. */
                 "\"domWrites\":%ld,\"domWritesSited\":%ld,\"domWritesUnsited\":%ld,\"domSites\":%ld,"
                 /* The per-flow site sets: lifetime counts from solver/dom_cow.h's `dom_cow_site_set_stats`,
                    asserted in dom_cow.c: `domWritesSited == domSiteFolds + domSiteRepeats`,
                    `domSiteSetsSeen <= domSiteFolds` and `domSites <= domSiteFolds` (a name new to the session
                    was new to its flow), so `domSiteFolds / domSites` is how many flows ran the average site.
                    `domSiteSetsSeen` is a ceiling on surfaces, not a count: a flow passes through every prefix
                    of its set, and when a set has settled cannot be decided from inside the flow. */
                 "\"domSiteFolds\":%ld,\"domSiteRepeats\":%ld,\"domSiteSetsSeen\":%ld,"
                 /* The moment the standard names: HTML §8.1.7.3 "Processing model"'s rendering opportunity,
                    from solver/dom_cow.h's `dom_cow_rendering_stats` (asserted in dom_cow.c):
                    `renderingOpportunities <= renderingOpportunityAsks`, `renderingOpportunitiesSited <=
                    renderingOpportunities`, `renderingDigests <= renderingOpportunitiesSited + 1`. They can
                    refute the moment: opportunities near zero make it unreachable (zero asks is the rung never
                    reached, asks without grants the gate declining); digests near opportunities mean the
                    collapse buys nothing; digests well below are the collapse working. A low
                    `renderingOpportunitiesSited` means opportunities land on flows that rendered nothing (the
                    empty set). Declines are derived, not a row. Not bounded by `domSiteSetsSeen`: a flow at the
                    empty set has never folded. */
                 "\"renderingOpportunityAsks\":%ld,\"renderingOpportunities\":%ld,"
                 "\"renderingOpportunitiesSited\":%ld,\"renderingDigests\":%ld,"
                 /* The per-flow coroutine-activation swap, the `is_gendata` entry kind (a shared generator's or
                    async closure's execution-state pointer) that `cowStateAsks`/`cowStateMade` cannot see.
                    `Calls` because they are raised before any gate cow.c owns. The gap is per sub-kind and the
                    two are not added: the generator producer's only early return is the dedup replace of a
                    re-fork, so `GenCalls - GenMade` is re-forks; the async hook's is the decline, so
                    `AsyncCalls - AsyncMade` is declines. Lifetime counts (cow.c's `g_coro_*`); `Made <= Calls`
                    is DCHECKed in cow.c. A nonzero `coroSwapGenMade` proves the counting reaches this census,
                    not that the async hook (the interpreter's await-resume) is reachable. */
                 "\"coroSwapGenCalls\":%ld,\"coroSwapGenMade\":%ld,"
                 "\"coroSwapAsyncCalls\":%ld,\"coroSwapAsyncMade\":%ld,"
                 /* Which state units were exercised: per-kind lifetime counts (cow.c's `g_state_asks`/
                    `g_state_made`). A pair because `made` 0 is nothing reached under a running flow, a gate
                    correctly refusing a flow-private object, or a broken unit (solver/cow.h). Not a ratio: a walk
                    asks once per key and records once, so `asks` far ahead of `made` is healthy; `made <= asks`
                    is asserted in cow.c. `cowHostRecAsksBySite` splits the `hostRec` asks per component site, a
                    per-site lifetime count summing to `cowStateAsks.hostRec`, asserted over the counters in
                    cow.c's `cow_host_rec_sites` and over the rendered rows by engine/build.mjs's @SWAP reader. */
                 "\"cowStateAsks\":%s,\"cowStateMade\":%s,\"cowHostRecAsksBySite\":%s}",
                 sc, st, sm, sc ? (double)st / (double)sc : 0.0, hs, he, ds, de,
                 dw, dsi, du, dn, df, dr, dss, roa, rog, ros, rod,
                 gc, gm, ac, am, asks, made, sites);
    free(sites);
    return out;
}

/* What the frontier is made of and what its parked snapshots weigh: solver/cold.h's ColdCensus, this
   instance's totals (solver/engine.h's EngineFrontierCensus), the step-unit runs, the replay and refinement
   ledgers, the park preview, and what a resume rebuilt (see result.h). Each row's kind is declared in the
   block above result_cold_json and follows its accessor; `instanceUs` is a span (one subtraction of two clock
   readings), not an accumulator, and `perFlowKiB`/`sharedKiB` are sums of gauges from one walk.
   Per-flow rows multiply by the frontier's size; shared rows are counted once, since a frozen segment is
   referenced by every flow forked below it, so `perFlowKiB` and `sharedKiB` are what a pager trades. `dynKiB`
   is priced with the shared half: a program's text is one buffer however many timelines hold it
   (solver/dyn_body.h).
   Every row is emitted, zeroes included, and engine/build.mjs's `coldFields()` derives its required set from
   this format string, so a row dropped or renamed fails there. These are reports, never bounds: nothing in
   the engine reads them to decide anything. Sized by composef (solver/compose.h), which measures the length
   this run writes. */
/* One row composer for the step-unit histograms: they differ only in the population counted, so one loop
   spells solver/step_unit.h's row format. `what` names the histogram in the width assert, since a shared
   helper's DCHECK stamps this line for every caller. Returns the sum and asserts nothing about it: each
   caller's identity has a different other side (the live members, the step count). */
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

/* The composer for a histogram whose row set is the population's own (solver/cold.h): there is no list to
   derive a width from, so it applies compose.h's mechanism to a row list — measure, allocate exactly, write.
   C99 §7.19.6.5 "The snprintf function" ("If n is zero, nothing is written, and s may be a null pointer")
   makes the measuring pass legal. Both passes read one array, so they disagree only if a count changed; a
   truncation would lose the closing brace. */
static char *cursor_hist_json(const long *counts, int n, const char *what)
{
    int k, need = 2, hi;   /* the two braces, then each row as it measures */
    char *out;

    DCHECK(what != NULL, "a population-sized histogram was composed with no name — the asserts below are the "
                         "only thing that says WHICH of this composer's histograms a crash is about, and this "
                         "helper stamps one file and line for every caller");
    DCHECKF(counts != NULL && n > 0,
            "the %s histogram was composed from nothing, or over an empty row set — solver/cold.c gives it "
            "row 0 even on an empty frontier precisely so that `{}` never reaches a reader that refuses one, "
            "so an extent of zero here is that walk having been skipped rather than a frontier with nobody "
            "standing in it", what);
    for (k = 0; k < n; k++) {
        int w = snprintf(NULL, 0, "%s\"%d\":%ld", k ? "," : "", k, counts[k]);
        CHECKF(w > 0, "a %s row could not be MEASURED — snprintf reported an encoding error, so there is no "
                      "length to allocate against, and any size chosen instead would be the hand-counted "
                      "guess solver/compose.h replaced", what);
        need += w;
    }
    out = malloc((size_t)need + 1);
    if (!out) return NULL;
    out[0] = '{';
    hi = 1;
    for (k = 0; k < n; k++)
        hi += snprintf(out + hi, (size_t)(need + 1 - hi), "%s\"%d\":%ld", k ? "," : "", k, counts[k]);
    out[hi++] = '}';
    out[hi] = 0;
    DCHECKF(hi == need,
            "the %s histogram was WRITTEN to a different length than it was MEASURED for — the two passes "
            "read one array of counts, so they can only disagree if a count changed between them, and the "
            "row about to be spliced into the census is truncated at its closing brace", what);
    return out;
}

/* The kind of every row this composer publishes, in a form testing/census_rows.js reads from source: a
   quantity whose kind a reader cannot name from its output may not be used in arithmetic. The kinds:
     lifetime  raised and never lowered — may be differenced across two samples of one instance.
     gauge     a walk at one instant — may fall, so differencing one reads a level as a rate.
     constant  written once at a seed — neither differenced nor read as a level.
     maximum   a high-water mark — may be differenced, but a plateau is not a ceiling, so unlike `lifetime`
               it may not be compared across runs of different length.
   Declared at the emitter because whoever adds a row knows its kind. The reader enforces set equality both
   ways (a declared row no longer published, a published row with no kind) and refuses a second declaration
   for one composer or a row stated twice. Comment text changes no emitted byte, so a reader of the source is
   right about an artifact of any age. Histograms carry kinds like scalars.
   Kinds come from the filling accessor, never the name. `cold_census` is a walk, so its rows are gauges
   (including the out-of-programs rows, keyed on the per-member `script_i == dyn_n`, and `live`, declared so
   `live - outOfPrograms` is one walk); EngineFrontierCensus admits no gauge (solver/engine.h), so its rows are
   lifetime, maxima or seed constants; ColdPreviewCensus only accumulates; ColdResumed is the session's at
   most one rebuild, so constant. Exceptions: `owed` is its own walk whose marks age out (a gauge), and
   `orphanClaims` comes from the rebuild (constant) while `orphanClaimsMet`/`Unmet` are lifetime. The @H
   surface splits: `endpoint_mark_asset` marks a record already in `g_eps` when the reply lands, so
   `epEmitted = epMinted - epAssets` falls with nothing wrong, and it, `epPreProgram` and the histograms
   partitioning it are gauges while `epMinted` and `epAssets` are lifetime.

   @kinds-of cold
   @kind gauge: stepUnits programCursors replyOutstanding rowsAwaitingBytes
   @kind gauge: live outOfPrograms outOfProgramsUnrun outOfProgramsFramed outOfProgramsAtTheLadder
   @kind gauge: outOfProgramsAtTheLadderUnits
   @kind lifetime: stepUnitRuns stepUnitOverruns stepUnitOverrunSeamlessArms stepUnitOverrunAskArms
   @kind lifetime: hostAsked hostAnswered replyAsked replyAnswered replyDeclined replyDropped
   @kind lifetime: replayHits replayLeft replayLeftArms
   @kind lifetime: branchAsked branchRefined
   @kind lifetime: forkOverPinned
   @kind lifetime: steps sliceUs sliceOverruns sliceOverrunAsks sliceOverrunSeamless stepUs schedUs
   @kind lifetime: unitMidProgram unitParked unitCheckpointOwed unframedStepsLifetime
   @kind lifetime: stepReachedRenderingLife stepReachedTimerLife stepReachedIdleLife
   @kind lifetime: clockAdvanceAskedLife clockAdvanceDeclinedLife
   @kind lifetime: classicCompiles classicCompileOverruns finished
   @kind lifetime: classicCompileAgain classicCompileAgainBytes classicCompileOwnDecode
   @kind lifetime: classicCompileResumed classicParseShared
   @kind lifetime: epMinted epAssets
   @kind gauge: epEmitted epPreProgram epDoors epReach epAddressClass epRazorClass epWitnessClass
   @kind lifetime: epAsks epAskPreProgram epAskSuppressed epAskMerged epAskMinted epAskMergedPreProgram
   @kind lifetime: netProgQueuedLife netProgFetchAsksLife netProgFetchQueuedLife
   @kind lifetime: netProgXhrAsksLife netProgXhrQueuedLife
   @kind constant: rootPrograms rootProgramsHeldAtSeed rootProgramsAwaitedAtSeed
   @kind maximum: deepest completed deepestLeft sliceOverrunGapUs stepUnitOverrunGapArms
   @kind gauge: framed blocked owed
   @kind gauge: decEntries decKiB headEntries headKiB domHeadEntries domHeadKiB pendKiB miscKiB perFlowKiB
   @kind gauge: jobs pend pendReady stackEmpty canDeliver
   @kind gauge: segKiB domSegKiB pinSegs pinSegEntries pinSegKiB decSegs decSegEntries decSegKiB
   @kind gauge: dynBodies dynKiB sharedKiB programsAhead
   @kind lifetime: finishedFlows finishedCands progStarts progStartsCand progStartsOther progQueuedCand
   @kind lifetime: sold soldFlows soldCands forks orphanClaimsMet orphanClaimsUnmet orphanPreferred orphanScripts
   @kind lifetime: hostAnswersExtra hostAnswersLate hostTerminated
   @kind lifetime: pagedReqs pagedAsks pagedUnarmed pagedFloor
   @kind lifetime: previewAsks previewAsksRefusing previewAsksEmpty previewAsksWritable
   @kind lifetime: previewAsksWithFlows previewAsksWithCands previewAsksWithDeep previewAsksWithDeepCands
   @kind lifetime: previewAsksWithWorlds previewAsksWithOrphans previewAsksWithCommits previewAsksWithDelivers
   @kind lifetime: previewAsksAfterCommit previewCommitRowsWritten
   @kind lifetime: instanceUs loopUs betweenSlicesUs slices
   @kind constant: resumed resumedSegs resumedFlows resumedCands resumedWorlds orphanClaims
*/
char *result_cold_json(void) {
    ColdCensus c;
    /* The step-unit histograms, each composed into its own buffer and spliced as one `%s`. Their width
       STEP_UNITS_JSON_MAX is derived from solver/step_unit.h's list, so an arm added there widens them all.
       `hist` is the members standing in each arm now; `runs` the steps this instance ran in each arm — neither
       derivable from the other (an arm never entered and one entered and left before every census both read 0
       in `hist`). `ladder` is `hist` restricted to members at the orphan ladder, whose identity is a subset sum
       (solver/cold.h). Every row is emitted including zeroes, since an absent row and a zero row differ.
       `programCursors` is keyed on the frontier's own `script_i` (closed at `dyn_n`), so cursor_hist_json
       measures it instead. */
    char hist[STEP_UNITS_JSON_MAX];
    char runs[STEP_UNITS_JSON_MAX];
    /* …`ladder`: the same list restricted to members at the orphan ladder (solver/cold.h). */
    char ladder[STEP_UNITS_JSON_MAX];
    /* …`over`: restricted to the turns that overran the cooperative slice. */
    char over[STEP_UNITS_JSON_MAX];
    /* …`seam`: of those, the turns that offered no suspend point; a row because `over` and the seamless scalar
       cannot be joined (solver/engine.h's `over_seamless_arms`). */
    char seam[STEP_UNITS_JSON_MAX];
    /* …`oask`: of the overrunning turns that did offer a point, how many points each arm's offered; the scalar
       sums every arm, and arms' densities differ (solver/engine.h's `over_ask_arms`). */
    char oask[STEP_UNITS_JSON_MAX];
    /* …`ogap`: not a count but a maximum — each arm's widest interval, in the slice's own measure, during which
       one of its non-seamless overrunning turns offered nothing (solver/engine.h's `over_gap_arms`). */
    char ogap[STEP_UNITS_JSON_MAX];
    /* The two frontier-extent histograms live on the heap, since there is no list width to derive; see
       cursor_hist_json, which takes each one's name for its asserts. */
    char *cursors;
    char *ahead;
    char *out;
    ColdResumed resumed;
    /* The park preview ask census, taken in one call so the partition over its arms is one moment. */
    ColdPreviewCensus pv;
    EngineFrontierCensus e;
    EngineStepUnitRuns r;
    int ran;
    /* The replay ledger, taken in one call (decide.h) so the identity below is about one moment. */
    long rp_hits, rp_left, rp_left_arms;
    /* The refinement pair, in one call (decide.h): decide_arm's third arm, beside `replayHits` and `_forkAt`. */
    long rf_asked, rf_refined;
    long fk_total, fk_pinned;
    long orph_pref;
    long orph_scripts;
    /* The clock licence's pair — see core/timing/event_loop.h. Both or neither at the accessor. */
    long adv_asked, adv_declined;
    long awaiting_rows;   /* the awaited-rows gauge, read once below and used by the assert and the row */
    /* what the emitted @H array is a fraction of, and what of it predates any program — endpoint.h */
    long ep_minted, ep_assets, ep_emitted, ep_pre_program;
    long ep_asks, ep_ask_pre, ep_ask_sup, ep_ask_merged, ep_ask_minted, ep_ask_merged_pre;
    /* …and what stands in front of the door those six are counted at: solver/endpoint.h states the contract,
       its three identities, and that the stage arms are a partition. Spliced as rows beside `epAsks`, so
       consumers of `_cold` (including extension/bridge.js and extension/popup.js, live on write while this
       half is live only after a build) render them with no edit. */
    char *edge;
    char *xedge;
    /* …and the invoker rungs' own denominator, beside `stepUnitRuns`: a rung's arm there is an `else if` in
       flow_step, so its zero cannot separate a document that hangs nothing off the rung from a ladder that
       never gave it a turn (solver/rung_entry.h). */
    char *rungs;
    /* …the same surface partitioned by the mechanism that composed each address (solver/endpoint.h). */
    char *doors;
    /* …by what a markup parse would have reached through each mechanism: the row above coarsened by a map, one
       fact at two grains (solver/endpoint.h states the map and its three classes). */
    char *reach;
    /* …by whether the run had determined each address at all: a second observation, not a third grain, since
       no door implies it (solver/endpoint.h states each class and why the field is a floor). */
    char *acls;
    /* …and the bar itself: the per-row union of that row with the door's bytes column, which no marginal
       carries. A fourth walk, and a cross-check against the emitted array's `razorClass`, never a sum with it
       (solver/endpoint.h). */
    char *razor;
    /* …and the one fact that bar does not carry: how many addresses rest on a path that had pinned a source
       (`path_pinned`, set in flow.c and stamped onto requests at engine.c's pending_push sites). It bounds
       reproducibility — bytes this engine chose rather than a server sent — and is never unioned into the bar,
       since a pin's bytes are a literal the page's own predicate spelled (solver/endpoint.h). */
    char *witness;

    cold_census(&c);
    engine_step_unit_runs(&r);
    cursors = cursor_hist_json(c.program_cursors, c.program_cursor_n, "program-cursor");
    ahead   = cursor_hist_json(c.programs_ahead, c.programs_ahead_n, "remaining-rows");
    {
        /* The compositions run outside the asserts because a DCHECK's condition is compiled out in release; only
           the comparisons are asserted. */
        long standing = cold_hist_json(hist, sizeof hist, c.step_units, "stepUnits");
        long stepped  = cold_hist_json(runs, sizeof runs, r.arms, "stepUnitRuns");
        long atladder = cold_hist_json(ladder, sizeof ladder, c.at_the_ladder_units,
                                       "outOfProgramsAtTheLadderUnits");
        long overran  = cold_hist_json(over, sizeof over, r.over_arms, "stepUnitOverruns");
        long seamless = cold_hist_json(seam, sizeof seam, r.over_seamless_arms,
                                       "stepUnitOverrunSeamlessArms");
        long askarms = cold_hist_json(oask, sizeof oask, r.over_ask_arms,
                                      "stepUnitOverrunAskArms");
        /* `ogap` goes through the same row composer for the shared row format, and its return — a sum of maxima —
           is discarded; its own identity is the maximum re-folded below. */
        (void)cold_hist_json(ogap, sizeof ogap, r.over_gap_arms, "stepUnitOverrunGapArms");
        long atcursor = 0;
        long atahead = 0;
        int k;
        for (k = 0; k < c.program_cursor_n; k++) atcursor += c.program_cursors[k];
        for (k = 0; k < c.programs_ahead_n; k++) atahead += c.programs_ahead[k];
        /* Every live member carries exactly one arm, so the counts sum to the frontier. */
        DCHECK(standing == c.flows,
               "the step-unit histogram does not account for every member of the frontier — each live flow "
               "carries exactly one arm, so a total that is not `flows` means the census walk and the "
               "histogram disagree about who is standing");
        /* The lifetime histogram's identity, asked again at the boundary the number crosses (engine.c asserts it
           at the convergence point): a difference only here is a row lost on the way into this document. */
        DCHECK(stepped == r.steps,
               "the lifetime step histogram does not account for every scheduler step — the arms are counted "
               "at the convergence point and the steps at flow_step's entry, so a total that is not `steps` "
               "means one of the two stopped being written, and every reading of which rung the ladder stops "
               "at is then about a ladder this document did not climb");
        /* The same contract over the overrunning subset (engine.c asserts it in the raising branch). This
           histogram is normally sparse, so a lost row would read as a loop that rested more often. */
        DCHECK(overran == r.slice_overruns,
               "the slice-overrun histogram does not account for every overrunning turn — the arm and the "
               "total are raised on one line from one turn's clock readings, so a total that is not "
               "`sliceOverruns` means a row was lost crossing into this document, and a sparse histogram "
               "losing a row reads as a loop that rested more often rather than as a broken count");
        /* One restriction further: a lost row here reads as an arm whose overruns offered suspend points, when the
           truth may be a C activation with no step boundary. */
        DCHECK(seamless == r.slice_overrun_seamless,
               "the seamless-overrun histogram does not account for every seamless turn — the arm and the "
               "scalar are raised one statement apart from one turn's arm and one turn's consultation delta, "
               "so a total that is not `sliceOverrunSeamless` means a row was lost crossing into this "
               "document, and the loss reads as an arm that DID offer suspend points");
        /* The complement's identity: a lost row reads as turns that offered fewer points than they did. The cast
           is the narrowing solver/engine.h's `over_ask_arms` states: the scalar is 64-bit, the row a `long`. */
        DCHECK((uint64_t)askarms == r.slice_overrun_asks,
               "the per-arm ask histogram does not account for every suspend point an overrunning turn "
               "offered — the arm and the scalar are raised one statement apart in the same branch from one "
               "turn's arm and one turn's consultation delta, so a total that is not `sliceOverrunAsks` means "
               "a row was lost crossing into this document, and the loss reads as turns that offered fewer "
               "points than they did");
        /* `ogap`'s identity is a maximum, re-folded here from the array the row was composed from. A lost row
           moves the maximum down, which would read as points offered more evenly. The per-arm containment
           against the scalar is entailed (solver/engine.h's `over_gap_arms`). */
        {
            long gapmax = 0;
            int g;

            for (g = 0; g < STEP_UNIT_N; g++)
                if (r.over_gap_arms[g] > gapmax) gapmax = r.over_gap_arms[g];
            DCHECK((int64_t)gapmax == r.slice_overrun_gap_us,
                   "the per-arm worst-gap row and `sliceOverrunGapUs` disagree — the two are folded from ONE "
                   "turn's interval one statement apart in the engine's overrun branch and the fold is a "
                   "MAXIMUM rather than a sum, so a scalar the arms do not reach means a row was lost "
                   "crossing into this document, and the loss reads as a turn that offered its suspend "
                   "points more evenly than it did");
        }
        /* Every live member stands at exactly one program cursor, so the counts sum to the frontier; a short walk
           would misreport where the mass stands. */
        DCHECK(atcursor == c.flows,
               "the program-cursor histogram does not account for every member of the frontier — each live "
               "flow stands at exactly one cursor, so a total that is not `flows` means the census walk and "
               "the histogram disagree about who is standing, and this is the one row a reader consults to "
               "decide whether the mass advanced or a few members ran deep ahead of it");
        /* Every live member has one distance to the end of its own sequence, so these sum to the frontier. */
        DCHECK(atahead == c.flows,
               "the remaining-rows histogram does not account for every member of the frontier — each live "
               "flow has exactly one distance to the end of its own program sequence, so a total that is not "
               "`flows` means the census walk and the histogram disagree about who is standing, and every "
               "reading of whether the frontier is converging on a retirement is then composed from a "
               "population nobody enumerated");
        /* `out_of_programs` selects `script_i == dyn_n` and this histogram buckets `dyn_n - script_i`, so bucket 0
           is that row counted by a second walk of the same pass. */
        DCHECKF(c.programs_ahead[0] == c.out_of_programs,
                "the remaining-rows histogram's ZERO bucket (%ld) and `outOfPrograms` (%ld) disagree — they "
                "are the same predicate written two ways over the same two fields on the same pass "
                "(`dyn_n - script_i == 0` and `script_i == dyn_n`), so a difference is one of the two walks "
                "having been given a member the other was not, and the census is about a frontier that is not "
                "the one standing",
                c.programs_ahead[0], c.out_of_programs);
        /* The orphan ladder's histogram is over a subset, so it sums to the row raised on the same if/else chain;
           a disagreement is that chain gaining an arm without a row, a selection read as a partition. */
        DCHECK(atladder == c.out_of_programs_at_the_ladder,
               "the orphan-ladder step-unit histogram does not account for every member standing at the "
               "ladder — the two are raised in one pass over one if/else arm, so a total that is not "
               "`outOfProgramsAtTheLadder` means the breakdown is a SELECTION being read as a partition and "
               "the arm the members are actually stopping at is not the one this row names");
    }
    cold_resumed(&resumed);
    cold_preview_census(&pv);
    engine_frontier_census(&e);
    /* The awaited-rows gauge, read once so the assert below and the emitted row are the same sample. */
    awaiting_rows = engine_rows_awaiting_bytes();
    /* What the emitted @H array is a fraction of, read once beside the other gauges; endpoint.c asserts the two
       arms sum to the mint where all three are in one hand. */
    endpoint_surface_census(&ep_minted, &ep_assets, &ep_emitted, &ep_pre_program);
    endpoint_ask_census(&ep_asks, &ep_ask_pre, &ep_ask_sup, &ep_ask_merged, &ep_ask_minted,
                        &ep_ask_merged_pre);
    /* Rows awaiting bytes are a subset of the register entries owing them. Every row standing as an external
       script has one entry naming it by `dyn_id` on the same member's register: engine.c pushes both at the
       one creating site, flow_deliver_one_reply removes the entry and flips the row's kind in one activation,
       a fork duplicates both, and HTML §7.5.10 "Destroying documents"' removal walk takes both. An excess is a
       row whose park was retired without it, which this sees for every row of every member. */
    DCHECK(awaiting_rows <= c.pend_count,
           "more program rows of the frontier are standing on an address than there are pending register "
           "entries to owe them bytes — a row awaiting a program and the park that names it are created and "
           "retired together at every site that does either, so this is a row whose park was taken without "
           "it: that member will stop at this position for the rest of the session on a reply nothing will "
           "ever be asked for, and `rowsAwaitingBytes` is about to be published as a debt the reply door does "
           "not hold");
    decide_replay_stats(&rp_hits, &rp_left, &rp_left_arms);
    decide_refine_stats(&rf_asked, &rf_refined);
    /* The fork pair's containment, at the one moment both are in one hand (solver/decide.h); decide.c raises
       them at two lines. `fk_total` is read only for this assert. */
    decide_fork_pinned_stats(&fk_total, &fk_pinned);
    /* The orphan walk's order witnesses, read here because their denominator `_orphansDriven` is in hand only
       here. `JS_OrphanTakeOne` prefers a body whose source resolved a network door's entry name against the
       global object and falls through otherwise, so a run with no such body is byte-identical to no order.
       Nonzero: read the addresses. Zero beside nonzero `epFetchAskNamedLife`: the `fetch` occurrences are in
       program bodies, which this walk skips by construction. */
    orph_pref = engine_orphan_preferred();
    orph_scripts = engine_orphan_scripts();
    event_loop_advance_census(&adv_asked, &adv_declined);
    DCHECK(adv_declined <= adv_asked,
           "the clock licence was refused more often than it was asked for — both are raised in ONE "
           "evaluation at the ask, ahead of the answer, so a refusal above the ask count is a second "
           "writer of one of them and the partition this pair exists to be is no longer one");
    {
        /* A preferred take is a take. The two counts come from different mechanisms (a runtime field bumped in the
           walk, and `g_orphans_driven` raised by the visitor it calls), so the inequality is what ties them to
           one walk: it fires on a take that marks a body and returns without the visitor. `od` is read only
           for these asserts. */
        long od, oa;

        engine_orphan_census(&od, &oa);
        DCHECKF(orph_pref >= 0 && orph_pref <= od,
                "the orphan walk reports %ld PREFERRED takes against %ld drives — a preferred take is a take, "
                "so a numerator above this denominator means the walk marked a body `entered` and returned "
                "without its visitor seeding a flow, which loses that body for the life of the instance and "
                "publishes an order that fired over work that never happened", orph_pref, od);
        /* The fairness witness's containment, asserted apart so the abort names which witness disagreed: a script
           enters the take table only when a take is charged to it. */
        DCHECKF(orph_scripts >= 0 && orph_scripts <= od,
                "the orphan walk reports takes charged to %ld distinct SCRIPTS against %ld drives — a script is "
                "charged only when a take is made, so a numerator above this denominator means the per-script "
                "take table gained a row for a body that was never handed to a visitor", orph_scripts, od);
    }
    DCHECKF(fk_pinned >= 0 && fk_pinned <= fk_total,
            "forks taken over an already-proved subject (%ld) outnumber the forks this session took (%ld) — "
            "the two are raised in decide.c at the SAME decision, the subset's raise being gated on the very "
            "`forked` answer the total counts, so this is a numerator that outran its denominator and the "
            "share is about to be published above 1", fk_pinned, fk_total);
    /* The cursor histogram against the maximum it is read beside. A cursor `c > 0` means the member left row
       `c - 1`, and every cursor advance is engine.c's ENGINE_LEAVE_ROW, which raises `deepestLeft` to the row
       before leaving it; so the top index is at most `deepestLeft + 1`. An empty frontier has row 0 and
       `deepestLeft` -1. Not `deepest` (the deepest program started): HTML §4.12.1.1 "Processing model"'s
       execute-the-script-element step 4 skips a row whose result is null, so a failed external script is left
       without being started. A break is a cursor advanced elsewhere, or that macro failing to raise the row;
       the width is measured, so do not widen it here. */
    DCHECK(c.program_cursor_n - 1 <= e.deepest_left + 1,
           "the frontier's deepest STANDING cursor is more than one past the deepest row this document has "
           "ever LEFT — a cursor is one-past-the-row-it-left, so those two numbers are the same fact read "
           "twice and a member cannot stand beyond a row nothing reached. Either a cursor advanced somewhere "
           "other than engine.c's ENGINE_LEAVE_ROW, or that macro moved one without raising `deepestLeft`. "
           "This is NOT `deepest`: a row HTML §4.12.1.1's step 4 skipped is left without being started, so a "
           "failed external script legitimately puts a cursor one past a program that never began");
    /* The reply door's pair is one population: pending_index_key DCHECKs a record is keyed at most once and
       pending_index_answered untracks the record it credits, so a break is a record credited elsewhere. */
    DCHECK(pending_index_answered_total() <= pending_index_asked_total(),
           "the reply door was answered more times than it was asked — a record is keyed once and untracked "
           "when it is answered, so a payment credited without a key is a reply settling a record the host was "
           "never shown, and `replyAnswered/replyAsked` is about to be published as a rate over two different "
           "populations. Both terms are written in solver/pending_index.c and nowhere else");
    /* …and the gap between them is a partition: every record the door has keyed is, at this instant, answered,
       declined by the trusted zone, dropped with the flow that asked, or still owed. solver/pending_index.c
       credits each at the one line that puts it there (`pend_untrack`'s three callers, and `pend_unkey` for
       the fourth). Three terms are lifetime counts and `keyed_now` is a gauge, so the identity holds only
       with all five in one hand, here. */
    DCHECK(pending_index_asked_total() ==
               pending_index_answered_total() + pending_index_declined_total() +
               pending_index_dropped_total() + pending_index_keyed_now(),
           "the reply door's four ends do not sum to what it was asked — every keyed record is answered, "
           "declined, dropped with the flow that asked, or still outstanding, and those are the only four "
           "ends solver/pending_index.c has. A difference means a record left the set without being credited "
           "to any of them, and `replyAsked - replyAnswered` is about to be published as a gap whose reading "
           "a reader cannot recover: a host that still owes replies and a surface this tool refused to ask "
           "for are opposite findings and only one of them is about the reply door");
    /* Two subset chains over one cold_census walk of one frontier, so an excess is two sums over different
       populations: `pend_ready` (entries flow_deliver_one_reply may take) within `pend_count` (every register
       entry), and `can_deliver` (the delivery arm's whole guard) within `stack_empty` (its left conjunct)
       within the member count. */
    DCHECK(c.can_deliver <= c.stack_empty && c.stack_empty <= c.flows,
           "the frontier holds more members that can DELIVER than members whose execution context stack is "
           "empty, or more of the latter than there are members at all — the two rows are the whole of "
           "flow_stack_empty's guard and its left conjunct, counted in one pass of cold_census over one "
           "frontier, so this is two sums over different populations and `canDeliver`/`stackEmpty` are about "
           "to be published as a reading of a frontier that was never walked");
    DCHECK(c.pend_ready <= c.pend_count,
           "more of the frontier's register entries are DELIVERABLE than there are register entries — the two "
           "rows are counted in one pass of cold_census over one frontier and the deliverable set is a subset "
           "of the register by construction, so this is two sums over different populations and the census is "
           "about to publish a delivery debt larger than the registers it was read out of");
    ran = resumed.flows + resumed.cands > 0;
    /* A rebuild is all of itself or none of it. cold_resume memsets its census on entry and ends with
       `DCHECK(flows > 0)`, so `flows + cands == 0` is the never-called state and must carry no segments,
       foreign worlds or orphan locators; `resumed: 0` is a positive claim that no residue was handed over. */
    DCHECK(ran || (resumed.segs == 0 && resumed.worlds == 0 && resumed.orphans == 0),
           "the cold tier reported a rebuild that landed no flow and yet rebuilt segments, foreign worlds or "
           "orphan locators — `resumed` is about to be emitted as 0, which STATES that this session was handed "
           "no residue, and that would be a lie about a residue that was read back. cold_resume's own "
           "`flows > 0` is the other side of this pair");
    /* A claim is evidence of an inherited drive, never one this session started: `orphan_want` is written only
       at cold.c's 'o' record and spreads by fork. The comparison holds because engine_sched_begin calls
       cold_resume at most once per session; met and unmet may exceed `orphans` (a drive's arms are one drive),
       so the implication runs one way. */
    DCHECK(resumed.orphans > 0 || (e.claims_met == 0 && e.claims_unmet == 0),
           "an inherited-drive claim was met or lost in a session whose rebuild carried no orphan locator — the "
           "three orphanClaims rows are about to describe a round trip that this document also says did not "
           "happen");
    /* The park preview's partition, at the boundary the numbers cross; cold.c asserts it at every ask, so a
       difference only here is a row lost between the accessor and this document. */
    DCHECK(pv.asks_refusing + pv.asks_empty + pv.asks_writable == pv.asks,
           "the park preview's ask census does not partition itself — `previewAsksRefusing`, "
           "`previewAsksEmpty` and `previewAsksWritable` are the three arms of one `if` chain over one ask "
           "and sum to `previewAsks` by construction, so a difference is a row lost crossing into this "
           "document and the rows are about to be read as which conjunct of a park moment refused");
    /* The replay ledger's identity, at the one moment all three are in one hand (decide.h returns them in one
       call). Both clauses are needed: `left_arms >= left` alone permits `left == 0` beside a nonzero arm sum.
       dec_leave_path's precondition (`g_c < dec_total()`) makes every divergence abandon at least one arm. */
    DCHECK(rp_left_arms >= rp_left && (rp_left == 0) == (rp_left_arms == 0),
           "the replay ledger's two divergence rows contradict each other — every call of dec_leave_path "
           "abandons AT LEAST ONE arm (its own precondition is that the cursor is short of the end), so the "
           "arm total can be neither smaller than the event count nor zero beside a non-zero one. "
           "`replayLeft` and `replayLeftArms` are about to be published as the statement of what a resume did "
           "with its recorded path, and they are counted at one site two lines apart");
    /* `refined` is raised one line below the test that raises `asked`, on the same `key`, so an excess is a
       second writer. One clause only: `refined == 0` beside `asked == 0` is the legitimate state of no
       spellable question reached (decide.h's witness paragraph). */
    DCHECK(rf_refined <= rf_asked,
           "more decisions were refined out of a flow's own constraint than were ASKED over a spellable "
           "question — decide_arm raises the denominator on the `key` test and the numerator one line below "
           "it, so a subset larger than its population is a second writer of one of the two. "
           "`branchRefined / branchAsked` is about to be published as the share of this session's spellable "
           "decisions that cost nothing at all");
    /* A histogram that could not be allocated makes the census absent (composef's NULL contract), never a census
       with a row missing, which readers that assert the shape would report as a broken relay. */
    edge = endpoint_fetch_edge_rows();
    xedge = endpoint_xhr_edge_rows();
    rungs = rung_entry_rows();
    doors = endpoint_door_hist_json();
    reach = endpoint_reach_hist_json();
    acls  = endpoint_address_hist_json();
    razor = endpoint_razor_hist_json();
    witness = endpoint_witness_hist_json();
    if (!cursors || !ahead || !edge || !xedge || !rungs || !doors || !reach || !acls || !razor || !witness) {
        free(cursors);
        free(ahead);
        free(edge);
        free(xedge);
        free(rungs);
        free(doors);
        free(reach);
        free(acls);
        free(razor);
        free(witness);
        cold_census_release(&c);
        return NULL;
    }
    out = composef(
                 /* `live` is the frontier's current size; the created total is `_flows` on the document. A
                    stalled frontier has retired (`finished`) or paged (`sold`) its members. `live` is not split
                    by candidate (`_wfq.cands` carries that half), and `framed` (the park's re-execution cost)
                    and `blocked` (what the host owes) stay whole. `owed` counts flows that told the scheduler
                    they cannot progress, which the pick reads; `blocked` asks each register whether the host
                    owes it anything; the gap between them is marks cleared faster than the sweep lays them. */
                 "{\"live\":%ld,\"framed\":%ld,\"blocked\":%ld,\"owed\":%d,"
                 /* `finished` and `sold` carry their two populations: an exploration flow retiring is coverage
                    gained, a candidate member retiring is search spent on a payload that did not fire.
                    `*Cands` count members, not sessions — engine_sibling_assemble copies the candidate identity
                    to every sibling, and `_candidates` is the per-session count (solver/engine.h). The parts are
                    rows rather than a subtraction so they can be checked; engine_frontier_census asserts the
                    identity. The keys are not renamed: the composer ships in `qjs.wasm` while build.mjs reads
                    the tree live. */
                 "\"finished\":%ld,\"finishedFlows\":%ld,\"finishedCands\":%ld,"
                 /* `deepest` (deepest program started) and `completed` are global high-water marks set by one
                    member; `deepestLeft` is the deepest row any member left. */
                 "\"deepest\":%d,\"completed\":%d,\"deepestLeft\":%d,"
                 /* …and the number those two maxima are read against: the root document's own <script> count, not
                    any flow's sequence length (which continues with chunks, injections and candidates).
                    solver/engine.h states why no inequality between them holds. */
                 "\"rootPrograms\":%d,"
                 /* …and its two arms. `rootProgramsAwaitedAtSeed` is the reply-door openings this document owes
                    for its own bundle, so `replyAsked` equal to it is a run that issued no page `fetch()`, XHR or
                    dynamic `import()`. A partition, asserted at engine_frontier_census. */
                 "\"rootProgramsHeldAtSeed\":%d,\"rootProgramsAwaitedAtSeed\":%d,"
                 /* …and the live half of that pair: a gauge, where `…AwaitedAtSeed` is a constant written once at
                    the seed. It counts program rows of the live frontier standing on an address now: `17` beside
                    `0` is a bundle that arrived whole (read `programCursors`), `17` beside `17` a bundle whose
                    bytes never came (the fetch path). Summed per member, so a fork raises it and a sale lowers it
                    and no inequality against the seed's arm holds; its relation to `pend` is asserted at the head
                    of this composer. */
                 "\"rowsAwaitingBytes\":%ld,"
                 /* The @S search's own numerator and denominator (solver/engine.h has the full reading).
                    `progStartsCand` 0 is either no breakout queued or N queued and none started;
                    `progQueuedCand` tells them apart, and `progStarts` 0 says nothing either way. The parts are
                    rows rather than a subtraction so the identity engine.c asserts in dev is checkable off a
                    release log. A fork copies unstarted rows, so `progStartsCand` may exceed `progQueuedCand`.
                    `cand*` on the @WFQ line counts members carrying a payload substitution, a different
                    population from these programs. */
                 "\"progStarts\":%ld,\"progStartsCand\":%ld,\"progStartsOther\":%ld,"
                 "\"progQueuedCand\":%ld,"
                 /* …and whether a reply became a program — the "fetched JS is always executed" surface. Two doors
                    build it, solver/engine.c's FLOW_PENDING_RESOLVE delivery and core/xhr/xml_http_request.c's
                    `xhr_take_reply`; both end in `engine_queue_fetched_script`, which queues a DYN_PAGE_SCRIPT,
                    so `progStartsOther` cannot separate them from the page's own bundle.
                    Each door is an ask/queued pair: `…Asks` is raised where the door holds a reply record,
                    upstream of the type gate, so `0/0` is a door never reached and `0/N` one reached that
                    queued nothing (correctly, for a non-JavaScript type). Both asks count reply records.
                    `netProgQueuedLife` is raised inside `engine_queue_fetched_script` itself, and test_forced.c's
                    `loadScript` host edge is a third caller, so `fetch + xhr <= total` (asserted in the engine);
                    the residue is zero in the shipped program. A door crediting a program the compile entry
                    never saw would break it. The two doors are independent (axios's browser adapter is XHR). */
                 "\"netProgQueuedLife\":%ld,"
                 "\"netProgFetchAsksLife\":%ld,\"netProgFetchQueuedLife\":%ld,"
                 "\"netProgXhrAsksLife\":%ld,\"netProgXhrQueuedLife\":%ld,"
                 /* `sold` and `resumed` are outcome censuses over gates with legitimate declining arms.
                    `sold` is gated on the allocator's refusal, so 0 is the RAM floor never met; its ask side is
                    `pagedAsks` with `pagedFloor` and `pagedUnarmed`. `resumed` is gated in the trusted zone on a
                    key of the document's address plus bundle id that no document publishes, so 0 is no residue
                    written, a key miss (the design working on a changed bundle), or a rebuild that produced
                    nothing — only the last a defect, and separating them is a trusted-zone diff.
                    The capability is checkable on a tracked fixture (a live page's rolling deploy makes a miss
                    correct): serve testing/fixtures and drive one document twice,
                      node testing/harness.js restart      <port>
                      node testing/live-run.js 1 'http://127.0.0.1:<p>/wjp_absent.html?__forcepark=1'
                      node testing/harness.js restart-keep <port>     # preserves IndexedDB
                      node testing/live-run.js 1 'http://127.0.0.1:<p>/wjp_absent.html?__forcepark=1'
                    and the second must report `resumed > 0` and `park` 0 (the forced park arms only when no
                    prior recipes exist). `?__forcepark=1` is a test hook; a visit parks only under Level-1 RAM
                    pressure, which is why these rows read 0 on an ordinary visit. */
                 "\"sold\":%ld,\"soldFlows\":%ld,\"soldCands\":%ld,\"forks\":%ld,"
                 /* `resumed` is the positive statement that a rebuild ran, always present: `resumed: 0` says
                    no residue was handed over, so zeroes beside it are not a verdict. `resumedSegs`/`Flows`/
                    `Cands`/`Worlds` decompose it into which arms of the residue grammar ran (cold.h). No
                    `resumedOrphans` row: `orphanClaims` is that number. */
                 "\"resumed\":%d,\"resumedSegs\":%ld,\"resumedFlows\":%ld,\"resumedCands\":%ld,"
                 "\"resumedWorlds\":%ld,"
                 /* The path half's verdict rows, lifetime counts. Units differ: `replayHits` and `replayLeftArms`
                    are arms (decision-vector slots), `replayLeft` is divergence events; their identity is asserted
                    above. They say what the `resumed*` rows cannot: whether a rebuilt session followed its
                    recorded path. A divergence is permitted (a replay runs against today's code and replies);
                    decide.h names the residual. `resumed: 0` beside nonzero `replayLeft` attributes it to
                    siblings. */
                 "\"replayHits\":%ld,\"replayLeft\":%ld,\"replayLeftArms\":%ld,"
                 /* The third of decide_arm's three arms. A spellable decision is refined (no slot, no member),
                    replayed (`replayHits`) or new (`_forkAt`'s predicate rows). The unit is decisions, not
                    `replayHits`' arms, so the two ledgers may not be summed. `branchAsked` is the reachability
                    witness: zero `branchRefined` beside zero `branchAsked` is no spellable question reached
                    (decide.c's fork_site_name residual). Read as a fraction or not at all. */
                 "\"branchAsked\":%ld,\"branchRefined\":%ld,"
                 /* …and of the new decisions, how many forked over a subject this flow had already proved. The two
                    pin mint arms hand back a bare primitive that never reaches a branch hook, but a source the
                    page materialized before its gate keeps its record (`concolic_example`), so a second predicate
                    over it forks with a singleton domain. A population size, not a defect count. Its
                    denominator `forks` comes from the same call (solver/decide.h), so `forkOverPinned <= forks`
                    holds within one moment. */
                 "\"forkOverPinned\":%ld,"
                 /* The orphan walk's preferred takes, over `_orphansDriven` on this document; the containment is
                    asserted where both are in one hand. */
                 "\"orphanPreferred\":%ld,"
                 /* …and over how many distinct scripts the same drives were spread, independent of the preferred
                    count: one script against a hundred drives is a monopoly. `orphanScripts <= orphansDriven` is
                    asserted where both are in one hand. */
                 "\"orphanScripts\":%ld,"
                 /* The cold round trip's verdict: `orphanClaims` is the inherited drives a resume rebuilt,
                    `orphanClaimsMet` the waits a take satisfied, `orphanClaimsUnmet` the waiting flows that
                    finished never handed a body — the loss, zero when the bytes did not change. Met may exceed
                    claims (a drive's arms are one drive). Read only under `resumed: 1`. No `orphans` row:
                    `_orphansDriven` is that number. */
                 "\"orphanClaims\":%ld,\"orphanClaimsMet\":%ld,\"orphanClaimsUnmet\":%ld,"
                 /* `hostAsked`/`hostAnswered` are minted at engine.c's `mint_req` for FLOW_PENDING_HOSTREQ: the
                    four cross-instance reads and XMLHttpRequest, whose async and sync (§3.5.6) arms share that
                    rendezvous. The reply door (fetch, injected `<script src>`, the document's script slots,
                    dynamic `import()`) is the `reply*` pair below. `hostAnswersExtra` is not part of
                    `hostAnswered`: only a rendezvous's first answer settles it, the rest fork arms. */
                 "\"hostAsked\":%ld,\"hostAnswered\":%ld,\"hostAnswersExtra\":%ld,"
                 "\"hostAnswersLate\":%ld,\"hostTerminated\":%ld,"
                 /* The four ends of the reply door, together because the asked/answered gap means nothing without
                    the other two: a gap may be refusals of the page's own requests, where the door owes nothing.
                    `replyOutstanding` is the gauge (is the host behind); the other three are lifetime counts
                    (solver/pending_index.h). */
                 "\"replyAsked\":%ld,\"replyAnswered\":%ld,"
                 "\"replyDeclined\":%ld,\"replyDropped\":%ld,\"replyOutstanding\":%ld,"
                 "\"pagedReqs\":%ld,"
                 "\"pagedAsks\":%ld,\"pagedUnarmed\":%ld,\"pagedFloor\":%ld,"
                 /* …and why a park that never happened did not. These are the host asking the cold tier what a
                    park would write; `previewAsks: 0` is no host consulting it, and every row below is then
                    meaningless. A moment is a conjunction, so the rows split it: a row that never rose wants its
                    producer built, rows that rose and never coincided want a different moment. The partition is
                    the tier's own verdict — refusing (would abort), empty (writes no bytes), writable (writes a
                    residue) — and the rest derive from ColdPreview's struct. */
                 "\"previewAsks\":%ld,"
                 "\"previewAsksRefusing\":%ld,\"previewAsksEmpty\":%ld,"
                 "\"previewAsksWritable\":%ld,"
                 /* The eight `previewAsksWith*` rows are contained in `previewAsks` and partition nothing, since
                    several conjuncts may stand at once. `previewAsksAfterCommit` counts asks after the commitment
                    ledger was written, differing from `previewAsksWithCommits` by commitments that departed with
                    their flows; `previewCommitRowsWritten` counts ledger rows (solver/flow.h), not asks. Read the
                    three together (cold.h). */
                 "\"previewAsksWithFlows\":%ld,\"previewAsksWithCands\":%ld,"
                 "\"previewAsksWithDeep\":%ld,\"previewAsksWithDeepCands\":%ld,"
                 "\"previewAsksWithWorlds\":%ld,\"previewAsksWithOrphans\":%ld,"
                 "\"previewAsksWithCommits\":%ld,\"previewAsksWithDelivers\":%ld,"
                 "\"previewAsksAfterCommit\":%ld,\"previewCommitRowsWritten\":%ld,"
                 "\"decEntries\":%ld,\"decKiB\":%ld,\"headEntries\":%ld,\"headKiB\":%ld,"
                 "\"domHeadEntries\":%ld,\"domHeadKiB\":%ld,\"jobs\":%ld,\"pend\":%ld,\"pendReady\":%ld,"
                 "\"stackEmpty\":%ld,\"canDeliver\":%ld,"
                 "\"pendKiB\":%ld,"
                 "\"miscKiB\":%ld,\"perFlowKiB\":%ld,"
                 "\"segKiB\":%ld,\"domSegKiB\":%ld,\"pinSegs\":%ld,\"pinSegEntries\":%ld,"
                 "\"pinSegKiB\":%ld,\"decSegs\":%ld,\"decSegEntries\":%ld,\"decSegKiB\":%ld,"
                 "\"dynBodies\":%ld,\"dynKiB\":%ld,\"sharedKiB\":%ld,"
                 /* `steps` and `stepUnitRuns` are lifetime counts of steps run through each arm, and `stepUnits` a
                    gauge of members standing in each arm now: with the gauge alone, an arm never entered and one
                    entered and left before every census read alike. `stepUs` is `steps`' denominator — the thread
                    time the loop's turns consumed, in the slice's own measure — so a turn costing a whole slice
                    makes one choice per slice by construction, while cheap turns mean little thread time was
                    given. Read `stepUs / steps` as a ratio within one run, never a total across runs; calling it
                    CPU needs the @QUANTUM line. `%lld` because on wasm32 a `long` of microseconds saturates in
                    35.8 minutes (solver/engine.h's `step_us`; engine.c asserts the width). */
                 "\"steps\":%ld,\"stepUs\":%lld,"
                 /* …and what that total is a share of: the thread measure this instance consumed since its
                    dispatch loop first ran. `stepUs` counts only turns taken, so a small `stepUs/steps` is a cheap
                    loop or one barely entered. Both sides are one clock, `quantum_thread_us()` (named on
                    @QUANTUM), so the ratio survives a wall-only host. `stepUs <= instanceUs` is asserted at
                    engine_step_unit_runs; solver/engine.h's `instance_us` holds the residual. */
                 "\"instanceUs\":%lld,"
                 /* …and its two halves. `loopUs` is the engine's thread inside its dispatch bracket outside a turn
                    and `betweenSlicesUs` the host's between slices; they partition `instanceUs` exactly (the open
                    tail closes into `betweenSlicesUs` from the same clock reading), asserted at
                    engine_step_unit_runs. `loopUs` small points at the driver; large with `stepUs` small points
                    at this scheduler, starting with `slices` against `steps`. `slices` is `%ld` because it is a
                    count (solver/engine.h's `loop_us`). */
                 "\"loopUs\":%lld,\"betweenSlicesUs\":%lld,\"slices\":%ld,"
                 /* The two phases `stepUs` sums: a step overrunning the slice is the quantum with no asynchronous
                    source to expire it, a dominant pick-and-swap is the ordering costing more than the work.
                    `schedUs` is everything in the turn that is not the step, including the previous iteration's
                    tail (the charge telescopes; solver/engine.h); identity asserted at engine_frontier_census. */
                 /* …and `sliceOverruns`, because `stepUs/steps` is a mean no turn is near: raised once per turn
                    from the readings `sliceUs` accumulates, containment asserted at engine_step_unit_runs
                    (solver/engine.h's `slice_overruns`). */
                 "\"sliceUs\":%lld,\"schedUs\":%lld,\"sliceOverruns\":%lld,\"stepUnitRuns\":%s,"
                 /* …and which arm each overrunning turn was in: `stepUnitRuns` restricted to `sliceOverruns`'
                    turns. An arm whose two counts are equal is a step that cannot rest (solver/engine.h's
                    `over_arms`). */
                 "\"stepUnitOverruns\":%s,"
                 /* …and which of those turns offered no suspend point, per arm: against the arm's overruns it says
                    whether the thread was inside C that declares no step boundary, which decides between a
                    step-machine conversion and a page's own stretch (solver/engine.h's `over_seamless_arms`). */
                 "\"stepUnitOverrunSeamlessArms\":%s,"
                 /* …and how many suspend points each arm's other overrunning turns offered. The denominator is the
                    arm's overruns minus its seamless turns, which contribute zero by construction; the scalar
                    `sliceOverrunAsks / sliceOverruns` averages arms that answer differently (solver/engine.h's
                    `over_ask_arms`). */
                 "\"stepUnitOverrunAskArms\":%s,"
                 /* …and the widest no-offer stretch of one such turn, in the slice's own measure. The preempt
                    policy's only false arm is the budget test, monotone within a slice, and the first true parks
                    the flow, so every consultation sits within ENGINE_QUANTUM_MS of the slice's opening and this
                    row is the worst turn's span to within one budget (`stepUnitOverruns` is only a count). A 0 is
                    no non-seamless overrunning turn or gaps under a microsecond; `stepUnitOverruns` minus
                    `stepUnitOverrunSeamlessArms` tells them apart (solver/engine.h's `over_gap_arms`). */
                 "\"stepUnitOverrunGapArms\":%s,"
                 /* …and whether the page's own code ran in those turns: an arm is where a step ended, and the time
                    may be the page's bytecode between its raise points or one native call that never returned.
                    `sliceOverrunAsks` is the suspend points those turns offered and `sliceOverrunSeamless` how
                    many offered none; all-seamless asks for a step-machine conversion, a large sum is a question
                    about the page (solver/engine.h's `slice_overrun_asks`). */
                 "\"sliceOverrunAsks\":%llu,\"sliceOverrunSeamless\":%ld,"
                 /* …and the worst no-offer stretch over every arm at once, so `max(arms) == this` makes the
                    per-arm row checkable as whole. A high-water mark: read against ENGINE_QUANTUM_MS, never
                    another run's figure; its evidence is bounded by `stepUnitOverruns` minus
                    `stepUnitOverrunSeamlessArms`. */
                 "\"sliceOverrunGapUs\":%lld,"
                 /* …and which phase of a start step spent the time. A start compiles then executes; the compile
                    rests (JS_FlowCompileStep hands the parse back part way), so one program parses over one or
                    more stints. `classicCompiles` is one per program, `classicCompileOverruns` one per stint that
                    met the slice, so they are not a ratio; the stint population is `classicCompiles +
                    stepUnitRuns[compile-handed-the-thread-back]`, against which the containment is asserted.
                    `classicCompiles` counts every flow, timeline and appended row (chunks, injected scripts,
                    `javascript:` URLs, peer operations, value dumps), so it far exceeds `rootPrograms` on a
                    healthy app page. `classicCompileAgain` states repeats directly (bytes some flow already
                    parsed to completion), priced by `classicCompileAgainBytes` and bounded by
                    `classicCompileOwnDecode`, since a reply is decoded per delivery. The two subsets partition
                    differently and may not be added (solver/engine.h's `classic_compiles`). */
                 "\"classicCompiles\":%ld,\"classicCompileOverruns\":%ld,"
                 "\"classicCompileAgain\":%ld,\"classicCompileAgainBytes\":%lld,"
                 "\"classicCompileOwnDecode\":%ld,"
                 /* …and programs started from a parse another timeline already finished, which parse no bytes and
                    raise none of the four rows above: `classicCompiles + classicParseShared` is the classic
                    programs that obtained a closure at all. `classicCompileAgain` staying nonzero is not sharing
                    failing (a flow mid-parse of a row someone finished resumes its own parse). See
                    solver/engine.h's `classic_parse_shared` and solver/dyn_body.h. */
                 "\"classicParseShared\":%ld,"
                 /* …and whether a parse handed back was carried forward:
                    `stepUnitRuns[compile-handed-the-thread-back]` minus this is the parses begun and not ended.
                    Not a subset of `classicCompiles`, and not a rate against it: one is raised per continued
                    stint, the other per ended parse (solver/engine.h's `classic_compile_resumed`). */
                 "\"classicCompileResumed\":%ld,"
                 /* …and why each turn that ended no unit of work did not. `_unitsDone` is a gated count, so low is
                    either a thread that did nothing or one advancing programs it never finished. With the credited
                    arm (`_unitsDone`, in the other object) the four sum to `steps`, asserted in the engine
                    (solver/engine.h's `unit_mid_program`). */
                 "\"unitMidProgram\":%ld,\"unitParked\":%ld,\"unitCheckpointOwed\":%ld,"
                 /* …and how many steps descended flow_step's work ladder: every arm of it (deliveries, checkpoint,
                    reply, sequence, task, lifecycle, orphan rungs, clock sources, resting arms) sits inside one
                    `if (!f->frame)`. A lifetime count of passes through that block, so not comparable with
                    `steps` (the loop body iterates) but with `_orphansAsked`, raised on the same basis, the
                    containment asserted in the engine. 0 puts the cause upstream of the ladder; large, with
                    `_orphansAsked == 0`, says an arm above the orphan rung took every descent (`stepUnitRuns`).
                    The `outOfProgramsAtTheLadder` family cannot stand in: it selects on `script_i == dyn_n`,
                    while the rung binds to `seq_compiles` (solver/engine.h's `unframed_steps`). */
                 "\"unframedStepsLifetime\":%ld,"
                 /* …and how far down the ladder the descents got. The three clock arms of `stepUnitRuns` are
                    raised only when their hook takes the step, so a 0 there is "nothing due" or "never asked";
                    these count the ask, at the arm ahead of each gate. The three rungs are consecutive arms of one
                    `else if` chain, so these are suffix sums: `stepReachedRenderingLife` is every descent reaching
                    the chain, the next two those the rendering, then the timer, rung did not take. Unlike a 0 in
                    the partition, the lowest 0 here localises. `finished` 0 with `stepReachedRenderingLife` 0
                    means retirement was never asked; with it large, one of the ten arms above `finished` took
                    every descent. No sum is published beside them; the identities with `stepUnitRuns` and
                    containment in `unframedStepsLifetime` are asserted at engine_step_unit_runs. Lifetime
                    counts per instance (solver/engine.h's `clock_render_asks`). */
                 "\"stepReachedRenderingLife\":%ld,\"stepReachedTimerLife\":%ld,"
                 "\"stepReachedIdleLife\":%ld,"
                 /* …and which of two reasons the timer rung declined for: no source became due, or one was due and
                    core/timing/event_loop.h's licence refused to manufacture its dueness (that header's
                    residual). The pair partitions rather than restates `stepReachedIdleLife`, and its ask half
                    spans both refusing rungs (timer and rendering). Lifetime counts; `Declined <= Asked` is
                    asserted where both are in one hand. */
                 "\"clockAdvanceAskedLife\":%ld,\"clockAdvanceDeclinedLife\":%ld,"
                 /* `outOfPrograms` separates a member inside its last program from one past the last row, which
                    a cursor value cannot: only the latter reaches engine.c's orphan arm. Its three parts also
                    name members never dispatched or suspended in a live frame (the ladder sits below
                    `if (!f->frame)`). solver/cold.h owns the ladder's list; do not enumerate its rungs here. */
                 "\"outOfPrograms\":%ld,"
                 "\"outOfProgramsUnrun\":%ld,\"outOfProgramsFramed\":%ld,"
                 "\"outOfProgramsAtTheLadder\":%ld,"
                 "\"outOfProgramsAtTheLadderUnits\":%s,"
                 /* `programCursors` is the per-member companion to the maxima: `deepest 11` is as true of one
                    member at 11 as of two thousand. Buckets are cursors (closed at `dyn_n`); a cursor one past
                    the deepest row left is `deepestLeft + 1`, the bound asserted above. A failed external script
                    is left without being started, so the mass may run ahead of `deepest`. A top bucket says where
                    the mass reached, never that the sequence ends (engine_seed_scripts queues the whole table);
                    `outOfPrograms` says who has no row left. */
                 "\"stepUnits\":%s,\"programCursors\":%s,"
                 /* …and how far each standing member is from having nothing left to run (solver/cold.h;
                    `outOfPrograms` is its bucket 0). The cursor histogram is a position, this a distance. */
                 "\"programsAhead\":%s,"
                 /* The @H surface's own denominator — endpoint.h states why its length is three states: small
                    `epEmitted` with large `epAssets` learned little because the addresses were files.
                    `epAssets` counts the flag `endpoint_mark_asset` set, an outcome over a classifier with a
                    declining arm, so 0 under a large `epMinted` is: no reply reached the classifier; one
                    arrived with an empty `computedType` (the chokepoint's refusal record); or the type was good
                    and `is_asset` answered false (e.g. a `.woff2` served `application/octet-stream` with
                    `nosniff`), the design working. Widening the predicate by URL suffix would be matching, not
                    running.
                    Named residual. Not covered: the ask — replies reaching the classifier with a type that
                    parsed — so the first state cannot be told from the others (`hostAsked`/`hostAnswered` only
                    bound it). Next diff: that count, raised in solver/reply_decode.c where the type parses,
                    published beside this one. Absence shows as a 0 here reported as a reply-path defect on a
                    run whose classifier declined correctly. */
                 /* …and how many emitted rows were minted before this instance started a program: `epEmitted -
                    epPreProgram` bounds what forced execution contributed, and equal values learned nothing the
                    markup did not state (a surface can be the `<head>`'s own `<script src>` and `<link>` rows
                    counted back). endpoint.h holds the contract, the ceiling reading and the resumed-timeline
                    residual. */
                 "\"epMinted\":%ld,\"epAssets\":%ld,\"epEmitted\":%ld,\"epPreProgram\":%ld,"
                 /* …and which mechanism composed each emitted address: a partition of `epEmitted`, asserted at the
                    composer (solver/endpoint.c) over the record array and re-checked over the emitted document by
                    engine/build.mjs's `censusHistRows` and extension/bridge.js's histogram contract.
                    The door is which mechanism and `epPreProgram` is when; neither implies the other (a router's
                    `<link>` and a markup one share a door, and a `<link>` after a `<script src>` is minted
                    post-program). Each emitted row carries both. */
                 "\"epDoors\":%s,"
                 /* …and which of those mechanisms a markup parse of the served document would have reached, keyed
                    on the door list's third column: `markup`, `either` (a parser-inserted or script-created
                    `link-element`, `image-element` or `form-submit`, which the door does not distinguish) and
                    `beyond`, a floor under the markup question. This is a diagnostic, not the razor: a literal
                    chunk URL through `module-import` is `beyond` yet scores zero at the bar, which is
                    `epRazorClass` below (composed by `endpoint_razor_class_of`); build.mjs renders this as the
                    markup diagnostic and `endpointRazorClassReading` as the bar.
                    Entailed by the door row (each class sums its doors), so the two are one observation; what
                    this adds is the map. Never a target: `beyond` 0 against nonzero `epEmitted` is a refusal to
                    claim, and the row is an identity within one run, not comparable across wall-denominated
                    runs. Its denominator `epEmitted` travels on this line; endpoint.c asserts the sum and
                    build.mjs re-checks it against the emitted document. */
                 "\"epReach\":%s,"
                 /* …and the address's own class, read off the address value's concolic provenance at endpoint.c's
                    door: the bar is a property of the address, not the mechanism (`/api/{location.hash}` through
                    `fetch` clears it; a literal chunk URL through `module-import` does not). A second
                    observation beside `epDoors`/`epReach`, which are one.
                    A floor, not a verdict: `unknown` is a positive statement that the address rested on an
                    unknown supplied from outside the engine (one resting on a hole the engine minted for its own
                    drive is refused), so it clears the bar. `concrete` claims nothing about a parse; what it
                    hides is listed at solver/endpoint.h (a literal, the document's own address, or a pinned
                    source re-read). Never a target. Its denominator `epEmitted` travels on this line;
                    endpoint.c asserts the sum. */
                 "\"epAddressClass\":%s,"
                 /* …and the bar itself, which no reader can compose from its operands: a union is a statement
                    about per-row membership, and the marginals above carry no overlap. A cross-check against the
                    emitted array's `razorClass`, never a sum with it: both come off `endpoint_razor_class_of`,
                    taken at two instants over a gauge (`epEmitted` falls when an asset verdict lands between
                    them), so a disagreement names which records each document described. `runtime-only` 0
                    against nonzero `epEmitted` is a refusal to claim; `unproven` claims nothing about a parse.
                    Its denominator `epEmitted` travels on this line; endpoint.c asserts the sum. */
                 "\"epRazorClass\":%s,"
                 /* …and the necessary condition under that bar, beside it and never inside it: the bar is a floor
                    of positive statements, and this is a "may" (solver/endpoint.h). `may-rest-on` bounds the
                    addresses composed by a path that had pinned a source, which `addressClass` calls
                    `concrete` because the pin arm re-reads a bare primitive: 0 means no chosen-bytes population
                    under this surface, nonzero means addresses that may not reproduce for a real session and
                    the next diff is the pin. It is not a bound on the bar's slack — a pin's bytes are a literal
                    the page's own text spells (solver/concolic.c), so `unproven` holds them correctly.
                    `unasked` is no flow standing when the record was minted: a door composing a request outside
                    the scheduler. Its denominator `epEmitted` travels on this line; endpoint.c asserts the sum. */
                 "\"epWitnessClass\":%s,"
                 "\"epAsks\":%ld,\"epAskPreProgram\":%ld,\"epAskSuppressed\":%ld,"
                 /* …and the one cut inside the merged arm (endpoint.h). Not summed with the three arms beside it:
                    they partition the door's exits and this selects inside one. Nonzero means running code
                    composed an address the markup had already named — a network call site reached and nothing
                    learned. */
                 "\"epAskMerged\":%ld,\"epAskMinted\":%ld,\"epAskMergedPreProgram\":%ld"
                 /* …and the host edge's own entry, which the rows above cannot see: they count at endpoint_record,
                    so a `fetch()` the engine threw out of or parked inside before core/fetch's `FETCH_CALL` (its
                    sixth stage) is in none of them. These count that machine's states at capture and teardown,
                    partitioned by the stage each torn-down construction reached. A bare `%s` because the rows
                    carry their own names and leading comma; the absent form is the empty string, since a host
                    with no fetch has no population (solver/endpoint.h). */
                 /* …and the other host edge's, beside it and never summed with it: core/xhr records from the
                    lifecycle machine its `send()` mints, whose seven stages are its own. A second bare `%s`,
                    absent as the empty string for a host with no XMLHttpRequest (solver/endpoint.h). */
                 /* …and the invoker rungs' denominator, spliced: which rungs have one is the declaring components'
                    fact, so an arm with no component installed has no row rather than a zero. */
                 "%s%s%s}",
                 c.flows, c.framed, c.blocked, flow_host_owed_count(),
                 e.finished, e.finished_flows, e.finished_cands,
                 e.deepest, e.completed, e.deepest_left,
                 e.root_programs,
                 e.root_programs_held_at_seed, e.root_programs_awaited_at_seed,
                 awaiting_rows,
                 e.prog_starts, e.prog_starts_cand, e.prog_starts_other, e.prog_queued_cand,
                 e.net_prog_queued,
                 e.net_prog_fetch_asks, e.net_prog_fetch_queued,
                 e.net_prog_xhr_asks, e.net_prog_xhr_queued,
                 e.sold, e.sold_flows, e.sold_cands, e.forks,
                 ran, resumed.segs, resumed.flows, resumed.cands, resumed.worlds,
                 rp_hits, rp_left, rp_left_arms,
                 rf_asked, rf_refined,
                 fk_pinned,
                 orph_pref,
                 orph_scripts,
                 resumed.orphans, e.claims_met, e.claims_unmet,
                 e.host_asked, e.host_answered, e.host_answers_extra, e.host_answers_late, e.host_terminated,
                 pending_index_asked_total(), pending_index_answered_total(),
                 pending_index_declined_total(), pending_index_dropped_total(),
                 pending_index_keyed_now(),
                 e.paged_reqs,
                 e.paged_asks, e.paged_unarmed, e.paged_floor,
                 pv.asks,
                 pv.asks_refusing, pv.asks_empty, pv.asks_writable,
                 pv.asks_with_flows, pv.asks_with_cands,
                 pv.asks_with_deep, pv.asks_with_deepcands,
                 pv.asks_with_worlds, pv.asks_with_orphans,
                 pv.asks_with_commits, pv.asks_with_delivers,
                 pv.asks_after_commit, pv.commit_rows_written,
                 c.dec_entries, c.dec_bytes / 1024, c.head_entries, c.head_bytes / 1024,
                 c.dom_head_entries, c.dom_head_bytes / 1024, c.job_count, c.pend_count, c.pend_ready,
                 c.stack_empty, c.can_deliver,
                 c.pend_bytes / 1024, c.misc_bytes / 1024,
                 (c.dec_bytes + c.head_bytes + c.dom_head_bytes + c.pend_bytes + c.misc_bytes) / 1024,
                 c.seg_bytes / 1024, c.dom_seg_bytes / 1024,
                 c.pin_seg_count, c.pin_seg_entries, c.pin_seg_bytes / 1024,
                 c.dec_seg_count, c.dec_seg_entries, c.dec_seg_bytes / 1024,
                 c.dyn_count, c.dyn_bytes / 1024,
                 (c.seg_bytes + c.dom_seg_bytes + c.pin_seg_bytes + c.dec_seg_bytes + c.dyn_bytes) / 1024,
                 r.steps, (long long)r.step_us,
                 (long long)r.instance_us,
                 (long long)r.loop_us, (long long)r.between_slices_us, r.slices,
                 (long long)r.slice_us, (long long)r.sched_us, (long long)r.slice_overruns, runs, over,
                 seam,
                 oask,
                 ogap,
                 (unsigned long long)r.slice_overrun_asks, r.slice_overrun_seamless,
                 (long long)r.slice_overrun_gap_us,
                 r.classic_compiles, r.classic_compile_overruns,
                 r.classic_compile_again, (long long)r.classic_compile_again_bytes,
                 r.classic_compile_own_decode,
                 r.classic_parse_shared,
                 r.classic_compile_resumed,
                 r.unit_mid_program, r.unit_parked, r.unit_checkpoint_owed,
                 r.unframed_steps,
                 r.clock_render_asks, r.clock_timer_asks, r.clock_idle_asks,
                 adv_asked, adv_declined,
                 c.out_of_programs,
                 c.out_of_programs_unrun, c.out_of_programs_framed, c.out_of_programs_at_the_ladder,
                 ladder, hist, cursors, ahead,
                 ep_minted, ep_assets, ep_emitted, ep_pre_program, doors, reach, acls, razor, witness,
                 ep_asks, ep_ask_pre, ep_ask_sup, ep_ask_merged, ep_ask_minted, ep_ask_merged_pre,
                 edge, xedge, rungs);
    free(cursors);
    free(ahead);
    free(edge);
    free(xedge);
    free(rungs);
    free(doors);
    free(reach);
    free(acls);
    free(razor);
    free(witness);
    cold_census_release(&c);
    return out;
}

/* What the runtime and the C allocator under it hold: QuickJS's JS_ComputeMemoryUsage walk, the child realms
   navigable.c built, and mallinfo (see result.h). Rows are split by kind because a climbing `allocations` with
   a flat `objects` is memory no GC object owns (atom, string, property table, bytecode), each with its own owner.
   `miscBytes`/`miscParts` are QuickJS's `memory_used_*` bucket: two entries per realm plus every property
   array, fast-array element vector, var_ref, bound function, C-closure record and module entry. The realm
   count is navigable.c's. `childRealmsMade` (lifetime) and `childRealmsPeak` (high-water) ride beside the live
   `childRealms`: `made == peak` within one sample says no realm was reclaimed (HTML §7.5.10 "Destroying
   documents" step 7's reference drop), `made > peak` says reclamation ran.
   `unattributed` is malloc_size minus `memory_used_size`, one subtraction: the named rows are already summed
   into `memory_used_size`, so adding them again would count the named heap twice. `stepMachines` and
   `trampFrames` decompose it, being the two largest things QuickJS cannot name.
   Kinds: every row is a gauge (`allocations` is QuickJS's live `malloc_count`, lowered on free) except
   `childRealmsMade`, a lifetime count and the only row that may be differenced, and `childRealmsPeak`, a
   high-water mark. Sized by composef (solver/compose.h). */
char *result_heap_json(JSContext *ctx) {
    JSMemoryUsage mem;
    JSRuntime *rt;
    long long attributed;
    /* Who holds the child realms, taken in one call beside the three counts it explains (see navigable.h).
       Gauges: the identity holds within one sample and none may be differenced. -1 for min and max is the
       empty set, which a live realm's refcount cannot be. */
    int rmin = -1, rmax = -1;
    long rtotal = 0;
    /* …and which function took each reference, at the same instant (see core/frame/navigable.h). A map
       keyed by origin, whose keys are discovered data and so cannot be format literals; each value is the
       positional `[min, max, total]`, safe because that triple is fixed here. The rows are composed one at a
       time and spliced, since their number is not known to a format string. */
    NavigableRealmRefSite rsites[NAVIGABLE_REALM_REF_SITES_MAX];
    long rreleased = 0;
    int  rsites_n, ri;
    char *rsites_json, *heap;

    DCHECK(ctx != NULL, "the heap census was asked for against no realm — every row of it is a walk of ONE "
                        "runtime, so a census with no runtime to walk is a reading of nothing");
    rt = JS_GetRuntime(ctx);
    JS_ComputeMemoryUsage(rt, &mem);
    navigable_realm_refs(NULL, &rmin, &rmax, &rtotal);   /* `live` is navigable_realm_count() one line down */
    rsites_n = navigable_realm_ref_sites(rsites, NAVIGABLE_REALM_REF_SITES_MAX, &rreleased);
    /* `null` for a build that watches no references, never `[]`: an empty array is a run holding no live
       realm, the opposite fact. */
    rsites_json = composef("%s", rsites_n < 0 ? "null" : "{");
    for (ri = 0; ri < rsites_n; ri++) {
        char *next;
        /* The origin is the engine's own `__func__`, emitted unescaped; the DCHECK makes that an assertion. */
        DCHECK(rsites[ri].site != NULL && strcspn(rsites[ri].site, "\"\\") == strlen(rsites[ri].site),
               "a realm's reference origin is not a plain identifier — the origins are the engine's own "
               "function names, so a quotation mark or a backslash here is a row that came from somewhere "
               "else and would close this census's JSON string in the middle of a key");
        next = composef("%s%s\"%s\":[%d,%d,%lld]",
                        rsites_json, ri ? "," : "", rsites[ri].site,
                        rsites[ri].min, rsites[ri].max, (long long)rsites[ri].total);
        free(rsites_json);
        rsites_json = next;
    }
    if (rsites_n >= 0) {
        char *closed = composef("%s}", rsites_json);
        free(rsites_json);
        rsites_json = closed;
    }
    attributed = (long long)mem.memory_used_size;
    /* The spliced rows are this function's and die here; the census composef returns is the caller's. */
    heap = composef(
                 "{\"allocations\":%lld,\"atoms\":%lld,\"strings\":%lld,\"objects\":%lld,"
                 "\"shapes\":%lld,\"props\":%lld,\"funcs\":%lld,\"funcCode\":%lld,\"arrays\":%lld,"
                 "\"miscBytes\":%lld,\"miscParts\":%lld,\"childRealms\":%d,"
                 "\"childRealmsMade\":%d,\"childRealmsPeak\":%d,\"destroyStep9Releases\":%lld,"
                 "\"childRealmRefsMin\":%d,\"childRealmRefsMax\":%d,\"childRealmRefsTotal\":%lld,"
                 "\"childRealmRefsReleased\":%lld,\"childRealmRefSites\":%s,"
                 "\"objBytes\":%lld,\"propBytes\":%lld,\"shapeBytes\":%lld,\"strBytes\":%lld,"
                 "\"atomBytes\":%lld,\"funcBytes\":%lld,\"arrayElemBytes\":%lld,"
                 "\"unattributed\":%lld,\"stepMachines\":%d,\"trampFrames\":%d,"
                 "\"cLiveKiB\":%lld,\"arenaKiB\":%lld}",
                 (long long)mem.malloc_count, (long long)mem.atom_count, (long long)mem.str_count,
                 (long long)mem.obj_count, (long long)mem.shape_count, (long long)mem.prop_count,
                 (long long)mem.js_func_count, (long long)mem.js_func_code_size, (long long)mem.array_count,
                 (long long)mem.memory_used_size, (long long)mem.memory_used_count,
                 navigable_realm_count(), navigable_realm_made(), navigable_realm_peak(),
                 (long long)window_proxy_destroy_releases(),
                 rmin, rmax, (long long)rtotal, (long long)rreleased, rsites_json,
                 (long long)mem.obj_size, (long long)mem.prop_size, (long long)mem.shape_size,
                 (long long)mem.str_size, (long long)mem.atom_size, (long long)mem.js_func_size,
                 (long long)mem.fast_array_elements * (long long)sizeof(JSValue),
                 (long long)mem.malloc_size - attributed,
                 JS_StepMachineCount(rt), JS_TrampFrameCount(rt),
                 (long long)engine_c_alloc_live() / 1024, (long long)engine_c_alloc_arena() / 1024);
    free(rsites_json);
    return heap;
}

/* The composition, and nothing else. Each surface serializes itself — endpoint.c walks its deduped endpoints,
   solve.c its fire-verified sinks — and this only decides that they are one document and what it is called,
   so no host assembles structure again. */
char *result_json(JSContext *ctx) {
    char *eps = endpoint_json_array();
    char *sinks = solve_json_array(ctx);
    char *errs = errs_json_array(ERRS_STANDING);
    /* …and the ones this engine took back (see errs_json_array). Kept apart from `pageErrors`: a page that
       raised nothing and one that handled everything it raised are different pages. */
    char *errsRetracted = errs_json_array(ERRS_RETRACTED);
    /* …and the ones this engine minted itself: orthogonal to those two, so a message here is also in exactly
       one of them. errs_json_array states the three facts. */
    char *errsExplored = errs_json_array(ERRS_EXPLORED);
    /* The ordering, on the one surface that crosses the ABI — see result.h. Composed here rather than by a host
       so the production entry carries it. */
    char *wfq = result_wfq_json();
    /* …and what the frontier is made of, what the runtime holds, and what a context switch costs (see
       result.h); `decide.c`'s table joins them as the same question one level down — which predicate grows
       the frontier. */
    char *cold = result_cold_json();
    char *heap = result_heap_json(ctx);
    char *swap = result_swap_json();
    char *forkAt = decide_fork_json();
    /* …and what the document asked a standard for that this realm did not answer. An unbuilt API a page
       feature-detects answers `undefined`, so every endpoint and sink behind that guard is unreachable with
       nothing else saying so. Emitted on every document, including ones with no rows, because the run that
       finds nothing is the one whose reasons are wanted. solver/absent.c states the population, denominator,
       kinds and identity. */
    char *absent = absent_json();
    /* …and what the order was denominated in: solver/quantum.h's composer, a property of the host, without
       which two `_wfq` orderings are not comparable (see result.h). */
    char *quantum = quantum_json();
    /* No `probeResults` surface: a probe answer is the reply to a deliberately malformed request, which this
       engine cannot make (its only network edge is the pending register, and the host performs a GET through
       safeFetch). extension/lib/req2proto.js issues the probe as the page and writes
       `globalStore.probeResults`; nothing about it crosses this seam. */
    char *out;

    if (!eps || !sinks || !errs || !errsRetracted || !errsExplored || !wfq || !cold || !heap || !swap ||
        !forkAt || !quantum || !absent) {
        free(eps); free(sinks); free(errs); free(errsRetracted); free(errsExplored); free(wfq);
        free(cold); free(heap); free(swap); free(forkAt); free(quantum); free(absent);
        return NULL;
    }
    /* Sized by composef (solver/compose.h), so a field added below needs no count, slack or margin. The fit is
       measured in every build, on the same argument list, so release is right for the same reason dev is. */
    /* The park document rides the result, since it is what this engine has left to say about a page it did not
       finish. "[]" — the ordinary case — says this engine drained rather than paged out, which is what makes
       the host delete the origin's cold entry instead of resuming a stale residue. */
    {
        /* The three cost numbers together: a switch count alone cannot say whether a slower run grew its frontier
           or the work inside each flow, which need opposite fixes. And what the cross-instance seam did. */
        /* Held and made are two numbers. `_worldSegmentsHeld` is the live table (`world_segments_held`, lowered
           by world_release on a sender's flow death); `_worldSegmentsMade` is `world_segment_stats`' cumulative
           history, lowered only by world_segment_counts_reset. Together, as cold.c's park hook also prints them:
           held far below made is a seam that materialized and released. `world_segments_held`'s own DCHECK (a
           table larger than its history) rides along. */
        int held = world_segments_held(), made = 0, segf = 0;
        /* …and why the security array is the length it is: solver/metrics/scensus.def's rows, spliced at the
           top level, and the schema that declares them, which a reader takes the row set and kinds from. On
           the document because the renderer does not tee stdout. */
        char *scensus = metrics_family_members_text(METRIC_FAMILY_SCENSUS);
        char *schema = metrics_schema_text();
        /* …and what this instance did with the records a peer sent it — see engine.h for why the pair travels
           together and why a host's routed count is not comparable to a page's handler invocations. */
        long routedDelivered = 0, routedRefused = 0, routedZeroDelivery = 0;
        /* …and whether the uncalled-code surface ran at all: solver/engine.h's orphan census. `_orphansAsked`
           tells "this bundle ships no uncalled code" from "no flow reached the end of its own work". A zero has
           two more readings: the walk found nothing, or the session is non-forking and returns at the ask's
           gate before the counter moves (engine_session_forks tells it apart). The only host composing this
           document begins its sessions forking, so that reading cannot arise here; a host that begins
           non-forking owes the regime beside the pair. */
        long orphansDriven = 0, orphansAsked = 0;
        /* …and which exit each ask took, read at the same instant as the pair, since the four terms of one
           equality may not come from two moments. See solver/engine.h's `EngineOrphanExits`. */
        EngineOrphanExits orphanExits;
        JSOrphanWalkCost orphanWalk;
        /* …and what became of the tasks those deliveries queued: a listener running fewer times than delivered
           covers "the spec declined it" (§9.3.3 step 8.1), "no Document left to fire at" (§7.5.10 step 5) and
           "the scheduler lost the task", and only the last is a defect. */
        long routedEnds[ROUTED_TASK_END_N];
        /* The render's geometry census, read once into a local so the rows a reader divides share one moment. */
        FlowPlacementCensus place;
        /* …and what the run spent on the cascade, into a local for the same reason. This is these counters' only
           reader on a path the product takes; core/css/css_cascade_pass.h states each row. */
        CssCascadePassCensus casc;

        flow_placement_census(&place);
        css_cascade_pass_census(&casc);
        world_segment_stats(&made, &segf);
        engine_routed_census(&routedDelivered, &routedRefused, &routedZeroDelivery);
        engine_orphan_census(&orphansDriven, &orphansAsked);
        orphanExits = engine_orphan_exits();
        orphanWalk = JS_OrphanWalkCost(JS_GetRuntime(ctx));
        /* The orphan exits partition the asks; asserted here, where all four terms are in one hand (engine.c
           asserts it again at engine_step_unit_runs). An equality because all four are released together at
           engine_session_close. */
        DCHECK(orphanExits.memo + orphanExits.empty + orphanExits.took == orphansAsked,
               "the orphan surface's exits do not account for every ask this document is about to publish — "
               "the total is raised at engine_orphan_seed's entry past the forking gate and each part at "
               "exactly one of its exits, so a difference is an exit that records nothing and the residue a "
               "reader computes from these rows has silently stopped being the memo and the empty walk");
        /* One walk per ask that walked: `JS_OrphanTakeOne` enumerates the object list exactly once per call and is
           called once per ask the generation memo did not answer, so the walk count is the memo's complement.
           Asserted here, where the QuickJS-side count and the host's exit counts are in one hand. */
        DCHECK(orphanWalk.walks == (uint64_t)(orphanExits.empty + orphanExits.took),
               "the orphan walk count does not equal the asks that walked — one take is one enumeration of the "
               "object list and the memo answers the rest, so a difference means either a call enumerated the "
               "list more than once or an ask walked without being charged to an exit, and every price this "
               "document publishes per walk is then over the wrong denominator");
        engine_routed_task_census(routedEnds);
        out = composef("{\"fetchCallSites\":%s,\"securitySinks\":%s,\"pageErrors\":%s,"
                             /* The ones this engine named and then took back — beside `pageErrors` because the two
                                are disjoint and read together; errs_json_array states the three facts they keep
                                apart. */
                             "\"pageErrorsRetracted\":%s,"
                             /* …and the ones this engine minted: orthogonal to the pair above, so a message here
                                is also in exactly one of them and a consumer renders it once, under the context
                                this array decides. */
                             "\"pageErrorsExplored\":%s,"
                             /* `_switches` is not `_unitsDone`'s denominator. A switch is counted only where the
                                pick returns a different member (engine.c, beside flow_credit_pick); a unit at
                                every entry into flow_step, so `_unitsDone / _switches` can exceed 1. The
                                denominator is `steps` in result_cold_json's `engine_step_unit_runs` block, the
                                total solver/engine.h's `unit_mid_program` partition is asserted against; it is not
                                re-emitted here, because one number spelled twice in one document drifts. */
                             "\"_switches\":%d,\"_flows\":%ld,\"_candidates\":%d,"
                             "\"_jobsQueued\":%ld,\"_jobsRun\":%ld,\"_unitsDone\":%ld,"
                             "\"_worldSegmentsHeld\":%d,\"_worldSegmentsMade\":%d,"
                             "\"_worldSegmentsForked\":%d,"
                             "\"_routedDelivered\":%ld,\"_routedRefused\":%ld,"
                             /* …and the one of the three about a record rather than an attachment — see
                                solver/engine.h. Emitted only with the pair: alone, the pair invites comparing a
                                host's routed count with a sum of attachments, which assumes the receiver has one
                                timeline. A gauge: it rises on arrival and falls when a timeline admits the record,
                                so it is a backlog until the receiver drains to a stall, and a loss only after. */
                             "\"_routedZeroDelivery\":%ld,"
                             "\"_routedTasksFired\":%ld,\"_routedTasksTargetOrigin\":%ld,"
                             "\"_routedTasksTargetGone\":%ld,\"_routedTasksThrew\":%ld,"
                             "%s,\"_metricsSchema\":%s,"
                             /* …and the frontier's order at composition — result.h says why it rides here and what
                                its two shapes mean. One nested object, because its rows are mostly readings of an
                                instant. The nesting separates subsystems, never kinds. Every `_`-prefixed sibling
                                above is a lifetime count of events except `_worldSegmentsHeld` and
                                `_routedZeroDelivery`, which are gauges; `_worldSegmentsMade` is the former's
                                lifetime half, and the latter is read only at a drained receiver. The key is not
                                renamed because five consumers assert it (extension/bridge.js,
                                extension/popup.js, engine/solvergate.mjs, engine/route.mjs, testing/live-run.js).
                                "Lifetime" means this agent's: solve_init, world_registry_free and
                                solver_agent_free (which releases the metrics block) zero the resettable rows,
                                each once per agent on every host; the rest are reset by nothing. So the totals
                                are the agent cluster's: a second document taken through `qjs_join` calls none
                                of those inits, and a same-origin frame's reads sum into the root's. A second
                                call site of those inits within one agent would show as a row falling between
                                two `qjs_result` calls of one instance. */
                             "\"_orphansDriven\":%ld,\"_orphansAsked\":%ld,"
                             /* …and which exit each ask took. `_orphanAskMemo` is an ask the generation cache
                                answered with no walk; `_orphanAskEmpty` a walk that found nothing, a fact about
                                the heap rather than the bundle (engine_orphan_seed's residual). They sum with
                                `_orphanAskTook` to `_orphansAsked`, asserted above. A low `memo` with a high
                                `took` names the walk, not the cache, as the cost to repair: no cache skips a walk
                                that succeeds. */
                             "\"_orphanAskMemo\":%ld,\"_orphanAskEmpty\":%ld,\"_orphanAskTook\":%ld,"
                             /* …and what those walks cost, with both denominators. `_orphanWalkEntries /
                                _orphanWalks` is the mean object-list length one take reads.
                                `_orphanWalkFullCandidates / _orphanWalksFull` is the mean candidate population
                                over walks not cut short by a preferred candidate at the lowest quota, which see
                                only a floor of the set; zero full walks beside nonzero walks says every take was
                                decided early. That population is what separates the two readings of
                                `_orphansDriven: 0` — no uncalled code shipped, or a frontier that never reached
                                the question (engine_orphan_seed's residual). */
                             "\"_orphanWalks\":%llu,\"_orphanWalkEntries\":%llu,"
                             "\"_orphanWalksFull\":%llu,\"_orphanWalkFullCandidates\":%llu,"
                             "\"_wfq\":%s,"
                             /* The subsystem censuses, each one nested object for `_wfq`'s reason; a reader
                                compares within a census, never across (result.h). The nesting does not state any
                                row's kind — `_cold` carries many lifetime counts — so each composer states its own
                                grouping: see result_cold_json, result_heap_json and result_swap_json. */
                             "\"_cold\":%s,\"_heap\":%s,\"_swap\":%s,\"_forkAt\":%s,"
                             /* …and the names a standard owns that this realm answered with silence, a fifth
                                nested object. Lifetime counts zeroed with the published-namespace registry at the
                                agent's release; solver/absent.c states that at its composer. */
                             "\"_absent\":%s,"
                             /* …and what the render spent on geometry: lifetime counters of this agent, every one
                                differenceable. `childTopAsks` counts asks for a box's position under CSS 2.1
                                §9.4.1 "Block formatting contexts", `childTopServed` those
                                core/layout/flow_placement.h served from a whole-tree pass's record,
                                `childTopWalks` those that ran §9.4.1's walk over the container's whole child list;
                                the three close, asserted there. A `childTopWalks` growing with the square of a
                                container's children is one layout per box. `placements / childTopWalks` is what
                                one walk pays for; `passes` counts whole-tree spans. `origin*` is CSS 2 §8.1 "Box
                                dimensions"' border-box origin, derived from the containing block's (§10.1 second
                                case), in or out of a render; read `originDerived` against a document's depth.
                                `box*` is CSS 2.1 §10.6.3's contribution to a parent, taken over the one baseline
                                pass the record serves (§10.8.1). `cbWidth*` is CSS 2.1 §10.1 "Definition of
                                'containing block'"'s width (fourth case), the extent to `origin*`'s point through
                                a separate chain. Each `*Asks == *Served + *Derived` closes and is asserted. */
                             "\"_layout\":{\"childTopAsks\":%lld,\"childTopServed\":%lld,"
                             "\"childTopWalks\":%lld,\"placements\":%lld,\"passes\":%lld,"
                             "\"originAsks\":%lld,\"originServed\":%lld,\"originDerived\":%lld,"
                             "\"boxAsks\":%lld,\"boxServed\":%lld,\"boxDerived\":%lld,"
                             "\"cbWidthAsks\":%lld,\"cbWidthServed\":%lld,\"cbWidthDerived\":%lld},"
                             /* What the run spent on css-cascade-5 §4.2 "Cascaded Values".
                                core/css/css_cascade_pass.h states the two shapes that compound: §7.2 inheritance
                                makes an inherited read climb, and css-logical-1 §4 makes two inherited properties
                                a prerequisite of every box-side cascade. All four are lifetime counts, so the
                                suffix says so; the record's live size is not among them because it could not be.
                                `asksLife == servedLife + resolvedLife` is asserted by css_cascade_pass_census
                                before copying. `passesLife` tells a never-consulted record (no caller) from one
                                whose keys never repeat. css_cascade_pass_open has one caller,
                                core/paint/document_paint.c, so off a paint — every path the shipped extension
                                takes — `passesLife` and `servedLife` are 0; a nonzero `servedLife` is quoted with
                                the driver that painted. */
                             "\"_cascade\":{\"asksLife\":%lld,\"servedLife\":%lld,"
                             "\"resolvedLife\":%lld,\"passesLife\":%lld},"
                             /* …and what all of the above was denominated in: a property of the host that decides
                                whether two of these documents may be compared at all. result.h and
                                solver/quantum.h state the argument; nothing in this file composes it. */
                             "\"_quantum\":%s,\"_park\":%s}",
                     eps, sinks, errs, errsRetracted, errsExplored,
                     engine_switch_count(), flow_created_count(), solve_candidate_count(),
                     engine_jobs_queued(), engine_jobs_run(), engine_units_done(), held, made, segf,
                     routedDelivered, routedRefused, routedZeroDelivery,
                     routedEnds[ROUTED_TASK_FIRED], routedEnds[ROUTED_TASK_TARGET_ORIGIN],
                     routedEnds[ROUTED_TASK_TARGET_GONE], routedEnds[ROUTED_TASK_THREW],
                     scensus, schema,
                     orphansDriven, orphansAsked,
                     orphanExits.memo, orphanExits.empty, orphanExits.took,
                     (unsigned long long)orphanWalk.walks, (unsigned long long)orphanWalk.entries,
                     (unsigned long long)orphanWalk.walks_full,
                     (unsigned long long)orphanWalk.full_candidates,
                     wfq, cold, heap, swap, forkAt, absent,
                     place.asks, place.served, place.walks, place.placements, place.passes,
                     place.origin_asks, place.origin_served, place.origin_derived,
                     place.box_asks, place.box_served, place.box_derived,
                     place.cb_width_asks, place.cb_width_served, place.cb_width_derived,
                     casc.asks_life, casc.served_life, casc.resolved_life, casc.passes_life,
                     quantum,
                     cold_park_json());
        free(scensus);
        free(schema);
    }
    free(eps);
    free(sinks);
    free(errs);
    free(errsRetracted);
    free(errsExplored);
    free(wfq);
    free(cold);
    free(heap);
    free(swap);
    free(forkAt);
    free(absent);
    free(quantum);
    return out;
}
