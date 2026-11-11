/* Which absent read is input, and which is a component this engine owes.
 *
 * Both look the same: a name that resolves nowhere. Server-injected app state (`window.__FLAGS`, `__USER`,
 * `gon`) is written for a logged-in visitor and absent for this one, so its value is unknown input: the read
 * answers a symbolic value, the gate over it forks, and the logged-in arm is explored. A Web API this engine
 * has not built is honestly absent: its ReferenceError names the component to write.
 *
 * The standards decide which is which. A global name belongs to the platform when a generated vocabulary holds
 * it: browser/platform_names.h (the names Web IDL exposes on Window, engine/idlgen.mjs),
 * browser/language_names.h (ECMAScript §19, engine/esglobalgen.mjs) and browser/i18n_names.h (ECMA-402 §8 The
 * Intl Object, engine/i18nglobalgen.mjs). A name no vocabulary claims is answered as app state. That is a
 * default and not a decision: another host's global or a library's own store (`process`, `webpackJsonp`) is
 * indistinguishable from `__FLAGS` at the read, and a standard with no table here falls into it too. The
 * default errs toward the fork because a concrete `undefined` buries the code behind the gate. Nothing in the
 * program's own handling of absence (a `typeof` guard, `x = x || {}`) can decide it, since the program's
 * authors do not know either (`window.__FLAGS = window.__FLAGS || {}` is an ordinary hydration line); only a
 * fact about the producer can. This arm has no cap, and ordering the worlds it creates is the scheduler's.
 *
 * The same question is asked of a published record. A member the record lacks (`gon.current_user_id` on
 * `window.gon={}`) is unknown, and a member it holds (`window.__FLAGS={admin:false}`) is unknown for control
 * flow with the served bytes as its example. Both halves share one path composition and one registry, because
 * a member's identity is its whole path whether or not the record holds it.
 *
 * The engine decides which containers are on this channel and this file never widens it. A container is on it
 * iff the document's own bytes wrote its extent (an object or array literal, or a `JSON.parse`, in an inline
 * `<script>`), granted at the operation that writes the member list (js_document_container_grant). A list is
 * only a path component (`__STATE__.users[0]`), never a base this file is asked about (§10.4.2 Array Exotic
 * Objects: the list states its own length). A platform object is off the channel: Web IDL §3.8 "Platform
 * objects implementing interfaces" makes its member list the interface's, so a member it lacks answers
 * `undefined`, and a member of an unbuilt interface is honestly absent.
 *
 * The registry holds no reference. A row is consulted only for an object the engine has marked published; the
 * mark is cleared at every allocation and set only by the publication that files the row, so a recycled
 * address cannot answer with a dead document's path. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "solver/absent.h"
#include "solver/concolic.h"

#define ABSENT_COMPONENT "absent"   /* the claimant of the precondition rows this file declares */
#include "solver/flow.h"     /* flow_mark_forced_arm, for the read this session throws at */
#include "browser/i18n_names.h"
#include "browser/language_names.h"
#include "browser/obsolete_names.h"
#include "browser/platform_names.h"
#include "solver/reclaim.h"

#define PLATFORM_NAMES_N ((int)(sizeof(PLATFORM_NAMES) / sizeof(PLATFORM_NAMES[0])))
#define LANGUAGE_NAMES_N ((int)(sizeof(LANGUAGE_NAMES) / sizeof(LANGUAGE_NAMES[0])))
#define I18N_NAMES_N ((int)(sizeof(I18N_NAMES) / sizeof(I18N_NAMES[0])))
#define OBSOLETE_NAMES_N ((int)(sizeof(OBSOLETE_NAMES) / sizeof(OBSOLETE_NAMES[0])))

/* Membership in one generated vocabulary, by binary search: each table is sorted by its generator, and the
   lookup runs on every unresolved global read. A disordered table would miss silently and answer a standard's
   name as app state, so the order is asserted once per table in dev builds; `checked` is that latch and
   `which` names the vocabulary in the abort. Returns the table's own static entry, which callers key on by
   pointer so that no page byte enters anything keyed that way, or NULL when the table does not hold the name. */
static const char *names_find(const char *const *tbl, int n, const char *name, const char *which, int *checked)
{
    int lo = 0, hi = n - 1;

    DCHECK(name != NULL, "a generated vocabulary was asked about no name at all");
#if APICLIENT_DEV
    if (!*checked) {
        int i;

        *checked = 1;
        for (i = 1; i < n; i++)
            DCHECKF(strcmp(tbl[i - 1], tbl[i]) < 0,
                    "the %s vocabulary is not in strcmp order at entry %d (\"%s\" then \"%s\") — this "
                    "lookup is a binary search over a table its generator sorts, and a disordered table does "
                    "not report an error, it MISSES: a name the standard owns is then answered as "
                    "server-injected app state and a branch on it FORKS instead of naming the component this "
                    "engine owes",
                    which, i, tbl[i - 1], tbl[i]);
    }
#else
    (void)which;
    (void)checked;
#endif
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        int c = strcmp(name, tbl[mid]);
        if (c == 0)
            return tbl[mid];
        if (c < 0)
            hi = mid - 1;
        else
            lo = mid + 1;
    }
    return NULL;
}

/* One flag per table; the host is one agent per instance and nothing here is entered from a second thread. */
static int g_platform_sorted, g_language_sorted, g_i18n_sorted, g_obsolete_sorted;

#if APICLIENT_DEV
/* The vocabularies are disjoint. absent_standard_name returns the first table that holds a name and its search
   order is chosen for cost, so an overlap would let that order decide which standard owes the name. Runs once.
   The searches are hoisted out of the DCHECK conditions because names_find writes its `checked` latch. */
static void vocab_assert_disjoint(void)
{
    int i;

    for (i = 0; i < LANGUAGE_NAMES_N; i++) {
        const char *dup = names_find(PLATFORM_NAMES, PLATFORM_NAMES_N, LANGUAGE_NAMES[i],
                                     "Web IDL [Exposed=Window]", &g_platform_sorted);
        DCHECKF(dup == NULL, "\"%s\" is in BOTH browser/platform_names.h and browser/language_names.h, so "
                             "which standard owns it is decided by the order absent_standard_name happens to "
                             "search — and that order is written for cost. Two standards claiming one global "
                             "name is a fact about the standards, and this file may not resolve it by luck",
                LANGUAGE_NAMES[i]);
    }
    for (i = 0; i < I18N_NAMES_N; i++) {
        const char *dp = names_find(PLATFORM_NAMES, PLATFORM_NAMES_N, I18N_NAMES[i],
                                    "Web IDL [Exposed=Window]", &g_platform_sorted);
        const char *dl = names_find(LANGUAGE_NAMES, LANGUAGE_NAMES_N, I18N_NAMES[i],
                                    "ECMAScript §19 global-object", &g_language_sorted);
        DCHECKF(dp == NULL, "\"%s\" is in BOTH browser/i18n_names.h and browser/platform_names.h — see the "
                            "disjointness note above", I18N_NAMES[i]);
        DCHECKF(dl == NULL, "\"%s\" is in BOTH browser/i18n_names.h and browser/language_names.h — ECMA-402 "
                            "putting a name ECMAScript §19 already names would mean this engine owes it under "
                            "two standards, which is a fact to read rather than to search past", I18N_NAMES[i]);
    }
    /* A removed name no live table holds: engine/obsoletegen.mjs subtracts the three, so an overlap here is a
       live table regenerated after browser/obsolete_names.h — rerun that generator. */
    for (i = 0; i < OBSOLETE_NAMES_N; i++) {
        const char *dp = names_find(PLATFORM_NAMES, PLATFORM_NAMES_N, OBSOLETE_NAMES[i],
                                    "Web IDL [Exposed=Window]", &g_platform_sorted);
        const char *dl = names_find(LANGUAGE_NAMES, LANGUAGE_NAMES_N, OBSOLETE_NAMES[i],
                                    "ECMAScript §19 global-object", &g_language_sorted);
        const char *di = names_find(I18N_NAMES, I18N_NAMES_N, OBSOLETE_NAMES[i],
                                    "ECMA-402 §8 The Intl Object", &g_i18n_sorted);

        DCHECKF(dp == NULL && dl == NULL && di == NULL,
                "\"%s\" is in browser/obsolete_names.h and in a live vocabulary — a standard still defines it, "
                "so it is not removed from the platform; rerun engine/obsoletegen.mjs", OBSOLETE_NAMES[i]);
    }
}
#endif

/* Which standard's vocabulary holds a global name: returns the table's entry and writes `*vocab`, or returns
   NULL when none does. Every realm has %Intl% (core/intl/intl.h), so that name misses only where the page
   deleted it; an unbuilt member of it misses on the Intl object, which this file is not asked about. */
const char *absent_standard_name(const char *name, AbsentVocab *vocab)
{
    const char *e;

    DCHECK(vocab != NULL, "a global name was asked which standard owns it with nowhere to put the answer — "
                          "the vocabulary is the half of this answer a `@WHY` has to be able to state, and a "
                          "caller that wants only the yes/no is a caller that will re-derive it wrongly");
#if APICLIENT_DEV
    {
        static int checked;
        if (!checked) { checked = 1; vocab_assert_disjoint(); }
    }
#endif
    e = names_find(PLATFORM_NAMES, PLATFORM_NAMES_N, name, "Web IDL [Exposed=Window]", &g_platform_sorted);
    if (e) {
        *vocab = ABSENT_VOCAB_WEBIDL;
        return e;
    }
    e = names_find(LANGUAGE_NAMES, LANGUAGE_NAMES_N, name, "ECMAScript §19 global-object", &g_language_sorted);
    if (e) {
        *vocab = ABSENT_VOCAB_ECMASCRIPT;
        return e;
    }
    /* Asked last because it is the smallest table; the tables are disjoint, so the order decides nothing. */
    e = names_find(I18N_NAMES, I18N_NAMES_N, name, "ECMA-402 §8 The Intl Object", &g_i18n_sorted);
    if (e) {
        *vocab = ABSENT_VOCAB_ECMA402;
        return e;
    }
    return NULL;
}

/* The owed-name census: what this document asked a standard for and this realm did not answer.
 *
 * `new EventSource(url)` on an unbuilt API throws and names itself. `if (window.EventSource)` does not: the
 * read misses, the suppression below leaves it alone, §10.1.8.1 OrdinaryGet ( O, P, Receiver ) step 2.b
 * answers `undefined`, and the branch behind the guard is unreachable with nothing saying so. So the
 * suppression records what it suppressed, and decides nothing differently. The population is the names this
 * document read that a vocabulary claims and this realm missed, never the names the build does not install.
 *
 * Every number is a lifetime count of events: monotone within one agent's life, and zeroed with the
 * published-namespace registry at absent_free on the agent's release. It shares that lifetime with
 * `_candidates` and `_sourceReads` on the result document, and not with `_switches`, which is the instance's.
 *
 * The denominator is the reads of the global object this file was told about, by any entry. It partitions
 * into four arms, all emitted, and the composer asserts `owed + removed + index + appState == globalReads`:
 *   owed       a standard owns the name and this realm has none (the rows, summed)
 *   removed    a standard removed the name (browser/obsolete_names.h); absent, and nothing is owed
 *   index      an integer key on the global (HTML §7.2.2.2 Indexed access on the Window object)
 *   app state  no vocabulary claimed the name
 * The hook is reached only on a miss, so this is not a fraction of the platform surface a document uses. */
/* Three operations ask about a global name and answer with no [[Get]], so the read hook is never asked. The
 * engine records each on its way past (JSConcolicHooks.absent_unresolved -> absent_unresolved_note), and a
 * name read every way raises one row.
 *   typeof       OP_get_var_undef, produced only at the parser's `una_typeof_done`, answers ECMAScript §13.5.3
 *                The typeof Operator's §13.5.3.1 Runtime Semantics: Evaluation step 2.a, "If
 *                IsUnresolvableReference(value) is true, return "undefined".", before any GetValue.
 *   in           OP_in's §13.10.1 Runtime Semantics: Evaluation ends at "Return ? HasProperty(rightValue, ?
 *                ToPropertyKey(leftValue))": §7.3.11 HasProperty ( obj, propertyKey ), not §7.3.2 Get. It is
 *                recorded at the operator's placement (do_opkeyed_place), not at the [[HasProperty]] miss
 *                every identifier resolution also issues. The engine filters a symbol key on this arm.
 *   Reflect.has  §28.1.8 "Reflect.has ( target, key )": the same §7.3.11 reached by a call, recorded from
 *                js_reflect_prop_step's GP_HAS arm.
 * Each is a cut of the denominator and not a fifth arm, asserted `<= globalReads` on its own, and kept a
 * separate member because a bundle writes one spelling. Only the read entry mints, so the app-state arm alone
 * does not count the unknowns minted on the global. */
/* Named residual: not covered — an own-only query on the global: `Object.hasOwn(globalThis, "X")`,
 * `Object.prototype.hasOwnProperty.call(window, "X")` (one step machine, `js_hasown_step`, whose argument
 * chooses which of the two orders §20.1.2.14 and §20.1.3.2 state) and
 * `Object.getOwnPropertyDescriptor(window, "X")`. Each answers with no [[Get]] and none reaches this census.
 * The gates the entries above use (the base is the global, the answer is false, the key is nameable) do not
 * transfer: those entries establish absence on the whole prototype chain, and §7.3.12 HasOwnProperty
 * establishes own-absence only, so a name this engine answers by inheritance (`addEventListener`, on
 * EventTarget.prototype; DOM §2.7 Interface EventTarget) would be filed as owed. Next diff — the engine
 * stating chain-absence at that site beside the own-absence it already computed, or a JSConcolicAbsentOp
 * member whose host entry counts the detect without entering the owed arm. Absence shows as — for one
 * document and one absent name, a census that moves when the detect is spelled `"X" in window` or
 * `Reflect.has(window, "X")` and does not move when it is spelled `Object.hasOwn(window, "X")`. */
/* Named residual: not covered — whether a recorded read was the silent one. js_absent_ask has two callers, the
 * property-read miss that degrades to `undefined` and the unresolvable Reference that throws, and both land in
 * the row's `read` bucket. Next diff — that bucket split by the `answer` the read hook receives. Absence shows
 * as — a page that only writes `new EventSource(…)` is fully diagnosed by its ReferenceError in `pageErrors`
 * and still appears here, so a name in this census with no matching `pageErrors` entry is the silent case and
 * a name in both is ambiguous. */
/* Which of this file's entries raised a read. It is a host enum and not the engine's JSConcolicAbsentOp,
   because that enum names only the operations that answer with no [[Get]] and so has no member for the read
   hook's entry; the engine's members map onto these in absent_unresolved_note and never the other way.
   `ABSENT_ENTRY_N` is the count, and the poison an unclassified read carries into owed_note's assert. */
typedef enum { ABSENT_ENTRY_READ, ABSENT_ENTRY_TYPEOF, ABSENT_ENTRY_IN, ABSENT_ENTRY_HAS_CALL,
               ABSENT_ENTRY_N } AbsentEntry;

/* One row per owed name. `by_entry` holds counts and not one latched entry, because a row accumulates events
   for the agent's life and one name is commonly read through two entries (`typeof X !== "undefined" && new
   X()`). The buckets sum to `reads`, which the composer asserts. */
typedef struct { const char *name; long reads; long by_entry[ABSENT_ENTRY_N]; AbsentVocab vocab; } OwedRow;
static OwedRow *g_owed;
static int g_owed_n, g_owed_cap;
/* The arms of the global miss, each raised at the site that decides it so a reader can see which arm moved. */
static long g_owed_reads, g_index_refused, g_appstate_reads;
/* A name a standard removed (browser/obsolete_names.h), left absent as a browser leaves it. */
static long g_removed_reads;
/* The population the four arms partition. It is raised on its own at the top of global_miss_note and not
   derived as their sum, so the composer's identity can fail. */
static long g_global_reads;
/* The cuts of that population by which no-[[Get]] operation asked; this one is `typeof`. A cut is independent
   of the arms: such a read still lands in whichever arm its name decides. Each cut is raised in
   absent_unresolved_note before the classification and asserted `<= g_global_reads` in the composer. */
static long g_unresolved_reads;
/* `"X" in window`, ECMAScript §13.10.1 Runtime Semantics: Evaluation's
   `RelationalExpression : RelationalExpression in ShiftExpression`. */
static long g_in_reads;
/* `Reflect.has(window, "X")`, ECMAScript §28.1.8 "Reflect.has ( target, key )" step 3's
   "Return ? target.[[HasProperty]](propertyKey)": the same §7.3.11 "HasProperty ( obj, propertyKey )" the `in`
   operator ends at, reached by a call. */
static long g_has_call_reads;
/* The owed reads split by which standard owes them: an unbuilt Web IDL interface is a browser component to
   write, and an uninstalled ECMAScript §19 name is a language intrinsic this build did not link. */
static long g_owed_by_vocab[ABSENT_VOCAB_N];

/* Records one read of a name a standard owns and this realm does not answer. `name` is the vocabulary's own
   entry, so rows are keyed by pointer. The table has no cap and no seen-set: a repeated read raises its row,
   because the census reports both how many names went unanswered and how often each was read. The scan is
   linear over pointer equality; its rows are the distinct owed names one document read. */
static void owed_note(const char *name, AbsentVocab vocab, AbsentEntry entry)
{
    int i, e;

    DCHECK(name != NULL, "a read was recorded as owed with no name — the caller's name is the vocabulary's own "
                         "entry and absent_standard_name returns NULL rather than an empty one, so this is a "
                         "caller recording the MISS arm of that lookup as though it were the hit arm");
    /* CHECKs and not DCHECKs: the lines below index arrays with `vocab` and `entry` in every build. */
    CHECKF(vocab < ABSENT_VOCAB_N, "a read was recorded as owed by a standard this file has no vocabulary for "
                                   "(%d of %d) — the enum is the only thing that mints one and "
                                   "absent_standard_name writes it on every hit, so an out-of-range value is a "
                                   "caller passing an uninitialised local, and the line below indexes the "
                                   "per-standard split with it", (int)vocab, (int)ABSENT_VOCAB_N);
    CHECKF(entry < ABSENT_ENTRY_N, "a read was recorded as owed by an entry this file does not have (%d of "
                                   "%d) — the enum is the only thing that mints one and every caller states "
                                   "it, so an out-of-range value is the poison an unclassified arm carries "
                                   "and the line below indexes the row's per-entry split with it",
           (int)entry, (int)ABSENT_ENTRY_N);
    g_owed_reads++;
    g_owed_by_vocab[vocab]++;
    for (i = 0; i < g_owed_n; i++)
        if (g_owed[i].name == name) {
            g_owed[i].reads++;
            g_owed[i].by_entry[entry]++;
            return;
        }
    if (g_owed_n == g_owed_cap) {
        int cap = g_owed_cap ? g_owed_cap * 2 : 8;
        OwedRow *rows = (OwedRow *)realloc(g_owed, sizeof(*rows) * (size_t)cap);
        CHECK(rows != NULL, "absent: OOM growing the table of standard-owned names this realm did not answer — "
                            "the alternative to failing here is publishing a census that reads clean for a "
                            "document whose feature branches were all taken false");
        g_owed = rows;
        g_owed_cap = cap;
    }
    g_owed[g_owed_n].name = name;
    g_owed[g_owed_n].reads = 1;
    /* realloc does not zero what it grows into, so the new row's buckets are zeroed here. */
    for (e = 0; e < ABSENT_ENTRY_N; e++)
        g_owed[g_owed_n].by_entry[e] = 0;
    g_owed[g_owed_n].by_entry[entry] = 1;
    g_owed[g_owed_n].vocab = vocab;
    g_owed_n++;
}

/* Classifies one miss on the global by its name, and raises that arm's counter and the denominator. Every
   entry calls it, so the read hook and the no-[[Get]] entries cannot classify one name two ways. Returns 1
   when the read is left alone (a standard owes or removed the name, or the key is an integer) and 0 when no
   vocabulary claimed it, the arm the read hook mints on. `key` is the atom as ns_key_str spelled it; `entry`
   is which entry asked, stated by the caller and carried to the owed row.
   A symbol never arrives, because every entry applies the key rule first. js_absent_ask and the `in` and
   `Reflect.has` arms filter, since their key is the page's; the `typeof` arm asserts, since the grammar makes
   its atom an identifier's. */
static int global_miss_note(const char *key, JSAtom name, AbsentEntry entry)
{
    /* The poison owed_note asserts on: absent_standard_name writes `vocab` on every hit and only then. */
    AbsentVocab vocab = ABSENT_VOCAB_N;
    const char *owed;

    /* owed_note's CHECK is reached only on the owed arm; this fires on the first read of any kind that arrives
       unclassified. */
    DCHECKF(entry < ABSENT_ENTRY_N,
            "a read of the global object reached the classification with no entry stated (%d of %d) — every "
            "caller of this function names which of the three it is, and the only value that is none of them "
            "is the poison absent_unresolved_note opens with, so this is a switch over JSConcolicAbsentOp "
            "that gained a member and classified it nowhere",
            (int)entry, (int)ABSENT_ENTRY_N);
    g_global_reads++;
    /* A name a standard owns is a component this engine owes: the read is left alone so its throw names it.
       Asked only of the global, because `gon.Node` is a field of an app record that happens to be spelled
       like an interface. */
    owed = absent_standard_name(key, &vocab);
    if (owed) {
        /* Recorded on the way past; the decision is unchanged. */
        owed_note(owed, vocab, entry);
        return 1;
    }
    /* A name a standard removed is neither owed nor app state: every browser answers it absent, so the read is
       left alone. The tables are disjoint (vocab_assert_disjoint), so asking after the live ones decides
       nothing by order. */
    if (names_find(OBSOLETE_NAMES, OBSOLETE_NAMES_N, key, "removed-interface", &g_obsolete_sorted)) {
        g_removed_reads++;
        return 1;
    }
    /* An integer key on the global is not a field of a record. It is HTML §7.2.2.2 Indexed access on the
       Window object ("Indexed access to document-tree child navigables is defined through the
       [[GetOwnProperty]] internal method of the WindowProxy object"), so its extent is the interface's, as
       Web IDL §3.8 Platform objects implementing interfaces makes a platform object's member list. This
       engine owns the navigable tree, so an index past the child count has the computed answer of §10.1.8.1
       OrdinaryGet ( O, P, Receiver ) step 2.b, `undefined`, and no unknown is minted. The rule is this arm's
       and not ns_join's: a record an inline script publishes at `window[0]` is present, and its members are
       read through the record arm. `typeof` cannot reach this arm, since ECMAScript §12.7 Names and Keywords
       gives no IdentifierStart that is a decimal digit. */
    if (JS_AtomIsIndexName(name)) {
        g_index_refused++;
        return 1;
    }
    /* Counted where the arm is decided and not at the mint: the mint is shared with the record arm, and a
       no-[[Get]] entry decides this arm and mints nothing. */
    g_appstate_reads++;
    return 0;
}

/* One formatted append for the composer's two passes. `out == NULL` is the measuring pass (C99 §7.19.6.5 "The
   snprintf function": "If n is zero, nothing is written, and s may be a null pointer"), so the same argument
   list is measured and then written with no hand-counted size. The fit is asserted on the writing pass only;
   the measuring pass has no buffer. */
static size_t absent_emitf(char *out, size_t cap, size_t len, const char *fmt, ...) APICLIENT_PRINTF(4, 5);
static size_t absent_emitf(char *out, size_t cap, size_t len, const char *fmt, ...)
{
    va_list ap;
    int need;

    va_start(ap, fmt);
    need = out ? vsnprintf(out + len, cap - len, fmt, ap) : vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    CHECK(need >= 0, "absent: the owed-name census could not be MEASURED — vsnprintf reported an encoding "
                     "error, so there is no length to allocate against and any size chosen here would be the "
                     "hand-counted guess solver/compose.h exists to stop anyone writing");
    DCHECK(!out || (size_t)need < cap - len,
           "the owed-name census wrote past the length its own measuring pass computed — the two passes read "
           "one argument list over one table, so they can only disagree if a row changed between them, and "
           "what a truncation here loses is not a digit but the CLOSING BRACE: the document that embeds this "
           "census would not parse and every finding for the page would be discarded");
    return len + (size_t)need;
}

/* Composes the census as one JSON object; the caller frees it. Row keys are written with no escaping pass,
   which is sound only because each is a generated vocabulary's own entry (owed_note keys on that pointer) and
   never a page's bytes; the row loop asserts it. Members open on `_` and rows cannot, so a consumer sums the
   rows' buckets for the reads and reads the members as the population they are drawn from. Every member is
   emitted on every census, zeroes included, so the object is never `{}` and a clean run reads `owed 0 of N`.
   testing/absent_census.js reads the `KEY_` declarations in this function's body by their C identifiers. */
char *absent_json(void)
{
    /* Member names are prose with the value appended: extension/popup.js renders a census as `key value`. */
    static const char KEY_READS[] = "_reads of the global object this file was asked about";
    static const char KEY_OWED[]  = "_of those, reads of a name below — a standard owns it and this realm has none";
    static const char KEY_REMOVED[] = "_of those, a name a standard removed — absent, as in a browser";
    static const char KEY_INDEX[] = "_of those, refused as an integer key on the global";
    static const char KEY_APP[]   = "_of those, no standard owns the name — server-injected app state";
    static const char KEY_TYPEOF[] = "_of those, answered by the typeof operator with no [[Get]] performed";
    static const char KEY_IN[]    = "_of those, answered by the in operator with no [[Get]] performed";
    static const char KEY_HASCALL[] = "_of those, answered by Reflect.has with no [[Get]] performed";
    /* One per vocabulary, indexed by the enum. The array is sized, so a vocabulary added without a key compiles
       and writes `(null)`; KEY_ENTRY's unsized form with a count assert is the one to copy. */
    static const char *const KEY_VOCAB[ABSENT_VOCAB_N] = {
        "_of those owed reads, names Web IDL exposes on Window",
        "_of those owed reads, names ECMAScript §19 The Global Object puts there",
        "_of those owed reads, names ECMA-402 §8 The Intl Object puts there"
    };
    char *out = NULL;
    /* Bucket names, rendered inside a row (`EventSource read 1, typeof 1, in 1`). The array is unsized and its
       count is asserted at compile time below, so an entry added without a name does not compile. */
    static const char *const KEY_ENTRY[] = { "read", "typeof", "in", "Reflect.has" };
    size_t cap = 0, len = 0;
    long sum = 0, rowsby[ABSENT_VOCAB_N], rowsbyentry[ABSENT_ENTRY_N];
    int pass, i, v, e;

    _Static_assert(sizeof KEY_ENTRY / sizeof *KEY_ENTRY == ABSENT_ENTRY_N,
                   "absent.c's census has an entry with no bucket name, or a name with no entry");
    for (v = 0; v < ABSENT_VOCAB_N; v++)
        rowsby[v] = 0;
    for (e = 0; e < ABSENT_ENTRY_N; e++)
        rowsbyentry[e] = 0;
    /* The rows are summed whole, per entry and per standard, and compared below with the counters owed_note
       raises per event. The two sides are raised at different places, so they disagree if the grow path
       drops a row or a bucket is raised twice. */
    for (i = 0; i < g_owed_n; i++) {
        long rsum = 0;

        sum += g_owed[i].reads;
        for (e = 0; e < ABSENT_ENTRY_N; e++) {
            rsum += g_owed[i].by_entry[e];
            rowsbyentry[e] += g_owed[i].by_entry[e];
        }
        /* The buckets partition the row's reads. */
        DCHECKF(rsum == g_owed[i].reads,
                "the owed-name census holds a row (\"%s\") whose per-entry split does not sum to its own "
                "reads (%ld by the buckets, %ld by the row) — owed_note raises exactly one bucket per read "
                "and zeroes every bucket of a row it creates, so a mismatch is a row the grow path left "
                "uninitialised or a bucket raised beside a read rather than as part of one",
                g_owed[i].name, rsum, g_owed[i].reads);
        /* A CHECK: the line below indexes `rowsby` with this value in every build. */
        CHECKF(g_owed[i].vocab < ABSENT_VOCAB_N,
               "the owed-name census holds a row (\"%s\") filed under a standard this file has no vocabulary "
               "for — owed_note asserts the same thing at the raise, so a row that reaches the composer with "
               "one is a row written past the end of the table or a struct laid out differently by the two",
               g_owed[i].name);
        rowsby[g_owed[i].vocab] += g_owed[i].reads;
    }
    /* The identities, asserted where all their terms are in hand. */
    DCHECK(sum == g_owed_reads,
           "the owed-name census's rows do not sum to the owed reads it counted, and the object is where that "
           "identity is READ — a consumer takes the share of unanswered reads off these rows, so a partition "
           "that leaks hands back a fraction of a denominator that is not the number of reads. Every raise of "
           "the total is owed_note's and owed_note raises exactly one row with it, so a mismatch is a row "
           "dropped by the grow path or a total raised by a second writer");
    DCHECK(g_owed_reads + g_removed_reads + g_index_refused + g_appstate_reads == g_global_reads,
           "the four arms of the global miss do not sum to the reads this hook was asked about — the "
           "denominator is raised ONCE at the top of that arm and each arm raises its own counter, so this "
           "fires exactly when a fifth way out of the block was added without classifying its read. Every "
           "number in this census is a fraction of that denominator, so an unclassified arm does not make one "
           "row wrong, it makes the whole object a partition of a population it no longer covers");
    /* Each cut is contained in the population it cuts. The denominator is raised per classified read in
       global_miss_note and a cut per operator entry before it classifies, so a cut exceeds it when an entry
       records without classifying. Asserted per cut, since a summed bound hides one cut running ahead. */
    DCHECKF(g_unresolved_reads <= g_global_reads,
            "the typeof cut of the owed-name census (%ld) is larger than the population it is a cut of (%ld) "
            "— every read this file is told about is classified by global_miss_note, which raises the "
            "denominator, so a larger cut is a caller that recorded a typeof read and never classified it",
            g_unresolved_reads, g_global_reads);
    DCHECKF(g_in_reads <= g_global_reads,
            "the `in` cut of the owed-name census (%ld) is larger than the population it is a cut of (%ld) — "
            "the engine records this arm from do_opkeyed_place and every read it records is classified by "
            "global_miss_note, which raises the denominator, so a larger cut is a recording that skipped the "
            "classification. The likeliest way to build one is a second call site for the same arm: there is "
            "exactly ONE, and it is the placement, because the [[HasProperty]] miss underneath it is the same "
            "request every unresolved identifier issues",
            g_in_reads, g_global_reads);
    DCHECKF(g_has_call_reads <= g_global_reads,
            "the Reflect.has cut of the owed-name census (%ld) is larger than the population it is a cut of "
            "(%ld) — every read this file is told about is classified by global_miss_note, which raises the "
            "denominator, so a larger cut is a recording that skipped the classification. There is exactly "
            "ONE call site for this arm and it is the machine's own REFLPROP_OP stage, gated on GP_HAS, "
            "because that is the only point at which the operation, its target and its answer are in one hand",
            g_has_call_reads, g_global_reads);
    /* The per-name buckets against the cuts. It is `<=` because the slack is a population: operator reads on
       names no standard owns (`typeof __NEXT_DATA__`) raise the cut and no owed row. */
    DCHECKF(rowsbyentry[ABSENT_ENTRY_TYPEOF] <= g_unresolved_reads,
            "the owed-name census's per-name typeof buckets (%ld) outnumber the typeof reads the engine told "
            "this file about (%ld) — the cut is raised once per typeof entry before the classification and "
            "the buckets once per OWED typeof read inside it, so the buckets can only be FEWER (the residue "
            "is typeof reads on names no standard owns). More of them is a row's bucket raised by something "
            "that never came through absent_unresolved_note",
            rowsbyentry[ABSENT_ENTRY_TYPEOF], g_unresolved_reads);
    DCHECKF(rowsbyentry[ABSENT_ENTRY_IN] <= g_in_reads,
            "the owed-name census's per-name `in` buckets (%ld) outnumber the `in` reads the engine told this "
            "file about (%ld) — stated as its own assert rather than as a sum with the typeof pair for that "
            "pair's own reason: a summed statement closes over one bucket running ahead while the other runs "
            "behind, and which operator a document actually writes is the fact this split exists to state",
            rowsbyentry[ABSENT_ENTRY_IN], g_in_reads);
    DCHECKF(rowsbyentry[ABSENT_ENTRY_HAS_CALL] <= g_has_call_reads,
            "the owed-name census's per-name Reflect.has buckets (%ld) outnumber the Reflect.has reads the "
            "engine told this file about (%ld) — stated as its own assert rather than summed with the pair "
            "above for that pair's own reason, and the residue here is the same population: a call-spelled "
            "detect on a name no standard owns, which `Reflect.has(window, \"__NEXT_DATA__\")` is",
            rowsbyentry[ABSENT_ENTRY_HAS_CALL], g_has_call_reads);
    for (v = 0; v < ABSENT_VOCAB_N; v++)
        DCHECKF(rowsby[v] == g_owed_by_vocab[v],
                "the per-standard split of the owed reads disagrees with the rows it is a split OF (standard "
                "%d: %ld by the rows, %ld by the counter) — the counter is raised once per READ in owed_note "
                "and this sum is taken once per ROW here, so the two part company exactly when a row is "
                "dropped by the grow path or a bucket is raised twice. It is stated per standard rather than "
                "as one total because the total closes over a bucket raised for the WRONG standard while the "
                "split does not, and which standard owes the work is the whole content of this pair: an "
                "unbuilt Web IDL interface is a browser component to write and an uninstalled ECMAScript §19 "
                "name is a language intrinsic this build did not link",
                v, rowsby[v], g_owed_by_vocab[v]);

    for (pass = 0; pass < 2; pass++) {
        len = absent_emitf(out, cap, 0, "{\"%s\":%ld", KEY_READS, g_global_reads);
        len = absent_emitf(out, cap, len, ",\"%s\":%ld", KEY_OWED, g_owed_reads);
        len = absent_emitf(out, cap, len, ",\"%s\":%ld", KEY_REMOVED, g_removed_reads);
        len = absent_emitf(out, cap, len, ",\"%s\":%ld", KEY_INDEX, g_index_refused);
        len = absent_emitf(out, cap, len, ",\"%s\":%ld", KEY_APP, g_appstate_reads);
        len = absent_emitf(out, cap, len, ",\"%s\":%ld", KEY_TYPEOF, g_unresolved_reads);
        len = absent_emitf(out, cap, len, ",\"%s\":%ld", KEY_IN, g_in_reads);
        len = absent_emitf(out, cap, len, ",\"%s\":%ld", KEY_HASCALL, g_has_call_reads);
        for (v = 0; v < ABSENT_VOCAB_N; v++)
            len = absent_emitf(out, cap, len, ",\"%s\":%ld", KEY_VOCAB[v], g_owed_by_vocab[v]);
        for (i = 0; i < g_owed_n; i++) {
            const char *k = g_owed[i].name;
#if APICLIENT_DEV
            const char *c;

            /* The key is written raw, so it must hold no byte JSON would need escaped. The `#if` is over the loop
               because a DCHECKF body compiles out and the loop around it would not. */
            for (c = k; *c; c++)
                DCHECKF(*c != '"' && *c != '\\' && (unsigned char)*c >= 0x20,
                        "the owed-name census is writing a row key holding a byte JSON cannot carry raw "
                        "(\"%s\", byte 0x%02x) — this row is written with no escaping pass because its key is "
                        "browser/platform_names.h's, browser/language_names.h's or browser/i18n_names.h's OWN "
                        "entry, keyed by pointer "
                        "so a page's bytes can never reach it. A byte like this means that provenance no "
                        "longer holds, and the document embedding this census would not parse",
                        k, (unsigned char)*c);
            DCHECKF(*k != '_',
                    "the owed-name census is writing a row named \"%s\", which opens on the `_` its MEMBERS "
                    "are told apart by — a consumer sums the rows' buckets to get the reads and reads the "
                    "members as the population they are drawn from, so a row in the member namespace puts a "
                    "denominator inside its own numerator. No name Web IDL exposes on Window and no name "
                    "ECMAScript §19 puts on the global object opens on an underscore, so this is a generated "
                    "table having gained a name neither standard spells", k);
#endif
            /* A row is a histogram with every bucket emitted, zeroes included: extension/bridge.js refuses an
               empty histogram on this census. The total is not emitted beside them; the buckets sum to it. */
            len = absent_emitf(out, cap, len, ",\"%s\":{", k);
            for (e = 0; e < ABSENT_ENTRY_N; e++)
                len = absent_emitf(out, cap, len, "%s\"%s\":%ld", e ? "," : "", KEY_ENTRY[e],
                                   g_owed[i].by_entry[e]);
            len = absent_emitf(out, cap, len, "}");
        }
        len = absent_emitf(out, cap, len, "}");
        if (pass == 0) {
            cap = len + 1;
            out = (char *)reclaim_malloc(cap);
            if (!out) return NULL;
        }
    }
    DCHECK(len + 1 == cap,
           "the owed-name census was written to a different length than it was measured for — the two passes "
           "walk one table with one argument list, so they can only disagree if a row changed between them, "
           "and the byte the second pass lost is the closing brace rather than a digit");
    return out;
}

/* The published records and the paths they were published at, keyed by the record's address (the header says
   why that is exact). Lookup is newest first, so the row found for a recycled address describes the live
   record. */
typedef struct { void *obj; char *path; } NsRow;
static NsRow *g_ns;
static int g_ns_n, g_ns_cap;

static const char *ns_path_of(JSValueConst v)
{
    void *p = JS_VALUE_GET_PTR(v);
    int i;

    for (i = g_ns_n - 1; i >= 0; i--)
        if (g_ns[i].obj == p)
            return g_ns[i].path;
    return NULL;
}

void absent_free(void)
{
    int i;

    concolic_undeclare_sources(ABSENT_COMPONENT);

    for (i = 0; i < g_ns_n; i++)
        free(g_ns[i].path);
    free(g_ns);
    g_ns = NULL;
    g_ns_n = g_ns_cap = 0;
    /* The census goes with the registry, which makes it a per-agent reading. The rows own no allocation, so
       only the vector is freed, and the counters are zeroed beside it so the composer's identities hold for
       the next agent. */
    free(g_owed);
    g_owed = NULL;
    g_owed_n = g_owed_cap = 0;
    g_owed_reads = g_removed_reads = g_index_refused = g_appstate_reads = g_global_reads = 0;
    /* A cut left out of this reset exceeds the zeroed denominator at the next agent's first census, where the
       composer's containment assert fires. */
    g_unresolved_reads = g_in_reads = g_has_call_reads = 0;
    for (i = 0; i < ABSENT_VOCAB_N; i++)
        g_owed_by_vocab[i] = 0;
}

/* Spells a key for both halves of this file; the caller frees it with JS_FreeCString. The channel is a record
   of string- and index-keyed fields, and the engine applies that key rule (JS_AtomIsPublishedName) before it
   asks any hook, on the miss, the hit and the publication walk alike. So a symbol here is asserted and not
   filtered: its description is neither unique nor a name, and a well-known symbol answered with an unknown
   replaces a slot the interpreter is about to call (§7.1.1 ToPrimitive ( input [ , preferredType ] ) step
   1.a's `? GetMethod(input, %Symbol.toPrimitive%)` reads one off every object it coerces). A record reached
   through a symbol key is an internal-slot record (ECMAScript §6.1.7.2 Object Internal Methods and Internal
   Slots, held as fields of an object hung off a private Symbol, engine/host/browser/core/idl_slots.h) and
   must not become a namespace. The key rule and the extent grant do not substitute for each other: one is
   about the key a record is reached by, the other about whose choice its member list was. */
static const char *ns_key_str(JSContext *ctx, JSAtom name)
{
    const char *s;

    DCHECK(JS_AtomIsPublishedName(JS_GetRuntime(ctx), name),
           "the engine asked this channel about a key it cannot NAME — the injected-state channel is a record "
           "of string- and index-keyed fields, so a symbol here is a read or a publication that reached the "
           "hook without going through js_absent_ask / js_present_ask / js_publish_document_namespace's key "
           "rule. A well-known symbol answered with an unknown replaces a slot the interpreter is about to "
           "CALL: §7.1.1 ToPrimitive ( input [ , preferredType ] ) step 1.a reads %Symbol.toPrimitive% off "
           "every object it coerces; a record published under one turns every internal slot behind it into an "
           "unknown, which throws nothing and is read as state");
    s = JS_AtomToCString(ctx, name);
    /* A CHECK: every caller's other return is JS_UNINITIALIZED, which the engine reads as "this read is not on
       the channel", so a failed allocation would answer an injected read concretely and decide its gate.
       Allocation is the only failure here: the DCHECK above makes the atom a tagged integer or a string atom,
       and neither takes JS_AtomToString's exception path for another reason. */
    CHECK(s != NULL, "absent: OOM spelling a key of the document's injected-state namespace — the alternative "
                     "to failing here is answering a server-injected read concretely, which decides its gate "
                     "for the whole program and buries the surface behind it with nothing to say so");
    return s;
}

/* How a key extends the path it is read at: the one join both halves use, so one unknown has one name. The
   path is the expression the run evaluated. An index is joined as `base[0]` and a name as `base.name`; at the
   root a name is bare (`__FLAGS`) and an index is `window[0]`. The atom decides which, never the key's
   characters: ECMAScript §6.1.7 The Object Type makes an array index a canonical numeric string, so `x["01"]`
   is a name. The caller frees the result. */
static char *ns_join(const char *base, JSAtom name, const char *key)
{
    static const char *const root = "window";
    size_t len = (base ? strlen(base) : strlen(root)) + strlen(key) + 3;
    char *out = (char *)reclaim_malloc(len);

    CHECK(out != NULL, "absent: OOM composing a path in the document's injected-state namespace — the "
                       "alternative to failing here is a member reported under a name no document published, "
                       "or an injected read answered concretely, which decides its gate for the whole program");
    if (JS_AtomIsIndexName(name))
        snprintf(out, len, "%s[%s]", base ? base : root, key);
    else if (base)
        snprintf(out, len, "%s.%s", base, key);
    else
        snprintf(out, len, "%s", key);
    return out;
}

void absent_publish_hook(JSContext *ctx, JSValueConst parent, JSAtom name, JSValueConst value)
{
    int is_root = JS_IsGlobalReceiver(ctx, parent);   /* the global object or the WindowProxy over it */
    const char *base = is_root ? NULL : ns_path_of(parent);
    const char *n = ns_key_str(ctx, name);
    char *path;

    /* js_publish_document_namespace publishes a record before it descends into it, so the parent is the global
       or a filed row. */
    DCHECK(is_root || base != NULL,
           "a record was published under a parent this file has never filed — the engine's walk publishes a "
           "parent before descending into it, so a missing parent path means the two disagree about what the "
           "published graph is, and every member read off this record would be reported under a name the "
           "document never published it at");
    path = ns_join(base, name, n);
    JS_FreeCString(ctx, n);

    if (g_ns_n == g_ns_cap) {
        int cap = g_ns_cap ? g_ns_cap * 2 : 8;
        NsRow *rows = (NsRow *)realloc(g_ns, sizeof(*rows) * (size_t)cap);
        CHECK(rows != NULL, "absent: OOM growing the published-namespace registry");
        g_ns = rows;
        g_ns_cap = cap;
    }
    g_ns[g_ns_n].obj = JS_VALUE_GET_PTR(value);
    g_ns[g_ns_n].path = path;
    g_ns_n++;
}

/* A member of injected state is not an attacker source: only the application's own write path changes it, so a
   finding rooted there is labelled with that precondition (concolic.h's SRC_PRE_APPLICATION_STATE). */
static void absent_declare_precondition(const char *src)
{
    if (!concolic_source_precondition_declared_by(ABSENT_COMPONENT, src))
        concolic_declare_source_precondition(ABSENT_COMPONENT, src, SRC_PRE_APPLICATION_STATE);
}

/* Spells an injected member's provenance for both halves: `*src` is the whole path as ns_join composes it and
   `*shape` is `{path}`. `base` is the record's published path, or NULL for a member of the global namespace.
   The path is allocated whole and never truncated, because a truncated provenance names every unknown that
   shares its prefix. The caller frees both. */
static void ns_member_spell(const char *base, JSAtom name, const char *key, char **shape, char **src)
{
    char *path = ns_join(base, name, key);
    size_t n = strlen(path) + 3;

    *shape = (char *)malloc(n);
    CHECK(*shape != NULL, "absent: OOM spelling the provenance of an injected member");
    snprintf(*shape, n, "{%s}", path);
    *src = path;
}

/* A member the published record holds. It is the same unknown as a member the record lacks, with the held
   value as its example, so the gate over it still forks and the report keeps the bytes the server sent. The
   mint goes through concolic_new like every source read, so an @S candidate substitutes at `__FLAGS.admin`.
   JSConcolicHooks.present's contract says which base the engine asks about.
   Membership is js_publish_document_member's: any container an inline script built and left reachable from
   the global, which includes a record an inline script keeps as its own working state, so a comparison over a
   value this run computed forks. Whether an inline reader should be off the channel is open. Two inline
   scripts of one document, one writing `window.__FLAGS={admin:false}` and the next reading it, is an ordinary
   server-rendered shape that must stay on it. flow_dyn_kind cannot decide it either: it describes the
   top-level program, and a bundle function called from an inline program would read as inline. The reader's
   JS_EVAL_FLAG_INLINE_SCRIPT is the discriminator, and only the engine holds it. */
JSValue absent_present_hook(JSContext *ctx, JSValueConst holder, JSAtom name, JSValueConst value)
{
    const char *s = ns_key_str(ctx, name);
    const char *base;
    char *shape = NULL, *src = NULL;
    JSValue r = JS_UNINITIALIZED;

    DCHECK(JS_VALUE_GET_TAG(value) != JS_TAG_OBJECT,
           "the engine asked about an OBJECT-valued member of a published record. A record hanging off a "
           "published record is published in its OWN right by the same walk, and its address is this file's "
           "registry key — minting a fresh unknown per read would answer `gon.user === gon.user` false and "
           "hide the key behind a value nothing filed");
    base = ns_path_of(holder);
    /* The engine marks a record published only in the walk that files its row, so the path exists. */
    DCHECK(base != NULL,
           "a read hit a member of a record the engine says the document published, and this file holds no "
           "path for it — without the path this member would be reported as a bare field name that any other "
           "namespace's identically-named field is indistinguishable from");
    if (!base)
        goto done;
    ns_member_spell(base, name, s, &shape, &src);
    /* A MEMBER THE PAGE'S OWN PROGRAM WROTE (quickjs's JS_RecordMemberWriteSeq: a [[Set]], not the literal).
       Three facts decide it. A write in this flow's current unit is proof by run-to-completion: nothing can run
       between it and this read, so the member reads as what the program wrote and no arm is minted. A write in
       an earlier unit is not: a script the engine never observed or another window holding the record can
       change it at a task boundary, so the arm stays and its source names that assumption. With no program
       write the member is the served value, minted below. */
    {
        uint64_t wseq, now = flow_running_unit_seq();

        if (JS_RecordMemberWriteSeq(ctx, holder, name, &wseq)) {
            char *path = src, *label;
            size_t n;

            DCHECK(wseq <= now, "a published member's program write is latched in a unit after the read's");
            if (wseq == now)
                goto done;
            n = strlen(path) + 128;
            label = (char *)malloc(n);
            CHECK(label != NULL, "absent: OOM naming a written member's assumption");
            /* The units stay out of the name: a source's name is its identity, and one member read after one
               write must be one source however many units later it is read. */
            snprintf(label, n, "%s as the page wrote it in an earlier task, assumed changed by unobserved code",
                     path);
            free(path);
            src = label;
            free(shape);
            n = strlen(src) + 3;
            shape = (char *)malloc(n);
            CHECK(shape != NULL, "absent: OOM spelling a written member's provenance");
            snprintf(shape, n, "{%s}", src);
        }
    }
    /* Served: the record holds the member and `value` is its bytes, written by an inline script of the
       document this engine was served. It is minted unknown anyway so that a gate over it forks; an address
       composed from it reads `unknown-unproven`, which is what separates this arm from the miss arm.
       Named residual: not covered — a record published by a script the run fetched, and not by the served
       document, mints here too and is called served. Next diff — the published-namespace registry records
       which script wrote the record, so the word is read off the row `ns_path_of` finds. Absence shows as —
       an emitted row whose address rests on a member of a record no element of the served markup spells,
       reading as a class that says a parse of the markup could have stated it. */
    absent_declare_precondition(src);
    r = concolic_new(ctx, shape, src, CONCOLIC_WHOSE_SERVED, JS_DupValue(ctx, value));
    /* The record holds the member and its value is `undefined`: those are its bytes, not an absence. */
    if (JS_IsUndefined(value) && concolic_is(r)) concolic_hold_undefined(r);
done:
    free(shape);
    free(src);
    JS_FreeCString(ctx, s);
    return r;
}

/* The same miss on the global, reached by an operation that performed no [[Get]]; `op` is the engine stating
   which. It records only. The return is void because none of these algorithms contains a read for a host to
   answer (the census banner cites each), so a value handed back would answer a question the standard does not
   ask. The classification and the row are global_miss_note's, shared with the read hook, because
   `window.EventSource`, `typeof EventSource` and `"EventSource" in window` are one question about one name.
   The key is nameable on every arm: the `typeof` atom is an identifier's, and the engine filters a symbol key
   before the other arms call here, which is why ns_key_str may assert. */
void absent_unresolved_note(JSContext *ctx, JSAtom name, JSConcolicAbsentOp op)
{
    const char *s = ns_key_str(ctx, name);

    /* A cut is raised here, before the classification, so the composer's containment asserts compare counts
       raised at two different events. The switch has no `default`, so a member added to JSConcolicAbsentOp
       is a compile diagnostic; `entry` opens on the poison, so an unmapped member aborts at the
       classification. */
    AbsentEntry entry = ABSENT_ENTRY_N;

    switch (op) {
    case JS_CONCOLIC_ABSENT_TYPEOF: g_unresolved_reads++; entry = ABSENT_ENTRY_TYPEOF; break;
    case JS_CONCOLIC_ABSENT_IN:     g_in_reads++;         entry = ABSENT_ENTRY_IN;     break;
    case JS_CONCOLIC_ABSENT_HAS_CALL:
        g_has_call_reads++; entry = ABSENT_ENTRY_HAS_CALL; break;
    }
    /* The operator already answered; only the counters and the row are wanted. */
    (void)global_miss_note(s, name, entry);
    JS_FreeCString(ctx, s);
}

JSValue absent_read_hook(JSContext *ctx, JSValueConst obj, JSAtom name, JSConcolicAbsentAnswer answer)
{
    int is_global = JS_IsGlobalReceiver(ctx, obj);   /* `window.X`, `self.X` and `this.X` address the global */
    const char *s = ns_key_str(ctx, name);
    const char *base = NULL;
    JSValue r = JS_UNINITIALIZED;
    char *shape = NULL, *src = NULL;

    if (is_global) {
        /* A nonzero answer leaves the read alone: JS_UNINITIALIZED tells the engine the read is not on the
           channel, and it completes §10.1.8.1 OrdinaryGet ( O, P, Receiver ) step 2.b's `undefined`. Zero is
           the app-state arm, minted below. */
        if (global_miss_note(s, name, ABSENT_ENTRY_READ))
            goto done;
    } else {
        base = ns_path_of(obj);
        /* The engine asks only about a record it has marked published, so the path exists. */
        DCHECK(base != NULL,
               "a read missed on a record the engine says the document published, and this file holds no path "
               "for it — the mark is set only by the walk that files the row, so the two cannot come apart "
               "unless a row was dropped; without the path this member would be reported as a bare field name "
               "that any other namespace's identically-named field is indistinguishable from");
        if (!base)
            goto done;
    }
    /* The provenance is spelled by the same speller as the present half's — see ns_member_spell. */
    ns_member_spell(base, name, s, &shape, &src);
    /* The world's: this is the miss arm, so the served document states nothing about the member, and no byte
       this run received holds what a server would inject. An address composed from it reads `unknown`. */
    absent_declare_precondition(src);
    r = concolic_new(ctx, shape, src, CONCOLIC_WHOSE_WORLD, JS_UNDEFINED);
    /* The example is the absence, so an arm against it takes the member as present (decide.c's assumption). */
    if (concolic_is(r)) concolic_source_precondition_absent(ABSENT_COMPONENT, src);
    /* What this session itself reads here. A property read that misses answers undefined (§10.1.8.1
       OrdinaryGet step 2.b), which the unknown holds as its example, so every derivation computes the session's
       value from it and decide.c marks the arm the session does not take forced. An identifier no binding
       resolves throws (§6.2.5.5 GetValue step 2), so the session stops at this read and the flow reading on
       stands on a world this engine chose from here. A pin answers bytes, not a record, and holds nothing. */
    switch (answer) {
    case JS_CONCOLIC_ABSENT_ANSWERS_UNDEFINED:
        if (concolic_is(r)) concolic_hold_undefined(r);
        break;
    case JS_CONCOLIC_ABSENT_ANSWERS_THROW:
        flow_mark_forced_arm();
        flow_note_assumed_read(src);   /* reading past the throw takes the absent name as present */
        break;
    }
done:
    free(shape);
    free(src);
    JS_FreeCString(ctx, s);
    return r;
}
