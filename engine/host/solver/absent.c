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
 * that was written for it. The engine decides WHICH records those are (a document-built record reachable from
 * the global — see js_publish_document_namespace); what this file owes is the PATH each one is read by,
 * because a member's identity is `gon.current_user_id` and never a bare `current_user_id` that a second
 * namespace's identically-named field would be indistinguishable from.
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

void absent_publish_hook(JSContext *ctx, JSValueConst parent, JSAtom name, JSValueConst value)
{
    JSValue g = JS_GetGlobalObject(ctx);
    int is_root = (JS_VALUE_GET_PTR(parent) == JS_VALUE_GET_PTR(g));
    const char *base = is_root ? NULL : ns_path_of(parent);
    const char *n = JS_AtomToCString(ctx, name);
    char *path;
    size_t len;

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
    CHECK(n != NULL, "absent: OOM spelling the name a document published a record under");
    len = (base ? strlen(base) + 1 : 0) + strlen(n) + 1;
    path = (char *)malloc(len);
    CHECK(path != NULL, "absent: OOM composing the path a document published a record at");
    if (base)
        snprintf(path, len, "%s.%s", base, n);
    else
        snprintf(path, len, "%s", n);
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

/* THE KEY, READ ONCE FOR BOTH HALVES, AND THE ENGINE'S GATE ASSERTED WHERE THE PATH IS COMPOSED FROM IT.
   The channel is a server writing a RECORD OF FIELDS, so its keys are strings and array indices — and the
   engine gates on exactly that (JS_AtomIsPublishedName) before it asks either hook. This is the other side of
   that gate: a SYMBOL reaching here would be spelled into a provenance out of its DESCRIPTION, which is
   neither unique nor a name (`Symbol()` twice spells one path for two keys), and a WELL-KNOWN one is the
   engine's own protocol — a slot the interpreter is about to CALL.
   IT IS ASSERTED RATHER THAN FILTERED BECAUSE THE FILTER ALREADY EXISTS AND WAS ALREADY GONE AROUND. The gate
   arrived with the HIT arm and the MISS arm asked nothing, so for the life of that asymmetry §7.1.1
   ToPrimitive ( input [ , preferredType ] ) step 1.a's `? GetMethod(input, %Symbol.toPrimitive%)` — a read
   that misses on EVERY object — was answered here with a callable unknown, and step 1.b.vi's "Throw a
   TypeError exception" ended the document. Measured: `var b={}; 1 & b` and `1 & globalThis` both died. A
   second filter here would have hidden that instead of ending it; an assert makes the next route that skips
   the gate crash at the first read rather than at a coercion a thousand statements later. */
static const char *ns_key_str(JSContext *ctx, JSAtom name)
{
    DCHECK(JS_AtomIsPublishedName(JS_GetRuntime(ctx), name),
           "the engine asked this channel about a key it cannot NAME — the injected-state channel is a record "
           "of string- and index-keyed fields, so a symbol here is a read that reached the hook without going "
           "through js_absent_ask / js_present_ask's key rule. A well-known symbol answered with an unknown "
           "replaces a slot the interpreter is about to CALL: §7.1.1 ToPrimitive ( input [ , preferredType ] ) "
           "step 1.a reads %Symbol.toPrimitive% off every object it coerces");
    return JS_AtomToCString(ctx, name);
}

/* THE ONE SPELLING of an injected member's provenance, used by both halves of this file.
   The PROVENANCE is the whole read as the run composed it — `gon` and `gon.current_user_id` are two different
   unknowns and each must decide only its own predicates, so the path is composed WHOLE rather than into a
   fixed buffer: a truncated provenance is not a shorter name for one unknown, it is one name for every unknown
   that shares a prefix, and every predicate over any of them would then decide all of them. A server's state
   tree is as deep and as verbosely named as the server chose.
   `base` is the record's published path, or NULL for a member of the global namespace itself. Both outputs are
   the caller's to free. */
static void ns_member_spell(const char *base, const char *name, char **shape, char **src)
{
    size_t n = (base ? strlen(base) + 1 : 0) + strlen(name) + 3;

    *shape = (char *)malloc(n);
    *src   = (char *)malloc(n);
    CHECK(*shape != NULL && *src != NULL, "absent: OOM spelling the provenance of an injected member");
    if (base) {
        snprintf(*shape, n, "{%s.%s}", base, name);
        snprintf(*src,   n, "%s.%s", base, name);
    } else {
        snprintf(*shape, n, "{%s}", name);
        snprintf(*src,   n, "%s", name);
    }
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

    if (!s)
        return JS_UNINITIALIZED;
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
    ns_member_spell(base, s, &shape, &src);
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
    if (!s)
        return JS_UNINITIALIZED;
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
    ns_member_spell(base, s, &shape, &src);
    r = concolic_new(ctx, shape, src, JS_UNDEFINED);
done:
    free(shape);
    free(src);
    JS_FreeCString(ctx, s);
    return r;
}
