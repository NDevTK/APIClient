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
 * AND EVERY ANCESTOR IS A DEAD WORLD, which is what makes forking one sound at all. A fork RETIRES the world it
 * branched at and mints a child for BOTH arms (world_mint_child), so a name that appears in an ancestry names
 * a timeline that ended at that branch. The alternative — the arm that keeps running keeps the fork point's
 * name — makes the peer's fork race the sender: cow_delta_fork freezes the ancestor's head at the instant it
 * runs, so whether the OTHER arm inherited the running arm's post-branch writes depended on which of them
 * reached that peer first. It also makes the two arms look like ancestor-and-descendant, which is the relation
 * that otherwise means "one is a continuation of the other" — so a peer could not tell a contradictory pair
 * from a sequential one. Both are asserted where the chain is walked.
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

/* A GLOBALLY UNIQUE WORLD NAME. `doc` is the minting instance's document id, `serial` is its own counter and
   `session` is its GENERATION (world_session below), so uniqueness needs no allocator, no lock and no round
   trip — which matters because a world is minted on the fork path, the hottest path in the engine. A zeroed
   WorldId is the NONE value: serial counts from 1, and a generation legitimately starts at 0. */
typedef struct { uint32_t doc; uint32_t serial; uint32_t session; } WorldId;

#define WORLD_NONE ((WorldId){ 0, 0, 0 })
static inline bool world_is_none(WorldId w) { return w.doc == 0 && w.serial == 0; }
static inline bool world_eq(WorldId a, WorldId b)
{
    return a.doc == b.doc && a.serial == b.serial && a.session == b.session;
}

/* THIS INSTANCE'S DOCUMENT, BY NAME. The root instance's name comes from the host, because only the host knows
   there is more than one document at all; every name below it is minted here — see world_mint_doc. */
void world_registry_init(const char *doc_name);
void world_registry_free(JSContext *ctx);

/* THIS SESSION'S GENERATION — the third coordinate of every name this instance mints, and what stops a resumed
 * session from answering for the one that ended.
 *
 * THE DEFECT IT CLOSES. `serial` counts from 1 in every session, while the document NAME is stable across a
 * park BY REQUIREMENT — routing keys on it (SECURITY.md: the engine NAMES its own child documents, the
 * offscreen ROUTES them). So without a third coordinate an instance that parks its frontier and comes back
 * would hand a surviving peer the very names it handed it before; world_segment would find the entry those
 * names already own, and the resumed flow would be answered inside the timeline of a flow that no longer
 * exists. The ancestry would match too, so neither side could tell. The only thing between that and silence
 * was engine.c's empty-delta assert, which fires only if the ended session happened to WRITE through that
 * world.
 *
 * WHY A GENERATION AND NOT A PARK-TIME NOTICE. A notice makes a peer FORGET a segment; it cannot make a NAME
 * unresolvable, and names outlive it — a vector already on the wire, a reference proxy the peer's page still
 * holds. It also has to be DELIVERED at the instant an instance leaves memory, which a browser restart does
 * not offer, while the residue crosses that boundary by construction. A generation is the same argument the
 * pair above already rests on: disjoint by construction, no allocator, no lock, no round trip. The notice is
 * still owed for a DIFFERENT reason — a peer holding a segment for a session that will never resume is a LEAK
 * — and that is world_session_gone below, announced INSIDE the park step because the flows themselves are
 * released by the teardown that follows, after the host has drained its last notices.
 *
 * WHERE IT COMES FROM. A park is the ONLY thing that makes a document name outlive a session: a document
 * loaded afresh is named by the host from what the browser minted for THAT load, so a fresh instance's
 * generation-0 names can collide with nothing. So the generation rides the RESIDUE — cold.c writes this
 * session's into the park document and a resumed session mints one above it. Nothing else has to cross. */
uint32_t world_session(void);
void     world_session_resume(uint32_t prev);

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
 * materialize the child.
 *
 * A FORK MINTS TWO OF THESE AND RETIRES THE FORK POINT — both arms get a child, and the world the flow branched
 * at names neither of them afterwards. Keeping the primary arm on the parent's name was the shape this had, and
 * it is wrong twice over:
 *   - IT MAKES THE ARMS UNCOMPARABLE. A peer's question about two arrivals is "do these two senders CONTRADICT,
 *     or is one a continuation of the other?" — sequential messages from one world are §9.4.4 tasks the page
 *     must see in order, and two arms of one branch are timelines that may not share one. With the primary on
 *     the parent's name the two arms are related as ancestor-and-descendant, which is exactly the relation that
 *     otherwise means "compatible". With both retired, divergence is decidable from the vectors alone: one
 *     world is a continuation of another exactly when it IS it or names it as an ancestor.
 *   - IT LETS ONE ARM'S LATER WRITES REACH THE OTHER. A peer materializes an arm's segment by FORKING the
 *     nearest ancestor it holds, and forking freezes that ancestor's head at the moment of the fork. While the
 *     primary kept writing into the fork point's segment, whether the sibling inherited those post-branch
 *     writes depended on the ORDER the two arms happened to reach that peer — the sibling arriving second forks
 *     a segment the primary has since written to. Retiring the fork point makes every ancestor a DEAD world, so
 *     what a peer forks can no longer change under it.
 * The cost is one extra minted row per fork and a world that changes at every branch a flow takes; the ancestry
 * a vector carries does not grow with it, because only worlds that have themselves crossed are named. */
WorldId world_mint_child(WorldId parent);

/* THE WIRE FORM OF A WORLD AND ITS ANCESTRY — `doc:session:serial,anc:session:serial,...`, nearest ancestor
   first. The GENERATION is in every field because it is part of the name, not a header: a peer keys its
   segment on the head of a vector it received, and a head short of its generation is the name of a session
   that may have ended. Every
   request or notice that crosses to another instance carries this, and it is ONE function because two
   spellings of it would be two peers materializing different segments for the same flow. Writes at most `cap`
   bytes including the NUL and returns the length written; a truncation is a corrupted vector, so it crashes
   rather than sending a prefix.
   THE ANCESTRY WALK IS NOT EXPORTED, and that is what makes the sentence above enforceable rather than
   advisory. It was, and `core/html/html_iframe.c` used it to write the vector a second way — head by hand,
   then its own loop — which had both failure modes this function's CHECK exists for: a `size_t` underflow past
   the end of the record's buffer, and a silently TRUNCATED chain that makes the peer fork a more distant
   ancestor and lose every write in between. With the walk internal there is no second way to spell it.
   ONLY ANCESTORS THAT HAVE THEMSELVES CROSSED ARE NAMED. A peer keys a segment on the HEAD of a vector it
   received, so a world that has never been a head is one no peer can be holding and naming it puts a field in
   the record that every reader scans past. That is not a size optimisation: a fork retires the fork point, so a
   flow's world changes at every branch it takes, and the unfiltered chain would grow with the number of
   branches rather than with the fork depth — past this record's buffer on any page whose boot flow forks
   freely. Filtered, the chain is the flow's cross-instance HISTORY, which is the only part of it a peer can act
   on. */
int world_serialize(WorldId w, char *dst, size_t cap);

/* READ that wire form back: the world into `*out` and its ancestry (nearest first) into `*ancestry`, returning
   how many ancestors were read. The inverse of world_serialize and deliberately its neighbour — a grammar
   with two readers is two grammars, and the writers of the second one are the hosts, where nothing can check it
   against this. Interning the document NAMES is part of reading, because a handle means nothing to a peer.
   THE ANCESTRY IS BORROWED FROM THE REGISTRY and is valid until the next world_parse — a caller-sized array is
   a bound on how many times a sending flow may have branched, which is a page-shaped number and not a design
   one, and the version of this that took one CRASHED on a vector that outran it. Every caller reads it into
   world_segment on the line after the parse, which is what makes the borrow window a statement rather than a
   hope. */
int world_parse(const char *s, WorldId *out, const WorldId **ancestry);

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
 * neither is a parameter of the other. The two coincided in every host there had ever been, because the
 * operation that makes them differ had no caller; it has one now, so `held` far below `made` is the ordinary
 * reading of a seam whose senders have finished, and `held == made` is a peer every one of whose senders is
 * still running. */
int world_segments_held(void);

/* WHOSE LIFETIME A FOREIGN SEGMENT HAS, AND THE TWO DEATHS THAT END IT.
 *
 * A segment is materialized by the RECEIVING instance, keyed on the SENDING flow's world, and it must survive
 * every later arrival that could name that world: the world itself (a flow posts twice), or any DESCENDANT of
 * it, since world_segment materializes a child by FORKING the nearest ancestor present. So the segment for `w`
 * is reachable exactly while some live flow of the minting instance has `w` on its world chain — `w` itself, or
 * `w` as a retired fork point above it. Only the MINTING instance can know that, which is why the release is a
 * notice and not a decision the holder takes.
 *
 * A LIVE FLOW'S WORLD IS NEVER RETIRED (world_mint_child retires the fork point and mints a child for BOTH
 * arms), so the chain is decidable from two facts per minted row and no walk: whether a live flow HOLDS it, and
 * how many of its children are still live. `w` dies when the flow holding it leaves the frontier, and every
 * ancestor whose LAST live descendant it was dies with it — one collapse up the chain, no refcount on the
 * segment, no ceiling on anything.
 *
 * BOTH RETURN THE NAMES A PEER MAY BE HOLDING A SEGMENT FOR — the ones that actually CROSSED — written in
 * world_serialize's own field grammar and borrowed until the next call. A world whose death has been announced
 * is no longer `sent`, so no death is announced twice and the two entry points cannot double-count. The caller
 * announces them (engine_notify_worlds_gone), because a notice is the host's line and not the registry's. */
int world_flow_gone(WorldId w, const char *const **names);

/* …AND THE SESSION'S OWN DEATH, which is not the sum of the flow deaths above. A park writes the frontier out
   as RECIPES and the resumed session mints in a disjoint generation (world_session), so every name this session
   ever put on the wire is unusable from that instant — while the peer that never left memory still holds a
   segment for each of them. Announced at the PARK and not at the teardown that follows it, because by the
   teardown the host has drained its last notices. */
int world_session_gone(const char *const **names);

/* The world is gone: release this instance's segment. Releasing a world with no segment is not an error — a
   world that never wrote here never had one, and the sender deliberately does not track which peers a flow
   reached, so a death is BROADCAST and this no-op is what makes that free. */
void world_release(JSContext *ctx, WorldId w);

#endif
