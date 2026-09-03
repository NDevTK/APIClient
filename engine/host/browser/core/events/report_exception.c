/* HTML §8.1.4.6 "REPORT AN EXCEPTION" — the algorithm three callers needed and none of them could reach.
 *
 * DOM §2.9's inner invoke step 2.11 says a listener that throws has its exception REPORTED and the walk
 * CONTINUES. HTML §8.12 Animation frames says the same of an animation-frame callback, and RESIZE OBSERVER §3.4.6's "deliver
 * resize loop error" is the identical fire. Without this component each of those had exactly two options —
 * unwind the machine (which skips the rest of the algorithm and swallows the exception with nothing anywhere
 * to say a page threw) or swallow it silently — and rendering.c carried a DFAIL naming this file rather than
 * choosing either.
 *
 * IT IS A REQUEST, not a call, because step 6.2 FIRES AN EVENT and firing one runs the page's `onerror`. The
 * work record belongs to the CALLING machine, exactly as abort.h's AbortSignalWork does: a fork mid-report must
 * not hand two arms one dispatch, and the caller is what has a `visit`.
 *
 * THE STEP NUMBERS IN THIS FILE WERE ONE BEHIND THE STANDARD, and every one of them is fixed here. §8.1.4.6
 * takes `optional boolean omitError (default false)` and spends step 5 on it ("if omitError is true, then set
 * errorInfo[error] to null"), which moved the error-reporting-mode block from step 5 to step 6 and the
 * notHandled block from step 6 to step 7. A stale step number is the stale-DFAIL failure exactly — it stays
 * true about the algorithm the day it was written and goes wrong about the STANDARD, so it reads as
 * authoritative while naming a step that is now something else — and the stage labels below are resolved by a
 * cross-session resume, so here it would be resolved against the wrong step rather than merely read.
 *
 * WHAT IS NOT MODELLED, AND WHY THAT IS NOT A STUB. errorInfo's `message`, `filename`, `lineno` and `colno` are
 * IMPLEMENTATION-DEFINED by the standard's own words ("implementation-defined values derived from exception"),
 * so deriving the message from the exception without a script position is a permitted implementation and not a
 * gap. `error` is the exception itself and is the one the standard pins. What each of the standard's other
 * steps does here is stated once, at the stage declaration below, rather than twice. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/agent_state.h"
#include "core/idl_slots.h"
#include "core/events/error_event.h"
#include "core/events/event_target.h"
#include "core/events/report_exception.h"
#include "core/realm.h"

/* WHERE A PARKED REPORT IS, AS A STEP OF §8.1.4.6 RATHER THAN AS A PRIVATE INTEGER.
 *
 * This record rested at the bare integers 0 and 1. JSTrampStepDef::steps refuses that for a DEFINITION — an
 * index means nothing to the build that resumes a parked flow, so the rest point a machine holds has to be a
 * LABEL — and nothing refuses it for a WORK RECORD, which is the only reason it survived here. A record is not
 * exempt: it suspends inside the page's `error` listeners, it rides the calling machine's snapshot to the cold
 * tier and back, and the caller's own stage says only "report an exception" and cannot say which step of it.
 *
 * THE EXTRACT IS THE STAGE THIS RECORD DID NOT HAVE. Step 2's extract derives the message, filename, lineno and
 * colno the standard leaves implementation-defined, and this engine derives the last three by RENDERING THE
 * BACKTRACE the exception carries — one CallSite per frame, so the work is the page's own recursion depth. That
 * sat inside what was the entry span, ahead of the first guard, which is the one place a stage's body can hide.
 * It is its own stage and it ends in JS_STEP_YIELD, so the scheduler is ASKED between the walk and step 6
 * instead of being told the whole head is one uninterruptible step.
 *
 * STEPS 3 AND 4 ARE PERFORMED AND ARE THE IDENTITY, which is not the same as being skipped. Step 3 says "let
 * script be a script found in an implementation-defined way, OR NULL"; this agent finds null, which is the
 * standard's own alternative, and step 4's condition ("script is a classic script and script's muted errors is
 * true") is therefore false. The day the classic-script loader records muted errors for a cross-origin script
 * fetched without CORS, step 4 is read here and nowhere else. Step 5 is `omitError`, whose only true is passed
 * by step 7.2 — see the assertion at step 7.
 *
 * A STAGE THAT IS NOT A REST POINT IS NOT DECLARED. Step 6.3 (set the mode back to false) and step 7 run in the
 * arm the fire ENDS in, because the algorithm does not stop there — the label names where the record is
 * re-entered, and nothing re-enters it after the dispatch has answered. */
#define RX_STAGES(X)                                                                                        \
    X(RX_EXTRACT, "HTML §8.1.4.6 step 2 (errorInfo is the result of extracting error information from "      \
                  "exception, whose own step 3 derives message, filename, lineno and colno — here by "       \
                  "rendering the backtrace the exception carries, one CallSite per frame)")                  \
    X(RX_MODE,    "HTML §8.1.4.6 steps 6-6.1 (global is not in error reporting mode; set global's in error "  \
                  "reporting mode to true — the range is one O(1) read and one O(1) write of one flag on "   \
                  "the global, and the test and the set are the one operation the standard states)")         \
    X(RX_FIRE,    "HTML §8.1.4.6 step 6.2 (notHandled is the result of firing an event named error at "      \
                  "global, using ErrorEvent, with the cancelable attribute initialized to true and "         \
                  "additional attributes initialized according to errorInfo)")
enum { RX_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const RX_STEPS[] = { RX_STAGES(JS_STEP_STAGE_LABEL) NULL };
#define RX_STAGE_COUNT ((int)(sizeof RX_STEPS / sizeof *RX_STEPS) - 1)
/* ONE NAME FOR THE ALGORITHM THESE STAGES ARE STEPS OF — the dispatch's abort and the teardown's assertion
   share it, so neither can name a different algorithm from the other. */
#define RX_ALGORITHM "HTML §8.1.4.6 report an exception"

/* §8.1.4.6 step 6's IN ERROR REPORTING MODE, which is a flag on the GLOBAL and not on this component: a report
   whose own `error` listener throws must not report that throw recursively, and "recursively" is per global.
   It hangs off the global under a private Symbol, which is also what makes it per-flow for free — a flag set by
   one forked arm is a property write the COW delta captures.
   THE INITIALISER IS EXPLICIT because core/agent_state.h's SLOT_VALUE pre-init is `JS_IsUndefined`, and a bare
   static JSValue is all-zero bytes rather than JS_UNDEFINED — the declared kind and the static's actual
   pre-init state would then disagree on a component that had been declared and never released. */
static JSValue g_key = JS_UNDEFINED;
static int g_ready;

/* §8.1.4.6 STEP 7.3'S DEVELOPER CONSOLE, which is the HOST's and not this file's — the same shape and the same
   argument as unhandled_rejection.h's report hook, because it is the same sentence of the same standard read
   one section apart. A host that publishes a result document writes the message into it; one whose output is a
   stream of lines prints it as it happens. Registering none is a positive statement ("this host has no
   console"), never a hole: step 7 is performed either way and the hook only decides whether anything says so.
   THE FUNCTION IS THE HOST'S AND OUTLIVES THE AGENT; THE REGISTRATION IS THE AGENT'S AND DOES NOT — which is
   why report_exception_free gives it back and why it is declared to core/agent_state.h as a POINTER slot. The
   pointer is a link-time constant in the program's text (solver/result.c's result_page_error_value, installed
   by solver/engine.c at the session's begin); what is agent-scoped is this static being non-null, and a
   registration carried past the agent is what would make the next host's claim read as a SECOND, different
   claimant to the DCHECK below. */
static void (*g_console)(JSContext *ctx, JSValueConst exception);

/* §8.1.4.4 step 8's third bullet, as a machine this runtime can be handed — see report_exception_flow. */
static int g_flow_stepid = -1;

void report_exception_set_console_hook(void (*fn)(JSContext *ctx, JSValueConst exception))
{
    /* THE SAME CLAIMANT MAY SAY SO AGAIN, AND A DIFFERENT ONE MAY NOT. A host that opens a second session on
       one instance registers the same console a second time, which is one claim restated; two DIFFERENT
       non-null claimants is one report written to a console nobody is reading, because only the second
       survives. */
    DCHECK(fn == NULL || g_console == NULL || g_console == fn,
           "two different hosts claimed §8.1.4.6 step 7.3's developer console — only the second claimant's is "
           "reachable, so every report the first was registered for goes to a console nothing reads");
    g_console = fn;
}

void report_exception_init(JSContext *ctx)
{
    DCHECK(!g_ready, "report_exception_init ran twice — the key is one per AGENT");
    g_key = JS_NewSymbol(ctx, "inErrorReportingMode", false);
    CHECK(!JS_IsException(g_key), "the error-reporting-mode key allocation failed");
    g_ready = 1;
    /* WHAT THIS COMPONENT HOLDS FOR THE AGENT, DECLARED. All four were given back by the release below and
       declared to nobody — the released half of a pairing that had only one side, which is a slot
       agent_state_check_released holds nothing to assert. `g_flow_stepid` is declared here even though
       report_exception_flow is what SETS it: the declaration is the standing claim that this static is agent
       state, and the question it makes askable ("is it back at -1 when the agent goes?") has an answer whether
       or not any classic script of this agent completed abruptly. */
    agent_state_flag("report_exception", &g_ready,
                     "HTML §8.1.4.6 Runtime script errors' declaration latch");
    agent_state_value("report_exception", &g_key,
                      "the private Symbol HTML §8.1.4.6 Runtime script errors step 6's `in error reporting "
                      "mode` flag hangs off the global by");
    agent_state_id("report_exception", &g_flow_stepid,
                   "HTML §8.1.4.4 Calling scripts, run a classic script step 8's third bullet, as a step "
                   "machine registered with this runtime");
    /* A POINTER INTO ANOTHER COMPONENT, which is what agent_state_ptr exists for — the same shape and the same
       argument as core/html/unhandled_rejection.c's report hook, because it is the same sentence of the same
       standard read one section apart. */
    agent_state_ptr("report_exception", &g_console,
                    "the host edge HTML §8.1.4.6 step 7.3's developer console is written through");
}

/* THE RUNTIME, NOT A REALM — core/platform.h's release column. This was a line each of the three hosts wrote
   out by hand, AFTER platform_agent_free had already run that whole column, and they did not agree on where:
   main.c and test_forced.c ran `realm_intrinsics_free(); report_exception_free(ctx);` while wpt_runner.c ran
   `report_exception_free(ctx); … realm_intrinsics_free();`. Nothing was missing and the order was still three
   answers — and out there NONE of the four slots above could be declared at all, because a row with agent
   state and an empty release column is exactly what platform_check_agent_state fires on. What this gives back
   is the AGENT's: a private Symbol, a step-machine registration and a host edge. */
void report_exception_free(JSRuntime *rt)
{
    /* NOT `if (!g_ready) return;`. The release is the inverse of the DECLARATION and rides the same row of the
       same list — core/platform.c's declare loop runs `d_report_exception` unconditionally — so the test could
       never be true and what it could do was hide a release that left the latch set. */
    DCHECK(g_ready, "HTML §8.1.4.6 Runtime script errors was released in an agent that never declared it — the "
                    "release column is the inverse of the declare column, so reaching here without an init "
                    "means this component was torn down by something that is not the platform's one list");
    JS_FreeValueRT(rt, g_key);
    g_key = JS_UNDEFINED;
    g_ready = 0;
    /* THE STEP DEFINITION NAMES A RUNTIME THAT IS GOING AWAY. `JS_RegisterStepDef` hands out an index into the
       runtime's own tail, so an id kept across a teardown is an index into a table the next agent has not
       built — and report_exception_flow's own `if (g_flow_stepid < 0)` is precisely what would read it. */
    g_flow_stepid = -1;
    /* …AND SO DOES THE CONSOLE. It is the HOST's function and outlives this agent, but the registration is the
       agent's: giving it back is the inverse of the claim, and a hook left standing is what would make the
       next host's claim read as a second, different one. */
    g_console = NULL;
}

static bool reporting_mode(JSContext *ctx, JSValueConst global, int set, bool on)
{
    JSAtom k;
    JSValue v;
    bool was;

    DCHECK(g_ready, "the error reporting mode was asked for before report_exception_init ran");
    k = JS_ValueToAtom(ctx, g_key);
    if (k == JS_ATOM_NULL)
        return false;
    /* AN OWN SLOT, never a property LOOKUP: a miss on the global is the solver's absent-state seam, which mints
       a concolic for a name nobody defined — and a concolic flag would fork the report. */
    if (JS_GetOwnSlot(ctx, &v, global, k) <= 0)
        v = JS_UNDEFINED;
    was = JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);
    if (set)
        JS_SetProperty(ctx, (JSValue)global, k, JS_NewBool(ctx, on));
    JS_FreeAtom(ctx, k);
    return was;
}

void report_exception_work_start(ReportExceptionWork *w)
{
    w->stage = RX_EXTRACT;
    w->reporting = 0;
    w->phase = 0;
    w->ev = JS_UNDEFINED;
    w->cb[0] = w->cb[1] = w->cb[2] = w->cb[3] = JS_UNDEFINED;
}

void report_exception_work_visit(JSContext *ctx, ReportExceptionWork *w, JSStepVisit *v)
{
    int i;
    v->val(ctx, &w->ev);
    for (i = 0; i < 4; i++)
        v->val(ctx, &w->cb[i]);
}

void report_exception_work_unlock(JSContext *ctx, ReportExceptionWork *w)
{
    /* THE FLAG IS A LOCK, SO ITS OWNER HAS TO RELEASE IT ON EVERY EXIT. A report that is ABANDONED — the flow
       it belongs to is dropped, or the dispatch machine holding it is torn down while the `error` event is
       still in flight — would otherwise leave the global in error reporting mode forever, and step 6 then
       silently skips EVERY later report on that global. That is invisible: the first exception is reported and
       every one after it is swallowed, which reads exactly like "reporting does not work sometimes".
       IT IS ITS OWN FUNCTION because it is the only part of this record's teardown a `visit` cannot express: a
       global flag is not a reference, so no declaration names it. Everything else here IS named by the visit
       above, and a teardown that restated it would be the second list.
       IT ASKS ABOUT THE LOCK, NOT ABOUT THE STAGE, and the stages it would have been asking about are now
       declared: `if (w->stage != 0)` reads RX_MODE — the stage that has finished the extract and has NOT yet
       taken the mode — as a record that owes the flag back, and would clear a mode some other report is holding.
       That is what a negation over the stages always is: a claim about every stage that is not the first, made
       before those stages exist. The lock is not a step of the algorithm, it is a thing the algorithm HOLDS. */
    DCHECK(w->stage < RX_STAGE_COUNT,
           RX_ALGORITHM " abandoned a report at a cursor its stage declaration does not name — a record torn "
           "down at a stage nobody declares is one whose step 6.1 lock nobody can say was taken");
    if (w->reporting) {
        JSValue g = JS_GetGlobalObject(ctx);
        reporting_mode(ctx, g, /*set*/ 1, false);
        JS_FreeValue(ctx, g);
        w->reporting = 0;
    }
    /* A TEARDOWN IS NOT A TRANSITION, which is why this is an assignment and not a STEP_GOTO: the record is
       abandoned wherever it stood, so its fire request's cursor is legitimately mid-flight and the assert
       STEP_GOTO makes — that no sub-sequence is left in flight across a stage move — is about a machine that
       CONTINUES. What this restores is the start state, so a record reached again is asked from its first step
       rather than resumed into a dispatch nobody is holding. */
    w->stage = 0;
}

void report_exception_work_release(JSContext *ctx, ReportExceptionWork *w)
{
    report_exception_work_unlock(ctx, w);
    report_exception_work_visit(ctx, w, JS_StepFreeVisitor());
}

/* ONE FRAME OF THE RENDERING `js_callsite_data_line` WRITES, DECODED BY THAT RENDERING'S OWN GRAMMAR — which
   is three shapes and not one:
       at <file>[:<line>:<col>]                       a record with no function at all (a parse error's frame)
       at <name> (native)                             a frame that is not a script and HAS no position
       at <name> ([<eval origin>, ]<file>[:<line>:<col>])
   `true` only when the frame carries a SCRIPT POSITION, which is exactly the condition the renderer itself
   writes `:<line>:<col>` under (`csd->line_num != -1`), so the two agree by construction. [*fb,*fe) is the
   filename inside the string the caller still owns; `pline`/`pcol` are written only on a true return.
   TWO THINGS ARE MATCHED RATHER THAN SCANNED FOR, AND BOTH USED TO BE WRONG. The location group is found by
   MATCHING BACK from the `)` the line ends with, never by the first `(` in the line — an eval origin is
   itself parenthesised (`eval at f (a.js:1:1)`) and is rendered INSIDE the same group, so the first `(` opens
   the origin and the filename came out as `eval at f (a.js:1:1), b.js`. And the origin is stripped at the
   LAST `, ` at paren depth zero, which is the separator `js_callsite_data_line` joins the two with.
   A LINE THIS CANNOT READ IS NOT A FRAME AND IS SKIPPED, never asserted on: `JS_GetErrorStackString` answers a
   non-Error from its own `stack` property, and `throw {stack:"anything"}` is a page's to write. */
static bool frame_site(const char *b, const char *e, const char **fb, const char **fe,
                       uint32_t *pline, uint32_t *pcol)
{
    const char *gb, *ge, *p, *q, *cn, *ln_end;
    int depth;

    while (b < e && (*b == ' ' || *b == '\t')) b++;
    while (e > b && (e[-1] == '\r' || e[-1] == ' ')) e--;
    if ((size_t)(e - b) < 3 || strncmp(b, "at ", 3) != 0)
        return false;
    b += 3;

    if (e > b && e[-1] == ')') {
        depth = 0;
        for (q = e - 1;; q--) {
            if (*q == ')') depth++;
            else if (*q == '(' && --depth == 0) break;
            if (q == b) return false;          /* unbalanced: not this rendering's line */
        }
        gb = q + 1; ge = e - 1;
    } else {
        gb = b; ge = e;
    }

    /* `:<line>:<col>` sits at the very END of the location and nowhere else, so it is read from the right.
       `native` has neither, which is how a native frame falls out here rather than at a name comparison. */
    p = ge;
    while (p > gb && p[-1] >= '0' && p[-1] <= '9') p--;
    if (p == ge || p == gb || p[-1] != ':') return false;
    cn = p;
    p--;
    ln_end = p;
    while (p > gb && p[-1] >= '0' && p[-1] <= '9') p--;
    if (p == ln_end || p == gb || p[-1] != ':') return false;

    *fb = gb;
    *fe = p - 1;
    depth = 0;
    for (q = gb; q + 1 < *fe; q++) {
        if (*q == '(') depth++;
        else if (*q == ')') { if (depth > 0) depth--; }
        else if (depth == 0 && q[0] == ',' && q[1] == ' ') *fb = q + 2;
    }
    *pline = (uint32_t)strtoul(p, NULL, 10);
    *pcol  = (uint32_t)strtoul(cn, NULL, 10);
    return true;
}

/* THE THROW SITE, OUT OF THE BACKTRACE THE ENGINE ALREADY RECORDED — the other three of step 2's
   implementation-defined values. They were zero and an empty string, which is a conforming answer and is also
   a worse one than the engine can give: an Error carries a CallSite per frame with its line and its column,
   and JS_GetErrorStackString renders them WITHOUT running any of the page's code (which is the same reason the
   message goes through JS_DiagCString). A value with no backtrace — every non-Error a page can throw — leaves
   all three at their defaults rather than being guessed at.
   WPT reads all three off the event (`lineno` and `colno` are asserted to be greater than zero in six of
   dom/observable's subtests alone), so a fabricated number would be a wrong answer dressed as a right one and
   a zero is a right answer that loses information the engine has.
   IT IS THE FIRST *ADDRESSED* FRAME AND NOT THE TOP ONE, WHICH IS WHAT THESE THREE VALUES MEAN. §8.1.4.6's
   extract derives all three together and they name ONE site; a site is a place in a SCRIPT, and a builtin has
   none — `CustomElementRegistry.define` throwing §4.13.4's SyntaxError renders `at CustomElementRegistry.define
   (native)` on top of the page frame that called it, and reading the top frame answered "no throw site" with
   the address one line below it. That is not a smaller answer, it is a WRONG one: the address is what a reader
   partitions a run's errors by, so a staged error and a real one became one population. Every engine that
   reports a position does this walk (V8 computes its message location from the first frame that HAS a script,
   which is why Chrome's `onerror` for `JSON.parse('{')` names the calling script and not the parser), and it
   is not a heuristic here either — the renderer emits a position for exactly the frames that have one.
   AND A TRACE WITH NO ADDRESSED FRAME AT ALL ANSWERS `""`, WHICH IS A STATEMENT AND NOT A HOLE. §8.1.4.6's own
   muted-errors branch sets errorInfo[filename] to the empty string, so that is the standard's vocabulary for
   "no filename", and it is the honest answer for an exception raised entirely inside the engine: a
   `releaseLock()` rejection has one native frame and no script anywhere under it. The hosts print it as its own
   population rather than folding it into an absent field. */
/* THE ADDRESS IS RETURNED, NOT COPIED INTO THE CALLER'S BUFFER, AND THAT IS THE WHOLE OF THIS CHANGE. It took
   a `char *file` and a `size_t file_cap` and did `if (n >= file_cap) n = file_cap - 1;` — a silent truncation
   at whatever length the caller happened to declare, which both callers declared as 512. A script URL is not
   bounded by 512 bytes: a `data:` URL is a whole program, and a bundler's `sourceURL` can be any length at
   all. What came out was a PREFIX OF AN ADDRESS THAT STILL PARSES AS AN ADDRESS — indistinguishable from a
   real one, carrying no mark of having been cut, and this is the value a reader partitions a run's errors by,
   so two different scripts sharing a 511-byte prefix become one population. It is the defect the throw-site
   work exists to end, one column over: a plausible datum manufactured by a consumer's own storage decision.
   NOT A DCHECK, and not a bigger buffer. A very long script URL is legitimate PAGE DATA, so aborting on it is
   asserting about the document rather than about the engine, and any fixed size is the same bug at a different
   length. The representation is what has to stop being able to truncate, which is why this changes the
   signature and both call sites rather than a constant.
   `""` STAYS THE POSITIVE ANSWER for a value with no throw site (see the header) — the return is always an
   owned string and never NULL, so absence and emptiness remain ONE fact rather than two the caller must tell
   apart. The allocation is a CHECK because it is an allocation: a diagnostic that silently answers "no throw
   site" because a malloc failed would be exactly the fabricated datum this function just stopped producing. */
char *report_exception_position(JSContext *ctx, JSValueConst exception, uint32_t *pline, uint32_t *pcol)
{
    JSValue stack;
    const char *s, *b, *e, *fb = NULL, *fe = NULL;
    uint32_t line = 0, col = 0;
    char *file = NULL;

    DCHECK(pline != NULL && pcol != NULL,
           "§8.1.4.6's throw site was asked for into no out-parameter — the three values are derived together "
           "and a caller that wants one of them still owns storage for all three");
    *pline = 0; *pcol = 0;
    stack = JS_GetErrorStackString(ctx, exception);
    if (JS_IsString(stack)) {
        s = JS_ToCString(ctx, stack);
        JS_FreeValue(ctx, stack);
        /* THE THREE ARE PUBLISHED TOGETHER OR NOT AT ALL — they are ONE site, so a frame that yields a position
           and no name must not leave its line behind for the next frame's answer to be read beside. */
        for (b = s; s && *b; b = (*e == '\n') ? e + 1 : e) {
            e = b + strcspn(b, "\n");
            if (!frame_site(b, e, &fb, &fe, &line, &col) || fe == fb)
                continue;
            file = malloc((size_t)(fe - fb) + 1);
            CHECK(file != NULL, "§8.1.4.6's filename could not be allocated — a diagnostic that answered "
                  "\"no throw site\" here would be reporting a malloc failure as a fact about the page");
            memcpy(file, fb, (size_t)(fe - fb));
            file[fe - fb] = '\0';
            *pline = line;
            *pcol = col;
            break;
        }
        if (s) JS_FreeCString(ctx, s);
    } else {
        JS_FreeValue(ctx, stack);
    }
    if (!file) {
        file = malloc(1);
        CHECK(file != NULL, "§8.1.4.6's empty filename could not be allocated — the empty string is a POSITIVE "
              "answer here and a caller must be able to read it as one");
        file[0] = '\0';
    }
    return file;
}

/* §8.1.4.6's EXTRACT ERROR INFORMATION, materialized as the event whichever algorithm asked for it then fires.
   `error` is the exception; the other four are implementation-defined, and the message is derived WITHOUT
   running the page's `toString` — a host reporting what went wrong must not depend on the code that went wrong,
   which is exactly what JS_DiagCString is for. report_exception.h states why the ErrorEvent IS this engine's
   errorInfo, and names the SECOND caller this is exported for: §7.2.6.8's abort a NavigateEvent, whose step 4
   extracts and whose step 6 fires `navigateerror` with the result.
   Steps 3-5 of report-an-exception are the only writers that standard puts between its own step 2 and its step
   6.2, and all three are the identity here — see the stage declaration. */
JSValue extract_error_information(JSContext *ctx, JSValueConst exception, const char *type, bool cancelable)
{
    char *owned = NULL;
    const char *what = JS_DiagCString(ctx, exception, &owned);
    JSValue message = JS_NewString(ctx, what ? what : "Uncaught exception");
    char *path;
    uint32_t line = 0, col = 0;
    JSValue filename;
    JSValue ev;

    path = report_exception_position(ctx, exception, &line, &col);
    filename = JS_NewString(ctx, path);
    free(path);
    JS_DiagFreeCString(ctx, what, owned);
    ev = error_event_new(ctx, type, cancelable, message, filename, line, col, exception);
    JS_FreeValue(ctx, message);
    JS_FreeValue(ctx, filename);
    return ev;
}

int report_exception_run(JSContext *ctx, ReportExceptionWork *w, JSValueConst exception, JSValue in,
                         JSValue **out_cb, int *out_argc)
{
    JSValue g = JS_GetGlobalObject(ctx);
    JSValueConst global = g;
    /* STEP 1: "let notHandled be true". It is above the dispatch because it is a statement about every entry —
       the fire is the only writer, and the entry that collects the fire's answer is the only entry in which
       step 7 reads anything but this. Code that legitimately runs on EVERY entry belongs here, which is a
       statement about it rather than an accident of where a guard happened to land. */
    bool not_canceled = true;
    int r;

    JS_FreeValue(ctx, g);   /* a realm owns its global for the realm's whole life — this is a borrow */

    STEP_DISPATCH(RX_STAGES, w->stage, RX_ALGORITHM, JS_STEP_ABRUPT);

    STEP_ARM(RX_EXTRACT);
    JS_FreeValue(ctx, in);   /* nothing has asked for anything yet, so this entry's answer belongs to nobody */
    /* STEP 2. It walks the backtrace, so the stage ENDS here and the scheduler is asked before step 6 rather
       than after the whole head has run. A mint that fails is an allocation failure and nothing else — this
       runs none of the page's code — which is why it is a CHECK and not a swallowed exception: returning 0 here
       left the caller continuing with a live throw in the context and a report that had reported nothing. */
    /* STEP 6.2's own two flags ride the extraction, because they are what the event IS: `error`, and "with the
       cancelable attribute initialized to true" — which is how an `onerror` returning true says it handled the
       exception, and which step 7 reads back as notHandled. */
    w->ev = extract_error_information(ctx, exception, "error", /*cancelable*/ true);
    CHECK(!JS_IsException(w->ev), "the ErrorEvent carrying §8.1.4.6 step 2's errorInfo could not be allocated");
    /* STEPS 3-5 are performed and are the identity — script is null, so step 4's condition is false, and this
       component takes no omitError, so step 5's is too. See the stage declaration for both, and step 7 for the
       assertion that names the day step 5 acquires a caller. */
    STEP_GOTO(w->stage, RX_MODE, &w->phase, NULL);
    return JS_STEP_YIELD;

    STEP_ARM(RX_MODE);
    JS_FreeValue(ctx, in);
    /* STEP 6: the whole of steps 6.1-6.3 happen only when the global is NOT already reporting. A re-entrant
       report skips them and arrives at step 7 with notHandled still step 1's true, which is what the standard
       says and is not the same as being finished. */
    if (reporting_mode(ctx, global, /*set*/ 0, false))
        goto not_handled;
    reporting_mode(ctx, global, /*set*/ 1, true);   /* step 6.1 */
    w->reporting = 1;                               /* and this record now owes it back — see the unlock */
    /* AND IT RETURNS RATHER THAN RUNNING ON. The fire is the next stage; a body that sets its stage and falls
       into the next arm has crossed a boundary the driver never saw, so the label would claim a rest point the
       engine cannot park at. JS_STEP_YIELD is what asks: the scheduler parks this report if a sibling flow
       outranks it and re-enters immediately if none does. */
    STEP_GOTO(w->stage, RX_FIRE, &w->phase, NULL);
    return JS_STEP_YIELD;

    STEP_ARM(RX_FIRE);
    /* STEP 6.2: fire `error` at the global, using ErrorEvent, cancelable. It is the SAME §2.9 dispatch every
       other fire in this engine uses — reached as a REQUEST because this caller can park. */
    r = event_target_fire_run(ctx, &w->phase, STEP_CB(w->cb), global, w->ev, JS_UNDEFINED, in,
                              &not_canceled, out_cb, out_argc);
    if (r)
        return r;
    reporting_mode(ctx, global, /*set*/ 1, false);      /* step 6.3 */
    w->reporting = 0;                                   /* given back at the step that gives it back */
not_handled:
    /* STEP 7's notHandled is `not_canceled`: a listener that called `preventDefault()` cancelled the event, and
       that is how a page says it handled the error. Step 7.1 nulls errorInfo[error], whose only readers are 7.2
       and 7.3; 7.2 is the WORKER branch — also the one caller in the standard that passes step 5's omitError
       true — and 7.3 is "the user agent MAY report exception to a developer console", which is the hook below.
       THE CONSOLE IS WHERE THE EXCEPTION'S NAME SURVIVES, and this file used to say this build had none. It
       has: solver/result.h's `pageErrors` is exactly a developer console — one line per distinct message,
       carried out of the run — and the sentence that denied it was written before the classic-script arm had
       any route through this algorithm at all. Every earlier caller (a §2.9 listener that threw, a custom
       element reaction, an animation-frame callback, an IntersectionObserver callback, an Observable) was
       therefore reporting into a fire and nowhere else: the page's `error` listeners ran and, if nothing was
       listening, the exception's message left no trace anywhere. That is not a smaller report than the classic
       arm's, it is the SAME step, so it belongs at this one site rather than at each caller.
       IT IS BEHIND notHandled, WHICH IS THE WHOLE OF WHAT `preventDefault()` BUYS A PAGE. A page that cancels
       the `error` event is doing its own reporting, so §8.1.4.6 stops here — and a console line written anyway
       would report as unhandled an exception the page handled. */
    if (not_canceled && g_console)
        g_console(ctx, exception);
    if (not_canceled)
        realm_awaits(ctx, "Worker",
                     "§8.1.4.6 step 7.2 queues a global task on the DOM manipulation task source, at the "
                     "worker's owner, that fires `error` at the Worker object using ErrorEvent and — if that "
                     "is not handled either — REPORTS the exception again for the owner's global with "
                     "omitError true. That parameter is step 5, which this component does not yet take, so "
                     "both land together: give report_exception_run an omitError argument that nulls the "
                     "ErrorEvent's `error` before the fire, and write 7.1 and 7.2 here. `Worker` is what makes "
                     "a DedicatedWorkerGlobalScope reachable, so with it in this build they are due");
    JS_FreeValue(ctx, w->ev);
    w->ev = JS_UNDEFINED;
    /* AND THE ALGORITHM IS BACK AT ITS FIRST STEP: the record a caller holds names the step it would be
       ENTERED at rather than the last one it rested at, so a record reached again is asked from step 2. */
    STEP_GOTO(w->stage, RX_EXTRACT, &w->phase, NULL);
    return 0;
}

/* ---- HTML §8.1.4.4 "Calling scripts", run a classic script step 8's third bullet ---------------------------
 *
 * "Otherwise, rethrow errors is false. Perform the following steps: 1. Report an exception given by
 * evaluationStatus.[[Value]] for script's settings object's global object. 2. Clean up after running script
 * with settings. 3. Return evaluationStatus."
 *
 * THE OTHER TWO BULLETS BELONG TO A CALLER THIS ENGINE DOES NOT HAVE YET, and saying which is the whole of why
 * this one is unconditional here. Step 8's first two bullets are both `rethrow errors is true` — the parameter
 * §8.1.4.4 defaults to FALSE and which only a caller that wants the throw back sets. §4.12.1.1 "Processing
 * model"'s execute-the-script-element passes nothing, so every `<script>` in every document takes this bullet;
 * the two that rethrow are §7.4.2.3.2's `javascript:` URL and the event-handler compile, neither of which
 * reaches this file. A day one of them does, `rethrow errors` becomes a PARAMETER of the classic-script entry
 * and the muted-errors arm ("throw a NetworkError DOMException") is written at the same site — which is also
 * where this engine's absence of script muted errors stops being the identity §8.1.4.6 step 3 permits.
 *
 * IT IS A FLOW AND NOT A QUEUED TASK, AND THE DIFFERENCE IS OBSERVABLE IN ONE LINE OF PAGE CODE. The report is
 * step 8.3.1 and "clean up after running script" is 8.3.2, whose own step 3 performs the MICROTASK CHECKPOINT.
 * So `<script>Promise.resolve().then(m); throw e</script>` runs the page's `error` listeners BEFORE `m`, and
 * anything that enqueued the report — a microtask lands behind everything the script already queued, a task
 * lands behind the whole checkpoint — would run them after. The report therefore has to be the very next thing
 * the flow does, which is what a FRAME is and what a queue entry cannot be.
 *
 * AND A FRAME IS ALSO WHAT MAKES IT SUSPEND. §Every-runtime-job-is-a-scheduler-flow: the listeners are the
 * page's code and may loop, await, fork or park to the cold tier, so the report is preemptible per opcode and
 * resumable at any depth exactly like the program that threw. A `JS_Call` from the scheduler would be a C
 * activation with no flow base under it — the drive-to-completion this engine aborts on. */
#define RXF_STAGES(X)                                                                                       \
    X(RXF_REPORT, "HTML §8.1.4.4 run a classic script step 8's third bullet, sub-step 1 (report an exception " \
                  "given by evaluationStatus.[[Value]] for script's settings object's global object)")
enum { RXF_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const RXF_STEPS[] = { RXF_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct JSReportFlow {
    JSStepHdr hdr;              /* FIRST — the driver writes the def and the operand bounds through it */
    uint8_t   started;
    ReportExceptionWork rep;    /* §8.1.4.6, as a request, held by the machine that asks for it */
} JSReportFlow;

static void js_report_flow_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    report_exception_work_visit(ctx, &((JSReportFlow *)st)->rep, v);
}

static JSValue js_report_flow_fini(JSContext *ctx, void *st, bool take_result)
{
    (void)take_result;
    /* §8.1.4.6 step 6.1's FLAG, if the report was abandoned holding it. Not a reference, so no declaration
       names it and the visit above is what releases the record's references. */
    report_exception_work_unlock(ctx, &((JSReportFlow *)st)->rep);
    /* §8.1.4.4 step 8.3.3 returns evaluationStatus, and nothing in this engine reads it: the flow's completion
       value is not the page's (a <script> has none) and the scheduler discards it. */
    return JS_UNDEFINED;
}

static int js_report_flow_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSReportFlow *s = st;
    int r;

    DCHECK(s->hdr.stage == RXF_REPORT,
           "§8.1.4.4 step 8's report resumed into a stage it does not have — this machine is ONE step of the "
           "standard and delegates the whole of §8.1.4.6 to the record it holds, which keeps its own");
    if (!s->started) {
        /* EVERY OWNED FIELD PLACED BEFORE THE FIRST THING THAT CAN FAIL — the failure path tears this state
           down through `fini`, which gives back exactly what the state holds and nothing else. */
        report_exception_work_start(&s->rep);
        s->started = 1;
    }
    r = report_exception_run(ctx, &s->rep, step_arg(&s->hdr, 0), cb_result, out_cb, out_argc);
    if (r)
        return r;   /* parked inside the `error` event's own dispatch, or at one of §8.1.4.6's rest points */
    return JS_STEP_DONE;
}

static const JSTrampStepDef js_report_flow_def = {
    sizeof(JSReportFlow), js_report_flow_step, js_report_flow_fini, 0, .visit = js_report_flow_visit,
    .algorithm = "HTML §8.1.4.4 run a classic script step 8's third bullet — report an abrupt completion",
    .steps = RXF_STEPS
};

JSValue *report_exception_flow(JSContext *ctx, JSValueConst exception)
{
    JSValue fn;
    JSValue *base;

    DCHECK(g_ready, "a classic script's abrupt completion asked for §8.1.4.4 step 8's report before this "
                    "component was declared — the error reporting mode has no key, so step 6 cannot be asked");
    if (g_flow_stepid < 0) {
        g_flow_stepid = JS_RegisterStepDef(JS_GetRuntime(ctx), &js_report_flow_def);
        CHECK(g_flow_stepid >= 0, "§8.1.4.4 step 8's report machine could not be registered with this runtime");
    }
    /* MINTED IN THE REALM THAT USES IT, per report. `js_call_c_function` takes the callee's own realm, so this
       object is what decides WHICH global §8.1.4.6 reports for — "script's settings object's global object",
       which is the realm the program was compiled in and not the one the scheduler happens to hold. A function
       held in a static would answer every document's report with the first document's ctx, which is the
       per-realm defect §Browser-half names; there is nothing to cache here anyway, since a report is the
       thing that just went wrong. */
    fn = JS_NewCFunction2(ctx, NULL, "reportAnException", 1, JS_CFUNC_step, g_flow_stepid);
    CHECK(!JS_IsException(fn), "§8.1.4.4 step 8's report callee could not be allocated");
    /* take_result FALSE — HTML §8.1.4.6 "Runtime script errors"' report an exception has no completion value
       to hand anybody: what its frame owes the scheduler is whether it ITSELF completed abruptly, which is
       the throw, and which arrives through `pres` either way. */
    base = JS_FlowNewCall(ctx, fn, JS_UNDEFINED, 1, &exception, false);
    JS_FreeValue(ctx, fn);   /* JS_FlowNewCall dup'd the callee and the argument into the frame */
    CHECK(base != NULL, "§8.1.4.4 step 8's report frame could not be allocated — the exception has already "
                        "been taken off the context, so there is no second chance to report it");
    return base;
}
