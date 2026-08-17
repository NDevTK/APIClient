/* THE RENDERER REGISTRY — which agent clusters have an instance, what routing id each was given, and the
 * refusal of a second instance for one cluster. It is the BROWSER PROCESS's own state, and it is the reason
 * that program's name is a description rather than an aspiration — it is now the WHOLE of that program.
 * `browser_process/network/` stood beside it holding §7 sniffing, CORB, nosniff and resource classification,
 * and those are deleted: CLAUDE.md §Architecture puts type sniffing back in `extension/lib/safe-fetch.js`,
 * which reads the bytes and stamps what it decided onto the reply record. A network service was never what
 * decides that a renderer exists, and deciding that is what is left here.
 *
 * WHY IT IS C, WHICH IS THE WHOLE POINT OF THE FILE. It was JavaScript — a `Map`, a `_nextRoutingId++`, three
 * counters and a duplicate check, in `extension/browser-process.js` — and CLAUDE.md §Architecture leaves that
 * zone a BRIDGE and never logic. Deciding which renderers exist, minting the name each is known by, and
 * refusing the second one is computation over a string and an integer; nothing in it needs a `document`, a
 * `postMessage` or a `Worker`, which is exactly the test for whether a bridge edge is irreducible. What is
 * irreducible is one line and it stays JavaScript: `content.mojom.Zygote.ForkRenderer`, because a dedicated
 * Worker's global is `DedicatedWorkerGlobalScope` — no `document`, no `createElement`, no frame — so this
 * process can ORDER a renderer materialized and can never materialize one, exactly as Chromium's browser
 * process asks a zygote to fork rather than forking itself.
 *
 * WHAT THE INVARIANT IS. SECURITY.md: "One WASM instance per ORIGIN-KEYED AGENT CLUSTER — `(browsing-context
 * group, origin)`". The instance IS the principal, so two instances for one cluster is two heaps behind one
 * principal and a heap split across same-origin documents breaks HTML's own single-heap agent. This table is
 * the authority for that rule and it is the ONLY authority: the offscreen pool asks, this decides, the zygote
 * obeys. The rule is a PRODUCTION invariant and not a fidelity nicety, so the assertions that guard it are
 * `CHECK` and not `DCHECK` — a release build that quietly registered the second renderer would hand two
 * principals one origin, which is worse than crashing. In JavaScript that same rule was a `DCHECK` whose
 * release path OVERWROTE the map entry: the first renderer's id was then unknown to the registry that minted
 * it and its termination would have freed the second renderer's cluster.
 *
 * WHAT A ROUTING ID IS. The only name a renderer has, minted here and nowhere else, which is what makes it
 * evidence of WHICH PROCESS decided an instance should exist. `rendererPoolProbe` cross-checks the id set the
 * offscreen holds frames for against the set this table holds — and that comparison only means something
 * because the two sides are now two PROGRAMS. Before this file, both sides of it were produced by two JS
 * modules in one trust zone and one realm, so it read identically whether the browser process decided or the
 * offscreen did.
 *
 * WHY THE KEY IS BYTES AND NOT A C STRING. `clusterKeyOf` joins the browsing-context group and the origin with
 * a NUL, "because neither half can contain one" — so the key is a byte sequence with an interior NUL, and
 * `ccall`'s `"string"` marshalling would deliver it truncated at the separator. Every origin in one tab would
 * then answer to one key, and the registry whose entire job is to refuse a merged cluster would perform one.
 * The key therefore crosses as a pointer and a LENGTH, the same shape a resource header crosses in, and this
 * component treats it as opaque bytes: which halves it has is `bridge.js`'s structure, not this table's. (The
 * renderer probe asks for the cluster `"probe"`, which has no separator at all and is a legitimate key here
 * for exactly that reason.) */
#ifndef ENGINE_HOST_BROWSER_PROCESS_RENDERER_REGISTRY_H
#define ENGINE_HOST_BROWSER_PROCESS_RENDERER_REGISTRY_H

#include <stddef.h>

/* DECIDE THAT AN AGENT CLUSTER GETS AN INSTANCE, and mint the routing id it will be known by. Returns that id,
   which is always positive. It does not return a refusal, and that is deliberate: the only refusals this
   decision has are a second renderer for a live cluster and an empty cluster key, both of which are the
   security invariant above rather than an outcome a caller may inspect and carry on past. A returned refusal
   is a value a caller can ignore, and what it would be ignoring is two heaps behind one principal.
   THE SLOT IS TAKEN BEFORE THE CALLER'S FORK ORDER GOES OUT, and in C that is structural rather than a
   discipline: this call cannot suspend, so a second request for one cluster arriving while a fork is
   outstanding finds the slot taken. In JavaScript it was a comment asking the next editor not to `await`
   between the check and the write. */
int renderer_registry_create(const unsigned char *cluster_key, size_t cluster_key_n);

/* THE FORK ORDER WAS ANSWERED. `launched` — the zygote handed back a pipe, so a renderer exists under this id.
   `launch_failed` — it handed back a reason instead, so the frame is already gone and the agent cluster is
   FREED here; leaving it registered would refuse that cluster an instance forever with nothing anywhere to say
   why. They are two entries and not one taking a boolean because they are two different transitions of the
   slot, each with its own precondition to assert. */
void renderer_registry_launched(int routing_id);
void renderer_registry_launch_failed(int routing_id);

/* A RENDERER'S DEATH, OBSERVED. A real renderer can exit on its own and the browser learns of it; here the
   offscreen owns the frame and so is what notices. The slot is released. A routing id this process never
   minted CRASHES rather than being ignored — it is the one number only this program can produce, so one coming
   back that this table never issued is the inversion this whole transport exists to make impossible. */
void renderer_registry_terminated(int routing_id);

/* THE TABLE, AS THE RECORD `content.mojom.RendererHost.GetRegistry` declares — one JSON document carrying the
   registered cluster keys, the live routing ids, and the four counters, built where the table lives so no
   consumer re-derives a field. The returned pointer is owned by this component and stays valid until the next
   call of this function. */
const char *renderer_registry_snapshot_json(void);

#endif
