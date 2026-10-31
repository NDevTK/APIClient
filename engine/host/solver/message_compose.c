/* The data an attacker document posts, composed from what the running flow's path decided about a message's
 * unknown `data` (HTML §9.3.3's message, the attacker source `message.data:<id>`).
 *
 * The value is read off the unknown's constraint domain and its member structure, never off how a decision was
 * spelled: the member reads under the root (concolic_members_each) are the nodes, and each node's facets are what
 * the flow filed on it — a pinned literal (concolic_src_pin), the types still open (concolic_types), its
 * truthiness (concolic_value_truth), the tokens an equality proved it is not (concolic_excluded), the loose
 * equalities that held, an ordering bound, and the string call predicates on either arm. A node with members is
 * an object; the injection node holds the payload; any other node holds the simplest JSON value its facets
 * admit, and a node nothing constrained is left out. A facet it cannot satisfy refuses the composition, naming
 * the facet and the node.
 *
 * The result is a candidate: the fire it is for ran with the data unknown, so only a fire with this value
 * delivered proves a sender reproduces it. */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "core/json_buf.h"
#include "solver/concolic.h"
#include "solver/message_compose.h"
#include "solver/reclaim.h"

typedef struct { const char *src, *ident; const char *const *path; int n; } Node;
typedef struct { Node *v; int n, cap; } Nodes;

static void nodes_add(const char *src, const char *ident, const char *const *path, int n, void *ud)
{
    Nodes *ns = ud;

    if (ns->n == ns->cap) {
        int nc = ns->cap ? ns->cap * 2 : 16;
        Node *g = realloc(ns->v, (size_t)nc * sizeof *g);
        CHECK(g, "message compose: OOM collecting a root's members");
        ns->v = g; ns->cap = nc;
    }
    ns->v[ns->n++] = (Node){ src, ident, path, n };
}

typedef struct {
    Nodes ns;
    const char *inject, *payload;
    char *why;
} Compose;

static void refuse(Compose *c, const char *why, const char *at)
{
    size_t n;

    if (c->why) return;
    n = strlen(why) + strlen(at) + 4;
    c->why = reclaim_malloc(n);
    CHECK(c->why, "message compose: OOM stating a refusal");
    snprintf(c->why, n, "%s: %s", why, at);
}

/* Is `child` a member of `parent`: one step deeper with the same path prefix. */
static int is_child(const Node *parent, const Node *child)
{
    if (child->n != parent->n + 1) return 0;
    for (int i = 0; i < parent->n; i++) if (strcmp(parent->path[i], child->path[i])) return 0;
    return 1;
}

static int excluded(const char *src, const char *tok)
{
    char *hole = concolic_hole_key(src);
    int n = 0, hit = 0;
    const char *const *ex = hole ? concolic_excluded(hole, &n) : NULL;

    for (int i = 0; i < n && !hit; i++) hit = !strcmp(ex[i], tok);
    free(hole);
    return hit;
}

/* The domain rows filed on a node's hole: an ordering bound, the loose equalities that held, and the string call
   predicates on either arm. Borrowed from the running flow's constraint. */
typedef struct {
    char *hole;
    int has_bound;
    ConcolicBound bound;
    const ConcolicLooseEq *leq;
    int nleq;
    const ConcolicPred *pred;
    int npred;
} Rows;

static void rows_read(Rows *r, const char *src)
{
    memset(r, 0, sizeof *r);
    r->hole = concolic_hole_key(src);
    if (!r->hole) return;
    r->has_bound = concolic_bound_read(r->hole, &r->bound);
    r->leq = concolic_looseeq_read(r->hole, &r->nleq);
    r->pred = concolic_strpred_read(r->hole, &r->npred);
}

/* Does `s` satisfy one string call predicate on the arm the path took? -1 for a predicate this composition
   cannot evaluate (a method it does not run, or a position argument). */
static int pred_satisfied(const ConcolicPred *p, const char *s)
{
    size_t ls = strlen(s), la;
    int r;

    if (p->nargs != 1) return -1;
    la = strlen(p->args[0]);
    if (!strcmp(p->method, "startsWith")) r = ls >= la && !memcmp(s, p->args[0], la);
    else if (!strcmp(p->method, "endsWith")) r = ls >= la && !memcmp(s + ls - la, p->args[0], la);
    else if (!strcmp(p->method, "includes")) r = strstr(s, p->args[0]) != NULL;
    else return -1;
    return r == (p->holds != 0);
}

/* A string the held call predicates build (each held startsWith's argument, then each held includes', then each
   held endsWith'), checked against every predicate and exclusion; malloc'd, or NULL with `*why` naming the
   predicate it cannot satisfy. */
static char *pred_string(const Rows *r, const char *src, int truth, const char **why)
{
    JsonBuf b = { 0 };
    char *s;
    static const char *const ORDER[3] = { "startsWith", "includes", "endsWith" };

    *why = NULL;
    for (int o = 0; o < 3; o++)
        for (int i = 0; i < r->npred; i++)
            if (r->pred[i].holds && r->pred[i].nargs == 1 && !strcmp(r->pred[i].method, ORDER[o]))
                json_buf_raw(&b, r->pred[i].args[0]);
    if (truth == 1 && !b.n) json_buf_raw(&b, "x");
    s = json_buf_take(&b);
    if (!s) { s = reclaim_strdup(""); CHECK(s, "message compose: OOM spelling a string"); }
    for (int i = 0; i < r->npred && !*why; i++) {
        int ok = pred_satisfied(&r->pred[i], s);
        if (ok < 0) *why = "a call predicate this composition does not run";
        else if (!ok) *why = "call predicates no one string built from their arguments satisfies";
    }
    if (!*why && excluded(src, s)) *why = "the string the call predicates build is a token an equality excluded";
    if (*why) { free(s); return NULL; }
    return s;
}

/* A number in the ordering bound that avoids excluded tokens and has the truthiness the path decided, spelled
   with §6.1.6.1.20's shortest round-tripping digits; 0 when the bound admits none this tries. */
static int bound_number(const Rows *r, const char *src, int truth, char *out, size_t cap)
{
    double lo = r->bound.has_lo ? r->bound.lo : -1e9, hi = r->bound.has_hi ? r->bound.hi : 1e9;
    double tries[5];
    int nt = 0;

    tries[nt++] = 0;
    tries[nt++] = 1;
    if (r->bound.has_lo) tries[nt++] = r->bound.lo_incl ? lo : lo + 1;
    if (r->bound.has_hi) tries[nt++] = r->bound.hi_incl ? hi : hi - 1;
    tries[nt++] = (lo + hi) / 2;
    for (int i = 0; i < nt; i++) {
        double v = tries[i];

        if (r->bound.has_lo && (v < lo || (v == lo && !r->bound.lo_incl))) continue;
        if (r->bound.has_hi && (v > hi || (v == hi && !r->bound.hi_incl))) continue;
        if ((truth == 1 && v == 0) || (truth == 0 && v != 0)) continue;
        snprintf(out, cap, "%.17g", v);
        if (excluded(src, out)) continue;
        return 1;
    }
    return 0;
}

/* A string the facets admit: empty unless truthy, and not a token an equality excluded. */
static void emit_string(JsonBuf *b, const char *src, int truth)
{
    char tok[16] = "";
    size_t len = truth == 1 ? 1 : 0;

    for (;; len++) {
        memset(tok, 'x', len);
        tok[len] = 0;
        if (!excluded(src, tok)) break;
        CHECK(len < sizeof tok - 1, "message compose: every short string was excluded");
    }
    json_buf_str(b, tok);
}

static void emit_node(Compose *c, JsonBuf *b, const Node *node, int *omitted);

static void emit_members(Compose *c, JsonBuf *b, const Node *node)
{
    int first = 1;

    json_buf_raw(b, "{");
    for (int i = 0; i < c->ns.n; i++) {
        const Node *ch = &c->ns.v[i];
        JsonBuf v = { 0 };
        int omitted = 0;
        char *txt;

        if (!is_child(node, ch)) continue;
        emit_node(c, &v, ch, &omitted);
        txt = json_buf_take(&v);
        if (!omitted && txt) {
            if (!first) json_buf_raw(b, ",");
            first = 0;
            json_buf_str(b, ch->path[ch->n - 1]);
            json_buf_raw(b, ":");
            json_buf_raw(b, txt);
        }
        free(txt);
    }
    json_buf_raw(b, "}");
}

static int has_children(const Compose *c, const Node *node)
{
    for (int i = 0; i < c->ns.n; i++) if (is_child(node, &c->ns.v[i])) return 1;
    return 0;
}

/* One node's value into `b`, or `*omitted` for a member the value leaves out (it reads undefined). */
static void emit_node(Compose *c, JsonBuf *b, const Node *node, int *omitted)
{
    unsigned types = concolic_types(node->src);
    int truth = concolic_value_truth(node->ident);
    ConcolicLit kind;
    const char *val;
    Rows dr;

    *omitted = 0;
    rows_read(&dr, node->src);
    if (!strcmp(node->src, c->inject)) {
        if (!(types & CONCOLIC_TYF_STRING) || (truth == 0 && *c->payload)) {
            refuse(c, "the injection member's facets admit no string the payload is", node->src);
            goto done;
        }
        json_buf_str(b, c->payload);
        goto done;
    }
    if (concolic_src_pin(node->src, &kind, &val)) {
        switch (kind) {
        case CONCOLIC_LIT_STRING: json_buf_str(b, val); goto done;
        case CONCOLIC_LIT_BOOL: case CONCOLIC_LIT_NULL: json_buf_raw(b, val); goto done;
        case CONCOLIC_LIT_NUMBER:
            if (strcmp(val, "NaN") && !strstr(val, "Infinity")) { json_buf_raw(b, val); goto done; }
            break;
        case CONCOLIC_LIT_UNDEFINED: *omitted = 1; goto done;
        default: break;
        }
        refuse(c, "a pinned value a message carries but JSON does not spell", node->src);
        goto done;
    }
    if (has_children(c, node)) {
        if (!(types & CONCOLIC_TYF_OBJECT) || truth == 0) {
            refuse(c, "members were read of a value whose facets admit no object", node->src);
            goto done;
        }
        emit_members(c, b, node);
        goto done;
    }
    /* A loose equality that held (§7.2.13 IsLooselyEqual ( x, y )) is answered by its own operand, which step 1
       makes equal whenever its type is still open; every other one that held must spell the same operand.
       Named residual. Not covered: holding loose equalities over different operands, or one whose operand's own
       type the facet excludes (step 5 or 6's ToNumber would answer a Number). Next diff builds: candidates from each
       operand's conversions checked against every equality. Absence shows as `compositionRefused` naming a
       loose equality. */
    if (dr.nleq) {
        for (int i = 1; i < dr.nleq; i++)
            if (dr.leq[i].kind != dr.leq[0].kind || strcmp(dr.leq[i].tok, dr.leq[0].tok)) {
                refuse(c, "loose equalities that held with different operands", node->src);
                goto done;
            }
        if (!(types & (dr.leq[0].kind == CONCOLIC_LIT_STRING ? CONCOLIC_TYF_STRING
                      : dr.leq[0].kind == CONCOLIC_LIT_NUMBER ? CONCOLIC_TYF_NUMBER
                      : dr.leq[0].kind == CONCOLIC_LIT_BOOL ? CONCOLIC_TYF_BOOLEAN
                      : dr.leq[0].kind == CONCOLIC_LIT_NULL ? CONCOLIC_TYF_NULL
                      : dr.leq[0].kind == CONCOLIC_LIT_UNDEFINED ? CONCOLIC_TYF_UNDEFINED : 0u))) {
            refuse(c, "a loose equality whose operand's type the facet excludes", node->src);
            goto done;
        }
        switch ((ConcolicLit)dr.leq[0].kind) {
        case CONCOLIC_LIT_STRING: json_buf_str(b, dr.leq[0].tok); goto done;
        case CONCOLIC_LIT_NUMBER: case CONCOLIC_LIT_BOOL: case CONCOLIC_LIT_NULL: json_buf_raw(b, dr.leq[0].tok); goto done;
        case CONCOLIC_LIT_UNDEFINED: *omitted = 1; goto done;
        default: refuse(c, "a loose equality whose operand JSON does not spell", node->src); goto done;
        }
    }
    if (dr.npred) {
        const char *why;
        char *str;

        if (!(types & CONCOLIC_TYF_STRING)) { refuse(c, "call predicates on a value whose facets admit no string", node->src); goto done; }
        str = pred_string(&dr, node->src, truth, &why);
        if (!str) { refuse(c, why, node->src); goto done; }
        json_buf_str(b, str);
        free(str);
        goto done;
    }
    if (dr.has_bound) {
        char num[40];

        if (!(types & CONCOLIC_TYF_NUMBER) || !bound_number(&dr, node->src, truth, num, sizeof num)) {
            refuse(c, "an ordering bound no number this composition tries satisfies", node->src);
            goto done;
        }
        json_buf_raw(b, num);
        goto done;
    }
    if (types == CONCOLIC_TYF_ALL && truth < 0) { *omitted = 1; goto done; }   /* nothing constrained it */
    if (types & CONCOLIC_TYF_STRING) { emit_string(b, node->src, truth); goto done; }
    if ((types & CONCOLIC_TYF_NUMBER) && !excluded(node->src, truth == 1 ? "1" : "0")) {
        json_buf_raw(b, truth == 1 ? "1" : "0");
        goto done;
    }
    if (types & CONCOLIC_TYF_BOOLEAN) { json_buf_raw(b, truth == 1 ? "true" : "false"); goto done; }
    if ((types & CONCOLIC_TYF_OBJECT) && truth != 0) { json_buf_raw(b, "{}"); goto done; }
    if ((types & CONCOLIC_TYF_NULL) && truth != 1) { json_buf_raw(b, "null"); goto done; }
    if ((types & CONCOLIC_TYF_UNDEFINED) && truth != 1) { *omitted = 1; goto done; }
    refuse(c, "the facets admit only types a structured-clone message or JSON cannot carry", node->src);
done:
    free(dr.hole);
}

char *message_compose(const char *root, const char *inject, const char *payload, char **why)
{
    Compose c = { { 0 }, inject, payload, NULL };
    const Node *top = NULL;
    Node bare = { root, NULL, NULL, 0 };
    JsonBuf b = { 0 };
    int omitted = 0;

    DCHECK(root && inject && payload && why, "a message composition was asked with no root, injection or payload");
    concolic_members_each(root, nodes_add, &c.ns);
    for (int i = 0; i < c.ns.n && !top; i++) if (c.ns.v[i].n == 0) top = &c.ns.v[i];
    emit_node(&c, &b, top ? top : &bare, &omitted);
    if (!c.why && omitted) json_buf_raw(&b, "null");   /* a message whose data the path never constrained */
    free(c.ns.v);
    *why = c.why;
    if (c.why) { json_buf_free(&b); return NULL; }
    return json_buf_take(&b);
}

/* The constants the page compared an origin with, each reduced to a host fragment: the host of an absolute URL,
   or the constant itself, kept to the characters a host label may hold (lowercased) and trimmed of separators.
   `dotted` says the constant began with a label separator, so a host ending in it is a subdomain of the site it
   names. */
typedef struct { char **frag; int *dotted; int n; } OriginFrags;

static void origin_frag(void *ud, const char *tok)
{
    OriginFrags *o = ud;
    const char *p = strstr(tok, "://"), *end;
    char frag[256];
    size_t n = 0;
    int dotted;

    p = p ? p + 3 : tok;
    dotted = *p == '.';
    end = p + strcspn(p, "/:?#");
    for (; p < end && n < sizeof frag - 1; p++) {
        char ch = (char)(*p >= 'A' && *p <= 'Z' ? *p - 'A' + 'a' : *p);

        if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '.') frag[n++] = ch;
    }
    while (n && (frag[n - 1] == '.' || frag[n - 1] == '-')) n--;
    frag[n] = 0;
    p = frag;
    while (*p == '.' || *p == '-') p++;
    if (!*p) return;
    o->frag = realloc(o->frag, (size_t)(o->n + 1) * sizeof *o->frag);
    o->dotted = realloc(o->dotted, (size_t)(o->n + 1) * sizeof *o->dotted);
    CHECK(o->frag && o->dotted, "message compose: OOM collecting an origin's constants");
    o->frag[o->n] = reclaim_strdup(p);
    CHECK(o->frag[o->n], "message compose: OOM collecting an origin's constants");
    o->dotted[o->n++] = dotted;
}

/* The pins the running flow made on the origin, as constants like the call operands. */
typedef struct { const char *src; OriginFrags *o; } OriginPins;

static void origin_pins(void *user, const char *src, const ConcolicPrincipalPin *pin, int n)
{
    const OriginPins *op = user;

    if (strcmp(src, op->src)) return;
    for (int i = 0; i < n; i++) origin_frag(op->o, pin[i].tok);
}

static char *origin_of_host(const char *host)
{
    size_t n = strlen(host) + sizeof "https://";
    char *out = reclaim_malloc(n);

    CHECK(out, "message compose: OOM spelling an origin");
    snprintf(out, n, "https://%s", host);
    return out;
}

int message_compose_origins(const char *origin_src, char ***out)
{
    OriginFrags o = { NULL, NULL, 0 };
    OriginPins op = { origin_src, &o };
    JsonBuf sub = { 0 };
    char **c;
    char *host;
    int n = 0;

    DCHECK(origin_src && *origin_src && out, "origins were composed for no source");
    concolic_principal_tokens_each(origin_src, origin_frag, &o);
    concolic_principal_pins(origin_pins, &op);
    c = reclaim_malloc((size_t)(o.n + 1) * sizeof *c);
    CHECK(c, "message compose: OOM listing an origin's candidates");
    for (int i = 0; i < o.n; i++) { json_buf_raw(&sub, o.frag[i]); json_buf_raw(&sub, "."); }
    json_buf_raw(&sub, concolic_attacker_site());
    host = json_buf_take(&sub);
    CHECK(host, "message compose: OOM spelling an origin");
    c[n++] = origin_of_host(host);
    free(host);
    for (int i = 0; i < o.n; i++) {
        /* A registrable domain of the attacker's own that ends in the constant: a fresh label prefixed to it, a
           different name and so a different site. A constant that begins with a separator names subdomains of
           its site, and one with no separator is no domain to end in. */
        if (!o.dotted[i] && strchr(o.frag[i], '.')) {
            size_t len = strlen(o.frag[i]) + 2;
            char *own = reclaim_malloc(len);

            CHECK(own, "message compose: OOM spelling an origin");
            snprintf(own, len, "x%s", o.frag[i]);
            c[n++] = origin_of_host(own);
            free(own);
        }
        free(o.frag[i]);
    }
    free(o.frag);
    free(o.dotted);
    *out = c;
    return n;
}
