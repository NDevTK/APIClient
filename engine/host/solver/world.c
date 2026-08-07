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

static MintedWorld *g_minted;
static uint32_t     g_minted_n, g_minted_cap;

static ForeignSegment *g_segs;
static int             g_segs_n, g_segs_cap;

void world_registry_init(uint32_t doc_id)
{
    /* ZERO IS THE NONE VALUE, so a document that took it would mint worlds indistinguishable from "no world".
       The host assigns this, and it is the host that knows whether there is more than one document at all. */
    DCHECK(doc_id != 0, "the world registry was given document id 0, which is the reserved NONE value — the "
                        "host must assign every document a distinct non-zero id");
    DCHECK(g_doc == 0 || g_doc == doc_id, "the world registry was re-initialised with a different document id "
                                          "— one instance is one document, and its worlds are named by it");
    g_doc = doc_id;
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
