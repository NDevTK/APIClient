/* The data an attacker document posts, composed from what the running flow's path decided about a message's
 * unknown `data` (HTML §9.3.3's message, the attacker source `message.data:<id>`).
 *
 * The value is read off the unknown's constraint domain and its member structure, never off how a decision was
 * spelled: the member reads under the root (concolic_members_each) are the nodes, and each node's facets are
 * what the flow filed on it — a pinned literal (concolic_src_pin), the typeof rows still open
 * (concolic_typeof_rows), its truthiness (concolic_value_truth), and the tokens an equality proved it is not
 * (concolic_excluded). A node with members is an object; the injection node holds the payload; any other node
 * holds the simplest JSON value its facets admit, and a node nothing constrained is left out. A domain row this
 * composition does not read (an ordering bound, a loose equality, a call predicate) refuses it, naming the row,
 * since composing past it could pick a value the path did not admit.
 *
 * The result is a candidate: the fire it is for ran with the data unknown, so only a fire with this value
 * delivered proves a sender reproduces it. */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "core/json_buf.h"
#include "solver/concolic.h"
#include "solver/message_compose.h"

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
    c->why = malloc(n);
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

/* A domain row this composition does not read, named, or NULL. */
static const char *unread_row(const char *src)
{
    char *hole = concolic_hole_key(src);
    ConcolicBound b;
    int n = 0;
    const char *row = NULL;

    if (hole && concolic_bound_read(hole, &b)) row = "an ordering bound";
    else if (hole && concolic_looseeq_read(hole, &n) && n) row = "a loose equality";
    else if (hole && concolic_strpred_read(hole, &n) && n) row = "a call predicate";
    free(hole);
    return row;
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
    unsigned rows = concolic_typeof_rows(node->src);
    int truth = concolic_value_truth(node->ident);
    ConcolicLit kind;
    const char *val, *row;

    *omitted = 0;
    if ((row = unread_row(node->src))) { refuse(c, row, node->src); return; }
    if (!strcmp(node->src, c->inject)) {
        if (!(rows & CONCOLIC_TYR_STRING) || (truth == 0 && *c->payload)) {
            refuse(c, "the injection member's facets admit no string the payload is", node->src);
            return;
        }
        json_buf_str(b, c->payload);
        return;
    }
    if (concolic_src_pin(node->src, &kind, &val)) {
        switch (kind) {
        case CONCOLIC_LIT_STRING: json_buf_str(b, val); return;
        case CONCOLIC_LIT_BOOL: case CONCOLIC_LIT_NULL: json_buf_raw(b, val); return;
        case CONCOLIC_LIT_NUMBER:
            if (strcmp(val, "NaN") && !strstr(val, "Infinity")) { json_buf_raw(b, val); return; }
            break;
        case CONCOLIC_LIT_UNDEFINED: *omitted = 1; return;
        default: break;
        }
        refuse(c, "a pinned value a message carries but JSON does not spell", node->src);
        return;
    }
    if (has_children(c, node)) {
        if (!(rows & CONCOLIC_TYR_OBJECT) || truth == 0) {
            refuse(c, "members were read of a value whose facets admit no object", node->src);
            return;
        }
        emit_members(c, b, node);
        return;
    }
    if (rows == CONCOLIC_TYR_ALL && truth < 0) { *omitted = 1; return; }   /* nothing constrained it */
    if (rows & CONCOLIC_TYR_STRING) { emit_string(b, node->src, truth); return; }
    if ((rows & CONCOLIC_TYR_NUMBER) && !excluded(node->src, truth == 1 ? "1" : "0")) {
        json_buf_raw(b, truth == 1 ? "1" : "0");
        return;
    }
    if (rows & CONCOLIC_TYR_BOOLEAN) { json_buf_raw(b, truth == 1 ? "true" : "false"); return; }
    if ((rows & CONCOLIC_TYR_OBJECT) && truth != 0) { json_buf_raw(b, "{}"); return; }
    if ((rows & CONCOLIC_TYR_OBJECT) && truth != 1) { json_buf_raw(b, "null"); return; }
    if ((rows & CONCOLIC_TYR_UNDEFINED) && truth != 1) { *omitted = 1; return; }
    refuse(c, "the facets admit only types a structured-clone message or JSON cannot carry", node->src);
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
