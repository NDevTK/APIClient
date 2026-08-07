/* THE WORLD REGISTRY — how one flow's COW delta stays coherent across documents in different WASM instances.
 *
 * THE PROBLEM. One WASM instance is one DOCUMENT, regardless of origin. A flow that scripts an iframe or a
 * popup therefore writes state in TWO instances, and those writes are one world: rewinding that flow has to
 * rewind both, and parking it has to park both. A per-instance delta that only agrees locally is two timelines
 * wearing one name — the same bug class as modelling async state globally.
 *
 * WHY THE DELTA CANNOT TRAVEL. A CowEntry names its target by a LIVE HEAP POINTER. A pointer has no meaning
 * outside the linear memory it was taken from, which is exactly why CLAUDE.md forbids paging delta bytes to the
 * cold tier. The instance boundary is that same boundary, so the answer has to be the same: the delta stays
 * where its targets are, and something ELSE crosses.
 *
 * WHAT CROSSES IS THE NAME. A flow's world gets a globally unique id — (document id, serial) — minted by the
 * instance that created the flow, so no two instances can collide without ever coordinating. A cross-document
 * request carries that id, and the peer looks up (or builds) ITS OWN segment for that world. So a world is a
 * SET of per-instance segments, and this registry is one instance's half of it.
 *
 * HOW A PEER BUILDS A SEGMENT IT HAS NEVER SEEN — and this is the part that makes the scheme O(what actually
 * arrived) rather than O(the sender's frontier). Flows fork; a child inherits the parent's writes, including
 * the parent's writes in OTHER instances. So a request carries the world's ANCESTRY (the fork chain, nearest
 * first) and the peer forks the nearest ancestor it already holds. That is cow_delta_fork, which is already
 * O(1) through the refcounted shared base segment. If the peer holds NONE of the ancestry, the world has never
 * written anything here and starts from this instance's baseline — an empty segment, which is the truth rather
 * than a default.
 *
 * NEAREST, NOT ANY. If a peer holds both a grandparent and a parent, forking the grandparent silently drops
 * the parent's writes. The ancestry is therefore ordered nearest-first and the scan stops at the first hit —
 * asserted, because a wrong answer here is invisible: the flow just sees an older document than it wrote.
 *
 * WHAT IS NOT HERE YET. The transport (which instance holds which document, and the suspend/resume edge a
 * cross-document read rides) is the host's, because only the trusted zone knows the routing — the same reason
 * authorization keys on sender.tab.url. This component is the half that has to be right BEFORE a transport can
 * carry anything, and it crashes rather than guessing at every question the transport has not answered yet. */
#ifndef ENGINE_HOST_SOLVER_WORLD_H
#define ENGINE_HOST_SOLVER_WORLD_H

#include <stdbool.h>
#include <stdint.h>

#include "quickjs.h"
#include "solver/cow.h"

/* A GLOBALLY UNIQUE WORLD NAME. `doc` is the minting instance's document id and `serial` is its own counter, so
   uniqueness needs no allocator, no lock and no round trip — which matters because a world is minted on the
   fork path, the hottest path in the engine. A zeroed WorldId is the NONE value: serial counts from 1. */
typedef struct { uint32_t doc; uint32_t serial; } WorldId;

#define WORLD_NONE ((WorldId){ 0, 0 })
static inline bool world_is_none(WorldId w) { return w.doc == 0 && w.serial == 0; }
static inline bool world_eq(WorldId a, WorldId b) { return a.doc == b.doc && a.serial == b.serial; }

/* This instance's document id, which every world it mints is qualified by. Must be non-zero and must differ
   from every peer's — the host assigns it, because the host is what knows there is more than one. */
void world_registry_init(uint32_t doc_id);
void world_registry_free(JSContext *ctx);

/* THIS INSTANCE'S DOCUMENT ID — the ONE answer to "which document am I", so that "is this navigable remote?"
   is a comparison against a single identity rather than a second naming scheme kept in parallel. */
uint32_t world_local_doc(void);

/* MINT a root world, for a flow created in this instance from the baseline. */
WorldId world_mint(void);

/* MINT a child of `parent` and RECORD the edge, so this instance can later hand a peer the ancestry it needs to
   materialize the child. Called at the one place a sibling flow is created from a parent. */
WorldId world_mint_child(WorldId parent);

/* THE ANCESTRY OF A LOCALLY-MINTED WORLD, nearest first, written into `out` (at most `cap`); returns how many
   were written. This is what travels with a cross-document request. Asserts the world was minted here — a
   world minted elsewhere has an ancestry only its own instance can answer for. */
int world_ancestry(WorldId w, WorldId *out, int cap);

/* THIS INSTANCE'S SEGMENT for a world minted ELSEWHERE, materialized on first use by forking the nearest
   ancestor present in `ancestry` (nearest first, as world_ancestry writes it), or empty if none is. Borrowed:
   the registry owns it until world_release. */
CowDelta *world_segment(JSContext *ctx, WorldId w, const WorldId *ancestry, int n_anc);

/* Has this instance already materialized a segment for `w`? The sender asks the equivalent question about a
   peer before mirroring a fork; here it is what makes "materialized lazily" observable to a fixture. */
bool world_has_segment(WorldId w);

/* The world is gone (its flow finished, or was dropped): release this instance's segment. Releasing a world
   with no segment is not an error — a world that never wrote here never had one. */
void world_release(JSContext *ctx, WorldId w);

#endif
