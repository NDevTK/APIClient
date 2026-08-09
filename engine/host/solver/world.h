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

/* THIS INSTANCE'S DOCUMENT, BY NAME. The root instance's name comes from the host, because only the host knows
   there is more than one document at all; every name below it is minted here — see world_mint_doc. */
void world_registry_init(const char *doc_name);
void world_registry_free(JSContext *ctx);

/* THIS INSTANCE'S ROOT DOCUMENT — the one the host named. It is NOT "the document I am": an instance is an
   origin-keyed AGENT and holds one realm per same-origin document, so several documents are this instance's. */
uint32_t world_local_doc(void);

/* DOES THIS INSTANCE HOLD THE REALM OF `doc`? THE question behind "is this navigable remote?", and it is a
   question about WHERE the document lives, never about which document is asking. A same-origin child is a
   second realm in THIS heap and answers every read in this turn; a cross-origin one is another instance and
   every read through it suspends. Comparing against the root document instead answered "remote" for a
   same-origin child that is sitting in the same runtime. */
bool world_doc_hosted(uint32_t doc);
/* THIS AGENT HOLDS `doc` — said once, by §7.4, because deciding to host it is the fact that makes it true. It
   is NOT "its realm has been built": a hosted navigable's realm is materialized on the first read that reaches
   through to its active document (navigable.h), and a navigable nothing ever reads through is still this
   agent's. Minting a name, hosting, and building the realm are three separate statements in that order. */
void world_doc_adopt(uint32_t doc);

/* A DOCUMENT IS NAMED, AND A `uint32_t doc` IS THIS INSTANCE'S HANDLE FOR A NAME — an index into the local
   table, 1-based so zero stays the NONE value. Handles are local and mean nothing to a peer; the NAME is what
   crosses the seam, which is why every request that carries a document carries world_doc_name.
 *
 * WHY NAMES AND NOT NUMBERS THE HOST HANDS OUT. A document created by this one is named "<my name>.<n>":
 * unique by induction from the root name, with no allocator, no lock and no round trip — the identical argument
 * the WorldId above rests on, applied one level up. It has to be, because HTML §4.8.5 creates a child navigable
 * in the INSERTION STEPS: `frame.contentWindow` answers on the line after the append, and a round trip cannot
 * happen there. Asking the host to mint turned the one operation the spec defines as synchronous into a
 * suspend, and every host answered it with "not created" rather than host a second document — so an iframe's
 * contentWindow was null and the whole of html/browsers read members of null. Numbering by a host-assigned
 * block would have been the other way to avoid the round trip, and it is a cap on how many documents can
 * exist, which §scheduler bans. */
uint32_t    world_doc_intern(const char *name);
const char *world_doc_name(uint32_t doc);

/* MINT A DOCUMENT CREATED BY `parent` and return its handle. Synchronous and unbounded. The parent is named
   rather than assumed to be the instance root: a same-origin child is a realm of this instance and creates
   children of its own, and naming them "<root>.<n>" would collide the moment two realms both minted. */
uint32_t world_mint_doc(uint32_t parent);

/* MINT a root world, for a flow created in this instance from the baseline. */
WorldId world_mint(void);

/* MINT a child of `parent` and RECORD the edge, so this instance can later hand a peer the ancestry it needs to
   materialize the child. Called at the one place a sibling flow is created from a parent. */
WorldId world_mint_child(WorldId parent);

/* THE ANCESTRY OF A LOCALLY-MINTED WORLD, nearest first, written into `out` (at most `cap`); returns how many
   were written. This is what travels with a cross-document request. Asserts the world was minted here — a
   world minted elsewhere has an ancestry only its own instance can answer for. */
int world_ancestry(WorldId w, WorldId *out, int cap);

/* THE WIRE FORM OF A WORLD AND ITS ANCESTRY — `doc:serial,anc:serial,...`, nearest ancestor first, exactly as
   world_ancestry writes it. Every request or notice that crosses to another instance carries this, and it is
   ONE function because two spellings of it would be two peers materializing different segments for the same
   flow. Writes at most `cap` bytes including the NUL and returns the length written; a truncation is a
   corrupted vector, so it crashes rather than sending a prefix. */
int world_serialize(WorldId w, char *dst, size_t cap);

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
