/* See console.h. The Console Standard, https://console.spec.whatwg.org/. */
#include <stdio.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"

#include "core/agent_state.h"
#include "core/console/console.h"
#include "core/console/console_label.h"
#include "core/idl_args.h"
#include "core/json_buf.h"
#include "core/realm.h"
#include "core/timing/hr_time.h"
#include "solver/concolic.h"

/* THE THREE MAPS AND THE THREE INTRINSICS, ON ONE PER-REALM RECORD.
 *
 * §1.2 "Counting functions" says "Each console namespace object has an associated count map, which is a map of
 * strings to numbers, initially empty"; §1.3 "Grouping functions" says the same of a group stack and §1.4
 * "Timing functions" of a timer table. There is one namespace object per realm, so there is one of each per
 * realm — and each is written by a member a page calls in a loop, which makes it PER-FLOW state that has to
 * time-travel. Held as ORDINARY JS OBJECTS on a record nothing reachable from the page holds, so every write is
 * an ordinary property write the heap COW delta already captures: one arm of a fork counts and its sibling does
 * not, and a parked flow's counters ride its snapshot to the cold tier and back. A malloc'd C map would be
 * invisible to the delta and would revert a POINTER on a context switch — solver/cow.h's own worked example.
 *
 * THE COUNT MAP AND THE TIMER TABLE HAVE A NULL PROTOTYPE, and that is load-bearing rather than tidy:
 * `console.count("__proto__")`, `console.time("constructor")` and `console.countReset("hasOwnProperty")` are
 * labels a page may use, and on an ordinary object the first would write through Object.prototype's `__proto__`
 * accessor and the other two would find inherited values that were never counted or started.
 *
 * §2.2's THREE CONVERSIONS ARE INTRINSICS AND ARE READ ONCE, HERE. Formatter calls %String%, %parseInt% and
 * %parseFloat% — the percent signs are the spec's and they mean the realm's own, so a page that assigns
 * `parseInt = () => 0` must not change what `console.log("%d", x)` does. core/realm.h states the rule and the
 * store: a per-realm value lives in quickjs's own per-context slot, read at the realm's creation before any
 * script of that document has run. */
static int g_rec_slot = -1;

/* THE MEMBERS, WHICH ARE ALSO §2.1's LOG LEVELS. Every one of §1.1-§1.4's operations performs Logger or
   Printer with a logLevel that is spelled exactly like the operation, so the two lists are one list. */
enum {
    M_ASSERT = 0, M_CLEAR, M_DEBUG, M_ERROR, M_INFO, M_LOG, M_TABLE, M_TRACE, M_WARN, M_DIR, M_DIRXML,
    M_COUNT, M_COUNT_RESET, M_GROUP, M_GROUP_COLLAPSED, M_GROUP_END, M_TIME, M_TIME_LOG, M_TIME_END,
    M_N
};
static const char *const CONSOLE_MEMBER[M_N] = {
    "assert", "clear", "debug", "error", "info", "log", "table", "trace", "warn", "dir", "dirxml",
    "count", "countReset", "group", "groupCollapsed", "groupEnd", "time", "timeLog", "timeEnd"
};
static int g_id[M_N];

/* ---- the per-realm record --------------------------------------------------------------------------------- */

#define CON_COUNTS   "counts"
#define CON_TIMERS   "timers"
#define CON_GROUPS   "groups"
#define CON_STRING   "String"
#define CON_PARSEINT "parseInt"
#define CON_PARSEFLT "parseFloat"

/* ---- §1.2/§1.4's `label`, WHICH IS FIVE MEMBERS AND TWO MAPS -------------------------------------------- */

/* WHICH MAP EACH LABELLED MEMBER'S QUESTION IS ABOUT, AND NULL FOR A MEMBER THAT HAS NO LABEL. Stated as a
 * table rather than as a condition at the seam for the reason core/idl_args.h gives about every hand-copied
 * list: the fact "this member asks the label question, of THAT map" is one fact per member, and a `magic ==
 * A || magic == B || …` at the call site is the same list with nowhere for the map to be written down. It is
 * indexed by the enum above, so a member added without an entry is a NULL — which is the honest answer for
 * every member that has no label, and which the DCHECK at each labelled case turns into an abort for one that
 * does.
 *
 * §1.2 "Counting functions": "Each console namespace object has an associated count map, which is a map of
 * strings to numbers, initially empty." §1.4 "Timing functions" says the same with "times" for "numbers" of
 * the timer table. Two maps, one question, five askers. */
static const char *const CONSOLE_LABEL_MAP[M_N] = {
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    CON_COUNTS, CON_COUNTS, NULL, NULL, NULL, CON_TIMERS, CON_TIMERS, CON_TIMERS
};

/* EACH LABELLED MEMBER'S OWN SPEC IDENTITY — the ADDRESS every assert the label question makes on its behalf
 * reports. The machine's IdlStepDecl names the whole namespace, which is the right answer for "which
 * algorithm is this flow parked in" and far too coarse for "which member reached this crash": a
 * should-never-happen stamps the line it is WRITTEN at, so a check inside the shared chain would name
 * console_label.c for all five (CLAUDE.md's §AN-ASSERT-THAT-NAMES-A-REMEDY). The site travels with the
 * operation instead, and it is the member's own spec identity rather than a file and a line because that name
 * is stable across an edition of this tree in a way a coordinate is not. */
static const char *const CONSOLE_LABEL_ALGORITHM[M_N] = {
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    "Console §1.2.1 count(label)", "Console §1.2.2 countReset(label)", NULL, NULL, NULL,
    "Console §1.4.1 time(label)", "Console §1.4.2 timeLog(label, ...data)", "Console §1.4.3 timeEnd(label)"
};

/* OWNED — the caller frees. */
static JSValue console_rec(JSContext *ctx)
{
    JSValue rec = realm_value_get(ctx, g_rec_slot);

    DCHECK(JS_IsObject(rec),
           "a console member ran in a realm that has no console record — the namespace object, its count map, "
           "its group stack and its timer table are built together by this component's realm intrinsic, so a "
           "realm reaching a member without one is a realm core/realm.h's list never ran for");
    return rec;
}

/* OWNED. `which` is one of the six field names above. */
static JSValue console_field(JSContext *ctx, const char *which)
{
    JSValue rec = console_rec(ctx), v = JS_GetPropertyStr(ctx, rec, which);

    JS_FreeValue(ctx, rec);
    DCHECK(!JS_IsUndefined(v), which);
    return v;
}

/* §1.3's GROUP STACK DEPTH — how far Printer indents. */
static int console_group_depth(JSContext *ctx)
{
    JSValue groups = console_field(ctx, CON_GROUPS);
    int64_t n = 0;

    CHECK(JS_GetLength(ctx, groups, &n) == 0, "console: the group stack has no length");
    JS_FreeValue(ctx, groups);
    DCHECK(n >= 0, "console: the group stack reported a negative depth");
    return (int)n;
}

/* ---- §2.3 Printer ----------------------------------------------------------------------------------------- */

/* §2.3 "The printer operation is implementation-defined." What this implementation does is write ONE line to
 * the engine's diagnostic stream, and the two decisions in it are worth stating.
 *
 * IT RUNS NONE OF THE PAGE'S CODE. §2.3.3's "Common object formats" are implementation-defined, and a printer
 * that reached for ToString on the page's object would run the page's `toString` at a point no algorithm says
 * to — a side effect the page can observe and that no browser performs. So a value is rendered from what it IS,
 * never from what it would coerce to: a String by its bytes, a Number by the engine's own number-to-string, a
 * boolean/null/undefined by its literal, and everything else by its KIND. A CONCOLIC renders as its shape,
 * which is the one rendering a solver engineer wants — `console.log(location.hash)` says `{location.hash}`
 * rather than `[object]`.
 *
 * THE LINE IS JSON AND THE PAGE'S BYTES NEVER LEAVE THE STRING. The engine's diagnostic channel carries records
 * a reader and engine/build.mjs both parse — `@WHY`, `@COLD`, `@WFQ` — so a printer that pasted page-controlled
 * text onto it would let a page FORGE one, newline and all. core/json_buf.h is the writer for strings the
 * engine owns: quotes, backslashes and every C0 byte escaped, so one call is one line and nothing in it can end
 * the line early.
 *
 * "It is important that console is always visible and usable to scripts, even if the developer console has not
 * been opened or does not exist" — §1. A host with nowhere to show this still runs every step above it, which
 * is what makes the count map, the timer table and §2.2's conversions real regardless. */
static void console_render(JSContext *ctx, JsonBuf *b, JSValueConst v)
{
    if (concolic_is(v)) {
        const char *shape = concolic_shape_c(v);

        DCHECK(shape != NULL, "console: a concolic value carries no shape — concolic_new mints the pair "
                              "together, so a value with one and not the other was not minted there");
        json_buf_str(b, shape);
        return;
    }
    if (JS_IsString(v) || JS_IsNumber(v)) {
        /* NEITHER RUNS THE PAGE'S CODE: a String is already one, and a Number's ToString is 6.1.6.1.20
           Number::toString, which is the engine's own and reaches nothing of the page's. */
        const char *s = JS_ToCString(ctx, v);

        CHECK(s != NULL, "console: a string or number could not be rendered");
        json_buf_str(b, s);
        JS_FreeCString(ctx, s);
        return;
    }
    if (JS_IsUndefined(v)) { json_buf_str(b, "undefined"); return; }
    if (JS_IsNull(v))      { json_buf_str(b, "null"); return; }
    if (JS_IsBool(v))      { json_buf_str(b, JS_ToBool(ctx, v) ? "true" : "false"); return; }
    if (JS_IsSymbol(v))    { json_buf_str(b, "[symbol]"); return; }
    if (JS_IsFunction(ctx, v)) { json_buf_str(b, "[function]"); return; }
    if (JS_IsBigInt(v))    { json_buf_str(b, "[bigint]"); return; }
    DCHECK(JS_IsObject(v), "console: a value that is neither a primitive this printer names nor an object "
                           "reached §2.3.3's rendering — every JS value is one or the other, so a third kind "
                           "is a value type this engine gained without telling this file");
    json_buf_str(b, "[object]");
}

/* `list` is §2.3's args, an Array. */
static void console_printer(JSContext *ctx, const char *level, JSValueConst list)
{
    JsonBuf b = { 0 };
    int64_t n = 0, i;
    char depth[24];
    char *line;

    CHECK(JS_GetLength(ctx, list, &n) == 0, "console: the printer was handed something with no length");
    json_buf_raw(&b, "@LOG {"); json_buf_key(&b, "level");
    json_buf_str(&b, level);
    /* The DEPTH is formatted; the two names around it are not. A `snprintf` that carries a field name inside its
       format is the same hiding place json_buf_key exists to close — the name is still a literal, but it is one
       nobody looking at the emitter's vocabulary would think to read. */
    json_buf_raw(&b, ","); json_buf_key(&b, "group");
    snprintf(depth, sizeof depth, "%d", console_group_depth(ctx));
    json_buf_raw(&b, depth);
    json_buf_raw(&b, ","); json_buf_key(&b, "args"); json_buf_raw(&b, "[");
    for (i = 0; i < n; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, list, (uint32_t)i);

        if (i) json_buf_raw(&b, ",");
        console_render(ctx, &b, e);
        JS_FreeValue(ctx, e);
    }
    json_buf_raw(&b, "]}\n");
    line = json_buf_take(&b);
    CHECK(line != NULL, "console: the printer's line could not be allocated");
    fputs(line, stderr);
    free(line);
    json_buf_free(&b);
}

/* A one-element §2.3 args list, for the members whose Logger call has a single element and therefore reaches
   Printer directly (§2.1 step 4). CONSUMES `v`. */
static void console_print_one(JSContext *ctx, const char *level, JSValue v)
{
    JSValue list = JS_NewArray(ctx);

    CHECK(!JS_IsException(list), "console: a printer list could not be allocated");
    CHECK(JS_SetPropertyUint32(ctx, list, 0, v) >= 0, "console: a printer list could not be filled");
    console_printer(ctx, level, list);
    JS_FreeValue(ctx, list);
}

/* ---- §2.2 Formatter: finding the specifier ---------------------------------------------------------------- */

/* §2.2.1 "Summary of formatting specifiers" lists exactly these seven. */
static int console_is_specifier(char c)
{
    return c == 's' || c == 'd' || c == 'i' || c == 'f' || c == 'o' || c == 'O' || c == 'c';
}

/* §2.2 step 4 "Find the first possible format specifier specifier, from the left to the right in target."
   `target` is whatever args[0] is; a value that is not a String contains no specifier, which is the step's own
   answer for it rather than a case this skips. Returns the byte offset of the `%`, or -1, and writes the
   specifier's letter to *pkind. `bytes`/`plen` receive target's UTF-8, BORROWED from ctx (free with
   JS_FreeCString) and NULL when target is not a String. */
static int console_find_specifier(JSContext *ctx, JSValueConst target, const char **bytes, size_t *plen,
                                  char *pkind)
{
    size_t len = 0, i;
    const char *s;

    *bytes = NULL; *plen = 0; *pkind = 0;
    if (!JS_IsString(target)) return -1;
    s = JS_ToCStringLen(ctx, &len, target);
    CHECK(s != NULL, "console: §2.2's target string could not be read");
    *bytes = s; *plen = len;
    for (i = 0; i + 1 < len; i++)
        if (s[i] == '%' && console_is_specifier(s[i + 1])) { *pkind = s[i + 1]; return (int)i; }
    return -1;
}

/* §2.2 step 5's last clause: "replace specifier in target with converted". `converted` is a String (%s) or a
   Number (%d/%i/%f); both render without running the page's code, which is why this can build the new target
   from bytes. Returns the new string (owned). */
static JSValue console_substitute(JSContext *ctx, const char *bytes, size_t len, int at, JSValueConst converted)
{
    const char *ins;
    size_t ins_len = 0;
    JsonBuf b = { 0 };
    JSValue out;
    char *whole;

    DCHECK(JS_IsString(converted) || JS_IsNumber(converted),
           "§2.2 step 5 produced a `converted` that is neither a String nor a Number — %s is a %String% call "
           "and %d/%i/%f are %parseInt%/%parseFloat% calls, so there is no third shape it can have");
    ins = JS_ToCStringLen(ctx, &ins_len, converted);
    CHECK(ins != NULL, "console: §2.2's converted value could not be rendered");
    /* json_buf is a byte buffer; json_buf_raw appends NUL-terminated runs, and neither half here can contain a
       NUL — a JS string's UTF-8 encodes U+0000 as 0xC0 0x80 in quickjs's CESU-style output, so the two halves
       are C strings. The prefix is written by length instead, because it is a SLICE of one. */
    {
        char *pre = js_malloc(ctx, (size_t)at + 1);

        CHECK(pre != NULL, "console: §2.2's prefix could not be allocated");
        memcpy(pre, bytes, (size_t)at);
        pre[at] = 0;
        json_buf_raw(&b, pre);
        js_free(ctx, pre);
    }
    json_buf_raw(&b, ins);
    json_buf_raw(&b, bytes + at + 2);   /* everything after the two bytes of the specifier */
    JS_FreeCString(ctx, ins);
    whole = json_buf_take(&b);
    CHECK(whole != NULL, "console: §2.2's substituted target could not be allocated");
    out = JS_NewString(ctx, whole);
    free(whole);
    json_buf_free(&b);
    CHECK(!JS_IsException(out), "console: §2.2's substituted target could not be interned");
    return out;
}

/* §2.2 step 6: "Let result be a list containing target together with the elements of args starting from the
   third onward." So the SECOND element — §2.2's `current`, whether or not a specifier consumed it — is dropped
   and `target` is replaced. Done in place on the Array the state owns. */
static void console_advance(JSContext *ctx, JSValueConst args, JSValue target)
{
    int64_t n = 0, i;

    CHECK(JS_GetLength(ctx, args, &n) == 0, "console: §2.2's list has no length");
    DCHECK(n >= 2, "§2.2 step 6 ran on a list of fewer than two elements — step 1 returns for a list of one, so "
                   "reaching the shift means the size check above it was skipped");
    CHECK(JS_SetPropertyUint32(ctx, args, 0, target) >= 0, "console: §2.2's target could not be replaced");
    for (i = 2; i < n; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, args, (uint32_t)i);

        CHECK(JS_SetPropertyUint32(ctx, args, (uint32_t)(i - 1), e) >= 0,
              "console: §2.2's list could not be compacted");
    }
    CHECK(JS_SetPropertyStr(ctx, args, "length", JS_NewInt64(ctx, n - 1)) >= 0,
          "console: §2.2's list could not be shortened");
}

/* ---- the machine ------------------------------------------------------------------------------------------ */

#define CON_STAGES(X) \
    X(CON_RUN,    "Console §1.1-§1.4 this member's own steps — for §1.2/§1.4's five, the label question its " \
                  "map is asked first — up to the Logger, Formatter or Printer it performs") \
    X(CON_FORMAT, "Console §2.2 Formatter step 5 — ONE format specifier: the %String%/%parseInt%/%parseFloat% " \
                  "call over `current`, the substitution into `target`, and step 6's shift")
enum { IDL_STEP_STAGE_BASE(CON_STAGES) CON_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const CON_STEPS[] = { CON_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    /* §2.1's `args` and §2.2's evolving `result` are ONE list — §2.2 recurses on the list it just built, so a
       second field would be the same list under a second name. An Array rather than a C vector for the reason
       the maps are objects: it rides the flow's snapshot and its mutations are captured. OWNED. */
    JSValue args;
    JSValue cb[4];    /* step_call_run's [this, func, arg0, arg1] — %parseInt%(current, 10) is the widest */
    uint8_t phase;    /* step_call_run's cursor */
    /* §1.2/§1.4's LABEL QUESTION, for the five members that have a `label` — core/console/console_label.h owns
       it. It is EMBEDDED rather than allocated because the fork copies a machine's state, and its zeroed
       arrival is the valid "no question asked yet" shape, which is why nothing initialises it below. */
    ConsoleLabelChain label;
} ConsoleState;

static void console_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    ConsoleState *s = st;
    int i;

    v->val(ctx, &s->args);
    for (i = 0; i < 4; i++)
        v->val(ctx, &s->cb[i]);
    console_label_chain_visit(ctx, &s->label, v);
}

/* An Array of argv[from..argc). OWNED. */
static JSValue console_list_from(JSContext *ctx, int from, int argc, JSValueConst *argv)
{
    JSValue list = JS_NewArray(ctx);
    int i;

    CHECK(!JS_IsException(list), "console: an argument list could not be allocated");
    for (i = from; i < argc; i++)
        CHECK(JS_SetPropertyUint32(ctx, list, (uint32_t)(i - from), JS_DupValue(ctx, argv[i])) >= 0,
              "console: an argument list could not be filled");
    return list;
}

/* §2.1 Logger(logLevel, args). CONSUMES `list`. Returns 0 when the member is finished (steps 1 and 4 both end
   the operation) and 1 when §2.2's Formatter has work, in which case the caller advances to CON_FORMAT. */
static int console_logger(JSContext *ctx, ConsoleState *s, const char *level, JSValue list)
{
    int64_t n = 0;

    CHECK(JS_GetLength(ctx, list, &n) == 0, "console: §2.1's args has no length");
    if (n == 0) { JS_FreeValue(ctx, list); return 0; }          /* step 1: "If args is empty, return." */
    if (n == 1) {                                               /* step 4: rest is empty -> Printer directly */
        console_printer(ctx, level, list);
        JS_FreeValue(ctx, list);
        return 0;
    }
    DCHECK(JS_IsUndefined(s->args), "console: a second Logger began while §2.2's list was still live");
    s->args = list;                                             /* step 5: Printer(logLevel, Formatter(args)) */
    return 1;
}

static int console_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                        JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    ConsoleState *s = st;
    int magic = idl_step_magic(hdr);
    const char *level;
    int i;

    DCHECK(magic >= 0 && magic < M_N, "a console member ran with a magic no operation of this namespace has");
    /* Every operation's logLevel is spelled like the operation — §1.1.3 performs Logger("debug", data), §1.2.1
       performs Logger("count", …) — with ONE exception the standard states outright: §1.1.7 table() says "log
       it with a logLevel of `log`". The exception is here rather than at that member's Printer because the
       level is read again at `formatted:`, after §2.2 has run, and two statements of it could disagree. */
    level = (magic == M_TABLE) ? "log" : CONSOLE_MEMBER[magic];
    *presult = JS_UNDEFINED;

    if (hdr->stage == CON_RUN) {
        /* §1.2/§1.4's LABEL, RESOLVED BELOW BEFORE ANY MEMBER'S OWN STEP RUNS. JS_ATOM_NULL for the fourteen
           members that have no label; OWNED by this activation and freed by the case that reads it. */
        JSAtom label_key = JS_ATOM_NULL;

        /* WHY RE-ENTERING THIS STAGE IS SOUND, ASSERTED RATHER THAN ARGUED. A labelled member can PARK here —
           its label question forks — and the resume lands back at CON_RUN, which re-runs the init below. That
           is safe only while the member has performed none of its own steps yet, which is exactly what the
           label question being FIRST buys: the init would otherwise drop a live §2.2 list on the floor. The
           assert is the structural half of that sentence, so a later edit that moves a member's own work above
           the label question aborts here instead of leaking one. */
        DCHECK(!s->label.taken || (JS_IsUndefined(s->args) && s->phase == 0),
               "a console member resumed into its first stage with §2.2's list or a call cursor already live — "
               "the only park that lands back here is §1.2/§1.4's label question, which is the FIRST thing a "
               "labelled member does, so a member that has begun its own steps and parked at this stage has "
               "work the re-entry below is about to discard");
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        /* EVERY OWNED FIELD IN PLACE BEFORE THE FIRST THING THAT CAN THROW. A step state arrives ZEROED and a
           zeroed JSValue is the integer 0, not undefined, so a slot read before it is written is a real value
           the page can see — the trap solver/streams paid for five times. */
        s->args = JS_UNDEFINED;
        for (i = 0; i < 4; i++) s->cb[i] = JS_UNDEFINED;
        s->phase = 0;

        /* §1.2.1, §1.2.2, §1.4.1, §1.4.2 and §1.4.3 ALL BEGIN WITH ONE QUESTION ABOUT ONE MAP, AND IT IS ASKED
           HERE RATHER THAN FIVE TIMES BELOW. "If map[label] exists", "If the associated timer table contains an
           entry with key label" and the two `timerTable[label]` reads are four spellings of it. With a known
           label it is a lookup; with unknown external input it is an elimination over the map's own keys, and
           the answer to BOTH is the key the member then works through — which is why every case below takes
           `label_key` where it used to convert `argv[0]` itself. core/console/console_label.h holds the
           argument, including why the answer is one atom and not an atom plus a "was it there" flag. */
        if (CONSOLE_LABEL_MAP[magic] != NULL) {
            JSValue map = console_field(ctx, CONSOLE_LABEL_MAP[magic]);
            int rc;

            DCHECK(argc >= 1, "a §1.2/§1.4 member reached its body with no `label` position — every one of the "
                              "five declares `optional DOMString label = \"default\"`, so Web IDL §3.6 places "
                              "the default whether or not the page reached it");
            DCHECK(CONSOLE_LABEL_ALGORITHM[magic] != NULL,
                   "a console member names the map its label question is about and does not name ITSELF — the "
                   "two tables are one fact per member and an entry in one without the other leaves every "
                   "assert that question makes reporting no site");
            rc = console_label_run(ctx, hdr, &s->label, argv[0], map, CONSOLE_LABEL_ALGORITHM[magic],
                                   &label_key);
            JS_FreeValue(ctx, map);
            if (rc)
                return rc;   /* parked at a link of the chain; the resume re-enters this stage and re-asks */
        }

        switch (magic) {

        /* §1.1.2 clear(): "Empty the appropriate group stack. If possible for the environment, clear the
           console. (Otherwise, do nothing.)" There is no console to clear here, which is the parenthesis's own
           branch; emptying the stack is not optional and is the whole observable effect. */
        case M_CLEAR: {
            JSValue groups = console_field(ctx, CON_GROUPS);

            CHECK(JS_SetPropertyStr(ctx, groups, "length", JS_NewInt32(ctx, 0)) >= 0,
                  "console: §1.1.2's group stack could not be emptied");
            JS_FreeValue(ctx, groups);
            return 0;
        }

        /* §1.3.3 groupEnd(): "Pop the last group from the group stack." A pop of an EMPTY stack is what a page
           doing `console.groupEnd()` at top level performs, and the standard says only "pop the last group" —
           there is none, so nothing is popped. Written as a positive branch rather than a clamp: the empty
           stack is a state the algorithm reaches, not a broken invariant. */
        case M_GROUP_END: {
            JSValue groups = console_field(ctx, CON_GROUPS);
            int64_t n = 0;

            CHECK(JS_GetLength(ctx, groups, &n) == 0, "console: §1.3.3's group stack has no length");
            if (n > 0)
                CHECK(JS_SetPropertyStr(ctx, groups, "length", JS_NewInt64(ctx, n - 1)) >= 0,
                      "console: §1.3.3's group could not be popped");
            JS_FreeValue(ctx, groups);
            return 0;
        }

        /* §1.2.1 count(label) and §1.2.2 countReset(label). The map is the realm's; `label_key` is the entry
           the label names IN THIS WORLD, answered by the seam above — an existing key on an arm the chain
           pinned, or the unknown's own stable slot when it named none. This comment used to say the label
           "arrived as a real String (the declaration's DOMString conversion, which is where the page's
           `toString` ran)", which is true of a KNOWN label and false of the one this member exists to explore:
           Web IDL §3.2.10 DOMString's conversion is one unknown external input CROSSES as itself. */
        case M_COUNT:
        case M_COUNT_RESET: {
            JSValue map = console_field(ctx, CON_COUNTS);
            JSAtom key = label_key;
            JSValue prev, concat;
            int32_t next = 1;
            int had;

            /* THE KEY IS THE ANSWER TO §1.2's OWN QUESTION AND WAS RESOLVED ABOVE, WHICH IS WHY THE CONVERSION
               AND THE TYPE ASSERT THAT STOOD HERE ARE BOTH GONE. The assert said the label is a String, which
               is FALSE of every call this member exists to explore: a DOMString argument position is a
               boundary unknown external input crosses AS ITSELF. It is stated where it is true of both shapes
               now, at console_label_run's entry, beside the algorithm it is about. */
            DCHECK(key != JS_ATOM_NULL,
                   "§1.2's count member reached its body with no resolved label key — the seam above answers "
                   "for every member CONSOLE_LABEL_MAP names, so a null here is a member whose table entry is "
                   "missing");
            had = JS_HasProperty(ctx, map, key);
            CHECK(had >= 0, "console: §1.2's count map could not be read");
            if (magic == M_COUNT_RESET) {
                /* §1.2.2: "If map[label] exists, set map[label] to 0. Otherwise: … Perform
                   Logger("countReset", « message »)." The message is "a string without any formatting
                   specifiers indicating generically that the given label does not have an associated count". */
                if (had) CHECK(JS_SetProperty(ctx, map, key, JS_NewInt32(ctx, 0)) >= 0,
                               "console: §1.2.2's count could not be reset");
                JS_FreeAtom(ctx, key);
                JS_FreeValue(ctx, map);
                if (!had)
                    console_print_one(ctx, level, JS_NewString(ctx, "Count for label does not exist"));
                return 0;
            }
            /* §1.2.1: "If map[label] exists, set map[label] to map[label] + 1. Otherwise, set map[label] to 1."
               The stored value is a Number this component wrote, so reading it back runs nothing. */
            if (had) {
                int32_t cur = 0;

                prev = JS_GetProperty(ctx, map, key);
                CHECK(JS_ToInt32(ctx, &cur, prev) == 0, "console: §1.2.1's count is not a number");
                JS_FreeValue(ctx, prev);
                next = cur + 1;
            }
            CHECK(JS_SetProperty(ctx, map, key, JS_NewInt32(ctx, next)) >= 0,
                  "console: §1.2.1's count could not be stored");
            JS_FreeValue(ctx, map);
            /* "Let concat be the concatenation of label, U+003A (:), U+0020 SPACE, and ToString(map[label])."
               THE LABEL IS READ OFF THE RESOLVED KEY AND NEVER OFF THE ARGUMENT, which is what makes this line
               work for both worlds. On the arm that named an existing entry the key IS the label in this
               world, so printing it states what the flow established; on the remainder arm it is the unknown's
               own display shape, which is the same rendering console_render gives a concolic anywhere else. A
               JS_ToCString on the argument was the second of §1.2/§1.4's two aborts — a byte consumer handed
               unknown external input, dying in js_force_tostring with this file's line in the message. */
            {
                const char *lab = JS_AtomToCString(ctx, key);
                char tail[32];
                JsonBuf b = { 0 };
                char *whole;

                CHECK(lab != NULL, "console: §1.2.1's label could not be read");
                snprintf(tail, sizeof tail, ": %d", (int)next);
                json_buf_raw(&b, lab);
                json_buf_raw(&b, tail);
                JS_FreeCString(ctx, lab);
                whole = json_buf_take(&b);
                CHECK(whole != NULL, "console: §1.2.1's concat could not be allocated");
                concat = JS_NewString(ctx, whole);
                free(whole);
                json_buf_free(&b);
            }
            JS_FreeAtom(ctx, key);   /* held until here: the concatenation reads the LABEL out of it */
            /* "Perform Logger("count", « concat »)" — one element, so §2.1 step 4 reaches Printer directly. */
            console_print_one(ctx, level, concat);
            return 0;
        }

        /* §1.4.1 time(label): "If the associated timer table contains an entry with key label, return …
           Otherwise, set the value of the entry with key label … to the current time." */
        case M_TIME: {
            JSValue table = console_field(ctx, CON_TIMERS);
            JSAtom key = label_key;
            int had;

            /* THE ENTIRE OBSERVABLE OF §1.4.1 IS THIS TEST, WHICH IS WHY THE CONVERSION THAT USED TO STAND
               HERE WAS THE WORST OF THE FIVE. A JS_ValueToAtom over unknown external input does not abort — it
               reaches ECMAScript §7.1.21 ToPropertyKey ( arg ), whose concolic arm answers with the
               value's display shape as a real string — so this member RAN, filed a timer under a slot no page
               key can equal, and answered NO to the test below for every label ever started, with no fork and
               nothing to say a question had been asked. */
            DCHECK(key != JS_ATOM_NULL,
                   "§1.4.1 reached its body with no resolved label key — the seam above answers for every "
                   "member CONSOLE_LABEL_MAP names, so a null here is a member whose table entry is missing");
            had = JS_HasProperty(ctx, table, key);
            CHECK(had >= 0, "console: §1.4.1's timer table could not be read");
            if (!had)
                CHECK(JS_SetProperty(ctx, table, key, hr_time_current(ctx)) >= 0,
                      "console: §1.4.1's timer could not be started");
            JS_FreeAtom(ctx, key);
            JS_FreeValue(ctx, table);
            /* The "optionally reporting a warning" of the had-one branch is the §2.4 reporting operation, which
               is a developer-tools surface and not a page-observable one; the RETURN is the part that is. */
            return 0;
        }

        /* §1.4.2 timeLog(label, ...data) and §1.4.3 timeEnd(label). Both read `timerTable[label]` and take the
           difference from the current time; timeEnd additionally REMOVES the entry. Neither performs Logger —
           both go straight to Printer — so neither reaches §2.2 and `data` is printed unformatted, which is
           §1.4.2's own wording ("Perform Printer("timeLog", data)"). */
        case M_TIME_LOG:
        case M_TIME_END: {
            JSValue table = console_field(ctx, CON_TIMERS);
            JSAtom key = label_key;
            double start = 0;
            JSValue startv, nowv, concat, list;
            const char *lab, *shape = NULL;
            JsonBuf b = { 0 };
            char *whole;

            DCHECK(key != JS_ATOM_NULL,
                   "§1.4.2/§1.4.3 reached its body with no resolved label key — the seam above answers for "
                   "every member CONSOLE_LABEL_MAP names, so a null here is a member whose table entry is "
                   "missing");
            startv = JS_GetProperty(ctx, table, key);
            /* whatwg/console#134: the standard reads `timerTable[label]` with no test that it exists, and the
               issue it links is the open question of what to report when it does not. An absent entry is
               undefined, whose ToNumber is NaN, so the duration is NaN — which is what the algorithm as written
               produces and what this prints, rather than a value invented in its place. */
            nowv = hr_time_current(ctx);
            if (JS_IsUndefined(startv)) {
                start = 0.0 / 0.0;
            } else if (concolic_is(startv) || concolic_is(nowv)) {
                /* §1.4.2/.3 print "a string representing the difference … in an IMPLEMENTATION-DEFINED
                   FORMAT", and where either end of the duration is unknown external input the difference is
                   an unknown too — the event loop's clock is a moment and a timer set with an unknown
                   `timeout` moves it to one nothing computed (core/timing/event_loop.h). The honest string is
                   then the DERIVATION's own display shape rather than a number: `%.3f` over an unknown would
                   print a moment this engine never computed, which is the invention §Solver forbids, and
                   NaN would say the timer was never started. The subtraction is ECMAScript §13.8.2 The
                   Subtraction Operator ( - ),
                   RUN — the same operator hr_time_relative discharges its duration through — so the shape
                   reads as the expression the page's own timing produced. */
                /* The hook's stack effect is the interpreter's: both operands freed, the result placed in
                   sp[-2]. `nowv`'s reference is handed over and comes back as the difference. */
                JSValue sp[2];

                sp[0] = nowv;
                sp[1] = JS_DupValue(ctx, startv);
                if (!concolic_arith_hook(ctx, sp + 2, JS_CARITH_SUB, 2))
                    DFAIL("§13.8.2 The Subtraction Operator ( - ) declined an operand this component has "
                          "already established is "
                          "UNKNOWN — the concolic value semantics are not installed in this host "
                          "(solver/concolic.h: concolic_install_hooks)");
                nowv = sp[0];
                shape = concolic_shape_c(nowv);
                DCHECK(shape != NULL,
                       "§1.4's duration over an unknown moment has no display shape — every derivation this "
                       "engine mints carries one, so a value without it was not minted by an operator");
            } else {
                DCHECK(JS_IsNumber(startv),
                       "§1.4's timer table held something that is neither a number nor unknown external input "
                       "— §1.4.1 is the only writer and it stores HR-Time §4's current high resolution time, "
                       "so anything else here is a second writer this component does not know about");
                CHECK(JS_ToFloat64(ctx, &start, startv) == 0, "console: §1.4's start time could not be read");
            }
            JS_FreeValue(ctx, startv);
            if (magic == M_TIME_END)
                CHECK(JS_DeleteProperty(ctx, table, key, 0) >= 0, "console: §1.4.3's timer could not be removed");
            JS_FreeValue(ctx, table);

            /* THE LABEL IS READ OFF THE RESOLVED KEY, for the reason §1.2.1's concatenation states: the key IS
               the label in this world on the arm that named an entry, and is the unknown's own display shape
               on the remainder arm. The JS_ToCString on the argument that stood here is where
               `console.timeEnd(location.hash)` died — in js_force_tostring, naming this line as the byte
               consumer, one step AFTER the membership question had already been decided wrongly and silently. */
            lab = JS_AtomToCString(ctx, key);
            CHECK(lab != NULL, "console: §1.4's label could not be read");
            JS_FreeAtom(ctx, key);
            /* "a string representing the difference … in an implementation-defined format". Milliseconds, which
               is the unit hr_time's DOMHighResTimeStamp already is, so no conversion invents precision.
               IT IS APPENDED RATHER THAN FORMATTED INTO A FIXED ARRAY, because the unknown arm's `shape` is a
               DERIVATION's display form and a derivation has no width — `{location.hash}` is short and the
               expression a page's own timing composes is not. This was a silent truncation that no run could
               reach while the label conversion above aborted first; routing the label is what makes it
               reachable, so removing it is part of the same diff. The numeric arm still formats, into a buffer
               sized for the widest `%.3f` a double can spell (309 integer digits, a point, three decimals and
               a sign) with the count asserted rather than assumed. */
            json_buf_raw(&b, lab);
            json_buf_raw(&b, ": ");
            if (shape) {
                json_buf_raw(&b, shape);
            } else {
                double now = 0;
                char num[400];
                int w;

                if (!JS_IsUndefined(nowv))
                    CHECK(JS_ToFloat64(ctx, &now, nowv) == 0, "console: §1.4's current time could not be read");
                w = snprintf(num, sizeof num, "%.3f", now - start);
                DCHECK(w > 0 && (size_t)w < sizeof num,
                       "§1.4's duration could not be spelled — the buffer is sized for the widest `%.3f` a "
                       "double has, so a truncation here is a number that is not one");
                json_buf_raw(&b, num);
            }
            json_buf_raw(&b, " ms");
            JS_FreeValue(ctx, nowv);
            JS_FreeCString(ctx, lab);
            whole = json_buf_take(&b);
            CHECK(whole != NULL, "console: §1.4's concat could not be allocated");
            concat = JS_NewString(ctx, whole);
            free(whole);
            json_buf_free(&b);

            if (magic == M_TIME_END) { console_print_one(ctx, level, concat); return 0; }
            /* §1.4.2 step 5: "Prepend concat to data." */
            list = console_list_from(ctx, 1, argc, argv);
            {
                int64_t n = 0, k;

                CHECK(JS_GetLength(ctx, list, &n) == 0, "console: §1.4.2's data has no length");
                for (k = n; k > 0; k--) {
                    JSValue e = JS_GetPropertyUint32(ctx, list, (uint32_t)(k - 1));

                    CHECK(JS_SetPropertyUint32(ctx, list, (uint32_t)k, e) >= 0,
                          "console: §1.4.2's data could not be shifted");
                }
                CHECK(JS_SetPropertyUint32(ctx, list, 0, concat) >= 0,
                      "console: §1.4.2's concat could not be prepended");
            }
            console_printer(ctx, level, list);
            JS_FreeValue(ctx, list);
            return 0;
        }

        /* §1.1.8 trace(...data): "Let trace be some implementation-defined … representation of the callstack …
           Perform Printer("trace", « trace »)." The optional Formatter of the middle step is declined, so no
           page code runs and `data` is not consumed — which is why this is not a Logger. */
        case M_TRACE:
            console_print_one(ctx, level, JS_NewString(ctx, "console.trace"));
            return 0;

        /* §1.1.10 dir(item, options): "Let object be item with generic JavaScript object formatting applied.
           Perform Printer("dir", « object », options)." Generic object formatting is §2.3.3's, which is
           implementation-defined and is what console_render performs — so the item crosses to Printer as
           itself and the formatting happens there. */
        case M_DIR:
            console_print_one(ctx, level, JS_DupValue(ctx, argc > 0 ? argv[0] : JS_UNDEFINED));
            return 0;

        /* §1.1.7 table(tabularData, properties): "Try to construct a table … Fall back to just logging the
           argument if it can't be parsed as tabular." Parsing a page object as tabular is reading its members,
           which is the page's code; the standard marks the whole operation "TODO: This will need a good
           algorithm", so the fallback IS the algorithm here and it is the standard's own. */
        case M_TABLE:
            /* "log it with a logLevel of `log`" — the level is the standard's and is NOT this member's name. */
            if (!console_logger(ctx, s, level, console_list_from(ctx, 0, argc > 0 ? 1 : 0, argv))) return 0;
            hdr->stage = CON_FORMAT;
            return JS_STEP_YIELD;

        /* §1.1.1 assert(condition, ...data). */
        case M_ASSERT: {
            JSValue list;
            static const char MSG[] = "Assertion failed";
            int64_t n = 0;

            DCHECK(argc >= 1, "§1.1.1's `condition` did not reach the body — the IDL declares "
                              "`optional boolean condition = false`, so the position is always filled");
            if (JS_ToBool(ctx, argv[0])) return 0;                 /* step 1: "If condition is true, return." */
            list = console_list_from(ctx, 1, argc, argv);
            CHECK(JS_GetLength(ctx, list, &n) == 0, "console: §1.1.1's data has no length");
            if (n == 0) {
                /* step 3: "If data is empty, append message to data." */
                CHECK(JS_SetPropertyUint32(ctx, list, 0, JS_NewString(ctx, MSG)) >= 0,
                      "console: §1.1.1's message could not be appended");
            } else {
                JSValue first = JS_GetPropertyUint32(ctx, list, 0);

                if (!JS_IsString(first)) {
                    /* step 4.2: "If first is not a String, then prepend message to data." */
                    int64_t k;

                    JS_FreeValue(ctx, first);
                    for (k = n; k > 0; k--) {
                        JSValue e = JS_GetPropertyUint32(ctx, list, (uint32_t)(k - 1));

                        CHECK(JS_SetPropertyUint32(ctx, list, (uint32_t)k, e) >= 0,
                              "console: §1.1.1's data could not be shifted");
                    }
                    CHECK(JS_SetPropertyUint32(ctx, list, 0, JS_NewString(ctx, MSG)) >= 0,
                          "console: §1.1.1's message could not be prepended");
                } else {
                    /* step 4.3: "Let concat be the concatenation of message, U+003A (:), U+0020 SPACE, and
                       first. Set data[0] to concat." A String's bytes, so nothing of the page's runs. */
                    const char *f = JS_ToCString(ctx, first);
                    JsonBuf b = { 0 };
                    char *whole;

                    CHECK(f != NULL, "console: §1.1.1's first datum could not be read");
                    json_buf_raw(&b, MSG);
                    json_buf_raw(&b, ": ");
                    json_buf_raw(&b, f);
                    JS_FreeCString(ctx, f);
                    JS_FreeValue(ctx, first);
                    whole = json_buf_take(&b);
                    CHECK(whole != NULL, "console: §1.1.1's concat could not be allocated");
                    CHECK(JS_SetPropertyUint32(ctx, list, 0, JS_NewString(ctx, whole)) >= 0,
                          "console: §1.1.1's concat could not be stored");
                    free(whole);
                    json_buf_free(&b);
                }
            }
            if (!console_logger(ctx, s, level, list)) return 0;
            hdr->stage = CON_FORMAT;
            return JS_STEP_YIELD;
        }

        /* §1.1.11 dirxml(...data): "Let converted be a DOM tree representation of item if possible; otherwise
           let converted be item with optimally useful formatting applied." Both renderings are §2.3's, which
           this implementation performs at the Printer — so finalList is data, element for element, and the
           Logger that follows is §1.1.11's own last step. */
        case M_DIRXML:
        /* §1.1.3-§1.1.6 and §1.1.9: "Perform Logger(<level>, data)." */
        case M_DEBUG: case M_ERROR: case M_INFO: case M_LOG: case M_WARN: {
            if (!console_logger(ctx, s, level, console_list_from(ctx, 0, argc, argv))) return 0;
            hdr->stage = CON_FORMAT;
            return JS_STEP_YIELD;
        }

        /* §1.3.1 group(...data) and §1.3.2 groupCollapsed(...data). Both are Formatter and NOT Logger — the
           standard calls Formatter directly, so a one-element `data` is formatted rather than passed through,
           and the PUSH happens after the Printer. The push is done at the end of CON_FORMAT, which is why the
           two share this machine's second stage rather than each ending here. */
        case M_GROUP:
        case M_GROUP_COLLAPSED: {
            JSValue list = console_list_from(ctx, 0, argc, argv);
            int64_t n = 0;

            CHECK(JS_GetLength(ctx, list, &n) == 0, "console: §1.3.1's data has no length");
            if (n == 0) {
                /* "Otherwise, let groupLabel be an implementation-chosen label representing a group." */
                CHECK(JS_SetPropertyUint32(ctx, list, 0, JS_NewString(ctx, "console.group")) >= 0,
                      "console: §1.3.1's implementation-chosen label could not be built");
                n = 1;
            }
            DCHECK(JS_IsUndefined(s->args), "console: §1.3.1 began with §2.2's list already live");
            s->args = list;
            hdr->stage = CON_FORMAT;
            return JS_STEP_YIELD;
        }

        default:
            DFAIL("a console member reached the machine with a magic no §1.1-§1.4 operation was declared with");
        }
    }

    DCHECK(hdr->stage == CON_FORMAT, "a console member resumed at a stage this machine does not declare");
    DCHECK(JS_IsObject(s->args), "§2.2's Formatter resumed with no list — the list is put in place before the "
                                 "stage advances, so a machine here without one lost it across a park");
    /* THE ONE VALUE THIS STAGE IS HANDED, AND EXACTLY ONE PATH CONSUMES IT. `cb_result` is the answer to
       whatever the machine last parked on, and step_call_run takes ownership of it as its `in` — so it is
       released here on every entry that is NOT a call resume, which is the first entry of the stage and every
       JS_STEP_YIELD re-entry. `phase` is step_call_run's own cursor and is 1 exactly while a call is in
       flight, which is why it is the thing asked rather than the stage. */
    if (s->phase == 0) {
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
    }
    {
        JSValue target, current, converted = JS_UNDEFINED;
        const char *bytes = NULL;
        size_t len = 0;
        char kind = 0;
        int at, r;
        int64_t n = 0;

        CHECK(JS_GetLength(ctx, s->args, &n) == 0, "console: §2.2's list has no length");
        /* §2.2 step 1: "If args's size is 1, return args." Every exit from the loop lands here. */
        if (n <= 1) goto formatted;

        target  = JS_GetPropertyUint32(ctx, s->args, 0);   /* step 2 */
        current = JS_GetPropertyUint32(ctx, s->args, 1);   /* step 3 */
        at = console_find_specifier(ctx, target, &bytes, &len, &kind);   /* step 4 */
        if (at < 0) {
            /* step 4's own answer: "If no format specifier was found, return args." */
            if (bytes) JS_FreeCString(ctx, bytes);
            JS_FreeValue(ctx, target);
            JS_FreeValue(ctx, current);
            goto formatted;
        }

        /* step 5. %s is `Call(%String%, undefined, « current »)`; %d and %i are
           `Call(%parseInt%, undefined, « current, 10 »)` and %f is `Call(%parseFloat%, undefined, « current »)`,
           each preceded by "If current is a Symbol, let converted be NaN". They are CALLS of the realm's
           INTRINSICS and not re-implementations: §solver states that a JS-engine builtin is modeled by running
           the real one, and parseInt's own grammar is not something this file may have a second opinion about.
           A call is where the page's `toString`/`valueOf` runs, so this is a request and the machine parks. */
        if ((kind == 'd' || kind == 'i' || kind == 'f') && JS_IsSymbol(current)) {
            converted = JS_NewFloat64(ctx, 0.0 / 0.0);
        } else if (kind == 's' || kind == 'd' || kind == 'i' || kind == 'f') {
            JSValue fn = console_field(ctx, kind == 's'                  ? CON_STRING
                                          : kind == 'f'                  ? CON_PARSEFLT
                                                                         : CON_PARSEINT);
            JSValue args2[2];
            int nargs = 1;

            args2[0] = current;
            if (kind == 'd' || kind == 'i') { args2[1] = JS_NewInt32(ctx, 10); nargs = 2; }
            r = step_call_run(ctx, &s->phase, s->cb, 4, fn, JS_UNDEFINED, nargs, (JSValueConst *)args2,
                              cb_result, &converted, out_cb, out_argc);
            if (nargs == 2) JS_FreeValue(ctx, args2[1]);
            JS_FreeValue(ctx, fn);
            if (r > 0) {
                JS_FreeCString(ctx, bytes);
                JS_FreeValue(ctx, target);
                JS_FreeValue(ctx, current);
                return r;      /* parked ON THE CALL; the resume re-derives target, current and the specifier */
            }
            DCHECK(r == 0, "step_call_run answered with neither a park nor a result");
            if (JS_IsException(converted)) {
                JS_FreeCString(ctx, bytes);
                JS_FreeValue(ctx, target);
                JS_FreeValue(ctx, current);
                return -1;     /* the page's own toString threw; the member propagates it */
            }
        }
        /* %o, %O and %c: "optionally let converted be …" and "TODO: process %c". The option is DECLINED and
           that is a statement rather than a gap — §2.3's "optimally useful formatting" and "generic JavaScript
           object formatting" are the PRINTER's renderings, and this implementation performs them there, on the
           value itself, rather than baking a rendering into the target string where a later reader could not
           tell it from the page's own text. `converted` is left unset, so step 5's last clause does nothing and
           the specifier stays in `target` — which is exactly what the algorithm does with the option declined,
           and which still consumes `current` at step 6. */

        /* step 5's last clause, then step 6 and step 7's recursion, which is this stage running again. */
        if (!JS_IsUndefined(converted)) {
            JSValue next = console_substitute(ctx, bytes, len, at, converted);

            JS_FreeValue(ctx, converted);
            JS_FreeValue(ctx, target);
            target = next;
        }
        JS_FreeCString(ctx, bytes);
        JS_FreeValue(ctx, current);
        console_advance(ctx, s->args, target);   /* CONSUMES target */
        /* THE LIST IS THE PAGE'S SIZE, so the walk offers a suspend point at every element. A machine that ran
           `console.log(fmt, ...thousands)` to completion inside one opcode is the drive-to-completion this
           engine has no other way to interrupt. */
        return JS_STEP_YIELD;
    }

formatted:
    /* §2.1 step 5's Printer over Formatter's result — and, for §1.3.1/§1.3.2, the PUSH that follows it. */
    console_printer(ctx, level, s->args);
    if (magic == M_GROUP || magic == M_GROUP_COLLAPSED) {
        JSValue groups = console_field(ctx, CON_GROUPS);
        int64_t n = 0;

        CHECK(JS_GetLength(ctx, groups, &n) == 0, "console: §1.3.1's group stack has no length");
        /* "Push group onto the appropriate group stack." The group is the label list — the only part of §1.3's
           "implementation-defined, potentially-interactive view" this environment has. */
        CHECK(JS_SetPropertyUint32(ctx, groups, (uint32_t)n, JS_DupValue(ctx, s->args)) >= 0,
              "console: §1.3.1's group could not be pushed");
        JS_FreeValue(ctx, groups);
    }
    JS_FreeValue(ctx, s->args);
    s->args = JS_UNDEFINED;
    return 0;
}

static const IdlStepDecl CON_DECL = {
    console_step, sizeof(ConsoleState), console_visit, NULL,
    "Console §1.1-§1.4 the console namespace's operations", CON_STEPS
};

/* ---- the per-realm install --------------------------------------------------------------------------------- */

/* §3.13's namespace object, with the Console Standard's own two departures from Web IDL §3.13.1. */
static void console_install_realm(JSContext *ctx)
{
    JSValue ns, rec, global, counts, timers, groups, proto;
    int i;

    global = JS_GetGlobalObject(ctx);

    /* §1: "For historical web-compatibility reasons, the namespace object for console must have as its
       [[Prototype]] an empty object, created as if by ObjectCreate(%ObjectPrototype%), instead of
       %ObjectPrototype%." Web IDL §3.13.1 step 1 would have made it %Object.prototype% directly; a page reads
       the difference with one `Object.getPrototypeOf`. */
    proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(proto), "console: §1's interposed prototype could not be allocated");
    ns = JS_NewObjectProto(ctx, proto);
    JS_FreeValue(ctx, proto);
    CHECK(!JS_IsException(ns), "console: the namespace object could not be allocated");

    /* Web IDL §3.13.1's last line: "The class string of a namespace object is the namespace's identifier."
       §3.2's class-string rule makes that a %Symbol.toStringTag% property, non-writable, non-enumerable,
       configurable — so `Object.prototype.toString.call(console)` is "[object console]".
       IT IS WRITTEN BEFORE THE OPERATIONS, because §3.2 says an object with a class string carries it "at the
       time it is created", and because engine/idl_installed.mjs reads this statement to decide which
       definition the properties installed on this object belong to. */
    idl_namespace_tag(ctx, ns, "console");

    /* Web IDL §3.13.1 step 3 "Define the regular operations of namespace on namespaceObject", whose descriptor
       §3.7.7 states as { [[Writable]]: true, [[Enumerable]]: true, [[Configurable]]: true } — which is what
       idl_install_method's define is. The `length` of each is §3.7.7's count of REQUIRED arguments, and every
       operation here declares only optional ones and variadic tails, so all twenty are 0. */
    for (i = 0; i < M_N; i++)
        idl_install_method(ctx, ns, CONSOLE_MEMBER[i], g_id[i]);

    /* THE RECORD, BUILT BEFORE THE NAMESPACE OBJECT IS REACHABLE. Its fields are §1.2's count map, §1.3's group
       stack, §1.4's timer table and §2.2's three intrinsics. */
    counts = JS_NewObjectProto(ctx, JS_NULL);
    CHECK(!JS_IsException(counts), "console: §1.2's count map could not be allocated");
    timers = JS_NewObjectProto(ctx, JS_NULL);
    CHECK(!JS_IsException(timers), "console: §1.4's timer table could not be allocated");
    groups = JS_NewArray(ctx);
    CHECK(!JS_IsException(groups), "console: §1.3's group stack could not be allocated");
    rec = JS_NewObjectProto(ctx, JS_NULL);
    CHECK(!JS_IsException(rec), "console: the per-realm record could not be allocated");
    CHECK(JS_SetPropertyStr(ctx, rec, CON_COUNTS, counts) >= 0, "console: §1.2's count map could not be held");
    CHECK(JS_SetPropertyStr(ctx, rec, CON_TIMERS, timers) >= 0, "console: §1.4's timer table could not be held");
    CHECK(JS_SetPropertyStr(ctx, rec, CON_GROUPS, groups) >= 0, "console: §1.3's group stack could not be held");
    /* THE INTRINSICS, READ FROM A GLOBAL NO PAGE SCRIPT HAS TOUCHED. core/realm.h's list runs at the realm's
       creation, so these three are %String%, %parseInt% and %parseFloat% by construction rather than by hope —
       and holding them is what makes `parseInt = null; console.log("%d", "7")` still print 7. */
    {
        static const char *const NAME[3] = { "String", "parseInt", "parseFloat" };
        static const char *const FIELD[3] = { CON_STRING, CON_PARSEINT, CON_PARSEFLT };

        for (i = 0; i < 3; i++) {
            JSValue fn = JS_GetPropertyStr(ctx, global, NAME[i]);

            DCHECK(JS_IsFunction(ctx, fn),
                   "§2.2's Formatter was given a realm whose %String%/%parseInt%/%parseFloat% is not a "
                   "function — these are read at the realm's creation, before any script of its document, so "
                   "one that is missing is an intrinsic quickjs did not install rather than a page's edit");
            CHECK(JS_SetPropertyStr(ctx, rec, FIELD[i], fn) >= 0,
                  "console: §2.2's conversion intrinsic could not be held");
        }
    }
    realm_value_set(ctx, g_rec_slot, rec);   /* asserts it is set exactly once in this realm */

    /* §3.13: "For every namespace that is exposed in a given realm, a corresponding property exists on the
       realm's global object." Web IDL §3.13's own step performs DefineMethodProperty(target, id,
       namespaceObject, false), which is Web IDL §3.8's descriptor — stated once for every name a realm's
       global carries, rather than by these bits being written out here and in css_namespace.c and nowhere
       else. §1: "For historical reasons, console is lowercased." */
    idl_define_global_property_reference(ctx, global, "console", ns);
    JS_FreeValue(ctx, global);
}

void console_init(JSContext *ctx)
{
    /* `any... data` — the tail's type is the last declared one, which is what `T...` states. */
    static const IdlArgType ANY_TAIL[]    = { IDL_ANY };
    /* `optional boolean condition = false, any... data` */
    static const IdlArgType ASSERT_ARGS[] = { IDL_BOOLEAN, IDL_ANY };
    /* `optional DOMString label = "default"` */
    static const IdlArgType LABEL_ARG[]   = { IDL_DOMSTRING };
    /* `optional DOMString label = "default", any... data` */
    static const IdlArgType LABEL_TAIL[]  = { IDL_DOMSTRING, IDL_ANY };
    /* `optional any tabularData, optional sequence<DOMString> properties` */
    static const IdlArgType TABLE_ARGS[]  = { IDL_ANY, IDL_SEQUENCE_DOMSTRING };
    /* `optional any item, optional object? options` */
    static const IdlArgType DIR_ARGS[]    = { IDL_ANY, IDL_OBJECT_NULLABLE };
    int i;

    DCHECK(g_rec_slot < 0, "console_init ran twice — the twenty pool entries and the realm-value slot are the "
                           "AGENT's and are declared once in it");
    for (i = 0; i < M_N; i++) g_id[i] = -1;
    g_rec_slot = realm_value_declare(ctx, "Console §1.2/§1.3/§1.4 the console namespace object's count map, "
                                          "group stack and timer table, with §2.2's conversion intrinsics");

    g_id[M_ASSERT] = idl_method_id_step(ctx, ASSERT_ARGS, 2, NULL, 0, &CON_DECL, M_ASSERT);
    idl_variadic();
    idl_optional_from(0);
    idl_arg_default(0, IDL_DEFAULT_FALSE, NULL);
    g_id[M_CLEAR] = idl_method_id_step(ctx, NULL, 0, NULL, 0, &CON_DECL, M_CLEAR);
    g_id[M_DEBUG] = idl_method_id_step(ctx, ANY_TAIL, 1, NULL, 0, &CON_DECL, M_DEBUG);
    idl_variadic();
    idl_optional_from(0);
    g_id[M_ERROR] = idl_method_id_step(ctx, ANY_TAIL, 1, NULL, 0, &CON_DECL, M_ERROR);
    idl_variadic();
    idl_optional_from(0);
    g_id[M_INFO] = idl_method_id_step(ctx, ANY_TAIL, 1, NULL, 0, &CON_DECL, M_INFO);
    idl_variadic();
    idl_optional_from(0);
    g_id[M_LOG] = idl_method_id_step(ctx, ANY_TAIL, 1, NULL, 0, &CON_DECL, M_LOG);
    idl_variadic();
    idl_optional_from(0);
    g_id[M_TABLE] = idl_method_id_step(ctx, TABLE_ARGS, 2, NULL, 0, &CON_DECL, M_TABLE);
    idl_optional_from(0);
    g_id[M_TRACE] = idl_method_id_step(ctx, ANY_TAIL, 1, NULL, 0, &CON_DECL, M_TRACE);
    idl_variadic();
    idl_optional_from(0);
    g_id[M_WARN] = idl_method_id_step(ctx, ANY_TAIL, 1, NULL, 0, &CON_DECL, M_WARN);
    idl_variadic();
    idl_optional_from(0);
    g_id[M_DIR] = idl_method_id_step(ctx, DIR_ARGS, 2, NULL, 0, &CON_DECL, M_DIR);
    idl_optional_from(0);
    g_id[M_DIRXML] = idl_method_id_step(ctx, ANY_TAIL, 1, NULL, 0, &CON_DECL, M_DIRXML);
    idl_variadic();
    idl_optional_from(0);
    g_id[M_COUNT] = idl_method_id_step(ctx, LABEL_ARG, 1, NULL, 0, &CON_DECL, M_COUNT);
    idl_optional_from(0);
    idl_arg_default(0, IDL_DEFAULT_STRING, "default");
    g_id[M_COUNT_RESET] = idl_method_id_step(ctx, LABEL_ARG, 1, NULL, 0, &CON_DECL, M_COUNT_RESET);
    idl_optional_from(0);
    idl_arg_default(0, IDL_DEFAULT_STRING, "default");
    g_id[M_GROUP] = idl_method_id_step(ctx, ANY_TAIL, 1, NULL, 0, &CON_DECL, M_GROUP);
    idl_variadic();
    idl_optional_from(0);
    g_id[M_GROUP_COLLAPSED] = idl_method_id_step(ctx, ANY_TAIL, 1, NULL, 0, &CON_DECL, M_GROUP_COLLAPSED);
    idl_variadic();
    idl_optional_from(0);
    g_id[M_GROUP_END] = idl_method_id_step(ctx, NULL, 0, NULL, 0, &CON_DECL, M_GROUP_END);
    g_id[M_TIME] = idl_method_id_step(ctx, LABEL_ARG, 1, NULL, 0, &CON_DECL, M_TIME);
    idl_optional_from(0);
    idl_arg_default(0, IDL_DEFAULT_STRING, "default");
    g_id[M_TIME_LOG] = idl_method_id_step(ctx, LABEL_TAIL, 2, NULL, 0, &CON_DECL, M_TIME_LOG);
    idl_variadic();
    idl_optional_from(0);
    idl_arg_default(0, IDL_DEFAULT_STRING, "default");
    g_id[M_TIME_END] = idl_method_id_step(ctx, LABEL_ARG, 1, NULL, 0, &CON_DECL, M_TIME_END);
    idl_optional_from(0);
    idl_arg_default(0, IDL_DEFAULT_STRING, "default");

    for (i = 0; i < M_N; i++)
        DCHECK(g_id[i] >= 0, CONSOLE_MEMBER[i]);
    agent_state_id("console", &g_rec_slot, "the per-realm record's slot number");
    for (i = 0; i < M_N; i++)
        agent_state_id("console", &g_id[i], "one of §1.1-§1.4's twenty operations");
    realm_declare_intrinsic(console_install_realm);
}

void console_free(void)
{
    int i;

    for (i = 0; i < M_N; i++) g_id[i] = -1;
    g_rec_slot = -1;
}
