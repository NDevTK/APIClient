/* HTML §6.10.2 "Close watcher infrastructure" — the manager, and the four algorithms that touch only it.
 * See close_watcher.h for why the manager is a per-realm record, why its groups are JS Arrays, why a watcher
 * names its three algorithms by a kind, and for the named residual covering the three algorithms that RUN
 * those actions.
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
 * only their PAIR reproduces the standard's three cases. */
#include <stdbool.h>
#include <stdint.h>

#include "check.h"
#include "quickjs.h"
#include "core/realm.h"
#include "core/dom/document.h"
#include "core/html/close_watcher.h"

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
               "global object, §6.10.3 passes this's)");                                                      \
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
}

void close_watcher_free(void)
{
    /* The MANAGERS are the realms' — each is released with its context. What the agent holds is the slot id,
       and a slot id is a class id in a runtime that is going away with it. */
    g_slot = -1;
}
