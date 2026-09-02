/* @H endpoint surface — see endpoint.h. Findings are C data; params + values merge in C; emit is C. */
#include "solver/endpoint.h"
#include "core/json_buf.h"
#include "core/mime/mime_type.h"
#include "solver/concolic.h"
#include "solver/engine.h"    /* the provenance's ONE wire spelling — this surface prints the same three words */
#include "solver/flow.h"
#include "solver/pending.h"   /* …and PROV_*, the vocabulary those words spell */
#include "check.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* WHERE THE VALUE LANDED IN THE REQUEST. A reviewer replays a path param by substituting it into the address,
   a query param by appending it and a body param by encoding it into the payload, so this is not a label on a
   param — it is half of what the param IS, and two params of the same name in two places are two params. */
typedef enum { EP_QUERY = 0, EP_PATH, EP_BODY } EpLoc;
static const char *const ep_loc_name[] = { "query", "path", "body" };

/* A PARAM STATES TWO FACTS AND THEY MERGE BY OPPOSITE RULES, which is the whole reason they are two fields.
   `vals` is PROVENANCE-and-example — each entry is a value the code COMPUTED on some path, so the merge is a
   UNION: another path computing another value adds knowledge and takes none away.
   `excl` is DOMAIN — each entry is a token some flow's own equality gate PROVED this hole is not, so the
   merge is an INTERSECTION: the record's claim is about the ENDPOINT, and only a constraint every observed
   path to it obeyed is true of the endpoint. Unioning them would state, of one request, a constraint that no
   single run of the program ever satisfied.
   `bnd` IS THE SAME KIND OF FACT AS `excl` OVER AN ORDERED DOMAIN, AND ITS MERGE IS THE SAME CLAIM SPELLED FOR
   AN INTERVAL: the record may state a bound only where EVERY observed path obeyed it, so a second sighting
   WIDENS to the interval hull rather than narrowing to the overlap. Two paths that reached one endpoint with
   `x >= 5` and `x >= 10` leave `x >= 5` — the weaker bound is the one both obeyed; two that reached it with
   `x >= 5` and `x <= 3` leave NO lower and NO upper bound, because neither claim survives the other path.
   That is exactly what intersecting the exclusion sets does over an unordered domain — the complement of the
   union of the two domains — and it is the same sentence, not a second rule. A sighting that observed NO
   bound on a side therefore ERASES that side: a path allowed to reach the request without obeying the claim
   is a path that disproves it.
   WITHIN ONE FLOW THE OPPOSITE HOLDS AND IT IS NOT A CONTRADICTION: `if (x > 5 && x < 100)` conjoins, because
   both gates are on ONE path. concolic.c owns that half; this file owns the across-paths half. */
/* `pred` IS THE SAME KIND OF FACT AS `excl` AND `bnd` OVER A DOMAIN NEITHER OF THEM CAN STATE — what this
   endpoint's own METHOD-CALL gates proved, `path.startsWith("/api")` being the shape §@H names. An equality
   determines a value, an ordering an interval, and a call neither, so it is a third field for the reason the
   first two are two. ITS MERGE IS THE SAME CLAIM AS THEIRS AND THEREFORE AN INTERSECTION: a predicate belongs
   on the record only where EVERY observed path obeyed it, so a sighting that reached the request without
   testing it is a path that DISPROVES it and the claim goes. There is no hull here and no widening — the
   domain is unordered, so intersecting the sets IS the rule, exactly as it is for `excl`. */
typedef struct { int has_lo, has_hi; double lo, hi; int lo_incl, hi_incl; char *lo_txt, *hi_txt; } ParamBound;
typedef struct { char *name; EpLoc loc; char **vals; int nvals, vcap;
                 char **excl; int nexcl; ParamBound bnd;
                 ConcolicPred *pred; int npred;
                 ConcolicLooseEq *leq; int nleq; } Param;
typedef struct { char *name; char *value; } EpHeader;   /* the transport half: what the request must carry */
/* `is_asset` IS WHAT THE RESOURCE AT THIS ADDRESS TURNED OUT TO BE, and it can only be written after the
   record exists. §Attacker sources: "Static assets are NEVER endpoints (magic-byte + content-type, not URL
   suffix) but still drive the code path" — the parenthetical is a rule about BYTES, and bytes arrive on a
   reply, while a request is recorded before its policy check for the reason endpoint_record's own call sites
   state (a request a policy refuses is still a request the bundle can make). So the two facts are learned at
   two times and the flag is the join between them; there is no request-time test that could stand in for it
   without either guessing from a suffix or losing every blocked request. */
/* `prov` IS WHAT THIS RECORD IS EVIDENCE OF and it is part of the record's IDENTITY rather than a label on it
   — see endpoint.h for why the pending line's most-observed fold is right there and wrong here. The one
   consequence to keep in view while reading the merges below: every value, exclusion, bound and predicate on
   a record was observed at ONE grade, because a sighting at another grade cannot reach it. */
typedef struct { char *method; char *path; Param *params; int np, pcap;
                 EpHeader *hdrs; int nh, hcap; int is_asset; int prov; } Endpoint;

/* A value carrying a `{hole}` is a SHAPE — an unknown the code did not compute — and a hole-free one is the
   real thing. The distinction decides the merge: a concrete value supersedes a shape for the same header, which
   is exactly what a param's example values already do. */
static int header_is_shape(const char *v) { return strchr(v, '{') != NULL; }

/* Returns HOW MANY HEADER NAMES THIS ENDPOINT DID NOT HAVE BEFORE — the caller credits the WFQ with it, and
   that is the whole reason this is not `void` any more. See the merge site. */
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
static int g_suppress = 0;   /* a candidate/verify re-run's requests are @S artifacts, NOT real @H */

void endpoint_init(void) { g_eps = NULL; g_eps_n = 0; g_eps_cap = 0; g_suppress = 0; }
void endpoint_suppress(int on) { g_suppress = on ? 1 : 0; }

/* THE ADDRESS AS THE SURFACE PRINTS IT — a concolic's SHAPE, a concrete URL's own bytes.
   THE `s ? s : "{}"` THAT STOOD HERE WAS A MASK OVER AN IMPOSSIBLE STATE, and it is the exact shape §Fix-the-
   ROOT forbids: concolic_alloc copies a shape for every value it mints and CHECKs the copy, so a live concolic
   ALWAYS has one and this branch could never run. What it would have done if it ever did is the reason it must
   not be a `?:` — `{}` is the unnameable hole path_scan mints nothing for, so a URL that had lost its display
   form would be emitted as a literal address with no param under it and read as a perfectly ordinary endpoint.
   That is the silent wrong answer this surface is most dangerous for, so it aborts at the read instead. */
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

/* THE PARAMS OF ONE OBSERVED REQUEST, in the order a reviewer meets them: path, then query, then body. Owned
   until they are merged into the surface. */
/* `excl`/`nexcl` are OWNED COPIES of what the running flow's path constraint held at the moment this param
   was read. They are copied here rather than borrowed because the constraint is a growable head that a later
   narrowing REALLOCS, and a borrowed row would then name freed memory — a lifetime rule this file would have
   to keep true across every future edit to the two producers below. Copying makes it unbreakable. */
/* …and `bnd` for the same reason, one step further: concolic_bound_read hands back BORROWED spellings that
   live in the flow's constraint head, which the very next narrowing reallocs. */
/* …and `pred` for the reason both of those are copied: concolic_strpred_read hands back a BORROWED row that
   lives in the flow's constraint head, which the very next narrowing reallocs. */
/* …and `leq` for the same reason a third time: concolic_looseeq_read hands back a BORROWED row out of that
   same head. */
typedef struct { char *name; char *val; EpLoc loc; char **excl; int nexcl; ParamBound bnd;
                 ConcolicPred *pred; int npred;
                 ConcolicLooseEq *leq; int nleq; } KV;
typedef struct { KV *e; int n, cap; } KvBuf;

/* ONE COPY AND ONE DISPOSER FOR A SET OF THEM. What a single ROW owns is concolic.c's to say
   (concolic_pred_copy / concolic_pred_release) and is never re-spelled here — a field added to ConcolicPred
   must have exactly one place it is copied and one where it is released, or one of the four holders silently
   keeps half a fact. What THIS file owns is the ARRAY around the rows, which is the part that differs between
   a holder and the constraint. */
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

/* THE SAME PAIR OVER THE LOOSE-EQUALITY ROW, and here for the reason the two above are here: what a single ROW
   owns is concolic.c's to say (concolic_looseeq_copy / concolic_looseeq_release) and is never re-spelled here;
   what THIS file owns is the ARRAY around the rows. */
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

/* WHICH TWO LOOSE EQUALITIES ARE ONE is asked at both ends — concolic.c dedups a repeat within a flow, this
   file intersects across sightings — so it is asked of ONE speller (concolic_looseeq_same), for the reason
   param_pred_same states of its own. */
static int param_leq_same(const ConcolicLooseEq *a, const ConcolicLooseEq *b) {
    return concolic_looseeq_same(a, (ConcolicLit)b->kind, b->tok);
}

/* WHICH TWO PREDICATES ARE ONE is asked at both ends — concolic.c dedups a repeat within a flow, this file
   intersects across sightings — so it is asked of ONE speller (concolic_pred_same) and never re-derived here.
   Two spellings of it are two rules free to disagree, and the direction they would disagree in is the one
   that matters: an intersection that thought two identical predicates were different would drop, from the
   record, a claim every observed path obeyed. */
static int param_pred_same(const ConcolicPred *a, const ConcolicPred *b) {
    return concolic_pred_same(a, b->method, (const char *const *)b->args, b->nargs, b->holds);
}

/* `hole` is the value's HOLE KEY — the name the flow's own equality gates recorded their domain under — or
   NULL when this param's value is a concrete one the code computed and there is no domain to look up. It is
   passed EXPLICITLY by each producer rather than derived here, because the two producers know it from
   different places: a path param IS its hole (the name is the brace-stripped segment), while a query or body
   param carries its hole inside the VALUE text. Deriving it from one of the two would silently answer NULL
   for the other, and a NULL hole is indistinguishable from a hole with no constraint on it. */
static void kv_add(KvBuf *b, const char *name, size_t nlen, const char *val, size_t vlen, EpLoc loc,
                   const char *hole) {
    DCHECK(nlen > 0, "an endpoint param was minted with no NAME — a nameless param cannot be substituted back "
                     "into a request, so every producer here must refuse the unnamed case at its own site "
                     "rather than emit a record keyed by the empty string");
    /* WHERE IT LANDED IS ASSERTED AT THE MINT, not read back later. A param whose location is not one of the
       three is a producer that learned to build a param and not to say what it is, and the consumer's own
       `p.location || "query"` is what made that unfalsifiable for the whole life of this surface: the path
       and body branches never ran once, and read as live the entire time. Every site that adds a param
       passes through here, so there is no way to add one without answering. */
    DCHECK(loc == EP_QUERY || loc == EP_PATH || loc == EP_BODY,
           "an endpoint param was minted with no LOCATION — a param the reviewer cannot place is a param "
           "that cannot be replayed, and a consumer defaulting it reads every one of them as a query param");
    if (b->n >= b->cap) { b->cap = b->cap ? b->cap * 2 : 8; b->e = realloc(b->e, (size_t)b->cap * sizeof(KV)); CHECK(b->e, "endpoint: OOM params"); }
    b->e[b->n].name = malloc(nlen + 1); CHECK(b->e[b->n].name, "endpoint: OOM param name");
    memcpy(b->e[b->n].name, name, nlen); b->e[b->n].name[nlen] = 0;
    b->e[b->n].val = malloc(vlen + 1); CHECK(b->e[b->n].val, "endpoint: OOM param value");
    if (vlen) memcpy(b->e[b->n].val, val, vlen);
    b->e[b->n].val[vlen] = 0;
    b->e[b->n].loc = loc;
    /* THE DOMAIN IS READ HERE BECAUSE HERE IS WHERE THE FLOW STILL EXISTS. The path constraint is per-flow and
       the serializer runs at the end of the run with no flow under it, so a domain fetched there would be
       whichever flow happened to park last — or none. */
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
    /* …AND THE INTERVAL, off the SAME flow at the SAME instant, for the same reason. A param whose gate was
       `x > 5` and one nothing ordered at all render with identical bytes without this, and that silence is
       read as the positive statement "anything goes" — §Solver-half calls it a wrong report, not a partial
       one, and it is the second-largest gate class a real minified bundle contains. */
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
    /* …AND THE CALL PREDICATES, off the SAME flow at the SAME instant, for the same reason again. A param
       whose only gate was `path.startsWith("/api")` and one nothing ever tested render with identical bytes
       without this, and §@H calls that a WRONG report rather than a thin one — the silence about the gate is
       read as the positive statement "anything goes". It is also the gate class §@H names in its own headline
       example, and the one an equality's pin and an ordering's interval both structurally miss. */
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
    /* …AND THE LOOSE EQUALITIES THAT HELD, off the SAME flow at the SAME instant, for the same reason a fourth
       time. It is the arm §7.2.13 IsLooselyEqual ( x, y ) leaves undetermined and concolic_pin therefore
       refuses to pin: a param whose only gate was `x == 0` and one nothing ever tested render with identical
       bytes without this, while that param's own SIBLING flow carries an exclusion — the two arms of one
       observation disagreeing about whether a gate was seen at all. */
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

/* ONE DISPOSER FOR ONE INTERVAL, because there are two places that hold one (a request's KV row and the
   endpoint's merged Param) and a side added to ParamBound must be freed at both or one of them leaks. */
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

/* `a=1&b=2` — the QUERY STRING's grammar, and `application/x-www-form-urlencoded`'s, which are the same
   grammar. Nothing is percent-DECODED: §Solver-half puts the codecs in the engine's builtins and forbids the
   solver hand-rolling one, and a value the page encoded is the value the page sends. */
static void kv_pairs(KvBuf *b, const char *text, EpLoc loc) {
    char *dup = strdup(text); CHECK(dup, "endpoint: OOM pair list");
    for (char *tok = strtok(dup, "&"); tok; tok = strtok(NULL, "&")) {
        char *eq = strchr(tok, '=');
        const char *name = tok, *val = "";
        if (eq) { *eq = 0; val = eq + 1; }
        if (!name[0]) continue;   /* `&&` / a leading `=`: no name, so no param — see kv_add's assert */
        {   /* the VALUE carries the hole here: `page={state.page}` is one unknown wearing a known name */
            char *hole = concolic_hole_key(val);
            kv_add(b, name, strlen(name), val, strlen(val), loc, hole);
            free(hole);
        }
    }
    free(dup);
}

/* The path half of a display URL (everything before `?`), malloc'd. */
static char *url_path_of(const char *disp) {
    const char *q = strchr(disp, '?');
    size_t plen = q ? (size_t)(q - disp) : strlen(disp);
    char *path = malloc(plen + 1); CHECK(path, "endpoint: OOM path"); memcpy(path, disp, plen); path[plen] = 0;
    return path;
}

/* THE CONCRETE URL THE CODE COMPUTED, beside the SHAPE that names its holes — §Solver-half's third member of
   the triple. `url_display` above answers with the shape and threw this away, which is what left every
   templated path a row of holes with no value under any of them. NULL means there is none to align against:
   a concrete URL is its own address and has no holes, and a concolic one that has not computed an example yet
   is honestly example-free. */
static char *url_example(JSContext *ctx, JSValueConst url) {
    JSValue ex;
    const char *s;
    char *r;

    if (!concolic_is(url)) return NULL;
    ex = concolic_example(ctx, url);
    if (JS_IsUndefined(ex)) { JS_FreeValue(ctx, ex); return NULL; }
    /* The example is the ADDRESS the interpreter computed, so it is a primitive that converts. An object is
       the `+` propagation having handed on a wrapper, and a symbol would THROW inside this conversion and
       leave the exception on a context that has nobody to hand it to — both are this engine's own logic
       being wrong, which is what a DCHECK asserts and why the conversion below cannot fail except on OOM. */
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

/* DOES THE EXAMPLE'S PATH LINE UP WITH THE SHAPE'S, SEGMENT BY SEGMENT? The alignment is what makes a hole's
   value a MEASUREMENT rather than a guess, so it is checked and not assumed: the two must have the same
   segment count and every hole-free segment must be byte-equal. A hole whose value contained a `/` breaks
   both, and then nothing is aligned and no path param carries an example — which is the honest answer, since
   this layer cannot know which segments that value spanned. */
static int path_aligned(const char *shape, const char *ex) {
    const char *a = shape, *b = ex;
    for (;;) {
        const char *ae = strchr(a, '/'), *be = strchr(b, '/');
        size_t an = ae ? (size_t)(ae - a) : strlen(a);
        size_t bn = be ? (size_t)(be - b) : strlen(b);
        if (!memchr(a, '{', an) && (an != bn || memcmp(a, b, an))) return 0;
        if (!ae != !be) return 0;
        if (!ae) return 1;
        a = ae + 1; b = be + 1;
    }
}

/* AN UNKNOWN PATH SEGMENT IS ONE PATH PARAMETER, AND IT IS ATOMIC. `/v1/users/{state}.id/posts` has one, and
   its value is the whole segment the example holds there — `42`.
   THE BRACES DO NOT DELIMIT THE UNKNOWN, which is the thing that makes this segment-wide rather than
   brace-wide, and reading `concolic_exotic_get` is what says so: a member read composes its display as
   `"%s.%s"` over the parent's, so `state.id` displays as `{state}.id` with the braces around the ROOT SOURCE
   and the member path trailing OUTSIDE them. Treating `}` as the end of the unknown therefore asks the example
   segment to end in a literal `.id` that is not in the address at all, and every such hole loses its value.
   SO THE SEGMENT IS RE-SPELLED AS ONE HOLE — braces stripped from the shape, one pair put around the whole —
   because the consumer's substitution grammar is `/\{([^}\/]+)\}/` (lib/popup-form.js's applyPathParams) and a
   name it cannot match is a value the reviewer can fill and the request can never carry. `{state}.id` becomes
   `{state.id}`: the same bytes, one brace moved, and now substitutable. A literal prefix inside the segment
   (`u{state}.id`) is folded into the hole rather than split off, because where the literal ends is not
   something this layer can see and the example carries the whole segment either way.
   AN UNNAMEABLE HOLE (`{}`, what a concolic with no shape displays as) MINTS NOTHING and keeps its shape in
   the address — lib/learn.js's own reconcile refuses the same segment for the same reason.
   Returns the re-spelled path, malloc'd. */
static char *path_scan(KvBuf *out, const char *shape, const char *ex) {
    int aligned = ex && path_aligned(shape, ex);
    const char *a = shape, *b = ex;
    /* core/json_buf.h's growing byte buffer, appended raw — a fourth private one in this file is the copy its
       own header says it deleted twice. Nothing here is JSON: the emit quotes this string later. */
    JsonBuf p = { 0 };

    for (;;) {
        const char *ae = strchr(a, '/');
        const char *be = NULL;
        size_t an = ae ? (size_t)(ae - a) : strlen(a);
        size_t bn = 0, i, nlen = 0;
        char *seg, *name;

        if (aligned) { be = strchr(b, '/'); bn = be ? (size_t)(be - b) : strlen(b); }
        seg = malloc(an + 1); CHECK(seg, "endpoint: OOM copying a path segment");
        memcpy(seg, a, an); seg[an] = 0;
        name = malloc(an + 1); CHECK(name, "endpoint: OOM naming a path segment");
        for (i = 0; i < an; i++) if (seg[i] != '{' && seg[i] != '}') name[nlen++] = seg[i];
        name[nlen] = 0;
        if (!strchr(seg, '{') || !nlen) {
            json_buf_raw(&p, seg);   /* a literal segment, or a `{}` this surface cannot name */
        } else {
            /* THE GRAMMAR THE CONSUMER SUBSTITUTES BY, ASSERTED AT THE MINT. Both bytes are stripped above and
               a segment cannot hold a `/`, so this holds by construction — which is exactly why it is worth
               asserting: the next producer of a name has to keep it true. */
            DCHECK(!strpbrk(name, "{}/"),
                   "a path param's NAME still holds a brace or a slash — the popup substitutes a hole by "
                   "matching /\\{([^}/]+)\\}/ against this path, so a name outside that grammar names a hole "
                   "no substitution can find");
            json_buf_raw(&p, "{"); json_buf_raw(&p, name); json_buf_raw(&p, "}");
            /* THE NAME *IS* THE HOLE KEY on this path — both are the segment with every brace stripped, which
               is concolic_hole_key's own rule, so a path param looks its domain up by the same string the
               popup substitutes it by. The VALUE here is the concrete example aligned out of the URL and
               carries no braces to read a hole out of. */
            kv_add(out, name, nlen, aligned ? b : "", aligned ? bn : 0, EP_PATH, name);
        }
        free(seg); free(name);
        if (!ae) break;
        json_buf_raw(&p, "/");
        a = ae + 1;
        if (aligned) b = be + 1;
    }
    return json_buf_take(&p);
}

/* THE VALUE OF ONE BODY FIELD, as the same STRING vocabulary every other value on this surface speaks: the
   literal the code computed, or its `{shape}` where the code did not compute one. The parsed value came out
   of JS_ParseJSON, so it holds no getter and no `toString` of the page's — which is why this conversion
   cannot run the page's code and cannot throw. A field whose value is an OBJECT or an ARRAY is recorded by
   NAME with no value: it is a real field of the request, and a nested document is not something a flat
   name -> string record can carry without inventing a naming convention for its members. */
static char *body_field_text(JSContext *ctx, JSValueConst v) {
    const char *s;
    char *r;

    if (JS_IsObject(v)) return NULL;
    s = JS_ToCString(ctx, v);
    /* Every value JS_ParseJSON produces is a primitive or a plain object, and the objects left above, so this
       conversion runs no page code and can only fail on allocation — which is why it is a CHECK and not a
       DCHECK: a compiled-out assert here would hand `strdup` a NULL in release. */
    CHECK(s, "endpoint: OOM rendering a JSON request body's field value");
    r = strdup(s);
    CHECK(r, "endpoint: OOM copying a JSON request body's field value");
    JS_FreeCString(ctx, s);
    return r;
}

/* THE REQUEST BODY, READ BACK IN ITS OWN FORMAT. §What-the-tool-produces wants the KEYS AND VALUES a call
   sends, and half of a POST's are in its payload; without this the surface reported the address of a
   `POST /v1/users` and nothing whatever about what it posts.
   The format is decided by the request's own content-type and never by sniffing the bytes: a page that sends
   JSON says so, and a body this engine has no reader for records no fields rather than a guess at some. */
static void body_params(JSContext *ctx, KvBuf *out, const EndpointBody *body) {
    MimeType mt;
    char *text;

    if (!body || !body->bytes) return;
    /* §4.4 "parse a MIME type" is the ONE reader of a Content-Type in this engine — never a `strcasecmp`
       against a literal, which is the C locale's answer where the standard's is ASCII's, and never a private
       essence split beside the record that already has one. A type that will not parse is §4.4's failure and
       reads no fields. */
    mime_type_init(&mt);
    if (!body->mime || !mime_type_parse(&mt, body->mime, strlen(body->mime))) { mime_type_free(&mt); return; }

    text = malloc(body->len + 1); CHECK(text, "endpoint: OOM request body");
    memcpy(text, body->bytes, body->len); text[body->len] = 0;

    /* `text/plain;charset=UTF-8` IS AN UNDECLARED BODY, NOT A BODY DECLARED AS TEXT, and reading it as one is
       what makes this capability fire on real code rather than on fixtures that spell the header out. Fetch
       §5.2 "BodyInit unions" gives a USVString body exactly that type, so `fetch(u, {method:"POST", body:
       JSON.stringify(x)})` — the commonest POST in any bundle — arrives carrying a type the PAGE never chose.
       So its bytes go through the real JSON parser and either are a name -> value document or are not; a
       plain-text body that is not one simply fails to parse and records nothing, which is the same answer as
       before. This is not sniffing: nothing branches on a
       PATTERN in the bytes, the engine runs the real codec and uses what it returns (§A JS-engine encoding
       builtin is modeled faithfully). A type the page DID declare is still the only thing consulted for it. */
    if (mime_type_is_json(&mt) || (mt.type && mt.subtype && !strcmp(mt.type, "text") && !strcmp(mt.subtype, "plain"))) {
        JSValue v = JS_ParseJSON(ctx, text, body->len, "<@H request body>");
        /* A PAGE MAY SEND BYTES THAT ARE NOT THE JSON ITS HEADER CLAIMS, and that is the page's fact rather
           than this engine's invariant (§offensive-programming exempts a flow throwing on page/attacker
           input). The exception is discharged here because it belongs to nobody downstream: no field is
           recorded, which is the true statement about a body whose fields could not be read. */
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
        kv_pairs(out, text, EP_BODY);
    }
    free(text);
    mime_type_free(&mt);
}

/* THE DOMAIN'S FIRST OBSERVATION — this endpoint's record takes the set the recording flow proved. */
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

/* …AND EVERY LATER ONE NARROWS IT. A constraint the record still carries is one EVERY observed path to this
   endpoint obeyed; a token this flow did not exclude is a token some path allowed, and the claim goes. The
   set only ever shrinks, which is also why a merge earns the WFQ nothing: there is no later event here that
   ADDS structure, and the endpoint's own first sighting already earned its point. */
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

/* THE INTERVAL'S FIRST OBSERVATION — this endpoint's record takes what the recording flow proved. The strings
   are re-copied rather than moved because the KV row that carries them is freed by kv_free whether it was
   merged or seeded, and one owner per string is the only rule that survives a new caller. */
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

/* …AND EVERY LATER ONE WIDENS IT TO THE INTERVAL HULL. A bound the record still carries is one EVERY observed
   path to this endpoint obeyed, so the surviving lower bound is the LOOSER of the two (and no lower bound at
   all if this path had none), and likewise above. It is `param_intersect_excl`'s claim over an ordered domain
   and not a second rule: intersecting exclusion sets is the complement of the UNION of the two paths' domains,
   and the hull is the strongest interval containing that union.
   ON A TIE AT THE SAME NUMBER THE INCLUSIVE SIDE WINS, which is the mirror of concolic.c's within-a-flow rule
   where the EXCLUSIVE one does: there the two facts are conjoined and the tighter survives; here they are
   hulled and the looser does. */
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

/* THE CALL PREDICATES' FIRST OBSERVATION — this endpoint's record takes the set the recording flow proved. */
static void param_set_pred(Param *p, const ConcolicPred *src, int n) {
    DCHECK(p->npred == 0 && p->pred == NULL,
           "an endpoint param's call-predicate set was seeded twice — the first sighting takes the set and "
           "every later one INTERSECTS it, so a second seed would replace a constraint that had already been "
           "narrowed by another path and state, of this endpoint, something no run of the program obeyed");
    if (n <= 0) return;
    p->pred = param_pred_copy(src, n);
    p->npred = n;
}

/* …AND EVERY LATER ONE NARROWS IT — `param_intersect_excl`'s claim over a set of predicates instead of a set
   of tokens, and the SAME rule rather than a second one. A predicate the record still carries is one EVERY
   observed path to this endpoint obeyed; one this flow did not test is one some path did not obey, and the
   claim goes. There is no hull because there is no order: `startsWith("/api")` and `startsWith("/admin")` do
   not weaken to a common predicate, and inventing one that covered both would mean deciding what the method
   MEANS — the recogniser §RUN-DON'T-MATCH forbids, arriving inside a merge rule. */
static void param_intersect_pred(Param *p, const ConcolicPred *src, int n) {
    int i, j, k = 0;
    for (i = 0; i < p->npred; i++) {
        int keep = 0;
        for (j = 0; j < n; j++) if (param_pred_same(&p->pred[i], &src[j])) { keep = 1; break; }
        if (keep) p->pred[k++] = p->pred[i];
        else concolic_pred_release(&p->pred[i]);   /* the ROW's strings; the array is this param's */
    }
    p->npred = k;
}

/* THE LOOSE EQUALITIES' FIRST OBSERVATION — this endpoint's record takes the set the recording flow proved. */
static void param_set_leq(Param *p, const ConcolicLooseEq *src, int n) {
    DCHECK(p->nleq == 0 && p->leq == NULL,
           "an endpoint param's loose-equality set was seeded twice — the first sighting takes the set and "
           "every later one INTERSECTS it, so a second seed would replace a constraint that had already been "
           "narrowed by another path and state, of this endpoint, something no run of the program obeyed");
    if (n <= 0) return;
    p->leq = param_leq_copy(src, n);
    p->nleq = n;
}

/* …AND EVERY LATER ONE NARROWS IT — `param_intersect_excl`'s claim over a set of loose equalities instead of a
   set of excluded tokens, and the SAME rule rather than a second one. A claim the record still carries is one
   EVERY observed path to this endpoint obeyed; a gate this flow did not hold is one some path reached the
   request without, and the claim goes. There is no hull because there is no order: `== 0` and `== ""` do not
   weaken to a common claim, and inventing one that covered both would mean computing the union of two of
   §7.2.13 IsLooselyEqual ( x, y )'s holding sets — which is deciding what `==` MEANS, in a merge rule, over an
   Object arm that runs the page's own ToPrimitive and is not running here. */
static void param_intersect_leq(Param *p, const ConcolicLooseEq *src, int n) {
    int i, j, k = 0;
    for (i = 0; i < p->nleq; i++) {
        int keep = 0;
        for (j = 0; j < n; j++) if (param_leq_same(&p->leq[i], &src[j])) { keep = 1; break; }
        if (keep) p->leq[k++] = p->leq[i];
        else concolic_looseeq_release(&p->leq[i]);   /* the ROW's string; the array is this param's */
    }
    p->nleq = k;
}

static void param_add_val(Param *p, const char *v) {   /* merge a validValue (dedup, skip empty) */
    if (!v || !v[0]) return;
    for (int i = 0; i < p->nvals; i++) if (!strcmp(p->vals[i], v)) return;
    if (p->nvals >= p->vcap) { p->vcap = p->vcap ? p->vcap * 2 : 4; p->vals = realloc(p->vals, (size_t)p->vcap * sizeof(char *)); CHECK(p->vals, "endpoint: OOM vals"); }
    p->vals[p->nvals++] = strdup(v);
}

/* `origin_of_path` AND `origin_on_surface` STOOD HERE AND THEIR ONE CONSUMER IS GONE. Between them they
   answered "is this the FIRST endpoint this surface has held on that host", which was the event that seeded
   the engine's own discovery probes — one flow per candidate document address. Active discovery is the trusted
   zone's again (extension/lib/discovery-probe.js), which asks the same question of the API keys and page
   credentials it holds and this engine does not, so nothing here asks it. They are deleted rather than kept:
   a pair of functions computing an answer nobody reads is indistinguishable from a live capability. */

/* an endpoint's IDENTITY is (method, path, provenance, param-set) — same identity merges param values. A
   param's LOCATION is part of it: an `id` the code puts in the path and an `id` it puts in the body are two
   different things to send, so two requests that agree only on the names are not one endpoint.
   AND THE PROVENANCE IS PART OF IT, which is what makes CLAUDE.md §A-REQUEST-CARRIES-THE-PROVENANCE's "never
   merged into the observed pool" a property of this structure rather than a rule somebody has to keep. A
   FORCED sighting cannot reach a record built from derived ones, so no value learned only because a gate was
   forced can be published under the claim that the app's own code computes it. The alternative — one record
   whose grade folds — is the same defect a `||` is: nothing crashes, the row looks ordinary, and the reviewer
   reads a fabricated example as a measured one. See endpoint.h for why the pending line folds and this does
   not. */
static int same_identity(Endpoint *e, const char *method, const char *path, int prov, const KvBuf *kv) {
    if (e->prov != prov || strcmp(e->method, method) || strcmp(e->path, path) || e->np != kv->n) return 0;
    for (int i = 0; i < kv->n; i++)
        if (e->params[i].loc != kv->e[i].loc || strcmp(e->params[i].name, kv->e[i].name)) return 0;
    return 1;
}

void endpoint_record(JSContext *ctx, const char *method, JSValueConst url,
                     const EndpointHeader *hdrs, int nhdrs, const EndpointBody *body, int prov) {
    /* WHAT THIS SIGHTING IS EVIDENCE OF, ASSERTED AT THE MINT for `kv_add`'s reason exactly: every site that
       records an endpoint passes through here, so there is no way to add one without answering, and the
       answer cannot be defaulted later by a consumer. The value outside the vocabulary is the dangerous one
       and not the missing one — `0` is `observed`, so an uninitialised grade reads as the strongest claim
       this surface makes. */
    DCHECKF(prov == PROV_OBSERVED || prov == PROV_DERIVED || prov == PROV_FORCED,
            "an endpoint was recorded with the provenance %d, which is none of the three solver/pending.h "
            "defines — the grade is part of this record's identity and is emitted on it, so an unrecognised "
            "one both merges with nothing and publishes a word no consumer can read. method=%s", prov, method);
    if (g_suppress) return;   /* candidate/verify run -> not a real @H endpoint */
    char *disp = url_display(ctx, url);
    char *ex = url_example(ctx, url);
    char *shape_path = url_path_of(disp);
    char *expath = ex ? url_path_of(ex) : NULL;
    /* PATH, THEN QUERY, THEN BODY — the order a request is written in, and the order the identity above
       compares in. The path is re-spelled by the scan (see path_scan) and it is the re-spelled one that
       becomes the endpoint's `url`, because the params are named in ITS grammar and a record whose holes and
       whose param names disagree is one nothing can replay. The example is aligned against the path only; the
       query and the body carry the values the display and the bytes already hold. */
    KvBuf kvb = { 0 };
    char *path = path_scan(&kvb, shape_path, expath);
    { const char *q = strchr(disp, '?'); if (q && q[1]) kv_pairs(&kvb, q + 1, EP_QUERY); }
    body_params(ctx, &kvb, body);
    free(disp); free(ex); free(expath); free(shape_path);

    for (int i = 0; i < g_eps_n; i++) {                 /* merge into an existing same-identity endpoint */
        if (same_identity(&g_eps[i], method, path, prov, &kvb)) {
            for (int j = 0; j < kvb.n; j++) {
                param_add_val(&g_eps[i].params[j], kvb.e[j].val);
                param_intersect_excl(&g_eps[i].params[j], kvb.e[j].excl, kvb.e[j].nexcl);
                param_widen_bound(&g_eps[i].params[j].bnd, &kvb.e[j].bnd);
                param_intersect_pred(&g_eps[i].params[j], kvb.e[j].pred, kvb.e[j].npred);
                param_intersect_leq(&g_eps[i].params[j], kvb.e[j].leq, kvb.e[j].nleq);
            }
            /* A REQUIRED HEADER THIS ENDPOINT DID NOT HAVE IS EMITTED OUTPUT, and this path credited the WFQ
               with nothing for it. An endpoint's IDENTITY is method + path + param names AND locations
               (same_identity), so a
               flow that builds a differently-SHAPED request already earns its point below — what reached here
               and went uncounted was a flow that called a known endpoint and revealed that it also wants
               `Authorization`, or a content type, or an API key. §What-the-tool-produces names required headers
               as one of the things this engine exists to learn, so a flow that learns one has emitted, and the
               ranking has to see it or the arm that found the authenticated call sinks back among arms that
               found nothing.
               STRUCTURE, NOT DATA — the line this credit stops at, and the reason `param_add_val` above earns
               nothing. A header NAME is a fact about what the endpoint requires, bounded by the code; a param
               VALUE is a better example of something already known, and it is unbounded in the INPUT, so a loop
               over opaque data could mint credit without limit and outrank the whole frontier by generating
               strings. One point per merge that gained structure, matching the granularity below: an endpoint
               is one discovery however many headers arrive with it, and a header the surface gains later is
               one more. */
            if (endpoint_merge_headers(&g_eps[i], hdrs, nhdrs) > 0)
                flow_credit_emit(1.0);
            goto done;
        }
    }
    if (g_eps_n >= g_eps_cap) { g_eps_cap = g_eps_cap ? g_eps_cap * 2 : 16; g_eps = realloc(g_eps, (size_t)g_eps_cap * sizeof(Endpoint)); CHECK(g_eps, "endpoint: OOM surface"); }
    Endpoint *e = &g_eps[g_eps_n++];
    memset(e, 0, sizeof *e);
    e->method = strdup(method); e->path = strdup(path); e->prov = prov;
    if (kvb.n) { e->params = calloc((size_t)kvb.n, sizeof(Param)); CHECK(e->params, "endpoint: OOM params"); }
    for (int j = 0; j < kvb.n; j++) {
        e->params[e->np].name = strdup(kvb.e[j].name);
        e->params[e->np].loc = kvb.e[j].loc;
        param_add_val(&e->params[e->np], kvb.e[j].val);
        param_set_excl(&e->params[e->np], kvb.e[j].excl, kvb.e[j].nexcl);
        param_set_bound(&e->params[e->np].bnd, &kvb.e[j].bnd);
        param_set_pred(&e->params[e->np], kvb.e[j].pred, kvb.e[j].npred);
        param_set_leq(&e->params[e->np], kvb.e[j].leq, kvb.e[j].nleq);
        e->np++;
    }
    /* The count is deliberately DROPPED here: every header of a brand-new endpoint is new, and the discovery
       being credited is the ENDPOINT. Crediting both would price one sighting at one point plus one per header
       it happened to carry, which makes a request's header count part of the ranking. */
    (void)endpoint_merge_headers(e, hdrs, nhdrs);
    flow_credit_emit(1.0);   /* a NEW endpoint: this flow just emitted value-of-information -> WFQ reward */
done:
    free(path);
    kv_free(&kvb);
}

/* THE READER THE ASSET DECISION DID NOT HAVE. solver/reply_decode.c has always ASKED what a reply is — of
   `computedType`, the one type decision, taken by the zone that read the bytes — and its only consumer was its
   own early return, so a body it declined to learn FROM stayed an endpoint on this surface. That is the
   mirror of the defect §Architecture counts seven of: not a field read with nothing writing it, but one
   written, asserted, and consumed by nothing on the surface the rule governs — "an observation with a computed
   writer and no reader is not a mechanism", and it is harder to see because the value is real. Measured: an
   image compressor whose document ships nine `<img>` elements and no API reported NINE endpoints, every one of
   them a file, and a reader of that popup is told the tool learned nine things about an API.
   THE KEY IS COMPUTED BY THE SAME TWO STEPS `endpoint_record` KEYS BY, called here rather than restated —
   a second normalisation would be two answers to one question, which is how a retraction silently matches
   nothing. `url_display` is not among them because it is the identity on a concrete string and this address is
   one: a concolic's request carries its SHAPE to the host (core/fetch/fetch.c), so the string that comes back
   with the reply is already the display form this surface filed it under.
   IT MARKS RATHER THAN DELETES. The address may be recorded again by another call site in the same run, and
   what was learned is a fact about the RESOURCE, so a later sighting must stay suppressed — and the params of
   a record dropped mid-array would take their neighbours' indices with them.
   EVERY same-(method, path) RECORD IS MARKED and the param set is not part of this identity: `same_identity`
   separates two REQUESTS to one address by what they carry, and what the bytes turned out to be is a fact
   about the address alone. */
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
}
