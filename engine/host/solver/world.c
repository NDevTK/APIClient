/* The world registry — see world.h. */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "solver/world.h"

/* A LOCALLY MINTED WORLD and the fork edge that produced it. Kept as a flat array indexed by serial-1, because
   serials are dense and monotonic: a world's parent is one load, and the ancestry walk is a pointer chase over
   contiguous memory on the hottest path in the engine. */
typedef struct {
    WorldId parent;   /* WORLD_NONE for a root */
} MintedWorld;

/* A SEGMENT this instance holds for a world minted somewhere ELSE. Sparse — only worlds that actually reached
   this instance appear — so a linear table indexed by nothing in particular is the right shape, and its length
   is the number of foreign worlds that touched this document rather than the sender's frontier size. */
typedef struct {
    WorldId   id;
    CowDelta *delta;
} ForeignSegment;

static uint32_t g_doc;
static uint32_t g_next_serial;

/* THE DOCUMENT NAME TABLE. A handle is an index+1, so 0 stays the NONE value; the table is append-only for the
   life of the instance because a handle already stored in a WindowProxy or a WorldId must never come to mean a
   different document. It is small by construction — one entry per document this instance has ever named. */
typedef struct { char *name; uint32_t next_child; bool hosted; } DocEntry;
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

static MintedWorld *g_minted;
static uint32_t     g_minted_n, g_minted_cap;

static ForeignSegment *g_segs;
static int             g_segs_n, g_segs_cap;

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
}

void world_registry_free(JSContext *ctx)
{
    int i;
    for (i = 0; i < g_segs_n; i++)
        cow_delta_free(ctx, g_segs[i].delta);
    free(g_segs);
    g_segs = NULL;
    g_segs_n = g_segs_cap = 0;
    free(g_minted);
    g_minted = NULL;
    g_minted_n = g_minted_cap = 0;
    for (i = 0; i < (int)g_docs_n; i++) free(g_docs[i].name);
    free(g_docs);
    g_docs = NULL;
    g_docs_n = g_docs_cap = 0;
    g_doc = 0;
    g_next_serial = 0;
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
    g_minted[g_minted_n].parent = parent;
    g_minted_n++;

    w.doc = g_doc;
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
    DCHECK(parent.serial != 0 && parent.serial <= g_minted_n, "a world was forked from a name never minted here");
    return mint(parent);
}

/* THE ANCESTRY OF A LOCALLY-MINTED WORLD, nearest first, written into `out` (at most `cap`); returns how many
   were written. Asserts the world was minted here — a world minted elsewhere has an ancestry only its own
   instance can answer for. INTERNAL: world_serialize is the one writer of the wire form (world.h), so this is
   its input and never a second way to produce one. */
static int world_ancestry(WorldId w, WorldId *out, int cap)
{
    int n = 0;

    DCHECK(w.doc == g_doc, "the ancestry of a world minted in another document was asked for here — only the "
                           "minting instance holds its fork edges, so the request must carry it");
    DCHECK(w.serial != 0 && w.serial <= g_minted_n, "the ancestry of a world never minted here was asked for");
    /* NEAREST FIRST, which is the order world_segment's scan depends on: the parent, then the grandparent, so
       a peer holding both forks the parent and keeps its writes. */
    for (w = g_minted[w.serial - 1].parent; !world_is_none(w); w = g_minted[w.serial - 1].parent) {
        DCHECK(w.doc == g_doc && w.serial <= g_minted_n, "a fork edge names a world outside this document's "
                                                         "minted table — the chain is corrupt");
        if (n < cap) out[n] = w;
        n++;
        /* The caller's buffer being too small would TRUNCATE the chain, and a truncated chain makes the peer
           fork a further ancestor than it should — dropping the nearer one's writes with no symptom. */
        DCHECK(n <= cap, "the ancestry buffer is too small for this world's fork chain — a truncated chain makes "
                         "a peer fork a more distant ancestor and silently lose the nearer one's writes");
    }
    return n;
}

/* See world.h. The DOCUMENT is named rather than numbered because a `uint32_t doc` is this instance's handle
   into its own table and means a different document in a peer's — the same reason every cross-instance request
   carries world_doc_name. */
int world_serialize(WorldId w, char *dst, size_t cap)
{
    WorldId anc[16];
    int n_anc, k, n;

    DCHECK(dst != NULL && cap > 0, "a world was serialized into no buffer");
    n_anc = world_ancestry(w, anc, (int)(sizeof anc / sizeof anc[0]));
    n = snprintf(dst, cap, "%s:%u", world_doc_name(w.doc), w.serial);
    CHECK(n > 0 && (size_t)n < cap, "the world vector did not fit its buffer — a truncated vector makes the "
                                    "peer fork a more distant ancestor and silently lose the nearer writes");
    for (k = 0; k < n_anc; k++) {
        int m = snprintf(dst + n, cap - (size_t)n, ",%s:%u", world_doc_name(anc[k].doc), anc[k].serial);
        CHECK(m > 0 && (size_t)(n + m) < cap,
              "the world vector did not fit its buffer — a truncated vector makes the peer fork a more distant "
              "ancestor and silently lose the nearer writes");
        n += m;
    }
    return n;
}

/* THE INVERSE, AND IT LIVES HERE BECAUSE A FORMAT WITH TWO READERS HAS TWO FORMATS. Every host that received a
   world vector used to take it apart by hand — strchr for the comma, strrchr for the colon, strtoul for the
   serial — which is the serializer's grammar restated somewhere it cannot be checked against the writer. The
   document NAME is what crosses (a handle means nothing to a peer), so interning is part of reading it, and the
   nearest-first ORDER is load-bearing: the reader forks the first ancestor it holds, so an order this function
   got wrong would silently fork a more distant one and lose the nearer writes. Destructive on a copy of its own
   making, so the caller's record is untouched. */
int world_parse(const char *s, WorldId *out, WorldId *ancestry, int cap)
{
    char *dup, *q;
    int n_anc = 0;

    DCHECK(s != NULL && *s, "a world vector was parsed from nothing — the answer would be true in no timeline");
    DCHECK(out != NULL && (cap == 0 || ancestry != NULL), "a world vector was parsed into no world");
    *out = WORLD_NONE;
    dup = strdup(s);
    CHECK(dup != NULL, "world: OOM parsing a world vector");
    q = dup;
    while (q && *q) {
        char *comma = strchr(q, ','), *colon;
        if (comma) *comma = 0;
        /* THE LAST colon: a document name is minted as "<parent>.<n>" and a host may name the root anything,
           so only the serial's separator is known to be final. */
        colon = strrchr(q, ':');
        /* EVERY FIELD THE WRITER WROTE IS A WORLD, so a field with no serial is not one to skip — skipping it
           drops an ancestor, which is the same silent loss the truncation below crashes on: the reader forks
           the next ancestor it holds and every write in between goes with it. world_serialize emits
           "<doc>:<serial>" for the head and for each ancestor, so there is no field this can legitimately be. */
        DCHECK(colon != NULL, "a world vector field carried no serial — world_serialize writes <doc>:<serial> "
                              "for every field, so this vector was not written by it, and reading past the "
                              "field would silently drop the ancestor it names");
        {
            WorldId id;
            *colon = 0;
            id.doc = world_doc_intern(q);
            id.serial = (uint32_t)strtoul(colon + 1, NULL, 10);
            if (world_is_none(*out)) *out = id;
            else if (n_anc < cap) ancestry[n_anc++] = id;
            else DFAIL("a world vector's ancestry outran the buffer the reader gave it — the reader would fork "
                       "a more distant ancestor than the sender named and lose every write in between");
        }
        q = comma ? comma + 1 : NULL;
    }
    free(dup);
    /* A VECTOR THAT NAMES NO WORLD IS TRUE IN NO TIMELINE — asserted where the name arrives rather than where
       the answer is used, because by then it is indistinguishable from whatever was last installed. */
    DCHECK(!world_is_none(*out), "a world vector named no world — its segment would come from whatever this "
                                 "instance last installed, which is a different timeline wearing one name");
    {
        int k;
        for (k = 0; k < n_anc; k++)
            DCHECK(ancestry[k].doc == out->doc,
                   "a world vector's ancestry names a world from another document than its own — world_ancestry "
                   "walks only the edges the minting instance holds, so a mixed chain means the vector was "
                   "mis-parsed and this instance would fork the wrong segment");
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
        /* THE CEILING THIS TABLE HAS, NAMED WHERE IT IS HIT. One segment per foreign world that ever reached
           this instance, and world_release — the operation that would drop one — has no caller: nothing tells a
           peer that a sending flow has finished, so every arm of every sender's fork that posts leaves a
           segment here for the life of the instance. A reader standing at this allocation needs to know that,
           because "OOM" alone sends them looking at the page. Build the release: a flow that finishes announces
           its world to the peers it may have reached, exactly as it announces a document it created. */
        CHECK(g != NULL, "world registry: OOM growing the segment table — one segment per foreign world that "
                         "has ever reached this instance, and none is ever released: build the sender-side "
                         "notice that tells a peer a world is gone, so world_release has a caller");
        g_segs = g;
        g_segs_cap = cap;
    }
    g_segs[g_segs_n].id = w;
    g_segs[g_segs_n].delta = d;
    g_segs_n++;
    g_seg_made++;
    return d;
}

void world_release(JSContext *ctx, WorldId w)
{
    ForeignSegment *s = find_segment(w);

    /* A WORLD THAT NEVER WROTE HERE NEVER HAD A SEGMENT, and the sender cannot know which peers a flow reached
       — it would have to track that to avoid telling one, which is state kept only to avoid a no-op. */
    if (!s) return;
    cow_delta_free(ctx, s->delta);
    *s = g_segs[--g_segs_n];
}
