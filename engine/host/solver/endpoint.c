/* @H endpoint surface — see endpoint.h. Findings are C data; params + values merge in C; emit is C. */
#include "solver/endpoint.h"
#include "core/json_buf.h"
#include "core/mime/mime_type.h"
#include "solver/concolic.h"
#include "solver/flow.h"
#include "check.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* WHERE THE VALUE LANDED IN THE REQUEST. A reviewer replays a path param by substituting it into the address,
   a query param by appending it and a body param by encoding it into the payload, so this is not a label on a
   param — it is half of what the param IS, and two params of the same name in two places are two params. */
typedef enum { EP_QUERY = 0, EP_PATH, EP_BODY } EpLoc;
static const char *const ep_loc_name[] = { "query", "path", "body" };

typedef struct { char *name; EpLoc loc; char **vals; int nvals, vcap; } Param;   /* validValues merged across same-shape hits */
typedef struct { char *name; char *value; } EpHeader;   /* the transport half: what the request must carry */
typedef struct { char *method; char *path; Param *params; int np, pcap;
                 EpHeader *hdrs; int nh, hcap; } Endpoint;

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

static char *url_display(JSContext *ctx, JSValueConst url) {
    if (concolic_is(url)) { const char *s = concolic_shape_c(url); return strdup(s ? s : "{}"); }
    const char *s = JS_ToCString(ctx, url);
    char *r = strdup(s ? s : "?");
    if (s) JS_FreeCString(ctx, s);
    return r;
}

/* THE PARAMS OF ONE OBSERVED REQUEST, in the order a reviewer meets them: path, then query, then body. Owned
   until they are merged into the surface. */
typedef struct { char *name; char *val; EpLoc loc; } KV;
typedef struct { KV *e; int n, cap; } KvBuf;

static void kv_add(KvBuf *b, const char *name, size_t nlen, const char *val, size_t vlen, EpLoc loc) {
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
    b->n++;
}

static void kv_free(KvBuf *b) {
    for (int i = 0; i < b->n; i++) { free(b->e[i].name); free(b->e[i].val); }
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
        kv_add(b, name, strlen(name), val, strlen(val), loc);
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
            json_buf_puts(&p, seg);   /* a literal segment, or a `{}` this surface cannot name */
        } else {
            /* THE GRAMMAR THE CONSUMER SUBSTITUTES BY, ASSERTED AT THE MINT. Both bytes are stripped above and
               a segment cannot hold a `/`, so this holds by construction — which is exactly why it is worth
               asserting: the next producer of a name has to keep it true. */
            DCHECK(!strpbrk(name, "{}/"),
                   "a path param's NAME still holds a brace or a slash — the popup substitutes a hole by "
                   "matching /\\{([^}/]+)\\}/ against this path, so a name outside that grammar names a hole "
                   "no substitution can find");
            json_buf_puts(&p, "{"); json_buf_puts(&p, name); json_buf_puts(&p, "}");
            kv_add(out, name, nlen, aligned ? b : "", aligned ? bn : 0, EP_PATH);
        }
        free(seg); free(name);
        if (!ae) break;
        json_buf_puts(&p, "/");
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
       JSON.stringify(x)})` — the commonest POST in any bundle, and the shape testing/test-spec.js asserts
       against — arrives carrying a type the PAGE never chose. So its bytes go through the real JSON parser and
       either are a name -> value document or are not; a plain-text body that is not one simply fails to parse
       and records nothing, which is the same answer as before. This is not sniffing: nothing branches on a
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
                if (key[0]) kv_add(out, key, strlen(key), val ? val : "", val ? strlen(val) : 0, EP_BODY);
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

/* an endpoint's IDENTITY is (method, path, param-set) — same identity merges param values. A param's LOCATION
   is part of it: an `id` the code puts in the path and an `id` it puts in the body are two different things to
   send, so two requests that agree only on the names are not one endpoint. */
static int same_identity(Endpoint *e, const char *method, const char *path, const KvBuf *kv) {
    if (strcmp(e->method, method) || strcmp(e->path, path) || e->np != kv->n) return 0;
    for (int i = 0; i < kv->n; i++)
        if (e->params[i].loc != kv->e[i].loc || strcmp(e->params[i].name, kv->e[i].name)) return 0;
    return 1;
}

void endpoint_record(JSContext *ctx, const char *method, JSValueConst url,
                     const EndpointHeader *hdrs, int nhdrs, const EndpointBody *body) {
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
        if (same_identity(&g_eps[i], method, path, &kvb)) {
            for (int j = 0; j < kvb.n; j++) param_add_val(&g_eps[i].params[j], kvb.e[j].val);
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
    e->method = strdup(method); e->path = strdup(path);
    if (kvb.n) { e->params = calloc((size_t)kvb.n, sizeof(Param)); CHECK(e->params, "endpoint: OOM params"); }
    for (int j = 0; j < kvb.n; j++) {
        e->params[e->np].name = strdup(kvb.e[j].name);
        e->params[e->np].loc = kvb.e[j].loc;
        param_add_val(&e->params[e->np], kvb.e[j].val);
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

/* Serialize the @H surface DIRECTLY to a JSON string in C (caller frees) — no JS-object round-trip. The
   writer is core/json_buf.h's: this file and solve.c each carried a private copy of it, which is one copy too
   many of a thing that has exactly one correct behaviour. */
char *endpoint_json_array(void) {
    JsonBuf b = { 0 };
    json_buf_puts(&b, "[");
    for (int i = 0; i < g_eps_n; i++) {
        Endpoint *e = &g_eps[i];
        if (i) json_buf_puts(&b, ",");
        json_buf_puts(&b, "{\"method\":"); json_buf_str(&b, e->method);
        json_buf_puts(&b, ",\"url\":"); json_buf_str(&b, e->path);
        json_buf_puts(&b, ",\"params\":[");
        for (int j = 0; j < e->np; j++) {
            if (j) json_buf_puts(&b, ",");
            json_buf_puts(&b, "{\"name\":"); json_buf_str(&b, e->params[j].name);
            DCHECK(e->params[j].loc == EP_QUERY || e->params[j].loc == EP_PATH || e->params[j].loc == EP_BODY,
                   "an endpoint param carries a location this surface has no name for — the enum and its "
                   "name table are read together at exactly this line, so one grown without the other is "
                   "caught here rather than emitted as a field the consumer cannot classify");
            json_buf_puts(&b, ",\"location\":"); json_buf_str(&b, ep_loc_name[e->params[j].loc]);
            json_buf_puts(&b, ",\"validValues\":[");
            for (int k = 0; k < e->params[j].nvals; k++) { if (k) json_buf_puts(&b, ","); json_buf_str(&b, e->params[j].vals[k]); }
            json_buf_puts(&b, "]}");
        }
        json_buf_puts(&b, "]");
        /* The transport half, and ONLY when there is one — an endpoint with no learned header must not claim an
           empty requirement, which reads as "needs nothing" rather than "nothing was observed". A record keyed
           by header name, which is the shape the popup's Required Headers section already reads. */
        if (e->nh) {
            json_buf_puts(&b, ",\"headers\":{");
            for (int j = 0; j < e->nh; j++) {
                if (j) json_buf_puts(&b, ",");
                json_buf_str(&b, e->hdrs[j].name);
                json_buf_puts(&b, ":");
                json_buf_str(&b, e->hdrs[j].value);
            }
            json_buf_puts(&b, "}");
        }
        json_buf_puts(&b, "}");
    }
    json_buf_puts(&b, "]");
    return json_buf_take(&b);
}

int endpoint_count(void) { return g_eps_n; }

void endpoint_free(void) {
    for (int i = 0; i < g_eps_n; i++) {
        free(g_eps[i].method); free(g_eps[i].path);
        for (int j = 0; j < g_eps[i].np; j++) { free(g_eps[i].params[j].name); for (int k = 0; k < g_eps[i].params[j].nvals; k++) free(g_eps[i].params[j].vals[k]); free(g_eps[i].params[j].vals); }
        free(g_eps[i].params);
        for (int j = 0; j < g_eps[i].nh; j++) { free(g_eps[i].hdrs[j].name); free(g_eps[i].hdrs[j].value); }
        free(g_eps[i].hdrs);
    }
    free(g_eps); g_eps = NULL; g_eps_n = g_eps_cap = 0;
}
