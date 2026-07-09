/* Per-flow value-domain constraint tracker — see constraints.h. Extracted from main.c. */
#include "constraints.h"
#include <stdlib.h>
#include <string.h>

Cons *g_cons = NULL; int g_cons_cap = 0, g_cons_n = 0;
char g_origin_req[256] = "";
char g_sink_jkey[128] = "";
char g_sink_root[64] = "";

void cons_reset(void) { for (int i = 0; i < g_cons_n; i++) { free(g_cons[i].src); free(g_cons[i].tok); free(g_cons[i].jkey); } g_cons_n = 0; g_origin_req[0] = 0; }
void cons_set(int i, const char *src, const char *tok, int op, const char *jkey) {   /* record the constraint that holds at decision i */
    if (i >= g_cons_cap) { int nc = g_cons_cap ? g_cons_cap * 2 : 64; while (nc <= i) nc *= 2; Cons *n = realloc(g_cons, (size_t)nc * sizeof(Cons)); if (!n) return; g_cons = n; g_cons_cap = nc; }
    for (int j = g_cons_n; j <= i; j++) { g_cons[j].src = NULL; g_cons[j].tok = NULL; g_cons[j].op = OPCMP_NONE; g_cons[j].jkey = NULL; }   /* fill gaps */
    if (i >= g_cons_n) g_cons_n = i + 1;
    free(g_cons[i].src); free(g_cons[i].tok); free(g_cons[i].jkey);
    g_cons[i].src = src ? strdup(src) : NULL; g_cons[i].tok = tok ? strdup(tok) : NULL; g_cons[i].op = op; g_cons[i].jkey = jkey ? strdup(jkey) : NULL;
}
int opcmp_neg(int op) { switch (op) { case OPCMP_EQ: return OPCMP_NE; case OPCMP_NE: return OPCMP_EQ; case OPCMP_LT: return OPCMP_GE; case OPCMP_GE: return OPCMP_LT; case OPCMP_GT: return OPCMP_LE; case OPCMP_LE: return OPCMP_GT; } return OPCMP_NONE; }
static int tok_num(const char *t, double *o) { if (!t || !*t) return 0; char *e; double d = strtod(t, &e); if (e == t || *e) return 0; *o = d; return 1; }
static int cmp_sat(double x, int op, double v) { switch (op) { case OPCMP_EQ: return x == v; case OPCMP_NE: return x != v; case OPCMP_LT: return x < v; case OPCMP_GT: return x > v; case OPCMP_LE: return x <= v; case OPCMP_GE: return x >= v; } return 1; }
/* Do `x op1 t1` and `x op2 t2` PROVABLY have no common x? Returns 1 only when certain (SOUND: never a false
   contradiction) — pure string EQ/NE, or a numeric interval/point contradiction; anything unprovable -> 0. */
static int pair_contradicts(int op1, const char *t1, int op2, const char *t2) {
    int e1 = (op1 == OPCMP_EQ || op1 == OPCMP_NE), e2 = (op2 == OPCMP_EQ || op2 == OPCMP_NE);
    int same = (t1 && t2 && strcmp(t1, t2) == 0);
    if (e1 && e2) {   /* string equality/disequality */
        if (op1 == OPCMP_EQ && op2 == OPCMP_EQ) return !same;                                   /* x==a & x==b (a!=b) */
        if ((op1 == OPCMP_EQ && op2 == OPCMP_NE) || (op1 == OPCMP_NE && op2 == OPCMP_EQ)) return same;   /* x==a & x!=a */
        return 0;
    }
    double a, b; if (!tok_num(t1, &a) || !tok_num(t2, &b)) return 0;   /* need numeric tokens to reason */
    if (op1 == OPCMP_EQ) return !cmp_sat(a, op2, b);   /* x fixed to a: must satisfy op2 b */
    if (op2 == OPCMP_EQ) return !cmp_sat(b, op1, a);
    if (op1 == OPCMP_NE || op2 == OPCMP_NE) return 0;  /* excluding one point never empties a relational */
    int lower1 = (op1 == OPCMP_GT || op1 == OPCMP_GE); /* both relational: a lower bound (GT/GE) vs an upper (LT/LE) */
    int lower2 = (op2 == OPCMP_GT || op2 == OPCMP_GE);
    if (lower1 == lower2) return 0;                     /* same side -> the tighter wins, never empty */
    double lo = lower1 ? a : b, hi = lower1 ? b : a;
    int lo_strict = (lower1 ? op1 : op2) == OPCMP_GT;   /* GT excludes the boundary */
    int hi_strict = (lower1 ? op2 : op1) == OPCMP_LT;   /* LT excludes the boundary */
    if (lo > hi) return 1;
    return (lo == hi && (lo_strict || hi_strict));      /* (5,5] / [5,5) / (5,5) empty; [5,5]={5} not */
}
/* Is `src <op> tok` consistent with the constraints already holding on this flow (indices < upto)? */
int cons_feasible(const char *src, const char *tok, int op, int upto) {
    if (!src) return 1;
    for (int i = 0; i < upto && i < g_cons_n; i++) {
        Cons *c = &g_cons[i];
        if (!c->src || strcmp(c->src, src)) continue;
        if (pair_contradicts(op, tok, c->op, c->tok)) return 0;
    }
    return 1;
}
/* @H CONCRETIZATION (never invention): the value an `==` gate PINNED on this flow (`x=='admin'` -> "admin"),
   or NULL. Only equality concretizes — the code determined the value, so it is COMPUTED, not fabricated. */
const char *cons_fixed_value(const char *src) {
    if (!src) return NULL;
    for (int i = 0; i < g_cons_n; i++) if (g_cons[i].src && g_cons[i].op == OPCMP_EQ && !strcmp(g_cons[i].src, src)) return g_cons[i].tok;
    return NULL;
}
void cons_free(void) { cons_reset(); free(g_cons); g_cons = NULL; g_cons_cap = 0; }
