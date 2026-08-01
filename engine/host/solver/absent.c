/* WHICH ABSENT GLOBAL IS INPUT, AND WHICH IS A COMPONENT THIS ENGINE OWES.
 *
 * Both wear the same shape — a name that resolves nowhere — and answering them the same way loses either the
 * whole logged-in surface or every forcing function.
 *
 * SERVER-INJECTED APP STATE is unknown INPUT. `window.__FLAGS`, `__USER`, `__NEXT_DATA__` are written into the
 * document by the server for a logged-in visitor and simply absent for this one, so what they hold is not
 * `undefined` — it is UNKNOWN. Answering `undefined` makes `__FLAGS.admin` throw on the first field access and
 * buries every endpoint behind it, which is precisely the surface this tool exists to reach: the bundle ships
 * the auth and admin code to a logged-out visitor and it never runs. Symbolic instead, so the gate FORKS and
 * the logged-in arm is explored.
 *
 * A WEB API THIS ENGINE HAS NOT BUILT is honestly absent. Its ReferenceError is the forcing function that names
 * the component to write, and handing back a symbol instead would let a flow run past a missing capability and
 * report a surface it never reached. The distinction is decidable because the components DECLARE what they
 * install: a name the platform surface owns is the engine's, and everything else the page reads is the
 * server's. */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "solver/absent.h"
#include "solver/concolic.h"

static char **g_platform;
static int    g_platform_n, g_platform_cap;

void absent_declare_platform(JSContext *ctx, const char *name)
{
    (void)ctx;
    DCHECK(name != NULL && *name, "a component declared an empty platform name");
    if (g_platform_n == g_platform_cap) {
        int c = g_platform_cap ? g_platform_cap * 2 : 16;
        char **a = realloc(g_platform, sizeof(*a) * (size_t)c);
        CHECK(a != NULL, "the platform-name table allocation failed");
        g_platform = a; g_platform_cap = c;
    }
    g_platform[g_platform_n] = strdup(name);
    CHECK(g_platform[g_platform_n] != NULL, "the platform-name allocation failed");
    g_platform_n++;
}

JSValue absent_global_hook(JSContext *ctx, JSAtom name)
{
    const char *s = JS_AtomToCString(ctx, name);
    JSValue r = JS_UNINITIALIZED;
    char shape[128], src[128];
    int i;

    if (!s)
        return JS_UNINITIALIZED;
    /* A name the platform owns is a component this engine owes; leave the read alone so its throw names it. */
    for (i = 0; i < g_platform_n; i++)
        if (strcmp(g_platform[i], s) == 0)
            goto done;
    /* Anything else the page reads and nothing defines is the server's to have injected. Example-free: nothing
       here knows what a logged-in visitor's flags WOULD hold, and inventing one fabricates an observation. */
    snprintf(shape, sizeof(shape), "{%s}", s);
    snprintf(src,   sizeof(src),   "%s", s);
    r = concolic_new(ctx, shape, src, JS_UNDEFINED);
done:
    JS_FreeCString(ctx, s);
    return r;
}
