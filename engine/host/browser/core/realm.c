/* See realm.h. */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "core/idl_args.h"   /* §3.3.7 [Exposed] step 1: resolving a [Global] interface's global names */
#include "core/realm.h"
#include "solver/concolic.h"

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
