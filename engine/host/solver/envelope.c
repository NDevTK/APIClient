/* @S structured-source delivery envelope — see envelope.h. Turns the sink's opaque value + the per-flow
   constraint set into the delivery string a JSON.parse / str.split / URLSearchParams sink reconstructs, with
   every sibling gate the handler checks placed at its address. ONE addressing model (env_sibling_addr) shared
   by all three reconstruction schemes. No scheduler coupling. */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "solver/envelope.h"
#include "solver/constraints.h"   /* Cons, g_cons/g_cons_n, OPCMP_*, the constraints-owned g_sink_jkey/g_sink_root */

/* A structured source decomposes into addressable children reconstructed one of three ways; ONE descriptor
   (kind + the fields below) drives ONE addressing function, so a new destructuring API is a new CASE, never a
   fourth bespoke reconstructor. Set per-sink by envelope_detect; read by env_sibling_addr + the builders. */
typedef enum { ENV_NONE = 0, ENV_JSON, ENV_QUERY, ENV_DELIM } EnvKind;
static EnvKind g_env_kind = ENV_NONE;
static char g_sink_qkey[64] = "";       /* URLSearchParams.get param KEY the sink reads (empty = not query-sourced) */
static char g_sink_delim[16] = "";      /* the .split() delimiter of a split-sourced sink (empty = not split-sourced) */
static char g_sink_sprefix[128] = "";   /* the src prefix up to ".split" that split siblings share */
static int  g_sink_sidx = -1;           /* the sink part's index within the split */

/* set obj at a DOTTED field path ("data.body") to val (consumed), building intermediate objects. */
static void obj_set_path(JSContext *ctx, JSValue obj, const char *path, JSValue val) {
    char buf[128]; snprintf(buf, sizeof buf, "%s", path);
    char *p = buf; JSValue cur = JS_DupValue(ctx, obj);
    for (;;) {
        char *dot = strchr(p, '.');
        if (!dot) { JS_SetPropertyStr(ctx, cur, p, val); JS_FreeValue(ctx, cur); return; }
        *dot = 0;
        JSValue next = JS_GetPropertyStr(ctx, cur, p);
        if (!JS_IsObject(next)) { JS_FreeValue(ctx, next); next = JS_NewObject(ctx); JS_SetPropertyStr(ctx, cur, p, JS_DupValue(ctx, next)); }
        JS_FreeValue(ctx, cur); cur = next; p = dot + 1;
    }
}

/* THE structured-source addressing function: if Cons c is a SIBLING gate of the active envelope — same
   structured source, an address OTHER than the sink's own — write its ADDRESS (JSON field / query param /
   part index) into buf and return it; else NULL. Every envelope builder AND the minimality skip route through
   this ONE place, so the three reconstruction schemes share a single addressing model instead of duplicating
   the g_cons scan five ways; a new destructuring API is a new CASE here, not a new function. */
static const char *env_sibling_addr(const Cons *c, char *buf, size_t bufn) {
    /* EQ pins the exact value; PREFIX(startsWith)/SUB(includes) are satisfied by the token itself placed at the
       address (a value starting-with/containing tok), so all three place tok at the sibling's address. */
    if (!(c->src && c->tok && (c->op == OPCMP_EQ || c->op == OPCMP_PREFIX || c->op == OPCMP_SUB))) return NULL;
    switch (g_env_kind) {
    case ENV_JSON: {   /* {root}.field gate, keyed by the method-CLEAN jkey (src is .slice-polluted) */
        size_t rl = strlen(g_sink_root);
        if (g_sink_jkey[0] != '.' || strncmp(c->src, g_sink_root, rl) || c->src[rl] != '.'
            || !c->jkey || c->jkey[0] != '.' || strcmp(c->jkey, g_sink_jkey) == 0) return NULL;
        snprintf(buf, bufn, "%s", c->jkey + 1); return buf;   /* dotted field path */
    }
    case ENV_QUERY: {   /* {root}?param gate */
        size_t rl = strlen(g_sink_root);
        if (strncmp(c->src, g_sink_root, rl) || c->src[rl] != '?') return NULL;
        const char *gk = c->src + rl + 1, *ge = gk; while (*ge && *ge != '.') ge++;   /* param name, up to a transform '.' */
        size_t gl = (size_t)(ge - gk);
        if (gl == strlen(g_sink_qkey) && !strncmp(gk, g_sink_qkey, gl)) return NULL;   /* the sink param itself */
        snprintf(buf, bufn, "%.*s", (int)gl, gk); return buf;
    }
    case ENV_DELIM: {   /* prefix.<index> gate (a trailing ".startsWith"/".includes" method segment is tolerated) */
        size_t pl = strlen(g_sink_sprefix);
        if (strncmp(c->src, g_sink_sprefix, pl) || c->src[pl] != '.') return NULL;
        const char *ip = c->src + pl + 1; char *end; long n = strtol(ip, &end, 10);
        if (end == ip || (*end != 0 && *end != '.') || n < 0 || (int)n == g_sink_sidx) return NULL;
        snprintf(buf, bufn, "%ld", n); return buf;
    }
    default: return NULL;
    }
}

/* @S JSON ENVELOPE: a JSON.parse(src).field sink rides a JSON object the parse yields — sink field = the
   breakout, each sibling EQ gate field = its token — JSON.stringify'd for correct nesting+escaping. */
static char *json_envelope_cand(JSContext *ctx, const char *cand) {
    if (g_env_kind != ENV_JSON) return NULL;
    JSValue obj = JS_NewObject(ctx);
    obj_set_path(ctx, obj, g_sink_jkey + 1, JS_NewString(ctx, cand));   /* sink field = the context breakout */
    char addr[128];
    for (int i = 0; i < g_cons_n; i++) { const char *a = env_sibling_addr(&g_cons[i], addr, sizeof addr);
        if (a) obj_set_path(ctx, obj, a, JS_NewString(ctx, g_cons[i].tok)); }   /* sibling gate field = its token */
    JSValue s = JS_JSONStringify(ctx, obj, JS_UNDEFINED, JS_UNDEFINED);
    JS_FreeValue(ctx, obj);
    char *out = NULL; const char *cs = JS_ToCString(ctx, s);
    if (cs) { out = strdup(cs); JS_FreeCString(ctx, cs); }
    JS_FreeValue(ctx, s);
    return out;
}

/* @S POSITIONAL ENVELOPE: a str.split(D)[i] sink rides a delimited string the split yields — sink part = the
   breakout, each sibling EQ gate part = its token, joined by D ("preview|<breakout>"). */
static char *delim_envelope_cand(const char *cand) {
    if (g_env_kind != ENV_DELIM) return NULL;
    const char *parts[32]; for (int i = 0; i < 32; i++) parts[i] = NULL;
    int maxidx = g_sink_sidx < 32 ? g_sink_sidx : 31;
    char addr[128];
    for (int i = 0; i < g_cons_n; i++) { const char *a = env_sibling_addr(&g_cons[i], addr, sizeof addr);
        if (a) { int n = atoi(a); if (n >= 0 && n < 32) { parts[n] = g_cons[i].tok; if (n > maxidx) maxidx = n; } } }
    if (g_sink_sidx < 32) parts[g_sink_sidx] = cand;   /* sink part = the breakout */
    char buf[1600]; int o = 0; buf[0] = 0; size_t dl = strlen(g_sink_delim);
    for (int i = 0; i <= maxidx; i++) {
        if (i && o + (int)dl < (int)sizeof buf - 1) { memcpy(buf + o, g_sink_delim, dl); o += (int)dl; buf[o] = 0; }
        o += snprintf(buf + o, sizeof buf - o, "%s", parts[i] ? parts[i] : "");
    }
    /* Trim trailing empty parts: "a|X|" and "a|X" both split to parts[1]==='X', the shorter is the equivalent
       minimal PoC. Never trims into the sink's content (that isn't a trailing delim). */
    while (o >= (int)dl && strcmp(buf + o - (int)dl, g_sink_delim) == 0) { o -= (int)dl; buf[o] = 0; }
    return strdup(buf);
}

/* @S QUERY ENVELOPE: a URLSearchParams.get(key) sink rides a query string the parse yields — sink param = the
   breakout, each sibling EQ gate param = its token ("mode=preview&data=<breakout>"); location.search encodes it
   and the real get() form-decodes, so the breakout arrives intact. */
static char *query_envelope_cand(const char *cand) {
    if (g_env_kind != ENV_QUERY) return NULL;
    char buf[1600]; int o = 0; buf[0] = 0; char addr[128];
    for (int i = 0; i < g_cons_n; i++) { const char *a = env_sibling_addr(&g_cons[i], addr, sizeof addr);
        if (a) o += snprintf(buf + o, sizeof buf - o, "%s%s=%s", o ? "&" : "", a, g_cons[i].tok); }   /* sibling gate param = its token */
    o += snprintf(buf + o, sizeof buf - o, "%s%s=%s", o ? "&" : "", g_sink_qkey, cand);   /* sink param = the breakout */
    return strdup(buf);
}

/* dispatch order = detection order (split -> query -> JSON): a source destructured a specific way is
   reconstructed that way. NULL when no envelope applies (caller uses the raw candidate). */
char *envelope_build(JSContext *ctx, const char *cand) {
    char *env = delim_envelope_cand(cand);
    if (!env) env = query_envelope_cand(cand);
    if (!env) env = json_envelope_cand(ctx, cand);
    return env;
}

/* An envelope places a SIBLING gate token STRUCTURALLY (mode=preview), so ALSO prefixing it onto the sink
   payload is redundant noise. Skip any token an envelope sibling already covers — the SAME env_sibling_addr
   the builders use, so the skip can never drift from what was actually placed; a non-EQ self-gate token
   (startsWith('cmd:') on the sink itself) is not a sibling and is kept. */
int envelope_handles_token(const char *tok) {
    char addr[128];
    for (int i = 0; i < g_cons_n; i++)
        if (g_cons[i].tok && !strcmp(g_cons[i].tok, tok) && env_sibling_addr(&g_cons[i], addr, sizeof addr)) return 1;
    return 0;
}

/* Detect the descriptor from the sink's opaque VALUE: its JSON field path (JS_OpaqueJKey), its structured
   source-leaf path (JS_OpaqueSrcC — the '?' marker distinguishes a real query param from a string transform,
   and a trailing ".<index>" with a split delimiter marks a positional part), resolving the ONE envelope kind. */
void envelope_detect(JSContext *ctx, JSValueConst val) {
    (void)ctx;
    { const char *jk = JS_OpaqueJKey(val); snprintf(g_sink_jkey, sizeof g_sink_jkey, "%s", jk ? jk : ""); }   /* JSON envelope field path */
    const char *sp = JS_OpaqueSrcC(val);   /* the source LEAF path ("{pm}.html") -> post {html:payload}, not a bare string */
    g_sink_root[0] = 0;   /* the root source token so the envelope can merge sibling gate fields */
    if (sp) { const char *rb = strchr(sp, '}'); if (rb) { size_t rl = (size_t)(rb - sp + 1); if (rl < sizeof g_sink_root) { memcpy(g_sink_root, sp, rl); g_sink_root[rl] = 0; } } }
    /* query-source sink: a URLSearchParams.get(key) opaque has src "{search}?data" — the '?' marker (set only by
       js_sp_get) distinguishes a real query param from a string transform chain ("{hash}.slice"), so a raw
       hash/search sink is NOT misread as query-sourced. Record the KEY (after '?', up to a transform '.'). */
    g_sink_qkey[0] = 0;
    if (sp) { const char *qm = strchr(sp, '?');
        if (qm && qm[1]) { const char *e = qm + 1; while (*e && *e != '.') e++;
            size_t kl = (size_t)(e - (qm + 1));
            if (kl && kl < sizeof g_sink_qkey) { memcpy(g_sink_qkey, qm + 1, kl); g_sink_qkey[kl] = 0; } } }
    /* split-source sink: parts[i] from str.split(D) carries the delimiter (JS_OpaqueSplitDelim) and a src ending
       ".<index>". Record the delimiter, the shared prefix, and the index for a delimited positional envelope. */
    g_sink_delim[0] = 0; g_sink_sprefix[0] = 0; g_sink_sidx = -1;
    { const char *dl = JS_OpaqueSplitDelim(val);
      if (dl && dl[0] && sp) { const char *ld = strrchr(sp, '.');
          if (ld && ld[1]) { char *end; long idx = strtol(ld + 1, &end, 10);
              if (*end == 0 && idx >= 0) { g_sink_sidx = (int)idx;
                  snprintf(g_sink_delim, sizeof g_sink_delim, "%s", dl);
                  size_t pl = (size_t)(ld - sp); if (pl < sizeof g_sink_sprefix) { memcpy(g_sink_sprefix, sp, pl); g_sink_sprefix[pl] = 0; } } } } }
    /* Resolve the ONE envelope kind (dispatch order matches envelope_build: split -> query -> JSON): a source
       destructured a specific way is reconstructed that way, and env_sibling_addr keys off this. */
    g_env_kind = (g_sink_delim[0] && g_sink_sidx >= 0 && g_sink_sprefix[0]) ? ENV_DELIM
               : g_sink_qkey[0] ? ENV_QUERY
               : (g_sink_jkey[0] == '.') ? ENV_JSON : ENV_NONE;
}
