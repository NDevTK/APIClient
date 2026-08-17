/* THE RENDERER REGISTRY. See registry.h for whose authority this is and why it is not JavaScript.
 *
 * ONE TABLE AND NO SECOND INDEX. A termination names a ROUTING ID while an admission names a CLUSTER KEY,
 * which is a standing invitation to keep two maps — and two maps for one authority is two answers with nothing
 * to say which is right the first time they disagree. The pool this serves holds a handful of renderers, so
 * both lookups are scans over the authority itself and the pairwise invariant below is affordable on every
 * mutation rather than only where somebody remembered to ask.
 *
 * THE SLOT'S STATE IS PART OF THE CONTRACT. A slot is RESERVED from the moment this process decides the
 * cluster gets an instance until the fork order it caused is answered, and LAUNCHED afterwards. Both are
 * registered — a reserved cluster is taken, which is the entire reason the id is minted before the order goes
 * out — but they are not the same fact, and keeping them apart is what lets `launch_failed` assert that it is
 * freeing a slot whose renderer never booted and `terminated` assert that it is freeing one whose renderer
 * did. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "renderer/registry.h"

typedef struct {
    unsigned char *key;   /* the agent cluster key's bytes, owned by this table; opaque here (registry.h) */
    size_t key_n;
    int routing_id;       /* minted by this table and by nothing else; always > 0 */
    bool launched;        /* the fork order was answered with a pipe; false while that order is outstanding */
} RendererSlot;

static RendererSlot *g_slots;
static size_t g_slots_n, g_slots_cap;
static int g_next_routing_id = 1;
static int g_launched, g_terminated, g_failed;

/* ── THE TABLE'S OWN INVARIANTS, ASSERTED AFTER EVERY MUTATION AND BEFORE EVERY READ. They are CHECK and not
   DCHECK for registry.h's reason: what a violation means is two heaps behind one principal, or an agent
   cluster refused an instance forever, and neither becomes acceptable because the build is a release one.
   THE ARITHMETIC IS WHAT MAKES A SNAPSHOT UNABLE TO DISAGREE WITH ITSELF. Every id this table ever minted is
   in exactly one of three states — still registered, terminated, or failed to launch — so the three counts
   must account for the whole id space it has issued; and the slots that say they launched must be exactly the
   launches it has recorded minus the terminations. A counter that stopped being incremented, or a slot freed
   without its counter, is caught here and not in a probe that happens to compare two of the four. */
static void registry_invariants(void)
{
    size_t i, j, live_launched = 0;

    CHECK(g_next_routing_id > 0, "the browser process's routing id counter is not positive — an id is the only "
                                 "name a renderer has, and a non-positive one is a counter that wrapped past "
                                 "the int32 the wire carries and is about to re-issue a live renderer's name");
    for (i = 0; i < g_slots_n; i++) {
        CHECK(g_slots[i].routing_id > 0 && g_slots[i].routing_id < g_next_routing_id,
              "the browser process's registry holds a renderer whose routing id this table never minted — the "
              "counter is the only source of an id, so a registered one outside its issued range is memory "
              "belonging to something else being read as a renderer");
        CHECK(g_slots[i].key != NULL && g_slots[i].key_n > 0,
              "the browser process's registry holds a renderer with no agent cluster key — a renderer IS a "
              "cluster's instance, so a nameless slot is an instance nothing can find and a cluster nothing "
              "can free");
        if (g_slots[i].launched)
            live_launched++;
        for (j = 0; j < i; j++) {
            CHECK(g_slots[i].routing_id != g_slots[j].routing_id,
                  "two renderers in the browser process's registry carry ONE routing id — an id is the only "
                  "name a renderer has, so a collision means a termination frees whichever of the two the scan "
                  "reaches first and leaves the other registered forever");
            CHECK(g_slots[i].key_n != g_slots[j].key_n ||
                      memcmp(g_slots[i].key, g_slots[j].key, g_slots[i].key_n) != 0,
                  "two renderers in the browser process's registry hold ONE agent cluster — two heaps for one "
                  "similar-origin window agent is the split SECURITY.md's one-instance-per-cluster rule exists "
                  "to forbid, and it is two principals behind one origin");
        }
    }
    CHECK((int64_t)g_slots_n + (int64_t)g_terminated + (int64_t)g_failed == (int64_t)g_next_routing_id - 1,
          "the browser process's registry does not account for every routing id it has minted — each one is "
          "registered, terminated or failed to launch, so a shortfall is a slot freed without its counter and "
          "a surplus is a counter moved without its slot");
    CHECK((int64_t)live_launched == (int64_t)g_launched - (int64_t)g_terminated,
          "the browser process's registry holds a different number of launched renderers than it has launched "
          "and not yet buried — the slots and the counters are one fact recorded twice, and the probe that "
          "compares this document's frames against this table would report whichever of the two it happened "
          "to read");
}

static RendererSlot *slot_by_id(int routing_id)
{
    size_t i;
    for (i = 0; i < g_slots_n; i++)
        if (g_slots[i].routing_id == routing_id)
            return &g_slots[i];
    return NULL;
}

static RendererSlot *slot_by_key(const unsigned char *key, size_t key_n)
{
    size_t i;
    for (i = 0; i < g_slots_n; i++)
        if (g_slots[i].key_n == key_n && memcmp(g_slots[i].key, key, key_n) == 0)
            return &g_slots[i];
    return NULL;
}

/* THE LOOKUP EVERY ID-TAKING ENTRY MAKES, WITH THE TWO WAYS IT CAN FAIL TOLD APART, because they are different
   accusations and a caller reading one @E line is standing where the fix has to be made. An id at or beyond
   the counter was NEVER MINTED by this process — the inversion this transport exists to make impossible. An id
   below it names a renderer this table has already buried, which is a second report of one death and would
   free an agent cluster that has a live instance. */
static RendererSlot *slot_require(int routing_id)
{
    RendererSlot *s;

    CHECK(routing_id > 0 && routing_id < g_next_routing_id,
          "the browser process was told about a renderer whose routing id it never minted — an id comes out of "
          "this registry and out of nothing else, so one arriving from outside its issued range is a renderer "
          "some other zone decided existed");
    s = slot_by_id(routing_id);
    CHECK(s != NULL,
          "the browser process was told about a renderer it has already buried — the id was minted here, so "
          "this is one renderer reported dead twice, and the second report frees an agent cluster that either "
          "has a live instance or is about to be given one");
    return s;
}

static void slot_release(RendererSlot *s)
{
    free(s->key);
    *s = g_slots[g_slots_n - 1];   /* the table is a set; the last entry fills the hole */
    g_slots_n--;
}

int renderer_registry_create(const unsigned char *cluster_key, size_t cluster_key_n)
{
    size_t i;
    int routing_id;

    CHECK(cluster_key != NULL && cluster_key_n > 0,
          "the browser process was asked for a renderer for an EMPTY agent cluster key — a renderer IS a "
          "cluster's instance, so an empty one would put every document that failed to state its cluster "
          "behind one heap and one principal");
    /* THE KEY IS JSON-SAFE, ASSERTED WHERE IT ARRIVES rather than where the snapshot is serialized. A cluster
       key is a browsing-context group and a URL-serialized origin, neither of which can hold a quote, a
       backslash or a control character — the NUL that joins them is the one exception, and the snapshot
       renders it as `|`. Asserting it here names the caller that passed something which is not an origin;
       asserting it at the serializer would name the serializer.
       THE TWO BYTES ARE SPELLED IN HEX AND NOT AS CHARACTER LITERALS, which is a property of the assertion
       machinery rather than a style: `APICLIENT_ASSERT_EMIT` stringifies the CONDITION into the `@WHY` line's
       own JSON unescaped, so a `'"'` written here would emit a malformed diagnostic on the one line whose
       whole job is to be readable by a machine. 0x22 is the quote, 0x5C the backslash. */
    for (i = 0; i < cluster_key_n; i++)
        DCHECK(cluster_key[i] == 0 ||
                   (cluster_key[i] >= 0x20 && cluster_key[i] != 0x22 && cluster_key[i] != 0x5C),
               "an agent cluster key carried a character neither an origin nor a browsing-context group can "
               "hold — the key is a browser-stated group and a URL-serialized origin joined by a NUL, so a "
               "quote, a backslash or another control character is a caller that built the key out of "
               "something else");
    CHECK(slot_by_key(cluster_key, cluster_key_n) == NULL,
          "the browser process was asked for a SECOND renderer for an agent cluster that already has one — two "
          "heaps for one similar-origin window agent is the split SECURITY.md's one-instance-per-cluster rule "
          "exists to forbid, and this registry is the authority that already held the answer");
    CHECK(g_next_routing_id < INT32_MAX,
          "the browser process has exhausted the routing id space — the wire carries an int32 and the next id "
          "would wrap into one a live renderer already answers to");

    routing_id = g_next_routing_id;
    CHECK(slot_by_id(routing_id) == NULL,
          "the routing id the browser process was about to mint is already held by a live renderer — the "
          "counter only moves forward, so a collision here is a table holding an id from outside it");
    g_next_routing_id++;

    if (g_slots_n == g_slots_cap) {
        size_t cap = g_slots_cap ? g_slots_cap * 2 : 8;
        RendererSlot *p = (RendererSlot *)realloc(g_slots, cap * sizeof *p);
        CHECK(p != NULL, "OOM growing the browser process's renderer registry — the alternative to crashing is "
                         "a renderer that exists with nothing recording which agent cluster it holds");
        g_slots = p;
        g_slots_cap = cap;
    }
    g_slots[g_slots_n].key = (unsigned char *)malloc(cluster_key_n);
    CHECK(g_slots[g_slots_n].key != NULL,
          "OOM copying an agent cluster key into the browser process's renderer registry — the key is what "
          "every later admission is refused against, so a slot without one is a cluster that would be given a "
          "second instance");
    memcpy(g_slots[g_slots_n].key, cluster_key, cluster_key_n);
    g_slots[g_slots_n].key_n = cluster_key_n;
    g_slots[g_slots_n].routing_id = routing_id;
    g_slots[g_slots_n].launched = false;
    g_slots_n++;

    registry_invariants();
    return routing_id;
}

void renderer_registry_launched(int routing_id)
{
    RendererSlot *s = slot_require(routing_id);

    CHECK(!s->launched,
          "the browser process was told a renderer launched that it has already recorded as launched — one "
          "fork order produces one renderer, so a second report inflates the launch count the offscreen's own "
          "fork total is checked against and hides an order that never arrived");
    s->launched = true;
    g_launched++;
    registry_invariants();
}

void renderer_registry_launch_failed(int routing_id)
{
    RendererSlot *s = slot_require(routing_id);

    CHECK(!s->launched,
          "the browser process was told a LAUNCHED renderer failed to launch — a renderer that booted dies by "
          "termination, and recording it as a failed launch would leave the launch count claiming a renderer "
          "that is gone");
    /* THE CLUSTER IS FREED HERE. The zygote has already removed the frame; leaving the registration would
       refuse this agent cluster a renderer forever, with nothing anywhere to say why. */
    slot_release(s);
    g_failed++;
    registry_invariants();
}

void renderer_registry_terminated(int routing_id)
{
    RendererSlot *s = slot_require(routing_id);

    CHECK(s->launched,
          "the browser process was told a renderer terminated whose fork order it has not yet answered — a "
          "renderer that never booted is reported by its order's own failure and burying it twice would count "
          "one dead instance against both totals");
    slot_release(s);
    g_terminated++;
    registry_invariants();
}

/* ── THE SNAPSHOT'S TEXT. It grows rather than living in a fixed buffer, because its length is the number of
   renderers this browser process has been asked for and a fixed buffer would be a cap on that — a truncation
   would deliver malformed JSON, and a refusal would be an admission rule invented by a serializer. It is
   reused across calls for main.c's reason: `ccall` converts it to a JS string before returning and JavaScript
   is run-to-completion, so no second call can begin while a first answer is still being read. */
static char *g_json;
static size_t g_json_n, g_json_cap;

static void jput(const char *s, size_t n)
{
    if (g_json_n + n + 1 > g_json_cap) {
        size_t cap = g_json_cap ? g_json_cap : 256;
        char *p;
        while (cap < g_json_n + n + 1)
            cap *= 2;
        p = (char *)realloc(g_json, cap);
        CHECK(p != NULL, "OOM building the browser process's registry snapshot — the record is what the "
                         "offscreen checks its own renderer frames against, and no answer at all is better "
                         "than a truncated one that would parse as a different set");
        g_json = p;
        g_json_cap = cap;
    }
    memcpy(g_json + g_json_n, s, n);
    g_json_n += n;
    g_json[g_json_n] = '\0';
}

static void jputs(const char *s) { jput(s, strlen(s)); }

static void jputi(int v)
{
    char b[16];
    int n = snprintf(b, sizeof b, "%d", v);
    DCHECK(n > 0 && (size_t)n < sizeof b,
           "a registry counter did not fit its decimal rendering — every one of them is an int32, which is ten "
           "digits and a sign, so a truncation here is a value that is not the counter it was read from");
    jput(b, (size_t)n);
}

/* THE DIAGNOSTIC VIEW OF A KEY, which substitutes `|` for the NUL joining its two halves. A NUL is invisible
   in every console that prints it and would end the C string this record crosses as, so the AUTHORITY keeps
   the real bytes and only this rendering makes them readable. */
static void jput_key(const RendererSlot *s)
{
    size_t i;
    for (i = 0; i < s->key_n; i++) {
        char c = s->key[i] ? (char)s->key[i] : '|';
        jput(&c, 1);
    }
}

/* Ordered as the record's reader expects: the ids ASCENDING NUMERICALLY, which is the order
   `rendererStats().routingIds` is sorted in and therefore the order the two sets are compared in; the keys
   lexicographically over that same rendering, which is a diagnostic and is sorted only so a reader can find
   one. Insertion sort because the table is a handful of renderers and a qsort comparator would need the
   rendering to live somewhere first. */
static int key_cmp(const RendererSlot *a, const RendererSlot *b)
{
    size_t n = a->key_n < b->key_n ? a->key_n : b->key_n, i;
    for (i = 0; i < n; i++) {
        unsigned char x = a->key[i] ? a->key[i] : (unsigned char)'|';
        unsigned char y = b->key[i] ? b->key[i] : (unsigned char)'|';
        if (x != y)
            return x < y ? -1 : 1;
    }
    if (a->key_n == b->key_n)
        return 0;
    return a->key_n < b->key_n ? -1 : 1;
}

const char *renderer_registry_snapshot_json(void)
{
    size_t *order = NULL;
    int *ids = NULL;
    size_t i, j;

    registry_invariants();

    if (g_slots_n > 0) {
        order = (size_t *)malloc(g_slots_n * sizeof *order);
        ids = (int *)malloc(g_slots_n * sizeof *ids);
        CHECK(order != NULL && ids != NULL,
              "OOM ordering the browser process's registry snapshot — an unordered answer would be compared "
              "against an ordered one and would report every live renderer as a disagreement");
        for (i = 0; i < g_slots_n; i++) {
            order[i] = i;
            ids[i] = g_slots[i].routing_id;
        }
        for (i = 1; i < g_slots_n; i++) {
            size_t v = order[i];
            for (j = i; j > 0 && key_cmp(&g_slots[order[j - 1]], &g_slots[v]) > 0; j--)
                order[j] = order[j - 1];
            order[j] = v;
        }
        for (i = 1; i < g_slots_n; i++) {
            int v = ids[i];
            for (j = i; j > 0 && ids[j - 1] > v; j--)
                ids[j] = ids[j - 1];
            ids[j] = v;
        }
    }

    g_json_n = 0;
    jputs("{\"clusters\":\"");
    for (i = 0; i < g_slots_n; i++) {
        if (i)
            jputs(",");
        jput_key(&g_slots[order[i]]);
    }
    jputs("\",\"routingIds\":\"");
    for (i = 0; i < g_slots_n; i++) {
        if (i)
            jputs(",");
        jputi(ids[i]);
    }
    jputs("\",\"live\":");
    jputi((int)g_slots_n);
    jputs(",\"launched\":");
    jputi(g_launched);
    jputs(",\"terminated\":");
    jputi(g_terminated);
    jputs(",\"failed\":");
    jputi(g_failed);
    jputs(",\"nextRoutingId\":");
    jputi(g_next_routing_id);
    jputs("}");

    free(order);
    free(ids);
    return g_json;
}
