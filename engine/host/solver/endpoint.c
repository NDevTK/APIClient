/* @H endpoint surface — see endpoint.h. Findings are C data; params + values merge in C; emit is C. */
#include "solver/endpoint.h"
#include "core/json_buf.h"
#include "solver/compose.h"   /* the edge splices' leading comma */
#include "solver/metrics.h"   /* the edge rows, declared in solver/metrics/{fetch,xhr}_edge.def */
#include "core/mime/mime_type.h"
#include "solver/concolic.h"
#include "solver/engine.h"    /* the provenance's one wire spelling; this surface prints the same three words */
#include "solver/flow.h"
#include "solver/pending.h"   /* …and PROV_*, the vocabulary those words spell */
#include "check.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Where the value landed in the request. A reviewer replays a path param by substituting it into the address, a
   query param by appending it and a body param by encoding it into the payload, so the location is half of what a
   param is: two params of one name in two places are two params. */
typedef enum { EP_QUERY = 0, EP_PATH, EP_BODY } EpLoc;
static const char *const ep_loc_name[] = { "query", "path", "body" };

/* A param states several facts that merge across sightings by different rules.
 *
 * `vals` is provenance and example: each entry is a value the code computed on some path, so the merge is a union;
 * another path computing another value adds knowledge and takes none away.
 *
 * `excl`, `bnd` and `pred` are domain. An exclusion is a token some flow's own equality gate proved this hole is
 * not, a bound an ordering gate's interval, and a predicate a method-call gate (`path.startsWith("/api")`, the
 * shape @H names), which determines neither a value nor an interval. The record's claim is about the endpoint, so
 * only a constraint every observed path obeyed is true of it: the sets intersect, and a bound widens to the
 * interval hull (two paths with `x >= 5` and `x >= 10` leave `x >= 5`; with `x >= 5` and `x <= 3`, no bound on
 * either side). A sighting that observed no bound or predicate erases it, since a path that reached the request
 * without obeying the claim disproves it. Within one flow gates conjoin instead (`if (x > 5 && x < 100)`);
 * concolic.c owns that half and this file the across-paths half.
 *
 * `has_hole` says whether this param's value named a hole at the read, that is whether `kv_add` could ask the flow
 * anything: a fact about the observation rather than a constraint on the value, so it unions as `vals` does.
 * Intersecting would answer `concrete` for a param one path did mint from a hole and make a coverage fraction read
 * higher than the truth; the union errs toward understating coverage. It cannot be re-derived downstream:
 * `kv_pairs` emits the aligned example where the shape held a hole, so a param minted from `{location.hash}`
 * renders `validValues:["abc"]`, the same bytes as a computed one. */
typedef struct { int has_lo, has_hi; double lo, hi; int lo_incl, hi_incl; char *lo_txt, *hi_txt; } ParamBound;
typedef struct { char *name; EpLoc loc; char **vals; int nvals, vcap;
                 char **excl; int nexcl; ParamBound bnd;
                 ConcolicPred *pred; int npred;
                 ConcolicLooseEq *leq; int nleq; int has_hole; } Param;
typedef struct { char *name; char *value; } EpHeader;   /* the transport half: what the request must carry */
/* `is_asset` is what the resource at this address turned out to be, written after the record exists. Whether a
   static asset is an endpoint is decided by bytes (magic bytes and content type, not a URL suffix), which arrive on
   a reply, while a request is recorded before its policy check (a request a policy refuses is still one the bundle
   can make), so the flag joins two facts learned at two times. */
/* `prov` is what this record is evidence of, part of its identity rather than a label (endpoint.h has why the
   pending line's most-observed fold is right there and wrong here): every value, exclusion, bound and predicate on
   a record was observed at one grade, because a sighting at another grade cannot reach it. */
/* `pre_program` is whether this instance had run a line of any program when this record was minted, the one fact
   about a sighting `prov` cannot carry (endpoint.h's census contract; solver/engine.h's
   `engine_any_program_started`). A property of the mint, not of the address: a later sighting of a recorded
   address teaches the surface no new endpoint, so the question is whether the sighting that created the record
   needed the page's code to run, and a flag re-armed on every merge would answer it about the last sighting. */
typedef struct { char *method; char *path; Param *params; int np, pcap;
                 EpHeader *hdrs; int nh, hcap; int is_asset; int prov; int pre_program; int door;
                 /* `addr_class` is whether the run had determined the address when it recorded it (endpoint.h's
                    ENDPOINT_ADDRESS_CLASSES has each word's claim and why it is a floor under the hard bar). No
                    producer states it: it is read off the `url` value's own concolic provenance at the door.
                    Merged by a max over the list's order, unlike `door` and `pre_program`, which are properties of
                    the mint: that some observed path composed the address from a value it had not determined
                    (and, at the top of the list, that some path's unknown was supplied from outside this engine)
                    is not taken back by a later sighting that proved less. */
                 int addr_class;
                 /* `witness_class` is whether this address may rest on a witness this engine chose (endpoint.h's
                    ENDPOINT_WITNESS_CLASSES: the necessary condition under the razor). Merged like `addr_class`,
                    by a max over its list's order: a sighting that could ask outranks one that could not, and a
                    path that had chosen a witness is not taken back by a later one that had not. The two fields
                    differ in what each order means, not in the shape of the merge. */
                 int witness_class;
                 /* Which sources the address value's bytes entered through, as concolic.h's `concolic_root_c`
                    spells them: the delivery provenance of the address. A diagnostic, never an operand of the hard
                    bar: `address_class_of` asks concolic.h's `concolic_root_whose_any` of the value, a fact the
                    mint stated and every derivation carried, and a classification keyed on this string's spelling
                    would be a match rather than a run. It is the root of the sighting whose class the record
                    carries, written at the mint and replaced on the line that raises the class, so class and root
                    are always one sighting's. NULL is two positive statements: a value that is not this engine's
                    concolic at all, or one whose bytes entered through nothing minted as a source. The first is
                    `addr_class == EPA_CONCRETE` and the second is not, so the class on the same row tells them
                    apart, which is why the emit spells NULL in two JSON types. */
                 char *addr_root;
                 /* The body this engine had no field reader for (endpoint.h): set only where `body_params` named
                    nothing, so never a second spelling of `params`' fields, and never overwritten, since a request
                    body is one example. Exactly one of two fields is set: `body_b64` is bytes the request sends, a
                    replay claim; `body_shape` is the display spelling of a body built from unknown external input,
                    naming the source and not replayable. The key each is emitted under tells a consumer which.
                    `body_example_b64` is a different claim about a different body: the bytes of a body whose
                    unknown ranges are named on `params`, set beside named fields (the span params say where a
                    reviewer edits, these bytes what stands there now). It makes no replay claim, since some bytes
                    are an unknown's example, so it cannot share `body_b64`'s key. */
                 char *body_mime, *body_b64, *body_shape, *body_example_b64; } Endpoint;

/* See endpoint.h. The switch is generated from the list the enum is, so a door added to `ENDPOINT_DOORS` gains its
   arm in the same expansion, unlike `ep_loc_name`'s hand-kept table, which needs a `CHECK` at every subscript. The
   fallthrough is a CHECK, as in `engine_provenance_token`: this runs once per emitted row in every build, a zero
   arrives here only in release (where the mint's DCHECK is compiled out), and a plausible mechanism name published
   on an @H record is worse than a crash. */
const char *endpoint_door_token(int door) {
    switch (door) {
#define ENDPOINT_DOOR_ARM(id, token, reach, bytes) case id: return token;
    ENDPOINT_DOORS(ENDPOINT_DOOR_ARM)
#undef ENDPOINT_DOOR_ARM
    }
    CHECK_FAILF("endpoint: an @H record states the door %d, which is none of endpoint.h's ENDPOINT_DOORS — "
                "the mechanism that composed an address is stated by its producer and refused at the door "
                "when it is not, so this is a record that reached the surface in a release build with no "
                "producer having said, and the word about to be published names a mechanism nothing ran",
                door);
}

/* See endpoint.h. Generated from the same list as `endpoint_door_token`, so a door added without a reach does not
   compile and the reach is stated in one place, a column rather than a table beside the enum. The fallthrough is a
   CHECK for `endpoint_door_token`'s reason: this runs once per emitted @H record in the reach census in every
   build, and a release build would classify a record under whatever the register held. */
int endpoint_door_reach(int door) {
    switch (door) {
#define ENDPOINT_DOOR_REACH_ARM(id, token, reach, bytes) case id: return reach;
    ENDPOINT_DOORS(ENDPOINT_DOOR_REACH_ARM)
#undef ENDPOINT_DOOR_REACH_ARM
    }
    CHECK_FAILF("endpoint: an @H record states the door %d, which is none of endpoint.h's ENDPOINT_DOORS — "
                "the reach census is asking what a markup parse would have reached through a mechanism "
                "nothing named, and the class it is about to raise is at an index outside the partition it "
                "is a member of",
                door);
}

/* See endpoint.h. `endpoint_door_token`'s construct and its severity, one list over. */
const char *endpoint_reach_token(int reach) {
    switch (reach) {
#define ENDPOINT_REACH_ARM(id, token) case id: return token;
    ENDPOINT_REACHES(ENDPOINT_REACH_ARM)
#undef ENDPOINT_REACH_ARM
    }
    CHECK_FAILF("endpoint: the @H reach census states the class %d, which is none of endpoint.h's "
                "ENDPOINT_REACHES — every class comes off `endpoint_door_reach`, which is generated from the "
                "door list's own third column, so this is a value no list produced and the key about to be "
                "published names a razor reading nothing decided",
                reach);
}

/* See endpoint.h. `endpoint_reach_token`'s construct and severity, with a sharper reason: this runs once per
   emitted row too, so a release fallthrough would write whatever the register held into the key that states the
   product's own hard bar. There is no `EPA_UNSTATED` arm, because no producer can forget: the class is derived at
   the door from the address value (`address_class_of`). */
const char *endpoint_address_class_token(int cls) {
    switch (cls) {
#define ENDPOINT_ADDRESS_CLASS_ARM(id, token) case id: return token;
    ENDPOINT_ADDRESS_CLASSES(ENDPOINT_ADDRESS_CLASS_ARM)
#undef ENDPOINT_ADDRESS_CLASS_ARM
    }
    CHECK_FAILF("endpoint: an @H record states the address class %d, which is none of endpoint.h's "
                "ENDPOINT_ADDRESS_CLASSES — every value comes off `address_class_of`, which returns a member "
                "of that list on every one of its arms, so this is a field nothing in this file wrote and the "
                "word "
                "about to be published is a claim about the product's runtime-only bar that no measurement "
                "made",
                cls);
}

/* See endpoint.h. Generated from the same list as `endpoint_door_reach`, one column over, so a door added without
   stating whose bytes it hands this surface does not compile; a forgotten door would err in the safe direction
   (`EPB_DOCUMENT` proves nothing), the under-claim nobody discovers. The fallthrough is a CHECK for
   `endpoint_door_reach`'s reason: once per emitted @H record in every build, it would otherwise hand the razor
   composer a plausible answer. */
int endpoint_door_bytes(int door) {
    switch (door) {
#define ENDPOINT_DOOR_BYTES_ARM(id, token, reach, bytes) case id: return bytes;
    ENDPOINT_DOORS(ENDPOINT_DOOR_BYTES_ARM)
#undef ENDPOINT_DOOR_BYTES_ARM
    }
    CHECK_FAILF("endpoint: an @H record states the door %d, which is none of endpoint.h's ENDPOINT_DOORS — "
                "the razor composer is asking whether that mechanism's bytes were in the served document at "
                "all, about a mechanism nothing named, and the class it is about to hand that composer is at "
                "an index outside the list it is a member of",
                door);
}

/* See endpoint.h. `endpoint_reach_token`'s construct and severity, with `endpoint_address_class_token`'s sharper
   reason: it runs once per emitted row in every build, on the key that states the product's hard bar. */
const char *endpoint_razor_class_token(int cls) {
    switch (cls) {
#define ENDPOINT_RAZOR_CLASS_ARM(id, token) case id: return token;
    ENDPOINT_RAZOR_CLASSES(ENDPOINT_RAZOR_CLASS_ARM)
#undef ENDPOINT_RAZOR_CLASS_ARM
    }
    CHECK_FAILF("endpoint: an @H record states the razor class %d, which is none of endpoint.h's "
                "ENDPOINT_RAZOR_CLASSES — every value comes off `endpoint_razor_class_of`, which returns one "
                "of exactly two members of that list, so this is a value nothing in this file composed and the "
                "word about to be published is this engine's answer to the product's runtime-only bar with no "
                "measurement under it",
                cls);
}

/* See endpoint.h. `endpoint_address_class_token`'s construct and severity, with that list's banner's extra reason:
   this word is the only published bound on the part of the razor's `unproven` whose bytes this engine chose (a
   pin's bytes are a literal the page's own predicate spelled, so what it bounds is reproducibility), and a release
   build publishing a word nothing decided would put a measurement under a figure nobody took. */
const char *endpoint_witness_class_token(int cls) {
    switch (cls) {
#define ENDPOINT_WITNESS_CLASS_ARM(id, token) case id: return token;
    ENDPOINT_WITNESS_CLASSES(ENDPOINT_WITNESS_CLASS_ARM)
#undef ENDPOINT_WITNESS_CLASS_ARM
    }
    CHECK_FAILF("endpoint: an @H record states the witness class %d, which is none of endpoint.h's "
                "ENDPOINT_WITNESS_CLASSES — every value comes off `witness_class_now`, which returns one of "
                "exactly three members, so this is a field nothing in this file wrote and the word about to "
                "be published is the only bound anybody has on what the product's own bar did not measure",
                cls);
}

/* What the running path has chosen, asked only where there is a path to ask (endpoint.h has each member's claim).
   The gate is not defensive: `engine_pinned_of_running_path` DCHECKs that a flow stands, and a record minted before
   this instance started any program is an ordinary population (`pre_program` counts it), so asking unconditionally
   would abort on the commonest document shape. `unasked` makes the gate a statement: the question was not asked,
   a different fact from `no-witness`. */
static int witness_class_now(void) {
    if (flow_running() == NULL) return EPW_UNASKED;
    return engine_pinned_of_running_path() ? EPW_MAY_REST_ON : EPW_NO_WITNESS;
}

/* See endpoint.h. A union rather than an intersection, for the address class's own merge's reason: each operand is
   a positive statement that the address exists only at run time, so an intersection would answer `unproven` for a
   row one of them proved and make the floor read lower than the run established. No assert on either operand:
   both arrive through generated switches whose fallthrough is a CHECK in every build, so an assert here could not
   fail. The two `if`s are not a precedence: both return the same member. */
int endpoint_razor_class_of(int door, int addr_class) {
    /* `== EPA_UNKNOWN`, not `!= EPA_CONCRETE`: every member added to `ENDPOINT_ADDRESS_CLASSES` goes below the one
       this bar reads, carving a population out of the claim without a character changing here, which is why that
       list is where a narrowing of the bar goes. `EPA_SOURCE_DETERMINED` is an address this flow determined out of
       a source, whose pin's bytes are a literal the page's own text spells, so it does not exist only at run time.
       `EPA_UNKNOWN_UNPROVEN` is one the run did not determine and whose every root this engine minted, or the
       served document spells, or no mint could speak for, so the bar is false of it or unproven about it.
       `!= EPA_CONCRETE` would claim both, the over-claim endpoint.h's banner measures; it reads as the natural
       complement because both members sit on the concolic side of `url_display`'s branch. Each is its own census
       row, so the population that left this class is countable. */
    if (addr_class == EPA_UNKNOWN) return EPZ_RUNTIME_ONLY;
    if (endpoint_door_bytes(door) == EPB_OFF_DOCUMENT) return EPZ_RUNTIME_ONLY;
    return EPZ_UNPROVEN;
}

/* A value carrying a `{hole}` is a shape, an unknown the code did not compute, and a hole-free one is the real
   thing; a concrete value supersedes a shape for the same header, as a param's example values do. */
static int header_is_shape(const char *v) { return strchr(v, '{') != NULL; }

/* Returns how many header names this endpoint did not have before; the caller credits the WFQ with it (see the
   merge site). */
static int endpoint_merge_headers(Endpoint *e, const EndpointHeader *hdrs, int nhdrs) {
    int gained = 0;
    for (int i = 0; i < nhdrs; i++) {
        const char *n = hdrs[i].name, *v = hdrs[i].value ? hdrs[i].value : "";
        int j, found = 0;
        if (!n || !n[0]) continue;
        for (j = 0; j < e->nh; j++) {
            if (strcmp(e->hdrs[j].name, n)) continue;
            found = 1;
            if (header_is_shape(e->hdrs[j].value) && !header_is_shape(v)) {
                free(e->hdrs[j].value);
                e->hdrs[j].value = strdup(v);
                CHECK(e->hdrs[j].value, "endpoint: OOM refining a header value");
            }
            break;
        }
        if (found) continue;
        if (e->nh >= e->hcap) {
            e->hcap = e->hcap ? e->hcap * 2 : 4;
            e->hdrs = realloc(e->hdrs, (size_t)e->hcap * sizeof(EpHeader));
            CHECK(e->hdrs, "endpoint: OOM growing an endpoint's headers");
        }
        e->hdrs[e->nh].name = strdup(n);
        e->hdrs[e->nh].value = strdup(v);
        CHECK(e->hdrs[e->nh].name && e->hdrs[e->nh].value, "endpoint: OOM copying a header");
        e->nh++;
        gained++;
    }
    return gained;
}

static Endpoint *g_eps = NULL;
static int g_eps_n = 0, g_eps_cap = 0;
static int g_suppress = 0;   /* a candidate/verify re-run's requests are @S artifacts, not real @H */

/* The ask, which the outcome figures beside the surface cannot carry. endpoint_surface_census walks the record
 * array, so every figure it produces counts outcomes, and a census of what landed cannot tell a component that
 * never asked from one whose ask a gate correctly refused; the repair is to record at the call. endpoint_record
 * is the one door every HTTP-shaped edge passes through (endpoint.h's funnel note), so a count at its entry is the
 * number of times this engine composed a request and offered its address to this surface. It is not the number of
 * times a network call site was reached: five Fetch §5.4 stages stand in front of this door (endpoint.h, with the
 * residual for that count).
 *
 * `emitted - preProgram` is a ceiling on what forced execution contributed (endpoint.h), and its zero has two
 * readings that take opposite work: no arm composed a request, or arms composed requests whose addresses were
 * already on the surface. A repeat sighting merges and moves nothing (the record's `pre_program` was decided at
 * its mint), so the second is invisible to every outcome row. `asks - preProgram` is nonzero in the second case
 * and zero in the first, a ceiling on that reading (it is also nonzero for a post-program ask that minted, and
 * for one that merged into a record post-program code minted); `merged_pre_program` below states it exactly.
 *
 * Six rows, two partitions and a cut over one event. By outcome, `suppressed + merged + minted == asks`, asserted
 * where all four are in hand; by program state, `preProgram <= asks`, a containment, since a stored complement
 * would be a second raise free to drift. The sixth is a cut inside the merged arm, summed with nothing, its
 * containments asserted at the accessor. Every one is a lifetime count, never a gauge: it may be differenced or
 * accumulated and never decreases, so a sample below its predecessor is this file and not the run. Their scope is
 * the surface's, not the process's: reset in endpoint_init and endpoint_free beside `g_eps_n`, because one process
 * runs many sessions and a standing count would report a previous document's asks under this one's name. A
 * report, never a bound: nothing branches on one. */
static long g_asks;             /* every call that reached the door, counted before the gate */
static long g_asks_pre_program; /* …of those, the ones asked before this instance had started any program */
static long g_ask_suppressed;   /* declined AT the gate: a candidate/verify re-fire is an @S artifact */
static long g_ask_merged;       /* reached a record this surface already held — the invisible arm */
static long g_ask_minted;       /* created a record — the arm, and the only one, that moves `epMinted` */
/* The cut of the merged arm that answers the razor's own sentence, which `asks - preProgram` bounds from above and
 * never states: the ask was made with a program running and the record it reached was minted before any program
 * had run, running code composing an address the markup had already named. Nothing else in either census can be
 * that, since a merge moves no surface figure. A cut, not a fourth arm: it selects inside `merged` on a fact
 * about the record, so it is contained in `merged` and in `asks - preProgram`, both asserted in
 * endpoint_ask_census. Its zero is four states the other rows separate: no post-program ask (`asks ==
 * preProgram`); asks suppressed; asks minted (shown by `emitted - preProgram` unless the reply classified them as
 * files); or merges only into records post-program code minted. A nonzero is one statement. */
static long g_ask_merged_pre_program;
/* Whether the boundary the two program-state rows are graded against was already past when this surface's
 * counters were last reset: a latch, because the disagreement it catches is between two scopes, which neither
 * census can see from inside itself. `engine_any_program_started` reads `g_prog_starts`, which solver/engine.c's
 * session teardown keeps on its not-reset list (the instance's total, monotone for the process), while every
 * counter here is reset at endpoint_free. So a second session's @H counters restart at zero and the program bit
 * does not, and every ask of the second document, including every `<script src>` and `<link>` of a `<head>` that
 * had not run a line, grades post-program: `preProgram` reads 0 and `emitted - preProgram` the whole surface, the
 * markup door reported as forced execution's whole contribution, silently and with every census identity holding.
 * A latch rather than a census-time comparison, because `pre_program` is decided at each mint and the wrong
 * grading leaves internally consistent figures; the one instant the scopes are distinguishable is where this
 * file's restarts and the engine's has not. Compiled out of release with the assert that reads it. */
static int g_boundary_spent;

/* The host edges' own entries (endpoint.h states both contracts). They live here rather than in core/fetch/fetch.c
   and core/xhr/xml_http_request.c because their scope must be this file's: they are read against the ask rows
   above, and reset beside `g_asks` in endpoint_init and endpoint_free, the scope disagreement `g_boundary_spent`
   catches is unreachable. `offered <= asks`, which makes an edge's share of this surface's ask population
   readable, is checkable only with both terms in one hand. The labels are each machine's, handed over rather than
   copied: `steps` is core/fetch's `js_fetch_steps[]` or core/xhr's `SEND_STEPS`, static string-literal tables that
   outlive every document, and `stage_n` comes from the NULL terminator, so a stage added to either X-list adds a
   row here with no edit, where a second list spelled here would drift silently (a missing row reads as a stage
   nothing died at). */
/* One struct and two instances: the two host edges (core/fetch's request construction, core/xhr's `send()`) have
   identical arithmetic, a partition over one teardown and a containment against the preceding ask. The row names
   stay per edge, because `json_buf_key` takes a string literal (core/json_buf.h), which keeps every published
   field name readable off this source; so state and arithmetic are shared and keys are written out per edge.
   `reached` is what each edge names for itself: for core/fetch the offer (the construction reached §5.6 step 12 and
   handed this surface an address), for core/xhr the placement (§3.5.6 step 12/13 handed the request to the
   lifecycle machine, one machine away from the door). Both mean "the construction completed". Lifetime counts,
   never gauges; a report, never a bound. */
typedef struct {
    const char *const *steps;   /* the edge's own stage labels, borrowed from its X-list — never a copy */
    long *died;                 /* one per stage, sized at the declare from the table above */
    int   stage_n;              /* how many, from that table's NULL terminator */
    int   stage_first;          /* the stage number `steps[0]` is — the edge states it, this file never assumes */
    /* The free global identifier a program must spell to reach this edge (`fetch`, `XMLHttpRequest`), stated by
       the edge beside its stage table, for `stage_first`'s reason: which name a component installs itself under is
       its own fact, and a table here would drift silently (a name that stopped matching reads as a program that
       never spelled it). Borrowed: a literal with static storage at the declaring site. */
    const char *entry;
    /* The ask one frame further out, and the only pair here that is not a fact about a flow. Every other row is
     * raised from a live frame, about a site some flow reached, so the census could not say what a reader of a
     * zero most needs: an occurrence nobody reached has no frame, no operand and no moment, so no runtime edge can
     * report it and no assert stand on it (the only invariant, "this document asked for something", has an operand
     * derived from the page's own bytes, which may not be asserted on; a page need only ship no `fetch`). So the
     * artifact is a row with a denominator, and this is the denominator. It is raised where quickjs's
     * `resolve_scope_var` reports every free identifier it resolves against the global object, the last instant
     * this engine knows what a program names independently of what any flow ran: upstream of reach, compilation is
     * the only recording point.
     *
     * With `named` beside it, `called == 0` separates "the program names no such call" from "a flow never reached
     * one it makes" (`named > 0 && called == 0`, the state the runtime-only razor is about, since every address
     * behind such a site is one a parse of the bytes recovers and this engine did not) and "the hook is not
     * wired". A floor over one spelling, with no containment against `called` in either direction: `called >
     * named` is ordinary (`window.fetch(u)` is a property read, `(function (fetch) {…})(self.fetch)` a local
     * slot), and so is `named > called`, also because every flow that replays a program recompiles it, so this
     * counts compiler resolutions, never source sites: a bit, not a magnitude, named `Named` rather than `Sites`.
     *
     * `named_typeof` is named for the operator: the unary parser patches an ordinary read into the non-throwing
     * form only for `typeof` (ECMAScript §13.5.3 "The typeof Operator" step 2.a answers an unresolvable reference
     * without throwing), so a bundle that only probes for `fetch` raises this row and not the one above. It is not
     * every guard shape: `window.fetch` and `"fetch" in self` are property reads and reach neither row. */
    long  named;
    long  named_typeof;
    /* And the property spelling of the same entry, a second fact about one occurrence. `globalThis.fetch(u)`
       resolves no scope for `fetch`: the member is a field get and only the base is a free identifier, so the row
       above is blind to it. Its own row, never summed into `named`, because the `typeof` split cannot be
       reproduced for a property (ECMAScript §13.5.3 "The typeof Operator" step 2.a needs a non-throwing read only
       for an unresolvable reference, and an absent property is `undefined`), so `typeof globalThis.fetch` emits
       the same field get as a use. A thin population can be the only door an app's API goes through, as with
       connect-es's `await (e.fetch ?? globalThis.fetch)(r.url, …)`. */
    long  named_prop;
    /* `called` is raised at the hosting machine's stage 0, one frame before `began` (raised at the member's
       first stage, IDL_STEP_FIRST). Stages 0 and 1 are the argument-count check and the ES-to-IDL conversions,
       both rest points, so a call that threw or parked in a conversion raises `called` and never `began`;
       that separates "the conversion died" from "nothing was called" within one run's census.
       Stage 0 runs once per call (its exits are two throws, and a park leaves the stage at 1), so `called`
       needs no one-time flag. `called` counts calls and `began` counts worlds: a fork at stage 1 (fetch's
       §3.2.18 enum fork over a RequestInit member, its §3.2.17 presence fork over an unknown init, a branch in
       a page getter the conversion runs) leaves stage 0 behind and gives each sibling its own begin against
       one call. So `called >= began` holds only where conversions cannot fork, and the fetch edge asserts
       only that a begin implies some call. */
    long  called;               /* ask: calls that entered the hosting machine's prologue (stage 0) */
    long  began;                /* ask: constructions that began (the machine's one-time capture) */
    long  reached;              /* ask: …of those, the ones whose construction completed (see above) */
    long  freed;                /* outcome: states torn down after a construction began */
    long  freed_reached;        /* outcome: …of those, the ones that had completed */
} EndpointEdge;

static EndpointEdge g_fetch_edge;
static EndpointEdge g_xhr_edge;
/* And the one row that is not a fact about any send state: core/xhr records from its lifecycle machine, after which
   the send state may be gone, so the edge's offer to this surface cannot sit in the struct above beside a partition
   it does not belong to. It carries the `offered <= asks` containment for that edge; endpoint.h states why no
   relation between it and `reached` may be asserted. */
static long  g_xhr_offered;

/* The counters back to zero, the allocation left alone: endpoint_init asserts the array absent rather than zeroing
   it (see its comment), so this resets what a new session owns and nothing else. */
static void edge_reset(EndpointEdge *e) {
    e->named = e->named_typeof = e->named_prop = 0;
    e->called = e->began = e->reached = e->freed = e->freed_reached = 0;
}

/* And the array given back and the table let go, in that order and both, because the next session's edge declares
   itself again at its own init: keeping the array would let a session's allocation outlive the session, and keeping
   the pointer while freeing the array would leave the next declare taking its early return and every teardown
   indexing a freed block. */
static void edge_release(EndpointEdge *e) {
    edge_reset(e);
    free(e->died);
    e->died = NULL;
    e->steps = NULL;
    /* And the entry name with the table, which scopes the compile report to the session: nothing matches an edge
       that has not declared, so a program compiled before this session opened or after it closed raises no row
       into it. A compile is the one event this file receives that a host can perform outside a session, so this is
       `g_boundary_spent`'s scope question answered by construction. */
    e->entry = NULL;
    e->stage_n = 0;
    e->stage_first = 0;
}

void endpoint_init(void) {
    g_eps = NULL; g_eps_n = 0; g_eps_cap = 0; g_suppress = 0;
    g_asks = g_asks_pre_program = g_ask_suppressed = g_ask_merged = g_ask_minted = 0;
    g_ask_merged_pre_program = 0;
    g_boundary_spent = 0;
    /* The edge's rows beside the ask rows they are read against. The partition array is asserted absent rather
       than zeroed: endpoint_free gives it back and lets the table go, so a session opening with one in hand has
       not had its predecessor torn down, and zeroing it would make that state produce a plausible census. */
    DCHECK(g_fetch_edge.died == NULL && g_fetch_edge.steps == NULL &&
           g_xhr_edge.died == NULL && g_xhr_edge.steps == NULL,
           "a session opened with the previous session's host edge table still in hand — endpoint_free is "
           "what gives the per-stage array back and lets the machine's label table go, so this is a host that "
           "opened a second session without tearing the first down, and every row this census is about to "
           "raise would be filed against an allocation the last session owned");
    edge_reset(&g_fetch_edge);
    edge_reset(&g_xhr_edge);
    g_xhr_offered = 0;
}
void endpoint_suppress(int on) { g_suppress = on ? 1 : 0; }

/* The address as the surface prints it: a concolic's shape, or a concrete URL's own bytes. A live concolic always
 * has a shape (concolic_alloc copies one for every value it mints and CHECKs the copy), so a missing one is
 * asserted: a fallback `{}` would be the unnameable hole path_scan mints nothing for, emitting a literal address
 * with no param under it.
 *
 * This `if` decides whether any param on the record can carry a domain. A path param exists only where the
 * returned string holds a nameable brace, and a query param carries a hole only where its value does, so
 * `valueClass`, the four domain reads it gates and what the trusted zone builds on them are all downstream of it.
 * lib/learn.js's live-traffic path walk can mint a `{path_*}` where two observed addresses of one record differ
 * at one segment, but it needs a second sighting and writes no `_astInferred`, so its hole carries no domain; the
 * only hole a first sighting can have is decided here. Three kinds of address reach the concrete branch:
 *   - composed of literals: a bundler's chunk and module addresses, in which no engine change mints a hole;
 *   - composed of the document's own address: core/dom/document.c's DocAddress holds a plain string where a
 *     response was fetched from it and core/frame/location.c's loc_of_address returns early on a concrete one, so
 *     HTML §7.2.4 "The Location interface"'s `origin`, `protocol`, `host`, `pathname` and `href` are concrete for
 *     the document this engine loaded, and concolic only after a route the run computed out of an unknown;
 *   - pinned and re-read: concretize-on-pin re-mints a proved source as the real value (concolic_new's and
 *     concolic_exotic_get's pin arms return pin_of's result, and pin_mint answers a primitive), so the true arm of
 *     `if (s.tier === 'silver') fetch('/api/' + s.tier)` composes an ordinary string. That is correct: @H emits a
 *     value the run determined.
 * A pin erases a hole only on a re-read. concolic_example asks of a value already in hand and leaves the record
 * untouched, so `var role = q("role"); if (role === "admin") fetch("...?role=" + role)` still holds the concolic:
 * this returns the shape, the param is minted, and the pinned bytes arrive as its example (concolic.h's last
 * clause). testing/corpus/control/gated-hole.js puts its equality rung's fetch on the `else` for this reason.
 * Which of the two a bundle writes is the bundle's choice, not the gate's. */
static char *url_display(JSContext *ctx, JSValueConst url) {
    if (concolic_is(url)) {
        const char *s = concolic_shape_c(url);
        DCHECK(s != NULL, "a concolic URL reached the @H surface with no display shape — every concolic is "
                          "minted with one, so this value lost its domain somewhere between its source and "
                          "this record, and the endpoint would be emitted as a literal address with no hole "
                          "in it and nothing to say a param went missing");
        return strdup(s);
    }
    const char *s = JS_ToCString(ctx, url);
    char *r = strdup(s ? s : "?");
    if (s) JS_FreeCString(ctx, s);
    return r;
}

/* Whether the run had determined this address: the product's hard bar, read off the test `url_display` makes. A
 * concolic `url` is an address some segment of which entered the program at a source and which therefore stands in
 * no served byte. No parser, no table of statable shapes, and nothing about the address's text is consulted.
 * Both concolic members are url_display's concolic branch, so `cls != EPA_CONCRETE` is exactly `concolic_is(url)`
 * and exactly "this row printed a shape"; the refinement answers a different question (did this flow determine
 * the symbolic value it holds, and whose unknown does it rest on) and changes no branch of url_display.
 *
 * The pin is asked through `concolic_example_state`, concolic.h's single decision, which `concolic_example` also
 * reads to mint the pinned bytes, so the value and the fact come out of one lookup; it is side-effect-free and
 * allocation-free, and it needs no running flow (it reads `g_pins` and its inherited chain, so with no flow
 * standing it reads the baseline, the sound answer for a record minted before any program ran). Three narrowings
 * keep `EPA_SOURCE_DETERMINED` rare: a pinned source re-read is a bare primitive, so the first line answers
 * `EPA_CONCRETE`; `src_self` is written only at the two mints that read a pin and never by `concolic_derived`, so
 * every composed address answers not-determined however its operand was pinned; and the holding arm of §7.2.13
 * IsLooselyEqual ( x, y ) writes `pinned_root` without a value. What remains is a source held in a page variable
 * before its own strict gate and passed bare, endpoint.h's worked example.
 *
 * Whose unknown it is (concolic.h's `concolic_root_whose_any`, a disjunction over the root set the mint states and
 * `derived_root_whose` unions at every join) separates three populations from the bar with one predicate: a hole
 * `engine_orphan_call` minted so a drive could happen at all (`{orphan<hex>.argN}`, an unknown nobody supplied);
 * a value whose bytes stand in the served document and is minted unknown so a gate over it forks (HTML §4.12.1's
 * data block, an injected global an inline script wrote: CONCOLIC_WHOSE_SERVED, stated by core/loader/data_block.c
 * and solver/absent.c's present arm); and one no mint could speak for. Each reads `EPA_UNKNOWN_UNPROVEN`. The
 * discriminator is the source identity, not the composition: `fetch(cfg.url)` emits `{__CFG.url}` with no page
 * literal and is entirely the page's, while `origin + "/" + <an orphan argument>` is page text around an engine
 * mint. The cheaper routes are wrong: `concolic_src_c` answers the first unknown operand's source, demoting a row
 * that mixes an orphan argument with a page unknown; keying on whether the driving flow is an orphan drive is wrong
 * for the product's best case (a drive that reaches `fetch("/api/" + <a page unknown>)` learned what the tool exists
 * to learn); a registry of minted identities in solver/engine.c matches only whole roots and is engine state a
 * resumed flow finds empty; a `{orphan` prefix test is a spelling. The example does not answer it either:
 * concolic.h grades a reply field HELD and an absent injected global NONE.
 *
 * Nothing is asserted about this operand: the address is composed of bytes a page wrote, so a DCHECK over its
 * shape, root or class would hand any document an abort switch, and whether a page re-reads a source is the
 * page's choice. Provenance threading is asserted at `concolic_alloc`.
 *
 * The bar is as good as what the mints stated, and one path loses the statement: a value somebody outside this
 * engine supplied whose whose-mask was lost reads `EPA_UNKNOWN_UNPROVEN`, an under-claim. The real path is §2.7.8
 * StructuredDeserialize's rebuild, which has the shape and source but mints CONCOLIC_WHOSE_UNSTATED; the gap is
 * owned by core/structured_clone.c's named residual at that mint, and this line reads the fix for free. Guessing
 * WORLD here instead would put a rebuilt orphan argument back on the bar.
 * Named residual. Not covered: the census publishes the address class and the witness class as two histograms, so
 * how many concolic rows were `unasked` is an inequality between marginals; the per-endpoint row carries both
 * adjacently, but no census joins them. Next diff builds: one keyed census row joining the two, as
 * testing/corpus/site.mjs keys `door|witnessClass`. Absence shows as: a census reader bounding the `unasked`
 * concolic population from two histograms instead of reading it. */
static int address_class_of(JSValueConst url) {
    if (!concolic_is(url)) return EPA_CONCRETE;
    if (concolic_example_state(url) == CONCOLIC_EX_DETERMINED) return EPA_SOURCE_DETERMINED;
    /* And whose unknown it rests on: `_any`, never `_all`, since one root somebody outside this engine supplied is
       enough however many engine-minted holes stand beside it, and `_all` would refuse a mixed set the run did
       prove, an invisible under-claim. One read separates all three non-WORLD populations, so a second read beside
       it would be two answers to one question. Nothing is asserted about the answer: the pair
       `!!root == !!root_whose` is asserted at `concolic_alloc`, and this answer's mapping is asserted at the one
       call site, where a door is in hand. */
    if (!concolic_root_whose_any(url, CONCOLIC_WHOSE_WORLD)) return EPA_UNKNOWN_UNPROVEN;
    return EPA_UNKNOWN;
}

/* The params of one observed request, in the order a reviewer meets them (path, query, body), owned until merged
   into the surface. `excl`, `bnd`, `pred` and `leq` are owned copies of what the running flow's path constraint held
   when the param was read: the constraint is a growable head that a later narrowing reallocs, and
   concolic_excluded, concolic_bound_read, concolic_strpred_read and concolic_looseeq_read hand back borrowed rows
   from it, so copying keeps the lifetime safe across every future edit to the producers. `has_hole` is copied from
   nothing: it is whether the caller had a hole key to ask with, known at the call and nowhere after. */
typedef struct { char *name; char *val; EpLoc loc; char **excl; int nexcl; ParamBound bnd;
                 ConcolicPred *pred; int npred;
                 ConcolicLooseEq *leq; int nleq; int has_hole; } KV;
typedef struct { KV *e; int n, cap; } KvBuf;

/* One copy and one disposer for a set of them. What a single row owns is concolic.c's to say (concolic_pred_copy /
   concolic_pred_release) and is never re-spelled here, so a field added to ConcolicPred has one place it is copied
   and one where it is released; this file owns the array around the rows. */
static ConcolicPred *param_pred_copy(const ConcolicPred *src, int n) {
    ConcolicPred *out;
    int i;

    if (n <= 0) return NULL;
    out = malloc((size_t)n * sizeof(ConcolicPred));
    CHECK(out, "endpoint: OOM taking a param's observed call predicates off the flow");
    for (i = 0; i < n; i++) concolic_pred_copy(&out[i], &src[i]);
    return out;
}

static void param_pred_free(ConcolicPred *p, int n) {
    int i;
    for (i = 0; i < n; i++) concolic_pred_release(&p[i]);
    free(p);
}

/* The same pair over the loose-equality row, for the same reason: concolic.c owns a row (concolic_looseeq_copy /
   concolic_looseeq_release), this file the array. */
static ConcolicLooseEq *param_leq_copy(const ConcolicLooseEq *src, int n) {
    ConcolicLooseEq *out;
    int i;

    if (n <= 0) return NULL;
    out = malloc((size_t)n * sizeof(ConcolicLooseEq));
    CHECK(out, "endpoint: OOM taking a param's observed loose equalities off the flow");
    for (i = 0; i < n; i++) concolic_looseeq_copy(&out[i], &src[i]);
    return out;
}

static void param_leq_free(ConcolicLooseEq *p, int n) {
    int i;
    for (i = 0; i < n; i++) concolic_looseeq_release(&p[i]);
    free(p);
}

/* Which two loose equalities are one is asked at both ends (concolic.c dedups within a flow, this file intersects
   across sightings), so it is asked of one speller, concolic_looseeq_same, for param_pred_same's reason. */
static int param_leq_same(const ConcolicLooseEq *a, const ConcolicLooseEq *b) {
    return concolic_looseeq_same(a, (ConcolicLit)b->kind, b->tok);
}

/* Which two predicates are one is asked at both ends (concolic.c dedups within a flow, this file intersects across
   sightings), so it is asked of one speller, concolic_pred_same. Two spellings could disagree, and an intersection
   that thought two identical predicates different would drop a claim every observed path obeyed. */
static int param_pred_same(const ConcolicPred *a, const ConcolicPred *b) {
    return concolic_pred_same(a, b->method, (const char *const *)b->args, b->nargs, b->holds);
}

/* `hole` is the value's hole key (the name the flow's own equality gates recorded their domain under), or NULL when
   the value is a concrete one the code computed. Passed explicitly by each producer, because they know it from
   different places: a path param is its hole (the brace-stripped segment), while a query or body param carries its
   hole inside the value text. Deriving it from one would answer NULL for the other, and a NULL hole is
   indistinguishable from an unconstrained one. */
static void kv_add(KvBuf *b, const char *name, size_t nlen, const char *val, size_t vlen, EpLoc loc,
                   const char *hole) {
    DCHECK(nlen > 0, "an endpoint param was minted with no NAME — a nameless param cannot be substituted back "
                     "into a request, so every producer here must refuse the unnamed case at its own site "
                     "rather than emit a record keyed by the empty string");
    /* Where it landed is asserted at the mint: a param whose location is not one of the three is a producer that
       built a param without saying what it is, and a consumer defaulting it (`p.location || "query"`) would hide
       that. Every site that adds a param passes through here. */
    DCHECK(loc == EP_QUERY || loc == EP_PATH || loc == EP_BODY,
           "an endpoint param was minted with no LOCATION — a param the reviewer cannot place is a param "
           "that cannot be replayed, and a consumer defaulting it reads every one of them as a query param");
    /* A path param always has a hole: `path_scan` mints a param only for a segment holding a brace and passes the
       brace-stripped segment as the key, so this holds by construction, and asserting it makes a `location:"path"`
       row a denominator a consumer can trust and holds the next producer of a path param to it. The query and body
       producers have no such invariant (`concolic_hole_key` answers NULL for a brace-free value), which is what
       `has_hole` is for. */
    DCHECK(loc != EP_PATH || hole != NULL,
           "a PATH param was minted with no hole key — path_scan mints one only for a braced segment and "
           "passes that segment's brace-stripped name as the key, so a path param with no hole is a second "
           "producer that learned to mint a path param and not to name the hole it stands for, and every "
           "domain read below would be silently skipped for it");
    if (b->n >= b->cap) { b->cap = b->cap ? b->cap * 2 : 8; b->e = realloc(b->e, (size_t)b->cap * sizeof(KV)); CHECK(b->e, "endpoint: OOM params"); }
    b->e[b->n].name = malloc(nlen + 1); CHECK(b->e[b->n].name, "endpoint: OOM param name");
    memcpy(b->e[b->n].name, name, nlen); b->e[b->n].name[nlen] = 0;
    b->e[b->n].val = malloc(vlen + 1); CHECK(b->e[b->n].val, "endpoint: OOM param value");
    if (vlen) memcpy(b->e[b->n].val, val, vlen);
    b->e[b->n].val[vlen] = 0;
    b->e[b->n].loc = loc;
    /* And whether there was anything to ask with, written before the four reads that depend on it. Each is gated
       on `hole`, so a NULL one makes all four answer nothing, which the emission spells as "no gate of that kind
       held on every observed path": true of a param whose hole was never narrowed (more gates wanted) and false of
       a param that had no hole (nothing wanted). The flag lets a consumer tell them apart, and it is the same
       `hole` the reads use, so they cannot disagree. */
    b->e[b->n].has_hole = hole ? 1 : 0;
    /* The domain is read here because the flow still exists here: the path constraint is per flow, and the
       serializer runs at the end of the run with no flow under it. */
    b->e[b->n].excl = NULL;
    b->e[b->n].nexcl = 0;
    if (hole) {
        int nex = 0, k;
        const char *const *ex = concolic_excluded(hole, &nex);
        if (nex > 0) {
            b->e[b->n].excl = malloc((size_t)nex * sizeof(char *));
            CHECK(b->e[b->n].excl, "endpoint: OOM taking a param's observed domain off the flow");
            for (k = 0; k < nex; k++) {
                b->e[b->n].excl[k] = strdup(ex[k]);
                CHECK(b->e[b->n].excl[k], "endpoint: OOM copying a value a flow proved a param is not");
            }
            b->e[b->n].nexcl = nex;
        }
    }
    /* And the interval, off the same flow at the same instant: without it a param gated `x > 5` and one nothing
       ordered render the same bytes, read as "anything goes", a wrong report rather than a partial one, for the
       second-largest gate class in a real minified bundle. */
    memset(&b->e[b->n].bnd, 0, sizeof b->e[b->n].bnd);
    if (hole) {
        ConcolicBound cb;
        if (concolic_bound_read(hole, &cb)) {
            b->e[b->n].bnd.has_lo = cb.has_lo; b->e[b->n].bnd.has_hi = cb.has_hi;
            b->e[b->n].bnd.lo = cb.lo;         b->e[b->n].bnd.hi = cb.hi;
            b->e[b->n].bnd.lo_incl = cb.lo_incl; b->e[b->n].bnd.hi_incl = cb.hi_incl;
            if (cb.has_lo) {
                b->e[b->n].bnd.lo_txt = strdup(cb.lo_txt);
                CHECK(b->e[b->n].bnd.lo_txt, "endpoint: OOM copying the bound a flow proved a param obeys");
            }
            if (cb.has_hi) {
                b->e[b->n].bnd.hi_txt = strdup(cb.hi_txt);
                CHECK(b->e[b->n].bnd.hi_txt, "endpoint: OOM copying the bound a flow proved a param obeys");
            }
        }
    }
    /* And the call predicates, likewise: a param whose only gate was `path.startsWith("/api")` (@H's headline
       example, the gate class an equality's pin and an ordering's interval both miss) would otherwise render as one
       nothing tested. */
    b->e[b->n].pred = NULL;
    b->e[b->n].npred = 0;
    if (hole) {
        int npr = 0;
        const ConcolicPred *pr = concolic_strpred_read(hole, &npr);
        if (npr > 0) {
            b->e[b->n].pred = param_pred_copy(pr, npr);
            b->e[b->n].npred = npr;
        }
    }
    /* And the loose equalities that held, likewise: the arm §7.2.13 IsLooselyEqual ( x, y ) leaves undetermined,
       which concolic_pin refuses to pin, so a param whose only gate was `x == 0` would render as untested while its
       sibling flow carries an exclusion. */
    b->e[b->n].leq = NULL;
    b->e[b->n].nleq = 0;
    if (hole) {
        int nlq = 0;
        const ConcolicLooseEq *lq = concolic_looseeq_read(hole, &nlq);
        if (nlq > 0) {
            b->e[b->n].leq = param_leq_copy(lq, nlq);
            b->e[b->n].nleq = nlq;
        }
    }
    b->n++;
}

/* One disposer for one interval, because two places hold one (a request's KV row and the endpoint's merged Param),
   and a side added to ParamBound must be freed at both. */
static void param_bound_free(ParamBound *b) {
    free(b->lo_txt); free(b->hi_txt);
    memset(b, 0, sizeof *b);
}

static void kv_free(KvBuf *b) {
    for (int i = 0; i < b->n; i++) {
        free(b->e[i].name); free(b->e[i].val);
        for (int k = 0; k < b->e[i].nexcl; k++) free(b->e[i].excl[k]);
        free(b->e[i].excl);
        param_bound_free(&b->e[i].bnd);
        param_pred_free(b->e[i].pred, b->e[i].npred);
        param_leq_free(b->e[i].leq, b->e[i].nleq);
    }
    free(b->e); b->e = NULL; b->n = b->cap = 0;
}

/* One `name=value` pair of a `&`-separated list, read in place: `p` is a position in the list, and the return is the
   next pair's position, or NULL at the end. Name and value are borrowed spans; a pair with no `=` has a zero-length
   value (what `?flag` sends), and an empty name is left for the caller to refuse (kv_add asserts it). In place,
   because the two spellings of one address are walked together and `strtok` keeps its state in the string it
   destroys. */
static const char *kv_pair_next(const char *p, const char **nm, size_t *nn, const char **vl, size_t *vn) {
    const char *amp, *eq;
    size_t plen;

    while (*p == '&') p++;      /* `&&`, and a trailing `&`: no pair — the same runs strtok collapsed */
    if (!*p) return NULL;
    amp = strchr(p, '&');
    plen = amp ? (size_t)(amp - p) : strlen(p);
    eq = (const char *)memchr(p, '=', plen);
    *nm = p;
    *nn = eq ? (size_t)(eq - p) : plen;
    *vl = eq ? eq + 1 : p + plen;
    *vn = eq ? plen - (size_t)(eq - p) - 1 : 0;
    return p + plen;
}

/* One `&`-separated pair of one address half, borrowed out of the string it was split from. `nm`/`vl` are the spans
   `kv_pair_next` reads; `end` is the byte after the pair's last, so a run of pairs is the bytes from the first pair's
   value start to the last pair's `end`, exactly as the code computed them. */
typedef struct { const char *end, *nm, *vl; size_t nn, vn; } Pair;

/* The pairs of one address half, malloc'd, from one walk, so the aligner below and the emission that consumes its
   answer index the same split. An empty list is zero pairs and a run of `&` is no pair, the arithmetic
   `kv_pair_next` performs. */
static Pair *kv_split(const char *s, int *n_out) {
    int n = 0, i = 0;
    const char *p = s, *next;
    const char *nm, *vl;
    size_t nn, vn;
    Pair *v;

    while ((p = kv_pair_next(p, &nm, &nn, &vl, &vn)) != NULL) n++;
    v = malloc((size_t)(n ? n : 1) * sizeof *v);
    CHECK(v, "endpoint: OOM splitting an address half into query pairs");
    p = s;
    while ((next = kv_pair_next(p, &nm, &nn, &vl, &vn)) != NULL) {
        DCHECK(i < n,
               "a query list split into more pairs than its own counting walk found — the count and the fill "
               "are two readings of one string and the array is sized by the first, so a further pair here "
               "is this walk writing past its end");
        v[i].end = next; v[i].nm = nm; v[i].nn = nn; v[i].vl = vl; v[i].vn = vn;
        i++;
        p = next;
    }
    DCHECK(i == n,
           "a query list filled a different number of pairs than its own separator walk counted — a "
           "disagreement means this walk left a row nobody filled and the aligner would read it");
    *n_out = n;
    return v;
}

/* The end of a run of `npair` whole pairs starting at `b`, with the first pair's name and value spans written out.
   It walks the splitter the alignment was built from, so a pair boundary is read one way; a span asked for was
   proved against this string, so a disagreement is a DCHECK. */
static const char *kv_run_end(const char *b, int npair,
                              const char **nm, size_t *nn, const char **vl, size_t *vn) {
    const char *p = b;
    int k;

    DCHECK(npair >= 1,
           "a query pair run was asked for no pairs at all — every span `query_align` records is at least "
           "one, a literal covering exactly one pair and a hole covering the value that stood there");
    for (k = 0; k < npair; k++) {
        const char *xn = NULL, *xv = NULL, *next;
        size_t xnn = 0, xvn = 0;

        next = kv_pair_next(p, &xn, &xnn, &xv, &xvn);
        DCHECK(next != NULL,
               "a query pair run ran off the end of the example it was measured against — `query_align` "
               "proved this span against this same string, so a run wanting more pairs than the string holds "
               "is that proof and this walk reading two different addresses");
        if (!next) break;
        if (k == 0) { *nm = xn; *nn = xnn; *vl = xv; *vn = xvn; }
        p = next;
    }
    return p;
}

/* Does the example's query line up with the shape's, in exactly one way? `path_align`'s question asked of the other
 * half of the address and answered by the same table, because the two strings are one concatenation rendered twice
 * and a hole's value may be read across only where everything around it agrees. Every hole-free shape pair must
 * stand over an example pair with the same name and value, and every pair whose value holds a hole covers one or
 * more whole example pairs: at least one, because a value that computed to the empty string still occupies its
 * pair, and more than one wherever that value holds a `&`. The run's first pair carries the param's own name, the
 * anchor the path half lacks: `b={x}` over `b=p&q=r` reads the hole as `p&q=r` and nothing else. The multi-pair
 * hole is the product's own case: `fetch("/s?" + new URLSearchParams(f))` displays as `/s?q={f}` and computes
 * `/s?q=a&page=2`. Ambiguity is a refusal, never a guess (an invented value is a wrong report); the count of
 * readings is saturated at two. Where the pair counts are equal every hole is forced to one pair, so the only
 * reading is the 1:1 walk (name equality, and value equality where the shape's value holds no brace), which the
 * recovery asserts.
 * Named residual. Not covered: a shape pair whose name holds a brace (`{k}=1`, what `"?" + key + "=1"` displays
 * as) takes the literal arm, matches no example pair, and the query aligns nowhere. Next diff builds, as one
 * landing: an aligner arm for a name-hole pair, whose run's last pair is anchored by the shape's next literal pair,
 * and a consumer that substitutes a parameter name (lib/popup-form.js's collectFormValues sends a name verbatim,
 * and applyPathParams substitutes only the path); the arm alone would emit a param named `{k}` and fabricate a
 * request, and the consumer alone would be a reader with no writer. Absence shows as: an address whose query
 * params are all emitted with no example while its path params carry theirs, on a record whose shape spells a
 * param name inside braces.
 * Returns a malloc'd span per shape pair and writes the shape's pair count, or NULL when there is no reading or
 * more than one. A shape with no pairs has the one empty reading and returns a non-NULL array. */
static int *query_align(const char *shape, const char *ex, int *nshape) {
    int n = 0, m = 0, i, j, k;
    Pair *S = kv_split(shape, &n);
    Pair *E = kv_split(ex, &m);
    /* ways[i][j]: how many readings match the shape's pairs from `i` onward against the example's from `j`
       onward, saturated at two. Filled backwards so each row reads only the row below it. */
    unsigned char *w = calloc((size_t)(n + 1) * (size_t)(m + 1), 1);
    int *span = NULL;

    CHECK(w, "endpoint: OOM aligning a request's query against the concrete URL a flow computed");
    w[(size_t)n * (size_t)(m + 1) + (size_t)m] = 1;   /* both exhausted together: one reading, the empty one */
    for (i = n - 1; i >= 0; i--) {
        unsigned char *cur = w + (size_t)i * (size_t)(m + 1);
        const unsigned char *nxt = cur + (m + 1);
        if (memchr(S[i].vl, '{', S[i].vn)) {
            unsigned suf = 0;                        /* the saturated sum of nxt[j + 1 .. m] */
            for (j = m; j >= 0; j--) {
                int anchored = j < m && S[i].nn == E[j].nn && !memcmp(S[i].nm, E[j].nm, S[i].nn);
                cur[j] = anchored ? (unsigned char)(suf > 2 ? 2 : suf) : 0;
                suf += nxt[j];
                if (suf > 2) suf = 2;
            }
        } else {
            for (j = 0; j <= m; j++)
                cur[j] = (j < m && S[i].nn == E[j].nn && !memcmp(S[i].nm, E[j].nm, S[i].nn)
                          && S[i].vn == E[j].vn && !memcmp(S[i].vl, E[j].vl, S[i].vn)) ? nxt[j + 1] : 0;
        }
    }
    if (w[0] == 1) {
        span = malloc((size_t)(n ? n : 1) * sizeof *span);
        CHECK(span, "endpoint: OOM recording how many example pairs each hole of a query stands over");
        j = 0;
        for (i = 0; i < n; i++) {
            const unsigned char *cur = w + (size_t)i * (size_t)(m + 1);
            const unsigned char *nxt = cur + (m + 1);
            /* Uniqueness is what makes this walk a read rather than a choice, so it is asserted at every step, as
               in `path_align`. */
            DCHECK(cur[j] == 1,
                   "a query alignment with exactly one reading reached a state holding a different number of "
                   "them — this walk is then choosing between readings instead of following the one the "
                   "count proved, and a hole would carry bytes another reading gives to its neighbour");
            if (!memchr(S[i].vl, '{', S[i].vn)) {
                span[i] = 1;
            } else {
                for (k = j + 1; k <= m; k++) if (nxt[k]) break;
                DCHECK(k <= m,
                       "a hole on the one reading of a query alignment stands over no example pair at all — "
                       "the table said this state had a reading and the row below it holds none, so the "
                       "count and the recovery are two walks over one table that disagree");
                span[i] = k - j;
            }
            DCHECK(n != m || span[i] == 1,
                   "a query whose shape and example hold the SAME number of pairs aligned with a hole "
                   "standing over more than one of them — a literal covers exactly one and a hole at least "
                   "one, so equal counts admit the 1:1 reading and no other, and this is an answer that was "
                   "never in question moving under a widening made only for the unequal case");
            j += span[i];
        }
        /* The parts sum to the total, the identity a reader of this array can check without re-deriving the
           table, which makes a span a fact about the example. */
        DCHECK(j == m,
               "a query alignment's spans do not cover the example's pairs exactly — their sum is what says "
               "each hole's value is the bytes that stood under it, so a short or long total means some param "
               "carries a slice of its neighbour's value");
    }
    free(w); free(S); free(E);
    *nshape = n;
    return span;
}

/* `a=1&b=2`: the query string's grammar and `application/x-www-form-urlencoded`'s, which are one grammar. Nothing
   is percent-decoded: the codecs are the engine's builtins, and a value the page encoded is the value it sends.
   `ex` is the same list rendered from the concrete example, or NULL where the value carried none. A param's hole
   key (what the domain, exclusions and bounds are looked up by) is read off the shape, and the value emitted where
   the shape holds a hole is the example the interpreter computed: the real operation on real operands, which has
   no example where any operand has none (concolic.h), so a gate-contradicted arm arrives exampleless and falls
   back to the shape. path_scan performs the same read for a path segment. */
static void kv_pairs(KvBuf *b, const char *text, const char *ex, EpLoc loc) {
    int npair = 0, i = 0;
    /* NULL for both ways there is nothing to align against (no example, or no unique reading), one answer to this
       walk: it has no bytes to attribute to a hole. `query_align`'s banner tells them apart. */
    int *span = ex ? query_align(text, ex, &npair) : NULL;
    const char *a = text, *e = ex;

    for (;;) {
        const char *nm, *vl, *en = NULL, *ev = NULL, *na, *ne = NULL;
        size_t nn, vn, enn = 0, evn = 0;
        char *shapeval, *hole;

        na = kv_pair_next(a, &nm, &nn, &vl, &vn);
        if (!na) break;
        a = na;
        /* The two walks are one rule, held to it here: `query_align` established the pairing with the same
           splitter, and asserting it again stops the alignment test and the emission drifting into two readings
           of one list, the only way a value could be attributed to the wrong param. */
        if (span) {
            DCHECK(i < npair,
                   "the shape walk ran past the pair count its own alignment was built from — `kv_split` and "
                   "this loop split ONE string and the span array is sized by the first, so a further pair "
                   "here is this walk reading past the end of that array");
            ne = kv_run_end(e, span[i], &en, &enn, &ev, &evn);
            DCHECK(enn == nn && !memcmp(en, nm, nn),
                   "an aligned example query's pair names a different param than the shape's at the same "
                   "position — the example value would be attributed to a param the code never computed it "
                   "for, which is a fabricated @H value and not a thin one");
            /* The run's bytes, not its first pair's value: a hole over more than one example pair computed a value
               holding the separator, so its value is every byte from its own value's start to the run's end, which
               is `evn` when the span is one. */
            evn = (size_t)(ne - ev);
            e = ne;
            i++;
        }
        if (!nn) continue;   /* `&&` / a leading `=`: no name, so no param — see kv_add's assert */
        shapeval = malloc(vn + 1);
        CHECK(shapeval, "endpoint: OOM copying a query value's display form");
        if (vn) memcpy(shapeval, vl, vn);
        shapeval[vn] = 0;
        hole = concolic_hole_key(shapeval);   /* the domain is a fact about the shape, whatever the value is */
        if (span && memchr(vl, '{', vn))
            kv_add(b, nm, nn, ev, evn, loc, hole);
        else
            kv_add(b, nm, nn, vl, vn, loc, hole);
        free(hole);
        free(shapeval);
    }
    DCHECK(!span || i == npair,
           "the shape walk visited a different number of pairs than the alignment was built over — the span "
           "array is indexed by this loop and sized by `kv_split`, so a short walk leaves a hole reading a "
           "span that belongs to another pair the next time this address is scanned");
    free(span);
}

/* The path half of a display URL (everything before `?`), malloc'd. */
static char *url_path_of(const char *disp) {
    const char *q = strchr(disp, '?');
    size_t plen = q ? (size_t)(q - disp) : strlen(disp);
    char *path = malloc(plen + 1); CHECK(path, "endpoint: OOM path"); memcpy(path, disp, plen); path[plen] = 0;
    return path;
}

/* The concrete URL the code computed, beside the shape that names its holes (the third member of the triple):
   without it every templated path would be a row of holes with no value. NULL means there is none to align
   against: a concrete URL is its own address with no holes, and a concolic one with no computed example is
   example-free. */
static char *url_example(JSContext *ctx, JSValueConst url) {
    JSValue ex;
    const char *s;
    char *r;

    if (!concolic_is(url)) return NULL;
    ex = concolic_example(ctx, url);
    if (JS_IsUndefined(ex)) { JS_FreeValue(ctx, ex); return NULL; }
    /* The example is the address the interpreter computed, so it is a primitive that converts: an object is the
       `+` propagation having handed on a wrapper, and a symbol would throw inside the conversion with nobody to
       hand the exception to. Both are this engine's logic being wrong, so it is asserted, and the conversion can
       fail only on OOM. */
    DCHECK(!JS_IsObject(ex) && !JS_IsSymbol(ex),
           "a URL's concolic EXAMPLE is not a primitive — the example rides the value the interpreter "
           "actually computed, so a wrapper or a symbol here is that propagation having lost the address");
    s = JS_ToCString(ctx, ex);
    CHECK(s, "endpoint: OOM rendering the concrete URL a flow computed");
    r = strdup(s);
    CHECK(r, "endpoint: OOM copying the concrete URL a flow computed");
    JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, ex);
    return r;
}

/* One `/`-separated segment of an address, borrowed out of the string it was split from. */
typedef struct { const char *p; size_t n; } Seg;

/* The segments of one address half, malloc'd, from one walk, so the aligner and the scan that consumes its answer
   index the same split. An address with no `/` is one segment and an empty one is one empty segment, as the
   `strchr` walks compute. */
static Seg *seg_split(const char *s, int *n_out) {
    int n = 1, i = 0;
    const char *p = s, *e;
    Seg *v;

    for (e = s; *e; e++) if (*e == '/') n++;
    v = malloc((size_t)n * sizeof *v);
    CHECK(v, "endpoint: OOM splitting an address into segments");
    for (;;) {
        e = strchr(p, '/');
        v[i].p = p;
        v[i].n = e ? (size_t)(e - p) : strlen(p);
        i++;
        if (!e) break;
        p = e + 1;
    }
    DCHECK(i == n,
           "an address split into a different number of segments than its own separator count — the count "
           "and the walk are two readings of one string and the array is sized by the first, so a "
           "disagreement means this walk wrote past its end or left a row nobody filled");
    *n_out = n;
    return v;
}

/* The end of a run of `nseg` whole segments starting at `b`: the separator that terminates it, or NULL when the run
   reaches the end of the string. It counts separators rather than chasing `strchr` results so that no build can
   dereference a NULL; a span asked for was proved against this string, so a disagreement is a DCHECK. */
static const char *seg_run_end(const char *b, int nseg) {
    const char *e;

    DCHECK(nseg >= 1,
           "a segment run was asked for no segments at all — every span `path_align` records is at least one, "
           "a literal covering exactly one segment and a hole covering the value that stood there");
    for (e = b; *e; e++) {
        if (*e != '/') continue;
        if (--nseg == 0) return e;
    }
    DCHECK(nseg == 1,
           "a segment run ran off the end of the example it was measured against — `path_align` proved this "
           "span against this same string, so a run wanting more segments than the string holds is that "
           "proof and this walk reading two different addresses");
    return NULL;
}

/* Does the example's path line up with the shape's, in exactly one way? The alignment is what makes a hole's value
 * a measurement rather than a guess, so it is checked: every hole-free shape segment must be byte-equal to the
 * example segment under it, and every hole covers one or more whole example segments, at least one because a value
 * that computed to the empty string still occupies its segment, and more than one wherever that value holds a `/`.
 * The multi-segment hole is the product's own case: `fetch(cfg.apiBase + "/v1/users/" + id)` displays as
 * `{cfg.apiBase}/v1/users/{id}` and computes `https://api.acme.com/v1/users/42`, so its first hole stands over three
 * example segments; an equal-count matcher would leave every param on the address example-free, while
 * `https://{cfg.host}/v1/users/{id}`, which differs only in where the hole starts, would align.
 * Ambiguity is a refusal, never a guess: `{a}/{b}` over `x/y/z` has two readings and nothing at this layer can know
 * which the code computed, and an invented value is a wrong report, so the alignment is taken only where the
 * reading is unique. The count of readings is saturated at two. Where the segment counts are equal every hole is
 * forced to one segment, so the only reading is the 1:1 walk; the recovery asserts that, so a later change to the
 * table cannot move an answer that was never in question.
 * Returns a malloc'd span per shape segment and writes the shape's segment count, or NULL when there is no reading
 * or more than one. */
static int *path_align(const char *shape, const char *ex, int *nshape) {
    int n = 0, m = 0, i, j, k;
    Seg *S = seg_split(shape, &n);
    Seg *E = seg_split(ex, &m);
    /* ways[i][j]: how many readings match the shape's segments from `i` onward against the example's from `j`
       onward, saturated at two. Filled backwards so each row reads only the row below it. */
    unsigned char *w = calloc((size_t)(n + 1) * (size_t)(m + 1), 1);
    int *span = NULL;

    CHECK(w, "endpoint: OOM aligning a request's shape against the concrete URL a flow computed");
    w[(size_t)n * (size_t)(m + 1) + (size_t)m] = 1;   /* both exhausted together: one reading, the empty one */
    for (i = n - 1; i >= 0; i--) {
        unsigned char *cur = w + (size_t)i * (size_t)(m + 1);
        const unsigned char *nxt = cur + (m + 1);
        if (memchr(S[i].p, '{', S[i].n)) {
            unsigned suf = 0;                        /* the saturated sum of nxt[j + 1 .. m] */
            for (j = m; j >= 0; j--) {
                cur[j] = (unsigned char)(suf > 2 ? 2 : suf);
                suf += nxt[j];
                if (suf > 2) suf = 2;
            }
        } else {
            for (j = 0; j <= m; j++)
                cur[j] = (j < m && S[i].n == E[j].n && !memcmp(S[i].p, E[j].p, S[i].n)) ? nxt[j + 1] : 0;
        }
    }
    if (w[0] == 1) {
        span = malloc((size_t)n * sizeof *span);
        CHECK(span, "endpoint: OOM recording how many example segments each hole of an address stands over");
        j = 0;
        for (i = 0; i < n; i++) {
            const unsigned char *cur = w + (size_t)i * (size_t)(m + 1);
            const unsigned char *nxt = cur + (m + 1);
            /* Uniqueness is what makes this walk a read rather than a choice, so it is asserted at every step:
               `w[0] == 1` says the whole match has one reading, so every state on it has one too (a hole's `cur[j]`
               is the saturated sum over the states it can hand on to, so a 1 is exactly one live successor). */
            DCHECK(cur[j] == 1,
                   "an address alignment with exactly one reading reached a state holding a different number "
                   "of them — this walk is then choosing between readings instead of following the one the "
                   "count proved, and a hole would carry bytes another reading gives to its neighbour");
            if (!memchr(S[i].p, '{', S[i].n)) {
                span[i] = 1;
            } else {
                for (k = j + 1; k <= m; k++) if (nxt[k]) break;
                DCHECK(k <= m,
                       "a hole on the one reading of an address alignment stands over no example segment at "
                       "all — the table said this state had a reading and the row below it holds none, so "
                       "the count and the recovery are two walks over one table that disagree");
                span[i] = k - j;
            }
            /* And the equal-count case is the 1:1 walk, asserted, since that is why this matcher answers
               byte-identically to an equal-count one for every address such a matcher aligns. */
            DCHECK(n != m || span[i] == 1,
                   "an address whose shape and example hold the SAME number of segments aligned with a hole "
                   "standing over more than one of them — a literal covers exactly one and a hole at least "
                   "one, so equal counts admit the 1:1 reading and no other, and this is an answer that was "
                   "never in question moving under a widening made only for the unequal case");
            j += span[i];
        }
        /* The parts sum to the total, the identity a reader of this array can check without re-deriving the table:
           a walk that lost or double-counted a segment would hand `path_scan` a value starting or ending mid-run. */
        DCHECK(j == m,
               "an address alignment's spans do not cover the example's segments exactly — their sum is what "
               "says each hole's value is the bytes that stood under it, so a short or long total means some "
               "param carries a slice of its neighbour's value");
    }
    free(w); free(S); free(E);
    *nshape = n;
    return span;
}

/* An unknown path segment is one path parameter, atomic in the template. `/v1/users/{state}.id/posts` has one, and
   its value is the run of example segments `path_align` proved it stands over: one segment there (`42`), more
   wherever the computed value holds a `/`. The template is built from the shape alone, so nothing the example does
   moves a hole's boundaries, and the identity `endpoint_mark_asset` recomputes through this function with no
   example cannot move either. The braces do not delimit the unknown: `concolic_exotic_get` composes a member read's
   display as `"%s.%s"` over the parent's, so `state.id` displays as `{state}.id`, braces around the root source and
   the member path outside them, and treating `}` as the unknown's end would ask the example segment to end in a
   literal `.id`. So the segment is re-spelled as one hole (braces stripped, one pair around the whole), because the
   consumer's substitution grammar is `/\{([^}\/]+)\}/` (lib/popup-form.js's applyPathParams): `{state}.id` becomes
   `{state.id}`. A literal prefix in the segment (`u{state}.id`) is folded into the hole, since where the literal
   ends is invisible at this layer and the example carries the whole segment. An unnameable hole (`{}`, a concolic
   with no shape) mints nothing and keeps its shape in the address, as lib/learn.js's reconcile refuses the same
   segment. Returns the re-spelled path, malloc'd. */
static char *path_scan(KvBuf *out, const char *shape, const char *ex) {
    int nseg = 0, i = 0;
    /* NULL for both ways there is nothing to align against (no example, or no unique reading), one answer to this
       walk: it has no bytes to attribute to a hole. `path_align`'s banner tells them apart. */
    int *span = ex ? path_align(shape, ex, &nseg) : NULL;
    const char *a = shape, *b = ex;
    /* core/json_buf.h's growing byte buffer, appended raw (a private one here would be the copy that header
       deleted); nothing here is JSON, and the emit quotes this string later. */
    JsonBuf p = { 0 };

    for (;;) {
        const char *ae = strchr(a, '/');
        const char *be = NULL;
        size_t an = ae ? (size_t)(ae - a) : strlen(a);
        size_t bn = 0, nlen = 0;
        char *seg, *name;

        if (span) {
            DCHECK(i < nseg,
                   "the shape walk ran past the segment count its own alignment was built from — `seg_split` "
                   "and this loop split ONE string and the span array is sized by the first, so a further "
                   "segment here is this walk reading past the end of that array");
            be = seg_run_end(b, span[i]);
            bn = be ? (size_t)(be - b) : strlen(b);
            /* The example runs out exactly when the shape does, asserted where the two walks are in one hand, as
               `kv_pairs` does for the query half: `path_align` proved the spans cover every example segment, so the
               last shape segment's run reaches the example's end and no earlier one does, and a run ending early
               would hand this hole its neighbour's bytes. */
            DCHECK(!ae == !be,
                   "an aligned example path ran out of segments before its shape did, or held segments the "
                   "shape's last one did not cover — `path_align` proved the spans sum to the example's own "
                   "segment count, so these two walks disagreeing is that proof and this walk having parted");
        }
        seg = malloc(an + 1); CHECK(seg, "endpoint: OOM copying a path segment");
        memcpy(seg, a, an); seg[an] = 0;
        /* The name is the hole key on this path, so it is spelled by the one speller: the emission rests on this
           string being byte-identical to the one solver/decide.c filed the domain under. concolic_hole_key also
           answers both conditions this branch needs: NULL for a brace-free segment and for the unnameable `{}`
           whose strip is empty. */
        name = concolic_hole_key(seg);
        if (!name) {
            json_buf_raw(&p, seg);   /* a literal segment, or a `{}` this surface cannot name */
        } else {
            nlen = strlen(name);
            /* The grammar the consumer substitutes by, asserted at the mint: the speller strips the braces and a
               segment cannot hold a `/` because this walk split on one, so it holds by construction, and the next
               producer of a name has to keep it true. */
            DCHECK(!strpbrk(name, "{}/"),
                   "a path param's NAME still holds a brace or a slash — the popup substitutes a hole by "
                   "matching /\\{([^}/]+)\\}/ against this path, so a name outside that grammar names a hole "
                   "no substitution can find");
            json_buf_raw(&p, "{"); json_buf_raw(&p, name); json_buf_raw(&p, "}");
            /* So a path param looks its domain up by the string the popup substitutes it by and decide.c filed it
               under. The value is the concrete example aligned out of the URL, with no braces to read a hole out of;
               it may carry a `/`, because a hole stands over the run of example segments its value occupied. */
            kv_add(out, name, nlen, span ? b : "", span ? bn : 0, EP_PATH, name);
        }
        free(seg); free(name);
        i++;
        if (!ae) break;
        json_buf_raw(&p, "/");
        a = ae + 1;
        if (span) b = be + 1;
    }
    DCHECK(!span || i == nseg,
           "the shape walk visited a different number of segments than the alignment was built over — the "
           "span array is indexed by this loop and sized by `seg_split`, so a short walk leaves a hole "
           "reading a span that belongs to another segment the next time this address is scanned");
    free(span);
    return json_buf_take(&p);
}

/* The value of one body field in the string vocabulary every other value on this surface speaks: the literal the
   code computed, or its `{shape}` where it computed none. The value came out of JS_ParseJSON, so it holds no getter
   and no page `toString`, and this conversion cannot run page code or throw. A field whose value is an object or
   array is recorded by name with no value: a real field, but a nested document cannot be carried by a flat
   name -> string record without inventing a naming convention for its members. */
static char *body_field_text(JSContext *ctx, JSValueConst v) {
    const char *s;
    char *r;

    if (JS_IsObject(v)) return NULL;
    s = JS_ToCString(ctx, v);
    /* Every value JS_ParseJSON produces is a primitive or a plain object, and objects left above, so this
       conversion runs no page code and fails only on allocation: a CHECK, since a compiled-out assert would hand
       `strdup` a NULL in release. */
    CHECK(s, "endpoint: OOM rendering a JSON request body's field value");
    r = strdup(s);
    CHECK(r, "endpoint: OOM copying a JSON request body's field value");
    JS_FreeCString(ctx, s);
    return r;
}

/* The fields of a body the page's own serializer wrote, named off its spans: the RPC field keys and example values
   of a real web app, with no decoder needed. Each span is a byte range the run observed an unknown land in, so this
   walk infers nothing: it prints what the engine recorded while the page's code ran. Asked before the content type,
   which is why it is a separate walk: Fetch §5.2 BodyInit unions give a BufferSource no type, so a protobuf payload
   handed to `fetch` as a Uint8Array with no Content-Type arrives with `mime == NULL`, where the reader below returns
   at its first line. The name is the byte range, because the range is what places the value back: a byte body is
   substituted by splicing, so `body[17:23]` is this param's address as `region` is a query param's (end exclusive).
   The value is the example where there is one and the provenance where there is not, kv_pairs' rule for a query
   param, with the domain read beside both off the same flow under the same hole key by kv_add. A coalesced run
   carries no example (core/fetch/body.c drops it when a run grows past one element, since a range's example is
   bytes and a param value is a NUL-terminated UTF-8 string); its bytes ride the record's `body_example_b64`. */
static int body_spans_params(KvBuf *out, const EndpointBody *body) {
    int i, before = out->n;

    if (!body || body->nspan <= 0) return 0;
    DCHECK(body->span != NULL,
           "a request body reached the @H surface counting unknown byte ranges it holds no array of — the "
           "pair is one fact, and reading the count alone walks a pointer its producer never set");
    for (i = 0; i < body->nspan; i++) {
        const EndpointBodySpan *s = &body->span[i];
        char name[64];
        int nl;
        char *hole;
        const char *val;

        DCHECK(s->shape != NULL,
               "an unknown byte range of a request body reached the @H surface with no provenance — the shape "
               "is what says WHERE these bytes came from, and a range with neither an example nor a source is "
               "a row a reviewer can read nothing off");
        DCHECK(i == 0 || body->span[i - 1].off + body->span[i - 1].len <= s->off,
               "a request body's unknown byte ranges arrived out of order or overlapping — the capture walks "
               "the page's view in index order, so two ranges claiming one byte means two records of one body "
               "were merged");
        nl = snprintf(name, sizeof name, "body[%zu:%zu]", s->off, s->off + s->len);
        DCHECK(nl > 0 && nl < (int)sizeof name,
               "a body span's name did not fit its buffer — the name is two decimal offsets into a body, so a "
               "truncation here would give two different ranges one param name");
        /* The domain is looked up by the key every other param on this surface uses, composed here rather than
           carried from the producer, so this file is the only caller of concolic_hole_key for a body. */
        hole = concolic_hole_key(s->shape);
        val = s->example ? s->example : s->shape;
        kv_add(out, name, (size_t)nl, val, strlen(val), EP_BODY, hole);
        free(hole);
    }
    return out->n - before;
}

/* The request body, read back in its own format: the keys and values a call sends, half of which a POST carries in
   its payload. The format is decided by the request's own content type, never by sniffing the bytes: a page that
   sends JSON says so, and a body this engine has no reader for records no fields rather than a guess. Returns how
   many fields it named, because the caller asks whether anything is left for the zone to read. */
static int body_params(JSContext *ctx, KvBuf *out, const EndpointBody *body) {
    MimeType mt;
    char *text;
    int before = out->n;

    /* The spans first and unconditionally (see body_spans_params). Its params are counted by this function's own
       `before`, so a body whose only fields are span-named is still a body whose fields were read, which is what
       the caller asks and what stops the raw-bytes store from making a replay claim over an example. */
    body_spans_params(out, body);
    if (!body || !body->bytes) return out->n - before;
    /* §4.4 "parse a MIME type" is the one reader of a Content-Type in this engine, never a `strcasecmp` against a
       literal (the C locale's answer where the standard's is ASCII's) and never a private essence split. A type that
       will not parse is §4.4's failure and reads no fields. */
    mime_type_init(&mt);
    if (!body->mime || !mime_type_parse(&mt, body->mime, strlen(body->mime))) {
        mime_type_free(&mt);
        return out->n - before;
    }

    text = malloc(body->len + 1); CHECK(text, "endpoint: OOM request body");
    memcpy(text, body->bytes, body->len); text[body->len] = 0;

    /* `text/plain;charset=UTF-8` is an undeclared body, not one declared as text: Fetch §5.2 "BodyInit unions" gives
       a USVString body that type, so `fetch(u, {method:"POST", body: JSON.stringify(x)})`, the commonest POST in a
       bundle, carries a type the page never chose. Its bytes go through the real JSON parser and either are a
       name -> value document or are not (a plain-text body fails to parse and records nothing). Not sniffing:
       nothing branches on a pattern in the bytes; the real codec runs. A type the page did declare is still the
       only thing consulted for it. */
    if (mime_type_is_json(&mt) || (mt.type && mt.subtype && !strcmp(mt.type, "text") && !strcmp(mt.subtype, "plain"))) {
        JSValue v = JS_ParseJSON(ctx, text, body->len, "<@H request body>");
        /* A page may send bytes that are not the JSON its header claims: the page's fact, not this engine's
           invariant, so the exception is discharged here, and no field is recorded, the true statement about a
           body whose fields could not be read. */
        if (JS_IsException(v)) { JS_FreeValue(ctx, JS_GetException(ctx)); }
        else if (JS_IsObject(v) && !JS_IsArray(v)) {
            JSPropertyEnum *tab = NULL;
            uint32_t n = 0, i;
            /* A PLAIN OBJECT JS_ParseJSON BUILT, so this walk reaches no trap and no getter: the only way it
               fails is allocation, which is what a CHECK is for. An `if (ok)` here would turn a body whose
               fields could not be listed into a body with no fields. */
            CHECK(JS_GetOwnPropertyNames(ctx, &tab, &n, v, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0,
                  "endpoint: OOM listing a JSON request body's fields");
            for (i = 0; i < n; i++) {
                JSValue pv = JS_GetProperty(ctx, v, tab[i].atom);
                const char *key = JS_AtomToCString(ctx, tab[i].atom);
                char *val = body_field_text(ctx, pv);
                CHECK(key, "endpoint: OOM naming a JSON request body's field");
                if (key[0]) {
                    char *hole = concolic_hole_key(val ? val : "");
                    kv_add(out, key, strlen(key), val ? val : "", val ? strlen(val) : 0, EP_BODY, hole);
                    free(hole);
                }
                free(val);
                JS_FreeCString(ctx, key);
                JS_FreeValue(ctx, pv);
            }
            JS_FreePropertyEnum(ctx, tab, n);
        }
        JS_FreeValue(ctx, v);
    } else if (mt.type && mt.subtype && !strcmp(mt.type, "application") &&
               !strcmp(mt.subtype, "x-www-form-urlencoded")) {
        /* No example list for a body: see the residual below. */
        kv_pairs(out, text, NULL, EP_BODY);
    }
    /* Named residual. Not covered: the value of a body field the page composed out of an unknown. Both arms above
       read one byte sequence, the display form (a concatenation carries its operands' display forms into the
       result), so a field whose value the interpreter computed is emitted as `{…}` while the same computation in
       the query half is emitted as bytes. Next diff builds: the concrete rendering beside the display one on
       `EndpointBody` (solver/endpoint.h states `mime`, `bytes`, `len` and `kind`, nothing concrete), so this
       function walks the two as kv_pairs walks a query and body_field_text can be asked for the example's field;
       its producers are outside this component, so the field and every fill land together, and a fill with no
       example states none. The same field serves the string-composed body residual above body_b64_encode.
       Absence shows as: an @H record whose query params carry concrete values and whose body fields carry
       brace-spelled shapes, for one request whose two halves were built from the same computed values. */
    free(text);
    mime_type_free(&mt);
    return out->n - before;
}

/* The body the engine could not name, carried as bytes so the zone can: for a gRPC-Web or protobuf call the payload
 * is the whole of what a request sends. It forwards and nothing decodes: reconstructing field numbers from a byte
 * stream would infer an answer the run already had (the page's own serializer ran with the real names and values),
 * and a MIME pattern selecting a decoder would be a protocol-specific recognizer. What names a field is the shape,
 * and for a string-composed body it already does: a concatenation carries its operands' display forms, so a body
 * the page builds by joining text reaches body_params spelling its own holes. This is what is left for a body not
 * composed as a string.
 *
 * A body built in a typed array: ECMAScript §10.4.5.18 TypedArraySetElement ( obj, index, value ) stores unknown
 * external input with the data block holding an example and a span beside it carrying the fact, and
 * body_spans_params names the fields off the spans. A single unknown element carries its own example; a coalesced
 * run cannot (a range's example is bytes, and json_buf_str writes a NUL-terminated string it trusts to be UTF-8),
 * so the bytes ride their own key, `body_example_b64` (base64 by the engine's own codec, EPB_EXAMPLE at the mint,
 * `bodyExampleBase64` at the emit, read by lib/popup-form.js): the span params are the addresses and these the
 * content, and a consumer holding both can splice. The grade constraint holds the design up: a span is recorded
 * even where no byte was written (an unknown may carry no example, and writing `0` would invent a value only to
 * satisfy a gate), so a body carrying spans has no bytes the request can be said to send and is never published
 * as EPB_SENT, which body_store asserts. The codec is the engine's own, for core/file/file_reader.c's reason:
 * `btoa`'s codec is implemented here, and an encoding builtin is never re-implemented beside itself.
 *
 * Named residual. Not covered: the bytes of a body composed as a string out of unknown input. They reach this file
 * as the engine's display spelling (§5.2's string arm over a concolic), so EPB_SHAPE's text is all that is known,
 * and the concrete characters the concatenation contributed around the hole are never rendered. Next diff builds:
 * the example rendering on `EndpointBody` that body_params' residual asks for, one field answering both. Absence
 * shows as: a Send panel that names the source composing a payload and shows no bytes, for a request whose
 * address half carries concrete values computed the same way. */
/* The codec, with no claim in it: the two fields make different claims about one kind of bytes, so the
   precondition stays at each caller, where it names the field it defends, and the encoding is written once. NULL
   for an empty body, which every caller reads as the absence it is. */
static char *body_b64_encode(const char *bytes, size_t len) {
    size_t cap, n;
    char *b64;

    if (!bytes || len == 0) return NULL;
    cap = JS_Base64EncodedSize(len) + 1;
    b64 = malloc(cap);
    CHECK(b64 != NULL, "endpoint: OOM base64-encoding a request body this engine has no field reader for");
    n = JS_Base64Encode(b64, cap, (const uint8_t *)bytes, len);
    CHECK(n > 0, "endpoint: the base64 buffer was sized wrong for a request body");
    b64[n] = 0;
    return b64;
}

/* The replay claim. */
static char *body_bytes_b64(const EndpointBody *body) {
    if (!body || !body->bytes || body->len == 0) return NULL;
    /* A shape is not a payload: a body built from unknown external input carries the engine's display spelling of
       that unknown, and base64 of it would publish a hole's characters under the record's claim that the request
       sent these bytes, which a reviewer replays. Asserted here and decided at one place, body_store: a shape
       reaching this function is that choice having gone wrong. */
    DCHECK(body->kind == EPB_SENT,
           "the base64 of a request body was asked for a body that is NOT bytes the request sends — this "
           "function makes the record's replay claim, so running it over an unknown body's display spelling "
           "publishes the characters of a hole as a payload a reviewer would send. body_store picks the "
           "field; a shape belongs in `body_shape` and never here");
    return body_b64_encode(body->bytes, body->len);
}

/* And the other claim over the same codec, which is why the assert is here and not in it: these bytes are what the
   payload looks like, never what the request sends (inside a span stands an unknown's example or a byte nobody
   wrote, outside every span a byte the page computed). Reaching this over an EPB_SENT body would understate a
   replayable payload as an example, a lost finding rather than a fabricated one. */
static char *body_example_b64(const EndpointBody *body) {
    if (!body || !body->bytes || body->len == 0) return NULL;
    DCHECK(body->kind == EPB_EXAMPLE,
           "the example bytes of a request body were asked for a body that is not one — this key states "
           "what a payload LOOKS LIKE so a consumer holding the span params can splice into it, and it is "
           "reached only for a body carrying spans. A body with none is either bytes the request sends or the "
           "display spelling of an unknown, and body_store picks between those two");
    return body_b64_encode(body->bytes, body->len);
}

/* Which of the three body facts this sighting carries, decided once, so body_bytes_b64 and body_example_b64 can each
   assert its own precondition. `body_named` says body_params already put this body's fields on `params`; for such
   a body the reviewer can vary the fields and the raw form is not carried beside them. The span arm is asked first
   because that rule is false of exactly one population: a span param names a byte range, an address silent about
   what stands there, so the bytes it addresses are the other half of one answer, and `body_named` is nonzero for
   every spanned body (every span mints a param), so behind that flag a spanned body would record nothing. First
   body wins and is never overwritten: a request body is one example of what this endpoint is sent, and a sighting
   at a different grade is already a different record (provenance is part of `same_identity`), so a forced body
   cannot mix into an observed one. */
static void body_store(Endpoint *e, const EndpointBody *body, int body_named) {
    if (!body || !body->bytes) return;
    /* First body wins across all three fields, not per field, so a second sighting cannot record a different body
       under a key the first left free; a record holding one sighting's spanned example and another's display shape
       would present two bodies as one request's. */
    if (e->body_b64 || e->body_shape || e->body_example_b64) return;
    if (body->nspan > 0) {
        /* The spanned body, asked before `body_named`: here named fields and raw bytes are not alternatives, since
           `body[17:23]` is an address, and an address with nothing at it lets a reviewer change which value a range
           carries without seeing what it carries now. A third field rather than a widening of either other:
           `body_b64` promises these are the bytes the request sends, and a spanned body's are an unknown's example
           or a byte nobody wrote; `body_shape` is a C string over bytes that may hold a NUL and may not be UTF-8. */
        DCHECK(body_named,
               "a request body carrying unknown byte ranges was stored with none of its ranges named — every "
               "span mints a param, so this is the span walk having been skipped and these example bytes "
               "would reach a reviewer with no address to splice at, which is the half that makes them safe");
        e->body_example_b64 = body_example_b64(body);
    } else if (body_named) {
        return;
    } else if (body->kind == EPB_SHAPE) {
        /* The shape text itself, a statement about a source rather than a payload: `{cfg.payload}` names who
           composes this body, more than an empty panel tells a reviewer and nothing a sniffer can produce.
           Recording nothing for an unknown body would trade a fabrication for a silence: a record that says nothing
           about a body reads as a request with no body. Carried under its own key so no consumer has to decide
           which it holds. A MIME is attached only if there is one; §5.2's string arm gives an unknown body
           `text/plain;charset=UTF-8`, which the page never chose, so it is reported as what it is, and the page's
           own declared type, where set, is what a reviewer wants. */
        e->body_shape = malloc(body->len + 1);
        CHECK(e->body_shape, "endpoint: OOM recording the display shape of an unknown request body");
        memcpy(e->body_shape, body->bytes, body->len);
        e->body_shape[body->len] = 0;
    } else if (body->mime) {
        /* Bytes, which mean something only beside the type that says how to read them (the pair `EndpointBody` is
           one struct for); this arm makes the record's replay claim. The example arm does not share the test, and
           the difference is what spans buy: a type names the fields of a payload nobody can otherwise address,
           while a spanned body is addressed by its span params, and Fetch §5.2 BodyInit unions give a BufferSource
           no Content-Type, so the protobuf payload this capability exists for arrives with `mime == NULL`. */
        e->body_b64 = body_bytes_b64(body);
    }
    if ((e->body_b64 || e->body_shape || e->body_example_b64) && body->mime) {
        e->body_mime = strdup(body->mime);
        CHECK(e->body_mime, "endpoint: OOM recording a request body's content type");
    }
}

/* The domain's first observation: this endpoint's record takes the set the recording flow proved. */
static void param_set_excl(Param *p, char *const *ex, int n) {
    int i;
    DCHECK(p->nexcl == 0 && p->excl == NULL,
           "an endpoint param's exclusion set was seeded twice — the first sighting takes the set and every "
           "later one INTERSECTS it, so a second seed would replace a constraint that had already been "
           "narrowed by another path and state, of this endpoint, something no run of the program obeyed");
    if (n <= 0) return;
    p->excl = malloc((size_t)n * sizeof(char *));
    CHECK(p->excl, "endpoint: OOM recording what a flow proved a param is not");
    for (i = 0; i < n; i++) {
        p->excl[i] = strdup(ex[i]);
        CHECK(p->excl[i], "endpoint: OOM copying a value a flow proved a param is not");
    }
    p->nexcl = n;
}

/* And every later one narrows it: a constraint the record still carries is one every observed path obeyed, and a
   token this flow did not exclude is one some path allowed. The set only shrinks, so a merge earns the WFQ nothing;
   the endpoint's first sighting already earned its point. */
static void param_intersect_excl(Param *p, char *const *ex, int n) {
    int i, j, k = 0;
    for (i = 0; i < p->nexcl; i++) {
        int keep = 0;
        for (j = 0; j < n; j++) if (!strcmp(p->excl[i], ex[j])) { keep = 1; break; }
        if (keep) p->excl[k++] = p->excl[i];
        else free(p->excl[i]);
    }
    p->nexcl = k;
}

/* The interval's first observation. The strings are re-copied rather than moved because kv_free frees the KV row
   whether it was merged or seeded, and one owner per string survives a new caller. */
static void param_set_bound(ParamBound *d, const ParamBound *s) {
    DCHECK(!d->has_lo && !d->has_hi && !d->lo_txt && !d->hi_txt,
           "an endpoint param's interval was seeded twice — the first sighting takes it and every later one "
           "WIDENS it, so a second seed would replace a bound another path had already disproved and state, "
           "of this endpoint, a constraint no run of the program obeyed");
    *d = *s;
    d->lo_txt = d->hi_txt = NULL;
    if (s->has_lo) { d->lo_txt = strdup(s->lo_txt); CHECK(d->lo_txt, "endpoint: OOM recording a param's lower bound"); }
    if (s->has_hi) { d->hi_txt = strdup(s->hi_txt); CHECK(d->hi_txt, "endpoint: OOM recording a param's upper bound"); }
}

/* And every later one widens it to the interval hull: a bound the record still carries is one every observed path
   obeyed, so the surviving lower bound is the looser of the two (none if this path had none), and likewise above.
   It is `param_intersect_excl`'s claim over an ordered domain: intersecting exclusion sets is the complement of the
   union of the two domains, and the hull is the strongest interval containing that union. On a tie the inclusive
   side wins, the mirror of concolic.c's within-a-flow rule (conjoined there, the tighter survives; hulled here, the
   looser). */
static void param_widen_bound(ParamBound *d, const ParamBound *s) {
    if (!s->has_lo) { free(d->lo_txt); d->lo_txt = NULL; d->has_lo = 0; }
    else if (d->has_lo && (s->lo < d->lo || (s->lo == d->lo && s->lo_incl && !d->lo_incl))) {
        free(d->lo_txt);
        d->lo = s->lo; d->lo_incl = s->lo_incl;
        d->lo_txt = strdup(s->lo_txt); CHECK(d->lo_txt, "endpoint: OOM widening a param's lower bound");
    }
    if (!s->has_hi) { free(d->hi_txt); d->hi_txt = NULL; d->has_hi = 0; }
    else if (d->has_hi && (s->hi > d->hi || (s->hi == d->hi && s->hi_incl && !d->hi_incl))) {
        free(d->hi_txt);
        d->hi = s->hi; d->hi_incl = s->hi_incl;
        d->hi_txt = strdup(s->hi_txt); CHECK(d->hi_txt, "endpoint: OOM widening a param's upper bound");
    }
}

/* The call predicates' first observation: this endpoint's record takes the set the recording flow proved. */
static void param_set_pred(Param *p, const ConcolicPred *src, int n) {
    DCHECK(p->npred == 0 && p->pred == NULL,
           "an endpoint param's call-predicate set was seeded twice — the first sighting takes the set and "
           "every later one INTERSECTS it, so a second seed would replace a constraint that had already been "
           "narrowed by another path and state, of this endpoint, something no run of the program obeyed");
    if (n <= 0) return;
    p->pred = param_pred_copy(src, n);
    p->npred = n;
}

/* And every later one narrows it, `param_intersect_excl`'s rule over predicates: a predicate the record still
   carries is one every observed path obeyed. No hull, since there is no order: `startsWith("/api")` and
   `startsWith("/admin")` do not weaken to a common predicate, and inventing one would mean deciding what the method
   means, a recognizer inside a merge rule. */
static void param_intersect_pred(Param *p, const ConcolicPred *src, int n) {
    int i, j, k = 0;
    for (i = 0; i < p->npred; i++) {
        int keep = 0;
        for (j = 0; j < n; j++) if (param_pred_same(&p->pred[i], &src[j])) { keep = 1; break; }
        if (keep) p->pred[k++] = p->pred[i];
        else concolic_pred_release(&p->pred[i]);   /* the row's strings; the array is this param's */
    }
    p->npred = k;
}

/* The loose equalities' first observation: this endpoint's record takes the set the recording flow proved. */
static void param_set_leq(Param *p, const ConcolicLooseEq *src, int n) {
    DCHECK(p->nleq == 0 && p->leq == NULL,
           "an endpoint param's loose-equality set was seeded twice — the first sighting takes the set and "
           "every later one INTERSECTS it, so a second seed would replace a constraint that had already been "
           "narrowed by another path and state, of this endpoint, something no run of the program obeyed");
    if (n <= 0) return;
    p->leq = param_leq_copy(src, n);
    p->nleq = n;
}

/* And every later one narrows it, `param_intersect_excl`'s rule over loose equalities: a gate this flow did not
   hold is one some path reached the request without. No hull: `== 0` and `== ""` do not weaken to a common claim,
   and covering both would mean computing the union of two of §7.2.13 IsLooselyEqual ( x, y )'s holding sets,
   deciding what `==` means over an Object arm that runs the page's own ToPrimitive. */
static void param_intersect_leq(Param *p, const ConcolicLooseEq *src, int n) {
    int i, j, k = 0;
    for (i = 0; i < p->nleq; i++) {
        int keep = 0;
        for (j = 0; j < n; j++) if (param_leq_same(&p->leq[i], &src[j])) { keep = 1; break; }
        if (keep) p->leq[k++] = p->leq[i];
        else concolic_looseeq_release(&p->leq[i]);   /* the row's string; the array is this param's */
    }
    p->nleq = k;
}

static void param_add_val(Param *p, const char *v) {   /* merge a validValue (dedup, skip empty) */
    if (!v || !v[0]) return;
    for (int i = 0; i < p->nvals; i++) if (!strcmp(p->vals[i], v)) return;
    if (p->nvals >= p->vcap) { p->vcap = p->vcap ? p->vcap * 2 : 4; p->vals = realloc(p->vals, (size_t)p->vcap * sizeof(char *)); CHECK(p->vals, "endpoint: OOM vals"); }
    p->vals[p->nvals++] = strdup(v);
}

/* An endpoint's identity is (method, path, provenance, param set); the same identity merges param values. A param's
   location is part of it: an `id` in the path and an `id` in the body are two different things to send. The
   provenance is part of it, which makes "forced replies are never merged into observed data" a property of this
   structure: a forced sighting cannot reach a record built from derived ones, so no value learned only because a
   gate was forced is published as one the app computes (endpoint.h has why the pending line folds and this does
   not). */
static int same_identity(Endpoint *e, const char *method, const char *path, int prov, const KvBuf *kv) {
    if (e->prov != prov || strcmp(e->method, method) || strcmp(e->path, path) || e->np != kv->n) return 0;
    for (int i = 0; i < kv->n; i++)
        if (e->params[i].loc != kv->e[i].loc || strcmp(e->params[i].name, kv->e[i].name)) return 0;
    return 1;
}

void endpoint_record(JSContext *ctx, const char *method, JSValueConst url,
                     const EndpointHeader *hdrs, int nhdrs, const EndpointBody *body, int prov, int door) {
    /* What this sighting is evidence of, asserted at the mint for `kv_add`'s reason: every site that records an
       endpoint passes through here. A value outside the vocabulary is the dangerous one: `0` is `observed`, so an
       uninitialised grade reads as the strongest claim this surface makes. */
    DCHECKF(prov == PROV_OBSERVED || prov == PROV_DERIVED || prov == PROV_FORCED,
            "an endpoint was recorded with the provenance %d, which is none of the three solver/pending.h "
            "defines — the grade is part of this record's identity and is emitted on it, so an unrecognised "
            "one both merges with nothing and publishes a word no consumer can read. method=%s", prov, method);
    /* And which mechanism composed the address, asserted at the same mint for the same reason. A membership test
       despite its spelling: `ENDPOINT_DOORS` gives its members no explicit values, so the enum is dense from
       `EPD_UNSTATED` to `EPD_COUNT`. `EPD_UNSTATED` is 0 so that a producer that forgets is refused here rather than
       read as whichever mechanism the numbering put first; endpoint_door_token's CHECK catches the same record in
       release. */
    DCHECKF(door > EPD_UNSTATED && door < EPD_COUNT,
            "an endpoint was recorded with the door %d, which is none of endpoint.h's ENDPOINT_DOORS — the "
            "mechanism that composed an address is the one fact about a sighting no consumer can re-derive, "
            "and it is what CLAUDE.md's `epEmitted - epPreProgram` razor subtracts two totals to approximate, "
            "so a record without one is a row that partitions with nothing. State the door for the mechanism "
            "this call site IS. method=%s", door, method);
    /* And whose bytes the body is: what the request sends, the engine's display spelling of an unknown, or an
       example of the payload, which only the producer knows (it asked core/fetch/body.h to obtain them). The zero
       is UNSTATED, so a producer that forgets is refused rather than read as the replayable arm. The message names
       mime and method because three callers pass a body and all would report this line. */
    DCHECKF(!body || body->kind == EPB_SENT || body->kind == EPB_SHAPE || body->kind == EPB_EXAMPLE,
            "a request body reached the @H surface without its producer saying whether the bytes are what the "
            "request SENDS, this engine's display spelling of an unknown, or an EXAMPLE of the payload. Those "
            "are not three renderings of one fact: the first is replayable evidence, the second is a hole, "
            "and the third is what the payload looks like at ranges `params` names. Emitting any of them as "
            "another hands a reviewer a request the page never made or hides one it did. State the kind from "
            "the arm core/fetch/body.h already told you. method=%s mime=%s", method, body->mime ? body->mime : "(none)");
    /* And the one pairing between the two facts above, which the example key rests on: §10.4.5.18
       TypedArraySetElement ( obj, index, value ) records a span even where no byte was written, so a body carrying
       spans has bytes the request cannot be said to send. Asserted at the mint, so body_store can dispatch on
       `nspan` and body_example_b64 assert its precondition. One-way: a body with no span may still be an example. */
    DCHECKF(!body || body->nspan == 0 || body->kind == EPB_EXAMPLE,
            "a request body carrying unknown byte ranges reached the @H surface graded as something other "
            "than an EXAMPLE of the payload — some of its bytes are an unknown's example and some are bytes "
            "nobody wrote, so no record of this request may claim it sends exactly them. method=%s kind=%d",
            method, (int)body->kind);
    /* And that its span record is one fact: `EndpointBody` is filled field by field at each producer, and a count
       with no array reads as unknown ranges nobody can name while an array with no count reads as none, neither
       distinguishable by a consumer from a body with no spans. */
    DCHECKF(!body || (body->span == NULL) == (body->nspan == 0),
            "a request body reached the @H surface with half a span record — its producer set the array or "
            "the count and not the other, so this body either claims unknown byte ranges it holds no "
            "provenance for or holds provenance nothing will read. method=%s", method);
    DCHECKF(!body || body->nspan >= 0,
            "a request body reached the @H surface claiming a negative number of unknown byte ranges, which "
            "is a producer that declared this record and never filled it. method=%s", method);
    /* The ask, counted here, before the gate on the next line: a count taken past a gate re-implements its
       refusals as absences, and this gate declines every request a candidate re-fire makes. The program bit is read
       once for the whole call (an operation takes its inputs with it); the mint below re-reads it and asserts the
       two agree, which a later diff cannot quietly falsify. */
    int ask_pre_program = !engine_any_program_started();
    /* And that the boundary this bit is read against belongs to this surface's session (see g_boundary_spent).
       Asserted at the first ask, not at the reset, which is legitimate on its own (a process may end there); an ask
       arriving afterwards is graded against a boundary another document crossed. The fix is endpoint.h's
       fourth-number residual: the any-program-started bit carried on the document the record's realm belongs to,
       raised where that document's first program starts, which also answers the child-navigable case. */
    DCHECK(!g_boundary_spent,
           "an @H ask arrived on a surface whose counters were reset while a program had already started on "
           "this instance — the program-state boundary is `g_prog_starts`, which the engine's session "
           "teardown names on its NOT-RESET list, and every counter here is reset per session, so this "
           "document's whole markup door will grade POST-program: `preProgram` reads 0, `emitted - "
           "preProgram` reads the entire surface, and the product's razor reports the `<head>` counted back "
           "as everything forced execution contributed. Nothing downstream can see it: both censuses' "
           "identities hold, and `pre_program` is decided at each mint and never re-armed");
    g_asks++;
    if (ask_pre_program) g_asks_pre_program++;
    if (g_suppress) { g_ask_suppressed++; return; }   /* candidate/verify run: not a real @H endpoint */
    /* Whether this sighting's address was one the run had determined, read once here from the value the display
       spelling is taken from: the merge arm and the mint both need it, the value is borrowed for this call, and
       nothing below holds the JSValue. */
    int acls = address_class_of(url);
    /* The bar's claiming member maps to `runtime-only` for every door: it is `endpoint_razor_class_of`'s first
       line. Asserted here because the mapping takes a door and only a caller holds one; a door arm put in front of
       that line fires this. */
    DCHECK(endpoint_razor_class_of(door, EPA_UNKNOWN) == EPZ_RUNTIME_ONLY,
           "`address_class_of`'s flow-less member is no longer the one `endpoint_razor_class_of` maps to "
           "`runtime-only` — that function's residual rests on the no-flow fallback CLAIMING on the product's "
           "bar, which is why a stale pin chain would assert a determination no flow made; if this member "
           "now maps elsewhere, the direction that argument states is inverted and the gate it justifies is "
           "protecting the opposite thing");
    /* And the member that does not claim: `address_class_of` narrows the bar by returning `EPA_UNKNOWN_UNPROVEN`,
       and that narrowing is real only while this mapping leaves such a row `unproven`; a door arm, or a member
       reordered above `EPA_UNKNOWN`, would hand the bar back every instrument hole with every census still summing.
       The `EPB_OFF_DOCUMENT` arm is excluded, not exempted: that operand is the bar's other positive statement
       (`reply-chunk` hands this surface bytes in no served document), so a row clearing through the door is the
       union working. */
    DCHECK(endpoint_door_bytes(door) == EPB_OFF_DOCUMENT ||
           endpoint_razor_class_of(door, EPA_UNKNOWN_UNPROVEN) == EPZ_UNPROVEN,
           "`address_class_of`'s whose-narrowed member now CLAIMS on the product's own bar through a door "
           "whose bytes were in the served document — that member exists so the bar refuses an address the "
           "run did not determine and whose every root this engine minted, the served document spells, or no "
           "mint could speak for, and a mapping that clears it restores exactly the over-claim the read "
           "closed: on a real site every row that cleared this bar was this engine's own instrument hole");
    /* And what the path that composed it had chosen, read on the same line and for the same reason: a fact about
       the running flow, which nothing below may re-ask. */
    int wcls = witness_class_now();
    /* And which sources the address's bytes entered through, copied here: `concolic_root_c` points into the value's
       own record, borrowed for this call, and the record below outlives it. NULL for a non-concolic value and for
       one whose bytes entered through nothing minted as a source; `addr_class` tells them apart (see the struct). */
    char *aroot = NULL;
    { const char *r = concolic_root_c(url);
      if (r) { aroot = strdup(r);
               CHECK(aroot, "endpoint: OOM copying an address's delivery root — the record would carry a "
                            "class derived from a provenance it cannot name, and this surface's own reader "
                            "would report the address as one this engine attributed to nothing"); } }
    char *disp = url_display(ctx, url);
    char *ex = url_example(ctx, url);
    char *shape_path = url_path_of(disp);
    char *expath = ex ? url_path_of(ex) : NULL;
    /* Path, then query, then body: the order a request is written in and the identity compares in. The path is
       re-spelled by the scan (see path_scan), and the re-spelled one becomes the endpoint's `url`, because the
       params are named in its grammar. The example is aligned against both halves of the address, so a hole in a
       query value (`fetch('/api/user?region=' + c.region)`) carries the bytes the code computed, as one in the path
       does; the halves are split here because `?` is the address's own boundary and neither grammar contains it.
       The body carries an example for exactly the values the run observed landing in it: a string-composed body
       spells its holes and the JSON arm reads them, and a byte-by-byte body is named off its span record
       (body_params' residual has what is still missing). */
    KvBuf kvb = { 0 };
    char *path = path_scan(&kvb, shape_path, expath);
    { const char *q = strchr(disp, '?'), *eq = ex ? strchr(ex, '?') : NULL;
      if (q && q[1]) kv_pairs(&kvb, q + 1, (eq && eq[1]) ? eq + 1 : NULL, EP_QUERY); }
    /* Named fields first; the raw bytes only where there are none: a body this engine reads is better described by
       its fields, and both would put one fact on the record twice. */
    int body_named = body_params(ctx, &kvb, body);
    free(disp); free(ex); free(expath); free(shape_path);

    for (int i = 0; i < g_eps_n; i++) {                 /* merge into an existing same-identity endpoint */
        if (same_identity(&g_eps[i], method, path, prov, &kvb)) {
            for (int j = 0; j < kvb.n; j++) {
                param_add_val(&g_eps[i].params[j], kvb.e[j].val);
                param_intersect_excl(&g_eps[i].params[j], kvb.e[j].excl, kvb.e[j].nexcl);
                param_widen_bound(&g_eps[i].params[j].bnd, &kvb.e[j].bnd);
                param_intersect_pred(&g_eps[i].params[j], kvb.e[j].pred, kvb.e[j].npred);
                param_intersect_leq(&g_eps[i].params[j], kvb.e[j].leq, kvb.e[j].nleq);
                /* The one union among the four intersections, for the struct's reason: those are claims about the
                   value that a path disproves, while this is whether the observation could ask anything, which a
                   later concrete sighting cannot take back. `same_identity` reads names and locations only, so the
                   two sightings may disagree about it. */
                g_eps[i].params[j].has_hole |= kvb.e[j].has_hole;
            }
            /* A required header this endpoint did not have is emitted output, credited to the WFQ: the identity is
               method + path + param names and locations, so a flow building a differently shaped request earns its
               point below, and what reaches here is a flow that called a known endpoint and revealed it also wants
               `Authorization`, a content type or an API key, which this engine exists to learn. Structure, not
               data: a header name is bounded by the code, while a param value (`param_add_val`, which earns
               nothing) is unbounded in the input, and a loop over opaque data could mint credit without limit. One
               point per merge that gained structure: an endpoint is one discovery however many headers arrive. */
            /* First body wins and is never overwritten (body_store): a request body is one example, and a sighting
               at a different grade is a different record. */
            body_store(&g_eps[i], body, body_named);
            /* The record-level merge of the address class, the only field on the record with one (`door` and
               `pre_program` are properties of the mint; see the struct). A max over the ordered list, the union's
               sentence for a list longer than two, so the whose fact needed no field or merge rule of its own: a
               sighting that proved more is not taken back by one that proved less, and at the top step that is
               concolic.h's `_any` one grain out (an address composed from an unknown supplied outside this engine
               is not taken back by one composed from an engine-minted hole). `same_identity` compares the display
               spelling, which for a concolic address is its shape and for a determined one its bytes, so sightings
               here may disagree about the class. The delivery root moves with the class on this line and nowhere
               else, so the two are always one sighting's. */
            if (acls > g_eps[i].addr_class) {
                g_eps[i].addr_class = acls;
                free(g_eps[i].addr_root);
                g_eps[i].addr_root = aroot;   /* ownership transferred; the `done:` arm frees what is left */
                aroot = NULL;
            }
            /* The same max over the witness, ordered by how much is claimed: a sighting that could ask outranks one
               that could not, and a path that had chosen a witness is not taken back by one that had not.
               `if (wcls == EPW_MAY_REST_ON)` would leave an `unasked` record `unasked` for ever once a later
               sighting read `no-witness`. */
            if (wcls > g_eps[i].witness_class) g_eps[i].witness_class = wcls;
            if (endpoint_merge_headers(&g_eps[i], hdrs, nhdrs) > 0)
                flow_credit_emit(1.0);
            /* The arm the surface has no row for: a merge teaches the record structure and moves no census figure
               (not `minted`, not `emitted`, not `preProgram`, whose flag belongs to the mint), so running code
               composing every address the markup had named would be byte-identical on all four rows to running
               code composing nothing. This is where the two part. */
            g_ask_merged++;
            /* And which population this merge was, read here, the only place both facts exist: the ask's program
               state is the local read at the door's entry, the record's is the flag its mint decided, and a later
               census holds only the second. */
            if (!ask_pre_program && g_eps[i].pre_program) g_ask_merged_pre_program++;
            goto done;
        }
    }
    if (g_eps_n >= g_eps_cap) { g_eps_cap = g_eps_cap ? g_eps_cap * 2 : 16; g_eps = realloc(g_eps, (size_t)g_eps_cap * sizeof(Endpoint)); CHECK(g_eps, "endpoint: OOM surface"); }
    Endpoint *e = &g_eps[g_eps_n++];
    memset(e, 0, sizeof *e);
    e->method = strdup(method); e->path = strdup(path); e->prov = prov;
    /* Whether any program had started when this record was born, read at the mint, the instant the fact is about;
       a read at the census would answer for the end of the run. */
    e->pre_program = !engine_any_program_started();
    /* And which mechanism composed it, decided here and never re-armed, for `pre_program`'s reason: a re-armed door
       would say a `<head>`'s own `<link>` was composed by a `fetch()` that merely asked for it again. */
    e->door = door;
    /* And whether the run had determined the address, written unconditionally rather than relying on the `memset`:
       a first sighting's class is the record's, and later ones can only raise it. */
    e->addr_class = acls;
    /* And the provenance that class was derived from, transferred rather than copied, since class and root are one
       sighting's fact (see the struct); the `done:` arm frees this local only where no record took it. NULL is the
       value's own answer, never a declined allocation (the CHECK at the read). */
    e->addr_root = aroot; aroot = NULL;
    /* And what the composing path had chosen, written unconditionally: the max's identity element only because
       `EPW_UNASKED` happens to be zero, and the list's order is a later diff's to change. */
    e->witness_class = wcls;
    /* And the ask and the mint are one instant. The bit is monotone and raised `++`-only at engine.c's single
       program-start line, so the two differ only if something on the path above (a display spelling, a path scan,
       a JSON body parse) runs page code. None does; the assert keeps a later diff from splitting the ask census's
       partition from the record's flag. */
    DCHECK(e->pre_program == ask_pre_program,
           "a program started between an endpoint ask reaching the @H surface and that ask minting its "
           "record — the two are counted as one instant, so the ask census's program-state partition and this "
           "record's own flag now grade the same sighting on either side of the boundary, and a reader "
           "computing what forced execution contributed would be subtracting two different populations");
    g_ask_minted++;
    if (kvb.n) { e->params = calloc((size_t)kvb.n, sizeof(Param)); CHECK(e->params, "endpoint: OOM params"); }
    for (int j = 0; j < kvb.n; j++) {
        e->params[e->np].name = strdup(kvb.e[j].name);
        e->params[e->np].loc = kvb.e[j].loc;
        param_add_val(&e->params[e->np], kvb.e[j].val);
        param_set_excl(&e->params[e->np], kvb.e[j].excl, kvb.e[j].nexcl);
        param_set_bound(&e->params[e->np].bnd, &kvb.e[j].bnd);
        param_set_pred(&e->params[e->np], kvb.e[j].pred, kvb.e[j].npred);
        param_set_leq(&e->params[e->np], kvb.e[j].leq, kvb.e[j].nleq);
        e->params[e->np].has_hole = kvb.e[j].has_hole;
        e->np++;
    }
    body_store(e, body, body_named);
    /* The count is dropped here: every header of a brand-new endpoint is new, and the discovery credited is the
       endpoint, so a request's header count does not enter the ranking. */
    (void)endpoint_merge_headers(e, hdrs, nhdrs);
    flow_credit_emit(1.0);   /* a new endpoint: this flow emitted value of information, the WFQ reward */
done:
    free(path);
    /* The root this call copied and no record took: a merge that did not raise the class. Both arms that keep it
       NULL the local on the same line. */
    free(aroot);
    kv_free(&kvb);
}

/* The asset decision's reader: solver/reply_decode.c asks what a reply is (of `computedType`, the one type decision,
   taken by the zone that read the bytes), and this marks the records whose replies were files, so an image's
   address is not reported as an API endpoint. The key is computed by the two steps `endpoint_record` keys by,
   called rather than restated, since a second normalisation would make a retraction silently match nothing.
   `url_display` is not among them: it is the identity on a concrete string, and a concolic request carries its
   shape to the host (core/fetch/fetch.c), so the string coming back with the reply is the display form this surface
   filed it under. A component that hands the host a serialization still owes this call the shape: XHR §3.5.1 The
   open() method parses, and URL Standard §1.3 "Percent-encoded bytes" puts U+007B ({) and U+007D (}) in the path
   percent-encode set, so a serialization spells holes `%7B…%7D`; core/xhr/xml_http_request.c answers through
   `xhr_request_address`, whose projection is this display form. It marks rather than deletes: the address may be
   recorded again in the same run and must stay suppressed, and dropping a record would shift its neighbours'
   params. Every same-(method, path) record is marked, since what the bytes were is a fact about the address. */
void endpoint_mark_asset(const char *method, const char *url) {
    KvBuf kvb = { 0 };
    char *upath, *path;

    DCHECK(method && *method && url && *url,
           "an asset verdict arrived naming no request — the reply register is keyed on the (method, url) pair "
           "the request was owed under, so a verdict missing either half names no record and would silently "
           "retract nothing while reading as a retraction");
    upath = url_path_of(url);
    path = path_scan(&kvb, upath, NULL);
    for (int i = 0; i < g_eps_n; i++)
        if (!strcmp(g_eps[i].method, method) && !strcmp(g_eps[i].path, path))
            g_eps[i].is_asset = 1;
    free(upath);
    free(path);
    kv_free(&kvb);
}

/* The numbers the emitted array is a fraction of (endpoint.h: its length alone is three states, and the fourth
   number says how much of the surface predates the page's own code running). One walk, the emitted arm spelled with
   the same `is_asset` skip as `endpoint_json_array`, so the two producers cannot disagree about what an endpoint
   is. */
void endpoint_surface_census(long *minted, long *assets, long *emitted, long *pre_program) {
    long a = 0, em = 0, pre = 0;
    DCHECK(minted && assets && emitted && pre_program,
           "the @H surface census was asked for with somewhere to put fewer than four of its numbers — the "
           "partition is the whole point and a caller taking one of them is reading a bare count again");
    for (int i = 0; i < g_eps_n; i++) {
        if (g_eps[i].is_asset) a++;
        else {
            em++;
            /* Counted inside the emitted arm only, so this is a subset of the figure it is read against rather
               than of the mint. */
            if (g_eps[i].pre_program) pre++;
        }
    }
    DCHECK(a + em == (long)g_eps_n,
           "the @H surface's asset and endpoint counts do not sum to the records minted — they are one walk "
           "over one array with a two-arm split, so a disagreement here is a record that is neither kind and "
           "the emitted figure is about a population this census cannot see");
    /* And the containment that makes the fourth number readable: pre-program records are a subset of the emitted
       ones, which a reader computing the complement must be able to assume; a value above `emitted` is a raise
       outside the skip. */
    DCHECK(pre <= em,
           "the @H surface counted more records minted BEFORE the first program than it emits at all — the "
           "pre-program count is raised only inside the emitted arm of this one walk, so a value above it is "
           "a second raise added outside that arm; a reader computing `emitted - preProgram` as the number of "
           "addresses running code could have composed would read a negative count as a large one");
    /* And the two relations that span the two instruments, asserted here, the only place the record array and the
       ask counts are in one hand (endpoint_ask_census asserts its own partitions and cut). `pre <=
       g_asks_pre_program`: every pre-program record was minted by an ask whose own bit was true (the mint asserts
       the pairing per record) and assets only make `pre` smaller, so if the two drift, `emitted - preProgram` and
       `asks - preProgram` are complements of two different boundaries; they are raised at different events on
       different structures, so the sides can disagree. `g_ask_minted == g_eps_n` fires on a record reaching `g_eps`
       by a path that did not pass endpoint_record's door: endpoint.h's funnel note, stated by something that cannot
       go stale. */
    DCHECK(pre <= g_asks_pre_program,
           "the @H surface emits more records minted before the first program than the door counted asks made "
           "before one — every such record was minted by such an ask and the assets skipped by this walk can "
           "only make the first number smaller, so a value above it means the ask census's program-state "
           "boundary and this walk's are two different boundaries, and `emitted - preProgram` and `asks - "
           "preProgram` are then complements of two different populations read as complements of one");
    DCHECK(g_ask_minted == (long)g_eps_n,
           "the @H surface holds a different number of records than the number of asks that minted one — the "
           "mint is counted on the one path that grows this array, so a disagreement is a record built "
           "somewhere else, and every figure on this line is then a fraction of a population the ask census "
           "cannot see while reading as a partition of the same surface");
    *minted = (long)g_eps_n; *assets = a; *emitted = em; *pre_program = pre;
}

/* The ask side of the same surface (endpoint.h has what it separates that the four numbers cannot; the statics at
   the head of this file, each row). A second call rather than more out-parameters on the walk above: these are
   monotone scalars raised on the three paths out of one door, not on the mutable array, so reading them at another
   instant costs nothing a reader can observe. The two partitions and the cut are asserted here, where all six are
   in hand; the cross identity against the record array is in endpoint_surface_census. */
void endpoint_ask_census(long *asks, long *pre_program, long *suppressed, long *merged, long *minted,
                         long *merged_pre_program) {
    DCHECK(asks && pre_program && suppressed && merged && minted && merged_pre_program,
           "the @H ask census was asked for with somewhere to put fewer than six of its numbers — the two "
           "partitions and the cut are the whole point and a caller taking one of them is reading a bare count again, "
           "which for this instrument means reading a total that cannot say whether running code ever composed "
           "a request at all");
    DCHECK(g_asks == g_ask_suppressed + g_ask_merged + g_ask_minted,
           "the @H surface's asks do not sum over the three things that can happen to one — they are three "
           "arms of ONE door, counted on the three paths out of it, so a total that moves without an arm "
           "moving is a fourth exit added above them, and the MERGED arm is the one a reader silently loses, "
           "which is exactly the population this census exists to make visible");
    DCHECK(g_asks_pre_program <= g_asks,
           "the @H surface counted more asks made BEFORE the first program than asks made at all — the "
           "pre-program count is raised only inside the one entry that raises the total, so a value above it "
           "is a second raise, and a reader computing `asks - preProgram` as the number of network call sites "
           "running code reached would read a negative count as a large one");
    /* And the two containments of the cut, asserted here, where every term of either is in hand; the cut selects on
       two independent facts, and each containment fires on a different way of getting it wrong. The first is
       against the arm it sits inside: it is raised only on the line after `g_ask_merged`, so a larger value is a
       raise outside that arm. */
    DCHECKF(g_ask_merged_pre_program <= g_ask_merged,
            "the @H surface counted more asks merging into a record minted before the first program (%ld) "
            "than asks that merged at all (%ld) — the cut is raised only inside the merge arm that raises the "
            "total, so a larger cut is a raise added outside it, and a reader taking the rest of the merges "
            "as the arm where running code re-composed its OWN addresses would read a negative count as a "
            "large one", g_ask_merged_pre_program, g_ask_merged);
    /* And the second is against the other fact the cut selects on: every member was asked with a program running,
       so it is contained in `asks - preProgram`, endpoint.h's separation between the two readings of a zero razor.
       The sides are raised at different events (the door's entry, the merge), so they part exactly when the door's
       program bit and the record's stop meaning one instant, the state the mint's DCHECK guards from the other
       side. */
    DCHECKF(g_ask_merged_pre_program <= g_asks - g_asks_pre_program,
            "the @H surface counted more asks merging into markup-minted records (%ld) than asks made after a "
            "program had started at all (%ld of %ld) — both facts are read at the same door, the ask's from "
            "the entry and the record's from its mint, so this fires when those two stop describing one "
            "instant and the cut is then a fraction of a population the ask census does not hold",
            g_ask_merged_pre_program, g_asks - g_asks_pre_program, g_asks);
    *asks = g_asks; *pre_program = g_asks_pre_program;
    *suppressed = g_ask_suppressed; *merged = g_ask_merged; *minted = g_ask_minted;
    *merged_pre_program = g_ask_merged_pre_program;
}

/* The edge hands over its own stage table once per session at its declaration: `js_fetch_steps[]` itself, a table of
   string literals with static storage that outlives every document, so the array sized from it is all that is given
   back. `first_stage` is the edge's to state: IDL_STEP_FIRST is idl_args.h's and a member's stages are numbered
   from it, so a row is keyed `steps[stage - first]` with the base the machine supplied; a constant here would break
   the day a member's prologue grows a stage. */
static void edge_declare(EndpointEdge *e, const char *who, const char *entry,
                         const char *const *steps, int first_stage) {
    int n = 0;

    DCHECKF(entry != NULL && entry[0] != '\0',
            "the %s edge declared no global entry identifier — the compile report keyed on it matches nothing, "
            "so this edge's `named` rows would read zero for every document and a reader would take that for a "
            "corpus of programs none of which spells the name, which is the one reading those rows exist to "
            "make impossible", who);
    DCHECKF(steps != NULL && steps[0] != NULL,
            "the %s edge declared no stage table at all — the census keyed on it would have a row for every "
            "state it counts and no name to put on one, and a histogram whose buckets cannot be named is a "
            "bare count wearing a partition's shape", who);
    while (steps[n]) n++;
    /* Re-declared with the same table is ordinary and costs nothing (a host that opens two sessions runs this
       twice). A different table is a second machine reaching this edge's rows, which are keyed by one machine's
       stages, so two machines summed into one histogram would be an averaged population. Per edge, which is why
       there are two structs: core/fetch and core/xhr each declare their own. */
    if (e->steps) {
        DCHECKF(e->steps == steps && e->stage_first == first_stage && e->stage_n == n && e->entry == entry,
                "a SECOND machine declared itself to the %s edge census, or the same machine declared a "
                "different table or a different global entry identifier — these rows are keyed by ONE "
                "machine's stages, so a second one's arms summed into them would publish a histogram of two "
                "populations under one edge's name, and a second entry name would file one edge's compile "
                "report under two spellings of which only the last would ever match", who);
        return;
    }
    e->died = (long *)calloc((size_t)n, sizeof *e->died);
    /* Allocation, so a CHECK: OOM is fatal in release too, and the alternative is a partition arm every teardown
       writes through as NULL. */
    CHECKF(e->died != NULL, "the %s edge census could not allocate its per-stage partition", who);
    e->steps = steps;
    e->stage_n = n;
    e->stage_first = first_stage;
    e->entry = entry;
}

/* The compile report, matched against one edge. `name` is bytes valid for this call only (quickjs converts the atom
   into its own frame's buffer, keeping this seam free of a runtime-lifetime atom reference the leak report at
   JS_FreeRuntime would name), so a match is a compare, never a keep. An edge that has not declared has a NULL
   `entry` and matches nothing. It also answers whether this edge claims the name as a use, since `JS_OrphanTakeOne`
   orders its candidates by it and the entry name's owner is here. A `typeof` read answers no and still raises its
   own row: `typeof fetch === "function"` is a feature detect, and driving the body that spells it reaches the
   guard, never the call, so claiming it would order capability probes ahead of request builders. The census keeps
   both; only the order reads one. */
static int edge_named(EndpointEdge *e, const char *name, int typeof_only) {
    if (!e->entry || strcmp(e->entry, name) != 0) return 0;
    if (typeof_only) { e->named_typeof++; return 0; }
    e->named++;
    return 1;
}

/* And the state is gone. `stage` is where it stood, `reached` whether its construction had completed (the fetch
   edge's offer, the XHR edge's placement; endpoint.h states which and why). The two arms are the partition, two arms
   of one teardown, so a third way out cannot be added without landing in neither, which the accessor's identity
   fires on. */
static void edge_freed(EndpointEdge *e, const char *who, int stage, int reached) {
    int i = stage - e->stage_first;

    DCHECKF(e->steps != NULL,
            "a %s state was torn down before its machine declared its stages — the census cannot name the arm "
            "this teardown belongs to, and a partition with an unnameable arm is a total with a hole in it",
            who);
    /* A CHECK, for absent.c's vocabulary bound's reason: the line below indexes the array with this value in every
       build, and a state standing at a stage its own machine does not name is a fact about the driver, not a row to
       file. */
    CHECKF(i >= 0 && i < e->stage_n,
           "a %s state was torn down standing at stage %d, which its machine's own table does not name "
           "(it declares %d stages based at %d) — the stage a machine holds is asserted against its "
           "declaration by the step driver, so a value outside it here is this census reading a stage the "
           "driver never wrote or a machine numbering its stages from something other than what it declared",
           who, stage, e->stage_n, e->stage_first);
    e->freed++;
    if (reached) e->freed_reached++;
    else e->died[i]++;
}

void endpoint_fetch_edge_declare(const char *entry, const char *const *steps, int first_stage) {
    edge_declare(&g_fetch_edge, "fetch", entry, steps, first_stage);
}

/* The same declaration for the other edge, with `SEND_STEPS` rather than the lifecycle machine's table (endpoint.h
   has the argument): `send()` is where an XMLHttpRequest request is constructed and can throw or park before a door,
   and the lifecycle machine's twenty stages are downstream of the record. `first_stage` is the edge's to state for
   core/fetch's reason: SEND_STAGES is based at IDL_STEP_FIRST, core/idl_args.h's constant. */
void endpoint_xhr_edge_declare(const char *entry, const char *const *steps, int first_stage) {
    edge_declare(&g_xhr_edge, "xhr send", entry, steps, first_stage);
}

/* The property spelling of an entry, `JSConcolicHooks.global_member_named`, reached through solver/concolic.c's
   dispatch rather than installed on that table directly, because this is its second consumer and the receiver test
   is not this file's: whether `base` denotes the global object is a fact about which names a realm binds to
   itself, stated once in the dispatch, so `base` never arrives here. Keyed on the edge's own `entry`, as its
   sibling is, so the order and the census cannot disagree about which names are doors; both edges are offered the
   member. It raises its own row and answers the order; there is no `typeof` parameter (see the field), so a body
   spelling `globalThis.fetch` may be probing, and ordering a probe early costs a position, never an answer. */
int endpoint_compile_global_member(const char *member) {
    int hit = 0;

    if (!member) return 0;
    if (g_fetch_edge.entry && strcmp(g_fetch_edge.entry, member) == 0) { g_fetch_edge.named_prop++; hit = 1; }
    if (g_xhr_edge.entry   && strcmp(g_xhr_edge.entry,   member) == 0) { g_xhr_edge.named_prop++;   hit = 1; }
    return hit;
}

/* The compiler resolved a free identifier against the global object, keyed on the name the edge declared and nothing
   else. quickjs calls this once per occurrence for every free identifier in every program this session compiles: it
   cannot know which are network entry points and must not be told, since a list of door names spelled in the
   compiler would read, for a name removed from it, as a program that never spelled it. Both edges are offered the
   name and both are asked whatever the first answers, so a name belonging to two would raise both rows rather than
   whichever `if` ran first; the return is the order's and the rows the census's, OR'd. The two entries are disjoint
   by construction (a member name and an interface name of two standards). It costs two length-guarded compares per
   free identifier, once per compile, on the compiler's own time and inside no flow's slice; it allocates nothing
   and raises no ask. */
int endpoint_compile_global_named(const char *name, int typeof_only) {
    int a, b;

    if (!name) return 0;
    a = edge_named(&g_fetch_edge, name, typeof_only);
    b = edge_named(&g_xhr_edge, name, typeof_only);
    return a || b;
}

/* A declared member's call entered the hosting machine's prologue, keyed on the stage table the caller hands over and
   nothing else. core/idl_args.c calls this once per call for every declared member; it cannot know which are
   network edges and must not be told, because a list of network members spelled there would drift silently (a
   member removed from it reads as a door nobody called). The key is the pointer, not a name: both edges declared
   themselves with their own X-list (core/fetch's `js_fetch_steps[]`, core/xhr's `SEND_STEPS`), static tables
   core/idl_args.c holds on the member's own `IdlStepDecl`, so the sides agree by identity. An edge that never
   declared has a NULL `steps` and matches nothing; a member that declares no steps passes NULL and is dropped
   here. */
void endpoint_edge_member_asked(const char *const *steps) {
    if (!steps) return;
    if (steps == g_fetch_edge.steps)    g_fetch_edge.called++;
    else if (steps == g_xhr_edge.steps) g_xhr_edge.called++;
}

/* The construction began, raised at the machine's one-time capture, the first line of its first stage. A deep-fork
   copy inherits that flag and does not come through here, which is why endpoint.h forbids pairing this row with
   either teardown row by a containment. */
void endpoint_fetch_edge_began(void) { g_fetch_edge.began++; }

/* And it reached the door, raised on the line before the edge's own endpoint_record call: this counts offers, never
   records, since the gate may still suppress one. */
void endpoint_fetch_edge_offered(void) { g_fetch_edge.reached++; }

void endpoint_fetch_edge_freed(int stage, int offered) { edge_freed(&g_fetch_edge, "fetch", stage, offered); }

/* The other edge's three, and the second is a placement, not an offer: `send()` hands the constructed request to the
   lifecycle machine at §3.5.6 step 12 or 13 and is then done with it, so its teardown can say whether the
   construction completed and not whether an address was offered; hence the partition over the placement and the
   offer as a row of its own. */
void endpoint_xhr_edge_began(void) { g_xhr_edge.began++; }
void endpoint_xhr_edge_placed(void) { g_xhr_edge.reached++; }
void endpoint_xhr_edge_freed(int stage, int placed) { edge_freed(&g_xhr_edge, "xhr send", stage, placed); }

/* And the address this edge offered the surface, raised on the line before `xhr_record_endpoint`'s endpoint_record
   call, inside the lifecycle machine: a fact about this edge and no send state (endpoint.h has why no inequality
   between it and the placement row may be asserted). */
void endpoint_xhr_edge_offered(void) { g_xhr_offered++; }

/* The census, as rows of `_cold` rather than a census of its own (endpoint.h has every row and the identities over
   them; the stage arms are a partition, not a ladder, so a zero in one stage says nothing about its neighbours). */
/* One number, in JSON's own spelling. core/json_buf.h writes a value a caller already formatted, as solver/solve.c
   does; a fixed buffer is fine because the width of a `%ld` is a property of the type. The return is checked: a
   truncation would publish a number missing its leading digits. */
static void edge_num(JsonBuf *b, long v) {
    char t[32];
    int n = snprintf(t, sizeof t, "%ld", v);

    DCHECK(n > 0 && (size_t)n < sizeof t,
           "a census number did not fit its own conversion buffer — the width of a long's decimal form is a "
           "property of the type, so this is a host whose long is wider than this buffer was written for and "
           "the number about to be published has lost its leading digits");
    json_buf_raw(b, t);
}

/* The emitted surface partitioned by the mechanism that composed each address (endpoint.h has what a door is and why
   neither `prov` nor the program-state flag can stand in for one): these rows say which mechanism each emitted
   address came through, which `epEmitted - epPreProgram` only bounds from above. Every door is emitted including
   zeroes: a table of only its nonzero rows needs its shape known before a reader can tell a mechanism that never ran
   from one that does not exist, and extension/bridge.js refuses an empty histogram on this census by name. The keys
   are computed (door tokens off `ENDPOINT_DOORS`, through the value entry), not field names, which `json_buf_key`'s
   literal rule keeps for fields. The comma is a latch, not the loop index, because the loop starts one past
   `EPD_UNSTATED`. The identity is a cross-check between two producers: this loop and endpoint_surface_census's walk
   `g_eps` with the same `is_asset` skip as `endpoint_json_array`, so it fires on one of the three skips changing
   alone; extension/bridge.js and engine/build.mjs re-check the sum against the emitted document, the independent
   half. */
char *endpoint_door_hist_json(void) {
    JsonBuf b = { 0 };
    long n[EPD_COUNT];
    long minted, assets, emitted, pre_program, sum = 0;
    int wrote_one = 0, d, i;

    memset(n, 0, sizeof n);
    for (i = 0; i < g_eps_n; i++) {
        if (g_eps[i].is_asset) continue;
        /* The range is re-asserted at the read, because the subscript is on the next line and the mint's check is
           compiled out of release: an out-of-range door would write outside this array. In release
           `endpoint_door_token` still refuses the record at the emit, so nothing publishes a fabricated door; this
           keeps the frame intact on the way. */
        DCHECKF(g_eps[i].door > EPD_UNSTATED && g_eps[i].door < EPD_COUNT,
                "an @H record reached the door census carrying the door %d, which is none of endpoint.h's "
                "ENDPOINT_DOORS — the mint refuses an unstated one, so this is a record that reached this "
                "surface in a build where that assert was compiled out, and the count about to be raised is "
                "at an index outside the table this census is a partition of", g_eps[i].door);
        n[g_eps[i].door]++;
    }
    endpoint_surface_census(&minted, &assets, &emitted, &pre_program);

    json_buf_raw(&b, "{");
    for (d = EPD_UNSTATED + 1; d < EPD_COUNT; d++) {
        if (wrote_one) json_buf_raw(&b, ",");
        wrote_one = 1;
        json_buf_str(&b, endpoint_door_token(d));
        json_buf_raw(&b, ":");
        edge_num(&b, n[d]);
        sum += n[d];
    }
    json_buf_raw(&b, "}");
    DCHECKF(sum == emitted,
            "the @H surface's per-door counts sum to %ld against the %ld rows it emits — the doors are a "
            "PARTITION of the emitted surface and the two are one array walked twice with the same asset "
            "skip, so a difference is one of those walks having stopped describing the population the other "
            "counts, and a reader taking a door's share of the surface would be taking a fraction of a "
            "number that is not its size", sum, emitted);
    return json_buf_take(&b);
}

/* THE SAME SURFACE PARTITIONED BY WHAT A PARSE OF THE DOCUMENT WOULD HAVE REACHED — see endpoint.h for the
   three classes, for why there are three and not two, and for why the map is a column of the door list rather
   than a table anywhere. `beyond` is the addresses a MARKUP parse does not reach, `either` is the population
   no door can decide, and `markup` is the `<head>` counted back.
   THIS OPENING READ THAT THE ROW IS CLAUDE.md §What-the-tool-produces' RAZOR STATED, AND IT IS REWRITTEN
   RATHER THAN DELETED BECAUSE A READER HOLDING A ROW NAMED `beyond` RE-DERIVES EXACTLY IT. endpoint.h records
   that same sentence as retired at this function's own declaration: the bar became a COLUMN, composed by
   `endpoint_razor_class_of` out of `addressClass` and the door list's FOURTH column, and this row is keyed on
   the THIRD, which is what a markup parse reaches. They are not one question at two grains, because that
   section names BY HAND a literal chunk URL through `module-import` as `beyond` and as scoring ZERO on the bar.
   THE HEADER WAS REPAIRED AND THE TWO FILES THAT STATE THE CENSUS WERE NOT, which is the one-header-two-answers
   defect endpoint.h already records one door over, arriving as the sweep a retirement leaves behind rather than
   as a fresh claim: the argument was retired where it is DECLARED and stayed written down where it is
   PUBLISHED, here and at solver/result.c's `epReach` row. That is CLAUDE.md §A-FIX-THAT-RETIRES-AN-ARGUMENT,
   whose whole point is that such a fix's code delta is not its size.
   AND THE COST WAS NOT A READING, WHICH IS WHY IT IS RECORDED RATHER THAN QUIETLY CORRECTED. Both emitters
   that show a person a razor figure off this census compose it out of a DOOR COUNT: engine/build.mjs's
   `endpointReachReading` takes this row and returns a line beginning `@H razor:`, and extension/popup.js reads
   it into a row labelled `razor`. That assembly is the one CLAUDE.md §What-the-tool-produces' own retirement
   clause asks to be made impossible, and this sentence was the tree INVITING it.
   RETIREMENT: this record goes when this census publishes the COMPOSED bar beside this row, because a reader
   holding `beyond` then has the bar itself in the same document and has nothing left to re-derive it from.
   IT IS A COARSENING OF THE ROW ABOVE AND NOT A SECOND OBSERVATION. Every count here is the sum of the door
   counts of its class, so a reader holding both histograms holds ONE fact at two grains and two zeroes here
   are one zero — CLAUDE.md §EVIDENCE-INFLATION, whose cure is that a derived row names its derivation where
   the number is rather than leaving a reader to find it by reading the producer. WHAT IT CARRIES THAT THE
   DOOR ROW DOES NOT is the map itself, which lived as incomplete prose in engine/build.mjs's verdict line and
   in testing/static_surface.mjs and as data nowhere — so no output in this tree could state the MARKUP REACH
   at all, and a coordinator who drove a real app had to read the emitted @H array by hand to find it read ZERO.
   A SECOND WALK AND NOT A SUM OVER `endpoint_door_hist_json`'s TABLE, which is deliberate and is this file's
   existing rule: that census and `endpoint_surface_census` are already two walks over `g_eps` spelled with
   the SAME `is_asset` skip precisely so that one of them drifting is a LOUD identity failure rather than a
   quiet agreement, and a third walk of that shape earns the same check. Summing that table instead would
   make this row true by construction of it and blind to the walk it is supposed to be checking.
   THE SUBSCRIPT NEEDS NO RANGE ASSERT AND THAT IS NOT AN OMISSION. `endpoint_door_reach` is generated from the
   door list and its fallthrough is a `CHECK` — fatal in EVERY build, unlike the mint's own `DCHECK` — so it
   returns a member of `ENDPOINT_REACHES` or it does not return. An assert here could not fail, and CLAUDE.md
   §AN-ASSERT-WHOSE-TWO-SIDES-CANNOT-DISAGREE rates a non-check wearing a check's syntax as worse than none:
   it certifies what it never examined. `endpoint_door_hist_json` asserts its range because ITS subscript is
   the raw `door` field off the record, which is a different operand with a different guard. */
char *endpoint_reach_hist_json(void) {
    JsonBuf b = { 0 };
    long n[EPR_COUNT];
    long minted, assets, emitted, pre_program, sum = 0;
    int r, i;

    memset(n, 0, sizeof n);
    for (i = 0; i < g_eps_n; i++) {
        if (g_eps[i].is_asset) continue;
        n[endpoint_door_reach(g_eps[i].door)]++;
    }
    endpoint_surface_census(&minted, &assets, &emitted, &pre_program);

    /* EVERY CLASS IS EMITTED INCLUDING THE ZEROES, for the door census's reason and with a sharper one here:
       a `beyond` that is ABSENT and a `beyond` that read 0 are the two things this row exists to keep apart,
       and a table listing only its non-empty classes renders a run that learned nothing beyond the markup
       identically to a run whose composer stopped writing the class. The first is THIS MARKUP DIAGNOSTIC
       answering and is a REFUSAL TO CLAIM the reach it measures — never the product's own bar, which
       `endpoint_razor_class_of` composes and which no count of doors reaches at any grain; the second is an
       instrument that stopped. */
    json_buf_raw(&b, "{");
    for (r = 0; r < EPR_COUNT; r++) {
        if (r) json_buf_raw(&b, ",");
        json_buf_str(&b, endpoint_reach_token(r));
        json_buf_raw(&b, ":");
        edge_num(&b, n[r]);
        sum += n[r];
    }
    json_buf_raw(&b, "}");
    DCHECKF(sum == emitted,
            "the @H surface's per-reach counts sum to %ld against the %ld rows it emits — the three classes "
            "are a PARTITION of the emitted surface and this walk carries the same `is_asset` skip the census "
            "beside it does, so a difference is one of those walks having stopped describing the population "
            "the other counts, and the markup reach a reader reads off this row would be a share of a "
            "number that is not the surface's size", sum, emitted);
    return json_buf_take(&b);
}

/* THE SAME SURFACE PARTITIONED BY WHETHER THE RUN HAD DETERMINED THE ADDRESS AND WHOSE UNKNOWN IT RESTS ON —
   see endpoint.h for what each class claims, for why there is no unstated member, and for why the honest
   field is a FLOOR under CLAUDE.md §What-the-tool-produces' HARD BAR rather than a boolean about what a parse
   could reach. `unknown` is the addresses this run reached holding a value it had not determined AND resting
   on an unknown somebody OUTSIDE this engine supplied, which is the one figure the hard bar can be scored
   off; `unknown-unproven` is undetermined and refused by that bar; `concrete` and `source-determined` are the
   rest, and neither claims anything whatever about a parse. (It read "the two classes" and "`concrete` is the
   rest", which is retired rather than deleted because it is what a reader re-derives from a boolean reading
   of this row: the list has grown twice and each widening carved a population OUT of the bar's claim.)
   IT IS A SECOND OBSERVATION AND NOT A COARSENING, WHICH IS WHERE IT DIFFERS FROM THE ROW ABOVE AND IS WHY IT
   EARNS ITS OWN WALK. `endpoint_reach_hist_json` is the door census summed by class and says so, so a reader
   holding both of those holds ONE fact at two grains. This is keyed on a property of the ADDRESS VALUE that
   no door implies in either direction — `link-element` is reached by a determined address and by a tainted
   one alike, and `reply-chunk` is `beyond` a parse with a determined address every time — so a reader holding
   all three histograms holds TWO observations, and CLAUDE.md §EVIDENCE-INFLATION is why that is said here,
   where the numbers are, rather than left to be worked out from the producers.
   A THIRD WALK OVER `g_eps` AND NOT A SUM OVER EITHER TABLE, which is this file's existing rule for the same
   reason it gave for the second: the walks are spelled with the SAME `is_asset` skip precisely so that one of
   them drifting is a LOUD identity failure rather than a quiet agreement, and a table summed from another is
   true by construction of it and blind to the walk it is supposed to be checking.
   THE SUBSCRIPT IS RE-ASSERTED AND THAT IS NOT REDUNDANT WITH THE TOKEN SPELLER. `endpoint_address_class_token`
   is a `CHECK` in every build, so the EMIT cannot publish a fabricated word — and this line indexes an array
   before any speller runs, which is `endpoint_door_hist_json`'s own reason for asserting its range: what the
   assert adds is that nothing corrupts the frame on the way to the refusal. BOTH SIDES ARE THIS CODEBASE'S
   OWN (a field this file wrote from a predicate this file spells), which is what makes it a DCHECK rather
   than a refusal about page bytes.
   AND THE RANGE ASSERT IS NO LONGER THE ONLY ONE HERE, WHICH IS WHAT EACH WIDENING OF THAT LIST COST — the
   difference `endpoint_witness_hist_json` already carries, arriving at this list for the same reason and with
   one more thing to hold at every widening. This list is ORDERED and the record's merge is a MAX over that
   order, so the thing a later diff can break WITHOUT going out of range is the ORDER ITSELF, and THREE facts
   about it are load-bearing: the LOW end, because `EPA_CONCRETE` is the identity element the `memset` leaves
   behind; the HIGH end, because `endpoint_razor_class_of` keys the product's own bar on `EPA_UNKNOWN` — so a
   reordering that moved it off the top would let a MAX carry a record OUT of the class the bar reads, and the
   bar would quietly stop claiming an address some path had proved; and the DETERMINED MEMBERS BEING A PREFIX,
   which the two ends cannot see and which the fourth member made load-bearing, because the list's own Q1 is a
   THRESHOLD once more than one member answers NOT DETERMINED. All three are checkable here, where the whole
   array is in one hand, and all three CAN fail: each names members of a list a later diff is free to think is
   arbitrary. */
char *endpoint_address_hist_json(void) {
    JsonBuf b = { 0 };
    long n[EPA_COUNT];
    long minted, assets, emitted, pre_program, sum = 0;
    int c, i;

    DCHECK(EPA_CONCRETE == 0,
           "endpoint.h's ENDPOINT_ADDRESS_CLASSES no longer begins with the member that PROVES NOTHING — the "
           "record's merge is a MAX over this list's order and `memset` leaves a fresh record at 0, so a "
           "reordering makes that zero mean a claim the mint never made");
    DCHECK(EPA_UNKNOWN == EPA_COUNT - 1,
           "endpoint.h's ENDPOINT_ADDRESS_CLASSES no longer ENDS with the member CLAUDE.md "
           "§What-the-tool-produces' hard bar is keyed on — `endpoint_razor_class_of` reads `EPA_UNKNOWN` and "
           "the record's merge is a MAX over this order, so a member above it lets a later sighting carry a "
           "record out of the one class that clears the bar, and this surface would stop claiming an address "
           "a path had already proved stands in no served byte");
    /* …AND THE THIRD FACT ABOUT THIS ORDER, WHICH THE TWO ENDS ABOVE CANNOT SEE AND WHICH THE FOURTH MEMBER
       MADE LOAD-BEARING. The list's two PREDICATES are asked of one stored member, and while every
       undetermined address was ONE member they were one test negated; they are not now, so Q1 is spelled as a
       THRESHOLD (`cls <= EPA_SOURCE_DETERMINED`) and that spelling is correct only while the DETERMINED
       members are a PREFIX of the order. A determined member added ABOVE an undetermined one satisfies both
       ends above, keeps every count summing, and makes Q1 answer DETERMINED for a row no path determined —
       which is the one reading this list's own banner hands a consumer and the one nothing else here pins.
       IT CAN FAIL, which is what separates it from a restatement: it names two members of a list a later diff
       is free to think is arbitrary, and both are in one hand here. */
    DCHECK(EPA_SOURCE_DETERMINED < EPA_UNKNOWN_UNPROVEN,
           "endpoint.h's ENDPOINT_ADDRESS_CLASSES no longer has its DETERMINED members as a PREFIX of the "
           "order — that list's banner spells Q1 as `cls <= EPA_SOURCE_DETERMINED`, which is a threshold and "
           "is the only spelling available once more than one member answers NOT DETERMINED, so a determined "
           "member standing above an undetermined one makes every consumer of that reading answer that the "
           "run had determined an address no path determined");
    memset(n, 0, sizeof n);
    for (i = 0; i < g_eps_n; i++) {
        if (g_eps[i].is_asset) continue;
        DCHECKF(g_eps[i].addr_class >= 0 && g_eps[i].addr_class < EPA_COUNT,
                "an @H record reached the address-class census carrying the class %d, which is none of "
                "endpoint.h's ENDPOINT_ADDRESS_CLASSES — the mint writes this field unconditionally from "
                "`address_class_of`, which returns a member of that list on every one of its arms, so this is "
                "a record minted by something that is not that line, and the count about to be raised is at "
                "an index outside the table this census is a partition of", g_eps[i].addr_class);
        n[g_eps[i].addr_class]++;
    }
    endpoint_surface_census(&minted, &assets, &emitted, &pre_program);

    /* EVERY CLASS IS EMITTED INCLUDING THE ZEROES, for the reach census's reason and with the same sharper
       one at the class that matters: an `unknown` that is ABSENT and an `unknown` that read 0 are the two
       things this row exists to keep apart. The first is an instrument that stopped writing the class; the
       second is the product's own hard bar answering, and it is a REFUSAL TO CLAIM the capability on this
       document rather than a smaller version of it. */
    json_buf_raw(&b, "{");
    for (c = 0; c < EPA_COUNT; c++) {
        if (c) json_buf_raw(&b, ",");
        json_buf_str(&b, endpoint_address_class_token(c));
        json_buf_raw(&b, ":");
        edge_num(&b, n[c]);
        sum += n[c];
    }
    json_buf_raw(&b, "}");
    DCHECKF(sum == emitted,
            "the @H surface's per-address-class counts sum to %ld against the %ld rows it emits — the "
            "classes are a PARTITION of the emitted surface and this walk carries the same `is_asset` skip "
            "the census beside it does, so a difference is one of those walks having stopped describing the "
            "population the other counts, and the hard bar a reader reads off this row would be a share of a "
            "number that is not the surface's size", sum, emitted);
    /* THE RESIDUAL THAT STOOD HERE IS RETIRED, AND BOTH OF ITS CLAUSES WERE WRONG — RECORDED RATHER THAN
       DELETED, BECAUSE A READER HOLDING TWO MARGINALS RE-DERIVES BOTH OF THEM IN THIS ORDER.
       ITS NOT-COVERED CLAUSE SAID "no surface anywhere carries, FOR ANY ROW, both of those facts at once",
       AND THE PER-RECORD EMITTER CARRIES `door` AND `addressClass` ON EVERY ROW, unconditionally, in every
       build, and has since before that sentence was written. What was true is the ARGUMENT under it — this
       walk and `endpoint_reach_hist_json`'s are MARGINALS over one array, and a union is a statement about
       per-row MEMBERSHIP that no marginal carries — and a clause that is right about its mechanism and wrong
       about the tree is the shape CLAUDE.md rates worst, since a reader who checks the reasoning finds it
       sound and only a reader who greps the emitter finds nothing there.
       ITS WHAT-THE-NEXT-DIFF-BUILDS CLAUSE NAMED THE WRONG OPERAND, WHICH IS THE HALF THAT WOULD HAVE BEEN
       EXECUTED. It said to raise a scalar over the rows that are `unknown` OR `endpoint_door_reach(door) ==
       EPR_BEYOND`, and `EPR_BEYOND` is what a MARKUP parse cannot reach while the bar is keyed on a parse of
       the SERVED BYTES — so that scalar would have counted every `fetch`, `xhr`, `beacon` and `module-import`
       row, which is essentially the whole of a real surface and is the figure CLAUDE.md
       §What-the-tool-produces DEMOTED by name ("a literal chunk URL delivered through `module-import` is
       `beyond` and scores ZERO here"). It would also have been an OVER-claim published as a FLOOR, which is
       the one direction a floor may not be wrong in.
       WHERE IT LANDED INSTEAD is `endpoint_razor_class_of`, composed PER ROW at the emitter out of
       `addressClass` and the door list's own FOURTH column — the set the prose operand was reaching for, now
       data, with `EPB_OFF_DOCUMENT` as its single member today. A scalar here would additionally have been a
       third marginal over a population the emitted array already answers per row, and a census cannot be the
       emitter CLAUDE.md's retirement condition asks for: this file's censuses reach a reader only through
       solver/result.c's @COLD line, while the razor figure a person is shown is composed in the trusted zone
       off the emitted array.
       AND THAT LAST CLAUSE IS REFUTED, KEPT RATHER THAN DELETED BECAUSE IT IS WHAT A READER HOLDING THE NEW
       PER-ROW COLUMN RE-DERIVES — it is the conclusion that leaves this census alone. Its premise was that the
       razor figure A PERSON IS SHOWN is composed in the trusted zone off the emitted array, which was true when
       it was written. MEASURED by reading the consumers rather than inferring from them: engine/build.mjs's
       `endpointReachReading` calls `censusHistRows` on `epReach` and returns a line beginning `@H razor:`, and
       extension/popup.js reads `m.cold.epReach` into a row labelled `razor` and `m.cold.epAddressClass` into one
       labelled `hard bar`. THREE razor figures reach a person and TWO are composed off THIS CENSUS, one of them
       out of a DOOR COUNT — which is the assembly that retirement clause asks to be made impossible. What the
       clause was right about is the MECHANISM, and it decides the fix's shape: a union is a statement about
       per-row MEMBERSHIP, so no marginal carries it and a SUM of these two tables means neither.
       THE RESIDUAL IS RETIRED AND ITS ARGUMENT IS KEPT, WHICH IS THE WHOLE OF WHY THIS PARAGRAPH IS STILL
       HERE. Its NOT-COVERED clause read "this census publishes the union's TWO OPERANDS and the union itself
       nowhere, so an @COLD reader has no row stating the bar and the nearest thing to one is keyed on the
       DOOR", and `endpoint_razor_hist_json` below is the fourth walk it named — spliced by solver/result.c as
       `epRazorClass`, declared a GAUGE beside `epReach`, and read by engine/build.mjs's
       `endpointRazorClassReading`, all in the one commit its scope clause said it had to be. What a reader
       holding two marginals re-derives is not the gap, which is closed; it is the ARGUMENT, which is that a
       union is a claim about per-row MEMBERSHIP and can therefore be assembled from neither table nor from
       both — and a reader who re-derives THAT will propose the sum, which is why it is not deleted with the
       clause it justified.
       AND THE SUM IS WRONG ARITHMETICALLY AND NOT MERELY WEAKLY, WHICH THE RETIRED CLAUSE NEVER SAID AND IS
       THE SHARPER REASON: the two operands OVERLAP, so a `reply-chunk` row whose address the run had not
       determined is counted by this walk AND by `endpoint_reach_hist_json`'s and is ONE row of the razor
       partition. A reader adding the two floors gets a number larger than the surface.
       ITS SCOPE CLAUSE WAS THE HALF THAT HELD AND IT HELD EXACTLY, which is worth recording because it is the
       clause CLAUDE.md rates as the least reliable kind: `censusRowSet` compares the object rows it is GIVEN
       against the ones it derives from this composer's own format string and THROWS on either difference, and
       `censusComposerFields` takes that source through `hostSource`, a `readFileSync(join(HOST, file))` of THE
       WORKING TREE — so a `%s` row written without the matching `coldFields()` entry reddens every lane's
       build from the moment it is WRITTEN, before it is committed. MEASURED by slicing that derivation out and
       running it at both revisions rather than by reasoning about it: at the parent it answers "list has,
       composer lacks: [epRazorClass]", and at the landing it agrees in both directions. That is the in-between
       state §A-CROSS-BOUNDARY-DIFF forbids, and the control is ARMED rather than assumed.
       AND THIS RECORD FIRST NAMED A DEFECT IN THE TRUSTED ZONE THAT IS NOT THERE, WHICH IS AN OVER-CLAIM BY
       THE AUTHOR OF THE LANDING AND IS CORRECTED HERE RATHER THAN QUIETLY DROPPED. It read: "extension/popup.js
       renders `m.cold.epReach` under a row labelled `razor`, which is the same defect this landing repaired in
       engine/build.mjs's verdict line — a person shown the word `razor` over a count of doors". BOTH HALVES ARE
       WRONG and one grep of that file refutes them, which is the whole hazard: §AN-OVER-CLAIM-IS-REFUTABLE says
       a reader who runs the cheap check discards the true part with the false, and the true part here is the
       retirement above it. Its label is `razor — what this engine reached that a markup parse could not`, so
       the qualification engine/build.mjs's bare `@H razor:` lacked is IN the label; and the block directly
       beneath it already states in its own words that the overlap sentence "is retired by this one and is kept
       in the block above rather than deleted, because a reader holding two marginals re-derives exactly it".
       That zone had already done this work at the per-row grain.
       WHAT IS TRUE IS NARROWER AND IS NOT A DEFECT: that file's sentence "Neither producer composes the union
       and neither does this" is now false in a SECOND way, because this census composes it too and not only
       the emitter — and the paragraph that retires it is already there, so nothing is owed. Its rows stay
       separate for a reason this row does not disturb: its denominator is `endpoints`, the emitted ARRAY's
       length, while every row here is over `epEmitted`, a GAUGE taken at an instant, so the two are one
       population at two moments and that view asserts no identity between them.
       THE TELL THAT PRODUCED THE WRONG SENTENCE IS THE ONE CLAUDE.md ALREADY GIVES AND IT COSTS ONE GREP: the
       claim was about ANOTHER FILE, written while reading this one, from what a row of that name WOULD be
       called rather than from what the file says. */
    return json_buf_take(&b);
}

/* THE SAME SURFACE PARTITIONED BY THE BAR RATHER THAN BY ONE OF ITS OPERANDS — see endpoint.h for what each
   class claims, for why the honest field is a FLOOR, and for why a union is not a coarsening of the two
   marginals above it. `endpoint_razor_class_of` is the whole of the keying: `runtime-only` where the run had
   not determined the address AND its unknown is one somebody OUTSIDE this engine supplied, OR where the
   door's bytes are not the served document's; `unproven` otherwise.
   A FOURTH WALK OVER `g_eps` AND NOT A SUM OF THE TWO TABLES, which is this file's rule for the second and
   the third and is not merely a preference here: the two operands OVERLAP, so a row that is `unknown` AND
   `EPB_OFF_DOCUMENT` is counted once by each marginal and is ONE row of this partition — a sum would be
   arithmetically wrong and not just weak. The `is_asset` skip is spelled identically to the three walks above
   precisely so that one of them drifting is a LOUD identity failure rather than a quiet agreement.
   THE `addr_class` RANGE IS RE-ASSERTED AND THE `door` IS DELIBERATELY NOT. A corrupt `addr_class` would fall
   through `endpoint_razor_class_of`'s first arm into `unproven`, which is an UNDER-claim published as a floor
   and is the one direction a floor may not be wrong in — so the same operand the walk above asserts is
   asserted here, where this walk reads it. `door` is left to `endpoint_door_bytes`, whose own `CHECK_FAILF` is
   fatal in EVERY build rather than dev-only, and this walk never subscripts an array by `door`: a second
   assert here would be a weaker copy of a stronger check, standing where nothing indexes.
   IT IS A CROSS-CHECK AGAINST THE PER-ROW `razorClass` COLUMN AND NEVER A SUM WITH IT. Both come off this one
   predicate, so they are ONE observation at two grains, and they are worth having together only because they
   are read at two INSTANTS over a GAUGE — `epEmitted` FALLS when an asset verdict lands between a census and
   the emission, so a difference names which records each document was describing and is not an error in
   either. Adding them would double-count the whole surface. */
char *endpoint_razor_hist_json(void) {
    JsonBuf b = { 0 };
    long n[EPZ_COUNT];
    long minted, assets, emitted, pre_program, sum = 0;
    int c, i;

    memset(n, 0, sizeof n);
    for (i = 0; i < g_eps_n; i++) {
        if (g_eps[i].is_asset) continue;
        DCHECKF(g_eps[i].addr_class >= 0 && g_eps[i].addr_class < EPA_COUNT,
                "an @H record reached the razor census carrying the address class %d, which is none of "
                "endpoint.h's ENDPOINT_ADDRESS_CLASSES — `endpoint_razor_class_of` reads that field as its "
                "FIRST arm and anything outside the list falls through it into `unproven`, so a corrupt value "
                "here is not a wrong count but an UNDER-CLAIM published as a floor, which is the one "
                "direction this row may not be wrong in", g_eps[i].addr_class);
        n[endpoint_razor_class_of(g_eps[i].door, g_eps[i].addr_class)]++;
    }
    endpoint_surface_census(&minted, &assets, &emitted, &pre_program);

    /* EVERY CLASS IS EMITTED INCLUDING THE ZEROES, for the reason the three walks above give and with the
       sharpest form of it at the class that matters: a `runtime-only` that is ABSENT and a `runtime-only`
       that read 0 are the two facts this row exists to keep apart. The first is an instrument that stopped
       writing the class; the second is the product's own bar answering, and CLAUDE.md
       §What-the-tool-produces calls that 0 a REFUSAL TO CLAIM the capability on this document rather than a
       smaller version of it. */
    json_buf_raw(&b, "{");
    for (c = 0; c < EPZ_COUNT; c++) {
        if (c) json_buf_raw(&b, ",");
        json_buf_str(&b, endpoint_razor_class_token(c));
        json_buf_raw(&b, ":");
        edge_num(&b, n[c]);
        sum += n[c];
    }
    json_buf_raw(&b, "}");
    DCHECKF(sum == emitted,
            "the @H surface's per-razor-class counts sum to %ld against the %ld rows it emits — the two "
            "classes are a PARTITION of the emitted surface and this walk carries the same `is_asset` skip "
            "the three censuses beside it do, so a difference is one of those walks having stopped describing "
            "the population the others count, and the BAR a reader reads off this row would be a share of a "
            "number that is not the surface's size", sum, emitted);
    return json_buf_take(&b);
}

/* THE FIFTH WALK, OVER THE ONE FACT THE BAR'S `unproven` DOES NOT CARRY — see endpoint.h for what each member
   claims and for why this is the NECESSARY CONDITION under that bar rather than a part of it. It is a separate
   walk and not a sum over any table above for `endpoint_reach_hist_json`'s stated reason: each of these walks
   asserts its OWN subscript against its OWN list, and a walk that derived its rows from another's would
   certify what it never examined.
   THE RANGE ASSERT IS NOT THE ONLY ONE HERE AND THAT IS THE DIFFERENCE FROM THE FOUR ABOVE. Those classes are
   unordered sets, so a corrupt value can only be outside the table; this list is ORDERED and the record's
   merge is a MAX over that order, so the thing a later diff can break without going out of range is the
   ORDER ITSELF — and the one property that would then be false is checkable here, where the whole array is in
   one hand: `EPW_UNASKED` is the identity element the `memset` leaves behind, so it must be the lowest member
   or a merge could silently lower a record that had already read something. */
char *endpoint_witness_hist_json(void) {
    JsonBuf b = { 0 };
    long n[EPW_COUNT];
    long minted, assets, emitted, pre_program, sum = 0;
    int c, i;

    DCHECK(EPW_UNASKED == 0,
           "endpoint.h's ENDPOINT_WITNESS_CLASSES no longer begins with the member that proves NOTHING — the "
           "record's merge is a MAX over this list's order and `memset` leaves a fresh record at 0, so a "
           "reordering makes that zero mean a CLAIM the mint never made, and every row minted outside a flow "
           "would publish it");
    memset(n, 0, sizeof n);
    for (i = 0; i < g_eps_n; i++) {
        if (g_eps[i].is_asset) continue;
        DCHECKF(g_eps[i].witness_class >= 0 && g_eps[i].witness_class < EPW_COUNT,
                "an @H record reached the witness census carrying the class %d, which is none of endpoint.h's "
                "ENDPOINT_WITNESS_CLASSES — the mint writes this field unconditionally from "
                "`witness_class_now`, which returns one of exactly three members, so this is a record minted "
                "by something that is not that line and the count about to be raised is at an index outside "
                "the table this census is a partition of", g_eps[i].witness_class);
        n[g_eps[i].witness_class]++;
    }
    endpoint_surface_census(&minted, &assets, &emitted, &pre_program);

    /* EVERY CLASS IS EMITTED INCLUDING THE ZEROES, for the four walks' reason and with its own sharpest form
       here: a `may-rest-on` that is ABSENT and one that read 0 are the two facts this row exists to keep
       apart, and they take OPPOSITE work. Absent is an instrument that stopped writing the field. Zero is a
       run on which no address was composed by a path that had pinned anything — which makes the bar's
       `unproven` beside it the run having genuinely proved nothing, and sends the next diff to the SOLVER
       rather than to the pin. */
    json_buf_raw(&b, "{");
    for (c = 0; c < EPW_COUNT; c++) {
        if (c) json_buf_raw(&b, ",");
        json_buf_str(&b, endpoint_witness_class_token(c));
        json_buf_raw(&b, ":");
        edge_num(&b, n[c]);
        sum += n[c];
    }
    json_buf_raw(&b, "}");
    DCHECKF(sum == emitted,
            "the @H surface's per-witness-class counts sum to %ld against the %ld rows it emits — the three "
            "classes are a PARTITION of the emitted surface and this walk carries the same `is_asset` skip "
            "the four censuses beside it do, so a difference is one of those walks having stopped describing "
            "the population the others count, and the BOUND a reader reads off this row would be a share of a "
            "number that is not the surface's size", sum, emitted);
    return json_buf_take(&b);
}

/* THE STAGE ARMS THE PARTITION IS MADE OF, SUMMED ONCE FOR THE IDENTITY AND WRITTEN ONCE FOR THE DOCUMENT —
   two edges, one walk each, and the arithmetic in one place. */
static long edge_stage_sum(const EndpointEdge *e) {
    long sum = 0;
    int i;

    for (i = 0; i < e->stage_n; i++) sum += e->died[i];
    return sum;
}

/* THE ONE IDENTITY THAT IS A FACT ABOUT EVERY EDGE'S OWN TEARDOWN, ASSERTED WHERE BOTH TERMS ARE IN ONE HAND.
   The other relations are NOT shared, and that is a finding about the two machines rather than a shape this
   helper could not express — each emitter states its own, with the reason it holds there.
   THE PARTITION. The two sides are raised in the two arms of ONE teardown, so they part company exactly when
   a third way out of that function is added without classifying the state it lets go — which is how a state
   that dies in a new way goes missing from a histogram that still sums to something plausible. It survives
   the deep-fork copy that defeats every OTHER relation here, because a copy gets its own teardown and that
   teardown raises exactly one of these two arms like any other. */
static void edge_partition(const EndpointEdge *e, const char *who, long sum) {
    DCHECKF(sum + e->freed_reached == e->freed,
            "the %s edge's torn-down states do not sum over what happened to them (%ld by the stage rows plus "
            "%ld whose construction had completed, %ld freed) — the rows are the two arms of ONE teardown, so "
            "a total that moves without an arm moving is a third arm added above them, and what a reader "
            "loses is the stage a construction died at, which is the whole content of this census",
            who, sum, e->freed_reached, e->freed);
}

/* ONE ARM PER STAGE, EVERY ONE EMITTED INCLUDING THE ZEROES, which is absent.c's rule for its histograms and
   is load-bearing twice over. It is what makes a stage reading 0 the positive statement that no construction
   died there, where a row listing only its non-zero arms is a table whose shape a reader has to know before
   they can tell an arm that never fired from an arm that does not exist — and extension/bridge.js REFUSES an
   empty histogram on this census by name, so a table that listed only its non-zero arms would abort the
   trusted zone for every document whose constructions all survived.
   THE KEY IS COMPUTED AND IS NOT A FIELD NAME — json_buf_key takes a literal and the compiler enforces it,
   which is what makes every FIELD name in this seam auditable; a STAGE LABEL is another component's
   declaration and no producer here could declare it, so it goes through the VALUE entry. That entry also
   ESCAPES, which this key needs and absent.c's row keys do not: these labels quote the standards' own
   `"no-cors"`, `init["body"]` and `"fetching req"`, so a raw write would end the JSON string early and the
   document embedding this census would not parse. */
static void edge_stage_hist(JsonBuf *b, const EndpointEdge *e) {
    int i;

    json_buf_raw(b, "{");
    for (i = 0; i < e->stage_n; i++) {
        if (i) json_buf_raw(b, ",");
        json_buf_str(b, e->steps[i]);
        json_buf_raw(b, ":");
        edge_num(b, e->died[i]);
    }
    json_buf_raw(b, "}");
}

/* The fetch edge's rows, declared in solver/metrics/fetch_edge.def, as a splice for `_cold`. */
char *endpoint_fetch_edge_rows(void) {
    JsonBuf hist = { 0 };
    char *died, *rows, *out;
    long sum = 0;

    if (!g_fetch_edge.steps) {
        /* NO ROWS AT ALL, WHICH IS A DIFFERENT DOCUMENT FROM FIVE ZEROES AND IS THE HONEST ONE. A host that
           installs no fetch runs no fetch machine, so there is no population — and §Testing's rule is that an
           absent count and a zero count are different facts that must never be averaged. Five zeroes here
           would be exactly that average, published on the census whose whole subject is what a zero means.
           The EMPTY STRING and not `{}`: these are ROWS of the census above, spliced between its own, so the
           absent form has to be the absence of the rows and of the comma in front of them. */
        METRIC_ABSENT(fetch_named);
        METRIC_ABSENT(fetch_named_typeof);
        METRIC_ABSENT(fetch_named_prop);
        METRIC_ABSENT(fetch_called);
        METRIC_ABSENT(fetch_began);
        METRIC_ABSENT(fetch_offered);
        METRIC_ABSENT(fetch_freed);
        METRIC_ABSENT(fetch_freed_offered);
        METRIC_ABSENT(fetch_died_at);
        return metrics_family_members_text(METRIC_FAMILY_FETCH_EDGE);
    }
    sum = edge_stage_sum(&g_fetch_edge);
    edge_partition(&g_fetch_edge, "fetch", sum);
    /* The containment that ties this census to the rows it sits beside, assertable only here: every offer is
       followed by an endpoint_record call that raises `g_asks` before any gate, so the fetch edge's offers are
       contained in this surface's asks and the slack is every other door (core/xhr's, the markup inventory's,
       the reply decoder's) — the fetch edge's share of the ask population. The two are raised in two files at
       two events, so they part company when an offer is raised without the record call under it. */
    DCHECKF(g_fetch_edge.reached <= g_asks,
            "the fetch edge offered this surface more addresses (%ld) than the surface counted asks at its "
            "door (%ld) — the offer is raised on the line before endpoint_record and that door raises the ask "
            "total before its own gate, so a larger offer count is an offer raised with no record call under "
            "it, and a reader taking the rest of the asks as the OTHER doors' share would read a negative "
            "count as a large one",
            g_fetch_edge.reached, g_asks);
    /* The one relation `began` has to the call row, and it is a presence relation. Every world that began
       descends from a world that entered core/idl_args.c's stage 0, but a fork in the conversions gives each
       sibling its own begin against that one call (see the struct), so `began <= called` is not an identity
       and `called - began` is not the count of calls that died in the conversions. */
    DCHECKF(g_fetch_edge.began == 0 || g_fetch_edge.called > 0,
            "the fetch edge counted constructions that began (%ld) with no call ever entering the hosting "
            "machine's prologue (%ld) — every begin descends from a stage-0 entry, so this is the member body "
            "reached by something other than core/idl_args.c's machine",
            g_fetch_edge.began, g_fetch_edge.called);
    edge_stage_hist(&hist, &g_fetch_edge);
    died = json_buf_take(&hist);
    METRIC_SET(fetch_named, g_fetch_edge.named);
    METRIC_SET(fetch_named_typeof, g_fetch_edge.named_typeof);
    METRIC_SET(fetch_named_prop, g_fetch_edge.named_prop);
    METRIC_SET(fetch_called, g_fetch_edge.called);
    METRIC_SET(fetch_began, g_fetch_edge.began);
    METRIC_SET(fetch_offered, g_fetch_edge.reached);
    METRIC_SET(fetch_freed, g_fetch_edge.freed);
    METRIC_SET(fetch_freed_offered, g_fetch_edge.freed_reached);
    METRIC_SET_OBJECT(fetch_died_at, died);
    rows = metrics_family_members_text(METRIC_FAMILY_FETCH_EDGE);
    free(died);
    out = composef(",%s", rows);
    free(rows);
    return out;
}

/* The other edge's rows, declared in solver/metrics/xhr_edge.def; endpoint.h states what each is, which
   identities close over them, that the stage arms are a partition and not a ladder, and why there is no
   relation between the placement row and the offer row. */
char *endpoint_xhr_edge_rows(void) {
    JsonBuf hist = { 0 };
    char *died, *rows, *out;
    long sum = 0;

    /* THE CROSS-CHECK THAT HAS TO COME BEFORE THE ABSENT ARM, because the absent arm returns. An offer at this
       edge's door with its send machine never declared is core/xhr having recorded an endpoint in a session
       whose own xhr_init did not run — which would publish this edge's share of `epAsks` with no census under
       it, and would do it as an ABSENCE, which §Testing's rule says is a different fact from a zero and is the
       one a reader stops at. */
    DCHECKF(g_xhr_offered == 0 || g_xhr_edge.steps != NULL,
            "the XHR edge offered this surface %ld addresses with no send machine ever declared to it — the "
            "declaration is xhr_init's and every member of §3 runs after it, so this is a session whose "
            "endpoint records came from a component this census was never told about, and the rows are about "
            "to be ABSENT from a document that nevertheless counted this edge's asks",
            g_xhr_offered);
    if (!g_xhr_edge.steps) {
        /* NO ROWS AT ALL, WHICH IS A DIFFERENT DOCUMENT FROM SIX ZEROES AND IS THE HONEST ONE — the same
           sentence as the fetch edge's absent arm above, and for the same reason: a host that installs no
           XMLHttpRequest runs no send machine and there is no population. The EMPTY STRING and not `{}`,
           because these are ROWS of the census above and the absent form has to be the absence of the rows
           and of the comma in front of them. */
        METRIC_ABSENT(xhr_named);
        METRIC_ABSENT(xhr_named_typeof);
        METRIC_ABSENT(xhr_named_prop);
        METRIC_ABSENT(xhr_called);
        METRIC_ABSENT(xhr_began);
        METRIC_ABSENT(xhr_placed);
        METRIC_ABSENT(xhr_offered);
        METRIC_ABSENT(xhr_freed);
        METRIC_ABSENT(xhr_freed_placed);
        METRIC_ABSENT(xhr_died_at);
        return metrics_family_members_text(METRIC_FAMILY_XHR_EDGE);
    }
    sum = edge_stage_sum(&g_xhr_edge);
    edge_partition(&g_xhr_edge, "xhr send", sum);
    /* AND THERE IS NO `freed_placed <= placed` HERE, WHICH IS A FACT ABOUT THIS MACHINE AND NOT AN OMISSION.
       A step state is BYTE-COPIED at a deep fork and the copy inherits `placed`, so two copies file against
       one placement — and unlike core/fetch, `XHR_SEND_DECL` declares NO `unforkable`, so nothing refuses
       that fork. It is not a corner either: §3.5.6's SYNCHRONOUS arm sets `placed` and then PARKS inside its
       own call to the lifecycle machine, which goes on to fire `readystatechange` and `progress` at the
       page's own listeners — page code, at a depth where the send frame is live and forkable. An assert here
       would therefore fire on a legitimate state, which is the concession shape §Offensive-programming
       refuses, and the row pair is read as two facts rather than subtracted. */
    /* THE CONTAINMENT THIS EDGE DOES HAVE IS OVER THE OFFER ROW AND NOT OVER THE PLACEMENT — the placement is
       one machine short of the door and endpoint.h states why nothing between them may be asserted. Every
       offer is raised on the line before an endpoint_record call that raises `g_asks` before its own gate, so
       this edge's offers are contained in the surface's asks and the slack is every OTHER door. */
    DCHECKF(g_xhr_offered <= g_asks,
            "the XHR edge offered this surface more addresses (%ld) than the surface counted asks at its door "
            "(%ld) — the offer is raised on the line before endpoint_record and that door raises the ask "
            "total before its own gate, so a larger offer count is an offer raised with no record call under "
            "it, and a reader taking the rest of the asks as the OTHER doors' share would read a negative "
            "count as a large one",
            g_xhr_offered, g_asks);
    edge_stage_hist(&hist, &g_xhr_edge);
    died = json_buf_take(&hist);
    METRIC_SET(xhr_named, g_xhr_edge.named);
    METRIC_SET(xhr_named_typeof, g_xhr_edge.named_typeof);
    METRIC_SET(xhr_named_prop, g_xhr_edge.named_prop);
    METRIC_SET(xhr_called, g_xhr_edge.called);
    METRIC_SET(xhr_began, g_xhr_edge.began);
    METRIC_SET(xhr_placed, g_xhr_edge.reached);
    METRIC_SET(xhr_offered, g_xhr_offered);
    METRIC_SET(xhr_freed, g_xhr_edge.freed);
    METRIC_SET(xhr_freed_placed, g_xhr_edge.freed_reached);
    METRIC_SET_OBJECT(xhr_died_at, died);
    rows = metrics_family_members_text(METRIC_FAMILY_XHR_EDGE);
    free(died);
    out = composef(",%s", rows);
    free(rows);
    return out;
}

/* Serialize the @H surface DIRECTLY to a JSON string in C (caller frees) — no JS-object round-trip. The
   writer is core/json_buf.h's: this file and solve.c each carried a private copy of it, which is one copy too
   many of a thing that has exactly one correct behaviour. */
char *endpoint_json_array(void) {
    JsonBuf b = { 0 };
    int wrote_one = 0;
    json_buf_raw(&b, "[");
    for (int i = 0; i < g_eps_n; i++) {
        Endpoint *e = &g_eps[i];
        /* A RESOURCE THAT TURNED OUT TO BE A STATIC ASSET IS NOT AN ENDPOINT — §Attacker sources says so in
           those words, and this is the only place that can act on it, because the verdict arrives after the
           record. The separator is a `wrote_one` latch and not the loop index for exactly this reason: an
           index-keyed comma emits a leading one the moment record 0 is the skipped kind, and that is invalid
           JSON on a document whose whole delivery is one parse. */
        if (e->is_asset) continue;
        if (wrote_one) json_buf_raw(&b, ",");
        wrote_one = 1;
        json_buf_raw(&b, "{"); json_buf_key(&b, "method"); json_buf_str(&b, e->method);
        json_buf_raw(&b, ","); json_buf_key(&b, "url"); json_buf_str(&b, e->path);
        /* AND THE BODY THIS ENGINE COULD NOT NAME, as bytes plus the type that decides how to read them.
           BOTH OR NEITHER: bytes with no type are bytes nothing can name the fields of, and a type with no
           bytes is not a body — the same sentence `EndpointBody` is one struct for. A consumer reads their
           presence as the POSITIVE statement "this request sends a body this engine has no reader for",
           never as a hole to default past. */
        if (e->body_b64 || e->body_shape || e->body_example_b64) {
            DCHECK((!!e->body_b64 + !!e->body_shape + !!e->body_example_b64) == 1,
                   "one request body was recorded under more than one of this surface's three claims — bytes "
                   "the request sends, the display spelling of an unknown, and an EXAMPLE of a payload whose "
                   "unknown ranges `params` names. body_store sets exactly one, so a record holding two is "
                   "two sightings merged across a grade the identity is supposed to keep apart");
            if (e->body_mime) { json_buf_raw(&b, ","); json_buf_key(&b, "bodyMime"); json_buf_str(&b, e->body_mime); }
            /* THREE KEYS, SO THE CONSUMER NEVER GUESSES. `bodyBase64` is the replay claim, `bodyShape` is a
               statement about WHICH SOURCE composes the body, and `bodyExampleBase64` is what the payload
               LOOKS LIKE at the byte ranges `params` names; a reader that met one field would have to decide
               which of those it was holding, and deciding wrongly is how a hole gets sent.
               THE THIRD NAME CARRIES ITS REFUSAL IN ITS OWN SPELLING and that is deliberate rather than
               decorative: `bodyExampleBase64` cannot be mistaken for `bodyBase64` by a consumer skimming for
               a payload to replay, where a `bodyBase64` plus a sibling flag could be, and the flag would be
               the field a reader defaults past. It is emitted BESIDE the span params rather than instead of
               them — they are the addresses and it is the content, so a splice needs both. */
            if (e->body_b64)   { json_buf_raw(&b, ","); json_buf_key(&b, "bodyBase64"); json_buf_str(&b, e->body_b64); }
            if (e->body_shape) { json_buf_raw(&b, ","); json_buf_key(&b, "bodyShape");  json_buf_str(&b, e->body_shape); }
            if (e->body_example_b64) {
                json_buf_raw(&b, ","); json_buf_key(&b, "bodyExampleBase64"); json_buf_str(&b, e->body_example_b64);
            }
        }
        /* …AND WHAT THIS RECORD IS EVIDENCE OF, ALWAYS, in the same three words the pending line spells and
           through the same mapping (solver/engine.h's `engine_provenance_token`), so the zone that reads both
           about one app cannot be shown two vocabularies. There is NO absence-is-the-statement here, which is
           the opposite of `excludes` and `bounds` one field down and is the difference between a fact that
           may legitimately have gone unobserved and one that is exhaustive: the three words cover every way
           this engine can come to know an address, so a silent grade is not "unconstrained", it is a record
           whose strongest reading — `observed` — a consumer would take by default. That is the fabrication
           CLAUDE.md §@H forbids, performed by omission. */
        json_buf_raw(&b, ","); json_buf_key(&b, "provenance");
        json_buf_str(&b, engine_provenance_token(e->prov));
        /* …AND WHICH MECHANISM COMPOSED THE ADDRESS, AND WHETHER THE PAGE'S CODE HAD RUN WHEN IT DID —
           ALWAYS, both of them, for `provenance`'s reason above and with its exhaustiveness. These are the
           two facts CLAUDE.md §What-the-tool-produces' razor is a SUBTRACTION OF TWO TOTALS over: `epEmitted
           - epPreProgram` says how many addresses forced execution CAN HAVE contributed and names none of
           them, so a reader cannot tell a run that learned ten gated API calls from one that learned ten
           `<link rel=preload>` elements of one `<head>`. Carried per row, the surface partitions directly and
           the subtraction has nothing left to say — which is that record's own retirement clause.
           NEITHER IS AN OPTIONAL KEY AND NEITHER HAS AN ABSENCE-IS-THE-STATEMENT READING, which is the
           opposite of `excludes` and `bounds` below and the same split `provenance` makes: `door` is
           exhaustive over the ways an address can reach this surface because endpoint.h derives the list from
           this function's own call sites, and `mintedAt`'s two words are exhaustive over a boolean fact. A
           silent one would be read as whichever value a consumer's `||` names, and for the door that is a
           mechanism nothing ran.
           `mintedAt` IS TWO WORDS AND NOT A BOOLEAN, deliberately and for `valueClass`'s reason exactly: this
           record is read by a zone deployed on WRITE while this engine is live only after a build, so a
           consumer WILL meet rows from an artifact that predates this key — and `false` and absent are one
           value under every truthiness test a reader reaches for, while neither word is.
           THEY ARE TWO FACTS AND NOT TWO SPELLINGS OF ONE. The door is WHICH MECHANISM and the mint state is
           WHEN, and the pair is what makes either readable: `link-element` alone cannot say whether the
           `<link>` was in the markup or one the router created, and `pre-program` alone cannot say what
           composed the address. A `<head>` whose first `<script src>` runs before the parser reaches the
           `<link>` below it mints that link POST-program, so the timing fact is a proxy for the markup door
           and not the door — in the flattering direction, on the commonest document shape there is. */
        /* THE RESIDUAL THAT STOOD HERE IS RETIRED, AND ITS WHAT-THE-NEXT-DIFF-BUILDS CLAUSE WAS WRONG —
           RECORDED RATHER THAN DELETED, BECAUSE THE WRONG SITE IS THE ONE A READER RE-DERIVES. It said no
           consumer in the trusted zone read either key, which was true, and named extension/lib/merge.js's
           call-site loop as the diff that would fix it, beside the `provenance` check that loop already
           makes. THE SECOND HALF DOES NOT HOLD AND THE REASON IS ABOUT POPULATIONS: that loop `continue`s
           past a structural @T candidate (`url == null`) and past every `data:`/`blob:`/`about:`/
           `javascript:` address before it registers anything, and it folds several rows of THIS array onto
           one `method+host+path` key — so a partition built there is over a smaller, deduped set than the
           emitted figure a reader holds beside it, and would sum to nothing checkable. A per-ADDRESS record
           would also have needed a fold rule for two doors reaching one address, which the enum above
           deliberately declines to invent.
           WHERE IT LANDED INSTEAD is extension/bridge.js's run-record composition, which holds THIS ARRAY at
           the instant it writes the `endpoints` figure that is its length — so the partition and the number
           it partitions are one population at one moment, and each histogram is asserted there to sum to it.
           WHAT THAT READER DOES NOT DO, AND IT IS THE HALF THE DEFERRAL WAS ABOUT (CLAUDE.md
           §A-CROSS-BOUNDARY-DIFF): it does not ASSERT either key's presence. That zone's JavaScript is
           INTERPRETED FROM THE TREE and live on WRITE, while these bytes are live only after somebody builds,
           so a reader demanding a key the SHIPPED wasm does not write would abort the trusted zone on every
           endpoint of every document until an install caught up. It buckets an absent key as its own stated
           third state instead, and the retiring observation and the actor who may make it are recorded at
           that reader rather than here — the tolerance is the JS half's to drop, and this file's own
           contract is unchanged: both keys are written on every row, unconditionally, in every build. */
        json_buf_raw(&b, ","); json_buf_key(&b, "door");
        json_buf_str(&b, endpoint_door_token(e->door));
        json_buf_raw(&b, ","); json_buf_key(&b, "mintedAt");
        json_buf_str(&b, e->pre_program ? "pre-program" : "post-program");
        /* …AND THE THIRD FACT ABOUT THE SIGHTING, WHICH IS NEITHER OF THOSE TWO AND WHICH NEITHER IMPLIES.
           The door is WHICH MECHANISM, the mint state is WHEN, and this is WHETHER THE RUN HAD DETERMINED THE
           ADDRESS *AND WHOSE UNKNOWN IT RESTS ON* — CLAUDE.md §What-the-tool-produces' HARD BAR at the grain
           the bar is stated at, which no count of doors can reach: a literal chunk URL through
           `module-import` and `/api/{location.hash}` through `fetch` are both `beyond` a markup parse and only
           the second clears this bar. THE SECOND FACT IS IN THIS ONE WORD AND NOT IN A COLUMN OF ITS OWN, so
           a consumer reading `unknown-unproven` beside `unproven` can still check the razor's answer against
           what the wire carries — which is `endpoint_razor_class_of`'s own contract and is why the whose fact
           rides this member rather than arriving as a third operand there. Written
           UNCONDITIONALLY on every row in every build, exactly as the two above are, so the `(unstated)`
           bucket a consumer keeps for a row from an artifact older than the key stays a fact about the BUILD
           and never about an address (see endpoint.h for that contract and for who may retire the
           tolerance). */
        json_buf_raw(&b, ","); json_buf_key(&b, "addressClass");
        json_buf_str(&b, endpoint_address_class_token(e->addr_class));
        /* …AND THE UNION OF TWO OF THE THREE, WHICH IS CLAUDE.md §What-the-tool-produces' HARD BAR AS ONE WORD
           AND IS THE FIELD THAT BAR'S OWN RETIREMENT CONDITION ASKS FOR: "the emitter that states a razor
           figure composes that union ITSELF, so a door count cannot be assembled into one at all". Before it,
           the floor was TWO facts a consumer had to union by hand, the second operand was named in prose at
           three sites with three different sets, and the one a reader would have reached for —
           `endpoint_door_reach(door) == EPR_BEYOND`, which the residual at `endpoint_address_hist_json`
           proposed — is the figure CLAUDE.md demoted rather than the one it chose, because `fetch`, `xhr`,
           `beacon` and `module-import` are all `beyond` a MARKUP parse and "a JS AST parse reads them off the
           text without running anything".
           IT IS DERIVED AND NOT STORED, AND THAT IS WHAT KEEPS IT HONEST UNDER THE TWO MERGE RULES ITS
           OPERANDS HAVE. `addr_class` is unioned on a merge and `door` is the first sighting's; both
           directions are sound as a floor (see endpoint.h), and computing the verdict HERE means there is no
           third merge rule to get wrong and a consumer can check this word against the two beside it.
           WRITTEN UNCONDITIONALLY ON EVERY ROW IN EVERY BUILD, exactly as the three above are, so the
           `(unstated)` bucket a consumer keeps for a row from an artifact older than the key stays a fact
           about the BUILD and never about an address. */
        json_buf_raw(&b, ","); json_buf_key(&b, "razorClass");
        json_buf_str(&b, endpoint_razor_class_token(endpoint_razor_class_of(e->door, e->addr_class)));
        /* …AND THE ONE FACT THAT BAR DOES NOT CARRY, BESIDE IT RATHER THAN INSIDE IT. endpoint.h's
           ENDPOINT_WITNESS_CLASSES states why: `razorClass` is a FLOOR and this is a MAY, so a consumer that
           wants the bound reads this column and one that wants the bar reads that one. They are adjacent here
           precisely so the pair is legible per row — `unproven` beside `may-rest-on` is the population the
           razor's zero does not account for, and `unproven` beside `no-witness` is the run having genuinely
           proved nothing about this address.
           WRITTEN UNCONDITIONALLY ON EVERY ROW IN EVERY BUILD, for the four columns above it. */
        json_buf_raw(&b, ","); json_buf_key(&b, "witnessClass");
        json_buf_str(&b, endpoint_witness_class_token(e->witness_class));
        /* …AND THE OPERAND THE ADDRESS CLASS WAS DERIVED FROM, WHICH IS THE ONE FACT THE FOUR COLUMNS ABOVE
           ARE COMPOSED OUT OF AND NONE OF THEM PUBLISHES. `addressClass` is read off the address VALUE's own
           concolic provenance and nothing else, so a reader who meets `unknown` beside `runtime-only` can see
           that the run held an unknown and cannot see WHICH SOURCE's bytes it was — and `address_class_of`'s
           residuals are entirely about the population where those two answers part, and both are now MET: a
           hole `engine_orphan_call` minted so that a drive could happen at all USED TO answer `unknown` here
           and clear the bar with nothing learned, because the hole is OURS, and it now answers
           `unknown-unproven` and the bar reads `unproven` for it. This column is still the OBSERVATION those
           residuals handed over — it is what a reader checks the narrowing against, per row, since
           `addressClass` says the run held an unknown somebody outside this engine supplied and only this
           says WHICH component carried the bytes in. An `unknown-unproven` row whose root names a component
           that carries a DOCUMENT'S or a SERVER'S bytes is the observation that reopens the residual below.
           IT READS THE ROOT AND NOT THE ADDRESS'S DISPLAY SHAPE, WHICH IS THE WHOLE OF WHY IT IS A NEW
           COLUMN. `url` already carries a composed IDENTITY — solver/concolic.h spends its `root` banner on
           the report that identity produced when it was asked a delivery question (`{location.hash}.slice()`
           matched no declared source, and the envelope said "there is no navigation that reproduces it") —
           and the residual's own last paragraph says the same thing one grain out: "Whose unknown it is, is a
           fact about the ROOT; where the literals fall is a fact about the page's syntax." A reader counting
           hole-bearing `url` spellings is reading the axis that file refutes.
           THREE JSON TYPES AND EACH IS A POSITIVE STATEMENT, because `concolic_root_c`'s NULL is two facts and
           a reader may not have them folded. `null` — the record's class says no sighting held a concolic
           address at all, so there was no provenance to ask. `false` — a sighting did, and its bytes entered
           through nothing this engine minted as a source (concolic.h: "a positive statement, never a hole to
           fill"). A STRING — the sources it entered through, as that file spells them, which may name a SET.
           THE TYPES ARE THE DISCRIMINATOR AND A TOKEN WOULD NOT BE: a root may be composed out of a name the
           PAGE chose (solver/absent.c mints an injected global's source from the property the page read), so
           any in-band word this file reserved would be a word a document could spell, and the collision would
           report an attributed address as one this engine attributed to nothing.
           IT COMPOSES NO VERDICT, HERE OR IN ANY CONSUMER, and that is a decision rather than a gap: see the
           struct for why a classification keyed on this string's spelling is the one §RUN-DON'T-MATCH forbids,
           and `address_class_of`'s residual for what has to be built before the bar may read it.
           WRITTEN UNCONDITIONALLY ON EVERY ROW IN EVERY BUILD, for the four columns above it, so the
           `(unstated)` bucket a consumer keeps stays a fact about the BUILD and never about an address.
           NAMED RESIDUAL — THIS COLUMN IS A FLOOR ON THE SOURCES AN ADDRESS'S BYTES ENTERED THROUGH AND NEVER
           THE SET, AND THE CLAUSE IS ABOUT THIS TREE'S MERGE RULE RATHER THAN ABOUT ANY STANDARD, so it is as
           unreliable as the remedy clause below it and is checked by reading the merge rather than trusted.
           WHAT IS NOT COVERED: the root is the root of the sighting whose CLASS this record carries, which is
           what keeps the pair one sighting's — so where TWO sightings of one address held the SAME class and
           DIFFERENT roots, the second root is not recorded anywhere. The record then names one component where
           the bytes of some observed path entered through another, which is the `root` banner's own case for a
           SET one grain out: "bytes that arrived through two components entered through two".
           WHAT THE NEXT DIFF BUILDS: the union, spelled in solver/concolic.c and never here, because
           `CONCOLIC_JOINT_SEP` is declared in that file and `derived_root_join` is its one speller — a union
           composed at this site would hold the second copy of a separator that file states once, which is the
           drifting restatement §AN-AUDITOR-DERIVES-THE-RULE forbids and which would drift in the silent
           direction (a root that stopped splitting reads as a single-source address). MEASURED ABSENT with the
           command, so this clause does not name a mechanism the tree already has: over solver/concolic.h,
           `concolic_root_member`, `concolic_root_walk`, `concolic_root_union`, `concolic_root_join`,
           `concolic_root_any` and `concolic_root_all` each answer NOTHING, against `concolic_root_c` answering
           as the armed control.
           HOW ITS ABSENCE WOULD SHOW: this column MOVING between two runs of ONE artifact over ONE document,
           read per row out of the emitted records — which sighting raises a record's class first is decided by
           the interleaving, so a floor is free to name a different source each time while a SET could not.
           Observed where the records are, beside the `endpointAddressRoot` partition the trusted zone composes
           from them; a column that is stable across such a pair is evidence about that document's sightings and
           not about this rule, which is why the pair is the observation and one run is not. */
        json_buf_raw(&b, ","); json_buf_key(&b, "addressRoot");
        if (e->addr_class == EPA_CONCRETE)   json_buf_raw(&b, "null");
        else if (!e->addr_root)              json_buf_raw(&b, "false");
        else                                 json_buf_str(&b, e->addr_root);
        json_buf_raw(&b, ","); json_buf_key(&b, "params"); json_buf_raw(&b, "[");
        for (int j = 0; j < e->np; j++) {
            if (j) json_buf_raw(&b, ",");
            json_buf_raw(&b, "{"); json_buf_key(&b, "name"); json_buf_str(&b, e->params[j].name);
            /* `CHECK` AND NOT `DCHECK`, BECAUSE THE SUBSCRIPT IS ON THE NEXT LINE AND IS IN EVERY BUILD. The
               message below already named what it was preventing — a field the consumer cannot classify — and
               a dev-only guard prevents it in the one build where nobody is reading the report. `ep_loc_name`
               is a THREE-element table of `const char *`, so an out-of-range read hands `json_buf_str` a
               pointer assembled out of whatever the link placed after it, and check.h's header says the wasm
               build does not fault on that: it reads bytes from that address and emits them as this param's
               `location`. That is not a missing field a consumer can see is missing — it is a plausible
               location token in the @H record, which §@H forbids by name, and it is the same shape one
               indirection further out as `provenance` defaulting to `observed`. */
            CHECK(e->params[j].loc == EP_QUERY || e->params[j].loc == EP_PATH || e->params[j].loc == EP_BODY,
                  "engine: an endpoint param carries a location this surface has no name for — the enum and "
                  "its name table are read together at exactly this line, so one grown without the other "
                  "would index outside a three-entry table and emit whatever that address holds as the "
                  "param's location");
            json_buf_raw(&b, ","); json_buf_key(&b, "location"); json_buf_str(&b, ep_loc_name[e->params[j].loc]);
            /* …AND WHETHER THIS PARAM'S VALUE EVER NAMED A HOLE, ALWAYS, for `provenance`'s reason one level
               in. There is NO absence-is-the-statement here — it is the opposite of `excludes` and `bounds`
               below — because the two words are EXHAUSTIVE over the ways a value reaches this surface, and a
               fact that cannot have gone unobserved must not be spelled by a silence that means "unobserved"
               everywhere else in this record.
               IT IS WHAT MAKES THOSE SILENCES READABLE, which is the whole of why it is here. `excludes`
               absent says "no equality gate over this hole took its false arm on every path that built this
               request" — a sentence that presupposes a hole. Where there was none, all four domain reads were
               skipped at the mint and the same absence means "this param is a literal and there was never
               anything to look up". Those two take opposite work and rendered with identical bytes, so a
               consumer counting parameters whose domain could have been narrowed was counting a population
               it could not name.
               TWO WORDS AND NOT A BOOLEAN, DELIBERATELY. This record is read by a zone that is deployed on
               WRITE while this engine is live only after a build, so a consumer WILL meet params from a
               build that predates this key — and `false` and absent are one value under every truthiness
               test a reader reaches for, while neither word is. The absence is a third state and has to stay
               one; lib/learn.js reads it as `unstated`, which is extension/lib/safe-fetch.js's own word for
               an act that comes off no line at all.
               THE WORDS ARE `concolic_hole_key`'s OWN, not coined here: its contract says it answers NULL
               where a shape "names no hole at all (a concrete value has no domain to look up and must not
               borrow one)", so `concrete` is the name of exactly the case that gates these reads out and
               `unknown` is §Solver-half's word for the other. */
            json_buf_raw(&b, ","); json_buf_key(&b, "valueClass");
            json_buf_str(&b, e->params[j].has_hole ? "unknown" : "concrete");
            json_buf_raw(&b, ","); json_buf_key(&b, "validValues"); json_buf_raw(&b, "[");
            for (int k = 0; k < e->params[j].nvals; k++) { if (k) json_buf_raw(&b, ","); json_buf_str(&b, e->params[j].vals[k]); }
            json_buf_raw(&b, "]");
            /* THE DOMAIN, AND ONLY WHERE ONE WAS OBSERVED. An empty array would be a third state beside "the
               field is absent" and "the field lists tokens", and a consumer cannot tell an empty constraint
               from an unconstrained param — so the ABSENCE is the statement: no equality gate over this hole
               took its false arm on every path that built this request. §Solver-half's asymmetry is what this
               field exists for: without it a param proved to be neither "admin" nor "prod" rendered with the
               same bytes as one nothing had ever tested, and that silence reads as "anything goes". */
            if (e->params[j].nexcl) {
                json_buf_raw(&b, ","); json_buf_key(&b, "excludes"); json_buf_raw(&b, "[");
                for (int k = 0; k < e->params[j].nexcl; k++) { if (k) json_buf_raw(&b, ","); json_buf_str(&b, e->params[j].excl[k]); }
                json_buf_raw(&b, "]");
            }
            /* …AND THE INTERVAL, IN THE STANDARD'S OWN VOCABULARY FOR IT, and only where a bound survived every
               observed path. JSON Schema Validation 2020-12 §6.2 Validation Keywords for Numeric Instances
               (number and integer) names all four: §6.2.2 "maximum" ("an inclusive upper limit"), §6.2.3
               "exclusiveMaximum" ("strictly less than (not equal to)"), §6.2.4 "minimum" ("an inclusive lower
               limit") and §6.2.5 "exclusiveMinimum" ("strictly greater than (not equal to)"). In 2020-12 each
               is a NUMBER — the draft-04 boolean form, where `exclusiveMinimum:true` modified `minimum`, is a
               different keyword shape and writing it here would be read as a bound of 1.
               THE NUMBER IS THE PAGE'S OWN SPELLING, carried from the literal the predicate compared against
               rather than re-printed from a double, so a report never states a number the bundle does not
               contain. It is written UNQUOTED because these keywords take a number and a quoted one is a
               different type; every spelling that reaches here came from §6.1.6.1.20 Number::toString of a
               FINITE Number (concolic_rel_hook refuses the rest), which is a JSON number by construction.
               IT STAYS A SHAPE. §@H forbids inventing `6` for `x > 5`, and nothing here picks a member of the
               interval — `validValues` is still only what the code COMPUTED. */
            if (e->params[j].bnd.has_lo || e->params[j].bnd.has_hi) {
                int wrote = 0;
                json_buf_raw(&b, ","); json_buf_key(&b, "bounds"); json_buf_raw(&b, "{");
                if (e->params[j].bnd.has_lo) {
                    if (e->params[j].bnd.lo_incl) json_buf_key(&b, "minimum"); else json_buf_key(&b, "exclusiveMinimum");
                    json_buf_raw(&b, e->params[j].bnd.lo_txt);
                    wrote = 1;
                }
                if (e->params[j].bnd.has_hi) {
                    if (wrote) json_buf_raw(&b, ",");
                    if (e->params[j].bnd.hi_incl) json_buf_key(&b, "maximum"); else json_buf_key(&b, "exclusiveMaximum");
                    json_buf_raw(&b, e->params[j].bnd.hi_txt);
                }
                json_buf_raw(&b, "}");
            }
            /* …AND THE CALL PREDICATES, IN THE ENGINE'S OWN VOCABULARY AND DELIBERATELY NOT JSON SCHEMA'S.
               JSON Schema Validation 2020-12 §6.3 Validation Keywords for Strings has one keyword that could
               carry a prefix test — §6.3.3 "pattern", whose text is "The value of this keyword MUST be a
               string. This string SHOULD be a valid regular expression, according to the ECMA-262 regular
               expression dialect. A string instance is considered valid if the regular expression matches the
               instance successfully. Recall: regular expressions are not implicitly anchored." — and it is
               the wrong shape here for three reasons that all point the same way. It cannot state the FALSE
               arm at all, and forced multi-path produces that arm at exactly the rate it produces the true
               one, so half of every observation would be dropped — which is §@H's wrong-report-not-a-partial-
               one, arriving through a vocabulary choice. Writing `^/api` out of `startsWith("/api")` means
               DECIDING WHAT THE METHOD MEANS, which is the recogniser §RUN-DON'T-MATCH forbids and which
               would have to enumerate the string builtins. And the translation itself is a place to be
               silently wrong: the quoted sentence says a pattern is not anchored and the page's literal may
               hold regex metacharacters, so the emission would owe an escaping rule nobody checks.
               So what is emitted is what was OBSERVED: the method the page named, the arguments it passed,
               and the answer the run got. `holds` is the arm, not a modifier — `false` is a fact this flow
               PROVED, and it is exactly the arm the shipped bundle did not take.
               THE ABSENCE IS THE STATEMENT, as it is for `excludes` and `bounds`: no key at all where no call
               predicate survived every observed path to this request, never an empty array, which a consumer
               could not tell apart from a param nothing tested.
               IT STAYS A SHAPE. Nothing here picks a string that would satisfy the predicate; §@H forbids
               inventing `/api/x` for `startsWith("/api")` exactly as it forbids inventing `6` for `x > 5`. */
            if (e->params[j].npred) {
                json_buf_raw(&b, ","); json_buf_key(&b, "predicates"); json_buf_raw(&b, "[");
                for (int k = 0; k < e->params[j].npred; k++) {
                    const ConcolicPred *pr = &e->params[j].pred[k];
                    if (k) json_buf_raw(&b, ",");
                    json_buf_raw(&b, "{"); json_buf_key(&b, "method"); json_buf_str(&b, pr->method);
                    json_buf_raw(&b, ","); json_buf_key(&b, "arguments"); json_buf_raw(&b, "[");
                    for (int a = 0; a < pr->nargs; a++) {
                        if (a) json_buf_raw(&b, ",");
                        json_buf_str(&b, pr->args[a]);
                    }
                    json_buf_raw(&b, "]");
                    DCHECK(pr->holds == 0 || pr->holds == 1,
                           "a call predicate reached the emission for an arm that is neither taken nor "
                           "not-taken — the arm IS the fact this record carries, so a third value would be "
                           "written as one of the two and the consumer could not tell which");
                    json_buf_raw(&b, ","); json_buf_key(&b, "holds");
                    json_buf_raw(&b, pr->holds ? "true" : "false");
                    json_buf_raw(&b, "}");
                }
                json_buf_raw(&b, "]");
            }
            /* …AND THE LOOSE EQUALITIES THAT HELD, which is the FOURTH way a gate narrows a domain and the one
               that used to reach the report as silence. §7.2.14 IsStrictlyEqual ( x, y ) step 1 is "If
               SameType(x, y) is false, return false", so a `===` that held DETERMINED the value and it appears
               in `validValues`; §7.2.13 IsLooselyEqual ( x, y ) coerces, so its holding arm determines nothing
               and `concolic_pin` refuses it — correctly, and until this key existed that refusal was the whole
               of the record. The param then rendered exactly like one nothing had ever tested, while its
               sibling flow's `excludes` carried the same gate's other arm.
               EACH ENTRY IS `{"value":<string>,"type":<string>}` AND BOTH HALVES ARE LOAD-BEARING. The value is
               the operand's own §7.1.19 ToString ( arg ), which flattens `undefined`, `null`, `0` and `false`
               onto text that is also a legal String operand — so `type` is what separates `== undefined`
               (which §7.2.13 steps 2 and 3 make a demand for null-or-undefined, and for a query parameter a
               demand that it be ABSENT) from `== "undefined"` (which demands nine characters). The word is
               concolic.c's own report name for the kind and never a re-spelling of it here.
               IT STATES THE PREDICATE AND NOT THE SET. §7.2.13's holding set depends on the token's kind and
               its Object arm runs the PAGE's own ToPrimitive, so a consumer that rendered the set would be
               re-implementing fourteen spec steps beside the engine that runs them — §RUN-DON'T-MATCH, in a
               report. What is carried is the transcript: the page wrote `== 0` and this run got true.
               IT INVENTS NOTHING. No member of the holding set is emitted; `validValues` still carries only
               what the code COMPUTED, exactly as it does beside `bounds`.
               THE ABSENCE IS THE STATEMENT, as it is for `excludes`, `bounds` and `predicates`: no key at all
               where no loose equality held on every observed path to this request, never an empty array. */
            if (e->params[j].nleq) {
                json_buf_raw(&b, ","); json_buf_key(&b, "looselyEquals"); json_buf_raw(&b, "[");
                for (int k = 0; k < e->params[j].nleq; k++) {
                    const ConcolicLooseEq *lq = &e->params[j].leq[k];
                    if (k) json_buf_raw(&b, ",");
                    json_buf_raw(&b, "{"); json_buf_key(&b, "value"); json_buf_str(&b, lq->tok);
                    json_buf_raw(&b, ","); json_buf_key(&b, "type");
                    json_buf_str(&b, concolic_lit_report_name((ConcolicLit)lq->kind));
                    json_buf_raw(&b, "}");
                }
                json_buf_raw(&b, "]");
            }
            json_buf_raw(&b, "}");
        }
        json_buf_raw(&b, "]");
        /* The transport half, and ONLY when there is one — an endpoint with no learned header must not claim an
           empty requirement, which reads as "needs nothing" rather than "nothing was observed". A record keyed
           by header name, which is the shape the popup's Required Headers section already reads. */
        if (e->nh) {
            json_buf_raw(&b, ","); json_buf_key(&b, "headers"); json_buf_raw(&b, "{");
            for (int j = 0; j < e->nh; j++) {
                /* THE ONE COMPUTED KEY IN THIS SEAM, AND IT IS NOT A FIELD NAME. json_buf_key takes a literal
                   and the compiler enforces it, which is what makes every FIELD name here auditable; this key
                   is a HEADER NAME, so it is DATA and no producer could ever declare it. Writing it through
                   the VALUE entry is the distinction: a reader who greps json_buf_key gets the contract, and
                   a key that is not one is visibly a different construct rather than an exception to a rule. */
                if (j) json_buf_raw(&b, ",");
                json_buf_str(&b, e->hdrs[j].name);
                json_buf_raw(&b, ":");
                json_buf_str(&b, e->hdrs[j].value);
            }
            json_buf_raw(&b, "}");
        }
        json_buf_raw(&b, "}");
    }
    json_buf_raw(&b, "]");
    return json_buf_take(&b);
}

void endpoint_free(void) {
    for (int i = 0; i < g_eps_n; i++) {
        free(g_eps[i].method); free(g_eps[i].path);
        /* THE BODY FIELDS ADDED TO `Endpoint` FREED WHERE EVERY OTHER OWNED FIELD IS — §Architecture's rule
           that a struct copied or freed field-by-field creates an obligation at every such site, which is
           what a field added to one and not the other silently breaks. It said TWO for as long as there were
           two; the count is gone rather than raised, because the obligation is the rule and the number is
           what goes stale the next time this struct grows. */
        free(g_eps[i].body_mime); free(g_eps[i].body_b64); free(g_eps[i].body_shape);
        free(g_eps[i].body_example_b64);
        free(g_eps[i].addr_root);
        for (int j = 0; j < g_eps[i].np; j++) {
            free(g_eps[i].params[j].name);
            for (int k = 0; k < g_eps[i].params[j].nvals; k++) free(g_eps[i].params[j].vals[k]);
            free(g_eps[i].params[j].vals);
            for (int k = 0; k < g_eps[i].params[j].nexcl; k++) free(g_eps[i].params[j].excl[k]);
            free(g_eps[i].params[j].excl);
            param_bound_free(&g_eps[i].params[j].bnd);
            param_pred_free(g_eps[i].params[j].pred, g_eps[i].params[j].npred);
            param_leq_free(g_eps[i].params[j].leq, g_eps[i].params[j].nleq);
        }
        free(g_eps[i].params);
        for (int j = 0; j < g_eps[i].nh; j++) { free(g_eps[i].hdrs[j].name); free(g_eps[i].hdrs[j].value); }
        free(g_eps[i].hdrs);
    }
    free(g_eps); g_eps = NULL; g_eps_n = g_eps_cap = 0;
    /* …AND THE ASK COUNTERS BESIDE THE ARRAY THEY ARE A FACT ABOUT. One process runs many sessions and this is
       the per-session teardown (solver/engine.c resets its own per-session rows on the same line it calls
       this, and says why), so a count left standing would report a previous document's asks under this
       document's name — and it would do it in the flattering direction, since `asks > preProgram` is the
       reading that says running code composed a request. The mint arm is reset with `g_eps_n` for a
       second reason: the identity between them is asserted every time the surface is read, so leaving one of
       the pair behind would abort the next session's first census. */
    g_asks = g_asks_pre_program = g_ask_suppressed = g_ask_merged = g_ask_minted = 0;
    g_ask_merged_pre_program = 0;
    /* AND WHAT THE NEXT SESSION WOULD BE GRADING AGAINST, TAKEN HERE BECAUSE HERE IS WHERE THE SCOPES PART.
       This line puts this file's program-state boundary back to the start of time and the engine's stays
       where it is; the latch is the difference, and the door asserts on it at the first ask. */
    g_boundary_spent = engine_any_program_started();
    /* …AND THE HOST EDGE'S ROWS, ON THE SAME LINE AND FOR THE SAME SENTENCE. They are read against the ask
       rows above, so a scope they did not share with them would be the defect the latch one line up exists to
       catch, arriving through a counter instead of through a boundary.
       THE ARRAY IS GIVEN BACK AND THE TABLE IS LET GO, in that order and both of them, because the next
       session's edge declares itself again at its own init: keeping the array would be a session's allocation
       outliving the session, and keeping the pointer while freeing the array would leave the next declare
       taking the early return above and every teardown indexing a freed block. */
    edge_release(&g_fetch_edge);
    edge_release(&g_xhr_edge);
    g_xhr_offered = 0;
}
