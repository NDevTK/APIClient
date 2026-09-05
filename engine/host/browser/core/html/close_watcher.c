/* HTML §6.10.2 "Close watcher infrastructure" — the manager, and every algorithm of that section: the four
 * that touch only the manager (notify, establish, destroy, active) and the three that RUN A WATCHER'S ACTIONS
 * (request to close, close, process close watchers). See close_watcher.h for why the manager is a per-realm
 * record, why its groups are JS Arrays, why a watcher names its three algorithms by a kind, why the second
 * three are a request a calling machine drives, and for the two named residuals that remain.
 *
 * THE ANTI-ABUSE ARITHMETIC IS THE SUBSTANCE OF THIS FILE, and §6.10.2 says so in its own note: "Most of the
 * complexity of the close watcher manager comes from anti-abuse protections designed to prevent developers
 * from disabling users' history traversal abilities". Two fields carry it. ALLOWED NUMBER OF GROUPS rises only
 * when the user interacts, so a page can hold at most "1 + the number of times the user activates the page"
 * independently-closable groups and every watcher beyond that JOINS THE LAST GROUP — one close request then
 * closes the whole group. NEXT USER INTERACTION ALLOWS A NEW GROUP is what stops interactions from BANKING:
 * §6.10.2's establish sets it true and its notify clears it, so two interactions in a row raise the allowance
 * once, and the standard spells the three cases out — "Allowed: user interaction; create a close watcher in
 * its own group; user interaction; create a close watcher in a second independent group." / "Disallowed: user
 * interaction; user interaction; create a close watcher in its own group; create a close watcher in a second
 * independent group."
 *
 * SO THE TWO FIELDS ARE NOT ONE FIELD, and collapsing them is the tempting error. A single counter would make
 * the second case legal, which is precisely the banking the note exists to prevent; and the boolean cannot be
 * derived from the counter, because whether an interaction has been spent on a group yet is a different
 * question from how many groups are allowed. They are written by different algorithms in different orders and
 * only their PAIR reproduces the standard's three cases.
 *
 * A NOTE ON WHAT §6.10.2's CANCEL-ACTION STRUCT ITEM SAYS, BECAUSE A READER WILL CHECK IT AND FIND IT
 * DISAGREEING WITH THIS CODE. The struct item reads, verbatim: "A cancel action, an algorithm accepting a
 * boolean argument and returning a boolean. … If the boolean argument is true, then the algorithm can return
 * either true to indicate that the caller will proceed to the close action, or false to indicate that the
 * caller will bail out. If the argument is false, then the return value is always false." The last sentence
 * cannot be right, and §6.10.2's own ALGORITHMS are what say so: both cancel actions that exist — §6.12's "to
 * return true" and §6.10.3's "return the result of firing an event named cancel at this, with the cancelable
 * attribute initialized to canPreventClose", which DOM §2.9 answers TRUE for because a non-cancelable event's
 * canceled flag can never be set — return TRUE for a false argument, and request to close step 10.1 asserts
 * "canPreventClose is true" on the arm a false return takes. Read the sentence as written and every close
 * request would bail out at step 10 into an assert that cannot hold. The ALGORITHMS decide, the prose is a
 * spec editorial defect, and step 10.1's assert is written below exactly as the standard writes it so that a
 * third kind whose cancel action really did return false for a false argument would abort here rather than
 * silently make every close request a no-op. */
#include <stdbool.h>
#include <stdint.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/realm.h"
#include "core/dom/document.h"
#include "core/events/event.h"
#include "core/events/event_target.h"
#include "core/html/close_watcher.h"
#include "core/html/html_dialog.h"
#include "core/html/close_watcher_interface.h"
#include "core/html/popover.h"
#include "core/html/user_activation.h"

/* The manager's three items, §6.10.2's own names. */
#define CW_GROUPS   "groups"
#define CW_ALLOWED  "allowedNumberOfGroups"
#define CW_NEXT     "nextUserInteractionAllowsANewGroup"

/* A close watcher's items. The three ALGORITHMS are the `kind` plus the `subject` they act on — see the
   header. "is running cancel action" is stored rather than derived because §6.10.2's request-to-close sets it
   around the cancel action precisely so a REENTRANT request (the page's own cancel handler calling
   requestClose again) returns early at its step 3 instead of recursing. */
#define CW_WINDOW   "window"
#define CW_KIND     "kind"
#define CW_SUBJECT  "subject"
#define CW_RUNNING  "isRunningCancelAction"

static int g_slot = -1;

/* THIS REALM'S §6.10.2 CLOSE WATCHER MANAGER. Owned — the caller frees. */
static JSValue cw_manager(JSContext *ctx)
{
    JSValue m = realm_value_get(ctx, g_slot);

    DCHECK(JS_IsObject(m),
           "a realm answered for its Window's HTML §6.10.2 Close watcher infrastructure close watcher manager "
           "with no record — every realm gets one from close_watcher_install_realm, so this is a realm built "
           "before close_watcher_init declared the slot");
    return m;
}

/* The manager's GROUPS — "a list of lists of close watchers", so an Array of Arrays. Owned. */
static JSValue cw_groups(JSContext *ctx, JSValueConst mgr)
{
    JSValue g = JS_GetPropertyStr(ctx, mgr, CW_GROUPS);

    DCHECK(JS_IsArray(g),
           "a close watcher manager's groups is not an Array — the record is built once per realm with an "
           "Array here and the only writers are this file's own appends and removals");
    return g;
}

static uint32_t cw_len(JSContext *ctx, JSValueConst arr)
{
    JSValue lv;
    uint32_t n = 0;

    DCHECK(JS_IsArray(arr), "a §6.10.2 list whose size was asked is not an Array");
    lv = JS_GetPropertyStr(ctx, arr, "length");
    JS_ToUint32(ctx, &n, lv);
    JS_FreeValue(ctx, lv);
    return n;
}

/* Infra's "append" onto one of the two list levels. */
static void cw_append(JSContext *ctx, JSValueConst arr, JSValueConst v)
{
    JS_SetPropertyUint32(ctx, (JSValue)arr, cw_len(ctx, arr), JS_DupValue(ctx, v));
}

/* The INDEX of an item in a list, or -1. IDENTITY, not equality: a close watcher is a struct and two distinct
   watchers with identical fields are two watchers, which is exactly what §6.10.2's grouping counts. */
static int64_t cw_index_of(JSContext *ctx, JSValueConst arr, JSValueConst v)
{
    uint32_t i, n = cw_len(ctx, arr);

    DCHECK(JS_IsObject(v), "a §6.10.2 list was searched for something that is not an object — a close watcher "
                           "and a group are both objects, and a pointer comparison against a non-object "
                           "compares a tag payload that is not an address");
    for (i = 0; i < n; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, arr, i);
        bool same = JS_VALUE_GET_PTR(e) == JS_VALUE_GET_PTR(v);

        JS_FreeValue(ctx, e);
        if (same) return (int64_t)i;
    }
    return -1;
}

/* REMOVE FROM THE MIDDLE OF A LIST — written as a shift plus a `length` write rather than through
   Array.prototype.splice, for core/css/top_layer.c's reason: splice is the PAGE's property, which a page may
   replace, and reaching for it would put the page's code inside a C activation with no flow base. */
static void cw_remove_at(JSContext *ctx, JSValueConst arr, uint32_t at)
{
    uint32_t i, n = cw_len(ctx, arr);

    DCHECK(at < n, "a §6.10.2 list removal was given an index past its end");
    for (i = at; i + 1 < n; i++)
        JS_SetPropertyUint32(ctx, (JSValue)arr, i, JS_GetPropertyUint32(ctx, arr, i + 1));
    JS_SetPropertyStr(ctx, (JSValue)arr, "length", JS_NewUint32(ctx, n - 1));
}

/* The manager's ALLOWED NUMBER OF GROUPS. */
static int32_t cw_allowed(JSContext *ctx, JSValueConst mgr)
{
    JSValue v = JS_GetPropertyStr(ctx, mgr, CW_ALLOWED);
    int32_t n = 0;

    DCHECK(JS_IsNumber(v), "a close watcher manager's allowed number of groups is not a number");
    JS_ToInt32(ctx, &n, v);
    JS_FreeValue(ctx, v);
    /* §6.10.2's establish states this invariant itself, at its step 5.1: "manager's allowed number of groups
       is always at least 1". Its two writers are notify's increment and process close watchers' decrement,
       and the latter is guarded by "if ... is greater than 1". */
    DCHECK(n >= 1, "a close watcher manager's allowed number of groups fell below 1 — HTML §6.10.2 Close "
                   "watcher infrastructure's establish step 5.1 asserts it is always at least 1, and its only "
                   "decrement is process close watchers' step 3, which is guarded by it being greater than 1");
    return n;
}

/* THE WATCHER BELONGS TO THE REALM IT IS BEING ACTED ON THROUGH — asserted at each site rather than inside a
   helper, per §AN-ASSERT-THAT-NAMES-A-REMEDY-BUT-NOT-A-SITE: expanded as a macro, the file and line a failure
   reports are the CALLER's, so "which algorithm was given a foreign watcher" is answered by the crash instead
   of by reading every call site. §6.10.2 reaches the manager through "closeWatcher's window", and this engine
   passes the realm instead — so the two must agree, and where they do not the caller has reached across a
   Window boundary with a struct that names a different manager than the one it is about to edit. */
#if APICLIENT_DEV
#define CW_ASSERT_WINDOW(wctx, watcher)                                                                       \
    do {                                                                                                      \
        JSValue cw__g = JS_GetGlobalObject(wctx);                                                             \
        JSValue cw__w;                                                                                        \
        bool cw__same;                                                                                        \
        DCHECK(JS_IsObject(watcher), "a §6.10.2 close watcher algorithm was given something that is not a "    \
                                     "close watcher — every watcher is minted by close_watcher_establish");   \
        cw__w = JS_GetPropertyStr((wctx), (watcher), CW_WINDOW);                                              \
        cw__same = JS_VALUE_GET_PTR(cw__w) == JS_VALUE_GET_PTR(cw__g);                                        \
        JS_FreeValue((wctx), cw__w);                                                                          \
        JS_FreeValue((wctx), cw__g);                                                                          \
        DCHECK(cw__same,                                                                                      \
               "a HTML §6.10.2 Close watcher infrastructure algorithm was given a close watcher established "  \
               "for a DIFFERENT Window than the realm it was called with. §6.10.2 reaches the manager through "\
               "\"closeWatcher's window\", so the manager this call is about to edit is not the one holding "  \
               "this watcher — pass the establishing Window's realm (§6.12 passes the element's relevant "     \
               "global object, §6.10.3 passes this's). A CALLER HOLDING ONLY THE WINDOW OBJECT CANNOT DO "     \
               "THAT YET, and that is the shape this fires in: an IDL member runs in the realm that DEFINED "  \
               "it, so `otherFrame.CloseWatcher.prototype.close.call(w)` reaches its body with the OTHER "     \
               "realm's ctx and a watcher belonging to this one. What that needs is an agent-wide map from a " \
               "Window's global object to its realm, which every such member would then resolve `this`'s "     \
               "window through instead of substituting its own");                                             \
    } while (0)
#else
#define CW_ASSERT_WINDOW(wctx, watcher) ((void)0)
#endif

/* ---- §6.10.2's four manager-only algorithms ---------------------------------------------------------------- */

/* "To NOTIFY THE CLOSE WATCHER MANAGER ABOUT USER ACTIVATION given a Window window", 3 steps. */
void close_watcher_notify_user_activation(JSContext *wctx)
{
    JSValue mgr = cw_manager(wctx);                                                            /* step 1 */
    JSValue next = JS_GetPropertyStr(wctx, mgr, CW_NEXT);

    DCHECK(JS_IsBool(next), "a close watcher manager's next-user-interaction-allows-a-new-group is not a "
                            "boolean — it is born true and its only writers are this step and establish's "
                            "step 6");
    /* Step 2 — "If manager's next user interaction allows a new group is true, then increment manager's
       allowed number of groups." THIS IS THE ONLY WAY THE ALLOWANCE RISES, which is the whole guarantee:
       §6.10.2's note bounds a page at "at most 1 + the number of times the user activates the page" groups. */
    if (JS_ToBool(wctx, next) > 0) {
        int32_t allowed = cw_allowed(wctx, mgr);

        DCHECK(allowed < INT32_MAX,
               "a close watcher manager's allowed number of groups reached INT32_MAX — it rises by one per "
               "user activation that a close watcher was established against, so this is a count of real "
               "interactions overflowing rather than anything this engine generated");
        JS_SetPropertyStr(wctx, mgr, CW_ALLOWED, JS_NewInt32(wctx, allowed + 1));
    }
    JS_FreeValue(wctx, next);
    /* Step 3 — cleared unconditionally, which is what makes a SECOND interaction with no establish in between
       raise nothing. Only establish's step 6 sets it back. */
    JS_SetPropertyStr(wctx, mgr, CW_NEXT, JS_FALSE);                                           /* step 3 */
    JS_FreeValue(wctx, mgr);
}

/* "To ESTABLISH A CLOSE WATCHER given a Window window, a list of steps cancelAction, a list of steps
   closeAction, and an algorithm that returns a boolean getEnabledState", 7 steps. */
JSValue close_watcher_establish(JSContext *wctx, CloseWatcherKind kind, JSValueConst subject)
{
    JSValue watcher, mgr, groups;
    uint32_t n;

    /* Step 1 — "Assert: window's associated Document is fully active." An assert in the standard is an assert
       here: every establisher reaches this having already refused a non-fully-active Document (§6.10.3's
       constructor throws "InvalidStateError" at its own step 1; §6.12's show popover cannot reach step 15
       without check popover validity having passed). */
    DCHECK(document_fully_active(wctx),
           "HTML §6.10.2 Close watcher infrastructure's establish a close watcher step 1 asserts that the "
           "Window's associated Document is fully active. A caller reaching here with one that is not has "
           "skipped its own guard — §6.10.3's constructor throws an InvalidStateError for this case and §6.12 "
           "show popover's check popover validity refuses it");
    DCHECK(kind >= 0 && kind < CLOSE_WATCHER_KIND_COUNT,
           "a close watcher was established with a kind outside close_watcher.h's registry — every kind names "
           "one establisher's three algorithms, and one added to the enum without them is a watcher the "
           "action dispatch cannot run");
    DCHECK(JS_IsObject(subject),
           "a close watcher was established with a subject that is not an object — the subject is what this "
           "kind's three algorithms act on (§6.12 passes the popover Element)");

    /* Step 2 — "Let closeWatcher be a new close watcher, with window / cancel action / close action / is
       running cancel action false / get enabled state". NULL PROTOTYPE: this is a spec struct and never a
       page-reachable object, so it gets nothing from Object.prototype and a page that somehow held one could
       not reach a method through it. */
    watcher = JS_NewObjectProto(wctx, JS_NULL);
    CHECK(!JS_IsException(watcher), "close watcher: OOM building a §6.10.2 close watcher");
    JS_SetPropertyStr(wctx, watcher, CW_WINDOW, JS_GetGlobalObject(wctx));
    JS_SetPropertyStr(wctx, watcher, CW_KIND, JS_NewInt32(wctx, (int)kind));
    JS_SetPropertyStr(wctx, watcher, CW_SUBJECT, JS_DupValue(wctx, subject));
    JS_SetPropertyStr(wctx, watcher, CW_RUNNING, JS_FALSE);

    mgr = cw_manager(wctx);                                                                    /* step 3 */
    groups = cw_groups(wctx, mgr);
    n = cw_len(wctx, groups);
    /* Step 4 — "If manager's groups's size is less than manager's allowed number of groups, then append
       « closeWatcher » to manager's groups." The « » is a LIST LITERAL: what is appended is a NEW GROUP
       containing this one watcher, which is what makes it independently closable.
       COMPARED IN int64_t, NOT THROUGH A CAST TO uint32_t. cw_allowed's "at least 1" is a DCHECK, which is
       compiled out in release — so a cast would turn a value that invariant excludes into a huge unsigned one
       and silently take this branch for ever, which is §A-DIFF-THAT-MAKES-A-DCHECKED-VALUE-LOAD-BEARING-IN-
       RELEASE. A width that holds both operands exactly needs no invariant to be correct. */
    if ((int64_t)n < (int64_t)cw_allowed(wctx, mgr)) {
        JSValue group = JS_NewArray(wctx);

        CHECK(!JS_IsException(group), "close watcher: OOM building a §6.10.2 group");
        cw_append(wctx, group, watcher);
        cw_append(wctx, groups, group);
        JS_FreeValue(wctx, group);
    } else {                                                                                   /* step 5 */
        JSValue last;

        /* Step 5.1's own assert, in the standard's words: "manager's groups's size is at least 1 in this
           branch, since manager's allowed number of groups is always at least 1." */
        DCHECK(n >= 1,
               "HTML §6.10.2 Close watcher infrastructure's establish a close watcher step 5.1 asserts the "
               "manager's groups is non-empty on this branch, because the allowed number of groups is always "
               "at least 1 and this branch was taken on the size not being less than it");
        last = JS_GetPropertyUint32(wctx, groups, n - 1);
        DCHECK(JS_IsArray(last), "a close watcher manager's groups held something that is not a group");
        /* Step 5.2 — THE GROUPED CASE. This watcher joins the most recently created group, so one close
           request closes them together; §6.10.3's own example calls that out for two watchers created in one
           click handler. */
        cw_append(wctx, last, watcher);
        JS_FreeValue(wctx, last);
    }
    /* Step 6 — an establish SPENDS the pending interaction, so the next notify can raise the allowance again. */
    JS_SetPropertyStr(wctx, mgr, CW_NEXT, JS_TRUE);                                            /* step 6 */
    JS_FreeValue(wctx, groups);
    JS_FreeValue(wctx, mgr);
    DCHECK(close_watcher_is_active(wctx, watcher),
           "a close watcher this call had just placed in a group did not read back as ACTIVE — §6.10.2 defines "
           "active as the manager containing any list which contains it, and both branches above append it to "
           "exactly one list of the manager's groups");
    return watcher;                                                                            /* step 7 */
}

/* "To DESTROY A CLOSE WATCHER closeWatcher", 3 steps. */
void close_watcher_destroy(JSContext *wctx, JSValueConst watcher)
{
    JSValue mgr, groups;
    uint32_t i, n;

    CW_ASSERT_WINDOW(wctx, watcher);
    mgr = cw_manager(wctx);                                                                    /* step 1 */
    groups = cw_groups(wctx, mgr);
    n = cw_len(wctx, groups);
    /* Step 2 — "For each group of manager's groups: remove closeWatcher from group." Infra's remove takes out
       EVERY matching item, and establish puts a watcher in exactly one group, so the inner loop normally runs
       at most once; it is written as the standard's operation rather than as that expectation because the
       expectation is establish's invariant and not this algorithm's precondition. */
    for (i = 0; i < n; i++) {
        JSValue group = JS_GetPropertyUint32(wctx, groups, i);
        int64_t at;

        DCHECK(JS_IsArray(group), "a close watcher manager's groups held something that is not a group");
        while ((at = cw_index_of(wctx, group, watcher)) >= 0)
            cw_remove_at(wctx, group, (uint32_t)at);
        JS_FreeValue(wctx, group);
    }
    /* Step 3 — "Remove any item from manager's groups that is empty." WALKED BACKWARDS, because a removal
       renumbers every position after it: forwards, removing the group at i slides the next one into i and the
       loop's own increment then steps over it, so two adjacent empty groups would leave one behind. */
    for (i = n; i-- > 0; ) {
        JSValue group = JS_GetPropertyUint32(wctx, groups, i);
        bool empty = cw_len(wctx, group) == 0;

        JS_FreeValue(wctx, group);
        if (empty) cw_remove_at(wctx, groups, i);
    }
    JS_FreeValue(wctx, groups);
    JS_FreeValue(wctx, mgr);
    DCHECK(!close_watcher_is_active(wctx, watcher),
           "a close watcher still read as ACTIVE after destroy — step 2 removes it from every group of the "
           "manager, so a watcher the manager still contains is one this walk did not reach");
}

/* §6.10.2's "A close watcher closeWatcher is ACTIVE if closeWatcher's window's close watcher manager contains
   any list which contains closeWatcher." */
bool close_watcher_is_active(JSContext *wctx, JSValueConst watcher)
{
    JSValue mgr, groups;
    uint32_t i, n;
    bool found = false;

    CW_ASSERT_WINDOW(wctx, watcher);
    mgr = cw_manager(wctx);
    groups = cw_groups(wctx, mgr);
    n = cw_len(wctx, groups);
    for (i = 0; i < n && !found; i++) {
        JSValue group = JS_GetPropertyUint32(wctx, groups, i);

        DCHECK(JS_IsArray(group), "a close watcher manager's groups held something that is not a group");
        found = cw_index_of(wctx, group, watcher) >= 0;
        JS_FreeValue(wctx, group);
    }
    JS_FreeValue(wctx, groups);
    JS_FreeValue(wctx, mgr);
    return found;
}

/* ---- §6.10.2's THREE ACTIONS, DISPATCHED OVER THE WATCHER'S KIND -------------------------------------------
 *
 * §6.10.2's struct holds "a cancel action", "a close action" and "a get enabled state", each "an algorithm";
 * what an establisher does is SUPPLY them, and close_watcher.h says why a kind id can hold three algorithms
 * where a JS closure and a C function pointer cannot. This is the other end of that: the ONE place the id is
 * turned back into the algorithms, with one arm per establisher and every arm citing the section that DEFINES
 * its three rather than this one.
 *
 * THE SWITCHES ENUMERATE EVERY KIND AND HAVE NO `default`, WHICH IS THE FORCING FUNCTION. A `default` arm
 * would let a kind be added to close_watcher.h's enum and silently take somebody else's algorithms; with every
 * enumerator listed, -Wswitch names this file at the compile that adds one, and the DFAIL below the switch is
 * what catches a value that is not an enumerator at all in a build where cw_kind's range DCHECK is gone. */

/* THIS WATCHER'S KIND — the id §6.10.2's three algorithms are named by. */
static CloseWatcherKind cw_kind(JSContext *ctx, JSValueConst watcher)
{
    JSValue v = JS_GetPropertyStr(ctx, watcher, CW_KIND);
    int32_t k = -1;

    DCHECK(JS_IsNumber(v), "a close watcher's kind is not a number — establish writes one from "
                           "close_watcher.h's enum and nothing else writes this field");
    JS_ToInt32(ctx, &k, v);
    JS_FreeValue(ctx, v);
    DCHECK(k >= 0 && k < CLOSE_WATCHER_KIND_COUNT,
           "a close watcher carries a kind outside close_watcher.h's registry — the enum is fixed at its "
           "definition and only ever appended to, so a value past its end is a watcher established by a build "
           "whose registry this one does not have");
    return (CloseWatcherKind)k;
}

/* THE OBJECT THIS KIND'S THREE ALGORITHMS ACT ON — §6.12 passes the popover Element, §6.10.3 passes the
   CloseWatcher instance the events are fired at. OWNED: the caller frees. */
static JSValue cw_subject(JSContext *ctx, JSValueConst watcher)
{
    JSValue s = JS_GetPropertyStr(ctx, watcher, CW_SUBJECT);

    DCHECK(JS_IsObject(s), "a close watcher's subject is not an object — establish DCHECKs that at the one "
                           "site that writes it");
    return s;
}

/* §6.10.2's "is running cancel action" boolean, whose two writers are request to close's steps 7 and 9. */
static bool cw_running_cancel(JSContext *ctx, JSValueConst watcher)
{
    JSValue v = JS_GetPropertyStr(ctx, watcher, CW_RUNNING);
    bool b;

    DCHECK(JS_IsBool(v), "a close watcher's is-running-cancel-action is not a boolean — establish's step 2 "
                         "writes false and only request to close's steps 7 and 9 write it afterwards");
    b = JS_ToBool(ctx, v) > 0;
    JS_FreeValue(ctx, v);
    return b;
}

static void cw_set_running_cancel(JSContext *ctx, JSValueConst watcher, bool v)
{
    JS_SetPropertyStr(ctx, (JSValue)watcher, CW_RUNNING, v ? JS_TRUE : JS_FALSE);
}

/* §6.10.2's GET ENABLED STATE, "an algorithm accepting no arguments and returning a boolean. This algorithm
   can never throw an exception." — so it is a plain predicate and not a request. */
static bool cw_get_enabled_state(JSContext *wctx, JSValueConst watcher)
{
    switch (cw_kind(wctx, watcher)) {
    case CLOSE_WATCHER_KIND_POPOVER:
        return true;    /* §6.12 show popover step 15: "getEnabledState being to return true" */
    case CLOSE_WATCHER_KIND_CLOSE_WATCHER:
        return true;    /* §6.10.3 constructor step 2.3: "getEnabledState being to return true" */
    case CLOSE_WATCHER_KIND_DIALOG: {
        /* §4.11.4 set the dialog close watcher step 3: "getEnabledState being to return true if dialog's
           enable close watcher for request close is true or dialog's computed closed-by state is not None;
           otherwise false." THE ONLY ARM THAT IS NOT A CONSTANT, and the reason this dispatch asks the kind at
           all rather than answering `true` from one place. Both of its disjuncts are §4.11.4's own state, so
           the question is asked of that component; it runs no page code, which §6.10.2 requires of this
           algorithm in its own words ("This algorithm can never throw an exception"). */
        JSValue subject = cw_subject(wctx, watcher);
        bool enabled = html_dialog_close_watcher_enabled(wctx, subject);

        JS_FreeValue(wctx, subject);
        return enabled;
    }
    case CLOSE_WATCHER_KIND_COUNT:
        break;
    }
    DFAIL("a close watcher's GET ENABLED STATE was asked for a kind this dispatch has no arm for — every "
          "entry in close_watcher.h's CloseWatcherKind names one establisher's three algorithms, and an entry "
          "added without them reaches here");
    return true;
}

/* §6.10.2's CANCEL ACTION, "an algorithm accepting a boolean argument and returning a boolean" — the argument
   is request to close step 6's canPreventClose and the answer is its step 8's shouldContinue. It is a REQUEST
   because §6.10.3's arm fires an event, which is the page's own handlers. `in` is CONSUMED. */
static int cw_cancel_action_run(JSContext *wctx, CloseWatcherRun *r, JSValueConst watcher,
                                bool can_prevent_close, JSValue in, bool *pcontinue,
                                JSValue **out_cb, int *out_argc)
{
    CloseWatcherKind kind = cw_kind(wctx, watcher);
    bool not_canceled = false;
    JSValue subject;
    int rc;

    switch (kind) {
    case CLOSE_WATCHER_KIND_POPOVER:
        /* §6.12 show popover step 15: "cancelAction being to return true". It reads no argument and runs no
           page code, so this arm has no cursor and the caller's re-entry value is spent here. */
        JS_FreeValue(wctx, in);
        *pcontinue = true;
        return 0;
    case CLOSE_WATCHER_KIND_CLOSE_WATCHER:
    case CLOSE_WATCHER_KIND_DIALOG:
        /* TWO ESTABLISHERS, ONE ARM, AND IT IS ONE ARM BECAUSE THE TWO SENTENCES SAY THE SAME THING — not
           because the code happened to fit. §6.10.3's constructor step 2.1 supplies its cancelAction as the
           result of firing `cancel` at the CloseWatcher instance with the cancelable attribute initialized to
           canPreventClose, and §4.11.4's set the dialog close watcher step 3 supplies the same fire at the
           dialog element; the only thing that differs is which object the establisher passed as the subject,
           which is the one thing cw_subject already answers. (Neither sentence is quoted here, because a run
           of words assembled from two sections with a placeholder in the middle is not either standard's text
           and engine/citegen.mjs is right to say so.) A second copy would be two places for the cancelable
           initializer and the not-canceled reading to drift. */
        break;
    case CLOSE_WATCHER_KIND_COUNT:
        JS_FreeValue(wctx, in);
        DFAIL("a close watcher's CANCEL ACTION was asked for a kind this dispatch has no arm for");
        *pcontinue = true;
        return 0;
    }
    /* §4.11.4's set the dialog close watcher step 3 states the same shape over the dialog element, and it is
       the OTHER kind's establisher; the words below are §6.10.3 constructor step 2.1's, which is the one they
       are taken from: "cancelAction given canPreventClose being to return the result of firing an event named
       cancel at this, with the cancelable attribute initialized to canPreventClose."
       NOT BUBBLING: §6.10.3 names one initialiser for this fire and no others, so `bubbles` stays at DOM
       §2.2's default. The event is TRUSTED — DOM §2.9's fire is the user agent dispatching, which is exactly
       what separates it from one the page constructs. */
    if (JS_IsUndefined(r->ev)) {
        r->ev = event_new(wctx, "cancel", /*bubbles*/ false, /*cancelable*/ can_prevent_close);
        if (JS_IsException(r->ev)) { r->ev = JS_UNDEFINED; JS_FreeValue(wctx, in); return -1; }
    }
    subject = cw_subject(wctx, watcher);
    rc = event_target_fire_run(wctx, &r->fphase, STEP_CB(r->cb), subject, r->ev, JS_UNDEFINED, in,
                               &not_canceled, out_cb, out_argc);
    JS_FreeValue(wctx, subject);
    if (rc) return rc;
    DCHECK(r->fphase == 0, "the `cancel` fire answered with its own request still in flight");
    JS_FreeValue(wctx, r->ev);
    r->ev = JS_UNDEFINED;
    /* "Fire an event" returns false if the event's canceled flag is set and true otherwise, and that IS
       shouldContinue — see the file header for why §6.10.2's own struct-item prose disagrees and why the
       algorithms decide. A non-cancelable `cancel` therefore always continues. */
    *pcontinue = not_canceled;
    return 0;
}

/* §6.10.2's CLOSE ACTION, "an algorithm accepting no arguments and returning nothing". `in` is CONSUMED. */
static int cw_close_action_run(JSContext *wctx, CloseWatcherRun *r, JSValueConst watcher, JSValue in,
                               JSValue **out_cb, int *out_argc)
{
    CloseWatcherKind kind = cw_kind(wctx, watcher);
    JSValue subject;
    int rc;

    switch (kind) {
    case CLOSE_WATCHER_KIND_POPOVER: {
        /* §6.12 show popover step 15: "closeAction being to hide a popover given element, true, true, false,
           and null." Those five are passed as five, in the standard's order, through the function object
           core/html/popover.h exports for exactly this — the algorithm §6.12's prose reaches WITHOUT the Web
           IDL bindings, so a page that reassigns `HTMLElement.prototype.hidePopover` does not change what an
           Esc does. throwExceptions is the FALSE here: a close request is not a member invocation and has
           nobody to throw to, so a popover that check popover validity refuses is left alone rather than
           destroying the close.
           THE SUBJECT IS THE ELEMENT, which is what establish was given — §6.12 passes the popover element as
           the watcher's subject precisely so this dispatch has it.
           EVERY LINK BETWEEN A GESTURE AND THIS LINE NOW EXISTS, and the sentence that stood here said the
           opposite for long enough to be worth the correction rather than the deletion. It read that a
           dispatch was missing because nothing produced a close request that could reach process close
           watchers, and that was true when written: §6.10.1's nine steps had a home and no producer. (That
           retired sentence is paraphrased and NOT put in quotation marks: a quoted run standing beside a
           citation is read as the STANDARD's words, and engine/citegen.mjs cannot tell a fabricated sentence
           from a piece of this tree quoting itself.) They have one —
           the arrival is MODELLED, as a task on the solver's own frontier (core/html/close_request.h's
           preamble section, and the scheduler arm it names) — so the road is show popover step 15.10's
           establish, the modelled arrival's task, §6.10.1's step 7, this file's process close watchers, its
           request to close, its close, and this arm. What no run has ever done is TRAVERSE it end to end, and
           that is a different claim from a missing link: the failure it would show is not an absent
           capability but a seam, so it is asserted at each end (the answer invariant at step 4 below, and the
           arity and element brand checks §6.12's hide a popover states at its own entry) rather than
           described here. */
        JSValueConst hide_argv[POPOVER_HIDE_ARGC];
        JSValue hide = popover_hide_algorithm(wctx);
        JSValue ignored = JS_UNDEFINED;

        subject = cw_subject(wctx, watcher);
        hide_argv[0] = subject;
        hide_argv[1] = JS_TRUE;    /* focusPreviousElement */
        hide_argv[2] = JS_TRUE;    /* fireEvents */
        hide_argv[3] = JS_FALSE;   /* throwExceptions */
        hide_argv[4] = JS_NULL;    /* source */
        rc = step_call_run(wctx, &r->hphase, STEP_CB(r->hide_cb), hide, JS_UNDEFINED,
                           POPOVER_HIDE_ARGC, hide_argv, in, &ignored, out_cb, out_argc);
        JS_FreeValue(wctx, subject);
        JS_FreeValue(wctx, hide);
        if (rc) return rc;
        DCHECK(JS_IsUndefined(ignored),
               "HTML §6.12's hide a popover answered a close action with a value — it returns nothing, and "
               "with throwExceptions false it has no abrupt completion to hand back either");
        JS_FreeValue(wctx, ignored);
        return 0;
    }
    case CLOSE_WATCHER_KIND_DIALOG: {
        /* §4.11.4 set the dialog close watcher step 3: "closeAction being to close the dialog given dialog,
           dialog's request close return value, and dialog's request close source element."
           THE TWO ARGUMENTS ARE READ NOW AND NOT AT THE ESTABLISH, which is what the standard's own phrasing
           says and what makes `requestClose(v)` work at all: the watcher is established when the `open`
           attribute arrives and both fields are null then, while `requestClose()`'s steps 5 and 6 write them
           between that moment and this one. Reading them at the establish would make every close request
           answer with the return value of whichever request came first — for a dialog closed by an Esc, with
           `null`, which is right for the wrong reason and wrong the moment a page uses `requestClose("ok")`.
           IT IS A SUB-SEQUENCE AND NOT A CALL: close the dialog fires `beforetoggle` at step 2 and runs
           §6.6.4's focusing steps at step 12.3, so it parks on its own cursor, which this run holds. */
        JSValue result, src;

        subject = cw_subject(wctx, watcher);
        result = html_dialog_request_close_return_value(wctx, subject);
        src = html_dialog_request_close_source_element(wctx, subject);
        rc = html_dialog_close_run(wctx, &r->dlg, subject, result, src, in, out_cb, out_argc);
        JS_FreeValue(wctx, src);
        JS_FreeValue(wctx, result);
        JS_FreeValue(wctx, subject);
        if (rc > 0) return rc;
        if (rc < 0) return -1;
        html_dialog_close_release(wctx, &r->dlg);
        return 0;
    }
    case CLOSE_WATCHER_KIND_CLOSE_WATCHER:
        break;
    case CLOSE_WATCHER_KIND_COUNT:
        JS_FreeValue(wctx, in);
        DFAIL("a close watcher's CLOSE ACTION was asked for a kind this dispatch has no arm for");
        return 0;
    }
    /* §6.10.3 constructor step 2.2: "closeAction being to fire an event named close at this." */
    if (JS_IsUndefined(r->ev)) {
        r->ev = event_new(wctx, "close", /*bubbles*/ false, /*cancelable*/ false);
        if (JS_IsException(r->ev)) { r->ev = JS_UNDEFINED; JS_FreeValue(wctx, in); return -1; }
    }
    subject = cw_subject(wctx, watcher);
    rc = event_target_fire_run(wctx, &r->fphase, STEP_CB(r->cb), subject, r->ev, JS_UNDEFINED, in,
                               NULL, out_cb, out_argc);
    JS_FreeValue(wctx, subject);
    if (rc) return rc;
    DCHECK(r->fphase == 0, "the `close` fire answered with its own request still in flight");
    JS_FreeValue(wctx, r->ev);
    r->ev = JS_UNDEFINED;
    return 0;
}

/* ---- §6.10.2's THREE ACTION-RUNNING ALGORITHMS -------------------------------------------------------------- */

/* Each cursor's own resume points. They are PHASES and not stages: a stage is what the driver asserts a machine
   is parked at, and these three are a SUB-SEQUENCE the calling member's one stage hosts — the shape
   core/html/form_entry_list.c's FEL_* has, for the same reason. */
enum { CWR_START = 0, CWR_ACTIVATION, CWR_CANCEL, CWR_CLOSING, CWR_DONE };
enum { CWC_START = 0, CWC_ACTION, CWC_DONE };
enum { CWP_START = 0, CWP_WALK, CWP_END, CWP_DONE };

void close_watcher_run_init(CloseWatcherRun *r)
{
    int k;

    r->rphase = r->cphase = r->pphase = r->fphase = r->ua_phase = r->hphase = 0;
    r->can_prevent = r->processed = 0;
    r->i = 0;
    r->group = r->cur = r->ev = r->running = JS_UNDEFINED;
    r->dlg = NULL;
    STEP_CB_FOREACH(r->cb, k) r->cb[k] = JS_UNDEFINED;
    STEP_CB_FOREACH(r->hide_cb, k) r->hide_cb[k] = JS_UNDEFINED;
}

void close_watcher_run_visit(JSContext *ctx, CloseWatcherRun *r, JSStepVisit *v)
{
    int k;

    v->val(ctx, &r->group);
    v->val(ctx, &r->cur);
    v->val(ctx, &r->ev);
    v->val(ctx, &r->running);
    STEP_CB_FOREACH(r->cb, k)
        v->val(ctx, &r->cb[k]);
    STEP_CB_FOREACH(r->hide_cb, k)
        v->val(ctx, &r->hide_cb[k]);
    /* AND THE DIALOG ARM'S CURSOR, forwarded rather than named field by field: html_dialog.c owns what a
       half-finished close the dialog holds, and its one operation does the clone and the teardown in the
       OPPOSITE ORDERS each needs. A fork mid-`beforetoggle` therefore gives each arm its own half-closed
       dialog, which is the whole reason this is a visit entry and not a plain pointer copy. */
    html_dialog_close_visit(ctx, &r->dlg, v);
}

void close_watcher_run_unlock(JSContext *ctx, CloseWatcherRun *r)
{
    if (JS_IsUndefined(r->running)) return;
    cw_set_running_cancel(ctx, r->running, false);
    JS_FreeValue(ctx, r->running);
    r->running = JS_UNDEFINED;
}

/* A COPY OF ONE GROUP'S MEMBERS — see close_watcher_process_run's step 2.1 for why the walk is over a copy. */
static JSValue cw_list_copy(JSContext *ctx, JSValueConst list)
{
    JSValue out = JS_NewArray(ctx);
    uint32_t i, n = cw_len(ctx, list);

    CHECK(!JS_IsException(out), "close watcher: OOM copying a §6.10.2 group");
    for (i = 0; i < n; i++)
        JS_SetPropertyUint32(ctx, out, i, JS_GetPropertyUint32(ctx, list, i));
    return out;
}

/* "To REQUEST TO CLOSE A CLOSE WATCHER closeWatcher with boolean requireHistoryActionActivation", 12 steps. */
int close_watcher_request_to_close_run(JSContext *wctx, JSStepHdr *hdr, CloseWatcherRun *r,
                                       JSValueConst watcher, bool require_history_action_activation,
                                       JSValue in, bool *pproceed, JSValue **out_cb, int *out_argc)
{
    int rc;

    CW_ASSERT_WINDOW(wctx, watcher);
    if (r->rphase == CWR_START) {
        JS_FreeValue(wctx, in);
        in = JS_UNDEFINED;
        /* Steps 1, 2, 3 and 5 are the four ways this algorithm answers TRUE having run nothing: a watcher that
           is not in any group, one whose get enabled state says no, one already INSIDE its own cancel action —
           which is what §6.10.2 stores that boolean for, so a page's `cancel` handler calling requestClose()
           again returns instead of recursing — and a Window whose Document is not fully active. Step 4, "Let
           window be closeWatcher's window", is the `wctx` the caller passed: CW_ASSERT_WINDOW above has
           already checked it against the watcher's own, which is what makes the substitution safe rather than
           assumed. */
        if (!close_watcher_is_active(wctx, watcher) ||                              /* step 1 */
            !cw_get_enabled_state(wctx, watcher) ||                                 /* step 2 */
            cw_running_cancel(wctx, watcher) ||                                     /* step 3 */
            !document_fully_active(wctx)) {                                         /* steps 4-5 */
            *pproceed = true;
            r->rphase = CWR_DONE;
            return 0;
        }
        /* Step 6: "Let canPreventClose be true if requireHistoryActionActivation is false, or if window's
           close watcher manager's groups's size is less than window's close watcher manager's allowed number
           of groups, and window has history-action activation; otherwise false."
           THE THREE TESTS ARE ASKED IN THAT ORDER AND SHORT-CIRCUIT, WHICH IS NOT A MICRO-OPTIMISATION HERE.
           The third is §6.4.1's history-action activation, which core/html/user_activation.h answers as a FORK
           because the timestamp behind it is unknown external state — so asking it when the first two have
           already decided the value would SPLIT THE FLOW over a question whose answer changes nothing, minting
           a sibling that differs from this one in no observable way. A disjunction's value does not depend on
           the order it is evaluated in; which worlds exist afterwards does. */
        if (!require_history_action_activation) {
            r->can_prevent = 1;
            r->rphase = CWR_CANCEL;
        } else {
            JSValue mgr = cw_manager(wctx);
            JSValue groups = cw_groups(wctx, mgr);
            /* int64_t on both sides for establish step 4's reason: cw_allowed's "at least 1" is a DCHECK and a
               cast to uint32_t would turn a value that invariant excludes into a huge unsigned one. */
            bool eligible = (int64_t)cw_len(wctx, groups) < (int64_t)cw_allowed(wctx, mgr);

            JS_FreeValue(wctx, groups);
            JS_FreeValue(wctx, mgr);
            if (!eligible) {
                r->can_prevent = 0;
                r->rphase = CWR_CANCEL;
            } else {
                r->rphase = CWR_ACTIVATION;
            }
        }
    }
    if (r->rphase == CWR_ACTIVATION) {
        bool has = false;

        /* The fork does not consume a re-entry value — step_fork_run re-enters the machine rather than
           delivering an answer to it — so anything the caller was handed is spent here rather than forwarded
           into the cancel action below, which would then collect an answer that is not its own. */
        JS_FreeValue(wctx, in);
        in = JS_UNDEFINED;
        rc = user_activation_history_action_run(wctx, hdr, &r->ua_phase, &has);
        if (rc) return rc;
        r->can_prevent = has ? 1 : 0;
        r->rphase = CWR_CANCEL;
    }
    if (r->rphase == CWR_CANCEL) {
        bool should_continue = true;

        /* Step 7, then step 8's action, then step 9. The flag is taken ONCE per run and recorded on the run,
           so a flow ABANDONED inside the page's `cancel` handler still gives it back: close_watcher_run_unlock
           is what a holding machine's `release` calls, and without it that watcher's step 3 would refuse every
           later request for the rest of the session. */
        if (JS_IsUndefined(r->running)) {
            cw_set_running_cancel(wctx, watcher, true);                             /* step 7 */
            r->running = JS_DupValue(wctx, watcher);
        }
        rc = cw_cancel_action_run(wctx, r, watcher, r->can_prevent != 0, in,         /* step 8 */
                                  &should_continue, out_cb, out_argc);
        if (rc) return rc;
        in = JS_UNDEFINED;   /* the action CONSUMED it */
        close_watcher_run_unlock(wctx, r);                                          /* step 9 */
        if (!should_continue) {                                                     /* step 10 */
            /* Step 10.1, in the standard's own words. It is the assert that catches a cancel action returning
               false for a FALSE canPreventClose — see the file header for why §6.10.2's struct-item prose
               says that happens and why both algorithms that exist say it does not. */
            DCHECK(r->can_prevent,
                   "HTML §6.10.2 Close watcher infrastructure's request to close a close watcher step 10.1 "
                   "asserts canPreventClose is true on the arm the cancel action refused — a cancel action "
                   "that answered false for a FALSE canPreventClose has taken it. Both of this build's arms "
                   "answer true there (§6.12's is \"to return true\" and §6.10.3's fires a NON-cancelable "
                   "event, which DOM §2.9's fire answers true for), so this is a third kind's arm to fix");
            user_activation_consume_history_action(wctx);                           /* step 10.2 */
            *pproceed = false;                                                      /* step 10.3 */
            r->rphase = CWR_DONE;
            return 0;
        }
        r->cphase = CWC_START;
        r->rphase = CWR_CLOSING;
    }
    if (r->rphase == CWR_CLOSING) {
        rc = close_watcher_close_run(wctx, r, watcher, in, out_cb, out_argc);        /* step 11 */
        if (rc) return rc;
        *pproceed = true;                                                           /* step 12 */
        r->rphase = CWR_DONE;
        return 0;
    }
    JS_FreeValue(wctx, in);
    DFAIL("HTML §6.10.2 Close watcher infrastructure's request to close a close watcher was re-entered after "
          "it had already answered — its cursor is reset by whoever starts a new run of it, and a caller that "
          "did not reset it is asking a finished algorithm for a second answer");
    *pproceed = true;
    return 0;
}

/* "To CLOSE A CLOSE WATCHER closeWatcher", 5 steps. */
int close_watcher_close_run(JSContext *wctx, CloseWatcherRun *r, JSValueConst watcher, JSValue in,
                            JSValue **out_cb, int *out_argc)
{
    int rc;

    CW_ASSERT_WINDOW(wctx, watcher);
    if (r->cphase == CWC_START) {
        JS_FreeValue(wctx, in);
        in = JS_UNDEFINED;
        /* Steps 1-3 are three bare returns, which is why this algorithm has no out-parameter: the standard
           gives a caller no way to tell a refusal from a close that ran, and inventing one here would be a
           fact about this engine that no §6.10 caller may branch on. */
        if (!close_watcher_is_active(wctx, watcher) ||                               /* step 1 */
            !cw_get_enabled_state(wctx, watcher) ||                                  /* step 2 */
            !document_fully_active(wctx)) {                                          /* step 3 */
            r->cphase = CWC_DONE;
            return 0;
        }
        /* Step 4 comes BEFORE step 5, and the order is the whole of the reentrancy guarantee: the watcher is
           out of the manager before its own close action runs, so a `close` handler that calls close() again
           finds it inactive at step 1 rather than firing a second time. */
        close_watcher_destroy(wctx, watcher);                                        /* step 4 */
        r->cphase = CWC_ACTION;
    }
    if (r->cphase == CWC_ACTION) {
        rc = cw_close_action_run(wctx, r, watcher, in, out_cb, out_argc);             /* step 5 */
        if (rc) return rc;
        r->cphase = CWC_DONE;
        return 0;
    }
    JS_FreeValue(wctx, in);
    DFAIL("HTML §6.10.2 Close watcher infrastructure's close a close watcher was re-entered after it had "
          "finished — its cursor is reset by whoever starts a new run of it");
    return 0;
}

/* "To PROCESS CLOSE WATCHERS given a Window window", 4 steps. */
int close_watcher_process_run(JSContext *wctx, JSStepHdr *hdr, CloseWatcherRun *r,
                              JSValue in, bool *pprocessed, JSValue **out_cb, int *out_argc)
{
    int rc;

    if (r->pphase == CWP_START) {
        JSValue mgr, groups;
        uint32_t n;

        JS_FreeValue(wctx, in);
        in = JS_UNDEFINED;
        r->processed = 0;                                                            /* step 1 */
        mgr = cw_manager(wctx);
        groups = cw_groups(wctx, mgr);
        n = cw_len(wctx, groups);
        if (n > 0) {                                                                 /* step 2 */
            JSValue last = JS_GetPropertyUint32(wctx, groups, n - 1);                /* step 2.1 */

            DCHECK(JS_IsArray(last), "a close watcher manager's groups held something that is not a group");
            /* THE WALK IS OVER A COPY OF THE GROUP, and that is what makes step 2.2's "in reverse order" mean
               the same thing at the end as at the start. Step 2.2.2 requests to close each member, which on
               the ordinary path DESTROYS it — and destroy removes it from this very list and renumbers
               everything after it, while the page's own `cancel` handler may destroy a member the walk has not
               reached yet. Over the live list either of those silently moves the cursor onto a neighbour. Over
               a copy the order is fixed, and a member some handler destroyed meanwhile is not skipped but
               ASKED: request to close's step 1 is "if closeWatcher is not active, then return true", which is
               the standard's own answer for exactly this. */
            r->group = cw_list_copy(wctx, last);
            r->i = cw_len(wctx, r->group);
            JS_FreeValue(wctx, last);
        }
        JS_FreeValue(wctx, groups);
        JS_FreeValue(wctx, mgr);
        r->pphase = CWP_WALK;
    }
    if (r->pphase == CWP_WALK) {
        if (r->i == 0) {
            JS_FreeValue(wctx, in);
            in = JS_UNDEFINED;
            r->pphase = CWP_END;
        } else {
            bool proceed = true;

            if (JS_IsUndefined(r->cur)) {
                r->cur = JS_GetPropertyUint32(wctx, r->group, r->i - 1);             /* step 2.2 */
                DCHECK(JS_IsObject(r->cur), "a §6.10.2 group held something that is not a close watcher");
                if (cw_get_enabled_state(wctx, r->cur))                              /* step 2.2.1 */
                    r->processed = 1;
            }
            rc = close_watcher_request_to_close_run(wctx, hdr, r, r->cur, /*requireHistory*/ true,
                                                    in, &proceed, out_cb, out_argc); /* step 2.2.2 */
            if (rc) return rc;
            in = JS_UNDEFINED;   /* the sub-run CONSUMED it */
            r->rphase = CWR_START;   /* the next member runs its own 12 steps from the top */
            JS_FreeValue(wctx, r->cur);
            r->cur = JS_UNDEFINED;
            r->i--;
            if (!proceed) {                                                          /* step 2.2.3 */
                r->pphase = CWP_END;
            } else {
                /* ONE MEMBER PER TURN. A group is a list of the PAGE's size — every establish past the
                   allowance joins it — so walking it inside one C activation is the drive-to-completion this
                   engine has no other bound against, and the yield is what makes a group of ten thousand fair
                   BFS work. */
                return JS_STEP_YIELD;
            }
        }
    }
    if (r->pphase == CWP_END) {
        JSValue mgr = cw_manager(wctx);
        int32_t allowed = cw_allowed(wctx, mgr);

        /* STEP 4'S ANSWER IS NOT THIS ALGORITHM'S ALONE, WHICH IS WHY IT IS ASSERTED HERE AND NOT MERELY
           RETURNED. §6.10.1's step 8 reads it as `closedSomething` and its step 9 is the fact a caller LATCHES:
           a task that fell through to alternative processing has established that nothing in this timeline is
           watching, and a caller that models the arrival is entitled to stop modelling it (core/html/
           close_request.h states the completion value). That entitlement is sound only while a FALSE answer
           means the manager had NOTHING for a close request to do — and step 2.2.1 raises the flag for every
           member whose get enabled state is true, which BOTH kinds in close_watcher.h's registry answer
           unconditionally. So today a bound group implies the flag, and the day it does not, the caller's latch
           is recording "nothing is watching" over a group that is merely DISABLED.
           WHAT FIRING IT MEANS, IN ORDER OF WHICH IS POSSIBLE. HTML §4.11.4 The dialog element's get enabled
           state is the one the standard has that can answer false, and the clause that makes it so is the one a
           shortened quotation drops, so it is written out whole: "getEnabledState being to return true if
           dialog's enable close watcher for request close is true or dialog's computed closed-by state is not
           None; otherwise false." A disabled group is not an absent one — the page can enable it again with no
           close request in between — so adding that kind owes the LATCH a way to be given back, and not only an
           arm in the dispatch above. The other reading is a last group with no members at all, which HTML
           §6.10.2 Close watcher infrastructure's destroy a close watcher step 3 is what excludes: "Remove any
           item from manager's groups that is empty." A group that survived empty is that walk not having
           reached it. */
        DCHECK(r->processed || JS_IsUndefined(r->group),
               "HTML §6.10.2 Close watcher infrastructure's process close watchers is about to answer step 4's "
               "processedACloseWatcher FALSE while its step 2.1 bound a group. Step 2.2.1 raises that flag for "
               "every member whose get enabled state is true and both kinds in close_watcher.h's registry "
               "answer it unconditionally, so a false answer over a bound group is a kind whose get enabled "
               "state can say NO — and §6.10.1 Close requests' step 9 answer that this feeds is LATCHED by "
               "whoever models the arrival, which would then stop asking a manager whose group is disabled "
               "rather than absent. Give that latch a way to be released when a watcher's enabled state "
               "changes, in the same diff that adds the kind");
        JS_FreeValue(wctx, in);
        /* Step 3 — "If window's close watcher manager's allowed number of groups is greater than 1, decrement
           it by 1." This is the ONLY fall the allowance has, which is why establish's "always at least 1"
           invariant holds: the guard is the invariant. */
        if (allowed > 1)
            JS_SetPropertyStr(wctx, mgr, CW_ALLOWED, JS_NewInt32(wctx, allowed - 1));
        JS_FreeValue(wctx, mgr);
        JS_FreeValue(wctx, r->group);
        r->group = JS_UNDEFINED;
        *pprocessed = r->processed != 0;                                             /* step 4 */
        r->pphase = CWP_DONE;
        return 0;
    }
    JS_FreeValue(wctx, in);
    DFAIL("HTML §6.10.2 Close watcher infrastructure's process close watchers was re-entered after it had "
          "answered — its cursor is reset by whoever starts a new run of it");
    *pprocessed = false;
    return 0;
}

/* ---- the declaration and the per-realm manager -------------------------------------------------------------- */

/* ONE MANAGER PER REALM, BUILT WITH THE REALM — §6.10.2's "Each Window has a close watcher manager", at its
   three spec-initial values: groups empty, allowed number of groups 1, next user interaction allows a new
   group true. Built here rather than lazily for core/html/user_activation.c's reason: a record minted on the
   first read is built inside whichever flow happened to ask first, and that flow's baseline becomes every
   sibling's. It is cheap enough to be unconditional — an empty Array and two immediates. */
static void close_watcher_install_realm(JSContext *ctx)
{
    JSValue mgr = JS_NewObjectProto(ctx, JS_NULL);
    JSValue groups;

    CHECK(!JS_IsException(mgr), "close watcher: OOM building a realm's §6.10.2 close watcher manager");
    groups = JS_NewArray(ctx);
    CHECK(!JS_IsException(groups), "close watcher: OOM building a realm's §6.10.2 groups");
    JS_SetPropertyStr(ctx, mgr, CW_GROUPS, groups);
    JS_SetPropertyStr(ctx, mgr, CW_ALLOWED, JS_NewInt32(ctx, 1));
    JS_SetPropertyStr(ctx, mgr, CW_NEXT, JS_TRUE);
    realm_value_set(ctx, g_slot, mgr);
}

void close_watcher_init(JSContext *ctx)
{
    DCHECK(g_slot < 0, "close_watcher_init ran twice — the manager's slot is declared once per AGENT, and a "
                       "second declaration would give every realm built after it a different slot from the "
                       "one every algorithm in this file reads");
    g_slot = realm_value_declare(ctx, "HTML §6.10.2 the Window's close watcher manager");
    realm_declare_intrinsic(close_watcher_install_realm);
    /* §6.10.3's `CloseWatcher`, declared from here because §6.10 is one section with one declaration point and
       because the ORDER is a dependency: its per-realm install is added to core/realm.h's list AFTER the
       manager's, so no realm can carry the interface object without carrying a manager for its constructor to
       establish into. */
    close_watcher_interface_declare(ctx);
}

void close_watcher_free(JSRuntime *rt)
{
    /* IT TAKES THE RUNTIME NOW, and the reason is the same one core/events/event_target.h's release column
       records: what §6.10.3 holds for the agent includes a SYMBOL, and a Symbol nobody frees is a live GC
       object the runtime's own walk counts as a leak. The manager half still gives back nothing but an id. */
    close_watcher_interface_free(rt);
    /* The MANAGERS are the realms' — each is released with its context. What the agent holds is the slot id,
       and a slot id is a class id in a runtime that is going away with it. */
    g_slot = -1;
}
