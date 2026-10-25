#ifndef SOLVER_REALM_HOLDERS_H
#define SOLVER_REALM_HOLDERS_H

#include <stdbool.h>

/* Which resident flows' deltas name each live child realm.
 *
 * A child realm is freed by the cycle collector once nothing outside its own cycle names it. A flow names it
 * through its COW delta: a captured WindowProxy record dups the child's Window (window_proxy.c's PROXY_VALS), and
 * a realm created in a flow is named by that flow's captured writes. A frozen segment names it for every member
 * sharing it, so the collection after the last such member is sold is the one that can free it.
 *
 * Per live realm (core/frame/navigable.h), counts the frontier members whose delta (head and frozen chain,
 * cow_delta_objects) owns a reference to its global, its WindowProxy or its Document object; for the realms no
 * delta names, counts the references to those handles from outside the GC graph (JS_GCNamedBy), which root a
 * realm against every collection. Other objects of a realm, suspended frames, jobs, foreign world segments and
 * the pre-fork baseline are not read, so `unheld` is an upper bound on what the next collection frees. Gauges at
 * one instant, each shared segment walked once. A refused allocation at the RAM floor answers false. */
typedef struct {
    int  min, max;          /* holders per live realm, -1 when none is live */
    long total;
    int  unheld;            /* live realms no member's delta names */
    int  unheld_rooted;     /* ...of which some handle has a reference from outside the GC graph */
    long unheld_outside;    /* those references, summed over the unheld realms' handles */
} RealmHolders;
bool realm_holders_census(RealmHolders *out);

/* Whether some census of this process found at least one live child realm and every live one unheld — the
   instant a realm's last delta holder had gone and the collection that can free it had not yet run. A lifetime
   latch, so a fixture row can read it after the session. */
bool realm_holders_all_unheld_seen(void);

#endif
