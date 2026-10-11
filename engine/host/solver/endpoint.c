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
#include "solver/witness_verify.h"   /* the queue an eligible witness is verified through */
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
/* What one sighting's flow assumed (endpoint.h's `witness`): pins, each a source and the value its equality
   determined, and assumed decisions that determined no value, each the condition and the arm taken or the
   operation and the completion. A record holds one Witness per distinct sighting witness and never merges two,
   since their union would state a combination no flow assumed. `asked` is 0 where no flow stood, so the
   question was not asked; an empty asked witness is the flow having assumed nothing. Entries are sorted, so
   two witnesses compare by their members and not by the order the constraint chain held them in. */
/* `verify` is what firing it established, in the order a merge may only raise (endpoint.h's `verification`):
   no candidate could be minted for it, a verification run was minted and has not yet observed it, or such a
   run reached its request assuming nothing of its own. `canon` is its identity as text, the verification key's
   third field; both are written when a record files it. */
typedef enum { WIT_PIN = 0, WIT_BRANCH, WIT_OUTCOME } WitWhat;
typedef enum { WIT_VERIFY_NO_CANDIDATE = 0, WIT_VERIFY_CANDIDATE, WIT_VERIFY_VERIFIED } WitVerify;
typedef struct { WitWhat what; char *subject; char *value; ConcolicLit kind; int taken; } WitEntry;
typedef struct { int asked; WitEntry *e; int n, cap; WitVerify verify; char *canon; } Witness;
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
                 /* `driven` is whose act the sighting was (solver/engine.h's PENDING_DRIVEN_*), part of the
                    identity beside `prov`: a drive's values are offered as examples marked driven, so they may
                    not merge into the page's own record. */
                 int driven;
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
                 char *body_mime, *body_b64, *body_shape, *body_example_b64;
                 Witness *wit; int nwit, witcap; } Endpoint;

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
    CHECK_FAILF("endpoint: an @H record states the door %d, which is none of endpoint.h's ENDPOINT_DOORS — every "
                "producer states its door and the mint refuses an unstated one, so this record reached the surface "
                "without a producer naming the mechanism that composed it",
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
    CHECK_FAILF("endpoint: an @H record states the door %d, which is none of endpoint.h's ENDPOINT_DOORS — the reach "
                "census can only classify a door the list names, so the class it would raise is outside the "
                "partition",
                door);
}

/* See endpoint.h. `endpoint_door_token`'s construct and its severity, one list over. */
const char *endpoint_reach_token(int reach) {
    switch (reach) {
#define ENDPOINT_REACH_ARM(id, token) case id: return token;
    ENDPOINT_REACHES(ENDPOINT_REACH_ARM)
#undef ENDPOINT_REACH_ARM
    }
    CHECK_FAILF("endpoint: the @H reach census states the class %d, which is none of endpoint.h's ENDPOINT_REACHES — "
                "every class comes off `endpoint_door_reach`, generated from the door list's third column, so no "
                "list produced this value",
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
                "ENDPOINT_ADDRESS_CLASSES — `address_class_of` returns a member of that list on every arm, so "
                "something other than the mint wrote this field",
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
    CHECK_FAILF("endpoint: an @H record states the door %d, which is none of endpoint.h's ENDPOINT_DOORS — the razor "
                "composer asks whether a door's bytes were in the served document, and it cannot ask that of a door "
                "the list does not name",
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
                "ENDPOINT_RAZOR_CLASSES — `endpoint_razor_class_of` returns one of exactly two members, so nothing "
                "in this file composed this value",
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
                "ENDPOINT_WITNESS_CLASSES — `witness_class_now` returns one of exactly three members, so something "
                "other than the mint wrote this field",
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

static void wit_add(Witness *w, WitWhat what, const char *subject, const char *value, ConcolicLit kind, int taken) {
    WitEntry *x;

    if (w->n >= w->cap) {
        int nc = w->cap ? w->cap * 2 : 4;
        WitEntry *ne = realloc(w->e, (size_t)nc * sizeof *ne);
        CHECK(ne, "endpoint: OOM recording a sighting's witness");
        w->e = ne; w->cap = nc;
    }
    x = &w->e[w->n++];
    x->what = what;
    x->subject = strdup(subject);
    x->value = value ? strdup(value) : NULL;
    CHECK(x->subject && (!value || x->value), "endpoint: OOM recording a sighting's witness");
    x->kind = kind;
    x->taken = taken;
}

static void wit_pin_visit(const char *src, ConcolicLit kind, const char *val, void *ud) {
    DCHECK(src != NULL && val != NULL && kind != CONCOLIC_LIT_NONE,
           "the pin store handed a witness a pin with no source, value or kind");
    wit_add(ud, WIT_PIN, src, val, kind, 0);
}

static void wit_decision_visit(ConcolicDecision what, const char *spelling, int taken, void *ud) {
    DCHECK(spelling != NULL, "the constraint handed a witness an assumed decision with no spelling");
    DCHECKF(what == CONCOLIC_DECISION_BRANCH ? (taken == 0 || taken == 1)
                                             : (what == CONCOLIC_DECISION_OUTCOME && taken >= 0),
            "the constraint handed a witness an assumed decision of kind %d taking %d, which no arm states",
            (int)what, taken);
    wit_add(ud, what == CONCOLIC_DECISION_BRANCH ? WIT_BRANCH : WIT_OUTCOME, spelling, NULL, CONCOLIC_LIT_NONE,
            taken);
}

static int wit_str_cmp(const char *a, const char *b) {
    if (!a || !b) return (a != NULL) - (b != NULL);
    return strcmp(a, b);
}

static int wit_entry_cmp(const void *pa, const void *pb) {
    const WitEntry *a = pa, *b = pb;
    int r;

    if (a->what != b->what) return (int)a->what - (int)b->what;
    if ((r = strcmp(a->subject, b->subject)) != 0) return r;
    if ((r = wit_str_cmp(a->value, b->value)) != 0) return r;
    if (a->kind != b->kind) return (int)a->kind - (int)b->kind;
    return a->taken - b->taken;
}

/* The running flow's witness, read at the act: its pin store and its assumed decisions. Two decision keys whose
   spellings and arms agree state one assumption, so a repeat is kept once. */
static Witness witness_now(void) {
    Witness w = { 0, NULL, 0, 0, WIT_VERIFY_NO_CANDIDATE, NULL };
    int i, u;

    if (flow_running() == NULL) return w;
    w.asked = 1;
    concolic_pins_each(wit_pin_visit, &w);
    concolic_assumed_decisions_each(wit_decision_visit, &w);
    if (w.n > 1) qsort(w.e, (size_t)w.n, sizeof *w.e, wit_entry_cmp);
    for (i = 1, u = 1; i < w.n; i++) {
        if (wit_entry_cmp(&w.e[u - 1], &w.e[i]) == 0) { free(w.e[i].subject); free(w.e[i].value); continue; }
        w.e[u++] = w.e[i];
    }
    if (w.n > 1) w.n = u;
    return w;
}

static int witness_has_pin(const Witness *w) {
    for (int i = 0; i < w->n; i++) if (w->e[i].what == WIT_PIN) return 1;
    return 0;
}

static int witness_same(const Witness *a, const Witness *b) {
    if (a->asked != b->asked || a->n != b->n) return 0;
    for (int i = 0; i < a->n; i++) if (wit_entry_cmp(&a->e[i], &b->e[i]) != 0) return 0;
    return 1;
}

static void witness_free(Witness *w) {
    for (int i = 0; i < w->n; i++) { free(w->e[i].subject); free(w->e[i].value); }
    free(w->e);
    free(w->canon);
    w->e = NULL; w->n = w->cap = 0; w->canon = NULL;
}

static const char *const wit_verify_token[] = { "no-candidate", "candidate", "verified" };

/* One record's `witness` value (endpoint.h): an array with one element per distinct sighting witness, `null`
   for a sighting no flow stood at, else `{assumed, verification}`. */
static void witness_json(JsonBuf *b, const Endpoint *e) {
    DCHECK(e->nwit >= 1, "an @H record carries no sighting witness — every mint and merge files one");
    json_buf_raw(b, "[");
    for (int i = 0; i < e->nwit; i++) {
        const Witness *w = &e->wit[i];
        if (i) json_buf_raw(b, ",");
        if (!w->asked) {
            DCHECK(w->n == 0, "a witness no flow stood at carries entries");
            json_buf_raw(b, "null");
            continue;
        }
        CHECK(w->verify >= WIT_VERIFY_NO_CANDIDATE && w->verify <= WIT_VERIFY_VERIFIED,
              "endpoint: a sighting witness states a verification outside its three states");
        json_buf_raw(b, "{"); json_buf_key(b, "verification"); json_buf_str(b, wit_verify_token[w->verify]);
        json_buf_raw(b, ","); json_buf_key(b, "assumed");
        json_buf_raw(b, "[");
        for (int j = 0; j < w->n; j++) {
            const WitEntry *x = &w->e[j];
            if (j) json_buf_raw(b, ",");
            json_buf_raw(b, "{");
            switch (x->what) {
            case WIT_PIN:
                json_buf_key(b, "source"); json_buf_str(b, x->subject);
                json_buf_raw(b, ","); json_buf_key(b, "value"); json_buf_str(b, x->value);
                json_buf_raw(b, ","); json_buf_key(b, "kind"); json_buf_str(b, concolic_lit_report_name(x->kind));
                break;
            case WIT_BRANCH:
                json_buf_key(b, "decision"); json_buf_str(b, x->subject);
                json_buf_raw(b, ","); json_buf_key(b, "taken"); json_buf_raw(b, x->taken ? "true" : "false");
                break;
            case WIT_OUTCOME: {
                char num[16];
                int wrote = snprintf(num, sizeof num, "%d", x->taken);
                CHECK(wrote > 0 && (size_t)wrote < sizeof num, "endpoint: a completion index did not fit");
                json_buf_key(b, "decision"); json_buf_str(b, x->subject);
                json_buf_raw(b, ","); json_buf_key(b, "completion"); json_buf_raw(b, num);
                break;
            }
            }
            json_buf_raw(b, "}");
        }
        json_buf_raw(b, "]}");
    }
    json_buf_raw(b, "]");
}

/* The witness's identity as text: each entry's fields composed, then the entries, with concolic_ident_compose's
   length-prefixed encoding, so two witnesses share a canon exactly when witness_same says they are one. */
static char *witness_canon(const Witness *w) {
    const char **ents = malloc((size_t)(w->n ? w->n : 1) * sizeof *ents);
    char *out;

    CHECK(ents, "endpoint: OOM spelling a witness");
    for (int i = 0; i < w->n; i++) {
        static const char *const tag[] = { "p", "b", "o" };
        char taken[16];
        const char *f[5];
        snprintf(taken, sizeof taken, "%d", w->e[i].taken);
        f[0] = tag[w->e[i].what]; f[1] = w->e[i].subject; f[2] = w->e[i].value ? w->e[i].value : "";
        f[3] = w->e[i].what == WIT_PIN ? concolic_lit_report_name(w->e[i].kind) : ""; f[4] = taken;
        ents[i] = concolic_ident_compose("e", f, 5);
        CHECK(ents[i], "endpoint: OOM spelling a witness entry");
    }
    out = concolic_ident_compose("w", ents, w->n);
    CHECK(out, "endpoint: OOM spelling a witness");
    for (int i = 0; i < w->n; i++) free((char *)ents[i]);
    free(ents);
    return out;
}

/* The verification keys a run of this session has observed (endpoint_verify_observe). A witness filed after its
   key was observed is verified at the filing. */
static char **g_verified;
static int g_verified_n, g_verified_cap;

static int verified_key(const char *key) {
    for (int i = 0; i < g_verified_n; i++) if (!strcmp(g_verified[i], key)) return 1;
    return 0;
}

/* WHETHER FIRING CAN CHECK THIS WITNESS, and with which value: every entry a String pin on one source, all to
   one value, the source a declared root delivered through the victim's own address, and the value one the
   address can carry (it begins with the component's prefix and holds something after it). The payload is the
   value past the prefix, which concolic_deliver puts back with the component's encoding.
   Named residual. Not covered: a witness holding a decision, a pin on a derived value (the beta row's
   `{location.hash}.slice(1)`), a non-String pin, more than one source, a source delivered by plant, referring
   address, user file or message, and a root no attacker delivers (a server reply field). Next diff: the @S
   solver composing a root value from the entries, and the delivery mechanisms beyond the address. How its
   absence shows: rows whose every witness reads `no-candidate` on a page whose gates test transformed or
   reply-borne values. */
static char *witness_payload(const Witness *w, const char **src) {
    const char *kind;
    char prefix = 0;

    if (!w->asked || w->n == 0) return NULL;
    for (int i = 0; i < w->n; i++)
        if (w->e[i].what != WIT_PIN || w->e[i].kind != CONCOLIC_LIT_STRING ||
            strcmp(w->e[i].subject, w->e[0].subject) || strcmp(w->e[i].value, w->e[0].value)) return NULL;
    if (!concolic_source_delivery(w->e[0].subject, &kind, &prefix) || strcmp(kind, "address") || !prefix)
        return NULL;
    if (w->e[0].value[0] != prefix || !w->e[0].value[1]) return NULL;
    *src = w->e[0].subject;
    return strdup(w->e[0].value + 1);
}

/* Files a sighting's witness on its record, taking ownership: a witness the record already holds is the same
   statement and is freed, anything else is one more, and is queued for verification when firing can check it.
   `*w` is empty afterwards either way. */
static void endpoint_witness_add(Endpoint *e, Witness *w) {
    for (int i = 0; i < e->nwit; i++)
        if (witness_same(&e->wit[i], w)) { witness_free(w); w->asked = 0; return; }
    DCHECK(w->canon == NULL && w->verify == WIT_VERIFY_NO_CANDIDATE,
           "a sighting witness reached its record already spelled or verified — both are written here");
    if (w->asked) {
        const char *src = NULL;
        char *payload = witness_payload(w, &src);
        w->canon = witness_canon(w);
        if (payload) {
            char *key = witness_verify_key(e->method, e->path, w->canon);
            witness_verify_queue(key, src, payload);
            w->verify = verified_key(key) ? WIT_VERIFY_VERIFIED : WIT_VERIFY_CANDIDATE;
            free(key);
            free(payload);
        }
    }
    if (e->nwit >= e->witcap) {
        int nc = e->witcap ? e->witcap * 2 : 2;
        Witness *nw = realloc(e->wit, (size_t)nc * sizeof *nw);
        CHECK(nw, "endpoint: OOM filing a sighting's witness");
        e->wit = nw; e->witcap = nc;
    }
    e->wit[e->nwit++] = *w;
    w->e = NULL; w->n = w->cap = 0; w->asked = 0; w->canon = NULL; w->verify = WIT_VERIFY_NO_CANDIDATE;
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

/* The ask, which the outcome figures cannot carry: endpoint_surface_census walks the record array, so it counts
 * outcomes and cannot tell a component that never asked from one whose ask a gate refused. endpoint_record is the
 * one door every HTTP-shaped edge passes (endpoint.h's funnel note), so a count at its entry is how often this
 * engine offered an address to the surface; five Fetch §5.4 stages stand in front of it (endpoint.h's residual).
 * `emitted - preProgram` is a ceiling on what forced execution contributed, and its zero reads either "no arm
 * composed a request" or "arms composed requests already on the surface" (a repeat merges and moves nothing, since
 * `pre_program` is decided at the mint). `asks - preProgram` is nonzero only in the second, a ceiling on it;
 * `merged_pre_program` states it exactly.
 * Six rows over one event. By outcome `suppressed + merged + minted == asks`, asserted where all four are in hand;
 * by program state `preProgram <= asks`, a containment (a stored complement would be a second raise); the sixth is
 * a cut inside the merged arm, its containments asserted at the accessor. Lifetime counts that never decrease, so
 * a sample below its predecessor is this file and not the run. Scoped to the surface: reset in endpoint_init and
 * endpoint_free beside `g_eps_n`, since one process runs many sessions. A report; nothing branches on one. */
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
    DCHECK(nlen > 0, "an endpoint param was minted with no name — a nameless param cannot be substituted back into a "
                     "request, so every producer must refuse the unnamed case at its own site");
    /* Where it landed is asserted at the mint: a param whose location is not one of the three is a producer that
       built a param without saying what it is, and a consumer defaulting it (`p.location || "query"`) would hide
       that. Every site that adds a param passes through here. */
    DCHECK(loc == EP_QUERY || loc == EP_PATH || loc == EP_BODY,
           "an endpoint param was minted with no location — a param that cannot be placed cannot be replayed, and "
           "every producer states query, path or body");
    /* A path param always has a hole: `path_scan` mints a param only for a segment holding a brace and passes the
       brace-stripped segment as the key, so this holds by construction, and asserting it makes a `location:"path"`
       row a denominator a consumer can trust and holds the next producer of a path param to it. The query and body
       producers have no such invariant (`concolic_hole_key` answers NULL for a brace-free value), which is what
       `has_hole` is for. */
    DCHECK(loc != EP_PATH || hole != NULL,
           "a path param was minted with no hole key — path_scan mints one only for a braced segment and passes its "
           "brace-stripped name as the key, so this is a producer that mints path params without naming the hole");
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
   destroys. `hm` is the list's hole mask aligned with `base` (solver/concolic.h's concolic_shape_hole_mask): a `&` or
   an `=` this engine spelled inside an unknown is no syntax of the page's. NULL for a list of the page's bytes only,
   such as the example. */
static int kv_is_page(const char *base, const unsigned char *hm, const char *p) {
    return !hm || !hm[p - base];
}

/* Whether `[p, p + n)` of a list holds a hole: a byte the mask marks, or, with no mask, a brace — the one marker a
   recorded address string still carries. Every hole this engine spells names one in braces (concolic_new asserts it
   of a source, and a derivation spells its operands), which is asserted per run. */
static int kv_has_hole(const char *base, const unsigned char *hm, const char *p, size_t n) {
    size_t i = 0, s;
    int any = 0;

    if (!hm) return memchr(p, '{', n) != NULL;
    while (i < n) {
        if (!hm[p - base + i]) { i++; continue; }
        for (s = i; i < n && hm[p - base + i]; i++) {}
        DCHECK(memchr(p + s, '{', i - s) != NULL,
               "a run of an address's bytes this engine spelled for an unknown names no `{`-delimited hole — "
               "every shape derives from a source's braced name, so the hole mask and the shape it marks "
               "have come apart");
        any = 1;
    }
    return any;
}

static const char *kv_pair_next(const char *base, const unsigned char *hm, const char *p, const char **nm,
                                size_t *nn, const char **vl, size_t *vn) {
    const char *amp, *eq;
    size_t plen;

    while (*p == '&' && kv_is_page(base, hm, p)) p++;   /* `&&`, and a trailing `&`: no pair */
    if (!*p) return NULL;
    for (amp = p; *amp && !(*amp == '&' && kv_is_page(base, hm, amp)); amp++) {}
    plen = (size_t)(amp - p);
    for (eq = p; eq < p + plen && !(*eq == '=' && kv_is_page(base, hm, eq)); eq++) {}
    if (eq == p + plen) eq = NULL;
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
static Pair *kv_split(const char *s, const unsigned char *hm, int *n_out) {
    int n = 0, i = 0;
    const char *p = s, *next;
    const char *nm, *vl;
    size_t nn, vn;
    Pair *v;

    while ((p = kv_pair_next(s, hm, p, &nm, &nn, &vl, &vn)) != NULL) n++;
    v = malloc((size_t)(n ? n : 1) * sizeof *v);
    CHECK(v, "endpoint: OOM splitting an address half into query pairs");
    p = s;
    while ((next = kv_pair_next(s, hm, p, &nm, &nn, &vl, &vn)) != NULL) {
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
           "a query pair run was asked for no pairs at all — every span `query_align` records covers at least one "
           "pair");
    for (k = 0; k < npair; k++) {
        const char *xn = NULL, *xv = NULL, *next;
        size_t xnn = 0, xvn = 0;

        next = kv_pair_next(b, NULL, p, &xn, &xnn, &xv, &xvn);
        DCHECK(next != NULL,
               "a query pair run ran off the end of its example — `query_align` proved this span against the same "
               "string, so this walk and that proof are reading two different addresses");
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
static int *query_align(const char *shape, const unsigned char *shm, const char *ex, int *nshape) {
    int n = 0, m = 0, i, j, k;
    Pair *S = kv_split(shape, shm, &n);
    Pair *E = kv_split(ex, NULL, &m);
    /* ways[i][j]: how many readings match the shape's pairs from `i` onward against the example's from `j`
       onward, saturated at two. Filled backwards so each row reads only the row below it. */
    unsigned char *w = calloc((size_t)(n + 1) * (size_t)(m + 1), 1);
    int *span = NULL;

    CHECK(w, "endpoint: OOM aligning a request's query against the concrete URL a flow computed");
    w[(size_t)n * (size_t)(m + 1) + (size_t)m] = 1;   /* both exhausted together: one reading, the empty one */
    for (i = n - 1; i >= 0; i--) {
        unsigned char *cur = w + (size_t)i * (size_t)(m + 1);
        const unsigned char *nxt = cur + (m + 1);
        if (kv_has_hole(shape, shm, S[i].vl, S[i].vn)) {
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
            if (!kv_has_hole(shape, shm, S[i].vl, S[i].vn)) {
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
                   "a query whose shape and example hold the same number of pairs aligned with a hole over more than "
                   "one pair — a literal covers one pair and a hole at least one, so equal counts admit only the 1:1 "
                   "alignment");
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
static void kv_pairs(KvBuf *b, const char *text, const unsigned char *thm, const char *ex, EpLoc loc) {
    int npair = 0, i = 0;
    /* NULL for both ways there is nothing to align against (no example, or no unique reading), one answer to this
       walk: it has no bytes to attribute to a hole. `query_align`'s banner tells them apart. */
    int *span = ex ? query_align(text, thm, ex, &npair) : NULL;
    const char *a = text, *e = ex;

    for (;;) {
        const char *nm, *vl, *en = NULL, *ev = NULL, *na, *ne = NULL;
        size_t nn, vn, enn = 0, evn = 0;
        char *shapeval, *hole;
        int holed;

        na = kv_pair_next(text, thm, a, &nm, &nn, &vl, &vn);
        if (!na) break;
        a = na;
        /* The two walks are one rule, held to it here: `query_align` established the pairing with the same
           splitter, and asserting it again stops the alignment test and the emission drifting into two readings
           of one list, the only way a value could be attributed to the wrong param. */
        if (span) {
            DCHECK(i < npair,
                   "the shape walk ran past the pair count its alignment was built from — `kv_split` and this loop "
                   "split one string and the span array is sized by the first, so this reads past the end of that "
                   "array");
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
        holed = kv_has_hole(text, thm, vl, vn);
        hole = holed ? concolic_hole_key(shapeval) : NULL;   /* the domain is a fact about the shape */
        /* The example where it holds bytes. An empty one (`location.hash` on an address with no fragment) is a
           value param_add_val records as no value at all, so the param would carry neither bytes nor the hole
           they came through; the shape is that provenance, and it is what the param then carries. */
        if (span && holed && evn > 0)
            kv_add(b, nm, nn, ev, evn, loc, hole);
        else
            kv_add(b, nm, nn, vl, vn, loc, hole);
        free(hole);
        free(shapeval);
    }
    DCHECK(!span || i == npair,
           "the shape walk visited a different number of pairs than the alignment was built over — the span array is "
           "sized by `kv_split` and indexed by this loop, so the two must visit the same pairs");
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
           "a URL's concolic example is not a primitive — the example is the value the interpreter computed, so a "
           "wrapper or a symbol here means the propagation lost the address");
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
           "a segment run was asked for no segments at all — every span `path_align` records covers at least one "
           "segment");
    for (e = b; *e; e++) {
        if (*e != '/') continue;
        if (--nseg == 0) return e;
    }
    DCHECK(nseg == 1,
           "a segment run ran off the end of its example — `path_align` proved this span against the same string, so "
           "this walk and that proof are reading two different addresses");
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
                   "an address whose shape and example hold the same number of segments aligned with a hole over "
                   "more than one segment — a literal covers one segment and a hole at least one, so equal counts "
                   "admit only the 1:1 alignment");
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
                   "the shape walk ran past the segment count its alignment was built from — `seg_split` and this "
                   "loop split one string and the span array is sized by the first, so this reads past the end of "
                   "that array");
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
                   "a path param's name still holds a brace or a slash — the popup substitutes a hole by matching "
                   "/\\{([^}/]+)\\}/ against the path, so a name outside that grammar names a hole no substitution "
                   "can find");
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
           "the shape walk visited a different number of segments than the alignment was built over — the span array "
           "is sized by `seg_split` and indexed by this loop, so the two must visit the same segments");
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
               "an unknown byte range of a request body reached the @H surface with no shape — the shape records "
               "where the bytes came from, so a range with neither an example nor a source carries nothing a "
               "reviewer can read");
        DCHECK(i == 0 || body->span[i - 1].off + body->span[i - 1].len <= s->off,
               "a request body's unknown byte ranges arrived out of order or overlapping — the capture walks the "
               "page's view in index order, so two ranges claiming one byte are two records of one body merged");
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
        kv_pairs(out, text, NULL, NULL, EP_BODY);
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
           "the base64 of a request body was asked for a body that is not bytes the request sends — this function "
           "makes the record's replay claim, so it may run only over an EPB_SENT body; body_store routes a shape to "
           "`body_shape`");
    return body_b64_encode(body->bytes, body->len);
}

/* And the other claim over the same codec, which is why the assert is here and not in it: these bytes are what the
   payload looks like, never what the request sends (inside a span stands an unknown's example or a byte nobody
   wrote, outside every span a byte the page computed). Reaching this over an EPB_SENT body would understate a
   replayable payload as an example, a lost finding rather than a fabricated one. */
static char *body_example_b64(const EndpointBody *body) {
    if (!body || !body->bytes || body->len == 0) return NULL;
    DCHECK(body->kind == EPB_EXAMPLE,
           "the example bytes of a request body were asked for a body that is not an example — this key is reached "
           "only for a body carrying spans, and body_store routes every other body to the sent bytes or the shape");
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
               "a request body carrying unknown byte ranges was stored with none of its ranges named — every span "
               "mints a param, so the span walk was skipped and the example bytes would reach a reviewer with no "
               "address to splice at");
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
           "an endpoint param's exclusion set was seeded twice — the first sighting takes the set and every later "
           "one intersects it, so a second seed would discard a narrowing another path made");
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
           "an endpoint param's interval was seeded twice — the first sighting takes it and every later one widens "
           "it, so a second seed would discard a bound another path had already widened");
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
           "an endpoint param's call-predicate set was seeded twice — the first sighting takes the set and every "
           "later one intersects it, so a second seed would discard a narrowing another path made");
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
           "an endpoint param's loose-equality set was seeded twice — the first sighting takes the set and every "
           "later one intersects it, so a second seed would discard a narrowing another path made");
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
static int same_identity(Endpoint *e, const char *method, const char *path, int prov, int driven,
                         const KvBuf *kv) {
    if (e->prov != prov || e->driven != driven || strcmp(e->method, method) || strcmp(e->path, path) ||
        e->np != kv->n) return 0;
    for (int i = 0; i < kv->n; i++)
        if (e->params[i].loc != kv->e[i].loc || strcmp(e->params[i].name, kv->e[i].name)) return 0;
    return 1;
}

/* The address's path, re-spelled by path_scan, and its path and query params into `kvb`: the one speller of a
   request's address for the record and for a verification's observation. Returns the path, owned. */
static char *address_scan(JSContext *ctx, JSValueConst url, KvBuf *kvb) {
    char *disp = url_display(ctx, url);
    char *ex = url_example(ctx, url);
    /* The shape's own `?` and `&` are the page's bytes only: a hole this engine spelled (an operator, a joint's
       separator, a member path) is one unknown and no syntax, so the split reads the hole mask. A concrete address
       has none and is split as it stands. */
    unsigned char *dhm = concolic_shape_hole_mask(url);
    const char *dq = disp;
    while (*dq && !(*dq == '?' && kv_is_page(disp, dhm, dq))) dq++;
    char *shape_path = strndup(disp, (size_t)(dq - disp));
    char *expath = ex ? url_path_of(ex) : NULL;
    CHECK(shape_path, "endpoint: OOM copying an address's path half");
    /* Named residual. Not covered: the path half is split on every `/`, so a hole whose spelling holds one (a
       division, `{a}/{b}`) is cut into two segments. Next diff: path_scan and path_align read the same mask, and a
       path param's name grammar (the popup's /\{([^}/]+)\}/) admits a hole key holding `/`. How its absence shows: a
       record whose path has a segment that is half of a hole's spelling. */
    char *path = path_scan(kvb, shape_path, expath);
    { const char *eq = ex ? strchr(ex, '?') : NULL;
      if (*dq && dq[1])
          kv_pairs(kvb, dq + 1, dhm ? dhm + (dq + 1 - disp) : NULL, (eq && eq[1]) ? eq + 1 : NULL, EP_QUERY); }
    free(disp); free(dhm); free(ex); free(expath); free(shape_path);
    return path;
}

/* A witness verification's own observation, made at its request (endpoint_record's suppressed arm): the run
   reached the witnessed method and path, and its own witness is empty, so the delivered value alone reached
   it. Every filed witness with that key is raised to verified, and the key is kept for one filed later. */
static void endpoint_verify_observe(JSContext *ctx, const char *key, const char *method, JSValueConst url) {
    char *vm, *vp, *vc, *path;
    KvBuf kvb = { 0 };
    Witness own;

    witness_verify_key_split(key, &vm, &vp, &vc);
    if (strcmp(vm, method)) { free(vm); free(vp); free(vc); return; }
    path = address_scan(ctx, url, &kvb);
    kv_free(&kvb);
    own = witness_now();
    DCHECK(own.asked, "a witness verification observed a request with no flow standing");
    if (!strcmp(path, vp) && own.n == 0) {
        if (!verified_key(key)) {
            if (g_verified_n >= g_verified_cap) {
                int nc = g_verified_cap ? g_verified_cap * 2 : 8;
                char **nv = realloc(g_verified, (size_t)nc * sizeof *nv);
                CHECK(nv, "endpoint: OOM recording a verified witness");
                g_verified = nv; g_verified_cap = nc;
            }
            g_verified[g_verified_n] = strdup(key);
            CHECK(g_verified[g_verified_n], "endpoint: OOM recording a verified witness");
            g_verified_n++;
        }
        for (int i = 0; i < g_eps_n; i++) {
            if (strcmp(g_eps[i].method, vm) || strcmp(g_eps[i].path, vp)) continue;
            for (int j = 0; j < g_eps[i].nwit; j++)
                if (g_eps[i].wit[j].canon && !strcmp(g_eps[i].wit[j].canon, vc))
                    g_eps[i].wit[j].verify = WIT_VERIFY_VERIFIED;
        }
    }
    witness_free(&own);
    free(path); free(vm); free(vp); free(vc);
}

void endpoint_record(JSContext *ctx, const char *method, JSValueConst url,
                     const EndpointHeader *hdrs, int nhdrs, const EndpointBody *body, int prov, int driven,
                     int door) {
    /* What this sighting is evidence of, asserted at the mint for `kv_add`'s reason: every site that records an
       endpoint passes through here. A value outside the vocabulary is the dangerous one: `0` is `observed`, so an
       uninitialised grade reads as the strongest claim this surface makes. */
    DCHECKF(prov == PROV_OBSERVED || prov == PROV_DERIVED || prov == PROV_FORCED,
            "an endpoint was recorded with the provenance %d, which is none of the three solver/pending.h "
            "defines; the grade is part of the record's identity and is emitted on it. method=%s", prov, method);
    /* And whose act it was, asserted at the producer with the grade beside it, because the two are independent
       facts about one path: the drive mark is flow_path_driven (an orphan drive or a candidate session) and the
       grade is `forced` exactly when the path stood on a forced arm, so a drive that forced no arm is `derived` and driven, and its values are
       offered as the drive computed them. A door stated off a record is exempt: a reply chunk takes the request's
       facts, and a program load takes its park's, composed at pending_push off the parking flow, which need not
       be the running one (a joined document's boot flow is seeded from inside its creator's). */
    DCHECKF(driven == 0 || driven == 1, "an endpoint was recorded with the drive mark %d. method=%s", driven,
            method);
    {
        const Flow *rf = flow_running();
        int off_record = door == EPD_REPLY_CHUNK || door == EPD_DOCUMENT_SCRIPT || door == EPD_INJECTED_SCRIPT ||
                         door == EPD_MODULE_IMPORT;
        DCHECKF(off_record || rf == NULL || driven == flow_path_driven(rf),
                "an endpoint minted by running code states the drive mark %d while the running flow's is %d — "
                "the producer composed it without asking engine_driven_of_running_path, so the trusted zone and "
                "the example pools would read this sighting as the wrong party's act. method=%s", driven,
                rf ? flow_path_driven(rf) : -1, method);
        DCHECKF(off_record || rf == NULL || (prov == PROV_FORCED) == flow_path_forced(rf),
                "an endpoint minted by running code states the grade %d while the running flow's forced bit is "
                "%d — the grade must be composed off `path_forced` alone, never off the drive mark, or a drive "
                "that forced nothing would lose its examples. method=%s", prov,
                rf ? flow_path_forced(rf) : -1, method);
    }
    /* And which mechanism composed the address, asserted at the same mint for the same reason. A membership test
       despite its spelling: `ENDPOINT_DOORS` gives its members no explicit values, so the enum is dense from
       `EPD_UNSTATED` to `EPD_COUNT`. `EPD_UNSTATED` is 0 so that a producer that forgets is refused here rather than
       read as whichever mechanism the numbering put first; endpoint_door_token's CHECK catches the same record in
       release. */
    DCHECKF(door > EPD_UNSTATED && door < EPD_COUNT,
            "an endpoint was recorded with the door %d, which is none of endpoint.h's ENDPOINT_DOORS — the door is "
            "the one fact about a sighting no consumer can re-derive; state the door of the mechanism this call site "
            "is. method=%s", door, method);
    /* And whose bytes the body is: what the request sends, the engine's display spelling of an unknown, or an
       example of the payload, which only the producer knows (it asked core/fetch/body.h to obtain them). The zero
       is UNSTATED, so a producer that forgets is refused rather than read as the replayable arm. The message names
       mime and method because three callers pass a body and all would report this line. */
    DCHECKF(!body || body->kind == EPB_SENT || body->kind == EPB_SHAPE || body->kind == EPB_EXAMPLE,
            "a request body reached the @H surface without its producer stating whether the bytes are sent, the "
            "display spelling of an unknown, or an example of the payload — the three are graded apart; state the "
            "kind from the arm core/fetch/body.h reports. method=%s "
            "mime=%s", method, body->mime ? body->mime : "(none)");
    /* And the one pairing between the two facts above, which the example key rests on: §10.4.5.18
       TypedArraySetElement ( obj, index, value ) records a span even where no byte was written, so a body carrying
       spans has bytes the request cannot be said to send. Asserted at the mint, so body_store can dispatch on
       `nspan` and body_example_b64 assert its precondition. One-way: a body with no span may still be an example. */
    DCHECKF(!body || body->nspan == 0 || body->kind == EPB_EXAMPLE,
            "a request body carrying unknown byte ranges was graded as something other than an example — some of its "
            "bytes are an unknown's example, so no record of it may claim the request sends exactly them. method=%s "
            "kind=%d",
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
           "an @H ask arrived on a surface whose counters were reset after a program had started on this instance — "
           "the program-state boundary `g_prog_starts` is not reset per session while these counters are, so "
           "`pre_program` would grade this whole document's markup as post-program");
    g_asks++;
    if (ask_pre_program) g_asks_pre_program++;
    if (g_suppress) {                       /* candidate/verify run: not a real @H endpoint */
        const Flow *f = flow_running();
        g_ask_suppressed++;
        if (f && f->cand_vfy) endpoint_verify_observe(ctx, f->cand_vfy, method, url);
        return;
    }
    /* Whether this sighting's address was one the run had determined, read once here from the value the display
       spelling is taken from: the merge arm and the mint both need it, the value is borrowed for this call, and
       nothing below holds the JSValue. */
    int acls = address_class_of(url);
    /* The bar's claiming member maps to `runtime-only` for every door: it is `endpoint_razor_class_of`'s first
       line. Asserted here because the mapping takes a door and only a caller holds one; a door arm put in front of
       that line fires this. */
    DCHECK(endpoint_razor_class_of(door, EPA_UNKNOWN) == EPZ_RUNTIME_ONLY,
           "`address_class_of`'s flow-less member does not map to `runtime-only` under `endpoint_razor_class_of` — "
           "the gate that stops a stale pin chain from claiming a determination rests on that mapping");
    /* And the member that does not claim: `address_class_of` narrows the bar by returning `EPA_UNKNOWN_UNPROVEN`,
       and that narrowing is real only while this mapping leaves such a row `unproven`; a door arm, or a member
       reordered above `EPA_UNKNOWN`, would hand the bar back every instrument hole with every census still summing.
       The `EPB_OFF_DOCUMENT` arm is excluded, not exempted: that operand is the bar's other positive statement
       (`reply-chunk` hands this surface bytes in no served document), so a row clearing through the door is the
       union working. */
    DCHECK(endpoint_door_bytes(door) == EPB_OFF_DOCUMENT ||
           endpoint_razor_class_of(door, EPA_UNKNOWN_UNPROVEN) == EPZ_UNPROVEN,
           "`address_class_of`'s whose-narrowed member clears the product's bar through a door whose bytes were in "
           "the served document — that member exists so the bar refuses an address the run did not determine");
    /* And what the path that composed it had chosen, read on the same line and for the same reason: a fact about
       the running flow, which nothing below may re-ask. */
    int wcls = witness_class_now();
    /* …and what that flow assumed, read at the same instant from the same flow. A path that chose a witness on
       an arm no example confirmed (`may-rest-on`) holds the pin it chose. */
    Witness wit = witness_now();
    DCHECK((wcls == EPW_UNASKED) == !wit.asked,
           "the witness class and the witness disagree about whether a flow stood at this sighting — both ask "
           "flow_running at one instant");
    DCHECK(wcls != EPW_MAY_REST_ON || witness_has_pin(&wit),
           "a sighting's path pinned a value on an arm no example confirmed (`may-rest-on`) and its pin store "
           "holds no pin — the pin and the path bit are written on one line of decide.c and both ride the fork");
    /* And which sources the address's bytes entered through, copied here: `concolic_root_c` points into the value's
       own record, borrowed for this call, and the record below outlives it. NULL for a non-concolic value and for
       one whose bytes entered through nothing minted as a source; `addr_class` tells them apart (see the struct). */
    char *aroot = NULL;
    { const char *r = concolic_root_c(url);
      if (r) { aroot = strdup(r);
               CHECK(aroot, "endpoint: OOM copying an address's delivery root — the record would carry a "
                            "class derived from a provenance it cannot name, and this surface's own reader "
                            "would report the address as one this engine attributed to nothing"); } }
    /* Path, then query, then body: the order a request is written in and the identity compares in. The path is
       re-spelled by the scan (see path_scan), and the re-spelled one becomes the endpoint's `url`, because the
       params are named in its grammar. The example is aligned against both halves of the address, so a hole in a
       query value (`fetch('/api/user?region=' + c.region)`) carries the bytes the code computed, as one in the path
       does; the halves are split here because `?` is the address's own boundary and neither grammar contains it.
       The body carries an example for exactly the values the run observed landing in it: a string-composed body
       spells its holes and the JSON arm reads them, and a byte-by-byte body is named off its span record
       (body_params' residual has what is still missing). */
    KvBuf kvb = { 0 };
    char *path = address_scan(ctx, url, &kvb);
    /* Named fields first; the raw bytes only where there are none: a body this engine reads is better described by
       its fields, and both would put one fact on the record twice. */
    int body_named = body_params(ctx, &kvb, body);

    for (int i = 0; i < g_eps_n; i++) {                 /* merge into an existing same-identity endpoint */
        if (same_identity(&g_eps[i], method, path, prov, driven, &kvb)) {
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
            endpoint_witness_add(&g_eps[i], &wit);
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
    e->method = strdup(method); e->path = strdup(path); e->prov = prov; e->driven = driven;
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
    endpoint_witness_add(e, &wit);
    /* And the ask and the mint are one instant. The bit is monotone and raised `++`-only at engine.c's single
       program-start line, so the two differ only if something on the path above (a display spelling, a path scan,
       a JSON body parse) runs page code. None does; the assert keeps a later diff from splitting the ask census's
       partition from the record's flag. */
    DCHECK(e->pre_program == ask_pre_program,
           "a program started between an endpoint ask reaching the @H surface and that ask minting its record — the "
           "ask census and the record's `pre_program` flag must grade one sighting on the same side of the boundary");
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
    witness_free(&wit);   /* empty: endpoint_witness_add took or freed it on every arm that reaches here */
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
           "an asset verdict arrived naming no request — the reply register is keyed on the (method, url) pair the "
           "request was owed under, so a verdict missing either half names no record");
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
           "the @H surface census was asked for with somewhere to put fewer than four of its numbers — the census is "
           "a partition and is returned whole");
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
           "the @H surface counted more records minted before the first program than it emits — the pre-program "
           "count is raised only inside the emitted arm of this walk, so a larger value is a raise outside that arm");
    /* And the two relations that span the two instruments, asserted here, the only place the record array and the
       ask counts are in one hand (endpoint_ask_census asserts its own partitions and cut). `pre <=
       g_asks_pre_program`: every pre-program record was minted by an ask whose own bit was true (the mint asserts
       the pairing per record) and assets only make `pre` smaller, so if the two drift, `emitted - preProgram` and
       `asks - preProgram` are complements of two different boundaries; they are raised at different events on
       different structures, so the sides can disagree. `g_ask_minted == g_eps_n` fires on a record reaching `g_eps`
       by a path that did not pass endpoint_record's door: endpoint.h's funnel note, stated by something that cannot
       go stale. */
    DCHECK(pre <= g_asks_pre_program,
           "the @H surface emits more pre-program records than the door counted pre-program asks — every such record "
           "was minted by such an ask, so the ask census and this walk are using two different program-state "
           "boundaries");
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
           "the @H ask census was asked for with somewhere to put fewer than six of its numbers — the two partitions "
           "and the cut are returned whole");
    DCHECK(g_asks == g_ask_suppressed + g_ask_merged + g_ask_minted,
           "the @H surface's asks do not sum over suppressed, merged and minted — they are the three exits of one "
           "door, so a total that moves without an arm is a fourth exit added above them");
    DCHECK(g_asks_pre_program <= g_asks,
           "the @H surface counted more pre-program asks than asks — the pre-program count is raised only inside the "
           "entry that raises the total, so a larger value is a second raise");
    /* And the two containments of the cut, asserted here, where every term of either is in hand; the cut selects on
       two independent facts, and each containment fires on a different way of getting it wrong. The first is
       against the arm it sits inside: it is raised only on the line after `g_ask_merged`, so a larger value is a
       raise outside that arm. */
    DCHECKF(g_ask_merged_pre_program <= g_ask_merged,
            "the @H surface counted more asks merging into a pre-program record (%ld) than asks that merged at all "
            "(%ld) — the cut is raised only inside the merge arm, so a larger cut is a raise outside "
            "it", g_ask_merged_pre_program, g_ask_merged);
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
                "a second machine declared itself to the %s edge census, or the same machine declared a different "
                "table or entry identifier — the rows are keyed by one machine's stages, so a second declaration "
                "would merge two populations", who);
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
            "a %s state was torn down before its machine declared its stages — the census cannot name the arm this "
            "teardown belongs to",
            who);
    /* A CHECK, for absent.c's vocabulary bound's reason: the line below indexes the array with this value in every
       build, and a state standing at a stage its own machine does not name is a fact about the driver, not a row to
       file. */
    CHECKF(i >= 0 && i < e->stage_n,
           "a %s state was torn down at stage %d, which its machine's table does not name (it declares %d stages "
           "based at %d) — the step driver asserts every stage against the declaration, so this census is reading a "
           "stage the driver never wrote",
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
           "a census number did not fit its conversion buffer — the buffer is sized for a long's decimal form, so "
           "this host's long is wider than the buffer was written for");
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
                "ENDPOINT_DOORS — the mint refuses an unstated door, so this record bypassed the mint's "
                "check", g_eps[i].door);
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
            "the @H surface's per-door counts sum to %ld against the %ld rows it emits — the doors partition the "
            "emitted surface over the same asset skip, so the two walks are counting different "
            "populations", sum, emitted);
    return json_buf_take(&b);
}

/* The same surface partitioned by what a parse of the document would have reached (endpoint.h has the three
   classes, why three, and why the map is a column of the door list): `beyond` is the addresses a markup parse does
   not reach, `either` the population no door can decide, and `markup` the `<head>` counted back. It is a markup
   diagnostic, not the product's bar, which `endpoint_razor_class_of` composes from `addressClass` and the door
   list's fourth column (`endpoint_razor_hist_json` below); this row is keyed on the third column, and a literal
   chunk URL delivered through `module-import` is `beyond` here and scores zero on the bar. It is a coarsening of
   the door row, not a second observation: every count is the sum of its class's door counts, so two zeroes here
   are one zero, and what it adds is the map as data. A separate walk rather than a sum of the door table, so one
   walk drifting is a loud identity failure (the walks share the `is_asset` skip). No range assert on the
   subscript: `endpoint_door_reach` is generated and its fallthrough is a CHECK in every build, so an assert could
   not fail; `endpoint_door_hist_json` asserts because its subscript is the raw `door` field. */
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

    /* Every class is emitted including zeroes, for the door census's reason and a sharper one: an absent `beyond`
       and a `beyond` of 0 are what this row keeps apart. The zero is this markup diagnostic declining to claim
       reach (never the product's bar, which no count of doors reaches); the absence is an instrument that
       stopped. */
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
            "partition the emitted surface over the same `is_asset` skip, so the two walks are counting different "
            "populations", sum, emitted);
    return json_buf_take(&b);
}

/* The same surface partitioned by whether the run had determined the address and whose unknown it rests on
   (endpoint.h has each class's claim, why there is no unstated member, and why the field is a floor under the
   hard bar). `unknown` is the addresses reached holding a value the run had not determined and resting on an
   unknown supplied outside this engine, the one figure the bar is scored off; `unknown-unproven` is undetermined
   and refused by the bar; `concrete` and `source-determined` are the rest and claim nothing about a parse. A second
   observation, not a coarsening: it is keyed on a property of the address value no door implies either way
   (`link-element` is reached by determined and tainted addresses alike, `reply-chunk` is `beyond` with a
   determined address every time), so the three histograms hold two observations. A third walk, for the reach
   row's reason. The subscript is re-asserted, though the token speller is a CHECK, because this line indexes the
   array before any speller runs; both sides are this codebase's own. Three facts about the list's order are
   asserted, since the record's merge is a max over it and a later diff can break the order without going out of
   range: `EPA_CONCRETE` is the low end (the identity element the `memset` leaves); `EPA_UNKNOWN` the high end (the
   bar keys on it, so a member above it would let a max carry a record out of the class the bar reads); and the
   determined members are a prefix (below). */
char *endpoint_address_hist_json(void) {
    JsonBuf b = { 0 };
    long n[EPA_COUNT];
    long minted, assets, emitted, pre_program, sum = 0;
    int c, i;

    DCHECK(EPA_CONCRETE == 0,
           "endpoint.h's ENDPOINT_ADDRESS_CLASSES does not begin with the member that proves nothing — the record's "
           "merge is a maximum over this order and `memset` leaves a fresh record at 0, so 0 must make no claim");
    DCHECK(EPA_UNKNOWN == EPA_COUNT - 1,
           "endpoint.h's ENDPOINT_ADDRESS_CLASSES does not end with EPA_UNKNOWN — `endpoint_razor_class_of` keys the "
           "product's bar on it and the merge is a maximum over this order, so a member above it would let a later "
           "sighting carry a record out of the class that clears the bar");
    /* The third fact, which the two ends cannot see: the list's two predicates are asked of one stored member, and
       with more than one undetermined member Q1 is spelled as a threshold (`cls <= EPA_SOURCE_DETERMINED`), which
       is correct only while the determined members are a prefix. A determined member added above an undetermined
       one would satisfy both end asserts, keep every count summing, and make Q1 answer determined for a row no
       path determined. */
    DCHECK(EPA_SOURCE_DETERMINED < EPA_UNKNOWN_UNPROVEN,
           "endpoint.h's ENDPOINT_ADDRESS_CLASSES does not hold its determined members as a prefix — consumers read "
           "determination as the threshold `cls <= EPA_SOURCE_DETERMINED`");
    memset(n, 0, sizeof n);
    for (i = 0; i < g_eps_n; i++) {
        if (g_eps[i].is_asset) continue;
        DCHECKF(g_eps[i].addr_class >= 0 && g_eps[i].addr_class < EPA_COUNT,
                "an @H record reached the address-class census carrying the class %d, which is none of endpoint.h's "
                "ENDPOINT_ADDRESS_CLASSES — the mint writes this field from `address_class_of`, which returns a "
                "member on every arm, so something else wrote it", g_eps[i].addr_class);
        n[g_eps[i].addr_class]++;
    }
    endpoint_surface_census(&minted, &assets, &emitted, &pre_program);

    /* Every class is emitted including zeroes, for the reach census's reason: an absent `unknown` is an instrument
       that stopped writing the class, while an `unknown` of 0 is the hard bar answering, a refusal to claim the
       capability on this document. */
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
            "the @H surface's per-address-class counts sum to %ld against the %ld rows it emits — the classes "
            "partition the emitted surface over the same `is_asset` skip, so the two walks are counting different "
            "populations", sum, emitted);
    /* The bar is not the sum of this table and the reach table: the two overlap (an undetermined `reply-chunk` row
       is counted by both), so their sum exceeds the surface; a union is per-row membership, which
       `endpoint_razor_hist_json` composes. */
    return json_buf_take(&b);
}

/* The same surface partitioned by the bar itself (endpoint.h has each class's claim and why the field is a floor):
   `endpoint_razor_class_of` is the whole keying, `runtime-only` where the run had not determined the address and
   its unknown was supplied outside this engine, or where the door's bytes are not the served document's, and
   `unproven` otherwise. A fourth walk, not a sum of the two tables: the operands overlap (a row that is `unknown`
   and `EPB_OFF_DOCUMENT` is in each marginal and is one row here), so a sum would be arithmetically wrong. The
   `addr_class` range is re-asserted because a corrupt value would fall through `endpoint_razor_class_of`'s first
   arm into `unproven`, an under-claim published as a floor; `door` is not, because `endpoint_door_bytes` CHECKs it
   in every build and nothing here indexes by it. This is a cross-check against the per-row `razorClass` column,
   never a sum with it: both come off one predicate, read at two instants over a gauge (`epEmitted` falls when an
   asset verdict lands between a census and the emission), so a difference names which records each document
   described. */
char *endpoint_razor_hist_json(void) {
    JsonBuf b = { 0 };
    long n[EPZ_COUNT];
    long minted, assets, emitted, pre_program, sum = 0;
    int c, i;

    memset(n, 0, sizeof n);
    for (i = 0; i < g_eps_n; i++) {
        if (g_eps[i].is_asset) continue;
        DCHECKF(g_eps[i].addr_class >= 0 && g_eps[i].addr_class < EPA_COUNT,
                "an @H record reached the razor census carrying the address class %d, which is none of endpoint.h's "
                "ENDPOINT_ADDRESS_CLASSES — `endpoint_razor_class_of` would read it as `unproven` and under-claim "
                "the published floor", g_eps[i].addr_class);
        n[endpoint_razor_class_of(g_eps[i].door, g_eps[i].addr_class)]++;
    }
    endpoint_surface_census(&minted, &assets, &emitted, &pre_program);

    /* Every class is emitted including zeroes, for the walks above's reason: an absent `runtime-only` is an
       instrument that stopped, and a `runtime-only` of 0 is the product's bar answering, a refusal to claim the
       capability on this document. */
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
            "the @H surface's per-razor-class counts sum to %ld against the %ld rows it emits — the two classes "
            "partition the emitted surface over the same `is_asset` skip, so the walks are counting different "
            "populations", sum, emitted);
    return json_buf_take(&b);
}

/* The fifth walk, over the one fact the bar's `unproven` does not carry (endpoint.h has each member's claim and why
   it is the necessary condition under the bar rather than part of it), separate for `endpoint_reach_hist_json`'s
   reason. This list is ordered and the record's merge is a max over it, so a later diff can break the order
   without going out of range; `EPW_UNASKED` is the identity element the `memset` leaves, so it must be the lowest
   member or a merge could lower a record that had already read something. */
char *endpoint_witness_hist_json(void) {
    JsonBuf b = { 0 };
    long n[EPW_COUNT];
    long minted, assets, emitted, pre_program, sum = 0;
    int c, i;

    DCHECK(EPW_UNASKED == 0,
           "endpoint.h's ENDPOINT_WITNESS_CLASSES does not begin with the member that proves nothing — the record's "
           "merge is a maximum over this order and `memset` leaves a fresh record at 0, so 0 must make no claim");
    memset(n, 0, sizeof n);
    for (i = 0; i < g_eps_n; i++) {
        if (g_eps[i].is_asset) continue;
        DCHECKF(g_eps[i].witness_class >= 0 && g_eps[i].witness_class < EPW_COUNT,
                "an @H record reached the witness census carrying the class %d, which is none of endpoint.h's "
                "ENDPOINT_WITNESS_CLASSES — the mint writes this field from `witness_class_now`, which returns one "
                "of three members, so something else wrote it", g_eps[i].witness_class);
        n[g_eps[i].witness_class]++;
    }
    endpoint_surface_census(&minted, &assets, &emitted, &pre_program);

    /* Every class is emitted including zeroes, for the four walks' reason, and here the two states take opposite
       work: an absent `may-rest-on` is an instrument that stopped writing the field, while a zero is a run on which
       no address was composed by a path that had pinned anything, which makes the bar's `unproven` beside it the
       run having proved nothing and sends the next diff to the solver rather than to the pin. */
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
            "the @H surface's per-witness-class counts sum to %ld against the %ld rows it emits — the three classes "
            "partition the emitted surface over the same `is_asset` skip, so the walks are counting different "
            "populations", sum, emitted);
    return json_buf_take(&b);
}

/* The stage arms the partition is made of, summed once for the identity and written once for the document. */
static long edge_stage_sum(const EndpointEdge *e) {
    long sum = 0;
    int i;

    for (i = 0; i < e->stage_n; i++) sum += e->died[i];
    return sum;
}

/* The one identity about every edge's own teardown, asserted where both terms are in hand; the other relations
   differ between the two machines, and each emitter states its own with its reason. The two sides are raised in the
   two arms of one teardown, so they part exactly when a third way out is added without classifying the state it
   lets go. It survives the deep-fork copy that defeats every other relation here, because a copy gets its own
   teardown, which raises one arm like any other. */
static void edge_partition(const EndpointEdge *e, const char *who, long sum) {
    DCHECKF(sum + e->freed_reached == e->freed,
            "the %s edge's torn-down states do not sum over their two arms (%ld by stage plus %ld whose construction "
            "completed, against %ld freed) — a total that moves without an arm is a third arm added above them",
            who, sum, e->freed_reached, e->freed);
}

/* One arm per stage, every one emitted including zeroes, absent.c's rule for its histograms: a stage of 0 is then
   the positive statement that no construction died there, and extension/bridge.js refuses an empty histogram on
   this census by name. The key is a stage label, another component's declaration, so it goes through the value
   entry rather than `json_buf_key`, and that entry escapes, which these labels need: they quote the standards'
   `"no-cors"`, `init["body"]` and `"fetching req"`, and a raw write would end the JSON string early. */
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
        /* No rows at all, a different document from zeroes and the honest one: a host that installs no fetch runs
           no fetch machine, so there is no population, and an absent count is a different fact from a zero. The
           empty string rather than `{}`, because these are rows spliced into the census above, so the absent form
           is the absence of the rows and of the comma before them. */
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
            "the fetch edge offered this surface more addresses (%ld) than the surface counted asks (%ld) — the "
            "offer is raised just before endpoint_record, which raises the ask total before its gate, so this is an "
            "offer with no record call under it",
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

    /* The cross-check before the absent arm, because the absent arm returns: an offer at this edge's door with its
       send machine never declared is core/xhr recording an endpoint in a session whose own xhr_init did not run,
       which would publish this edge's share of `epAsks` with no census under it, as an absence. */
    DCHECKF(g_xhr_offered == 0 || g_xhr_edge.steps != NULL,
            "the XHR edge offered this surface %ld addresses with no send machine declared to it — xhr_init declares "
            "the machine before any send runs, so these records came from a component this census was not told about",
            g_xhr_offered);
    if (!g_xhr_edge.steps) {
        /* No rows at all, for the fetch edge's absent arm's reason: a host that installs no XMLHttpRequest runs no
           send machine. The empty string, not `{}`, for the splice. */
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
    /* There is no `freed_placed <= placed` here, a fact about this machine: a step state is byte-copied at a deep
       fork and the copy inherits `placed`, so two copies file against one placement, and unlike core/fetch
       `XHR_SEND_DECL` declares no `unforkable`. It is ordinary: §3.5.6's synchronous arm sets `placed` and then
       parks inside its call to the lifecycle machine, which fires `readystatechange` and `progress` at the page's
       own listeners, page code at a depth where the send frame is live and forkable. So the two rows are read as
       two facts rather than subtracted. */
    /* The containment this edge does have is over the offer row, not the placement, which is one machine short of
       the door (endpoint.h): every offer is raised on the line before an endpoint_record call that raises `g_asks`
       before its gate, so this edge's offers are contained in the surface's asks, the slack being every other
       door. */
    DCHECKF(g_xhr_offered <= g_asks,
            "the XHR edge offered this surface more addresses (%ld) than the surface counted asks (%ld) — the offer "
            "is raised just before endpoint_record, which raises the ask total before its gate, so this is an offer "
            "with no record call under it",
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

/* Serialize the @H surface directly to a JSON string in C (caller frees), with core/json_buf.h's writer. */
char *endpoint_json_array(void) {
    JsonBuf b = { 0 };
    int wrote_one = 0;
    json_buf_raw(&b, "[");
    for (int i = 0; i < g_eps_n; i++) {
        Endpoint *e = &g_eps[i];
        /* A resource that turned out to be a static asset is not an endpoint, and this is the only place that can
           act on it, since the verdict arrives after the record. The separator is a `wrote_one` latch, not the
           loop index: an index-keyed comma would emit a leading one when record 0 is skipped, invalid JSON on a
           document delivered in one parse. */
        if (e->is_asset) continue;
        if (wrote_one) json_buf_raw(&b, ",");
        wrote_one = 1;
        json_buf_raw(&b, "{"); json_buf_key(&b, "method"); json_buf_str(&b, e->method);
        json_buf_raw(&b, ","); json_buf_key(&b, "url"); json_buf_str(&b, e->path);
        /* And the body this engine could not name, as bytes plus the type that decides how to read them. A
           consumer reads their presence as the positive statement "this request sends a body this engine has no
           reader for", never as a hole to default past. */
        if (e->body_b64 || e->body_shape || e->body_example_b64) {
            DCHECK((!!e->body_b64 + !!e->body_shape + !!e->body_example_b64) == 1,
                   "one request body was recorded under more than one of the three body claims (sent bytes, an "
                   "unknown's display spelling, an example) — body_store sets exactly one, so this is two sightings "
                   "merged across the grade");
            if (e->body_mime) { json_buf_raw(&b, ","); json_buf_key(&b, "bodyMime"); json_buf_str(&b, e->body_mime); }
            /* Three keys, so the consumer never guesses: `bodyBase64` is the replay claim, `bodyShape` a statement
               about which source composes the body, and `bodyExampleBase64` what the payload looks like at the byte
               ranges `params` names. The third carries its refusal in its spelling, so a consumer skimming for a
               payload to replay cannot mistake it for `bodyBase64`, where a sibling flag would be the field a
               reader defaults past. It is emitted beside the span params: they are the addresses, it the content. */
            if (e->body_b64)   { json_buf_raw(&b, ","); json_buf_key(&b, "bodyBase64"); json_buf_str(&b, e->body_b64); }
            if (e->body_shape) { json_buf_raw(&b, ","); json_buf_key(&b, "bodyShape");  json_buf_str(&b, e->body_shape); }
            if (e->body_example_b64) {
                json_buf_raw(&b, ","); json_buf_key(&b, "bodyExampleBase64"); json_buf_str(&b, e->body_example_b64);
            }
        }
        /* And what this record is evidence of, always, in the three words the pending line spells, through
           solver/engine.h's `engine_provenance_token`, so a zone reading both about one app meets one vocabulary.
           No absence-is-the-statement here, unlike `excludes` and `bounds`: the three words cover every way this
           engine comes to know an address, so a silent grade would be read as `observed`, the strongest. */
        json_buf_raw(&b, ","); json_buf_key(&b, "provenance");
        json_buf_str(&b, engine_provenance_token(e->prov));
        /* …and whose act the sighting was, always: a boolean the zone folds per method (endpoint.h). */
        json_buf_raw(&b, ","); json_buf_key(&b, "driven");
        json_buf_raw(&b, e->driven ? "true" : "false");
        /* And which mechanism composed the address and whether the page's code had run when it did, always, for
         * `provenance`'s reason: `epEmitted - epPreProgram` bounds how many addresses forced execution can have
         * contributed and names none, so a reader could not tell a run that learned ten gated API calls from one
         * that learned ten `<link rel=preload>` elements; carried per row, the surface partitions directly.
         * Neither has an absence-is-the-statement reading: `door` is exhaustive over the ways an address reaches
         * this surface (endpoint.h derives the list from this file's call sites), and `mintedAt` is two words over
         * a boolean, not a boolean, because this record is read by a zone deployed on write while this engine is
         * live only after a build, and `false` and absent are one value under every truthiness test. They are two
         * facts: the door is which mechanism and the mint state is when, and the pair makes either readable. A
         * `<head>` whose first `<script src>` runs before the parser reaches a `<link>` below it mints that link
         * post-program, so the timing fact is a proxy for the markup door, in the flattering direction.
         *
         * Their consumer is extension/bridge.js's run-record composition, which holds this array when it writes
         * the `endpoints` figure that is its length, so each histogram it builds is one population at one moment
         * and is asserted there to sum to it (extension/lib/merge.js's call-site loop would not do: it skips
         * structural @T candidates and `data:`/`blob:`/`about:`/`javascript:` addresses and folds rows onto one
         * `method+host+path` key). That reader buckets an absent key as its own third state rather than asserting
         * presence, because the zone's JavaScript is live on write and these bytes only after a build; the
         * tolerance is the JS half's to drop, and this file writes both keys on every row in every build. */
        json_buf_raw(&b, ","); json_buf_key(&b, "door");
        json_buf_str(&b, endpoint_door_token(e->door));
        json_buf_raw(&b, ","); json_buf_key(&b, "mintedAt");
        json_buf_str(&b, e->pre_program ? "pre-program" : "post-program");
        /* And the third fact about the sighting, which neither of those implies: whether the run had determined the
           address and whose unknown it rests on, the hard bar at its own grain, which no count of doors reaches (a
           literal chunk URL through `module-import` and `/api/{location.hash}` through `fetch` are both `beyond` a
           markup parse, and only the second clears the bar). The whose fact is in this one word rather than a
           column of its own, so a consumer reading `unknown-unproven` beside `unproven` can check the razor's
           answer against what the wire carries (`endpoint_razor_class_of`'s contract). Written on every row in
           every build, so a consumer's `(unstated)` bucket stays a fact about the build (endpoint.h). */
        json_buf_raw(&b, ","); json_buf_key(&b, "addressClass");
        json_buf_str(&b, endpoint_address_class_token(e->addr_class));
        /* And the hard bar as one word, the union of two of the three, so the emitter that states a razor figure
           composes the union itself and a door count cannot be assembled into one. (`endpoint_door_reach(door) ==
           EPR_BEYOND` would be the demoted figure: `fetch`, `xhr`, `beacon` and `module-import` are all `beyond` a
           markup parse, and a JS AST parse reads them off the text without running anything.) Derived, not stored,
           which keeps it honest under its operands' two merge rules (`addr_class` by a max, `door` the first
           sighting's), leaves no third merge rule to get wrong, and lets a consumer check this word against the two
           beside it. Written on every row in every build. */
        json_buf_raw(&b, ","); json_buf_key(&b, "razorClass");
        json_buf_str(&b, endpoint_razor_class_token(endpoint_razor_class_of(e->door, e->addr_class)));
        /* And the one fact the bar does not carry, beside it rather than inside it (endpoint.h's
           ENDPOINT_WITNESS_CLASSES): `razorClass` is a floor and this a may, adjacent so the pair is legible per
           row: `unproven` beside `may-rest-on` is the population the razor's zero does not account for, and
           `unproven` beside `no-witness` the run having proved nothing about this address. Written on every row in
           every build. */
        json_buf_raw(&b, ","); json_buf_key(&b, "witnessClass");
        json_buf_str(&b, endpoint_witness_class_token(e->witness_class));
        json_buf_raw(&b, ","); json_buf_key(&b, "witness");
        witness_json(&b, e);
        /* And the operand the address class was derived from, which none of the four columns above publishes:
         * `addressClass` says the run held an unknown somebody outside this engine supplied, and only this says
         * which component carried the bytes in, so it is the per-row observation a reader checks the whose
         * narrowing against; an `unknown-unproven` row whose root names a component carrying a document's or a
         * server's bytes reopens it. It reads the root, not the address's display shape: `url` carries a composed
         * identity (concolic.h's `root` banner records `{location.hash}.slice()` matching no declared source when
         * asked a delivery question), and whose unknown it is is a fact about the root, while where the literals
         * fall is a fact about the page's syntax.
         *
         * Three JSON types, each a positive statement, because `concolic_root_c`'s NULL is two facts: `null` when
         * the record's class says no sighting held a concolic address; `false` when one did and its bytes entered
         * through nothing this engine minted as a source (concolic.h: "a positive statement, never a hole to
         * fill"); a string naming the sources, possibly a set. The types discriminate, not a token: a root may be
         * composed from a name the page chose (solver/absent.c mints an injected global's source from the property
         * read), so any reserved in-band word could be spelled by a document. It composes no verdict here or in any
         * consumer: the bar reads whose through `concolic_root_whose_any`, never this string's spelling. Written on
         * every row in every build.
         * Named residual. Not covered: the root is that of the sighting whose class the record carries, so where
         * two sightings of one address held the same class and different roots, the second root is recorded
         * nowhere, and the record names one component where some observed path's bytes entered through another.
         * Next diff builds: the union of roots, spelled in solver/concolic.c (where `CONCOLIC_JOINT_SEP` and its one
         * speller `derived_root_join` live) and called from here, never composed here. Absence shows as: this
         * column moving between two runs of one artifact over one document, read per row, since which sighting
         * raises a record's class first depends on the interleaving. */
        json_buf_raw(&b, ","); json_buf_key(&b, "addressRoot");
        if (e->addr_class == EPA_CONCRETE)   json_buf_raw(&b, "null");
        else if (!e->addr_root)              json_buf_raw(&b, "false");
        else                                 json_buf_str(&b, e->addr_root);
        json_buf_raw(&b, ","); json_buf_key(&b, "params"); json_buf_raw(&b, "[");
        for (int j = 0; j < e->np; j++) {
            if (j) json_buf_raw(&b, ",");
            json_buf_raw(&b, "{"); json_buf_key(&b, "name"); json_buf_str(&b, e->params[j].name);
            /* A CHECK, not a DCHECK, because the subscript is on the next line in every build: `ep_loc_name` is a
               three-element table, and an out-of-range read would hand `json_buf_str` a pointer assembled from
               whatever the link placed after it (check.h: the wasm build does not fault on that), emitting a
               plausible location token in the @H record. */
            CHECK(e->params[j].loc == EP_QUERY || e->params[j].loc == EP_PATH || e->params[j].loc == EP_BODY,
                  "engine: an endpoint param carries a location this surface has no name for — the enum and "
                  "its name table are read together at exactly this line, so one grown without the other "
                  "would index outside a three-entry table and emit whatever that address holds as the "
                  "param's location");
            json_buf_raw(&b, ","); json_buf_key(&b, "location"); json_buf_str(&b, ep_loc_name[e->params[j].loc]);
            /* And whether this param's value ever named a hole, always, for `provenance`'s reason: the two words are
               exhaustive over the ways a value reaches this surface. It makes the silences below readable:
               `excludes` absent says no equality gate over this hole took its false arm on every path, which
               presupposes a hole; with no hole, all four domain reads were skipped at the mint and the same absence
               means the param is a literal. Two words, not a boolean, for `mintedAt`'s reason: a consumer will meet
               params from a build older than this key, and lib/learn.js reads the absence as `unstated`
               (extension/lib/safe-fetch.js's word for an act that comes off no line). The words are
               `concolic_hole_key`'s own: it answers NULL where a shape "names no hole at all (a concrete value has
               no domain to look up and must not borrow one)", so `concrete` names exactly the case that gates the
               reads out, and `unknown` is the other. */
            json_buf_raw(&b, ","); json_buf_key(&b, "valueClass");
            json_buf_str(&b, e->params[j].has_hole ? "unknown" : "concrete");
            json_buf_raw(&b, ","); json_buf_key(&b, "validValues"); json_buf_raw(&b, "[");
            for (int k = 0; k < e->params[j].nvals; k++) { if (k) json_buf_raw(&b, ","); json_buf_str(&b, e->params[j].vals[k]); }
            json_buf_raw(&b, "]");
            /* The domain, and only where one was observed: an empty array would be a third state a consumer could
               not tell from an unconstrained param, so the absence is the statement that no equality gate over this
               hole took its false arm on every path that built this request. Without it a param proved to be
               neither "admin" nor "prod" would render as one nothing tested. */
            if (e->params[j].nexcl) {
                json_buf_raw(&b, ","); json_buf_key(&b, "excludes"); json_buf_raw(&b, "[");
                for (int k = 0; k < e->params[j].nexcl; k++) { if (k) json_buf_raw(&b, ","); json_buf_str(&b, e->params[j].excl[k]); }
                json_buf_raw(&b, "]");
            }
            /* And the interval, in the standard's own vocabulary, only where a bound survived every observed path.
               JSON Schema Validation 2020-12 §6.2 Validation Keywords for Numeric Instances (number and integer)
               names all four: §6.2.2 "maximum" ("an inclusive upper limit"), §6.2.3 "exclusiveMaximum" ("strictly
               less than (not equal to)"), §6.2.4 "minimum" ("an inclusive lower limit") and §6.2.5
               "exclusiveMinimum" ("strictly greater than (not equal to)"). In 2020-12 each is a number; the draft-04
               boolean form (`exclusiveMinimum:true` modifying `minimum`) would read as a bound of 1. The number is
               the page's own spelling, carried from the literal the predicate compared against, so a report never
               states a number the bundle does not contain; written unquoted, since these keywords take a number,
               and every spelling here came from §6.1.6.1.20 Number::toString of a finite Number (concolic_rel_hook
               refuses the rest), a JSON number by construction. It stays a shape: nothing picks a member of the
               interval. */
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
            /* And the call predicates, in the engine's own vocabulary and deliberately not JSON Schema's. JSON
               Schema Validation 2020-12 §6.3 Validation Keywords for Strings has one keyword that could carry a
               prefix test, §6.3.3 "pattern" ("The value of this keyword MUST be a string. This string SHOULD be a
               valid regular expression, according to the ECMA-262 regular expression dialect. A string instance is
               considered valid if the regular expression matches the instance successfully. Recall: regular
               expressions are not implicitly anchored."), and it is the wrong shape: it cannot state the false arm,
               which forced multi-path produces as often as the true one; writing `^/api` for `startsWith("/api")`
               means deciding what the method means; and the translation owes an escaping rule nobody checks, since
               the page's literal may hold regex metacharacters. So what is emitted is what was observed: the method
               the page named, its arguments, and the answer the run got; `holds` is the arm, and `false` is a fact
               this flow proved. The absence is the statement, as for `excludes` and `bounds`. It stays a shape:
               nothing picks a string that satisfies the predicate. */
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
                           "a call predicate reached the emission for an arm that is neither taken nor not-taken — "
                           "the arm is the fact this record carries");
                    json_buf_raw(&b, ","); json_buf_key(&b, "holds");
                    json_buf_raw(&b, pr->holds ? "true" : "false");
                    json_buf_raw(&b, "}");
                }
                json_buf_raw(&b, "]");
            }
            /* And the loose equalities that held, the fourth way a gate narrows a domain. Step 1 of §7.2.14
               IsStrictlyEqual ( x, y ) is "If SameType(x, y) is false, return false", so a `===` that held
               determined the value and it appears in `validValues`; §7.2.13 IsLooselyEqual ( x, y ) coerces, so its
               holding arm determines nothing and `concolic_pin` refuses it, and without this key the param would
               render as untested while its sibling flow's `excludes` carries the gate's other arm. Each entry is
               `{"value":<string>,"type":<string>}`: the value is the operand's §7.1.19 ToString ( arg ), which
               flattens `undefined`, `null`, `0` and `false` onto text a String operand could also be, so `type`
               separates `== undefined` (by §7.2.13 steps 2 and 3 a demand for null or undefined, for a query
               parameter a demand that it be absent) from `== "undefined"`; the word is concolic.c's report name for
               the kind. It states the predicate, not the set (the holding set depends on the token's kind, and its
               Object arm runs the page's own ToPrimitive), and invents nothing. The absence is the statement. */
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
        /* The transport half, only when there is one: an empty requirement would read as "needs nothing" rather than
           "nothing observed". Keyed by header name, the shape the popup's Required Headers section reads. */
        if (e->nh) {
            json_buf_raw(&b, ","); json_buf_key(&b, "headers"); json_buf_raw(&b, "{");
            for (int j = 0; j < e->nh; j++) {
                /* The one computed key in this seam, and it is not a field name: a header name is data no producer
                   could declare, so it goes through the value entry, and a reader grepping json_buf_key gets the
                   field contract. */
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
        /* The body fields freed where every other owned field is: a struct copied or freed field by field creates
           an obligation at every such site, which a field added to one and not the other silently breaks. */
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
        for (int j = 0; j < g_eps[i].nwit; j++) witness_free(&g_eps[i].wit[j]);
        free(g_eps[i].wit);
    }
    free(g_eps); g_eps = NULL; g_eps_n = g_eps_cap = 0;
    for (int i = 0; i < g_verified_n; i++) free(g_verified[i]);
    free(g_verified); g_verified = NULL; g_verified_n = g_verified_cap = 0;
    /* And the ask counters beside the array they are a fact about. One process runs many sessions and this is the
       per-session teardown (solver/engine.c resets its own per-session rows on the line that calls this), so a
       standing count would report a previous document's asks under this one's name, in the flattering direction
       (`asks > preProgram` says running code composed a request). The mint arm is reset with `g_eps_n` because the
       identity between them is asserted on every read of the surface. */
    g_asks = g_asks_pre_program = g_ask_suppressed = g_ask_merged = g_ask_minted = 0;
    g_ask_merged_pre_program = 0;
    /* And what the next session would be grading against, taken here, where the scopes part: this resets this
       file's program-state boundary and the engine's stays, the latch is the difference, and the door asserts on
       it at the first ask. */
    g_boundary_spent = engine_any_program_started();
    /* And the host edges' rows, for the same reason: they are read against the ask rows above, so they must share
       their scope; edge_release gives each array back and lets each table go (see it). */
    edge_release(&g_fetch_edge);
    edge_release(&g_xhr_edge);
    g_xhr_offered = 0;
}
