/* WHICH ABSENT READ IS INPUT, AND WHICH IS A COMPONENT THIS ENGINE OWES.
 *
 * Both wear the same shape — a name that resolves nowhere — and answering them the same way loses either the
 * whole logged-in surface or every forcing function.
 *
 * SERVER-INJECTED APP STATE is unknown INPUT. `window.__FLAGS`, `__USER`, `gon` are written into the document
 * by the server for a logged-in visitor and simply absent for this one, so what they hold is not `undefined` —
 * it is UNKNOWN. Answering `undefined` makes `__FLAGS.admin` throw on the first field access and buries every
 * endpoint behind it, which is precisely the surface this tool exists to reach: the bundle ships the auth and
 * admin code to a logged-out visitor and it never runs. Symbolic instead, so the gate FORKS and the logged-in
 * arm is explored.
 *
 * A WEB API THIS ENGINE HAS NOT BUILT is honestly absent. Its ReferenceError is the forcing function that names
 * the component to write, and handing back a symbol instead would let a flow run past a missing capability and
 * report a surface it never reached.
 *
 * THE DISTINCTION IS WEB IDL'S TO MAKE, NOT A LIST'S. A global name belongs to the platform exactly when the
 * IDL exposes it on Window, and browser/platform_names.h is that set, generated from @webref/idl by
 * engine/idlgen.mjs. It replaced a 22-name list typed into main.c — and the difference is not cosmetic: every
 * interface off that list (Node, Element, Event, DOMException, HTMLElement, and ~1300 more) was mistaken for
 * app state, so a branch on one FORKED instead of throwing. A page touching eight of them multiplied the
 * frontier by 256; a WPT document exhausted 2.8 GB in forty seconds doing it. A hand-maintained allowlist
 * cannot be right about a surface of this size, and the moment it is wrong the error is silent.
 *
 * AND IT IS ASKED OF A PRESENT MEMBER AS WELL AS OF A MISSING ONE, which is the half that decides the case
 * §solver names by name. A server that ships `window.__FLAGS={admin:false}` has WRITTEN the field, so the read
 * hook below never sees it — the engine asks that one only where the prototype chain ran out. Answering
 * `false` from the slot then decides `if (__FLAGS.admin)` for the whole program and buries the admin surface,
 * which is the identical loss this file exists to prevent, reached through a slot instead of a hole. The
 * extent of that record was the SERVER'S choice against this visitor's credentials, so what it holds is a
 * per-session fact and not a program constant: unknown for control flow, and — unlike a missing member —
 * carrying the bytes the server actually sent as its EXAMPLE. §solver: "a loaded `features.admin:false` must
 * NOT concretize the gate, or the admin endpoint is lost — config is opaque-for-control-flow yet carries its
 * loaded value as the example."
 * The two halves share ONE path composition and ONE registry, which is why they are one file: a member's
 * provenance is `gon.current_user_id` whether or not the record holds it, and two spellers would be two names
 * for one unknown the moment either drifted.
 *
 * AND THE QUESTION IS ASKED OF A PRESENT PARENT AS OFTEN AS OF A MISSING GLOBAL, which is the half this file
 * did not have. A server does not only decline to write `window.__FLAGS`; far more often it writes
 * `window.gon={}` and then the two of the twenty-three fields the bundle reads that THIS visitor is entitled
 * to. Every one of the other twenty-one missed on a present object and answered `undefined`, so
 * `if (!gon.current_user_id) return null` never forked and the logged-in surface stayed buried behind a rule
 * that was written for it. The engine decides WHICH records those are and what this file owes is the PATH each
 * one is read by, because a member's identity is `gon.current_user_id` and never a bare `current_user_id` that
 * a second namespace's identically-named field would be indistinguishable from.
 *
 * AND THE ENGINE'S RULE IS WORTH STATING HERE, BECAUSE IT IS THE ONE THING THIS FILE MAY NOT SECOND-GUESS AND
 * THE ONE THING THAT HAS BEEN WRONG TWICE. A container is on this channel iff THE DOCUMENT'S OWN BYTES WROTE
 * ITS EXTENT — an object or array literal, or a `JSON.parse` in an inline `<script>`, granted at the operation
 * that writes the member list (js_document_container_grant), never inferred from how or when the object was
 * allocated. A LIST is such a container for the purpose of being DESCENDED THROUGH and is never a record whose
 * miss is the server's silence (§10.4.2 Array Exotic Objects: the list states its own length), so this file
 * sees a list only as a path component — `__STATE__.users[0]` — and never as a base it is asked about. The
 * two halves of that are what keep the channel honest in the two directions it can fail. A record wrongly ON
 * it answers a member that HAS a real answer with an unknown, and nothing throws: a PLATFORM OBJECT is the
 * case, because Web IDL §3.8 "Platform objects implementing interfaces" makes its member list the INTERFACE'S
 * and not the document's, so an `Event` an inline script leaves in a `var` must answer `undefined` for a
 * member it does not have — and a member of an interface this engine has not BUILT yet must be honestly
 * absent, since that absence is the forcing function naming the component to write. A record wrongly OFF it
 * loses a fork, which is loud and recoverable. So the engine grants narrowly and this file never widens: a
 * base it is asked about is one the engine has already decided, and every DCHECK below says so.
 *
 * THE REGISTRY HOLDS NO REFERENCE, and that is sound rather than lucky: a row is only ever consulted for an
 * object the ENGINE has marked as published, the mark is cleared at every allocation, and the only thing that
 * sets it is the publication that files the row. So a recycled address cannot answer with a dead document's
 * path — it has no mark until something publishes it, and publishing appends the row that describes it. */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "solver/absent.h"
#include "solver/concolic.h"
#include "browser/platform_names.h"

#define PLATFORM_NAMES_N ((int)(sizeof(PLATFORM_NAMES) / sizeof(PLATFORM_NAMES[0])))

/* The generated table is SORTED (idlgen sorts it), so membership is a binary search — a linear scan of 1300
   names would run on every unresolved global read, of which a forced-exec run does a great many. The sort is
   the generator's invariant and is asserted here, at the one place that depends on it. */
int absent_is_platform_name(const char *name)
{
    int lo = 0, hi = PLATFORM_NAMES_N - 1;

    DCHECK(name != NULL, "absent_is_platform_name: no name");
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        int c = strcmp(name, PLATFORM_NAMES[mid]);
        if (c == 0)
            return 1;
        if (c < 0)
            hi = mid - 1;
        else
            lo = mid + 1;
    }
    return 0;
}

/* THE PUBLISHED RECORDS AND THE PATHS THEY WERE PUBLISHED AT. Keyed by the record's ADDRESS — see the header
   comment for why that is exact and not a heuristic. Newest first on lookup, so that if an address is ever
   recycled between two published records the row that describes the live one is the one found. */
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

    for (i = 0; i < g_ns_n; i++)
        free(g_ns[i].path);
    free(g_ns);
    g_ns = NULL;
    g_ns_n = g_ns_cap = 0;
}

/* THE KEY, READ ONCE FOR EVERY HALF OF THIS FILE, AND THE ENGINE'S GATE ASSERTED WHERE THE PATH IS COMPOSED
   FROM IT. The channel is a server writing a RECORD OF FIELDS, so its keys are strings and array indices —
   and the engine gates on exactly that (JS_AtomIsPublishedName) before it asks any hook. This is the other
   side of that gate: a SYMBOL reaching here would be spelled into a provenance out of its DESCRIPTION, which
   is neither unique nor a name (`Symbol()` twice spells one path for two keys), and a WELL-KNOWN one is the
   engine's own protocol — a slot the interpreter is about to CALL.
   IT IS ASSERTED RATHER THAN FILTERED BECAUSE THE FILTER ALREADY EXISTS AND HAS BEEN GONE AROUND TWICE, and
   the two are worth keeping side by side because they are the same omission at two different altitudes.
   FIRST: the gate arrived with the HIT arm and the MISS arm asked nothing, so for the life of that asymmetry
   §7.1.1 ToPrimitive ( input [ , preferredType ] ) step 1.a's `? GetMethod(input, %Symbol.toPrimitive%)` — a
   read that misses on EVERY object — was answered here with a callable unknown, and step 1.b.vi's "Throw a
   TypeError exception" ended the document (`var b={}; 1 & b` and `1 & globalThis` both died).
   SECOND, AND WORSE, because it produced no exception at all: the WALK that decides which records are on the
   channel asked no key rule either, and a record reached through a key is a record PUBLISHED, not a member
   answered. So an internal-slot record — ECMAScript §6.1.7.2 Object Internal Methods and Internal Slots, which
   this engine holds as named fields on an object hung off a private Symbol
   (engine/host/browser/core/idl_slots.h) — became a namespace, and every one of its well-named fields then
   answered with an unknown through a key rule that passed. An `Event` an inline script left in a `var`
   reported `defaultPrevented` true and `dispatch` set, so DOM §2.7 Interface EventTarget's dispatchEvent(event)
   method step 1 threw InvalidStateError on the first dispatch of a freshly constructed event. THAT is why this
   assert covers the publication and not only the two reads: the publication is where the path is composed, so
   it is where a key that cannot be spelled must crash.
   AND THE KEY RULE WAS NEVER THE WHOLE OF THAT SECOND DEFECT, WHICH IS WHY MEMBERSHIP MOVED TOO. It stopped
   the SLOT RECORD being published; the `Event` itself is reachable under an ordinary string key, so it stayed
   on the channel and a read of a member it does not hold still minted an unknown for a value Web IDL §3.8
   defines. The rule this file's header states — extent GRANTED at the operation that wrote it — is what takes
   a platform object off the channel, and neither rule substitutes for the other: one is about the KEY a record
   is reached by, the other about WHOSE CHOICE its member list was. */
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
    /* ONE ALLOCATION FAILURE, ONE ANSWER, AND IT IS THE FATAL ONE — because what the other answer produces is
       not a degraded report but a FABRICATED one, and a fabricated answer to exactly these two reads is what
       this whole file exists to prevent. Every caller's only other return is JS_UNINITIALIZED, which the
       engine reads as the positive statement "this read is not on the channel" (js_absent_ask /
       js_present_ask): the MISS arm then completes §10.1.8.1 OrdinaryGet ( obj, propertyKey, receiver ) step
       2.b's `undefined`, and the HIT arm hands back the slot's own bytes. So a failed malloc answers
       `if (__FLAGS.admin)` concretely, the gate stops forking, and the logged-in surface this tool is for is
       buried — silently, with a smaller result set reported as a clean one, which is the defaulted-field
       defect reached through the allocator instead of through a `||`. The publication half of this same file
       has always spelled that failure `CHECK` (CLAUDE.md §Offensive programming names OOM as CHECK's own
       example: "a dropped flow corrupts the frontier"), and one function cannot hold two answers to one
       failure.
       AND ALLOCATION IS THE ONLY FAILURE IT HAS, which is what makes this a CHECK rather than a judgement
       call: the DCHECK above establishes the atom is a published name, and JS_AtomIsPublishedName defines
       that as a tagged integer or a JS_ATOM_TYPE_STRING atom — neither of which can drive JS_AtomToString
       down its exception path for any reason but a failed allocation. */
    CHECK(s != NULL, "absent: OOM spelling a key of the document's injected-state namespace — the alternative "
                     "to failing here is answering a server-injected read concretely, which decides its gate "
                     "for the whole program and buries the surface behind it with nothing to say so");
    return s;
}

/* HOW A KEY EXTENDS THE PATH IT IS READ AT — the ONE join, because a path composed two ways is two names for
   one unknown the moment either spelling drifts, and every predicate over either would then decide only half
   of them. Both halves of this file compose a path (the publication files one, the two read hooks spell a
   member's provenance out of one) and both went through `%s.%s`.
   AN INDEX IS NOT JOINED WITH A DOT, and that is not cosmetic. A provenance is the EXPRESSION THE RUN BUILT
   (CLAUDE.md §@H), which is what makes it a thing a person can paste and a thing the next candidate can be
   composed onto: `__STATE__.users[0].role` is that expression and `__STATE__.users.0.role` is a syntax error
   wearing a path. It used to be nearly unreachable — an index key arrived only where a server literally wrote
   `{"0":…}` — and it is now the ordinary case, because the engine's walk descends through the LISTS a state
   tree is made of and reaches every element by its index.
   THE ROOT SPELLS `window[0]` AND A BARE NAME. A member of the global namespace is read as `__FLAGS`, which is
   the expression, and `window` is what the same read needs the moment the key is an index: `0` alone names
   nothing, and the run that produced it evaluated `window[0]`.
   The atom decides, never the spelling: ECMAScript §6.1.7 The Object Type makes an array index a canonical
   numeric string, so `x["01"]` is a NAME and a digit test over the key's characters would join it as `x[01]` —
   one path for two different keys. JS_AtomIsIndexName is the engine's own already-made decision.
   The result is the caller's to free. */
static char *ns_join(const char *base, JSAtom name, const char *key)
{
    static const char *const root = "window";
    size_t len = (base ? strlen(base) : strlen(root)) + strlen(key) + 3;
    char *out = (char *)malloc(len);

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
    JSValue g = JS_GetGlobalObject(ctx);
    int is_root = (JS_VALUE_GET_PTR(parent) == JS_VALUE_GET_PTR(g));
    const char *base = is_root ? NULL : ns_path_of(parent);
    const char *n = ns_key_str(ctx, name);
    char *path;

    JS_FreeValue(ctx, g);
    /* A CHILD ARRIVING BEFORE ITS PARENT IS THE ENGINE AND THIS FILE DISAGREEING, not a case to default past.
       js_publish_document_namespace walks OUT from the global object and publishes a record before it descends
       into it, so a parent that is neither the global nor a filed row means the walk reached this record by
       some other route than the one this path is composed for — and the composed name would then describe a
       place in the document's namespace that nothing was published at. */
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

/* THE ONE SPELLING of an injected member's provenance, used by both halves of this file.
   The PROVENANCE is the whole read as the run composed it — `gon` and `gon.current_user_id` are two different
   unknowns and each must decide only its own predicates, so the path is composed WHOLE rather than into a
   fixed buffer: a truncated provenance is not a shorter name for one unknown, it is one name for every unknown
   that shares a prefix, and every predicate over any of them would then decide all of them. A server's state
   tree is as deep and as verbosely named as the server chose.
   `base` is the record's published path, or NULL for a member of the global namespace itself; the join is
   ns_join's, which is the same one the publication files a path with. Both outputs are the caller's to
   free. */
static void ns_member_spell(const char *base, JSAtom name, const char *key, char **shape, char **src)
{
    char *path = ns_join(base, name, key);
    size_t n = strlen(path) + 3;

    *shape = (char *)malloc(n);
    CHECK(*shape != NULL, "absent: OOM spelling the provenance of an injected member");
    snprintf(*shape, n, "{%s}", path);
    *src = path;
}

/* A MEMBER THE PUBLISHED RECORD HOLDS — see this file's header for why that is the same unknown as one it does
   not, and the header of JSConcolicHooks.present for which base the engine asks and why it is not the read
   hook's. The value the slot holds becomes the EXAMPLE, so the flow keeps forking on the gate over it AND the
   report keeps the bytes the server sent; the mint goes through concolic_new like every other source read, so
   an @S candidate substitutes at `__FLAGS.admin` exactly as it does at a member nothing wrote. */
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
    /* THE ENGINE HAS ALREADY DECIDED THIS RECORD IS PUBLISHED — the mark is set only by the walk that files
       the row — so a record with no path is the mark and the registry disagreeing, exactly as it is on the
       miss path, and the alternative to crashing is a member reported under a name no document published. */
    DCHECK(base != NULL,
           "a read hit a member of a record the engine says the document published, and this file holds no "
           "path for it — without the path this member would be reported as a bare field name that any other "
           "namespace's identically-named field is indistinguishable from");
    if (!base)
        goto done;
    ns_member_spell(base, name, s, &shape, &src);
    r = concolic_new(ctx, shape, src, JS_DupValue(ctx, value));
done:
    free(shape);
    free(src);
    JS_FreeCString(ctx, s);
    return r;
}

JSValue absent_read_hook(JSContext *ctx, JSValueConst obj, JSAtom name)
{
    JSValue g = JS_GetGlobalObject(ctx);
    int is_global = (JS_VALUE_GET_PTR(obj) == JS_VALUE_GET_PTR(g));
    const char *s = ns_key_str(ctx, name);
    const char *base = NULL;
    JSValue r = JS_UNINITIALIZED;
    char *shape = NULL, *src = NULL;

    JS_FreeValue(ctx, g);
    if (is_global) {
        /* A name the platform owns is a component this engine owes; leave the read alone so its throw names
           it. Asked ONLY of the global, because the platform's names live there: `gon.Node` is a field of an
           app record that happens to be spelled like an interface, and suppressing it would answer a real
           unknown with `undefined`. */
        if (absent_is_platform_name(s))
            goto done;
    } else {
        base = ns_path_of(obj);
        /* THE ENGINE HAS ALREADY DECIDED THIS RECORD IS PUBLISHED — it does not ask otherwise — so a record
           with no filed path is the mark and the registry disagreeing, and the alternative to crashing is a
           member reported under a name no document published. */
        DCHECK(base != NULL,
               "a read missed on a record the engine says the document published, and this file holds no path "
               "for it — the mark is set only by the walk that files the row, so the two cannot come apart "
               "unless a row was dropped; without the path this member would be reported as a bare field name "
               "that any other namespace's identically-named field is indistinguishable from");
        if (!base)
            goto done;
    }
    /* Example-free, and that is the ONE way this half differs from the present half: nothing here knows what a
       logged-in visitor's flags WOULD hold, and inventing one fabricates an observation. The provenance is
       spelled by the same speller either way — see ns_member_spell. */
    ns_member_spell(base, name, s, &shape, &src);
    r = concolic_new(ctx, shape, src, JS_UNDEFINED);
done:
    free(shape);
    free(src);
    JS_FreeCString(ctx, s);
    return r;
}
