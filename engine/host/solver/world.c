/* The world registry — see world.h. */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "solver/world.h"

/* A LOCALLY MINTED WORLD and the fork edge that produced it. Kept as a flat array indexed by serial-1, because
   serials are dense and monotonic: a world's parent is one load, and the ancestry walk is a pointer chase over
   contiguous memory on the hottest path in the engine.
 *
 * `forked` AND `sent` ARE TWO FACTS ABOUT A WORLD'S LIFE, and each of them is what makes one of this file's
 * answers correct rather than nearly correct.
 *   - `forked` — a child was minted from it, so NO FLOW HOLDS IT any more (world.h, world_mint_child): a fork
 *     retires the fork point and mints a child for EACH arm. It is what makes an ancestry a chain of DEAD
 *     worlds, which is the whole reason a peer may fork an ancestor's segment: a segment nothing will write to
 *     again cannot leak one arm's later writes into the other's.
 *   - `sent` — this world has itself crossed the seam as the HEAD of a vector, which is the only way a peer can
 *     come to hold a segment keyed by it. An ancestor that never crossed is therefore an ancestor no peer can
 *     be holding, and naming it in a vector is a field every reader scans past. Retiring the fork point makes a
 *     flow's world change at EVERY branch it takes, so the unfiltered chain would grow with the number of
 *     branches rather than with the fork depth at birth — thousands of fields on a page whose boot flow forks
 *     freely, past the 512-byte record every sender writes into. Filtering is not a size optimisation: it is
 *     what keeps the chain O(this flow's cross-instance history), which is the only part of it that carries
 *     information.
 *   - `held` AND `live_kids` — the two facts a DEATH is decided from, and they are two rather than one because
 *     a world is reachable at a peer for two different reasons. `held` is "a live flow's world is this one",
 *     true from the mint and false the instant either the flow leaves the frontier or a fork retires it (a
 *     fork mints a child for BOTH arms, so a live flow's world is never a retired one). `live_kids` is how
 *     many of its children are still live, which is what makes a RETIRED world reachable: world_segment
 *     materializes a descendant by FORKING the nearest ancestor present, so an ancestor with a live
 *     descendant is a segment some future arrival will fork. A world is dead when neither holds, and its
 *     death releases one from its parent — the collapse in world_flow_gone, which is O(depth) and never a
 *     refcount on the segment itself. */
typedef struct {
    WorldId  parent;    /* WORLD_NONE for a root */
    bool     sent;      /* a peer may hold a segment keyed by it: it crossed as the head of a vector, and its
                           death has not been announced yet */
    bool     forked;    /* a child was minted from it: it is a retired fork point and names no flow */
    bool     held;      /* a live flow's world is this one */
    uint32_t live_kids; /* children that are still live */
} MintedWorld;

/* A SEGMENT this instance holds for a world minted somewhere ELSE. Sparse — only worlds that actually reached
   this instance appear — so a linear table indexed by nothing in particular is the right shape, and its length
   is the number of foreign worlds that touched this document rather than the sender's frontier size.
   `vector` IS ITS CROSS-TIER NAME — the wire form it was materialized from, kept because that is the segment's
   whole recipe (world.h, world_segments_park) and because the WorldId beside it cannot be written down: `doc`
   is this instance's handle into its own table and names a different document in a peer's. Kept at
   materialization rather than rebuilt at the park, so what crosses is the vector that actually built this
   segment and not one re-derived from edges only the MINTING instance holds.
   THE TABLE IS ORDERED BY MATERIALIZATION, which is a topological order over the fork edges: a segment forks
   the nearest ancestor PRESENT when it is built, so an ancestor is always earlier. The park emits in that
   order and the resume's forward pass then finds every ancestor already there — which is why world_release
   removes in place rather than swapping the tail down. */
typedef struct {
    WorldId   id;
    CowDelta *delta;
    char     *vector;
} ForeignSegment;

static uint32_t g_doc;
static uint32_t g_next_serial;
/* THIS SESSION'S GENERATION — see world.h. Zero is a real value (a document nobody has parked), which is why
   nothing here tests it for presence: what a name needs is that no OTHER session of this document mints it. */
static uint32_t g_session;

/* THE DOCUMENT NAME TABLE. A handle is an index+1, so 0 stays the NONE value; the table is append-only for the
   life of the instance because a handle already stored in a WindowProxy or a WorldId must never come to mean a
   different document. It is small by construction — one entry per document this instance has ever named. */
typedef struct { char *name; uint32_t next_child; bool hosted; JSContext *realm; } DocEntry;
static DocEntry *g_docs;
static uint32_t g_docs_n, g_docs_cap;

uint32_t world_doc_intern(const char *name)
{
    uint32_t i;

    DCHECK(name != NULL && *name, "a document with no name was interned — the name is what crosses the seam, so "
                                  "an empty one names every document at once");
    for (i = 0; i < g_docs_n; i++)
        if (!strcmp(g_docs[i].name, name)) return i + 1;
    if (g_docs_n == g_docs_cap) {
        uint32_t cap = g_docs_cap ? g_docs_cap * 2 : 8;
        DocEntry *g = realloc(g_docs, cap * sizeof *g);
        CHECK(g != NULL, "world registry: OOM recording a document name — a document whose name is lost can "
                         "never be routed to, so every read through it would park its flow forever");
        g_docs = g;
        g_docs_cap = cap;
    }
    memset(&g_docs[g_docs_n], 0, sizeof g_docs[g_docs_n]);
    g_docs[g_docs_n].name = strdup(name);
    CHECK(g_docs[g_docs_n].name != NULL, "world registry: OOM recording a document name");
    return ++g_docs_n;
}

const char *world_doc_name(uint32_t doc)
{
    DCHECK(doc != 0 && doc <= g_docs_n, "a document handle that names no document was serialized — a handle is "
                                        "this instance's index into its own name table and means nothing else");
    return g_docs[doc - 1].name;
}

bool world_doc_hosted(uint32_t doc)
{
    DCHECK(doc != 0 && doc <= g_docs_n, "whether this instance holds a document was asked of a handle that "
                                        "names no document");
    return g_docs[doc - 1].hosted;
}

void world_doc_adopt(uint32_t doc)
{
    DCHECK(doc != 0 && doc <= g_docs_n, "a realm was built for a document handle that names no document");
    DCHECK(!g_docs[doc - 1].hosted, "a realm was built twice for one document — a document has ONE realm, and a "
                                    "second would give the same name two globals with two object graphs");
    g_docs[doc - 1].hosted = true;
}

/* See world.h. The realm is BORROWED: a realm is kept alive by its own function objects and dies with its
   navigable (navigable.c), so a counted reference here would be an external root making every document this
   instance ever named immortal. */
void world_doc_realm_set(uint32_t doc, JSContext *realm)
{
    DCHECK(doc != 0, "a realm was recorded against the NONE document");
    /* THE TABLE MAY ALREADY BE GONE, AND ONLY FOR THE CLEAR. A host frees the world registry and then the
       runtime, so the agent's last realms are torn down after the table that named their documents — there is
       no row left to clear because every row went with the table. Stated for the NULL write alone: a realm
       ARRIVING after the registry is gone is a realm named by nothing, which is a real and different defect
       and still crashes below. */
    if (realm == NULL && g_docs_n == 0) return;
    DCHECK(doc <= g_docs_n, "a realm was recorded against a document handle that names no document — a handle "
                            "is this instance's index into its own name table and means nothing else");
    DCHECK(g_docs[doc - 1].hosted,
           "a realm was recorded for a document this agent does not HOLD — hosting is decided by §7.4 before "
           "the realm is built (world_doc_adopt), so the two statements were made in the wrong order and every "
           "cross-instance route keyed on `hosted` would still call this document a peer's");
    DCHECK(realm == NULL || g_docs[doc - 1].realm == NULL,
           "a SECOND realm was built for one document — a Document has one Window, so these are two of them "
           "wearing one name, and a peer routing on that name cannot tell which one it asked. It is the LAZY "
           "MATERIALIZATION meeting a FORK: proxy_realm builds the initial about:blank Document's realm through "
           "the PER-FLOW WindowProxy record (navigable.h), so two arms that each read through one srcless "
           "navigable each build one. The fix is where the second is made and not here — either the built realm "
           "is state the flow's delta carries, or the navigable is materialized once for all of its timelines");
    g_docs[doc - 1].realm = realm;
}

JSContext *world_doc_realm(uint32_t doc)
{
    DCHECK(doc != 0 && doc <= g_docs_n, "the realm of a document handle that names no document was asked for");
    DCHECK(g_docs[doc - 1].hosted,
           "the realm of a document this agent does not HOLD was asked for — that document lives in a peer "
           "instance, so it has no realm here and every read through it crosses the seam");
    return g_docs[doc - 1].realm;
}

uint32_t world_mint_doc(uint32_t parent)
{
    char buf[64];
    const char *self;

    DCHECK(parent != 0 && parent <= g_docs_n, "a document was created by a parent that names no document");
    DCHECK(g_doc != 0, "a document was created before world_registry_init named the one creating it");
    self = world_doc_name(parent);
    /* "<my name>.<n>" — unique by induction from the root name, which is what lets this be synchronous. */
    DCHECK(strlen(self) + 12 < sizeof buf,
           "a document name grew past this buffer — names nest one component per navigable depth, so this is a "
           "frame tree deeper than the buffer holds and the fix is to grow it, never to truncate a name into a "
           "collision with another document");
    snprintf(buf, sizeof buf, "%s.%u", self, ++g_docs[parent - 1].next_child);
    CHECK(g_docs[parent - 1].next_child != 0, "the child-document counter wrapped — the next name it mints collides with a "
                             "document that already exists, merging two documents into one identity");
    return world_doc_intern(buf);
}

static void world_segment_counts_reset(void);   /* defined with the counters it clears */
static void world_gone_reset(void);             /* defined with the buffer it empties */

static MintedWorld *g_minted;
static uint32_t     g_minted_n, g_minted_cap;

static ForeignSegment *g_segs;
static int             g_segs_n, g_segs_cap;

/* THE TWO GROWABLE BUFFERS THIS FILE OWNS — one per DIRECTION of the wire form, and neither has a capacity a
   caller states. A fixed cap on either is a bound §scheduler forbids and it is not the harmless kind: the write
   side truncates a chain (the peer forks a more distant ancestor and silently loses every write in between) and
   the read side used to DFAIL, which is a crash on a page whose flow simply branched more times than a stack
   array had room for. They grow and are freed with the registry. */
static WorldId *g_anc;      static int g_anc_n, g_anc_cap;
static WorldId *g_parsed;   static int g_parsed_cap;
/* AND THE THIRD, WHICH IS THE OUTBOUND HALF OF A DEATH. A collapse can free a whole chain at once and a park
   frees every world a session ever sent, so the count is the session's history and not a number a caller can
   state — the same reason neither buffer above takes a capacity. Each entry is owned here and freed at the
   start of the next call, which is what makes the borrow window in world.h a statement rather than a hope. */
static char **g_gone;       static int g_gone_n, g_gone_cap;
/* …AND THE FOURTH, WHICH OWNS NOTHING. world_segments_park hands out the vectors the SEGMENTS own, so this is
   an array of borrowed pointers and only the array is freed — one entry per foreign segment held, which is a
   number the park is about to write a record for each of anyway. */
static const char **g_carried; static int g_carried_cap;

void world_registry_init(const char *doc_name)
{
    uint32_t doc;

    DCHECK(doc_name != NULL && *doc_name, "the world registry was given no document name — the host names the "
                                          "root document because only it knows there is more than one");
    doc = world_doc_intern(doc_name);
    DCHECK(g_doc == 0 || g_doc == doc, "the world registry was re-initialised with a different document name "
                                       "— one instance is one document, and its worlds are named by it");
    g_doc = doc;
    /* THE ROOT'S REALM IS THIS INSTANCE'S, by definition: the host named this document because it is the one it
       built the agent for. Every other document is hosted only once a realm has been built for it. */
    if (!g_docs[doc - 1].hosted) world_doc_adopt(doc);
    g_next_serial = 1;
    /* THE GENERATION IS NOT ASSIGNED HERE, AND THAT IS THE STATEMENT. A document reaching this line for the
       first time is generation 0 — the host minted its name for THIS load, so it can collide with nothing —
       and a RESUMED one is handed its namespace later, by the residue (world_session_resume). Re-initialising
       after that would silently put it back to the ended session's names, so it is asserted rather than set. */
    DCHECK(g_session == 0,
           "the world registry was re-initialised after a resumed session had installed its generation — every "
           "world minted from here would carry the names of the session that parked, which a peer that never "
           "left memory already keys its segments on");
}

uint32_t world_session(void)
{
    DCHECK(g_doc != 0, "this instance's generation was asked for before world_registry_init named its document "
                       "— a generation is one coordinate of a name and there is nothing here yet to name");
    return g_session;
}

void world_session_resume(uint32_t prev)
{
    DCHECK(g_doc != 0, "a resumed generation was installed before world_registry_init named this document");
    DCHECK(g_minted_n == 0,
           "a generation was installed after this session had already minted a world — every name minted "
           "before it carries the ENDED session's generation, so a peer that never left memory answers those "
           "flows out of segments the session that parked already owns. The generation record is the FIRST in "
           "a park document precisely so this cannot happen");
    DCHECK(g_session == 0, "a session's generation was installed twice — a session has one, and the second "
                           "would rename every world minted after it");
    g_session = prev + 1;
    DCHECK(g_session != 0, "the world generation counter wrapped — every name this session mints collides with "
                           "a session a peer may still hold segments for, merging two timelines");
}

void world_registry_free(JSContext *ctx)
{
    int i;
    for (i = 0; i < g_segs_n; i++) {
        cow_delta_release(ctx, g_segs[i].delta);
        free(g_segs[i].vector);   /* the segment's cross-tier name is the segment's, and dies with it */
    }
    free(g_carried); g_carried = NULL; g_carried_cap = 0;
    free(g_segs);
    g_segs = NULL;
    g_segs_n = g_segs_cap = 0;
    free(g_minted);
    g_minted = NULL;
    g_minted_n = g_minted_cap = 0;
    /* AND THE TWO WIRE-FORM BUFFERS, which belong to the registry for the reason the counts below do: a host
       that runs several documents in one process would otherwise carry one instance's scratch into the next. */
    free(g_anc);    g_anc = NULL;    g_anc_n = g_anc_cap = 0;
    free(g_parsed); g_parsed = NULL; g_parsed_cap = 0;
    world_gone_reset();
    free(g_gone);   g_gone = NULL;   g_gone_cap = 0;
    for (i = 0; i < (int)g_docs_n; i++) free(g_docs[i].name);
    free(g_docs);
    g_docs = NULL;
    g_docs_n = g_docs_cap = 0;
    g_doc = 0;
    g_next_serial = 0;
    g_session = 0;
    /* THE COUNTS GO WITH THE REGISTRY, because they say what THIS instance's seam did and a host that runs
       several documents in one process (the native WPT runner) would otherwise report the previous one's. */
    world_segment_counts_reset();
}

static WorldId mint(WorldId parent)
{
    WorldId w;

    DCHECK(g_doc != 0, "a world was minted before world_registry_init named this document");
    if (g_minted_n == g_minted_cap) {
        uint32_t cap = g_minted_cap ? g_minted_cap * 2 : 64;
        MintedWorld *g = realloc(g_minted, cap * sizeof *g);
        CHECK(g != NULL, "world registry: OOM recording a world — a world whose ancestry is lost can never be "
                         "materialized by a peer, which silently drops that flow's writes in another document");
        g_minted = g;
        g_minted_cap = cap;
    }
    memset(&g_minted[g_minted_n], 0, sizeof g_minted[g_minted_n]);
    g_minted[g_minted_n].parent = parent;
    /* MINTED FOR A FLOW, ALWAYS — mint() is reached from flow_add and from the fork's re-mint of the arm that
       keeps running, and nothing else may mint one. So a row is born HELD, and world_flow_gone is the only
       thing that clears it without retiring the row. */
    g_minted[g_minted_n].held = true;
    g_minted_n++;
    /* THE PARENT IS RETIRED BY ITS FIRST CHILD. Said here rather than in world_mint_child because this is the
       one place a fork edge is recorded, and the fact is about the EDGE: a world with a child is a world some
       flow branched at, and both arms of that branch are children of it (world.h). A root has no parent. */
    if (!world_is_none(parent)) {
        MintedWorld *p = &g_minted[parent.serial - 1];
        /* A DEAD WORLD CANNOT BE BRANCHED AT. The flow doing the branching holds `parent`, so this is the same
           statement world_flow_gone makes from the other side, asserted where the edge is built: a fork off a
           world whose death was already announced would hand every peer a child whose nearest ancestor is a
           segment they have already released. */
        DCHECK(p->held || p->live_kids > 0,
               "a world was forked from a DEAD one — the flow that branched does not hold it and no descendant "
               "of it is live, so its death has already been announced and every peer has released the segment "
               "this child's ancestry tells them to fork");
        p->forked = true;
        p->held = false;   /* retired: it names no flow, and both arms of the branch are children of it */
        p->live_kids++;
    }

    w.doc = g_doc;
    /* THE GENERATION IS STAMPED HERE and nowhere else, for the reason the document is: a name is composed at
       the one point a name is made, so a session cannot mint under another session's namespace. */
    w.session = g_session;
    w.serial = g_next_serial++;
    /* A REUSED SERIAL IS A MERGED TIMELINE. A peer keys its segment by this name, so a wrapped counter hands
       one flow another flow's writes in another document — silently, and only in the multi-document case. */
    DCHECK(g_next_serial != 0, "the world serial counter wrapped — every id it mints from here collides with a "
                               "world a peer already holds a segment for, merging two flows' timelines");
    DCHECK(w.serial == g_minted_n, "the world serial and the minted table disagree — the ancestry walk indexes "
                                   "the table by serial, so they are the same number by construction");
    return w;
}

uint32_t world_local_doc(void)
{
    DCHECK(g_doc != 0, "this document's id was asked for before world_registry_init named it — a component that "
                       "needs to know whether a navigable is remote cannot be answered before then");
    return g_doc;
}

WorldId world_mint(void) { return mint(WORLD_NONE); }

WorldId world_mint_child(WorldId parent)
{
    DCHECK(parent.doc == g_doc,
           "a world minted in ANOTHER document was forked here — a fork mirrors into the instance that owns the "
           "parent, so the peer holding that world is the one that must mint the child and send it back");
    DCHECK(parent.session == g_session,
           "a world minted in a PREVIOUS session of this document was forked here — that name belongs to a "
           "timeline that ended with its session, and a peer still keys a segment on it, so the child would "
           "inherit a dead flow's writes at every peer that has one");
    DCHECK(parent.serial != 0 && parent.serial <= g_minted_n, "a world was forked from a name never minted here");
    return mint(parent);
}

static WorldId *world_buf_room(WorldId **buf, int *cap, int n)
{
    if (n == *cap) {
        int c = *cap ? *cap * 2 : 16;
        WorldId *g = realloc(*buf, (size_t)c * sizeof *g);
        CHECK(g != NULL, "world registry: OOM growing a world vector — a vector that loses an ancestor makes "
                         "the peer fork a more distant one and silently drops every write in between");
        *buf = g;
        *cap = c;
    }
    return *buf;
}

/* THE ANCESTRY OF A LOCALLY-MINTED WORLD, nearest first, borrowed into `*out` and valid until the next call;
   returns how many were written. Asserts the world was minted here — a world minted elsewhere has an ancestry
   only its own instance can answer for. INTERNAL: world_serialize is the one writer of the wire form (world.h),
   so this is its input and never a second way to produce one.
 *
 * TWO INVARIANTS ARE STATED HERE BECAUSE THIS IS WHERE THE CHAIN IS WALKED, and each of them was silently false
 * for as long as a fork left the primary arm holding the fork point's name:
 *   - THE HEAD NAMES A LIVE FLOW. A retired fork point is a world no flow holds, so serializing one means a
 *     flow's world was never re-minted at its branch and a peer is about to be handed the name of a timeline
 *     that ended at the branch.
 *   - EVERY ANCESTOR IS RETIRED. A world becomes a parent only by being forked, so this holds by construction —
 *     and it is the whole reason world_segment may FORK an ancestor's segment. A live ancestor would keep
 *     receiving writes after the peer forked it, and whether those writes reached the other arm would depend on
 *     the order the two arms happened to arrive: two timelines wearing one name, which is exactly what this
 *     registry exists to prevent. */
static int world_ancestry(WorldId w, WorldId **out)
{
    DCHECK(w.doc == g_doc, "the ancestry of a world minted in another document was asked for here — only the "
                           "minting instance holds its fork edges, so the request must carry it");
    DCHECK(w.session == g_session,
           "the ancestry of a world minted in a PREVIOUS session of this document was asked for here — the "
           "fork edges below are this session's table, so the chain would be one session's edges walked under "
           "another session's name");
    DCHECK(w.serial != 0 && w.serial <= g_minted_n, "the ancestry of a world never minted here was asked for");
    DCHECK(!g_minted[w.serial - 1].forked,
           "a RETIRED FORK POINT was serialized as a flow's world — a fork mints a child for BOTH arms and "
           "leaves this name naming neither, so whatever holds it stopped existing at that branch, and the peer "
           "would key a segment on a timeline that ended");
    /* AND A DEAD WORLD IS NEVER READ AGAIN, which is the invariant the release exists inside. A world whose
       flow has left the frontier had its death ANNOUNCED to every peer (world_flow_gone), so a peer has
       released the segment this name keys — and serializing it again would make that peer materialize a
       SECOND, empty segment under a name it has been told is gone, silently losing every write the first one
       held. It is a different failure from the retired one above and so a different assert: retired means the
       flow branched, this means the flow ended. */
    DCHECK(g_minted[w.serial - 1].held,
           "a world NO LIVE FLOW HOLDS was serialized — the flow that held it left the frontier, so its death "
           "has been announced and every peer has released its segment; a vector naming it now makes them "
           "materialize a fresh empty one under a name that is gone");
    /* AND THIS WORLD HAS NOW CROSSED, which is the fact the filter below reads. Recorded at the one point a
       vector is produced, so a world can only be called `sent` by having been sent. */
    g_minted[w.serial - 1].sent = true;
    g_anc_n = 0;
    /* NEAREST FIRST, which is the order world_segment's scan depends on: the parent, then the grandparent, so
       a peer holding both forks the parent and keeps its writes. */
    for (w = g_minted[w.serial - 1].parent; !world_is_none(w); w = g_minted[w.serial - 1].parent) {
        DCHECK(w.doc == g_doc && w.serial <= g_minted_n, "a fork edge names a world outside this document's "
                                                         "minted table — the chain is corrupt");
        DCHECK(g_minted[w.serial - 1].forked,
               "an ANCESTOR of a world is not a retired fork point — a world becomes a parent only by being "
               "forked, so this chain was built by something that is not mint()");
        /* …AND IT IS ALIVE, which is what makes it safe to name. An ancestor of a live world has a live
           descendant BY CONSTRUCTION — the one this chain is being walked for — so a dead one here means the
           collapse in world_flow_gone released a link it should not have, and the peer is about to be told to
           fork a segment it has already dropped. */
        DCHECK(g_minted[w.serial - 1].live_kids > 0,
               "an ANCESTOR named in a world vector is DEAD — the world being serialized is its descendant, so "
               "it has a live descendant by construction; a dead one means a death was announced for a world "
               "still on a live flow's chain and the peer has released the segment this vector forks from");
        /* A WORLD THAT NEVER CROSSED CANNOT BE HELD BY ANY PEER, so naming it would put a field in the vector
           that every reader scans past. Dropping it loses nothing: world_segment keys its table on the HEAD of
           a vector it received, and a head is a world this instance called `sent`. */
        if (!g_minted[w.serial - 1].sent) continue;
        /* THE ROOM AND THE STORE ARE TWO STATEMENTS, because `room(&b,&c,n)[n++] = w` leaves the read of `n`
           inside the call unsequenced against the increment beside it. */
        {
            WorldId *buf = world_buf_room(&g_anc, &g_anc_cap, g_anc_n);
            buf[g_anc_n++] = w;
        }
    }
    *out = g_anc;
    return g_anc_n;
}

/* ONE FIELD OF THE WIRE FORM — "<document name>:<generation>:<serial>" — AND THE ONE PLACE IT IS WRITTEN. The
   head of a vector, each of its ancestors and the name a DEATH announces all go through here, so a world has
   exactly one spelling; the alternative is the defect world_parse's own comment names from the other side, two
   readers of one grammar being two grammars. The DOCUMENT is named rather than numbered because a `uint32_t
   doc` is this instance's handle into its own table and means a different document in a peer's. */
static int world_name_write(WorldId w, char *dst, size_t cap)
{
    int n = snprintf(dst, cap, "%s:%u:%u", world_doc_name(w.doc), w.session, w.serial);
    CHECK(n > 0 && (size_t)n < cap, "a world NAME did not fit its buffer — a truncated name is a different "
                                    "world, so the peer keys a segment on a timeline nobody is in");
    return n;
}

/* THE VECTOR ITSELF — a head and an ancestry, in the grammar world_name_write spells one field of. It is split
   out of world_serialize because a vector is produced from an ancestry this instance WALKED (a local flow's,
   below) and from one it was HANDED (a foreign segment's, kept as that segment's cross-tier name), and two
   spellings of the join would be two grammars — the defect world_parse's own comment names from the other
   side. The walk stays inside world_serialize, which is what keeps world.h's "the ancestry walk is not
   exported" true. */
static int world_vector_write(WorldId w, const WorldId *anc, int n_anc, char *dst, size_t cap)
{
    int k, n;

    DCHECK(dst != NULL && cap > 0, "a world was serialized into no buffer");
    DCHECK(n_anc == 0 || anc != NULL, "a world vector was written with an ancestor count and no ancestors");
    n = world_name_write(w, dst, cap);
    for (k = 0; k < n_anc; k++) {
        CHECK((size_t)n + 1 < cap,
              "the world vector did not fit its buffer — a truncated vector makes the peer fork a more distant "
              "ancestor and silently lose the nearer writes");
        dst[n++] = ',';
        n += world_name_write(anc[k], dst + n, cap - (size_t)n);
    }
    return n;
}

/* …AND THE SAME VECTOR INTO A BUFFER THIS FILE OWNS, sized from the NAMES rather than from a constant, for
   world_gone_push's reason: a document name nests one component per navigable depth, so any constant here is a
   cap on the frame tree. The rest of a field is two colons, two full-width uint32 decimals, the separating
   comma and the NUL. */
static char *world_vector_alloc(WorldId w, const WorldId *anc, int n_anc)
{
    size_t cap = strlen(world_doc_name(w.doc)) + 23;
    int i;
    char *s;

    for (i = 0; i < n_anc; i++) cap += strlen(world_doc_name(anc[i].doc)) + 24;
    s = malloc(cap);
    CHECK(s != NULL, "world registry: OOM naming a foreign segment — a segment whose NAME is lost cannot cross "
                     "the cold tier, so the instance holding it can never park and the peer flow's state in "
                     "this document is stranded here for the rest of the process");
    world_vector_write(w, anc, n_anc, s, cap);
    return s;
}

/* See world.h. */
int world_serialize(WorldId w, char *dst, size_t cap)
{
    WorldId *anc;
    int n_anc = world_ancestry(w, &anc);

    return world_vector_write(w, anc, n_anc, dst, cap);
}

/* THE INVERSE, AND IT LIVES HERE BECAUSE A FORMAT WITH TWO READERS HAS TWO FORMATS. Every host that received a
   world vector used to take it apart by hand — strchr for the comma, strrchr for the colon, strtoul for the
   serial — which is the serializer's grammar restated somewhere it cannot be checked against the writer. The
   document NAME is what crosses (a handle means nothing to a peer), so interning is part of reading it, and the
   nearest-first ORDER is load-bearing: the reader forks the first ancestor it holds, so an order this function
   got wrong would silently fork a more distant one and lose the nearer writes. Destructive on a copy of its own
   making, so the caller's record is untouched. */
int world_parse(const char *s, WorldId *out, const WorldId **ancestry)
{
    char *dup, *q;
    int n_anc = 0;

    DCHECK(s != NULL && *s, "a world vector was parsed from nothing — the answer would be true in no timeline");
    DCHECK(out != NULL && ancestry != NULL, "a world vector was parsed into no world");
    *out = WORLD_NONE;
    dup = strdup(s);
    CHECK(dup != NULL, "world: OOM parsing a world vector");
    q = dup;
    while (q && *q) {
        char *comma = strchr(q, ','), *colon, *gen;
        if (comma) *comma = 0;
        /* THE LAST TWO colons: a document name is minted as "<parent>.<n>" and a host may name the root
           anything, so only the serial's and the generation's separators are known to be final. */
        colon = strrchr(q, ':');
        /* EVERY FIELD THE WRITER WROTE IS A WORLD, so a field with no serial is not one to skip — skipping it
           drops an ancestor, which is the same silent loss a truncated vector causes: the reader forks
           the next ancestor it holds and every write in between goes with it. world_serialize emits
           "<doc>:<session>:<serial>" for the head and for each ancestor, so there is no field this can
           legitimately be. */
        DCHECK(colon != NULL, "a world vector field carried no serial — world_serialize writes "
                              "<doc>:<session>:<serial> for every field, so this vector was not written by it, "
                              "and reading past the field would silently drop the ancestor it names");
        {
            WorldId id;
            *colon = 0;
            /* THE GENERATION IS READ WHERE THE SERIAL IS, because it is part of the name rather than a header
               (world.h). A field short of one is a vector from a writer that does not have generations — and
               its names would land on THIS session's, which is the exact collision they exist to prevent. */
            gen = strrchr(q, ':');
            DCHECK(gen != NULL, "a world vector field carried no GENERATION — a name without one belongs to "
                                "every session of the document that minted it at once, so this instance would "
                                "key a segment for a parked session's flow on a live one's name");
            id.session = gen ? (uint32_t)strtoul(gen + 1, NULL, 10) : 0;
            if (gen) *gen = 0;
            id.doc = world_doc_intern(q);
            id.serial = (uint32_t)strtoul(colon + 1, NULL, 10);
            if (world_is_none(*out)) {
                *out = id;
            } else {
                WorldId *buf = world_buf_room(&g_parsed, &g_parsed_cap, n_anc);
                buf[n_anc++] = id;
            }
        }
        q = comma ? comma + 1 : NULL;
    }
    free(dup);
    /* PUBLISHED AFTER THE WALK, because the buffer MOVES: every push may realloc, so a pointer handed out
       before the last field names freed memory. */
    *ancestry = g_parsed;
    /* A VECTOR THAT NAMES NO WORLD IS TRUE IN NO TIMELINE — asserted where the name arrives rather than where
       the answer is used, because by then it is indistinguishable from whatever was last installed. */
    DCHECK(!world_is_none(*out), "a world vector named no world — its segment would come from whatever this "
                                 "instance last installed, which is a different timeline wearing one name");
    {
        int k;
        for (k = 0; k < n_anc; k++)
            DCHECK((*ancestry)[k].doc == out->doc && (*ancestry)[k].session == out->session,
                   "a world vector's ancestry names a world from another document or another SESSION than its "
                   "own — world_ancestry walks only the edges the minting instance holds, and those are one "
                   "session's, so a mixed chain means the vector was mis-parsed and this instance would fork "
                   "the wrong segment");
    }
    return n_anc;
}

static ForeignSegment *find_segment(WorldId w)
{
    int i;
    for (i = 0; i < g_segs_n; i++)
        if (world_eq(g_segs[i].id, w))
            return &g_segs[i];
    return NULL;
}

bool world_has_segment(WorldId w) { return find_segment(w) != NULL; }

static int g_seg_made, g_seg_forked;
void world_segment_stats(int *materialized, int *forked)
{
    if (materialized) *materialized = g_seg_made;
    if (forked) *forked = g_seg_forked;
}
static void world_segment_counts_reset(void) { g_seg_made = g_seg_forked = 0; }

int world_segments_held(void)
{
    /* HELD IS BOUNDED BY MATERIALIZED, which is the one relation the two numbers have and the only thing that
       can be checked about a table against its own history. Every entry is put here by world_segment, which
       counts as it does so, and world_release only ever removes one — so a table that has outgrown the count of
       times anything was put in it was written by something that is not this file. */
    DCHECK(g_segs_n <= g_seg_made,
           "this instance holds more foreign world segments than it has ever materialized — the table is grown "
           "in exactly one place and counted there, so a live count above the history means a segment was "
           "installed by something other than world_segment and nothing owns it");
    return g_segs_n;
}

CowDelta *world_segment(JSContext *ctx, WorldId w, const WorldId *ancestry, int n_anc)
{
    ForeignSegment *s = find_segment(w);
    CowDelta *d;
    int i;

    DCHECK(g_doc != 0, "a foreign world's segment was asked for before world_registry_init named this document");
    /* A LOCAL FLOW USES ITS OWN DELTA, not this table. Routing one through here would give it two segments in
       one instance, and the one the scheduler swaps would not be the one a cross-document write captured into. */
    DCHECK(w.doc != g_doc, "a world minted in THIS document was looked up as a foreign one — a local flow's "
                           "delta is the flow's own, and a second segment for it is a second timeline");
    DCHECK(!world_is_none(w), "the NONE world was asked for a segment");
    if (s) return s->delta;

    /* MATERIALIZE. The nearest ancestor present wins; the scan relies on world_ancestry's nearest-first order,
       which is why that order is asserted where it is produced rather than assumed here. */
    d = NULL;
    for (i = 0; i < n_anc; i++) {
        ForeignSegment *a = find_segment(ancestry[i]);
        if (a) {
            /* O(1): the parent's head freezes into a shared immutable base segment both now reference. Whether
               that ancestor is the delta the heap is showing depends on the host — a routed delivery arrives
               with the RECEIVING document's own timeline applied, while the WPT host's read loop leaves the
               previously-read world's segment installed and the ancestor may be exactly it — so the fork asks
               rather than being told. Told, one of those two callers would be wrong every time, silently. */
            d = cow_delta_fork(ctx, a->delta);
            g_seg_forked++;
            break;
        }
    }
    /* NONE OF THE ANCESTRY IS HERE, so this world has never written in this document and starts from its
       baseline. That is the truth, not a fallback: an empty delta over the baseline IS what "wrote nothing
       here" means, and inventing anything else would fabricate state the flow never produced. */
    if (!d) d = cow_delta_new();
    CHECK(d != NULL, "world registry: OOM materializing a segment — a dropped segment silently reverts another "
                     "document's writes for this flow");

    if (g_segs_n == g_segs_cap) {
        int cap = g_segs_cap ? g_segs_cap * 2 : 16;
        ForeignSegment *g = realloc(g_segs, (size_t)cap * sizeof *g);
        /* WHAT THIS TABLE IS SIZED BY, NAMED WHERE THE ALLOCATION IS. One entry per foreign world that has a
           LIVE flow behind it — a sender announces each of its worlds as it dies and the announcement is what
           removes the entry (world.h) — so the length here is the peers' live cross-instance frontier, per
           SESSION of the sender, and not the history of everything that ever arrived. A reader standing at
           this allocation needs that, because "OOM" alone sends them looking at the page. */
        CHECK(g != NULL, "world registry: OOM growing the segment table — one entry per foreign world with a "
                         "live flow behind it, per SESSION of the sender; a dropped entry silently reverts "
                         "another document's writes for that flow");
        g_segs = g;
        g_segs_cap = cap;
    }
    g_segs[g_segs_n].id = w;
    g_segs[g_segs_n].delta = d;
    /* ITS CROSS-TIER NAME, TAKEN HERE BECAUSE HERE IS WHERE IT IS TRUE. The vector this segment was
       materialized from is the operation that produced it, and world_segment is a pure function of it — so
       keeping it is keeping the recipe (world.h). Re-deriving it at the park is not available: the ancestry
       belongs to the MINTING instance's fork edges and this one holds none of them. */
    g_segs[g_segs_n].vector = world_vector_alloc(w, ancestry, n_anc);
    g_segs_n++;
    g_seg_made++;
    return d;
}

/* See world.h. */
int world_segments_park(const char *const **vectors)
{
    int i;

    DCHECK(vectors != NULL, "the foreign segments this instance carries across the tier were asked for into "
                            "nothing");
    if (g_segs_n > g_carried_cap) {
        const char **g = realloc(g_carried, (size_t)g_segs_n * sizeof *g);
        CHECK(g != NULL, "world registry: OOM naming the foreign segments a park has to carry — a name dropped "
                         "here is a peer flow's state in this document dropped at the boundary that was to "
                         "save it");
        g_carried = g;
        g_carried_cap = g_segs_n;
    }
    for (i = 0; i < g_segs_n; i++) {
        /* THE ONE THING THE VECTOR CANNOT SAY, ASSERTED WHERE THE VECTOR IS HANDED OVER. A segment's writes are
           produced by page code this instance ran UNDER a foreign world, which is the JOIN engine.c names at
           all four arrival sites and which does not exist yet — so today a segment IS its vector and the
           replay rebuilds it exactly. The day the join lands, those writes came from the peer's RECORDS and
           the recipe is the ordered log of them; this line is where that has to be built rather than where a
           park quietly writes a name and loses the state under it. */
        DCHECK(cow_delta_empty(g_segs[i].delta),
               "a foreign world's segment holds WRITES, and a park would carry only its NAME — the vector "
               "rebuilds an EMPTY segment in the resumed session, so everything this instance did under that "
               "peer's world is dropped at the tier that exists to save it. The writes came from the peer's "
               "records (the JOIN engine_route names is what puts them here), so the recipe is the ordered LOG "
               "of the records performed under this world, re-performed at resume — build that, and carry it "
               "beside the vector");
        DCHECK(g_segs[i].vector != NULL && *g_segs[i].vector,
               "a foreign segment has no cross-tier name — it is taken at materialization, which is the only "
               "moment the ancestry that built it is in this instance's hands");
        g_carried[i] = g_segs[i].vector;
    }
    *vectors = (const char *const *)g_carried;
    return g_segs_n;
}

static void world_gone_push(WorldId w)
{
    /* SIZED FROM THE NAME, never from a fixed buffer: a document name is "<root>.<n>" nested one component per
       navigable depth over whatever the host called the root, so any constant here is a cap on the frame tree.
       The rest of a field is two colons, two full-width uint32 decimals and the NUL — 2 + 20 + 1 = 23 — and
       world_name_write's own CHECK is what makes that arithmetic checkable rather than eyeballed. */
    const char *dn = world_doc_name(w.doc);
    size_t cap = strlen(dn) + 23;
    char *s = malloc(cap);

    CHECK(s != NULL, "world registry: OOM naming a world that has died — a death no peer is told about is a "
                     "segment that instance holds for the rest of its process, for a flow that is gone");
    world_name_write(w, s, cap);
    if (g_gone_n == g_gone_cap) {
        int c = g_gone_cap ? g_gone_cap * 2 : 16;
        char **g = realloc(g_gone, (size_t)c * sizeof *g);
        CHECK(g != NULL, "world registry: OOM collecting the worlds that have died");
        g_gone = g;
        g_gone_cap = c;
    }
    g_gone[g_gone_n++] = s;
    /* AND IT IS NO LONGER SENT, which is what `sent` means: a peer MAY hold a segment keyed by this name. Once
       the death is on its way, none does. It is what makes the two entry points below unable to announce one
       world twice — a park announces every live sent world, and the teardown that follows it then finds
       nothing left to say. */
    g_minted[w.serial - 1].sent = false;
}

static void world_gone_reset(void)
{
    int i;
    for (i = 0; i < g_gone_n; i++) free(g_gone[i]);
    g_gone_n = 0;
}

/* See world.h. */
int world_flow_gone(WorldId w, const char *const **names)
{
    DCHECK(names != NULL, "the worlds a departing flow killed were asked for into nothing");
    DCHECK(w.doc == g_doc, "a flow of ANOTHER document left this frontier — a world is minted by the instance "
                           "that created the flow, so a flow here holds a world named by this document");
    DCHECK(w.session == g_session,
           "a flow holding a world of a PREVIOUS session left this frontier — the fork edges below are this "
           "session's table, so the collapse would walk one session's chain under another session's name");
    DCHECK(w.serial != 0 && w.serial <= g_minted_n, "a flow held a world never minted here");
    world_gone_reset();

    DCHECK(g_minted[w.serial - 1].held,
           "the world of a departing flow is not HELD — either the flow's world was retired under it (a fork "
           "retires the fork point and re-mints BOTH arms, so a live flow never holds a retired name) or this "
           "flow has already been released once and the collapse below would free a second link off its "
           "ancestors' counts");
    g_minted[w.serial - 1].held = false;

    /* THE COLLAPSE. A world dies when no flow holds it and no child of it is live; its death releases exactly
       one link from its parent, which may then die too. It runs up the chain and stops at the first ancestor
       that is still alive — which is the ordinary case, since the other arm of the branch is usually still
       running — so this is O(the dead prefix) and not a walk of the table. */
    for (;;) {
        MintedWorld *m = &g_minted[w.serial - 1];
        WorldId parent;

        if (m->held || m->live_kids > 0) break;
        if (m->sent) world_gone_push(w);
        parent = m->parent;
        if (world_is_none(parent)) break;
        DCHECK(g_minted[parent.serial - 1].live_kids > 0,
               "a dying world's parent has no live children to lose — the child being collapsed IS one of "
               "them, so the count and the edges disagree and some other descendant's death has already been "
               "announced twice");
        g_minted[parent.serial - 1].live_kids--;
        w = parent;
    }
    *names = (const char *const *)g_gone;
    return g_gone_n;
}

/* See world.h. */
int world_session_gone(const char *const **names)
{
    uint32_t i;

    DCHECK(names != NULL, "the worlds a parked session killed were asked for into nothing");
    DCHECK(g_doc != 0, "a session announced its worlds before world_registry_init named its document");
    world_gone_reset();
    /* EVERY WORLD THAT CROSSED, whatever its liveness here: a park makes the whole GENERATION unusable, so a
       world with live flows behind it is exactly as unreachable as one whose flow finished. `held` and
       `live_kids` are deliberately left alone — the flows still exist until the teardown, and their own
       release then finds nothing left to announce because world_gone_push cleared `sent`. */
    for (i = 0; i < g_minted_n; i++) {
        if (!g_minted[i].sent) continue;
        {
            WorldId w;
            w.doc = g_doc;
            w.session = g_session;
            w.serial = i + 1;
            world_gone_push(w);
        }
    }
    *names = (const char *const *)g_gone;
    return g_gone_n;
}

void world_release(JSContext *ctx, WorldId w)
{
    ForeignSegment *s;

    /* THE SAME STATEMENT world_segment MAKES AT THE OTHER END: this table holds FOREIGN worlds only. A local
       flow's delta is the flow's own, so a release naming one would be a peer claiming a timeline of this
       document had ended — which nothing but this instance can know. */
    DCHECK(w.doc != g_doc, "a world minted in THIS document was released as a foreign one — a local flow's "
                           "delta belongs to the flow, and this table has never held it");
    DCHECK(!world_is_none(w), "the NONE world was released");
    s = find_segment(w);
    /* A WORLD THAT NEVER WROTE HERE NEVER HAD A SEGMENT, and the sender cannot know which peers a flow reached
       — it would have to track that to avoid telling one, which is state kept only to avoid a no-op. It is
       also what makes the BROADCAST the trusted zone performs free: every instance but the one holding a
       segment does nothing. */
    if (!s) return;
    cow_delta_release(ctx, s->delta);
    free(s->vector);
    /* REMOVED IN PLACE, NOT SWAPPED DOWN FROM THE TAIL. The table's order is MATERIALIZATION order and that is
       load-bearing now that a park emits it: a segment forks the nearest ancestor present when it is built, so
       an ancestor is always earlier, and a swap-remove would move a descendant above its own ancestor. The
       resumed session's forward pass would then rebuild the descendant from the baseline — silently, with the
       ancestor's writes lost — which is the same loss a truncated vector causes, arriving from the other side.
       memmove and not a swap costs one linear pass on a table every lookup already scans linearly. */
    memmove(s, s + 1, (size_t)(&g_segs[g_segs_n] - (s + 1)) * sizeof *s);
    g_segs_n--;
}
