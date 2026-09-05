/* See realm.h. */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "core/idl_args.h"   /* §3.3.7 [Exposed] step 1: resolving a [Global] interface's global names */
#include "core/realm.h"
#include "solver/concolic.h"
#if APICLIENT_DEV
/* THE GENERATED CORPUS TABLE ITSELF, read for the check below and not restated. This is the second
   translation unit to include it (core/idl_args.c is the first), and that is the point rather than a
   duplication: an auditor of a rule derives the rule from the artifact the rule is about, so the set of
   identifiers §3.8 defines on a global is asked of the same generated rows the install path asks. A
   hand-kept list here would be a third copy of a fact whose second copy is what this check exists to catch.
   Under APICLIENT_DEV because the walk below is, and an unused static table at DEV=0 is a diagnostic. */
#include "idl_exposure.h"
#endif

static RealmIntrinsic *g_list;
static int g_n, g_cap;

void realm_declare_intrinsic(RealmIntrinsic install)
{
    int i;

    DCHECK(install != NULL, "a component declared a per-realm intrinsic with no install to run");
    for (i = 0; i < g_n; i++)
        DCHECK(g_list[i] != install,
               "a per-realm intrinsic was declared twice — it would then build its prototype twice in every "
               "realm, and the second build leaves everything already chained to the first answering out of a "
               "discarded object");
    if (g_n == g_cap) {
        int cap = g_cap ? g_cap * 2 : 8;
        RealmIntrinsic *g = realloc(g_list, (size_t)cap * sizeof *g);
        CHECK(g != NULL, "realm: OOM recording a per-realm intrinsic — a realm missing one answers a member "
                         "out of another document");
        g_list = g;
        g_cap = cap;
    }
    g_list[g_n++] = install;
}

/* HTML §8.1.3.1's TOP-LEVEL CREATION URL, in the per-realm store this file already owns. Zero is the "not
   declared" value because it is also the invalid slot realm_value_set asserts against, so there is one
   sentinel rather than two. The DECLARATION is per AGENT (a slot is a class id, which belongs to a runtime),
   so it happens on the first realm and is released with the agent below. */
static int g_top_level_url_slot;

/* WEB IDL §3.3.8 [Global]'s GLOBAL NAMES OF THIS REALM, in the same per-realm store and for the same reason as
   the URL above: it is a fact about the ENVIRONMENT, it is settled when the realm is built, and §3.3.7 step 1
   is asked of it while the intrinsics are still installing. Resolved to the generated bit set at the call
   rather than at each ask, so a host names an interface and nothing downstream re-derives what that name
   means. */
static int g_global_names_slot;

#if APICLIENT_DEV
/* bsearch over IDL_EXPOSURE's own rows, keyed by the identifier — the same shape core/idl_args.c uses over the
   same table, because the table is sorted by that field and there is one right way to ask it. */
static int idl_exposure_row_name_cmp(const void *key, const void *row)
{
    return strcmp((const char *)key, ((const IdlExposureRow *)row)->name);
}

/* WEB IDL §3.8 Platform objects implementing interfaces' DESCRIPTOR, ASSERTED OVER THE FINISHED GLOBAL — the
   one check that can see an install which never called the install entry.
   WHY IT CANNOT LIVE AT THE ENTRY. `define the global property references` has exactly one door here
   (core/idl_args' idl_define_global_property_reference), and the failure this guards is a component that does
   not USE that door: it reaches for JS_SetPropertyStr, which is ECMAScript §10.1.9.2 OrdinarySetWithOwnDescriptor
   creating the property through CreateDataProperty, whose descriptor is writable AND ENUMERABLE AND
   configurable. §3.8 performs "DefineMethodProperty(target, id, interfaceObject, false)" and ECMAScript
   §10.2.8 DefineMethodProperty ( homeObj, name, closure, enumerable ) writes the triple down — "Let
   propertyDesc be the PropertyDescriptor { [[Value]]: closure, [[Writable]]: true, [[Enumerable]]: enumerable,
   [[Configurable]]: true }" — so with §3.8's `false` substituted the two agree on the value and disagree on
   [[Enumerable]]. An assert INSIDE the entry is blind to every caller that never arrives at it, and would in
   any case stamp the entry's own file and line for all hundred-odd callers, naming an action with no object.
   THE IDENTIFIER IS THE SITE. This does not thread a caller's __FILE__ anywhere: the message names the
   IDENTIFIER, and an identifier is an address — `git grep '"Screen"' engine/host/browser` is one command and
   lands on the line that installed it. That is what makes a walk over a finished object an actionable crash
   rather than a report that something, somewhere, is wrong.
   WHY IT IS SOUND. The subject is a realm this codebase has just finished building, before one byte of page
   script has run, so every property name it reads is a name this codebase wrote — never a value a stranger
   stated, which is the only thing that would make an abort here a switch somebody else holds. The two sets
   cannot collide by accident either: of the corpus's identifiers exactly ONE begins with a lower-case letter
   (`console`, a §3.13.1 namespace object, which is in this band and answers to the same descriptor), so a
   §3.7.6 attribute or §3.7.7 operation of the [Global] interface — which §3.7.6 requires to be ENUMERABLE, and
   which is spelled `location`, `document`, `fetch` — can never be mistaken for one of these. No ECMAScript
   intrinsic global name is a Web IDL identifier either, so the JS engine's own globals are outside the set.
   WHAT IT DOES NOT SEE, named because a check trusted past its evidence is worse than none: an interface
   object a HOST puts on the global AFTER this returns (a harness global, a solver seam) is outside the walk,
   because the walk runs where the platform's own installs finish. Those are not §3.8 property references and
   none of them is named by the corpus; the day one is, this check is the thing that has to move rather than
   the thing that was wrong. */
static void realm_assert_global_property_references(JSContext *ctx)
{
    JSPropertyEnum *tab = NULL;
    uint32_t n = 0, i;
    JSValue global = JS_GetGlobalObject(ctx);
    int ok = JS_GetOwnPropertyNames(ctx, &tab, &n, global, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY);

    /* This block only exists in a dev build, so the DCHECK is live here and the failure arm below it is not a
       path: a non-zero status aborts, and `tab`/`n` are never read out of a failed call. */
    DCHECK(ok == 0, "the realm's global could not be enumerated to check Web IDL §3.8's descriptor");
    for (i = 0; i < n; i++) {
        const char *name = JS_AtomToCString(ctx, tab[i].atom);
        const IdlExposureRow *row;

        if (name == NULL) continue;
        row = bsearch(name, IDL_EXPOSURE, sizeof IDL_EXPOSURE / sizeof IDL_EXPOSURE[0],
                      sizeof IDL_EXPOSURE[0], idl_exposure_row_name_cmp);
        DCHECKF(row == NULL,
                "`%s` is an ENUMERABLE own property of this realm's global, and Web IDL §3.8 Platform objects "
                "implementing interfaces performs DefineMethodProperty(target, id, interfaceObject, false) for "
                "it — ECMAScript §10.2.8 DefineMethodProperty ( homeObj, name, closure, enumerable ) makes that "
                "{ [[Writable]]: true, [[Enumerable]]: FALSE, [[Configurable]]: true }, so a browser never "
                "shows this name to `for (var k in globalThis)` or to `Object.keys(globalThis)`. It got here "
                "through an ordinary [[Set]] rather than through core/idl_args' "
                "idl_define_global_property_reference, which is §3.8's one door and also where §3.3.7 "
                "[Exposed] step 1 is asked — so this interface is ALSO present in every realm its exposure set "
                "excludes. Route the install: `git grep '\\\"%s\\\"' engine/host/browser` finds it", name, name);
        JS_FreeCString(ctx, name);
    }
    for (i = 0; i < n; i++) JS_FreeAtom(ctx, tab[i].atom);
    js_free(ctx, tab);
    JS_FreeValue(ctx, global);
}
#endif

void realm_install_intrinsics(JSContext *ctx, const char *top_level_creation_url,
                              const char *global_interface)
{
    JSValue url;
    int i;

    DCHECK(ctx != NULL, "the per-realm intrinsics were installed into no realm");
    /* AND THE HOST HAS SAID WHETHER IT EXPLORES, BEFORE ANY OF THEM RUN. An intrinsic below may mint a member
       through solver/concolic.h's source seam — navigator.c mints its whole environment record there, once,
       for this realm's LIFETIME — and that seam's answer is the whole of whether the value forks control flow.
       A realm built before the host declares therefore keeps the browser-only answer for the rest of the
       session, with nothing to say so: the members exist, carry the right example, coerce and compare
       correctly, and simply DECIDE at every gate a bundle writes over them instead of forking. Measured on
       this project's own smoke fixture, whose host installed the overlay a hundred lines after bringing its
       agent up: `navigator.userAgent.indexOf('Chrome') >= 0` and `navigator.maxTouchPoints > 0` took one arm
       each for the whole run and the sibling worlds were never created, while `screen.width < 768` in the same
       document forked — because screen.c mints unconditionally and asks nobody.
       IT IS ASSERTED HERE AND NOT AT THE SEAM ALONE because this is the one call every realm of every host
       goes through (the agent's first, and every child navigable's), so a host that gets the order wrong is
       told at the realm it built too early rather than at whichever member happens to be minted first. */
    DCHECK(concolic_source_overlay_declared(),
           "a realm's intrinsics were built before this host declared whether it EXPLORES — a per-realm member "
           "minted through concolic_source_wrap freezes the answer standing at this instant, so every gate a "
           "page writes over one of them would decide instead of forking, for this realm's whole life. Declare "
           "first: concolic_install_source_overlay for a solver host, concolic_declare_browser_only for a "
           "conformance one, both before platform_agent_init");
    /* EVERY ENVIRONMENT HAS ONE. §8.1.3.1's field is null only for a worker or a worklet, and this engine has
       neither — every environment it builds is a Window one, created AT an address. A host with nothing to
       pass here has not decided which document this realm is, which is the same thing location.c's install
       asserts one layer up and for the same reason: the empty string used to be answered quietly and the
       quiet answer hid a host passing the wrong field entirely. */
    DCHECK(top_level_creation_url != NULL && *top_level_creation_url,
           "a realm was built with no TOP-LEVEL CREATION URL — HTML §8.1.3.5 reads it to decide whether this "
           "environment is a SECURE CONTEXT, and Web IDL §3.3.13's members are installed or absent by that "
           "answer, so a realm without it is a realm whose platform surface is undecided");
    /* AND WHICH [Global] INTERFACE ITS GLOBAL OBJECT IMPLEMENTS, for the same reason and one line further on:
       Web IDL §3.3.7 [Exposed] step 1 decides which interface objects the intrinsics below may put on this
       global at all, so the answer has to exist before the first of them runs. A host that does not state it
       has not decided what KIND of realm this is, which is a different question from which document it is —
       and one this engine could not previously ask, which is why every realm it builds is a Window one. */
    DCHECK(global_interface != NULL && *global_interface,
           "a realm was built with no [Global] INTERFACE — Web IDL §3.3.7 [Exposed] step 1 asks which "
           "interface this realm's global object implements, and the answer decides which of the platform's "
           "interface objects exist here at all. A Window realm states \"Window\"");
    if (!g_top_level_url_slot)
        g_top_level_url_slot = realm_value_declare(ctx, "HTML §8.1.3.1 the environment's top-level creation URL");
    if (!g_global_names_slot)
        g_global_names_slot = realm_value_declare(ctx, "Web IDL §3.3.8 [Global] the realm's global names");
    url = JS_NewString(ctx, top_level_creation_url);
    CHECK(!JS_IsException(url), "realm: the environment's top-level creation URL could not be allocated");
    realm_value_set(ctx, g_top_level_url_slot, url);
    realm_value_set(ctx, g_global_names_slot,
                    JS_NewInt32(ctx, (int32_t)idl_global_names_of(global_interface)));
    for (i = 0; i < g_n; i++)
        g_list[i](ctx);
#if APICLIENT_DEV
    /* EVERY §3.8 PROPERTY REFERENCE THIS REALM HAS, NOW THAT IT HAS ALL OF THEM. It is asked here and not
       inside a component because the invariant is about the FINISHED global: a component can only see the one
       name it installed, and what has to be true is a statement about the whole object. */
    realm_assert_global_property_references(ctx);
#endif
}

JSValue realm_top_level_creation_url(JSContext *ctx)
{
    DCHECK(g_top_level_url_slot != 0,
           "a realm's top-level creation URL was read in an agent where no realm has been built — the field is "
           "created WITH the realm, so a reader that gets here is standing outside every realm there is");
    return realm_value_get(ctx, g_top_level_url_slot);   /* asserts THIS realm ran the install */
}

unsigned realm_global_names(JSContext *ctx)
{
    JSValue v;
    int32_t names = 0;

    DCHECK(g_global_names_slot != 0,
           "a realm's Web IDL §3.3.8 [Global] global names were read in an agent where no realm has been "
           "built — the field is created WITH the realm, so a reader that gets here is standing outside every "
           "realm there is");
    v = realm_value_get(ctx, g_global_names_slot);   /* asserts THIS realm ran the install */
    /* READ OFF THE TAG, NEVER THROUGH A COERCION. `JS_ToInt32` would have been the obvious spelling and it is
       the banned one: its result is what says whether the read worked, so the check that reads it is a check
       with a SIDE EFFECT — compiled out in release, `names` is never written, every construct whose exposure
       set is not `*` loses its global property, and a release build's whole platform surface goes with it.
       The value is one this file wrote with JS_NewInt32 and nothing else can reach the slot, so the tag is the
       invariant and reading it is not a conversion at all. */
    DCHECK(JS_VALUE_GET_TAG(v) == JS_TAG_INT,
           "a realm's Web IDL §3.3.8 [Global] global names are not the integer this file wrote there — the "
           "slot is written once, by realm_install_intrinsics, with the mask idl_global_names_of resolved");
    names = JS_VALUE_GET_INT(v);
    JS_FreeValue(ctx, v);
    return (unsigned)names;
}

void realm_intrinsics_free(void)
{
    free(g_list);
    g_list = NULL;
    g_n = g_cap = 0;
    /* The slot's VALUES are the realms' and went with them; what the agent holds is the slot id, which is a
       class id in a runtime that is going away with it. */
    g_top_level_url_slot = 0;
    g_global_names_slot = 0;
}

/* A slot IS a class id whose per-context prototype slot holds something that is not a prototype. Nothing is
   ever constructed with the class — it exists for the SLOT, which is the per-realm store quickjs already keeps
   and already frees with the context. The `what` string is the class name, so a heap dump names the slot. */
int realm_value_declare(JSContext *ctx, const char *what)
{
    JSClassID id = 0;
    JSClassDef d;

    DCHECK(what != NULL && *what, "a per-realm value was declared with no name — a dump would not say whose");
    d = (JSClassDef){ what };
    JS_NewClassID(JS_GetRuntime(ctx), &id);
    CHECK(JS_NewClass(JS_GetRuntime(ctx), id, &d) == 0, "realm: a per-realm value slot could not be declared");
    return (int)id;
}

void realm_value_set(JSContext *ctx, int slot, JSValue v)
{
    JSValue prev;

    DCHECK(slot > 0, "a per-realm value was set through a slot that was never declared");
    prev = JS_GetClassProto(ctx, (JSClassID)slot);
    DCHECK(JS_IsNull(prev), "a per-realm value was set twice in one realm — the first is what everything "
                            "already built in this realm is holding");
    JS_FreeValue(ctx, prev);
    JS_SetClassProto(ctx, (JSClassID)slot, v);
}

JSValue realm_value_get(JSContext *ctx, int slot)
{
    JSValue v;

    DCHECK(slot > 0, "a per-realm value was read through a slot that was never declared");
    v = JS_GetClassProto(ctx, (JSClassID)slot);
    DCHECK(!JS_IsNull(v), "a per-realm value was read in a realm that never ran the install that sets it");
    return v;
}

void realm_awaits(JSContext *ctx, const char *path, const char *what)
{
    JSValue cur = JS_GetGlobalObject(ctx);
    const char *p = path;
    bool present = false;

    DCHECK(path != NULL && *path, "a producer assertion named no producer");
    for (;;) {
        const char *dot = strchr(p, '.');
        size_t n = dot ? (size_t)(dot - p) : strlen(p);
        char name[64];
        JSAtom atom;
        int has;

        DCHECK(n > 0 && n < sizeof(name), "a producer assertion named a path this cannot read");
        if (!JS_IsObject(cur)) break;            /* the path died on the way; the producer is absent */
        memcpy(name, p, n);
        name[n] = 0;

        atom = JS_NewAtomLen(ctx, name, n);
        has = JS_HasProperty(ctx, cur, atom);
        CHECK(has >= 0, "a producer probe threw — [[HasProperty]] over an engine global runs no page code and "
                        "has nothing to throw with");
        if (!has) { JS_FreeAtom(ctx, atom); break; }
        if (!dot) { JS_FreeAtom(ctx, atom); present = true; break; }   /* the member exists: producer is built */

        {
            JSValue next = JS_GetProperty(ctx, cur, atom);
            JS_FreeAtom(ctx, atom);
            JS_FreeValue(ctx, cur);
            cur = next;
        }
        p = dot + 1;
    }
    JS_FreeValue(ctx, cur);
    DCHECK(!present, what);
}
