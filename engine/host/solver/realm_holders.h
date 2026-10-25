#ifndef SOLVER_REALM_HOLDERS_H
#define SOLVER_REALM_HOLDERS_H

#include <stdbool.h>

/* WHICH RESIDENT FLOWS' DELTAS NAME EACH LIVE CHILD REALM.
 *
 * A child realm is freed by the cycle collector once nothing outside its own cycle names it. On a forking session
 * the counted names a flow leaves are in its COW delta: a captured WindowProxy record dups the child's Window
 * (window_proxy.c's PROXY_VALS), and a realm created inside a flow is named by that flow's captured writes, such as
 * a global variable's new value holding its proxy. A frozen segment shared by a family names it for every member,
 * so the first collection after the last such member is sold is the one that can free it.
 *
 * For each live realm (core/frame/navigable.h), the number of frontier members whose delta (head and frozen chain,
 * cow_delta_objects) owns a reference to one of the realm's handles: its global object, its WindowProxy or its
 * Document object. `min`, `max` and `total` are over the live realms, -1 for min and max when none is live;
 * `unheld` counts live realms no member's delta names. Another object of a realm (a function, a node) named only
 * by a delta is not matched, and a realm named by a suspended frame, a job, a foreign world segment (world.c) or
 * the pre-fork baseline is held by a route this walk does not read, so `unheld` is an upper bound on what the next
 * collection frees. Gauges, read at one instant: a walk of every member's chain, each shared segment once, taken
 * only while a child realm is live. Its allocations may be refused at the RAM floor, as every census composer's
 * may be (solver/engine.c's census loop sells one member and composes again): it then answers false and writes
 * nothing. */
bool realm_holders_census(int *min, int *max, long *total, int *unheld);

/* Whether some census of this process found at least one live child realm and every live one unheld — the
   instant a realm's last delta holder had gone and the collection that can free it had not yet run. A lifetime
   latch, so a fixture row can read it after the session. */
bool realm_holders_all_unheld_seen(void);

#endif
