/* THE WORLD REGISTRY — how one flow's COW delta stays coherent across documents in different WASM instances.
 *
 * THE PROBLEM. One WASM instance is one ORIGIN-KEYED AGENT CLUSTER — `(browsing-context group, origin)` — so a
 * flow that scripts a CROSS-ORIGIN iframe or popup writes state in TWO instances, and those writes are one
 * world: rewinding that flow has to rewind both, and parking it has to park both. A per-instance delta that
 * only agrees locally is two timelines wearing one name — the same bug class as modelling async state globally.
 * (A SAME-ORIGIN child is a second REALM in this same heap and needs none of this; that is why the boundary is
 * tractable at all. This paragraph said "one instance is one document regardless of origin", which is the model
 * SECURITY.md rejects and would have made every same-origin frame read cross this transport.)
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

/* AND THE THIRD OF THOSE THREE STATEMENTS: WHICH REALM `doc` IS. A realm IS a document (core/dom/document.c),
 * so this is a fact about the document and it lives on the document's own row rather than in a table of its
 * own — document.c rejects a second list of documents in as many words, and names the failure mode that would
 * bite here: a stale row answering for a realm that is gone. The two edges are the document's, which is what
 * keeps the row honest without a walk: `document_install` is the moment a realm becomes the realm OF a
 * document, and `document_free` — reached from the realm's own teardown hook — is the moment it stops being
 * one.
 *
 * WHY THE QUESTION EXISTS AT ALL. An instance is an ORIGIN-KEYED AGENT CLUSTER (SECURITY.md), so SEVERAL
 * documents are this one's and a peer may hold a reference into any of them — `event.source` names the
 * document whose script posted, which is a child navigable as often as it is the root. A cross-instance
 * operation or delivery therefore arrives naming a document by NAME, and running it in this instance's root
 * realm instead would answer about the wrong document: `length` would be the count of the ROOT's child
 * navigables handed back as the child's. This is the direction HTML §7.3 states as "the navigable whose active
 * document is node's node document" — a document identifies exactly one of them, which is what makes the
 * answer a lookup rather than a search.
 *
 * NULL IS A REAL ANSWER AND NEVER "NOT FOUND": a hosted document with no realm is one whose initial
 * about:blank Document nothing has read through yet (navigable.h), and only the NAVIGABLE can materialize it.
 * The caller says what that means for it. */
void       world_doc_realm_set(uint32_t doc, JSContext *realm);
JSContext *world_doc_realm(uint32_t doc);

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

/* THE WIRE FORM OF A WORLD AND ITS ANCESTRY — `doc:serial,anc:serial,...`, nearest ancestor first. Every
   request or notice that crosses to another instance carries this, and it is ONE function because two
   spellings of it would be two peers materializing different segments for the same flow. Writes at most `cap`
   bytes including the NUL and returns the length written; a truncation is a corrupted vector, so it crashes
   rather than sending a prefix.
   THE ANCESTRY WALK IS NOT EXPORTED, and that is what makes the sentence above enforceable rather than
   advisory. It was, and `core/html/html_iframe.c` used it to write the vector a second way — head by hand,
   then its own loop — which had both failure modes this function's CHECK exists for: a `size_t` underflow past
   the end of the record's buffer, and a silently TRUNCATED chain that makes the peer fork a more distant
   ancestor and lose every write in between. With the walk internal there is no second way to spell it. */
int world_serialize(WorldId w, char *dst, size_t cap);

/* READ that wire form back: the world into `*out` and its ancestry (nearest first) into `ancestry`, returning
   how many ancestors were written. The inverse of world_serialize and deliberately its neighbour — a grammar
   with two readers is two grammars, and the writers of the second one are the hosts, where nothing can check it
   against this. Interning the document NAMES is part of reading, because a handle means nothing to a peer. */
int world_parse(const char *s, WorldId *out, WorldId *ancestry, int cap);

/* THIS INSTANCE'S SEGMENT for a world minted ELSEWHERE, materialized on first use by forking the nearest
   ancestor present in `ancestry` (nearest first, as world_ancestry writes it), or empty if none is. Borrowed:
   the registry owns it until world_release. */
CowDelta *world_segment(JSContext *ctx, WorldId w, const WorldId *ancestry, int n_anc);

/* Has this instance already materialized a segment for `w`? The sender asks the equivalent question about a
   peer before mirroring a fork; here it is what makes "materialized lazily" observable to a fixture. */
bool world_has_segment(WorldId w);

/* WHAT THIS SEAM HAS ACTUALLY DONE IN THIS INSTANCE: how many foreign worlds hold a segment here, and how many
   of those were materialized by FORKING an ancestor rather than starting from this instance's baseline. It
   belongs in the result document for the reason the context-switch count does — whether the nearest-ancestor
   fork ever runs cannot be inferred from the messages that arrived, and a mechanism nobody can see run is
   indistinguishable from one that has never run. Cumulative, and `forked` counts materializations rather than
   segments so a released-and-rebuilt world counts twice: it is a record of what the seam DID. */
void world_segment_stats(int *materialized, int *forked);

/* …AND HOW MANY IT HOLDS RIGHT NOW, WHICH IS A DIFFERENT QUESTION — and the one above answered it wrongly for
 * as long as the cold tier has existed. `materialized` is a HISTORY: it counts materializations, world_release
 * never decrements it, and the paragraph above says so in as many words ("a record of what the seam DID").
 * cold_park read it to decide a PRESENT-TENSE fact — "the frontier was parked while this instance HOLDS a
 * segment of a FOREIGN world" — and test_forced.c's world_registry_selftest materializes four peer worlds at
 * startup and RELEASES all four, so that instance holds none while the counter reads 4 for the rest of the
 * process. The first park that fixture ever succeeded in taking therefore aborted on a peer it does not have,
 * naming the cross-instance park (a large piece of work with the offscreen route in it) as the thing to build
 * next. A count that only ever rises cannot answer a question whose answer can fall, so they are two names and
 * neither is a parameter of the other.
 * `engine/route.mjs`, where a peer really exists, is unaffected either way: nothing there releases a world —
 * `world_release` still has no caller outside a self-test — so its two numbers coincide, which is precisely why
 * the substitution survived. */
int world_segments_held(void);

/* The world is gone (its flow finished, or was dropped): release this instance's segment. Releasing a world
   with no segment is not an error — a world that never wrote here never had one. */
void world_release(JSContext *ctx, WorldId w);

#endif
