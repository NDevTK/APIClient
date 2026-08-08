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

int world_ancestry(WorldId w, WorldId *out, int cap)
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

static ForeignSegment *find_segment(WorldId w)
{
    int i;
    for (i = 0; i < g_segs_n; i++)
        if (world_eq(g_segs[i].id, w))
            return &g_segs[i];
    return NULL;
}

bool world_has_segment(WorldId w) { return find_segment(w) != NULL; }

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
            /* O(1): the parent's head freezes into a shared immutable base segment both now reference. */
            d = cow_delta_fork(ctx, a->delta);
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
        CHECK(g != NULL, "world registry: OOM growing the segment table");
        g_segs = g;
        g_segs_cap = cap;
    }
    g_segs[g_segs_n].id = w;
    g_segs[g_segs_n].delta = d;
    g_segs_n++;
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
