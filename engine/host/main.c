/* APIClient v2 host entry — the ONE scheduler.
 *
 * DESIGN (the ONE invariant): ONE persistent runtime, ONE top-level scheduler loop, EVERYTHING is a
 * flow the loop schedules. No phases, no separate grind, no second loop.
 *
 * Capabilities so far, each verified on the proven loop:
 *  - value-ordered (NON-FIFO) flow registry in scheduler-owned C memory; everything-a-flow (__fork).
 *  - PREEMPTION: a flow parks (await __yield) + resumes WITH STATE via quickjs-ng async-frame suspend.
 *  - fetch(url) host edge -> @H (the COMPUTED endpoint), all flow code runs in the ONE loop.
 *  - ORPHAN-INVOKE: force-invoke never-executed functions (JS_CollectOrphans) -> the UNUSED endpoints.
 *  - FORCED BRANCH-ARMS (this milestone): __branch() explores BOTH arms of a gated branch by
 *    decision-vector BFS — a flow re-runs its function with a forced-choice table; a new decision
 *    returns true for this flow and FORKS a sibling that replays the prefix then takes false. This
 *    surfaces the branch-gated (login/flag-gated) endpoints. Value-ordered, so productive paths first.
 *    (Auto-forking at OP_if on OPAQUE external input — so real bundles need no __branch — is the next
 *    engine capability; the decision-vector scheduling is proven here.)
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "quickjs.h"
#include "quickjs-libc.h"
#include <lexbor/html/html.h>
#include <lexbor/css/css.h>
#include <lexbor/selectors/selectors.h>
#include <lexbor/dom/dom.h>
#include <lexbor/url/url.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define KEEP EMSCRIPTEN_KEEPALIVE   /* export qjs_init/step/teardown for the persistent-instance protocol */
#else
#define KEEP
#endif

/* ---- the ONE flow registry (scheduler-owned memory) --------------------------------
   A flow is a STARTER (a function to force-invoke, with a decision vector for its branch choices) or
   a RESUMER (the resolve fn of a parked flow's __yield promise). Both carry value-of-information. */
/* NO BOUNDS: the registry and the decision vector grow dynamically (until RAM/disk, the platform floor
   — the design's UNBOUNDED). No FLOW_MAX / DEC_MAX cap that would truncate distinct work the scheduler
   would otherwise reach. (Eviction of the cold/low-value tail to IDB is the further step; growth removes
   the artificial ceiling first.) */
typedef struct {
    JSValue handle;      /* the function (starter/suspended) or resolve fn (async __yield resume) */
    double val;          /* accumulated value-of-information (emits raise it) */
    int is_resume;       /* async __yield resume: JS_Call the resolve fn (not sync-preemptible) */
    signed char *dec; int dec_n;  /* per-flow decision vector (branch-arm BFS) */
    void *fs;            /* live heap frame if SUSPENDED mid-run (JS_FlowResume), else NULL */
    void *cow; int cow_n, cow_cap;  /* this flow's HEAP COW DELTA, stashed while parked (unapplied); swapped in on resume */
    void *dom; int dom_n, dom_cap;  /* this flow's DOM COW DELTA, same swap discipline */
    int saved_c;         /* per-flow branch cursor (g_c) snapshot, restored on resume */
    double cpu;          /* back-edge CPU ticks since last emit (WFQ decay; reset to 0 on emit) */
    int visits;          /* times scheduled (UCB/fairness explore term) */
    int orphan_idx;      /* cross-session locator: index in deterministic orphan collection (-1 = boot/yield, not park-replayable) */
    char *candidate;     /* @S REPLAY flow: a concrete breakout payload the source getters return (instead of opaque),
                            so this flow re-runs the orphan through the REAL code+branches with the candidate; the sink
                            then sees a CONCRETE value and checks breakout. NULL = a normal opaque exploration flow. */
    int session;         /* ATTACKER SESSION flow: fire ALL registered handlers in seed order over ONE accumulating
                            COW delta (handler A's tainted write to shared state is visible to handler B), modeling an
                            attacker firing a sequence of events — the sound way to reach cross-handler sinks. */
    char *vtarget;       /* @S candidate flow: the "sink|ctx" it verifies. Once that's in g_verified, this flow is
                            REDUNDANT (another candidate already broke out) -> skip it, saving a full bundle re-run. */
    int is_boot;         /* BOOT FLOW: re-run the page's boot (inline scripts) from the PRISTINE pre-boot baseline as a
                            FORKING starter, so an async reply (now cached, resolves synchronously on re-run) drives its
                            continuation's gated branches WITH the concolic example — the faithful boot-as-flow. */
} Flow;
static int g_in_session = 0;   /* a session flow is running -> solve_add enqueues candidate SESSION flows */
static int g_in_boot_flow = 0; /* a BOOT flow is re-running boot: fork boot siblings; suppress handler re-registration */
static Flow   *g_reg = NULL;
static int     g_reg_n = 0, g_reg_cap = 0;
static int     g_running = 0;
static double  g_cur_val = 0;
static Flow   *g_cur_flow = NULL;   /* running flow (a stable local copy; its weight is read by the yield hook) */
static int     g_emit_total = 0;
static JSRuntime *g_rt = NULL;
/* Cross-session frontier (park/resume by REPLAY): deterministic orphan collection gives each function a
   stable index; a parked flow's recipe = (orphan_idx, decision-vector). Parking is RAM-PRESSURE-driven
   (g_park_requested, host-set), never a dispatch/step count — with headroom the page runs to completion. */
static JSValue g_orphan_buf[4096];
static int     g_orphan_n = 0;
static int     g_cur_orphan_idx = -1;   /* running flow's orphan index (inherited by its branch siblings) */
static int     g_park_requested = 0;   /* host sets this under RAM pressure -> park the cold tail to IDB (NOT a dispatch count) */
static long    g_work = 0;              /* flow DISPATCHES this run (starter OR resume) — diagnostic only, never a park trigger */
static double  g_yield_floor = -1e300;  /* host cross-document WFQ: yield HOT to the host the moment this engine's best
                                           flow no longer outranks the RUNNER-UP engine (whose weight the host sets here).
                                           VALUE-driven, never a dispatch count — the WFQ, not a clock, decides the switch.
                                           -1e300 = no runner-up (single engine): run to completion/park. */
static int     g_made_progress = 0;     /* dispatched >=1 flow this qjs_step? (guards zero-work ping-pong; NOT a cap) */
static long    g_switches = 0;          /* flow SUSPEND/re-queue events (interleave) -> @RESULT._switches */
static int     g_resume_mode = 0;       /* resuming a parked frontier: seed ONLY the recipes, not fresh orphans */
static uint32_t g_bundle_id = 0;        /* stable id of THIS document's own scripts (Lexbor DOM scan, not regex) — the frontier key */
static JSValue g_opaque = JS_UNDEFINED;   /* the OPAQUE sentinel: external input the tool must not concretely decide */
static char *g_candidate = NULL;          /* @S: the running REPLAY flow's concrete candidate (source getters return it); NULL in normal flows */

/* A URL/shape carries an opaque HOLE — "{}" (generic) or "{tag}" (source-tagged: {hash}/{search}) — iff it
   has a '{' followed by only lowercase letters then '}'. Such a URL is not concretely fetchable. Generalizes
   the old literal strstr("{}") checks so source-tagged holes are still recognized as opaque. */
static int has_hole(const char *s) {
    if (!s) return 0;
    for (const char *p = s; (p = strchr(p, '{')); p++) {
        const char *q = p + 1; while (*q >= 'a' && *q <= 'z') q++;
        if (*q == '}') return 1;
    }
    return 0;
}

/* decision-vector state for the RUNNING starter flow (branch-arm BFS) — grows unbounded */
static JSValue      g_cur_fn = JS_UNDEFINED;   /* the running starter's function (borrowed) so __branch can fork a sibling that re-runs it */
static signed char *g_dec = NULL;              /* working decision vector: forced prefix + this flow's chosen-true suffix */
static int          g_dec_cap = 0;
static int          g_dec_n = 0;               /* length of decisions made/forced so far */
static int          g_c = 0;                   /* cursor: next decision index __branch will consume */

static int g_dec_ensure(int n) {              /* grow g_dec to hold >= n decisions */
    if (n <= g_dec_cap) return 1;
    int nc = g_dec_cap ? g_dec_cap * 2 : 64; while (nc < n) nc *= 2;
    signed char *nd = (signed char *)realloc(g_dec, (size_t)nc);
    if (!nd) return 0;
    g_dec = nd; g_dec_cap = nc; return 1;
}
/* PER-FLOW VALUE DOMAIN: the constraints on external sources that HOLD on this flow's path, one per decision
   (src NULL = the branch carried no value-domain provenance). Normalized to "src IS / ISN'T tok". Rebuilt as
   the flow runs (branch_decide re-sees each cond), so it needs no per-flow persistence. Feasibility of a new
   constraint is checked against these; a provably-contradicted branch arm is PRUNED (no phantom @H). */
typedef struct { char *src; char *tok; int op; } Cons;   /* op = the HOLDING comparison (OPCMP_*) of src vs tok on this flow's path */
static Cons *g_cons = NULL; static int g_cons_cap = 0, g_cons_n = 0;
static void cons_reset(void) { for (int i = 0; i < g_cons_n; i++) { free(g_cons[i].src); free(g_cons[i].tok); } g_cons_n = 0; }
static void cons_set(int i, const char *src, const char *tok, int op) {   /* record the constraint that holds at decision i */
    if (i >= g_cons_cap) { int nc = g_cons_cap ? g_cons_cap * 2 : 64; while (nc <= i) nc *= 2; Cons *n = realloc(g_cons, (size_t)nc * sizeof(Cons)); if (!n) return; g_cons = n; g_cons_cap = nc; }
    for (int j = g_cons_n; j <= i; j++) { g_cons[j].src = NULL; g_cons[j].tok = NULL; g_cons[j].op = OPCMP_NONE; }   /* fill gaps */
    if (i >= g_cons_n) g_cons_n = i + 1;
    free(g_cons[i].src); free(g_cons[i].tok);
    g_cons[i].src = src ? strdup(src) : NULL; g_cons[i].tok = tok ? strdup(tok) : NULL; g_cons[i].op = op;
}
static int opcmp_neg(int op) { switch (op) { case OPCMP_EQ: return OPCMP_NE; case OPCMP_NE: return OPCMP_EQ; case OPCMP_LT: return OPCMP_GE; case OPCMP_GE: return OPCMP_LT; case OPCMP_GT: return OPCMP_LE; case OPCMP_LE: return OPCMP_GT; } return OPCMP_NONE; }
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
static int cons_feasible(const char *src, const char *tok, int op, int upto) {
    if (!src) return 1;
    for (int i = 0; i < upto && i < g_cons_n; i++) {
        Cons *c = &g_cons[i];
        if (!c->src || strcmp(c->src, src)) continue;
        if (pair_contradicts(op, tok, c->op, c->tok)) return 0;
    }
    return 1;
}
/* @H CONCRETIZATION (never invention): the value an `==` gate PINNED on this flow (`x=='admin'` -> "admin"),
   or NULL. Only equality concretizes — the code determined the value, so it is COMPUTED, not fabricated. A
   range/prefix/regex gate pins NOTHING, so it stays a SHAPE (any in-range pick would masquerade as an observed
   key — banned). Range/regex solving is an @S-only tool (replay-verified there); it must never feed an @H example. */
static const char *cons_fixed_value(const char *src) {
    if (!src) return NULL;
    for (int i = 0; i < g_cons_n; i++) if (g_cons[i].src && g_cons[i].op == OPCMP_EQ && !strcmp(g_cons[i].src, src)) return g_cons[i].tok;
    return NULL;
}
/* Substitute each `{src}` hole the running flow FIXED (== gate) with its concrete value, so a URL built from
   gated input surfaces the SOLVED key in BOTH path and query (/api/{hash} -> /api/admin). NULL if nothing
   solved. The @H shape is re-derived downstream, so grouping is unaffected — only the example gains a value. */
static char *url_solve_holes(JSContext *ctx, const char *url) {
    (void)ctx;
    if (!url || !strchr(url, '{')) return NULL;
    size_t cap = strlen(url) + 64, len = 0; char *out = malloc(cap); if (!out) return NULL;
    int changed = 0;
    for (const char *p = url; *p; ) {
        if (*p == '{') {
            const char *close = strchr(p, '}');
            if (close && (size_t)(close - p + 1) < 64) {
                char hole[64]; size_t hl = (size_t)(close - p + 1); memcpy(hole, p, hl); hole[hl] = 0;
                const char *fixed = cons_fixed_value(hole);
                if (fixed) { size_t fl = strlen(fixed);
                    while (len + fl + 1 > cap) { cap *= 2; char *n = realloc(out, cap); if (!n) { free(out); return NULL; } out = n; }
                    memcpy(out + len, fixed, fl); len += fl; p = close + 1; changed = 1; continue; }
            }
        }
        if (len + 2 > cap) { cap *= 2; char *n = realloc(out, cap); if (!n) { free(out); return NULL; } out = n; }
        out[len++] = *p++;
    }
    out[len] = 0;
    if (!changed) { free(out); return NULL; }
    return out;
}

typedef struct DomUndo DomUndo;                 /* per-flow DOM COW delta buffer (defined below) */
static void dom_buf_free(DomUndo *buf, int n);
static int reg_add(JSContext *ctx, JSValue handle, double val, int is_resume, signed char *dec, int dec_n)
{
    if (g_reg_n >= g_reg_cap) {
        int nc = g_reg_cap ? g_reg_cap * 2 : 256;
        Flow *nr = (Flow *)realloc(g_reg, (size_t)nc * sizeof(Flow));
        if (!nr) { fflush(stdout); fprintf(stderr, "@E {\"phase\":\"reg_oom\"}\n"); fflush(stderr); abort(); }   /* OOM is a fatal error: crash, never continue with a dropped flow */
        g_reg = nr; g_reg_cap = nc;
    }
    g_reg[g_reg_n].handle = handle; g_reg[g_reg_n].val = val; g_reg[g_reg_n].is_resume = is_resume;
    g_reg[g_reg_n].dec = dec; g_reg[g_reg_n].dec_n = dec_n;
    g_reg[g_reg_n].fs = NULL; g_reg[g_reg_n].saved_c = 0; g_reg[g_reg_n].cpu = 0; g_reg[g_reg_n].visits = 0;
    g_reg[g_reg_n].cow = NULL; g_reg[g_reg_n].cow_n = 0; g_reg[g_reg_n].cow_cap = 0;
    g_reg[g_reg_n].dom = NULL; g_reg[g_reg_n].dom_n = 0; g_reg[g_reg_n].dom_cap = 0;
    g_reg[g_reg_n].orphan_idx = -1; g_reg[g_reg_n].candidate = NULL; g_reg[g_reg_n].session = 0; g_reg[g_reg_n].vtarget = NULL;
    g_reg[g_reg_n].is_boot = 0;
    g_reg_n++; return 1;
}

/* Re-add a SUSPENDED flow (a full copy, fs retained) so it interleaves back into the ONE registry. */
static int reg_readd(JSContext *ctx, Flow f)
{
    if (g_reg_n >= g_reg_cap) {
        int nc = g_reg_cap ? g_reg_cap * 2 : 256;
        Flow *nr = (Flow *)realloc(g_reg, (size_t)nc * sizeof(Flow));
        if (!nr) { fflush(stdout); fprintf(stderr, "@E {\"phase\":\"reg_oom\"}\n"); fflush(stderr); abort(); }   /* OOM is a fatal error: crash, never continue with a dropped flow */
        g_reg = nr; g_reg_cap = nc;
    }
    g_reg[g_reg_n++] = f; return 1;
}

/* __emit(tag): the ONLY progress signal — a real host edge (@H) surfaced. */
static JSValue js_emit(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    const char *s = argc > 0 ? JS_ToCString(ctx, argv[0]) : NULL;
    printf("@H %s\n", s ? s : "?"); fflush(stdout);
    if (s) JS_FreeCString(ctx, s);
    g_emit_total++;
    if (g_running) { g_cur_val += 1.0; if (g_cur_flow) { g_cur_flow->val = g_cur_val; g_cur_flow->cpu = 0; } }
    return JS_UNDEFINED;
}

static JSValue js_opaque_stub(JSContext *ctx, JSValueConst t, int c, JSValueConst *v);   /* fwd: opaque-returning host stub */
/* fromReply: a bridge-provided map { url -> concrete reply body text }. A real GET is fired by the
   TRUSTED offscreen (safeFetch, one-per-endpoint) and its body injected here so r.json()/r.text()
   return the CONCRETE server reply -> a reply field flowing into a downstream request param becomes a
   REAL example value instead of {}. Absent url -> opaque (the honest shape). */
static JSValue g_reply_table = JS_UNDEFINED;
static JSContext *g_ctx;   /* the run's context (defined = NULL below); fwd-declared for Lexbor callbacks */
/* ENDPOINT REGISTRY + IDENTITY: the engine ACCUMULATES every learned endpoint (method/url/params/
   headers/body) as a JS object here, and at finalize DEDUPS them (exact by method+hole-normalized url,
   then collapses a concrete instance into its shape with a path-param example) and emits the deduped
   set. Identity is the ENGINE's, not the host's (the JS mergeCallsites/dedupShapeConcrete were DELETED).
   The dedup runs on the engine's OWN quickjs (g_dedup_fn) — proven logic, executed in-engine, never a
   host context-switch. */
static JSValue g_endpoints = JS_UNDEFINED;   /* JS array of {method,url,params,headers,body} */
static JSValue g_dedup_fn = JS_UNDEFINED;    /* (eps) => deduped array, evaluated once at init */
static const char *DEDUP_JS =
"(function(eps){"
"  var HOLE=/\\{[a-z]*\\}/,HOLE_G=/\\{[a-z]*\\}/g,SEG_HOLE=/^\\{[a-z]*\\}$/;"
"  var hasHole=function(s){return HOLE.test(s||'');};"
"  var normHoles=function(s){return (s||'').replace(HOLE_G,'{}');};"
"  var pathSegs=function(u){var q=u.indexOf('?');var p=q>=0?u.slice(0,q):u;return {segs:p.split('/'),query:q>=0?u.slice(q):''};};"
"  for(var bi=0;bi<eps.length;bi++){var e=eps[bi];"
"    if(e.body){var bo=null;try{bo=JSON.parse(e.body);}catch(_e){bo=null;}"
"      if(bo&&typeof bo==='object'&&!Array.isArray(bo)){e.params=e.params||[];"
"        for(var bk in bo){var bv=bo[bk];"
"          var op=bv===null||bv==='{}'||(typeof bv==='object'&&bv&&!Array.isArray(bv)&&Object.keys(bv).length===0);"
"          var has=false;for(var qi=0;qi<e.params.length;qi++){if(e.params[qi].name===bk&&e.params[qi].location==='body'){has=true;break;}}"
"          if(!has)e.params.push({name:bk,location:'body',validValues:op?[]:[String(bv)]});"
"        }"
"      }else if(e.body.indexOf('=')>=0&&e.body.charAt(0)!=='{'&&e.body.charAt(0)!=='['){e.params=e.params||[];"
"        var frms=e.body.split('&');for(var fpi=0;fpi<frms.length;fpi++){var feq=frms[fpi].indexOf('=');if(feq<0)continue;"
"          var fk=frms[fpi].slice(0,feq),fv=frms[fpi].slice(feq+1),fop=hasHole(fv);"
"          var fhas=false;for(var fqi=0;fqi<e.params.length;fqi++){if(e.params[fqi].name===fk&&e.params[fqi].location==='body'){fhas=true;break;}}"
"          if(!fhas)e.params.push({name:fk,location:'body',validValues:fop?[]:[fv]});"
"        }"
"      }"
"    }"
"  }"
"  var map=new Map();"
"  var mergeInto=function(e,s){"                                                 /* UNION s into e (same identity) */
"    var sp=s.params||[];e.params=e.params||[];"
"    for(var pi=0;pi<sp.length;pi++){var np=sp[pi],f=null;"
"      for(var ei=0;ei<e.params.length;ei++){if(e.params[ei].name===np.name&&e.params[ei].location===np.location){f=e.params[ei];break;}}"
"      if(!f){e.params.push(np);}else{var nv=np.validValues||[];f.validValues=f.validValues||[];for(var vi=0;vi<nv.length;vi++){if(f.validValues.indexOf(nv[vi])<0)f.validValues.push(nv[vi]);}}"
"    }"
"    if(s.headers){e.headers=e.headers||{};var HH=/\\{[a-z]*\\}/;for(var hk in s.headers){var ov=e.headers[hk],nv=s.headers[hk];if(ov===undefined||(HH.test(ov)&&!HH.test(nv)))e.headers[hk]=nv;}}"
"    if(s.body&&(!e.body||(/\\{[a-z]*\\}/.test(e.body)&&!/\\{[a-z]*\\}/.test(s.body))))e.body=s.body;"
"  };"
"  for(var i=0;i<eps.length;i++){var s=eps[i];var k=(s.method||'GET')+' '+normHoles(s.url);if(!map.has(k))map.set(k,s);else mergeInto(map.get(k),s);}"
"  var arr=[];map.forEach(function(v){arr.push(v);});"
"  var shapes=arr.filter(function(e){return hasHole(e.url);});"
"  if(shapes.length){"
"    for(var ci=0;ci<arr.length;ci++){var c=arr[ci];if(hasHole(c.url))continue;"
"      for(var si=0;si<shapes.length;si++){var sh=shapes[si];"
"        if((sh.method||'GET')!==(c.method||'GET'))continue;"
"        var ss=pathSegs(sh.url),cs=pathSegs(c.url);"
"        if(ss.segs.length!==cs.segs.length||ss.query!==cs.query)continue;"
"        var ok=true,ex=[];"
"        for(var j=0;j<ss.segs.length;j++){"
"          if(SEG_HOLE.test(ss.segs[j])){if(cs.segs[j]&&!hasHole(cs.segs[j]))ex.push([j,cs.segs[j]]);else{ok=false;break;}}"
"          else if(ss.segs[j]!==cs.segs[j]){ok=false;break;}"
"        }"
"        if(ok&&ex.length){"
"          for(var e2=0;e2<ex.length;e2++){var idx=ex[e2][0],v=ex[e2][1];"
"            var pp=null,pl=sh.params||[];for(var pi=0;pi<pl.length;pi++){if(pl[pi].location==='path'&&pl[pi].name==='arg'+idx){pp=pl[pi];break;}}"
"            if(!pp){pp={name:'arg'+idx,location:'path',validValues:[]};(sh.params=sh.params||[]).push(pp);}"
"            if(pp.validValues.indexOf(v)<0)pp.validValues.push(v);"
"          }"
"          map.delete((c.method||'GET')+' '+c.url);break;"
"        }"
"      }"
"    }"
"  }"
"  var out=[];map.forEach(function(v){out.push(v);});return out;"
"})";
/* SELF-HOSTED Array.prototype iterators (forEach/map/filter/some/every): the C js_array_every holds
   its loop counter/accumulator on the C STACK across each callback JS_Call, so a fn recursing THROUGH
   the callback (node.children.forEach(walk)) C-recurses and overflows -- the continuation-holding
   re-entry the tail-forward trampoline cannot reach. As bytecode, the loop is a HEAP frame and the
   callback dispatches via the (trampolined) call path, so recursion through any of them is unbounded
   and snapshot/preempt/replay work at any iteration depth (verified to depth 3000). Each is a
   NON-CLOSURE (no IIFE, no captured var_refs, no shared helper -> no global pollution): an init-defined
   CLOSURE does NOT trampoline its deep recursion -- a real engine defect at the pre-boot-baseline/
   var_ref boundary; a non-closure avoids it (init non-closures trampoline, like the pages' boot fns).
   `>>>0` is internal ToUint32 (exact for real arrays; length>2^32 array-likes don't exist in practice).
   Spec-faithful: ToObject(this), HasProperty (`k in O`) skips holes, (value,index,array) args, thisArg,
   early-exit for some/every, hole-preserving map. Deviation: map/filter build a plain Array (Symbol.
   species not honored) -- effectively never overridden in real bundles. */
static const char *ARRAY_PRELUDE_JS =
"Object.defineProperty(Array.prototype,'forEach',{writable:true,enumerable:false,configurable:true,value:function forEach(cb,thisArg){"
"  if(typeof cb!=='function')throw new TypeError('Array.prototype.forEach: callback is not a function');"
"  var O=Object(this),L=O.length>>>0;"
"  for(var k=0;k<L;k++){if(k in O){cb.call(thisArg,O[k],k,O);}}return undefined;}});"
"Object.defineProperty(Array.prototype,'map',{writable:true,enumerable:false,configurable:true,value:function map(cb,thisArg){"  /* holes preserved; species not honored (plain Array) */
"  if(typeof cb!=='function')throw new TypeError('Array.prototype.map: callback is not a function');"
"  var O=Object(this),L=O.length>>>0,A=new Array(L);"
"  for(var k=0;k<L;k++){if(k in O){A[k]=cb.call(thisArg,O[k],k,O);}}return A;}});"
"Object.defineProperty(Array.prototype,'filter',{writable:true,enumerable:false,configurable:true,value:function filter(cb,thisArg){"
"  if(typeof cb!=='function')throw new TypeError('Array.prototype.filter: callback is not a function');"
"  var O=Object(this),L=O.length>>>0,A=[],n=0;"
"  for(var k=0;k<L;k++){if(k in O){var v=O[k];if(cb.call(thisArg,v,k,O))A[n++]=v;}}return A;}});"
"Object.defineProperty(Array.prototype,'some',{writable:true,enumerable:false,configurable:true,value:function some(cb,thisArg){"  /* early-exit on first truthy */
"  if(typeof cb!=='function')throw new TypeError('Array.prototype.some: callback is not a function');"
"  var O=Object(this),L=O.length>>>0;"
"  for(var k=0;k<L;k++){if(k in O){if(cb.call(thisArg,O[k],k,O))return true;}}return false;}});"
"Object.defineProperty(Array.prototype,'every',{writable:true,enumerable:false,configurable:true,value:function every(cb,thisArg){"  /* early-exit on first falsy */
"  if(typeof cb!=='function')throw new TypeError('Array.prototype.every: callback is not a function');"
"  var O=Object(this),L=O.length>>>0;"
"  for(var k=0;k<L;k++){if(k in O){if(!cb.call(thisArg,O[k],k,O))return false;}}return true;}});"
/* includes/indexOf are SELF-HOSTED not for overflow but for OPACITY-BY-CONSTRUCTION: the C builtins collapse
   `arr.includes(opaqueInput)` to a concrete false/-1 (identity compare), so a list-membership GATE
   (`if(allowed.includes(input))`) never forks and the gated code is never explored (missed @H). As bytecode
   the compare is OP_strict_eq, which propagates opacity -> the branch forks (explore) AND records the
   `{src}==element` constraint, so an ORIGIN whitelist auto-suppresses @S via cons_fixed_value while a DATA
   whitelist stays a real reachable sink. One mechanism, both outcomes. (SameValueZero for includes; strict
   === + hole-skip for indexOf; fromIndex via |0 — the prelude's small-index simplification.) */
"Object.defineProperty(Array.prototype,'includes',{writable:true,enumerable:false,configurable:true,value:function includes(sv,fi){"
"  var O=Object(this),L=O.length>>>0; if(L===0)return false;"
"  var n=fi|0,k=n>=0?n:L+n; if(k<0)k=0;"
"  for(;k<L;k++){var v=O[k]; if(v===sv||(v!==v&&sv!==sv))return true;} return false;}});"
"Object.defineProperty(Array.prototype,'indexOf',{writable:true,enumerable:false,configurable:true,value:function indexOf(sv,fi){"
"  var O=Object(this),L=O.length>>>0; if(L===0)return -1;"
"  var n=fi|0,k=n>=0?n:L+n; if(k<0)k=0;"
"  for(;k<L;k++){if(k in O&&O[k]===sv)return k;} return -1;}});"
/* lastIndexOf (=== + hole-skip, reverse) and find/findIndex (PREDICATE) collapse the same way: the C
   builtins run JS_ToBool on an opaque predicate-result -> concrete -> no fork. Self-hosted, the truthiness
   test is OP_if which propagates opacity, so `arr.find(x=>x===input)`-gated code forks + explores. */
"Object.defineProperty(Array.prototype,'lastIndexOf',{writable:true,enumerable:false,configurable:true,value:function lastIndexOf(sv,fi){"
"  var O=Object(this),L=O.length>>>0; if(L===0)return -1;"
"  var n=arguments.length>1?fi|0:L-1,k=n>=0?(n<L-1?n:L-1):L+n;"
"  for(;k>=0;k--){if(k in O&&O[k]===sv)return k;} return -1;}});"
"Object.defineProperty(Array.prototype,'find',{writable:true,enumerable:false,configurable:true,value:function find(cb,thisArg){"
"  if(typeof cb!=='function')throw new TypeError('Array.prototype.find: callback is not a function');"
"  var O=Object(this),L=O.length>>>0;"
"  for(var k=0;k<L;k++){var v=O[k]; if(cb.call(thisArg,v,k,O))return v;} return undefined;}});"
"Object.defineProperty(Array.prototype,'findIndex',{writable:true,enumerable:false,configurable:true,value:function findIndex(cb,thisArg){"
"  if(typeof cb!=='function')throw new TypeError('Array.prototype.findIndex: callback is not a function');"
"  var O=Object(this),L=O.length>>>0;"
"  for(var k=0;k<L;k++){if(cb.call(thisArg,O[k],k,O))return k;} return -1;}});"
/* reduce/reduceRight: siblings of forEach/map, self-hosted for the SAME two reasons — (1) OVERFLOW: the C
   builtin holds the accumulator on the C stack across each callback, so `arr.reduce((_,v)=>recur(v))` C-recurses
   and TRAPS the wasm stack (hard abort, not a catchable throw); as bytecode the callback dispatches via the
   trampolined OP_call, unbounded. (2) OPACITY: the callback runs as bytecode so an opaque accumulator/element
   propagates through it. No thisArg (spec: callback `this` is undefined) -> plain cb(...). */
"Object.defineProperty(Array.prototype,'reduce',{writable:true,enumerable:false,configurable:true,value:function reduce(cb,iv){"
"  if(typeof cb!=='function')throw new TypeError('Array.prototype.reduce: callback is not a function');"
"  var O=Object(this),L=O.length>>>0,k=0,acc;"
"  if(arguments.length>1){acc=iv;}else{while(k<L&&!(k in O))k++;if(k>=L)throw new TypeError('Reduce of empty array with no initial value');acc=O[k++];}"
"  for(;k<L;k++){if(k in O)acc=cb(acc,O[k],k,O);}return acc;}});"
"Object.defineProperty(Array.prototype,'reduceRight',{writable:true,enumerable:false,configurable:true,value:function reduceRight(cb,iv){"
"  if(typeof cb!=='function')throw new TypeError('Array.prototype.reduceRight: callback is not a function');"
"  var O=Object(this),L=O.length>>>0,k=L-1,acc;"
"  if(arguments.length>1){acc=iv;}else{while(k>=0&&!(k in O))k--;if(k<0)throw new TypeError('Reduce of empty array with no initial value');acc=O[k--];}"
"  for(;k>=0;k--){if(k in O)acc=cb(acc,O[k],k,O);}return acc;}});"
/* Array.sort: SELF-HOSTED (iterative bottom-up merge, stable) so the COMPARATOR dispatches via the trampolined
   OP_call — deep comparator recursion is UNBOUNDED, not a C-stack trap. The ordering branch must NOT fork on an
   OPAQUE compare (element order is meaningless for @H/@S; O(n log n) forks would explode): `__isOpaque` (a
   concrete-bool leaf) collapses a meaningless opaque order to 0, while the comparator STILL runs (its emits/side
   effects happen). Concrete comparisons use plain JS operators (spec-correct UTF-16 default order; a concrete
   compare never forks). No nested function (that crashes the trampoline). undefined sorts after all values, holes
   last (comparator never sees them); NaN comparator result -> 0; comparator `this` is undefined; in-place. */
"Object.defineProperty(Array.prototype,'sort',{writable:true,enumerable:false,configurable:true,value:function sort(cmp){"
"  if(cmp!==undefined&&typeof cmp!=='function')throw new TypeError('Array.prototype.sort: comparator is not a function');"
"  var O=Object(this),L=O.length>>>0,items=[],undef=0,holes=0,k;"
"  for(k=0;k<L;k++){if(k in O){var v=O[k];if(v===undefined)undef++;else items.push(v);}else holes++;}"
"  var cf=cmp?function(a,b){var r=cmp(a,b);if(__isOpaque(r))return 0;var d=+r;return d!==d?0:d;}"
"           :function(a,b){if(__isOpaque(a)||__isOpaque(b))return 0;var sa=''+a,sb=''+b;return sa<sb?-1:(sa>sb?1:0);};"
"  var N=items.length,buf=new Array(N),w,lo,mid,hi,i,j,t;"
"  for(w=1;w<N;w*=2){for(lo=0;lo<N;lo+=2*w){mid=lo+w<N?lo+w:N;hi=lo+2*w<N?lo+2*w:N;i=lo;j=mid;t=lo;"
"    while(i<mid&&j<hi){if(cf(items[i],items[j])<=0)buf[t++]=items[i++];else buf[t++]=items[j++];}"
"    while(i<mid)buf[t++]=items[i++];while(j<hi)buf[t++]=items[j++];"
"    for(t=lo;t<hi;t++)items[t]=buf[t];}}"
"  var idx=0;"
"  for(k=0;k<items.length;k++)O[idx++]=items[k];"
"  for(k=0;k<undef;k++)O[idx++]=undefined;"
"  for(k=0;k<holes;k++)delete O[idx++];"
"  return O;}});"
/* String.prototype.replace with a FUNCTION replacer: self-hosted so recursion THROUGH the replacer
   trampolines (unbounded, like reduce) and an opaque match/group propagates through it. Only the function
   path is self-hosted; a STRING replacer delegates to the original C builtin (`_o`, captured in the IIFE so
   no global is polluted) which keeps all the $&/$1/$<name> substitution semantics. Flat while-loop, no nested
   function (that crashes the trampoline). Matches native across 40k randomized regex/string/function cases.
   `repl.apply` is a tail-forward (already trampolined); exec is a leaf C call holding no continuation. */
"(function(){var _o=String.prototype.replace;"
"Object.defineProperty(String.prototype,'replace',{writable:true,enumerable:false,configurable:true,value:function replace(search,repl){"
"  if(typeof repl!=='function')return _o.call(this,search,repl);"
"  var str=String(this);"
"  if(search instanceof RegExp){var g=search.global,out='',last=0,m;if(g)search.lastIndex=0;"
"    while((m=search.exec(str))!==null){var ms=m[0],off=m.index,args=Array.prototype.slice.call(m);args.push(off,str);if(m.groups!==undefined)args.push(m.groups);"
"      out+=str.slice(last,off)+String(repl.apply(undefined,args));last=off+ms.length;if(!g)break;if(ms.length===0)search.lastIndex++;}"
"    return out+str.slice(last);}"
"  var ss=String(search),i=str.indexOf(ss);if(i===-1)return str;"
"  return str.slice(0,i)+String(repl(ss,i,str))+str.slice(i+ss.length);}});})();"
/* JSON.stringify: self-hosted so deep object-graph recursion AND a recursive replacer trampoline (unbounded).
   Q/BS/NL = fromCharCode(34/92/10) keep literal quote/backslash/newline OUT of this C string. __isOpaque
   short-circuits an OPAQUE value to ITSELF (taint PRESERVED — never de-tainted to a placeholder) BEFORE any
   typeof/type-dispatch that would fork on it. Spec-faithful (40k-case node differential vs native): toJSON,
   replacer fn/array, space (number clamped<=10 / string), wrapper unwrap, circular throws, undefined/function
   omitted, valid surrogate pairs literal + lone surrogates escaped. Recursive `ser` (nested-closure non-tail
   recursion — needs the trampoline caller_var_refs fix). */
"(function(){var Q=String.fromCharCode(34),BS=String.fromCharCode(92),NL=String.fromCharCode(10);"
"JSON.stringify=function stringify(value,replacer,space){"
"  var replacerFn=null,propertyList=null;"
"  if(typeof replacer==='function')replacerFn=replacer;"
"  else if(Array.isArray(replacer)){propertyList=[];var seen={};"
"    for(var ri=0;ri<replacer.length;ri++){var rv=replacer[ri],item;"
"      if(typeof rv==='string')item=rv;else if(typeof rv==='number')item=String(rv);"
"      else if(rv&&(rv instanceof String||rv instanceof Number))item=String(rv);else continue;"
"      if(!seen[item]){seen[item]=1;propertyList.push(item);}}}"
"  var gap='';"
"  if(typeof space==='object'&&space!==null){if(space instanceof Number)space=Number(space);else if(space instanceof String)space=String(space);}"
"  if(typeof space==='number'){var sn=space<10?Math.floor(space):10;if(sn>=1)gap=' '.repeat(sn);}"
"  else if(typeof space==='string')gap=space.length<=10?space:space.slice(0,10);"
"  var stack=[],indent='';"
"  function quote(s){var out=Q;for(var i=0;i<s.length;i++){var c=s.charCodeAt(i),ch=s.charAt(i);"
"    if(ch===Q)out+=BS+Q;else if(ch===BS)out+=BS+BS;"
"    else if(c===8)out+=BS+'b';else if(c===12)out+=BS+'f';else if(c===10)out+=BS+'n';else if(c===13)out+=BS+'r';else if(c===9)out+=BS+'t';"
"    else if(c<32){var h=c.toString(16);out+=BS+'u'+'0000'.slice(h.length)+h;}"
"    else if(c>=55296&&c<=57343){var c2=i+1<s.length?s.charCodeAt(i+1):0;"
"      if(c<=56319&&c2>=56320&&c2<=57343){out+=ch+s.charAt(i+1);i++;}else{var h2=c.toString(16);out+=BS+'u'+'0000'.slice(h2.length)+h2;}}"
"    else out+=ch;}return out+Q;}"
"  function ser(key,holder){var value=holder[key];"
"    if(__isOpaque(value)){var _ex=__opaqueExample(value);if(_ex===undefined)return value;value=_ex;}"
"    if(value!==null&&typeof value==='object'&&typeof value.toJSON==='function')value=value.toJSON(key);"
"    if(replacerFn)value=replacerFn.call(holder,key,value);"
"    if(__isOpaque(value)){var _ex=__opaqueExample(value);if(_ex===undefined)return value;value=_ex;}"
"    if(value!==null&&typeof value==='object'){if(value instanceof Number)value=Number(value);else if(value instanceof String)value=String(value);else if(value instanceof Boolean)value=value.valueOf();}"
"    if(value===null)return 'null';if(value===true)return 'true';if(value===false)return 'false';"
"    var t=typeof value;"
"    if(t==='string')return quote(value);"
"    if(t==='number')return isFinite(value)?String(value):'null';"
"    if(t==='bigint')throw new TypeError('Do not know how to serialize a BigInt');"
"    if(t==='object'){"
"      for(var si=0;si<stack.length;si++)if(stack[si]===value)throw new TypeError('Converting circular structure to JSON');"
"      stack.push(value);var stepback=indent;indent+=gap;var res;"
"      if(Array.isArray(value)){var parts=[];for(var ai=0;ai<value.length;ai++){var e=ser(String(ai),value);"
"        if(__isOpaque(e))parts.push(e);else parts.push(e===undefined?'null':e);}"
"        if(parts.length===0)res='[]';else if(gap==='')res='['+parts.join(',')+']';else res='['+NL+indent+parts.join(','+NL+indent)+NL+stepback+']';}"
"      else{var keys=propertyList||Object.keys(value),members=[];"
"        for(var ki=0;ki<keys.length;ki++){var pk=keys[ki],ps=ser(pk,value);"
"          if(__isOpaque(ps)||ps!==undefined)members.push(quote(pk)+(gap===''?':':': ')+ps);}"
"        if(members.length===0)res='{}';else if(gap==='')res='{'+members.join(',')+'}';else res='{'+NL+indent+members.join(','+NL+indent)+NL+stepback+'}';}"
"      stack.pop();indent=stepback;return res;}"
"    return undefined;}"
"  var wrapper={};wrapper['']=value;return ser('',wrapper);};})();";
/* In-place fetch (pivot M2): a reply-consume (r.json()/r.text()) with no concrete body PARKS — an
   unresolved promise whose resolve fn is held here with the (concrete) url. qjs_step returns NEED_FETCH
   while any are pending; the offscreen safe-fetches and qjs_provide()s the body, resolving the promise
   so the flow resumes IN PLACE (no re-instantiate, no re-run). Node CLI resolves them opaque (shapes). */
typedef struct { JSValue rf; char *url; int is_json; } Pending;
static Pending *g_pending = NULL;
static int g_pending_n = 0, g_pending_cap = 0;
static void pending_add(JSContext *ctx, JSValue rf, const char *url, int is_json) {
    if (g_pending_n >= g_pending_cap) {
        int nc = g_pending_cap ? g_pending_cap * 2 : 32;
        Pending *n = realloc(g_pending, (size_t)nc * sizeof(Pending));
        if (!n) { JS_FreeValue(ctx, rf); return; }
        g_pending = n; g_pending_cap = nc;
    }
    g_pending[g_pending_n].rf = rf; g_pending[g_pending_n].url = url ? strdup(url) : NULL; g_pending[g_pending_n].is_json = is_json;
    g_pending_n++;
}
/* Chunk-load pending (M3): an injected <script src> to fetch + eval IN PLACE (no promise; nothing awaits
   it — like the browser's async script load). The offscreen provides the body; qjs_provide evals it. */
static char **g_chunk_pending = NULL;
static int g_chunk_n = 0, g_chunk_cap = 0;
static void chunk_pending_add(const char *url) {
    if (!url) return;
    for (int i = 0; i < g_chunk_n; i++) if (strcmp(g_chunk_pending[i], url) == 0) return;   /* dedup */
    if (g_chunk_n >= g_chunk_cap) { int nc = g_chunk_cap ? g_chunk_cap * 2 : 16; char **n = realloc(g_chunk_pending, (size_t)nc * sizeof(char *)); if (!n) return; g_chunk_pending = n; g_chunk_cap = nc; }
    g_chunk_pending[g_chunk_n++] = strdup(url);
}

/* ---- ESM static-import graph (browser-faithful) ----
   A module's `import ... from "X"` is resolved SYNCHRONOUSLY at link time, but our fetch is async
   (chunk_pending -> host -> qjs_provide). So: the module loader compiles a dep from cached source if we
   have it; otherwise it requests the chunk (like a browser fetches the graph) and fails THIS link, and the
   importing module is parked in g_pendmod, retried each time a chunk arrives, until the whole graph links. */
typedef struct { char *url; char *src; size_t len; } ModSrc;
static ModSrc *g_modsrc = NULL; static int g_modsrc_n = 0, g_modsrc_cap = 0;
static void modsrc_put(const char *url, const char *src, size_t len) {
    for (int i = 0; i < g_modsrc_n; i++) if (strcmp(g_modsrc[i].url, url) == 0) return;   /* first source wins */
    if (g_modsrc_n >= g_modsrc_cap) { int nc = g_modsrc_cap ? g_modsrc_cap * 2 : 8; ModSrc *n = realloc(g_modsrc, (size_t)nc * sizeof(ModSrc)); if (!n) return; g_modsrc = n; g_modsrc_cap = nc; }
    char *s = malloc(len + 1); if (!s) return; memcpy(s, src, len); s[len] = 0;
    g_modsrc[g_modsrc_n].url = strdup(url); g_modsrc[g_modsrc_n].src = s; g_modsrc[g_modsrc_n].len = len; g_modsrc_n++;
}
static ModSrc *modsrc_get(const char *url) { for (int i = 0; i < g_modsrc_n; i++) if (strcmp(g_modsrc[i].url, url) == 0) return &g_modsrc[i]; return NULL; }
/* URLs discovered as STATIC-import deps: link them IN-GRAPH (loader compiles them), never eval standalone
   (that would double-run their side effects — the loader already links+runs them). */
static char **g_moddep = NULL; static int g_moddep_n = 0, g_moddep_cap = 0;
static void moddep_add(const char *u) { for (int i = 0; i < g_moddep_n; i++) if (strcmp(g_moddep[i], u) == 0) return; if (g_moddep_n >= g_moddep_cap) { int nc = g_moddep_cap ? g_moddep_cap * 2 : 8; char **n = realloc(g_moddep, (size_t)nc * sizeof(char *)); if (!n) return; g_moddep = n; g_moddep_cap = nc; } g_moddep[g_moddep_n++] = strdup(u); }
static int is_moddep(const char *u) { for (int i = 0; i < g_moddep_n; i++) if (strcmp(g_moddep[i], u) == 0) return 1; return 0; }
/* Modules whose link is deferred until their imported chunks arrive (source copy, retried on each provide). */
typedef struct { char *src; size_t len; } PendMod;
static PendMod *g_pendmod = NULL; static int g_pendmod_n = 0, g_pendmod_cap = 0;
static int g_modseq = 0;   /* unique module names so a retry never collides with a prior failed link */
static void pendmod_add(const char *src, size_t len) {
    if (g_pendmod_n >= g_pendmod_cap) { int nc = g_pendmod_cap ? g_pendmod_cap * 2 : 8; PendMod *n = realloc(g_pendmod, (size_t)nc * sizeof(PendMod)); if (!n) return; g_pendmod = n; g_pendmod_cap = nc; }
    char *s = malloc(len + 1); if (!s) return; memcpy(s, src, len); s[len] = 0;
    g_pendmod[g_pendmod_n].src = s; g_pendmod[g_pendmod_n].len = len; g_pendmod_n++;
}

/* wrap val in an already-RESOLVED promise (consumes val) so `await`/`.then` chains continue synchronously. */
static JSValue js_resolved(JSContext *ctx, JSValue val)
{
    JSValue rf[2]; JSValue promise = JS_NewPromiseCapability(ctx, rf);
    if (!JS_IsException(promise)) {
        JSValue rr = JS_Call(ctx, rf[0], JS_UNDEFINED, 1, &val); JS_FreeValue(ctx, rr);
        JS_FreeValue(ctx, rf[0]); JS_FreeValue(ctx, rf[1]);
    }
    JS_FreeValue(ctx, val);
    return promise;
}
/* Response body accessor for .blob/.arrayBuffer/.formData (binary/complex bodies): opaque, wrapped in a
   resolved promise so the fetch chain CONTINUES. NOTE json()/text() do NOT come here — resp_consume LOADS
   the real same-origin reply (fromReply, one-per-endpoint) so config/data fields are CONCRETE, not shapes:
   a same-origin reply is TRUSTED app data to load, not external-input opacity. */
static JSValue js_resp_body(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{ return js_resolved(ctx, JS_DupValue(ctx, g_opaque)); }
/* the concrete reply body injected onto this Response (fromReply), or JS_UNDEFINED */
static JSValue resp_body_str(JSContext *ctx, JSValueConst this_val) {
    JSValue b = JS_GetPropertyStr(ctx, this_val, "__body");
    if (JS_IsString(b)) return b;
    JS_FreeValue(ctx, b); return JS_UNDEFINED;
}
/* the bundle is CONSUMING this reply's body but we have no concrete one -> a real bounded GET to this
   (concrete) url is worth firing (the offscreen does it, one-per-endpoint). Emitted so the bridge fetches
   ONLY consumed-reply endpoints, not every GET (a blind GET can hit /logout, /delete?id=). */
static void reply_note_wanted(JSContext *ctx, JSValueConst this_val) {
    JSValue u = JS_GetPropertyStr(ctx, this_val, "url");
    const char *s = JS_IsString(u) ? JS_ToCString(ctx, u) : NULL;
    if (s && s[0] && !has_hole(s)) { printf("@REPLYWANT %s\n", s); fflush(stdout); }
    if (s) JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, u);
}
/* CONCOLIC leaf-wrap: a trusted loaded reply is ONE value whose STRUCTURE stays REAL (Object.keys/map/for-in/
   array index all work natively) but whose STRING/BOOL leaves become concolic — each FORKS at a branch (so a
   role/flag gate reaches the gated endpoint) AND carries its real value as the example (so a gate-INDEPENDENT
   field keeps its real value on the forced arm: /api/billing/enterprise/acme-42, not /{}). NUMBERS stay
   concrete — an opaque numeric would make `for(i<n)` / `.length` loops infinite (the known catastrophe). */
/* parse a concrete reply body into the value json()/text() should resolve to. String/bool leaves become
   concolic (fork + real example) via the SHARED JS_ConcolicWrap — the ONE home, also used by JSON.parse of
   an SSR data blob (same trust boundary: loaded same-origin app data). */
static JSValue reply_value(JSContext *ctx, JSValueConst body_str, int is_json) {
    if (!is_json) return JS_DupValue(ctx, body_str);   /* text: unchanged (avoid regressing text->JSON.parse) */
    size_t len = 0; const char *s = JS_ToCStringLen(ctx, &len, body_str);
    JSValue parsed = s ? JS_ParseJSON(ctx, s, len, "<reply>") : JS_EXCEPTION;
    if (s) JS_FreeCString(ctx, s);
    if (JS_IsException(parsed)) { JSValue e = JS_GetException(ctx); JS_FreeValue(ctx, e); return JS_DupValue(ctx, g_opaque); }
    return JS_ConcolicWrap(ctx, parsed, "reply");   /* structure real; string/bool leaves concolic (fork + real example) */
}
/* r.json()/r.text(): concrete body -> resolve it; else PARK (unresolved promise + pending) if the url is
   concrete (fetchable), so the offscreen provides the real reply IN PLACE; opaque if the url is a shape. */
static JSValue resp_consume(JSContext *ctx, JSValueConst this_val, int is_json) {
    JSValue body = resp_body_str(ctx, this_val);
    if (JS_IsString(body)) { JSValue v = reply_value(ctx, body, is_json); JS_FreeValue(ctx, body); return js_resolved(ctx, v); }
    JS_FreeValue(ctx, body);
    JSValue u = JS_GetPropertyStr(ctx, this_val, "url");
    const char *url = JS_IsString(u) ? JS_ToCString(ctx, u) : NULL;
    JSValue result;
    if (url && url[0] && !has_hole(url)) {   /* concrete url -> park for a real reply */
        JSValue rf[2]; JSValue promise = JS_NewPromiseCapability(ctx, rf);
        if (!JS_IsException(promise)) { pending_add(ctx, JS_DupValue(ctx, rf[0]), url, is_json); JS_FreeValue(ctx, rf[0]); JS_FreeValue(ctx, rf[1]); result = promise; }
        else result = js_resolved(ctx, JS_DupValue(ctx, g_opaque));
    } else {
        result = js_resolved(ctx, JS_DupValue(ctx, g_opaque));   /* shape url: not fetchable */
    }
    if (url) JS_FreeCString(ctx, url);
    JS_FreeValue(ctx, u);
    return result;
}
static JSValue js_resp_json(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) { return resp_consume(ctx, this_val, 1); }
static JSValue js_resp_text(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) { return resp_consume(ctx, this_val, 0); }
/* build a fetch Response whose identity is concrete (ok/status/url) but whose BODY is opaque. */
static JSValue make_response(JSContext *ctx, const char *url)
{
    JSValue resp = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, resp, "url", JS_NewString(ctx, url ? url : ""));
    JS_SetPropertyStr(ctx, resp, "ok", JS_TRUE);
    JS_SetPropertyStr(ctx, resp, "status", JS_NewInt32(ctx, 200));
    JS_SetPropertyStr(ctx, resp, "statusText", JS_NewString(ctx, "OK"));
    JS_SetPropertyStr(ctx, resp, "json", JS_NewCFunction(ctx, js_resp_json, "json", 0));
    JS_SetPropertyStr(ctx, resp, "text", JS_NewCFunction(ctx, js_resp_text, "text", 0));
    JS_SetPropertyStr(ctx, resp, "blob", JS_NewCFunction(ctx, js_resp_body, "blob", 0));
    JS_SetPropertyStr(ctx, resp, "arrayBuffer", JS_NewCFunction(ctx, js_resp_body, "arrayBuffer", 0));
    JS_SetPropertyStr(ctx, resp, "formData", JS_NewCFunction(ctx, js_resp_body, "formData", 0));
    {   /* headers.get(name) -> opaque (a response header is external input) */
        JSValue h = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, h, "get", JS_NewCFunction(ctx, js_opaque_stub, "get", 1));
        JS_SetPropertyStr(ctx, resp, "headers", h);
    }
    /* fromReply: inject the concrete reply body for THIS url (r.json()/r.text() then return real data) */
    if (url && JS_IsObject(g_reply_table)) {
        JSValue b = JS_GetPropertyStr(ctx, g_reply_table, url);
        if (JS_IsString(b)) JS_SetPropertyStr(ctx, resp, "__body", b);   /* consumes b */
        else JS_FreeValue(ctx, b);
    }
    return resp;
}
static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
/* Percent-decode a URL component (decodeURIComponent semantics: %XX -> byte; '+' stays '+'; a
   malformed % is left literal; control chars flattened to space so they can't break a line) into a
   NUL-terminated buffer. The ENGINE owns URL parsing — the host must never re-split the URL string. */
static void url_pct_decode(const char *s, size_t n, char *out, size_t outcap) {
    size_t o = 0;
    for (size_t i = 0; i < n && o + 1 < outcap; i++) {
        int hi, lo;
        if (s[i] == '%' && i + 2 < n
            && (hi = hexval(s[i+1])) >= 0 && (lo = hexval(s[i+2])) >= 0) {
            int c = (hi << 4) | lo; out[o++] = (c == '\n' || c == '\r') ? ' ' : (char)c; i += 2;
        } else {
            out[o++] = (s[i] == '\n' || s[i] == '\r') ? ' ' : s[i];
        }
    }
    out[o] = 0;
}
/* Build query-param objects ({name,location:"query",validValues:[value?]}) from a COMPUTED url onto the
   endpoint's params array. The engine owns URL parsing (Lexbor-canonical); the host never re-splits the
   URL string. A hole value ({search}) passes through literally (opacity marker, decoded downstream). */
static void build_query_params(JSContext *ctx, const char *url, JSValueConst params) {
    if (!url) return;
    const char *q = strchr(url, '?'); if (!q) return;
    q++;
    const char *end = strchr(q, '#'); if (!end) end = q + strlen(q);
    uint32_t idx = 0;
    for (const char *p = q; p < end; ) {
        const char *amp = memchr(p, '&', (size_t)(end - p)); if (!amp) amp = end;
        const char *eq = memchr(p, '=', (size_t)(amp - p));
        const char *ne = eq ? eq : amp;
        const char *vb = eq ? eq + 1 : amp;
        char nbuf[256], vbuf[512];
        url_pct_decode(p, (size_t)(ne - p), nbuf, sizeof nbuf);
        url_pct_decode(vb, (size_t)(amp - vb), vbuf, sizeof vbuf);
        if (nbuf[0]) {
            JSValue po = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, po, "name", JS_NewString(ctx, nbuf));
            JS_SetPropertyStr(ctx, po, "location", JS_NewString(ctx, "query"));
            JSValue vv = JS_NewArray(ctx);
            if (vbuf[0]) JS_SetPropertyUint32(ctx, vv, 0, JS_NewString(ctx, vbuf));   /* eurl already value-solved upstream */
            JS_SetPropertyStr(ctx, po, "validValues", vv);
            JS_SetPropertyUint32(ctx, params, idx++, po);
        }
        p = (amp < end) ? amp + 1 : end;
    }
}
static JSValue js_noop(JSContext *ctx, JSValueConst t, int c, JSValueConst *v);            /* fwd */
static JSValue js_add_listener(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv);/* fwd */
/* The shared @H sink: record one endpoint (consumes `ep`) + raise the running flow's value. A CANDIDATE flow
   carries a concrete @S breakout PAYLOAD, so its request URLs are @S artifacts, NOT real @H — only OPAQUE
   flows emit. fetch AND XMLHttpRequest funnel through here so XHR-based apps are learned like fetch ones. */
static void record_endpoint(JSContext *ctx, JSValue ep) {
    if (JS_IsArray(g_endpoints) && !g_candidate) {
        uint32_t n = 0; JSValue lv = JS_GetPropertyStr(ctx, g_endpoints, "length"); JS_ToUint32(ctx, &n, lv); JS_FreeValue(ctx, lv);
        JS_SetPropertyUint32(ctx, g_endpoints, n, ep);   /* consumes ep */
        g_emit_total++;
    } else {
        JS_FreeValue(ctx, ep);
    }
    if (g_running) { g_cur_val += 1.0; if (g_cur_flow) { g_cur_flow->val = g_cur_val; g_cur_flow->cpu = 0; } }
}
/* XMLHttpRequest — a PRIMARY request mechanism (many apps use it over fetch). Missing, `new XMLHttpRequest()`
   threw and lost every XHR endpoint. open() stashes method+url; send() emits the endpoint through the shared
   sink (concolic-example URL + query params + body), like fetch. Response fields are opaque (external input). */
/* Capture request header name:value pairs into ep.headers (required-headers replay spec). Reads a plain
   object OR a `new Headers()`'s __fields, and a concolic value's EXAMPLE (`'Bearer '+token` -> the real token).
   ONE home for fetch + XHR (no duplication). */
static void capture_headers(JSContext *ctx, JSValueConst ep, JSValueConst hdrs) {
    if (!JS_IsObject(hdrs) || JS_IsOpaque(hdrs)) return;
    JSValue hf = JS_GetPropertyStr(ctx, hdrs, "__fields");
    JSValueConst hsrc = JS_IsObject(hf) ? (JSValueConst)hf : hdrs;
    JSValue hobj = JS_NewObject(ctx); int any = 0;
    JSPropertyEnum *tab = NULL; uint32_t hn = 0;
    if (JS_GetOwnPropertyNames(ctx, &tab, &hn, hsrc, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
        for (uint32_t hi = 0; hi < hn; hi++) {
            const char *hk = JS_AtomToCString(ctx, tab[hi].atom);
            JSValue hv = JS_GetProperty(ctx, hsrc, tab[hi].atom);
            JSValue hex = JS_IsOpaque(hv) ? JS_OpaqueExample(ctx, hv) : JS_UNDEFINED;
            const char *hvs = !JS_IsUndefined(hex) ? JS_ToCString(ctx, hex) : JS_ToCString(ctx, hv);
            JS_FreeValue(ctx, hex);
            if (hk && hvs) { JS_SetPropertyStr(ctx, hobj, hk, JS_NewString(ctx, hvs)); any = 1; }
            if (hk) JS_FreeCString(ctx, hk);
            if (hvs) JS_FreeCString(ctx, hvs);
            JS_FreeValue(ctx, hv);
        }
        JS_FreePropertyEnum(ctx, tab, hn);
    }
    JS_FreeValue(ctx, hf);
    if (any) JS_SetPropertyStr(ctx, ep, "headers", hobj); else JS_FreeValue(ctx, hobj);
}
static JSValue js_xhr_setheader(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc >= 2) {   /* store into __headers so js_xhr_send captures the auth/CSRF/custom headers */
        JSValue h = JS_GetPropertyStr(ctx, this_val, "__headers");
        if (!JS_IsObject(h)) { JS_FreeValue(ctx, h); h = JS_NewObject(ctx); JS_SetPropertyStr(ctx, this_val, "__headers", JS_DupValue(ctx, h)); }
        const char *k = JS_ToCString(ctx, argv[0]);
        if (k) { JS_SetPropertyStr(ctx, h, k, JS_DupValue(ctx, argv[1])); JS_FreeCString(ctx, k); }
        JS_FreeValue(ctx, h);
    }
    return JS_UNDEFINED;
}
static JSValue js_xhr_open(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc >= 1) JS_SetPropertyStr(ctx, this_val, "__method", JS_DupValue(ctx, argv[0]));
    if (argc >= 2) JS_SetPropertyStr(ctx, this_val, "__url", JS_DupValue(ctx, argv[1]));
    return JS_UNDEFINED;
}
static JSValue js_xhr_send(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    JSValue mv = JS_GetPropertyStr(ctx, this_val, "__method");
    JSValue uv = JS_GetPropertyStr(ctx, this_val, "__url");
    const char *method = JS_IsString(mv) ? JS_ToCString(ctx, mv) : NULL;
    char *url = NULL;
    { JSValue exurl = JS_OpaqueExample(ctx, uv); const char *u = NULL;   /* concolic URL -> real computed value */
      if (!JS_IsUndefined(exurl)) u = JS_ToCString(ctx, exurl);
      if (!u && (JS_IsString(uv) || JS_IsOpaque(uv))) u = JS_ToCString(ctx, uv);
      if (u) { url = strdup(u); JS_FreeCString(ctx, u); }
      JS_FreeValue(ctx, exurl); }
    if (url) {
        char *usolved = url_solve_holes(ctx, url); const char *eurl = usolved ? usolved : url;
        JSValue ep = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, ep, "method", JS_NewString(ctx, method ? method : "GET"));
        JS_SetPropertyStr(ctx, ep, "url", JS_NewString(ctx, eurl));
        JS_SetPropertyStr(ctx, ep, "source", JS_NewString(ctx, "ast_analysis"));
        JSValue params = JS_NewArray(ctx); build_query_params(ctx, eurl, params); JS_SetPropertyStr(ctx, ep, "params", params);
        if (argc >= 1 && !JS_IsUndefined(argv[0]) && !JS_IsNull(argv[0])) {
            const char *bs = JS_ToCString(ctx, argv[0]);
            if (bs && bs[0]) { char *bsolved = url_solve_holes(ctx, bs); JS_SetPropertyStr(ctx, ep, "body", JS_NewString(ctx, bsolved ? bsolved : bs)); free(bsolved); }
            if (bs) JS_FreeCString(ctx, bs);
        }
        { JSValue h = JS_GetPropertyStr(ctx, this_val, "__headers"); capture_headers(ctx, ep, h); JS_FreeValue(ctx, h); }
        record_endpoint(ctx, ep);
        free(usolved); free(url);
    }
    if (method) JS_FreeCString(ctx, method);
    JS_FreeValue(ctx, mv); JS_FreeValue(ctx, uv);
    return JS_UNDEFINED;
}
static JSValue js_xhr_ctor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv) {
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "open", JS_NewCFunction(ctx, js_xhr_open, "open", 2));
    JS_SetPropertyStr(ctx, o, "send", JS_NewCFunction(ctx, js_xhr_send, "send", 1));
    JS_SetPropertyStr(ctx, o, "setRequestHeader", JS_NewCFunction(ctx, js_xhr_setheader, "setRequestHeader", 2));
    JS_SetPropertyStr(ctx, o, "abort", JS_NewCFunction(ctx, js_noop, "abort", 0));
    JS_SetPropertyStr(ctx, o, "addEventListener", JS_NewCFunction(ctx, js_add_listener, "addEventListener", 2));   /* onload handler -> driven */
    JS_SetPropertyStr(ctx, o, "removeEventListener", JS_NewCFunction(ctx, js_noop, "removeEventListener", 2));
    JS_SetPropertyStr(ctx, o, "getResponseHeader", JS_NewCFunction(ctx, js_opaque_stub, "getResponseHeader", 1));
    JS_SetPropertyStr(ctx, o, "getAllResponseHeaders", JS_NewCFunction(ctx, js_opaque_stub, "getAllResponseHeaders", 0));
    JS_SetPropertyStr(ctx, o, "responseText", JS_DupValue(ctx, g_opaque));   /* response = external input */
    JS_SetPropertyStr(ctx, o, "response", JS_DupValue(ctx, g_opaque));
    JS_SetPropertyStr(ctx, o, "status", JS_NewInt32(ctx, 200));
    JS_SetPropertyStr(ctx, o, "readyState", JS_NewInt32(ctx, 4));
    return o;
}
/* Observers (Intersection/Mutation/Resize/Performance): missing constructors threw. A no-op object (observe/
   disconnect/etc.) prevents the throw; the callback passed to the constructor is an uncalled function the
   orphan driver reaches, so its endpoints/sinks are still learned. */
static JSValue js_observer_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv) {
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "observe", JS_NewCFunction(ctx, js_noop, "observe", 2));
    JS_SetPropertyStr(ctx, o, "unobserve", JS_NewCFunction(ctx, js_noop, "unobserve", 1));
    JS_SetPropertyStr(ctx, o, "disconnect", JS_NewCFunction(ctx, js_noop, "disconnect", 0));
    JS_SetPropertyStr(ctx, o, "takeRecords", JS_NewCFunction(ctx, js_noop, "takeRecords", 0));
    return o;
}
static JSValue js_get_computed_style(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) {
    JSValue o = JS_NewObject(ctx); JS_SetPropertyStr(ctx, o, "getPropertyValue", JS_NewCFunction(ctx, js_opaque_stub, "getPropertyValue", 1)); return o;
}
/* Intl.NumberFormat/DateTimeFormat/etc.: `new Intl.X().format(v)` threw (not built into this quickjs). A
   constructor returning {format/…} whose results are opaque (locale-formatted external input). */
static JSValue js_intl_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv) {
    JSValue o = JS_NewObject(ctx);
    const char *m[] = { "format", "formatToParts", "formatRange", "formatRangeToParts", "resolvedOptions", "select", "compare", "of" };  /* .of = Intl.DisplayNames */
    for (int i = 0; i < 8; i++) JS_SetPropertyStr(ctx, o, m[i], JS_NewCFunction(ctx, js_opaque_stub, m[i], 1));
    return o;
}
/* WebSocket / EventSource: `new X(url)` — the url IS an endpoint (WS/SSE handshake is a GET), emit it. The
   object has send/close/addEventListener (onmessage handler -> driven) so real usage never throws. */
/* FormData: a real object recording append()/set() fields, so a `fetch(url,{body:fd})` POST surfaces the
   real request params (name=value&…) instead of an opaque body. Opaque (external-input) field values keep
   their shape. js_fetch's JS_ToCString(body) invokes toString -> the serialization. */
static JSValue js_formdata_append(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc >= 2) {
        JSValue f = JS_GetPropertyStr(ctx, this_val, "__fields");
        if (!JS_IsObject(f)) { JS_FreeValue(ctx, f); f = JS_NewObject(ctx); JS_SetPropertyStr(ctx, this_val, "__fields", JS_DupValue(ctx, f)); }
        const char *k = JS_ToCString(ctx, argv[0]);
        if (k) { JS_SetPropertyStr(ctx, f, k, JS_DupValue(ctx, argv[1])); JS_FreeCString(ctx, k); }
        JS_FreeValue(ctx, f);
    }
    return JS_UNDEFINED;
}
/* FormData shares the CONCOLIC serializer with URLSearchParams (js_sp_tostring, defined below): both store
   __fields and serialize to "k=v&…" carrying the concrete example when known, the shape otherwise. So a
   `fetch(url,{body:fd})` POST body surfaces REAL field values (the moat wants request-body examples, and a
   POST is never fired to learn -> the example can only come from this forced-exec serialization). */
static JSValue js_sp_tostring(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
static JSValue js_formdata_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv) {
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "__fields", JS_NewObject(ctx));
    JS_SetPropertyStr(ctx, o, "append", JS_NewCFunction(ctx, js_formdata_append, "append", 2));
    JS_SetPropertyStr(ctx, o, "set", JS_NewCFunction(ctx, js_formdata_append, "set", 2));
    JS_SetPropertyStr(ctx, o, "get", JS_NewCFunction(ctx, js_opaque_stub, "get", 1));
    JS_SetPropertyStr(ctx, o, "has", JS_NewCFunction(ctx, js_opaque_stub, "has", 1));
    JS_SetPropertyStr(ctx, o, "delete", JS_NewCFunction(ctx, js_noop, "delete", 1));
    JS_SetPropertyStr(ctx, o, "toString", JS_NewCFunction(ctx, js_sp_tostring, "toString", 0));
    return o;
}
/* navigator.sendBeacon(url, data): a real REQUEST (analytics/telemetry endpoint) — emit @H like fetch. */
static JSValue js_send_beacon(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc >= 1) {
        char *url = NULL; JSValue ex = JS_OpaqueExample(ctx, argv[0]); const char *u = NULL;
        if (!JS_IsUndefined(ex)) u = JS_ToCString(ctx, ex);
        if (!u) u = JS_ToCString(ctx, argv[0]);
        if (u) { url = strdup(u); JS_FreeCString(ctx, u); }
        JS_FreeValue(ctx, ex);
        if (url) {
            char *usolved = url_solve_holes(ctx, url); const char *eurl = usolved ? usolved : url;
            JSValue ep = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, ep, "method", JS_NewString(ctx, "POST"));
            JS_SetPropertyStr(ctx, ep, "url", JS_NewString(ctx, eurl));
            JS_SetPropertyStr(ctx, ep, "source", JS_NewString(ctx, "ast_analysis"));
            JSValue params = JS_NewArray(ctx); build_query_params(ctx, eurl, params); JS_SetPropertyStr(ctx, ep, "params", params);
            if (argc >= 2 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
                const char *bs = JS_ToCString(ctx, argv[1]);
                if (bs && bs[0]) { char *bsolved = url_solve_holes(ctx, bs); JS_SetPropertyStr(ctx, ep, "body", JS_NewString(ctx, bsolved ? bsolved : bs)); free(bsolved); }
                if (bs) JS_FreeCString(ctx, bs);
            }
            record_endpoint(ctx, ep); free(usolved); free(url);
        }
    }
    return JS_TRUE;
}
static JSValue js_ws_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv) {
    if (argc >= 1) {
        char *url = NULL; JSValue ex = JS_OpaqueExample(ctx, argv[0]); const char *u = NULL;
        if (!JS_IsUndefined(ex)) u = JS_ToCString(ctx, ex);
        if (!u) u = JS_ToCString(ctx, argv[0]);
        if (u) { url = strdup(u); JS_FreeCString(ctx, u); }
        JS_FreeValue(ctx, ex);
        if (url) {
            char *usolved = url_solve_holes(ctx, url); const char *eurl = usolved ? usolved : url;
            JSValue ep = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, ep, "method", JS_NewString(ctx, "GET"));
            JS_SetPropertyStr(ctx, ep, "url", JS_NewString(ctx, eurl));
            JS_SetPropertyStr(ctx, ep, "source", JS_NewString(ctx, "ast_analysis"));
            JSValue params = JS_NewArray(ctx); build_query_params(ctx, eurl, params); JS_SetPropertyStr(ctx, ep, "params", params);
            record_endpoint(ctx, ep); free(usolved); free(url);
        }
    }
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "send", JS_NewCFunction(ctx, js_noop, "send", 1));
    JS_SetPropertyStr(ctx, o, "close", JS_NewCFunction(ctx, js_noop, "close", 0));
    JS_SetPropertyStr(ctx, o, "addEventListener", JS_NewCFunction(ctx, js_add_listener, "addEventListener", 2));
    JS_SetPropertyStr(ctx, o, "removeEventListener", JS_NewCFunction(ctx, js_noop, "removeEventListener", 2));
    JS_SetPropertyStr(ctx, o, "readyState", JS_NewInt32(ctx, 1));
    return o;
}
/* Worker / SharedWorker(url): the worker SCRIPT is code with its OWN endpoints -> fetch + analyze it like a
   <script src> chunk (chunk_pending_add). The object exposes postMessage/terminate + addEventListener (the
   onmessage handler is driven) and a .port (SharedWorker). */
static JSValue js_worker_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv) {
    if (argc >= 1) {
        char *url = NULL; JSValue ex = JS_OpaqueExample(ctx, argv[0]); const char *u = NULL;
        if (!JS_IsUndefined(ex)) u = JS_ToCString(ctx, ex);
        if (!u) u = JS_ToCString(ctx, argv[0]);
        if (u) { url = strdup(u); JS_FreeCString(ctx, u); }
        JS_FreeValue(ctx, ex);
        if (url) { if (!has_hole(url)) chunk_pending_add(url); free(url); }   /* -> host fetch + engine analyze */
    }
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "postMessage", JS_NewCFunction(ctx, js_noop, "postMessage", 1));
    JS_SetPropertyStr(ctx, o, "terminate", JS_NewCFunction(ctx, js_noop, "terminate", 0));
    JS_SetPropertyStr(ctx, o, "addEventListener", JS_NewCFunction(ctx, js_add_listener, "addEventListener", 2));
    JS_SetPropertyStr(ctx, o, "removeEventListener", JS_NewCFunction(ctx, js_noop, "removeEventListener", 2));
    JSValue port = JS_NewObject(ctx);   /* SharedWorker.port (MessagePort) */
    JS_SetPropertyStr(ctx, port, "postMessage", JS_NewCFunction(ctx, js_noop, "postMessage", 1));
    JS_SetPropertyStr(ctx, port, "start", JS_NewCFunction(ctx, js_noop, "start", 0));
    JS_SetPropertyStr(ctx, port, "close", JS_NewCFunction(ctx, js_noop, "close", 0));
    JS_SetPropertyStr(ctx, port, "addEventListener", JS_NewCFunction(ctx, js_add_listener, "addEventListener", 2));
    JS_SetPropertyStr(ctx, o, "port", port);
    return o;
}
/* MessagePort / MessageChannel / BroadcastChannel: postMessage-style objects whose message handler is driven. */
static JSValue js_msgport_new(JSContext *ctx) {
    JSValue p = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, p, "postMessage", JS_NewCFunction(ctx, js_noop, "postMessage", 1));
    JS_SetPropertyStr(ctx, p, "start", JS_NewCFunction(ctx, js_noop, "start", 0));
    JS_SetPropertyStr(ctx, p, "close", JS_NewCFunction(ctx, js_noop, "close", 0));
    JS_SetPropertyStr(ctx, p, "addEventListener", JS_NewCFunction(ctx, js_add_listener, "addEventListener", 2));
    JS_SetPropertyStr(ctx, p, "removeEventListener", JS_NewCFunction(ctx, js_noop, "removeEventListener", 2));
    return p;
}
static JSValue js_msg_channel_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv) {
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "port1", js_msgport_new(ctx));
    JS_SetPropertyStr(ctx, o, "port2", js_msgport_new(ctx));
    return o;
}
static JSValue js_broadcast_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv) {
    JSValue o = js_msgport_new(ctx);   /* same shape (postMessage/close/addEventListener) */
    JS_SetPropertyStr(ctx, o, "name", (argc >= 1) ? JS_ToString(ctx, argv[0]) : JS_NewString(ctx, ""));
    return o;
}
static JSValue js_notification_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv) {
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "close", JS_NewCFunction(ctx, js_noop, "close", 0));
    JS_SetPropertyStr(ctx, o, "addEventListener", JS_NewCFunction(ctx, js_add_listener, "addEventListener", 2));
    return o;
}
static JSValue js_notif_request_perm(JSContext *ctx, JSValueConst t, int c, JSValueConst *v)
{ return js_resolved(ctx, JS_NewString(ctx, "default")); }   /* Notification.requestPermission() -> Promise<"default"> */
/* AbortSignal.timeout(ms) / .any([...]) / .abort(): a live-signal object (aborted=false so a branch on it forks). */
static JSValue js_abortsignal_make(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) {
    JSValue s = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, s, "aborted", JS_FALSE);
    JS_SetPropertyStr(ctx, s, "reason", JS_UNDEFINED);
    JS_SetPropertyStr(ctx, s, "addEventListener", JS_NewCFunction(ctx, js_add_listener, "addEventListener", 2));
    JS_SetPropertyStr(ctx, s, "removeEventListener", JS_NewCFunction(ctx, js_noop, "removeEventListener", 2));
    JS_SetPropertyStr(ctx, s, "throwIfAborted", JS_NewCFunction(ctx, js_noop, "throwIfAborted", 0));
    return s;
}
/* indexedDB: the page's own DB (bundles cache tokens/state). A full async model is a separate task; this is a
   NON-throwing object graph whose stored VALUES are opaque (external state -> an auth gate on them forks). */
static JSValue js_idb_request(JSContext *ctx, JSValue result) {
    JSValue r = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, r, "result", result);
    JS_SetPropertyStr(ctx, r, "error", JS_NULL);
    JS_SetPropertyStr(ctx, r, "readyState", JS_NewString(ctx, "done"));
    JS_SetPropertyStr(ctx, r, "addEventListener", JS_NewCFunction(ctx, js_add_listener, "addEventListener", 2));
    JS_SetPropertyStr(ctx, r, "onsuccess", JS_NULL);
    JS_SetPropertyStr(ctx, r, "onerror", JS_NULL);
    JS_SetPropertyStr(ctx, r, "onupgradeneeded", JS_NULL);
    return r;
}
static JSValue js_idb_opaque_req(JSContext *ctx, JSValueConst t, int c, JSValueConst *v)   /* get/getAll/... -> request, result opaque */
{ return js_idb_request(ctx, JS_DupValue(ctx, g_opaque)); }
static JSValue js_idb_store(JSContext *ctx, JSValueConst t, int c, JSValueConst *v);
static JSValue js_idb_store_self(JSContext *ctx, JSValueConst t, int c, JSValueConst *v)   /* index()/objectStore() -> a store */
{ return js_idb_store(ctx, t, c, v); }
static JSValue js_idb_store(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) {
    JSValue s = JS_NewObject(ctx);
    const char *reqm[] = { "get", "getAll", "getAllKeys", "getKey", "count", "add", "put", "delete", "clear", "openCursor", "openKeyCursor" };
    for (size_t i = 0; i < sizeof reqm / sizeof reqm[0]; i++) JS_SetPropertyStr(ctx, s, reqm[i], JS_NewCFunction(ctx, js_idb_opaque_req, reqm[i], 1));
    JS_SetPropertyStr(ctx, s, "index", JS_NewCFunction(ctx, js_idb_store_self, "index", 1));
    JS_SetPropertyStr(ctx, s, "createIndex", JS_NewCFunction(ctx, js_idb_store_self, "createIndex", 1));
    return s;
}
static JSValue js_idb_tx(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) {
    JSValue tx = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, tx, "objectStore", JS_NewCFunction(ctx, js_idb_store_self, "objectStore", 1));
    JS_SetPropertyStr(ctx, tx, "abort", JS_NewCFunction(ctx, js_noop, "abort", 0));
    JS_SetPropertyStr(ctx, tx, "addEventListener", JS_NewCFunction(ctx, js_add_listener, "addEventListener", 2));
    return tx;
}
static JSValue js_idb_db(JSContext *ctx) {
    JSValue db = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, db, "transaction", JS_NewCFunction(ctx, js_idb_tx, "transaction", 2));
    JS_SetPropertyStr(ctx, db, "createObjectStore", JS_NewCFunction(ctx, js_idb_store_self, "createObjectStore", 1));
    JS_SetPropertyStr(ctx, db, "close", JS_NewCFunction(ctx, js_noop, "close", 0));
    JS_SetPropertyStr(ctx, db, "addEventListener", JS_NewCFunction(ctx, js_add_listener, "addEventListener", 2));
    JSValue names = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, names, "contains", JS_NewCFunction(ctx, js_opaque_stub, "contains", 1));
    JS_SetPropertyStr(ctx, db, "objectStoreNames", names);
    return db;
}
static JSValue js_idb_open(JSContext *ctx, JSValueConst t, int c, JSValueConst *v)
{ return js_idb_request(ctx, js_idb_db(ctx)); }
static JSValue js_match_media(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) {
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "matches", JS_DupValue(ctx, g_opaque));   /* opaque -> a branch on it FORKS */
    JS_SetPropertyStr(ctx, o, "media", JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, o, "addListener", JS_NewCFunction(ctx, js_add_listener, "addListener", 1));
    JS_SetPropertyStr(ctx, o, "removeListener", JS_NewCFunction(ctx, js_noop, "removeListener", 1));
    JS_SetPropertyStr(ctx, o, "addEventListener", JS_NewCFunction(ctx, js_add_listener, "addEventListener", 2));
    return o;
}
/* fetch(url): the moat's host edge. URL = whatever the bundle COMPUTED. ACCUMULATE the endpoint record
   (method/url/params/headers/body) into g_endpoints — identity/dedup runs in-engine at finalize — raise
   the flow's value (the WFQ progress signal), and return a resolved promise wrapping the Response. */
static JSValue js_fetch(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    /* CONCOLIC: a URL built from a trusted-loaded reply is a symbol carrying the REAL computed URL as its
       example — use it, so a gated arm emits /api/billing/enterprise/acme-42 (the merge layer shapes it +
       records the value) instead of losing the gate-independent value to a {} shape. Pure-symbolic attacker
       input has no example -> the shape, as before. */
    const char *url = NULL;
    if (argc > 0) {
        JSValue exurl = JS_OpaqueExample(ctx, argv[0]);
        if (!JS_IsUndefined(exurl)) url = JS_ToCString(ctx, exurl);
        JS_FreeValue(ctx, exurl);
        if (!url) url = JS_ToCString(ctx, argv[0]);
    }
    /* HTTP method from the RequestInit (fetch(url,{method:'DELETE'})) — a big security signal (GET vs
       DELETE/POST). A concrete string only; opaque/missing options -> GET. */
    const char *method = NULL;
    if (argc > 1 && JS_IsObject(argv[1])) {
        JSValue m = JS_GetPropertyStr(ctx, argv[1], "method");
        if (JS_IsString(m)) method = JS_ToCString(ctx, m);   /* opaque options -> .method opaque (not a string) -> GET */
        JS_FreeValue(ctx, m);
    }
    if (!method && argc > 0 && JS_IsObject(argv[0])) {       /* fetch(new Request(url,{method})) -> read the Request's method */
        JSValue m = JS_GetPropertyStr(ctx, argv[0], "method");
        if (JS_IsString(m)) method = JS_ToCString(ctx, m);
        JS_FreeValue(ctx, m);
    }
    char *usolved = url_solve_holes(ctx, url);   /* value-solving: {src} holes the flow fixed -> concrete key (path + query) */
    const char *eurl = usolved ? usolved : url;
    JSValue ep = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, ep, "method", JS_NewString(ctx, method ? method : "GET"));
    JS_SetPropertyStr(ctx, ep, "url", JS_NewString(ctx, eurl ? eurl : "?"));
    JS_SetPropertyStr(ctx, ep, "source", JS_NewString(ctx, "ast_analysis"));
    JSValue params = JS_NewArray(ctx);
    build_query_params(ctx, eurl, params);
    free(usolved);
    JS_SetPropertyStr(ctx, ep, "params", params);
    /* REQUIRED HEADERS + REQUEST BODY: part of the endpoint spec (replay). A plain-object `headers` ->
       ep.headers{name:value}; a POST/PATCH body (already stringified by the bundle; opaque fields -> {}
       shape) -> ep.body. Both control-flattened + capped so a value can't break the emitted line. */
    {
        JSValueConst init = (argc > 1 && JS_IsObject(argv[1])) ? argv[1] : ((argc > 0 && JS_IsObject(argv[0])) ? argv[0] : JS_UNDEFINED);
        if (JS_IsObject(init)) {
            JSValue hdrs = JS_GetPropertyStr(ctx, init, "headers");
            capture_headers(ctx, ep, hdrs);   /* shared with XHR: plain object / Headers __fields, concolic value */
            JS_FreeValue(ctx, hdrs);
            JSValue body = JS_GetPropertyStr(ctx, init, "body");
            if (!JS_IsUndefined(body) && !JS_IsNull(body)) {
                const char *bs = NULL;
                /* CONCOLIC body: a pre-stringified opaque (URLSearchParams(..).toString(), a concat) OR an
                   OBJECT body (FormData) whose toString() returns a concolic opaque -> invoke toString to get
                   the concolic serialization, then read its concrete EXAMPLE (the JS ToString coercion would
                   otherwise flatten the opaque to its shape before we see the example). */
                JSValue braw = JS_DupValue(ctx, body);
                if (JS_IsObject(braw) && !JS_IsOpaque(braw)) {
                    JSValue ts = JS_GetPropertyStr(ctx, braw, "toString");
                    if (JS_IsFunction(ctx, ts)) { JSValue r = JS_Call(ctx, ts, braw, 0, NULL); if (!JS_IsException(r)) { JS_FreeValue(ctx, braw); braw = r; } else { JSValue e = JS_GetException(ctx); JS_FreeValue(ctx, e); } }
                    JS_FreeValue(ctx, ts);
                }
                JSValue exbody = JS_OpaqueExample(ctx, braw);
                if (!JS_IsUndefined(exbody)) bs = JS_ToCString(ctx, exbody);
                JS_FreeValue(ctx, exbody);
                if (!bs) bs = JS_ToCString(ctx, braw);          /* else the shape (opaque) or a plain string */
                JS_FreeValue(ctx, braw);
                if (bs && bs[0]) {
                    char *bsolved = url_solve_holes(ctx, bs);   /* value-solve {src} body holes the flow fixed (JSON.stringify keeps the shape now) */
                    JS_SetPropertyStr(ctx, ep, "body", JS_NewString(ctx, bsolved ? bsolved : bs));
                    free(bsolved);
                }
                if (bs) JS_FreeCString(ctx, bs);
            }
            JS_FreeValue(ctx, body);
        }
    }
    record_endpoint(ctx, ep);   /* the shared @H sink (dedup at finalize) — fetch AND XMLHttpRequest use it */
    JSValue resp = make_response(ctx, url);
    if (url) JS_FreeCString(ctx, url);
    if (method) JS_FreeCString(ctx, method);
    return js_resolved(ctx, resp);
}
/* At finalize: DEDUP the accumulated endpoints in-engine (g_dedup_fn) and emit the whole structured
   result as ONE `@RESULT <json>` line (JSON.stringify — correct escaping, single line). The host does
   ONE JSON.parse and relays it: no @H/@P/@HDR/@BODY text protocol, no host-side re-parse, no host
   identity. Sinks/chunks/errors/park are added to the same object by their accumulate sites. */
static JSValue g_chunkurls = JS_UNDEFINED;    /* JS array of discovered external <script src> */
static JSValue g_park = JS_UNDEFINED;         /* JS array of "hash,decbits" frontier replay recipes */
static void arr_push_str(JSContext *ctx, JSValueConst arr, const char *s) {
    if (!JS_IsArray(arr) || !s) return;
    uint32_t n = 0; JSValue lv = JS_GetPropertyStr(ctx, arr, "length"); JS_ToUint32(ctx, &n, lv); JS_FreeValue(ctx, lv);
    JS_SetPropertyUint32(ctx, arr, n, JS_NewString(ctx, s));
}
/* @WHY IS A FATAL ERROR, NOT A DIAGNOSTIC. A zero-result / gap / unresolved path means the design failed to
   handle something the moat requires. Emitting a @RESULT and CONTINUING would ship DEGRADED output that hides
   the gap behind a logged reason the reviewer ignores — the exact "0 endpoints + 0 errors is the worst output"
   trap, one level up. So every @WHY CRASHES the review WITHOUT continuing: the fix is to eliminate the ROOT so
   it never fires — never log-and-continue, never skip, never delete the check. The design goal is ZERO @WHY. */
static void why_add(JSContext *ctx, const char *phase, const char *reason) {
    (void)ctx;
    fflush(stdout);
    fprintf(stderr, "@E {\"phase\":\"%s\",\"reason\":\"%s\"}\n", phase ? phase : "why", reason ? reason : "");
    fflush(stderr);
    abort();   /* crash without continuing — a @WHY is an unfixed gap, surfaced loud, never papered over */
}
/* ── @S SOLVER (forced execution, not taint tracing) ─────────────────────────────
   Each SINK reached by external input is collected as a task {sink, ctx, expr} — expr is the evaluable
   transform chain with a {source} hole. At finalize the solver substitutes candidate breakout payloads
   into the hole and RUNS THE REAL CHAIN in a CLEAN JS REALM (g_solve_ctx — a fresh context with real
   eval/String methods, no forced-exec/opaque overrides). A candidate whose payload survives into an
   EXECUTABLE position after the real transforms IS the PoC (verified because the real filters ran); if
   none survives, the flow is PROVEN safe for the tried payloads. No taint label, no chain inversion. */
static JSValue g_solvetasks = JS_UNDEFINED;   /* JS array of {sink, ctx, expr} (the finalize expr-eval pre-filter) */
static JSValue g_verified = JS_UNDEFINED;     /* "sink|ctx" -> concrete PoC candidate that a REPLAY flow drove through the real code+branches to the sink where it broke out. The ONLY @S output: a working PoC is self-verifying; absence is NOT a safe verdict, only search-not-yet-solved. */
static JSValue g_enqueued = JS_UNDEFINED;     /* "orphanidx|sink|ctx" -> 1: candidate-replay flows already enqueued for this sink (dedup, not truncation) */
static JSContext *g_solve_ctx = NULL;         /* fresh realm for clean candidate eval */
static const char *CAND_HTML[] = { "<img src=x onerror=X9>", "<svg onload=X9>", "\"><img src=x onerror=X9>",
                                   "'><svg onload=X9>", "</script><svg onload=X9>", NULL };
static const char *CAND_URL[]  = { "javascript:X9", "javascript:X9//", NULL };
static const char *CAND_JS[]   = { "1;X9();//", "';X9();//", "\";X9();//", ");X9();//", "\n;X9();//", NULL };
static const char **cand_set(const char *sc) {
    if (sc && strcmp(sc, "url") == 0) return CAND_URL;
    if (sc && strcmp(sc, "js") == 0) return CAND_JS;
    return CAND_HTML;
}
/* GATE TOKENS: concrete strings the REAL code tested tainted input against (startsWith('cmd:'), =='x'…).
   The forced-exec search prefixes/suffixes each base payload with them so a gated sink is solved by the
   concrete input the gate requires — no symbolic solver, just what the code itself demanded. Deduped
   (identical token -> identical candidates, pure waste); no length/count bound (a gate may require a long
   exact prefix, and the WFQ starves low-value search flows rather than a cap dropping them). */
static char **g_gate_tokens = NULL; static int g_gate_n = 0, g_gate_cap = 0;
static void gate_collect(const char *token, const char *src) {
    if (!token || !token[0]) return;
    /* An {origin}/{source} constraint bounds the ATTACKER'S ORIGIN (`e.origin.indexOf('trusted')`), NOT the
       data payload — feeding it to the data-candidate search would build nonsense candidates like
       'trusted<img..>'. Drop it here (the data search is unaffected; the same string, if ALSO a real data gate
       elsewhere, is collected there with a data src). Per-flow origin-constraint SURFACING for delivery is a
       separate concern needing per-flow attribution. */
    /* origin/source constraints never feed the DATA-payload candidate search. Their @S suppression is unified in
       the constraint tracker: `===`/membership record {origin}==tok directly, and e.origin.endsWith('.host')
       gets EQ provenance at the opaque method-call site (quickjs.c) — all caught by cons_fixed_value("{origin}"),
       so no separate per-flow flag here. */
    if (src && (strncmp(src, "{origin}", 8) == 0 || strncmp(src, "{source}", 8) == 0)) return;
    for (int i = 0; i < g_gate_n; i++) if (strcmp(g_gate_tokens[i], token) == 0) return;
    if (g_gate_n >= g_gate_cap) { int nc = g_gate_cap ? g_gate_cap * 2 : 32;
        char **n = realloc(g_gate_tokens, (size_t)nc * sizeof(char *)); if (!n) return; g_gate_tokens = n; g_gate_cap = nc; }
    g_gate_tokens[g_gate_n++] = strdup(token);
}
static int mem_has_x9(const lxb_char_t *s, size_t n) {   /* the X9 fire-marker survives, as raw bytes (values aren't null-terminated) */
    if (!s) return 0;
    for (size_t i = 0; i + 1 < n; i++) if (s[i] == 'X' && s[i + 1] == '9') return 1;
    return 0;
}
struct x9_walk { int found; };
static lxb_status_t x9_walk_cb(lxb_dom_node_t *node, void *vctx) {
    struct x9_walk *c = vctx;
    if (node->type != LXB_DOM_NODE_TYPE_ELEMENT) return LXB_STATUS_OK;
    lxb_dom_element_t *el = lxb_dom_interface_element(node);
    size_t nl = 0; const lxb_char_t *nm = lxb_dom_element_qualified_name(el, &nl);   /* <script>...X9...</script> executes */
    if (nl == 6 && nm && memcmp(nm, "script", 6) == 0) {
        size_t cl = 0; lxb_char_t *ct = lxb_dom_node_text_content(lxb_dom_interface_node(node), &cl);
        if (mem_has_x9(ct, cl)) { c->found = 1; return LXB_STATUS_STOP; }
    }
    for (lxb_dom_attr_t *a = lxb_dom_element_first_attribute(el); a; a = lxb_dom_element_next_attribute(a)) {
        size_t al = 0; const lxb_char_t *an = lxb_dom_attr_local_name(a, &al);   /* on* event handler with X9 -> a LIVE handler (Lexbor lowercases names) */
        if (al >= 3 && an && an[0] == 'o' && an[1] == 'n') {
            size_t vl = 0; const lxb_char_t *av = lxb_dom_attr_value(a, &vl);
            if (mem_has_x9(av, vl)) { c->found = 1; return LXB_STATUS_STOP; }
        }
    }
    return LXB_STATUS_OK;
}
/* PARSE the sink string like a real browser (Lexbor) and check X9 reached an EXECUTABLE position: a live
   element's on* handler, or <script> content. A tag opener trapped inside an ATTRIBUTE value
   (href="<img onerror=X9>") or in TEXT (&lt;img..) is INERT -> NOT a breakout. Replaces the naive
   strstr("<img") which false-positived whenever a raw tag survived ANYWHERE, incl. attribute-trapped. */
static int solve_broke_html(const char *res) {
    lxb_html_document_t *doc = lxb_html_document_create();
    if (!doc) return 0;
    struct x9_walk c = { 0 };
    if (lxb_html_document_parse(doc, (const lxb_char_t *)res, strlen(res)) == LXB_STATUS_OK) {
        lxb_dom_node_t *root = lxb_dom_interface_node(lxb_html_document_body_element(doc));
        if (root) lxb_dom_node_simple_walk(root, x9_walk_cb, &c);
    }
    lxb_html_document_destroy(doc);
    return c.found;
}
/* context-aware breakout: did the candidate's active payload reach an EXECUTABLE position in `res`? */
static int solve_broke(const char *sc, const char *res) {
    if (!res) return 0;
    if (sc && strcmp(sc, "url") == 0) { const char *p = res; while (*p == ' ' || *p == '\t') p++; return strncmp(p, "javascript:X9", 13) == 0; }
    if (!strstr(res, "X9")) return 0;   /* our nonce must survive */
    if (sc && strcmp(sc, "js") == 0) {
        /* eval sink: the sink VALUE is code — RUN it in the clean realm; X9() firing = real code injection
           (sound, unlike a substring match which false-positives when X9 lands inside a string literal). */
        if (!g_solve_ctx) return 0;
        JSValue r0 = JS_Eval(g_solve_ctx, "globalThis.__f9=0", 17, "<r>", JS_EVAL_TYPE_GLOBAL); JS_FreeValue(g_solve_ctx, r0);
        JSValue cr = JS_Eval(g_solve_ctx, res, strlen(res), "<sinkcode>", JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(cr)) { JSValue e = JS_GetException(g_solve_ctx); JS_FreeValue(g_solve_ctx, e); }
        JS_FreeValue(g_solve_ctx, cr);
        JSValue fv = JS_Eval(g_solve_ctx, "globalThis.__f9", 15, "<f>", JS_EVAL_TYPE_GLOBAL);
        int fired = JS_ToBool(g_solve_ctx, fv); JS_FreeValue(g_solve_ctx, fv);
        return fired;
    }
    /* html/attr: PARSE like a browser and require X9 in an EXECUTABLE position (live on* handler / <script>),
       not merely a raw tag opener surviving somewhere (that false-positives when the payload is trapped in an
       attribute value, e.g. `<a href="<img onerror=X9>">`). */
    return solve_broke_html(res);
}
/* enqueue an @S REPLAY flow: re-run the CURRENT orphan with `cand` as the concrete source, driven by the
   ONE scheduler (high initial value so the search runs soon; transient — never parked as a recipe). */
static void reg_add_cand(JSContext *ctx, JSValueConst fn, const char *cand, const char *target) {
    if (g_in_session) {   /* sink reached inside a session -> a candidate SESSION flow re-fires ALL handlers with the candidate (cross-handler verify) */
        signed char *sdec = NULL;   /* inherit THIS session's decision vector so the candidate replays the SAME arms that reached the sink (an exploratory session now forks) */
        if (g_dec_n > 0) { sdec = (signed char *)malloc((size_t)g_dec_n); if (sdec) for (int i = 0; i < g_dec_n; i++) sdec[i] = g_dec[i]; }
        if (reg_add(ctx, JS_UNDEFINED, 2.0, 0, sdec, sdec ? g_dec_n : 0)) { g_reg[g_reg_n - 1].candidate = strdup(cand); g_reg[g_reg_n - 1].session = 1; }
        else free(sdec);
        return;   /* a session verifies MANY sinks -> not tagged with one vtarget */
    }
    if (JS_IsUndefined(fn)) return;
    if (reg_add(ctx, JS_DupValue(ctx, fn), 2.0, 0, NULL, 0)) {
        g_reg[g_reg_n - 1].candidate = strdup(cand);
        g_reg[g_reg_n - 1].orphan_idx = g_cur_orphan_idx;
        if (target) g_reg[g_reg_n - 1].vtarget = strdup(target);
    }
}
/* @S STRUCTURED DELIVERY: every EQ gate the flow took on a SIBLING field of the same attacker object
   ({pm}.type=='render' while the sink reads {pm}.html) is a field the delivery object MUST set, or the real
   handler's gate blocks the sink. Collect those {root}.field==token pairs into {field:token}; the sink field
   carries the payload separately. NULL if none (whole-value or ungated). */
static JSValue collect_gate_fields(JSContext *ctx, const char *root) {
    if (!root || !root[0]) return JS_UNDEFINED;
    size_t rl = strlen(root); JSValue o = JS_UNDEFINED;
    for (int i = 0; i < g_cons_n; i++) {
        Cons *c = &g_cons[i];
        if (c->src && c->op == OPCMP_EQ && c->tok && !strncmp(c->src, root, rl) && c->src[rl] == '.') {
            if (JS_IsUndefined(o)) o = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, o, c->src + rl + 1, JS_NewString(ctx, c->tok));   /* field path after "{root}." -> required token */
        }
    }
    return o;
}
/* Sink reached. DUAL-MODE:
   - REPLAY flow (g_candidate set): `val` is the CONCRETE transformed candidate that ran through the REAL
     code+branches to get here. If it breaks out, this PoC is PATH+BREAKOUT verified (reachability proven by
     the real branches) -> record it under "sink|ctx".
   - NORMAL flow (opaque val): record the finalize task (pre-filter/proven-safe display) AND enqueue
     candidate-replay flows once per (orphan,sink,ctx) into the ONE scheduler. */
static void solve_add(JSContext *ctx, const char *sink, const char *sctx, JSValueConst val) {
    if (g_candidate) {
        /* @S SOUNDNESS: if this path force-passed an EXACT origin gate (`e.origin === 'https://trusted'`), the
           attacker CANNOT forge that origin, so the sink is unreachable cross-origin -> the candidate would be a
           FALSE PoC. Suppress. (A substring/regex origin check records NO EQ constraint, so it is NOT suppressed
           -- those are genuinely bypassable and stay reportable, the origin-bypass frontier.) */
        if (cons_fixed_value("{origin}")) return;   /* attacker-unsatisfiable origin gate on this path (===, membership, endsWith('.host')) -> unreachable cross-origin */
        /* This flow drove a CONCRETE candidate through the real code+branches to the sink. If it broke out,
           THAT candidate is a working PoC — the only sound @S output. No breakout -> nothing recorded (not a
           "safe" verdict: the search may still solve a gate with a better candidate). */
        /* For a {parsedhtml}-tainted node (DOMParser/Range), the sink value is an OPAQUE carrying the candidate
           as its EXAMPLE — ToString'ing it gives the shape, not the payload. Read the example so the real
           candidate HTML is breakout-checked; a plain-string sink value is used directly. */
        JSValue exv = JS_IsOpaque(val) ? JS_OpaqueExample(ctx, val) : JS_UNDEFINED;
        const char *cv = !JS_IsUndefined(exv) ? JS_ToCString(ctx, exv) : JS_ToCString(ctx, val);
        JS_FreeValue(ctx, exv);
        if (cv && solve_broke(sctx, cv) && JS_IsObject(g_verified)) {
            char key[300]; snprintf(key, sizeof key, "%s|%s", sink, sctx);
            JS_SetPropertyStr(ctx, g_verified, key, JS_NewString(ctx, g_candidate));
        }
        if (cv) JS_FreeCString(ctx, cv);
        return;
    }
    if (!JS_IsOpaque(val) || !JS_IsArray(g_solvetasks)) return;
    const char *shape = JS_OpaqueShapeC(val);   /* @H-style display: which source(s) reach this sink, transforms flattened */
    JSValue t = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, t, "sink", JS_NewString(ctx, sink));
    JS_SetPropertyStr(ctx, t, "ctx", JS_NewString(ctx, sctx));
    JS_SetPropertyStr(ctx, t, "expr", JS_NewString(ctx, shape ? shape : "{}"));
    { const char *sp = JS_OpaqueSrcC(val);   /* @S structured delivery: the source LEAF path ("{pm}.html") -> post {html:payload}, not a bare string */
      if (sp && sp[0]) {
          JS_SetPropertyStr(ctx, t, "srcpath", JS_NewString(ctx, sp));
          char root[64]; const char *rb = strchr(sp, '}');   /* source token "{pm}" -> collect sibling gate fields the handler requires */
          if (rb && (size_t)(rb - sp + 1) < sizeof root) { size_t rl = (size_t)(rb - sp + 1); memcpy(root, sp, rl); root[rl] = 0;
              JSValue gf = collect_gate_fields(ctx, root);
              if (!JS_IsUndefined(gf)) JS_SetPropertyStr(ctx, t, "gatefields", gf); }
      } }
    JS_SetPropertyStr(ctx, t, "gated", JS_NewBool(ctx, g_c > 0));
    uint32_t n = 0; JSValue lv = JS_GetPropertyStr(ctx, g_solvetasks, "length"); JS_ToUint32(ctx, &n, lv); JS_FreeValue(ctx, lv);
    JS_SetPropertyUint32(ctx, g_solvetasks, n, t);
    g_emit_total++;   /* a reached sink is progress like @H */
    if (g_running && g_cur_flow) { g_cur_flow->val += 1.0; g_cur_flow->cpu = 0; }
    /* SPAWN candidate-replay flows in the ONE scheduler. Re-drive the FUNCTION that reached this sink (the
       nearest bytecode fn on the stack — works even at BOOT, where there is no orphan flow context) with each
       concrete candidate. Dedup by fn-SOURCE-IDENTITY (position-independent) + sink + ctx — avoids re-enqueue,
       not work-truncation: each candidate still runs once and the WFQ orders/starves them. */
    JSValueConst hitfn = JS_CurrentScriptFn(ctx);
    if (JS_IsObject(g_enqueued) && !JS_IsUndefined(hitfn)) {
        char ek[320]; snprintf(ek, sizeof ek, "%u|%s|%s", JS_OrphanHash(ctx, hitfn), sink, sctx);
        JSValue e = JS_GetPropertyStr(ctx, g_enqueued, ek); int done = !JS_IsUndefined(e); JS_FreeValue(ctx, e);
        if (!done) {
            JS_SetPropertyStr(ctx, g_enqueued, ek, JS_NewBool(ctx, 1));
            char vt[300]; snprintf(vt, sizeof vt, "%s|%s", sink, sctx);   /* the sink|ctx these candidates verify -> skip once one breaks out */
            const char **cands = cand_set(sctx);
            for (int i = 0; cands[i]; i++) {
                reg_add_cand(ctx, hitfn, cands[i], vt);
                /* GATE-GUIDED variants: try each observed gate token as a PREFIX and SUFFIX of the payload,
                   so startsWith('cmd:')/endsWith(...)/includes(...) gates are satisfied by the concrete input
                   the real code demanded. The one that passes the gate reaches the sink and breaks out. */
                for (int g = 0; g < g_gate_n; g++) {
                    size_t lt = strlen(g_gate_tokens[g]), lc = strlen(cands[i]);
                    char *pre = malloc(lt + lc + 1), *suf = malloc(lt + lc + 1);
                    if (pre) { memcpy(pre, g_gate_tokens[g], lt); memcpy(pre + lt, cands[i], lc + 1); reg_add_cand(ctx, hitfn, pre, vt); free(pre); }
                    if (suf) { memcpy(suf, cands[i], lc); memcpy(suf + lc, g_gate_tokens[g], lt + 1); reg_add_cand(ctx, hitfn, suf, vt); free(suf); }
                }
                /* CORRELATED gates (`startsWith('cmd:') && endsWith('!end')`): tokens are collected in EXECUTION
                   order, so the earlier gate is the PREFIX check and the later the SUFFIX/contains check. An
                   ADJACENT-pair candidate earlier+payload+later satisfies both — O(N), no method tracking. */
                for (int g = 0; g + 1 < g_gate_n; g++) {
                    size_t l0 = strlen(g_gate_tokens[g]), l1 = strlen(g_gate_tokens[g + 1]), lc = strlen(cands[i]);
                    char *comb = malloc(l0 + lc + l1 + 1);
                    if (comb) { memcpy(comb, g_gate_tokens[g], l0); memcpy(comb + l0, cands[i], lc); memcpy(comb + l0 + lc, g_gate_tokens[g + 1], l1 + 1);
                        reg_add_cand(ctx, hitfn, comb, vt); free(comb); }
                }
            }
        }
    }
}
/* build securitySinks[] by solving each collected task (dedup by sink+ctx+expr). */
static JSValue solve_all(JSContext *ctx) {
    JSValue out = JS_NewArray(ctx);
    if (!JS_IsArray(g_solvetasks)) return out;
    uint32_t tn = 0; { JSValue lv = JS_GetPropertyStr(ctx, g_solvetasks, "length"); JS_ToUint32(ctx, &tn, lv); JS_FreeValue(ctx, lv); }
    JSValue seen = JS_NewObject(ctx); uint32_t oi = 0;
    for (uint32_t i = 0; i < tn; i++) {
        JSValue t = JS_GetPropertyUint32(ctx, g_solvetasks, i);
        JSValue sv = JS_GetPropertyStr(ctx, t, "sink"), cv = JS_GetPropertyStr(ctx, t, "ctx"), ev = JS_GetPropertyStr(ctx, t, "expr");
        JSValue spv = JS_GetPropertyStr(ctx, t, "srcpath");
        JSValue gfv = JS_GetPropertyStr(ctx, t, "gatefields");
        const char *sink = JS_ToCString(ctx, sv), *sc = JS_ToCString(ctx, cv), *ex = JS_ToCString(ctx, ev);
        const char *srcpath = JS_IsString(spv) ? JS_ToCString(ctx, spv) : NULL;
        if (ex) {
            char keybuf[1200]; snprintf(keybuf, sizeof keybuf, "%s|%s|%s", sink ? sink : "", sc ? sc : "", ex);
            JSValue dup = JS_GetPropertyStr(ctx, seen, keybuf);
            int isdup = !JS_IsUndefined(dup); JS_FreeValue(ctx, dup);
            if (!isdup) {
                JS_SetPropertyStr(ctx, seen, keybuf, JS_NewBool(ctx, 1));
                /* The ONLY @S finding is a WORKING PoC: a candidate a REPLAY flow drove through the real
                   code+branches to this sink where it BROKE OUT. No PoC -> emit NOTHING here (a @WHY search
                   signal, not a "safe"/"verified:false" verdict — absence of a PoC never proves safety; the
                   forced-exec search may still solve a gate like startsWith('cmd:') with a better candidate). */
                char vk[300]; snprintf(vk, sizeof vk, "%s|%s", sink ? sink : "", sc ? sc : "");
                char *rpoc = NULL;
                if (JS_IsObject(g_verified)) {
                    JSValue vv = JS_GetPropertyStr(ctx, g_verified, vk);
                    if (JS_IsString(vv)) { const char *s = JS_ToCString(ctx, vv); if (s) { rpoc = strdup(s); JS_FreeCString(ctx, s); } }
                    JS_FreeValue(ctx, vv); }
                /* No working PoC yet -> emit NOTHING: this is an IN-PROGRESS @S search still in the frontier
                   (unbounded — a better candidate may break a gate like startsWith('cmd:') next burst/session),
                   NOT a gap and NOT a "safe" verdict (absence of a PoC never proves safety). It is therefore
                   NEVER a fatal @WHY — an unsolved sink is in-progress work, not a should-never-happen. */
                if (rpoc) {
                    JSValue rec = JS_NewObject(ctx);
                    JS_SetPropertyStr(ctx, rec, "type", JS_NewString(ctx, sink ? sink : "?"));
                    JS_SetPropertyStr(ctx, rec, "sink", JS_NewString(ctx, sink ? sink : "?"));
                    JS_SetPropertyStr(ctx, rec, "taint", JS_NewString(ctx, "forced-exec"));
                    JS_SetPropertyStr(ctx, rec, "shape", JS_NewString(ctx, ex));
                    if (srcpath && srcpath[0]) JS_SetPropertyStr(ctx, rec, "srcpath", JS_NewString(ctx, srcpath));   /* structured delivery hint */
                    if (JS_IsObject(gfv)) JS_SetPropertyStr(ctx, rec, "gatefields", JS_DupValue(ctx, gfv));   /* sibling gate fields the delivery object must set */
                    JS_SetPropertyStr(ctx, rec, "source", JS_NewString(ctx, "ast_analysis"));
                    JS_SetPropertyStr(ctx, rec, "poc", JS_NewString(ctx, rpoc));
                    { char eb[900]; snprintf(eb, sizeof eb, "sink %s <- input %s (forced-exec: this exact input, driven through the real code, breaks out at the sink)", sink ? sink : "?", rpoc);
                      JS_SetPropertyStr(ctx, rec, "evidence", JS_NewString(ctx, eb)); }
                    free(rpoc);
                    JS_SetPropertyUint32(ctx, out, oi++, rec);
                }
            }
        }
        if (sink) JS_FreeCString(ctx, sink); if (sc) JS_FreeCString(ctx, sc); if (ex) JS_FreeCString(ctx, ex);
        if (srcpath) JS_FreeCString(ctx, srcpath);
        JS_FreeValue(ctx, sv); JS_FreeValue(ctx, cv); JS_FreeValue(ctx, ev); JS_FreeValue(ctx, spv); JS_FreeValue(ctx, gfv); JS_FreeValue(ctx, t);
    }
    JS_FreeValue(ctx, seen);
    return out;
}
static void emit_result(JSContext *ctx) {
    if (JS_IsUndefined(g_dedup_fn) || !JS_IsArray(g_endpoints)) return;
    JSValueConst args[1] = { g_endpoints };
    JSValue deduped = JS_Call(ctx, g_dedup_fn, JS_UNDEFINED, 1, args);
    if (JS_IsException(deduped)) { js_std_dump_error(ctx); deduped = JS_NewArray(ctx); }
    JSValue result = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, result, "fetchCallSites", deduped);                 /* consumes deduped */
    JS_SetPropertyStr(ctx, result, "securitySinks", solve_all(ctx));           /* @S: forced-exec solve for a breakout PoC per sink */
    JS_SetPropertyStr(ctx, result, "chunkUrls", JS_DupValue(ctx, g_chunkurls));
    JS_SetPropertyStr(ctx, result, "_park", JS_DupValue(ctx, g_park));
    JS_SetPropertyStr(ctx, result, "_orphans", JS_NewInt32(ctx, g_orphan_n));
    JS_SetPropertyStr(ctx, result, "_emit", JS_NewInt32(ctx, g_emit_total));
    JS_SetPropertyStr(ctx, result, "_switches", JS_NewInt64(ctx, g_switches));   /* flow interleave events (fairness, incl. at depth) */
    JS_SetPropertyStr(ctx, result, "_work", JS_NewInt64(ctx, g_work));           /* flow dispatches this run (diagnostic) */
    JS_SetPropertyStr(ctx, result, "_parked", JS_NewInt32(ctx, g_park_requested)); /* 1 = host RAM pressure forced a cold-tier park this run */
    JSValue json = JS_JSONStringify(ctx, result, JS_UNDEFINED, JS_UNDEFINED);
    JS_FreeValue(ctx, result);
    if (JS_IsString(json)) { const char *js = JS_ToCString(ctx, json); if (js) { printf("@RESULT %s\n", js); JS_FreeCString(ctx, js); } }
    JS_FreeValue(ctx, json);
    fflush(stdout);
}

/* __branch(): a FORCED DECISION POINT (a gate on opaque external input). If the running flow's decision
   vector already fixes this point, replay it. Otherwise it's a NEW branch: FORK a sibling flow that
   re-runs the SAME function, replaying this flow's decisions so far then taking FALSE here; this flow
   takes TRUE (recorded so deeper new branches fork correctly). BFS over the decision tree -> both arms
   of every gate are explored, surfacing the branch-gated endpoints. */
/* branch_decide: the decision-vector fork logic (0/1). Called BOTH by __branch() (explicit) and by the
   engine's OP_if hook when a branch condition is OPAQUE (real bundles). Forced replay of this flow's
   decision prefix; a NEW decision forks the FALSE sibling (re-run the same function) and takes TRUE. */
static int g_boot_replay = 0;   /* re-running boot to re-establish shared state for a cross-flow @S candidate */
static int reg_add_boot(JSContext *ctx, signed char *dec, int dec_n);   /* fwd: defined near boot_replay */
static int reg_add_session(JSContext *ctx, signed char *dec, int dec_n); /* fwd: an exploratory session that FORKS */
static int branch_decide(JSContext *ctx, JSValueConst cond)
{
    if (g_boot_replay) return 1;                            /* boot-replay: fixed arm (re-establishing shared state for an @S candidate, no vector to replay) */
    if (!g_running || (JS_IsUndefined(g_cur_fn) && !g_in_boot_flow && !g_in_session)) return 0;   /* meaningful inside a starter flow, a boot flow, OR a session flow (all re-run without a single fn handle) */
    /* value-domain provenance of the condition: cond TRUE means `src <op> tok`; false arm holds the negation. */
    const char *src = NULL, *tok = NULL; int op = JS_OpaqueCmp(cond, &src, &tok);
    int has = (op != OPCMP_NONE) && src;
    int true_op = op, false_op = opcmp_neg(op);

    if (g_c < g_dec_n) {                                    /* forced replay: take the recorded arm; RE-RECORD its constraint */
        int arm = g_dec[g_c] ? 1 : 0;
        cons_set(g_c, has ? src : NULL, has ? tok : NULL, has ? (arm ? true_op : false_op) : OPCMP_NONE);
        g_c++; return arm;
    }
    if (!g_dec_ensure(g_c + 1)) return 1;                    /* only RAM/disk (the platform floor) bounds depth — not a cap */

    /* A CANDIDATE session (verifying an @S breakout) takes a fixed arm for a NEW branch — it replays the
       detecting session's vector (above) then follows through with the candidate, it does not re-explore. */
    if (g_in_session && g_candidate) { cons_set(g_c, has ? src : NULL, has ? tok : NULL, has ? true_op : OPCMP_NONE); g_dec[g_c] = 1; g_dec_n = g_c + 1; g_c++; return 1; }

    /* NEW decision: PRUNE a provably-infeasible arm given the accumulated domain (no phantom @H); else fork. */
    int tf = !has || cons_feasible(src, tok, true_op, g_c);
    int ff = !has || cons_feasible(src, tok, false_op, g_c);
    if (has && !tf && ff) { cons_set(g_c, src, tok, false_op); g_dec[g_c] = 0; g_dec_n = g_c + 1; g_c++; return 0; }   /* TRUE arm impossible */
    if (has && !ff && tf) { cons_set(g_c, src, tok, true_op);  g_dec[g_c] = 1; g_dec_n = g_c + 1; g_c++; return 1; }   /* FALSE arm impossible */

    signed char *sib = (signed char *)malloc((size_t)(g_c + 1));   /* both arms feasible: fork FALSE sibling, take TRUE */
    if (sib) {
        for (int i = 0; i < g_c; i++) sib[i] = g_dec[i];
        sib[g_c] = 0;
        if (g_in_session) reg_add_session(ctx, sib, g_c + 1);      /* an exploratory session forks ANOTHER session (re-fire handlers with the sibling vector) */
        else if (g_in_boot_flow) reg_add_boot(ctx, sib, g_c + 1);  /* a boot flow forks ANOTHER boot flow (re-run boot with the sibling vector) */
        else { reg_add(ctx, JS_DupValue(ctx, g_cur_fn), g_cur_val, 0, sib, g_c + 1);
               g_reg[g_reg_n - 1].orphan_idx = g_cur_orphan_idx; }   /* sibling = same function (same locator), different decisions */
    }
    cons_set(g_c, has ? src : NULL, has ? tok : NULL, has ? true_op : OPCMP_NONE);
    g_dec[g_c] = 1; g_dec_n = g_c + 1; g_c++;
    return 1;
}
/* crypto.getRandomValues(arr): the spec FILLS + RETURNS the same typed array. The bytes are external
   randomness — nondeterministic, so filling them would (a) break replay soundness and (b) fabricate a
   concrete value. We can't store an opaque in a numeric typed array, so we leave it unmodified (its
   deterministic initial zeros) and return the SAME array, so `crypto.getRandomValues(new Uint8Array(n))`
   and the fill-then-read idiom both work without throwing. randomUUID (the URL-relevant one) is opaque. */
static JSValue js_crypto_getrandom(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{ return argc >= 1 ? JS_DupValue(ctx, argv[0]) : JS_DupValue(ctx, g_opaque); }
/* setTimeout/setInterval/requestAnimationFrame(cb, ...): a deferred callback is NOT a wait on real time —
   it is just another BFS FLOW. Register cb in the ONE scheduler (reg_add) so it is driven, ordered, and
   starved by the same WFQ as every other flow (the whole point: bundles that defer init in a timer still
   get explored). Return an opaque timer id; the clear/cancel variants are no-ops (the WFQ starves it anyway). */
static JSValue js_set_timer(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    if (argc >= 1 && JS_IsFunction(ctx, argv[0]))
        reg_add(ctx, JS_DupValue(ctx, argv[0]), g_running ? g_cur_val : 1.0, 0, NULL, 0);
    else if (argc >= 1 && (JS_IsString(argv[0]) || JS_IsOpaque(argv[0])))
        solve_add(ctx, "setTimeout", "js", argv[0]);   /* @S: setTimeout(STRING) EVALs the string -> js-context sink (like eval); string(candidate)+opaque(detect) both, like the other sinks */
    return JS_DupValue(ctx, g_opaque);
}
/* localStorage/sessionStorage.getItem(k): stored data is EXTERNAL INPUT (a token/flag put there earlier or
   by another origin's code) -> OPAQUE (feeds auth headers/branches opaquely, replay-sound). set/remove/clear
   are no-ops (writes don't drive discovery). */
static JSValue js_storage_get(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{ return JS_DupValue(ctx, g_opaque); }
/* structuredClone(x): deep-clone is identity for forced-exec purposes (shape/opacity carry through). */
static JSValue js_structured_clone(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{ return argc >= 1 ? JS_DupValue(ctx, argv[0]) : JS_UNDEFINED; }

/* URL / URLSearchParams: endpoint construction. `new URL(path, base).href|pathname` is how a huge share of
   bundles build request URLs — undefined `URL` = ReferenceError = the endpoint is lost. A CONCRETE input is
   resolved by the REAL vendored LEXBOR URL parser (bind-before-build: existing Lexbor module, never a
   hand-rolled string resolver). An OPAQUE input (external-input-tainted, or a shape with {} holes Lexbor
   can't parse) -> OPAQUE, so its shape flows through untouched (the endpoint is learned as its shape) and
   the tool never concretely decides external input. searchParams.get is OPAQUE (query values = external). */
static const char *g_origin;   /* defined below; forward for the URL helpers */
static JSValue js_opaque(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
static JSValue js_noop(JSContext *ctx, JSValueConst t, int c, JSValueConst *v);
/* Resolve a URL with the vendored LEXBOR URL module (the real WHATWG URL Standard parser) — never a
   hand-rolled string resolver. Returns the serialized absolute href (malloc'd; caller frees) or NULL on a
   parse failure (-> the caller yields opaque, never an invented value). */
struct url_ser_buf { char *s; size_t n, cap; };
static lxb_status_t url_ser_cb(const lxb_char_t *data, size_t len, void *cbctx) {
    struct url_ser_buf *b = cbctx;
    if (b->n + len + 1 > b->cap) { size_t nc = (b->n + len + 1) * 2 + 64; char *ns = realloc(b->s, nc); if (!ns) return LXB_STATUS_ERROR_MEMORY_ALLOCATION; b->s = ns; b->cap = nc; }
    memcpy(b->s + b->n, data, len); b->n += len; b->s[b->n] = 0;
    return LXB_STATUS_OK;
}
static char *url_resolve(const char *input, const char *base) {
    lxb_url_parser_t *p = lxb_url_parser_create();
    if (!p || lxb_url_parser_init(p, NULL) != LXB_STATUS_OK) { if (p) lxb_url_parser_destroy(p, true); return NULL; }
    lxb_url_t *bu = (base && base[0]) ? lxb_url_parse(p, NULL, (const lxb_char_t *)base, strlen(base)) : NULL;
    lxb_url_t *u = lxb_url_parse(p, bu, (const lxb_char_t *)(input ? input : ""), input ? strlen(input) : 0);
    char *out = NULL;
    if (u) { struct url_ser_buf b = {0}; if (lxb_url_serialize(u, url_ser_cb, &b, false) == LXB_STATUS_OK) out = b.s; else free(b.s); }
    lxb_url_parser_destroy(p, true);   /* frees bu, u, and internal buffers */
    return out;
}
static void url_set(JSContext *ctx, JSValue o, const char *k, const char *s, size_t n) {
    JS_SetPropertyStr(ctx, o, k, JS_NewStringLen(ctx, s, n));
}
static JSValue js_url_tostring(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{ return JS_GetPropertyStr(ctx, this_val, "href"); }
/* url.searchParams shares the concolic __fields machinery with standalone URLSearchParams (js_sp_*), PLUS a
   back-ref to its owner URL so set/append/delete REBUILD the owner's href/search -> fetch(url) emits the
   app-BUILT query (a query the app SETS is the app constructing the request: concrete, not external input).
   Reads stay opaque for unknown keys (external query input). Defs live with the sp machinery below. */
static JSValue js_sp_get(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
static void sp_init(JSContext *ctx, JSValueConst o, JSValueConst init);
static JSValue js_url_sp_set(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
static JSValue js_url_sp_delete(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
/* __fields iteration, shared by url.searchParams / standalone URLSearchParams / Headers — a MISSING forEach or
   keys/values/entries throws, and uncaught in boot that discards the whole page (fatal @WHY). */
static JSValue js_sp_forEach(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
static JSValue js_sp_keys(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
static JSValue js_sp_values(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
static JSValue js_sp_entries(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
static JSValue js_url_ctor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv) {
    /* opaque (external-input-tainted) URL -> return the INPUT opaque so its SHAPE flows through unchanged
       (never concretely resolved — RUN-DON'T-MATCH). A concrete input is resolved by the REAL Lexbor parser. */
    if (argc >= 1 && JS_IsOpaque(argv[0])) return JS_DupValue(ctx, argv[0]);
    const char *input = argc >= 1 ? JS_ToCString(ctx, argv[0]) : NULL;
    const char *base  = argc >= 2 && JS_IsString(argv[1]) ? JS_ToCString(ctx, argv[1]) : NULL;
    char *resolved = (input && !has_hole(input)) ? url_resolve(input, base ? base : g_origin) : NULL;
    JSValue shaped = (input && has_hole(input)) ? JS_NewOpaqueShaped(ctx, input) : JS_UNDEFINED;  /* {}-shape string -> keep shape */
    if (input) JS_FreeCString(ctx, input);
    if (base) JS_FreeCString(ctx, base);
    if (!resolved) return JS_IsUndefined(shaped) ? JS_DupValue(ctx, g_opaque) : shaped;   /* shape/parse-fail -> opaque, never invent */
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "href", JS_NewString(ctx, resolved));
    /* Extract components from Lexbor's CANONICAL output (scheme://host/path?query#frag) — trivial split of a
       spec-parsed string, NOT a resolver reimplementation (Lexbor did the resolution). */
    const char *scol = strstr(resolved, "://");
    const char *host = scol ? scol + 3 : resolved;
    url_set(ctx, o, "protocol", resolved, scol ? (size_t)(scol - resolved) + 1 : 0);
    const char *pe = host; while (*pe && *pe != '/' && *pe != '?' && *pe != '#') pe++;
    url_set(ctx, o, "host", host, (size_t)(pe - host));
    url_set(ctx, o, "hostname", host, (size_t)(pe - host));
    url_set(ctx, o, "origin", resolved, (size_t)(pe - resolved));
    const char *path = pe; const char *q = strchr(path, '?'); const char *hsh = strchr(path, '#');
    const char *pend = q ? q : (hsh ? hsh : path + strlen(path));
    url_set(ctx, o, "pathname", *path ? path : "/", *path ? (size_t)(pend - path) : 1);
    if (q) { const char *qe = hsh ? hsh : q + strlen(q); url_set(ctx, o, "search", q, (size_t)(qe - q)); }
    else JS_SetPropertyStr(ctx, o, "search", JS_NewString(ctx, ""));
    if (hsh) JS_SetPropertyStr(ctx, o, "hash", JS_NewString(ctx, hsh)); else JS_SetPropertyStr(ctx, o, "hash", JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, o, "port", JS_NewString(ctx, ""));
    free(resolved);
    JSValue sp = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, sp, "__fields", JS_NewObject(ctx));
    { JSValue sv = JS_GetPropertyStr(ctx, o, "search"); sp_init(ctx, sp, sv); JS_FreeValue(ctx, sv); }   /* seed from the URL's existing query ("?a=1") */
    JS_SetPropertyStr(ctx, sp, "__owner", JS_DupValue(ctx, o));   /* back-ref for writeback (o<->sp cycle; cycle-GC collected) */
    JS_SetPropertyStr(ctx, sp, "get", JS_NewCFunction(ctx, js_sp_get, "get", 1));
    JS_SetPropertyStr(ctx, sp, "getAll", JS_NewCFunction(ctx, js_sp_get, "getAll", 1));
    JS_SetPropertyStr(ctx, sp, "has", JS_NewCFunction(ctx, js_sp_get, "has", 1));
    JS_SetPropertyStr(ctx, sp, "set", JS_NewCFunction(ctx, js_url_sp_set, "set", 2));
    JS_SetPropertyStr(ctx, sp, "append", JS_NewCFunction(ctx, js_url_sp_set, "append", 2));
    JS_SetPropertyStr(ctx, sp, "delete", JS_NewCFunction(ctx, js_url_sp_delete, "delete", 1));
    JS_SetPropertyStr(ctx, sp, "sort", JS_NewCFunction(ctx, js_noop, "sort", 0));
    JS_SetPropertyStr(ctx, sp, "forEach", JS_NewCFunction(ctx, js_sp_forEach, "forEach", 1));
    JS_SetPropertyStr(ctx, sp, "keys", JS_NewCFunction(ctx, js_sp_keys, "keys", 0));
    JS_SetPropertyStr(ctx, sp, "values", JS_NewCFunction(ctx, js_sp_values, "values", 0));
    JS_SetPropertyStr(ctx, sp, "entries", JS_NewCFunction(ctx, js_sp_entries, "entries", 0));
    JS_SetPropertyStr(ctx, sp, "toString", JS_NewCFunction(ctx, js_sp_tostring, "toString", 0));
    JS_SetPropertyStr(ctx, o, "searchParams", sp);
    JS_SetPropertyStr(ctx, o, "toString", JS_NewCFunction(ctx, js_url_tostring, "toString", 0));
    JS_SetPropertyStr(ctx, o, "toJSON", JS_NewCFunction(ctx, js_url_tostring, "toJSON", 0));
    return o;
}
/* URL.canParse(input, base): a static bool used to guard `new URL()`. Opaque/holey input -> true (don't
   collapse exploration on external input); concrete -> whether Lexbor actually parses it. */
static JSValue js_url_canparse(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc < 1) return JS_FALSE;
    if (JS_IsOpaque(argv[0])) return JS_TRUE;
    const char *input = JS_ToCString(ctx, argv[0]);
    const char *base = (argc >= 2 && JS_IsString(argv[1])) ? JS_ToCString(ctx, argv[1]) : NULL;
    int ok = 0;
    if (input && has_hole(input)) ok = 1;
    else if (input) { char *r = url_resolve(input, base ? base : g_origin); if (r) { ok = 1; free(r); } }
    if (input) JS_FreeCString(ctx, input);
    if (base) JS_FreeCString(ctx, base);
    return ok ? JS_TRUE : JS_FALSE;
}
/* new Request(input, init): fetch(new Request(url,{method})) is common. Resolve the url (shape-aware, like
   URL), expose .url/.method/.headers and toString->url so fetch(req) reads the endpoint. */
static JSValue js_request_ctor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv) {
    /* opaque input -> return the input opaque (url shape flows); concrete -> Lexbor-resolved url. */
    if (argc >= 1 && JS_IsOpaque(argv[0])) return JS_DupValue(ctx, argv[0]);
    const char *input = argc >= 1 ? JS_ToCString(ctx, argv[0]) : NULL;
    char *resolved = (input && !has_hole(input)) ? url_resolve(input, g_origin) : NULL;
    JSValue rshaped = (input && has_hole(input)) ? JS_NewOpaqueShaped(ctx, input) : JS_UNDEFINED;
    if (input) JS_FreeCString(ctx, input);
    if (!resolved) return JS_IsUndefined(rshaped) ? JS_DupValue(ctx, g_opaque) : rshaped;
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "url", JS_NewString(ctx, resolved));
    JS_SetPropertyStr(ctx, o, "href", JS_NewString(ctx, resolved));   /* toString reads href -> fetch(req) sees the url */
    free(resolved);
    JSValue method = JS_UNDEFINED;
    if (argc >= 2 && JS_IsObject(argv[1])) method = JS_GetPropertyStr(ctx, argv[1], "method");
    JS_SetPropertyStr(ctx, o, "method", JS_IsString(method) ? method : JS_NewString(ctx, "GET"));
    if (!JS_IsString(method)) JS_FreeValue(ctx, method);
    JS_SetPropertyStr(ctx, o, "toString", JS_NewCFunction(ctx, js_url_tostring, "toString", 0));
    return o;
}
/* A generic Web object whose reads are opaque (external input) and whose writes are no-ops: Headers,
   FormData, Blob, Response body, AbortController.signal, TextEncoder/Decoder. Prevents the ReferenceError
   that would kill the flow, and keeps any value read out of it OPAQUE (never a fabricated concrete). */
static JSValue js_webobj_ctor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv) {
    JSValue o = JS_NewObject(ctx);
    const char *op[] = { "get", "getAll", "has", "keys", "values", "entries", "forEach", "encode", "decode",
                         "text", "json", "arrayBuffer", "blob", "formData", "clone", "slice", "getReader" };
    for (size_t i = 0; i < sizeof op / sizeof op[0]; i++)
        JS_SetPropertyStr(ctx, o, op[i], JS_NewCFunction(ctx, js_opaque, op[i], 1));
    const char *np[] = { "set", "append", "delete", "abort", "add", "addEventListener" };
    for (size_t i = 0; i < sizeof np / sizeof np[0]; i++)
        JS_SetPropertyStr(ctx, o, np[i], JS_NewCFunction(ctx, js_noop, np[i], 2));
    JS_SetPropertyStr(ctx, o, "signal", JS_DupValue(ctx, g_opaque));   /* AbortController.signal */
    JS_SetPropertyStr(ctx, o, "toString", JS_NewCFunction(ctx, js_opaque, "toString", 0));
    return o;
}
/* URLSearchParams: a REAL object recording init + append()/set() fields (like FormData), so
   `new URLSearchParams({team:cfg.team,r:cfg.region}).toString()` -> a CONCOLIC query string carrying both the
   SHAPE ("team={team}&r={region}") and, when every value has a concrete example, the encoded EXAMPLE
   ("team=eng+team&r=us-east-1") -- not the old bare-opaque stub that discarded the params. get() returns the
   stored (concolic) value; unknown key -> opaque (external input). */
static void sp_encode(char *buf, int cap, int *o, const char *s) {   /* form-urlencode: space->+, reserved->%XX */
    static const char *hex = "0123456789ABCDEF";
    for (; *s && *o < cap - 3; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == ' ') buf[(*o)++] = '+';
        else if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c=='-'||c=='_'||c=='.'||c=='~') buf[(*o)++] = (char)c;
        else { buf[(*o)++] = '%'; buf[(*o)++] = hex[c >> 4]; buf[(*o)++] = hex[c & 15]; }
    }
    buf[*o] = 0;
}
static void sp_store(JSContext *ctx, JSValueConst this_val, JSValueConst key, JSValueConst val) {
    JSValue f = JS_GetPropertyStr(ctx, this_val, "__fields");
    if (!JS_IsObject(f)) { JS_FreeValue(ctx, f); f = JS_NewObject(ctx); JS_SetPropertyStr(ctx, this_val, "__fields", JS_DupValue(ctx, f)); }
    const char *k = JS_ToCString(ctx, key);
    if (k) { JS_SetPropertyStr(ctx, f, k, JS_DupValue(ctx, val)); JS_FreeCString(ctx, k); }
    JS_FreeValue(ctx, f);
}
static JSValue js_sp_append(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc >= 2) sp_store(ctx, this_val, argv[0], argv[1]);
    return JS_UNDEFINED;
}
static JSValue js_sp_get(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc >= 1) {
        JSValue f = JS_GetPropertyStr(ctx, this_val, "__fields");
        if (JS_IsObject(f)) {
            const char *k = JS_ToCString(ctx, argv[0]);
            if (k) { JSValue v = JS_GetPropertyStr(ctx, f, k); JS_FreeCString(ctx, k);
                     if (!JS_IsUndefined(v)) { JS_FreeValue(ctx, f); return v; } JS_FreeValue(ctx, v); }
        }
        JS_FreeValue(ctx, f);
    }
    return JS_DupValue(ctx, g_opaque);   /* unknown key -> external input */
}
static JSValue js_sp_tostring(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    JSValue f = JS_GetPropertyStr(ctx, this_val, "__fields");
    if (!JS_IsObject(f)) { JS_FreeValue(ctx, f); return JS_NewString(ctx, ""); }
    char shape[1600]; int so = 0; shape[0] = 0;
    char examp[1600]; int eo = 0; examp[0] = 0;
    int any_opaque = 0, all_examples = 1;
    JSPropertyEnum *tab = NULL; uint32_t n = 0;
    if (JS_GetOwnPropertyNames(ctx, &tab, &n, f, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
        for (uint32_t i = 0; i < n; i++) {
            const char *k = JS_AtomToCString(ctx, tab[i].atom);
            JSValue v = JS_GetProperty(ctx, f, tab[i].atom);
            int op = JS_IsOpaque(v); if (op) any_opaque = 1;
            const char *vsh; int free_vsh = 0;
            if (op) vsh = JS_OpaqueShapeC(v); else { vsh = JS_ToCString(ctx, v); free_vsh = 1; }
            if (k && vsh && so < (int)sizeof(shape) - 2) so += snprintf(shape + so, sizeof(shape) - so, "%s%s=%s", so ? "&" : "", k, vsh);
            JSValue vex = op ? JS_OpaqueExample(ctx, v) : JS_DupValue(ctx, v);
            if (JS_IsUndefined(vex) || JS_IsNull(vex)) all_examples = 0;
            else if (k && eo < (int)sizeof(examp) - 2) {
                const char *ve = JS_ToCString(ctx, vex);
                if (ve) { eo += snprintf(examp + eo, sizeof(examp) - eo, "%s%s=", eo ? "&" : "", k); sp_encode(examp, (int)sizeof(examp), &eo, ve); JS_FreeCString(ctx, ve); }
                else all_examples = 0;
            }
            JS_FreeValue(ctx, vex);
            if (vsh && free_vsh) JS_FreeCString(ctx, vsh);
            if (k) JS_FreeCString(ctx, k);
            JS_FreeValue(ctx, v);
        }
        JS_FreePropertyEnum(ctx, tab, n);
    }
    JS_FreeValue(ctx, f);
    if (!any_opaque) return JS_NewStringLen(ctx, shape, so);   /* all-concrete params -> a plain string */
    JSValue r = JS_NewOpaqueSourced(ctx, shape, "{}");         /* tainted query keeps its shape + @S taint */
    if (all_examples && JS_IsOpaque(r)) JS_SetOpaqueExample(ctx, r, JS_NewStringLen(ctx, examp, eo));
    return r;
}
static void sp_init(JSContext *ctx, JSValueConst o, JSValueConst init) {
    if (JS_IsString(init)) {                                    /* "a=1&b=2" (leading '?' tolerated) */
        const char *s = JS_ToCString(ctx, init); if (!s) return;
        const char *p = s; if (*p == '?') p++;
        while (*p) {
            const char *amp = strchr(p, '&'); const char *end = amp ? amp : p + strlen(p);
            const char *eq = memchr(p, '=', (size_t)(end - p));
            if (eq) { JSValue k = JS_NewStringLen(ctx, p, (size_t)(eq - p)); JSValue v = JS_NewStringLen(ctx, eq + 1, (size_t)(end - eq - 1));
                      sp_store(ctx, o, k, v); JS_FreeValue(ctx, k); JS_FreeValue(ctx, v); }
            if (!amp) break; p = amp + 1;
        }
        JS_FreeCString(ctx, s);
    } else if (JS_IsObject(init)) {                            /* {k:v} record (the common case) */
        JSPropertyEnum *tab = NULL; uint32_t n = 0;
        if (JS_GetOwnPropertyNames(ctx, &tab, &n, init, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
            for (uint32_t i = 0; i < n; i++) {
                JSValue kv = JS_AtomToString(ctx, tab[i].atom);
                JSValue v = JS_GetProperty(ctx, init, tab[i].atom);
                sp_store(ctx, o, kv, v);
                JS_FreeValue(ctx, kv); JS_FreeValue(ctx, v);
            }
            JS_FreePropertyEnum(ctx, tab, n);
        }
    }
}
static JSValue js_sp_forEach(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) return JS_UNDEFINED;
    JSValue f = JS_GetPropertyStr(ctx, this_val, "__fields");
    if (JS_IsObject(f)) {
        JSPropertyEnum *tab = NULL; uint32_t n = 0;
        if (JS_GetOwnPropertyNames(ctx, &tab, &n, f, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
            for (uint32_t i = 0; i < n; i++) {
                JSValue v = JS_GetProperty(ctx, f, tab[i].atom);
                JSValue k = JS_AtomToString(ctx, tab[i].atom);
                JSValueConst args[3] = { v, k, this_val };   /* spec order: (value, key, parent) */
                JSValue r = JS_Call(ctx, argv[0], JS_UNDEFINED, 3, args);
                JS_FreeValue(ctx, r); JS_FreeValue(ctx, v); JS_FreeValue(ctx, k);
            }
            JS_FreePropertyEnum(ctx, tab, n);
        }
    }
    JS_FreeValue(ctx, f);
    return JS_UNDEFINED;
}
/* keys/values/entries -> a plain ARRAY (iterable: for-of / spread / Array.from all work; an opaque stub would
   throw in for-of). mode: 0=keys 1=values 2=entries. */
static JSValue sp_iter_build(JSContext *ctx, JSValueConst this_val, int mode) {
    JSValue arr = JS_NewArray(ctx); uint32_t idx = 0;
    JSValue f = JS_GetPropertyStr(ctx, this_val, "__fields");
    if (JS_IsObject(f)) {
        JSPropertyEnum *tab = NULL; uint32_t n = 0;
        if (JS_GetOwnPropertyNames(ctx, &tab, &n, f, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
            for (uint32_t i = 0; i < n; i++) {
                JSValue k = JS_AtomToString(ctx, tab[i].atom);
                JSValue v = JS_GetProperty(ctx, f, tab[i].atom);
                JSValue item;
                if (mode == 0) { item = k; JS_FreeValue(ctx, v); }
                else if (mode == 1) { item = v; JS_FreeValue(ctx, k); }
                else { item = JS_NewArray(ctx); JS_SetPropertyUint32(ctx, item, 0, k); JS_SetPropertyUint32(ctx, item, 1, v); }
                JS_SetPropertyUint32(ctx, arr, idx++, item);
            }
            JS_FreePropertyEnum(ctx, tab, n);
        }
    }
    JS_FreeValue(ctx, f);
    return arr;
}
static JSValue js_sp_keys(JSContext *ctx, JSValueConst t, int c, JSValueConst *v)    { return sp_iter_build(ctx, t, 0); }
static JSValue js_sp_values(JSContext *ctx, JSValueConst t, int c, JSValueConst *v)  { return sp_iter_build(ctx, t, 1); }
static JSValue js_sp_entries(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) { return sp_iter_build(ctx, t, 2); }
/* Serialize a searchParams' __fields to a CONCRETE query string (no leading '?'): concrete values %-encoded,
   an opaque value -> its example (%-encoded) if known, else its literal {shape} hole. malloc'd; caller frees. */
static char *sp_serialize(JSContext *ctx, JSValueConst sp) {
    char buf[1600]; int o = 0; buf[0] = 0;
    JSValue f = JS_GetPropertyStr(ctx, sp, "__fields");
    if (JS_IsObject(f)) {
        JSPropertyEnum *tab = NULL; uint32_t n = 0;
        if (JS_GetOwnPropertyNames(ctx, &tab, &n, f, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
            for (uint32_t i = 0; i < n; i++) {
                const char *k = JS_AtomToCString(ctx, tab[i].atom);
                JSValue v = JS_GetProperty(ctx, f, tab[i].atom);
                char *valstr = NULL; int is_shape = 0;
                if (JS_IsOpaque(v)) {
                    JSValue ex = JS_OpaqueExample(ctx, v);
                    if (!JS_IsUndefined(ex)) { const char *s = JS_ToCString(ctx, ex); if (s) { valstr = strdup(s); JS_FreeCString(ctx, s); } }
                    JS_FreeValue(ctx, ex);
                    if (!valstr) { const char *sh = JS_OpaqueShapeC(v); valstr = strdup(sh ? sh : "{}"); is_shape = 1; }
                } else { const char *s = JS_ToCString(ctx, v); if (s) { valstr = strdup(s); JS_FreeCString(ctx, s); } }
                if (k && valstr && o < (int)sizeof(buf) - 2) {
                    o += snprintf(buf + o, sizeof(buf) - o, "%s%s=", o ? "&" : "", k);
                    if (is_shape) o += snprintf(buf + o, sizeof(buf) - o, "%s", valstr);   /* keep {hole} literal for url_solve/build_query */
                    else sp_encode(buf, (int)sizeof(buf), &o, valstr);
                }
                free(valstr);
                if (k) JS_FreeCString(ctx, k);
                JS_FreeValue(ctx, v);
            }
            JS_FreePropertyEnum(ctx, tab, n);
        }
    }
    JS_FreeValue(ctx, f);
    return strdup(buf);
}
/* After a set/append/delete on url.searchParams, rebuild the owner URL's search + href so a later fetch(url)
   / url.href read sees the app-built query. */
static void url_sp_writeback(JSContext *ctx, JSValueConst sp) {
    JSValue owner = JS_GetPropertyStr(ctx, sp, "__owner");
    if (!JS_IsObject(owner)) { JS_FreeValue(ctx, owner); return; }
    char *q = sp_serialize(ctx, sp);
    char search[1700]; if (q && q[0]) snprintf(search, sizeof search, "?%s", q); else search[0] = 0;
    JS_SetPropertyStr(ctx, owner, "search", JS_NewString(ctx, search));
    JSValue vo = JS_GetPropertyStr(ctx, owner, "origin");   const char *origin = JS_ToCString(ctx, vo);
    JSValue vp = JS_GetPropertyStr(ctx, owner, "pathname"); const char *path   = JS_ToCString(ctx, vp);
    JSValue vh = JS_GetPropertyStr(ctx, owner, "hash");     const char *hash   = JS_ToCString(ctx, vh);
    char href[2048]; snprintf(href, sizeof href, "%s%s%s%s", origin ? origin : "", path ? path : "", search, hash ? hash : "");
    JS_SetPropertyStr(ctx, owner, "href", JS_NewString(ctx, href));
    if (origin) JS_FreeCString(ctx, origin); if (path) JS_FreeCString(ctx, path); if (hash) JS_FreeCString(ctx, hash);
    JS_FreeValue(ctx, vo); JS_FreeValue(ctx, vp); JS_FreeValue(ctx, vh);
    free(q); JS_FreeValue(ctx, owner);
}
static JSValue js_url_sp_set(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc >= 2) sp_store(ctx, this_val, argv[0], argv[1]);   /* __fields is name-keyed: append coalesces to set (matches standalone) */
    url_sp_writeback(ctx, this_val);
    return JS_UNDEFINED;
}
static JSValue js_url_sp_delete(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc >= 1) {
        JSValue f = JS_GetPropertyStr(ctx, this_val, "__fields");
        if (JS_IsObject(f)) { const char *k = JS_ToCString(ctx, argv[0]);
            if (k) { JSAtom a = JS_NewAtom(ctx, k); JS_DeleteProperty(ctx, f, a, 0); JS_FreeAtom(ctx, a); JS_FreeCString(ctx, k); } }
        JS_FreeValue(ctx, f);
    }
    url_sp_writeback(ctx, this_val);
    return JS_UNDEFINED;
}
static JSValue js_searchparams_ctor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv) {
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "__fields", JS_NewObject(ctx));
    if (argc >= 1 && !JS_IsUndefined(argv[0]) && !JS_IsNull(argv[0])) sp_init(ctx, o, argv[0]);
    JS_SetPropertyStr(ctx, o, "get", JS_NewCFunction(ctx, js_sp_get, "get", 1));
    JS_SetPropertyStr(ctx, o, "getAll", JS_NewCFunction(ctx, js_sp_get, "getAll", 1));
    JS_SetPropertyStr(ctx, o, "has", JS_NewCFunction(ctx, js_sp_get, "has", 1));
    JS_SetPropertyStr(ctx, o, "append", JS_NewCFunction(ctx, js_sp_append, "append", 2));
    JS_SetPropertyStr(ctx, o, "set", JS_NewCFunction(ctx, js_sp_append, "set", 2));
    JS_SetPropertyStr(ctx, o, "delete", JS_NewCFunction(ctx, js_url_sp_delete, "delete", 1));   /* real: removes from __fields (no __owner -> writeback no-ops) */
    JS_SetPropertyStr(ctx, o, "sort", JS_NewCFunction(ctx, js_noop, "sort", 0));
    JS_SetPropertyStr(ctx, o, "forEach", JS_NewCFunction(ctx, js_sp_forEach, "forEach", 1));
    JS_SetPropertyStr(ctx, o, "keys", JS_NewCFunction(ctx, js_sp_keys, "keys", 0));
    JS_SetPropertyStr(ctx, o, "values", JS_NewCFunction(ctx, js_sp_values, "values", 0));
    JS_SetPropertyStr(ctx, o, "entries", JS_NewCFunction(ctx, js_sp_entries, "entries", 0));
    JS_SetPropertyStr(ctx, o, "toString", JS_NewCFunction(ctx, js_sp_tostring, "toString", 0));
    return o;
}
/* Headers: a REAL object storing append()/set() header fields (__fields), so `fetch(url,{headers:h})` with a
   `new Headers()` surfaces the REAL required headers (auth/CSRF/custom) instead of the object's own METHODS
   (the old js_webobj_ctor stub -> the header capture iterated get/set/has/... as bogus headers). js_fetch
   reads __fields when present. */
static JSValue js_headers_ctor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv) {
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "__fields", JS_NewObject(ctx));
    if (argc >= 1 && !JS_IsUndefined(argv[0]) && !JS_IsNull(argv[0])) sp_init(ctx, o, argv[0]);
    JS_SetPropertyStr(ctx, o, "append", JS_NewCFunction(ctx, js_sp_append, "append", 2));
    JS_SetPropertyStr(ctx, o, "set", JS_NewCFunction(ctx, js_sp_append, "set", 2));
    JS_SetPropertyStr(ctx, o, "get", JS_NewCFunction(ctx, js_sp_get, "get", 1));
    JS_SetPropertyStr(ctx, o, "has", JS_NewCFunction(ctx, js_sp_get, "has", 1));
    JS_SetPropertyStr(ctx, o, "getSetCookie", JS_NewCFunction(ctx, js_sp_get, "getSetCookie", 0));
    JS_SetPropertyStr(ctx, o, "delete", JS_NewCFunction(ctx, js_url_sp_delete, "delete", 1));   /* real: removes from __fields */
    JS_SetPropertyStr(ctx, o, "forEach", JS_NewCFunction(ctx, js_sp_forEach, "forEach", 1));
    JS_SetPropertyStr(ctx, o, "keys", JS_NewCFunction(ctx, js_sp_keys, "keys", 0));
    JS_SetPropertyStr(ctx, o, "values", JS_NewCFunction(ctx, js_sp_values, "values", 0));
    JS_SetPropertyStr(ctx, o, "entries", JS_NewCFunction(ctx, js_sp_entries, "entries", 0));
    return o;
}
static JSValue js_branch(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{ return branch_decide(ctx, argc > 0 ? argv[0] : JS_UNDEFINED) ? JS_TRUE : JS_FALSE; }

/* __opaque(): return the OPAQUE sentinel — external input the tool must not concretely decide. A branch
   on it (if(__opaque())) auto-forks BOTH arms via the engine OP_if hook, no explicit __branch needed. */
static JSValue js_opaque(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{ return JS_DupValue(ctx, g_opaque); }
/* Minimal host-edge stubs for a browser bundle: a no-op (addEventListener etc — the handler stays a
   never-fired function, so orphan-invoke drives it), and an opaque-returning stub (DOM reads that are
   external input the tool must not concretely decide). A missing capability is a missing stub, never a
   parallel resolver — the real Lexbor DOM replaces these when the host is wired. */
static JSValue js_noop(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) { return JS_UNDEFINED; }
static JSValue js_opaque_stub(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) { return JS_DupValue(ctx, g_opaque); }
/* A CONSTRUCTABLE base class so `class X extends HTMLElement {…}` DEFINES (else it throws and the whole Web
   Component — with its connectedCallback endpoints/sinks — is LOST). super() returns the default derived
   `this`; connectedCallback then becomes an uncalled method the orphan driver reaches like any other. */
static JSValue js_ctor_stub(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv) { return JS_UNDEFINED; }
static void def_ctor(JSContext *ctx, JSValueConst g, const char *name) {
    JSValue c = JS_NewCFunction2(ctx, js_ctor_stub, name, 0, JS_CFUNC_constructor, 0);
    JSValue proto = JS_NewObject(ctx);
    JS_SetConstructor(ctx, c, proto);   /* c.prototype = proto (an OBJECT) so `class X extends <c>` is valid */
    JS_FreeValue(ctx, proto);
    JS_SetPropertyStr(ctx, g, name, c);
}
/* addEventListener(type, handler): a registered handler that NEVER FIRES is exactly the unused surface —
   keep it reachable (in g_handlers) so orphan-invoke drives it and surfaces its gated endpoints. */
static JSValue g_handlers = JS_UNDEFINED;
static int g_handler_n = 0;
/* Handlers registered for 'message' (postMessage). Driven with a synthetic MessageEvent whose .data is
   the source-tagged opaque {pm}, so a postMessage-XSS sink reports {pm} and the PoC assembler builds a
   postMessage-delivered PoC. Borrowed refs (also held live in g_handlers). */
static void *g_msg_handlers[128];
static int g_msg_handler_n = 0;
static JSValue g_msg_event = JS_UNDEFINED;   /* synthetic MessageEvent: { data: opaque("{pm}"), origin, source } */
/* Handlers RE-REGISTERED during a candidate boot_replay (addEventListener) — captured so a closure handler
   (which isn't on any global) can be re-resolved to its candidate-closure version. Transient per candidate
   flow; cleared after the drive. */
static JSValue *g_replay_handlers = NULL; static int g_replay_handler_n = 0, g_replay_handler_cap = 0;
static void *g_replay_msg[128]; static int g_replay_msg_n = 0;   /* re-registered 'message' handler ptrs (candidate closure) */
static void replay_handlers_clear(JSContext *ctx) {
    for (int i = 0; i < g_replay_handler_n; i++) JS_FreeValue(ctx, g_replay_handlers[i]);
    g_replay_handler_n = 0; g_replay_msg_n = 0;
}
static JSValue js_add_listener(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    JSValueConst h0 = (argc >= 2) ? argv[1] : (argc >= 1 ? argv[0] : JS_UNDEFINED);
    if (g_in_boot_flow) return JS_UNDEFINED;   /* boot flow re-run: handlers already registered by the initial boot — don't duplicate g_handlers */
    if (g_boot_replay) {   /* capture the re-registered handler (candidate closure) for re-resolution; don't grow g_handlers */
        if (JS_IsFunction(ctx, h0)) {
            if (g_replay_handler_n >= g_replay_handler_cap) { int nc = g_replay_handler_cap ? g_replay_handler_cap * 2 : 16;
                JSValue *n = realloc(g_replay_handlers, (size_t)nc * sizeof(JSValue)); if (n) { g_replay_handlers = n; g_replay_handler_cap = nc; } }
            if (g_replay_handler_n < g_replay_handler_cap) g_replay_handlers[g_replay_handler_n++] = JS_DupValue(ctx, h0);
            const char *type = argc >= 2 ? JS_ToCString(ctx, argv[0]) : NULL;   /* a re-registered 'message' handler must still be driven with the {pm} event */
            if (type && strcmp(type, "message") == 0 && g_replay_msg_n < 128) g_replay_msg[g_replay_msg_n++] = JS_VALUE_GET_PTR(h0);
            if (type) JS_FreeCString(ctx, type);
        }
        return JS_UNDEFINED;
    }
    JSValueConst h = (argc >= 2) ? argv[1] : (argc >= 1 ? argv[0] : JS_UNDEFINED);
    if (JS_IsFunction(ctx, h) && !JS_IsUndefined(g_handlers)) {
        JS_SetPropertyUint32(ctx, g_handlers, (uint32_t)g_handler_n++, JS_DupValue(ctx, h));
        const char *type = argc >= 2 ? JS_ToCString(ctx, argv[0]) : NULL;   /* addEventListener(type, handler) */
        if (type && strcmp(type, "message") == 0 && g_msg_handler_n < 128)
            g_msg_handlers[g_msg_handler_n++] = JS_VALUE_GET_PTR(h);
        if (type) JS_FreeCString(ctx, type);
    }
    return JS_UNDEFINED;
}
static int is_msg_handler(JSValueConst h) {
    void *p = JS_VALUE_GET_PTR(h);
    for (int i = 0; i < g_msg_handler_n; i++) if (g_msg_handlers[i] == p) return 1;
    for (int i = 0; i < g_replay_msg_n; i++) if (g_replay_msg[i] == p) return 1;   /* re-resolved candidate 'message' closure */
    return 0;
}

/* The page's OWN identity (principal) is CONCRETE for URL building — location.origin/protocol/host/
   hostname/port/pathname/href are REAL (a bundle does `location.origin + '/api/...'`; opaque here would
   yield a "{}"-shaped garbage URL and lose the endpoint). Only the EXTERNAL-INPUT parts — search/hash —
   stay OPAQUE (must never force a branch), yet carry a concrete example when page state already has one.
   The host injects the real principal at wire time; g_origin is the node-harness placeholder. */
static const char *g_origin   = "https://app.example.com";   /* host-injected real page principal (argv[3]); placeholder for node tests */
static const char *g_protocol = "https:";
static const char *g_host     = "app.example.com";   /* host = hostname[:port] (location.host) */
static const char *g_hostname = "app.example.com";   /* hostname WITHOUT port (location.hostname) */
static const char *g_port     = "";                  /* port only, "" for default (location.port) */
/* Split a real origin ("https://localhost:8765") into protocol + host + hostname + port for the concrete
   location.*. A bundle that builds a URL from location.hostname/port (`"https://api."+location.hostname`)
   must see the port split out correctly, else the learned endpoint host is wrong. */
static void set_origin(const char *origin) {
    static char protobuf[64], hostbuf[256], hostnamebuf[256], portbuf[16];
    const char *p;
    if (!origin || !origin[0]) return;
    g_origin = origin;
    p = strstr(origin, "://");
    if (!p) return;
    { size_t plen = (size_t)(p - origin) + 1; if (plen < sizeof protobuf) { memcpy(protobuf, origin, plen); protobuf[plen] = 0; g_protocol = protobuf; } }
    { const char *h = p + 3; size_t hlen = strlen(h); const char *slash = strchr(h, '/'); if (slash) hlen = (size_t)(slash - h);
      if (hlen < sizeof hostbuf) { memcpy(hostbuf, h, hlen); hostbuf[hlen] = 0; g_host = hostbuf;
        /* split host into hostname + port on the LAST ':' (an IPv6 literal is bracketed [::1]:port, so a ':'
           after a ']' or with no '[' is the port separator). */
        const char *colon = strrchr(hostbuf, ':');
        const char *rbrack = strrchr(hostbuf, ']');
        if (colon && colon > rbrack) {   /* has a port */
          size_t nlen = (size_t)(colon - hostbuf);
          if (nlen < sizeof hostnamebuf) { memcpy(hostnamebuf, hostbuf, nlen); hostnamebuf[nlen] = 0; g_hostname = hostnamebuf; }
          size_t plen2 = strlen(colon + 1); if (plen2 < sizeof portbuf) { memcpy(portbuf, colon + 1, plen2 + 1); g_port = portbuf; }
        } else { g_hostname = hostbuf; g_port = ""; }   /* default port */
      } }
}
/* An external-input SOURCE getter (location.hash=magic 0, location.search=magic 1). Normal exploration:
   returns the source-tagged OPAQUE (control-flow forks, @H/@S record the shape). @S REPLAY flow (g_candidate
   set): returns the CONCRETE candidate string, so the real code runs the transforms concretely and the sink
   sees a real value — reachability + breakout decided by the REAL code, in the ONE scheduler. */
static const char *g_source_tag[] = { "{hash}", "{search}", "{pm}" };   /* 0=location.hash 1=location.search 2=postMessage e.data */
static const char *g_source_pfx[] = { "#", "?", "" };                   /* realistic leading char so slice(1)/substring behave faithfully */
static JSValue js_source_get(JSContext *ctx, JSValueConst this_val, int magic) {
    if (g_candidate) {   /* a replay flow injects the CONCRETE candidate — with the source's real prefix, so
                            code that strips it (location.hash.slice(1), search.substring(1)) sees the true payload. */
        const char *pfx = g_source_pfx[magic]; size_t lp = strlen(pfx), lc = strlen(g_candidate);
        char *buf = malloc(lp + lc + 1); if (!buf) return JS_NewString(ctx, g_candidate);
        memcpy(buf, pfx, lp); memcpy(buf + lp, g_candidate, lc + 1);
        if (magic == 2) {   /* postMessage e.data can be an OBJECT: return a CANDIDATE-CARRIER opaque so a FIELD
                               sink (`{html}=e.data; el.innerHTML=html`) delivers the candidate, while whole-value
                               use (`el.innerHTML=e.data`) reads the same candidate as the example. */
            JSValue c = JS_NewOpaqueSourced(ctx, g_source_tag[2], g_source_tag[2]);
            JS_SetOpaqueExample(ctx, c, JS_NewString(ctx, buf)); JS_SetOpaqueCarrier(ctx, c);
            free(buf); return c;
        }
        JSValue r = JS_NewString(ctx, buf); free(buf); return r;
    }
    return JS_NewOpaqueSourced(ctx, g_source_tag[magic], g_source_tag[magic]);   /* stamp root source identity for the per-flow value domain */
}
static void def_source(JSContext *ctx, JSValueConst loc, const char *name, int magic) {
    JSAtom a = JS_NewAtom(ctx, name);
    JS_DefinePropertyGetSet(ctx, loc, a,
        JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)js_source_get, "get", 0, JS_CFUNC_getter_magic, magic),
        JS_UNDEFINED, JS_PROP_CONFIGURABLE);
    JS_FreeAtom(ctx, a);
}
/* @S: a NAVIGATION (location.href=/.assign/.replace, or location=) is a sink — a javascript: URL runs,
   an attacker-controlled URL is an open redirect. url context (solve_broke checks a javascript:X9 prefix). */
static JSValue js_location_nav(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    if (argc >= 1) solve_add(ctx, "location.assign", "url", argv[0]);
    return JS_UNDEFINED;
}
static JSValue js_location_href_get(JSContext *ctx, JSValueConst t) { return JS_NewString(ctx, g_origin); }  /* concrete base for new URL(path, location.href) */
static JSValue js_location_href_set(JSContext *ctx, JSValueConst t, JSValueConst val) {
    solve_add(ctx, "location.href", "url", val);   /* @S: location.href = javascript:/redirect */
    return JS_UNDEFINED;
}
static JSValue g_location = JS_UNDEFINED;   /* the location object; window.location is a getset over it so `location = url` is a nav sink */
/* document.currentScript: the executing <script> element during boot — the CONFIG-INJECTION source (an embed
   reads `document.currentScript.dataset.apiKey` / .getAttribute('data-endpoint')). Set per-script in
   dom_run_scripts, NULL otherwise. */
static JSValue g_current_script = JS_NULL;
static JSValue js_doc_currentscript(JSContext *ctx, JSValueConst t) { return JS_DupValue(ctx, g_current_script); }
static JSValue js_window_location_get(JSContext *ctx, JSValueConst t) { return JS_DupValue(ctx, g_location); }
static JSValue js_window_location_set(JSContext *ctx, JSValueConst t, JSValueConst val) {
    solve_add(ctx, "location", "url", val);   /* @S: `location = url` / `window.location = url` navigation */
    return JS_UNDEFINED;
}
static JSValue make_location(JSContext *ctx)
{
    JSValue loc = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, loc, "origin",   JS_NewString(ctx, g_origin));
    JS_SetPropertyStr(ctx, loc, "protocol", JS_NewString(ctx, g_protocol));
    JS_SetPropertyStr(ctx, loc, "host",     JS_NewString(ctx, g_host));       /* hostname[:port] */
    JS_SetPropertyStr(ctx, loc, "hostname", JS_NewString(ctx, g_hostname));   /* WITHOUT port */
    JS_SetPropertyStr(ctx, loc, "port",     JS_NewString(ctx, g_port));       /* port only ("" = default) */
    JS_SetPropertyStr(ctx, loc, "pathname", JS_NewString(ctx, "/"));
    JSAtom ha = JS_NewAtom(ctx, "href");   /* href: getter = concrete base; SETTER = @S navigation sink */
    JS_DefinePropertyGetSet(ctx, loc, ha,
        JS_NewCFunction2(ctx, (JSCFunction *)js_location_href_get, "get href", 0, JS_CFUNC_getter, 0),
        JS_NewCFunction2(ctx, (JSCFunction *)js_location_href_set, "set href", 1, JS_CFUNC_setter, 0), JS_PROP_CONFIGURABLE);
    JS_FreeAtom(ctx, ha);
    JS_SetPropertyStr(ctx, loc, "assign",  JS_NewCFunction(ctx, js_location_nav, "assign", 1));   /* @S nav sinks */
    JS_SetPropertyStr(ctx, loc, "replace", JS_NewCFunction(ctx, js_location_nav, "replace", 1));
    JS_SetPropertyStr(ctx, loc, "reload",  JS_NewCFunction(ctx, js_noop, "reload", 0));
    JS_SetPropertyStr(ctx, loc, "toString", JS_NewCFunction(ctx, (JSCFunction *)js_location_href_get, "toString", 0));
    def_source(ctx, loc, "hash",   0);   /* external input: opaque (or candidate on @S replay) */
    def_source(ctx, loc, "search", 1);
    return loc;
}

/* ── Real DOM (Lexbor) ───────────────────────────────────────────────────────────
   The page's own structure/config is CONCRETE (a bundle reads `#cfg[data-api]` and builds a real
   URL); only external-input values stay opaque. document.* is backed by a live Lexbor DOM parsed from
   the page HTML, NOT a bridge-side scrape — so it can carry real attribute values now, and become
   per-flow COW state + intercept dynamic script injection (steps C/D). */
static lxb_html_document_t *g_dom = NULL;   /* parser + selectors are per-call (reuse corrupts the next query) */
static JSClassID g_el_class_id;

static JSValue el_wrap(JSContext *ctx, lxb_dom_element_t *el) {
    if (!el) return JS_NULL;
    JSValue o = JS_NewObjectClass(ctx, g_el_class_id);
    if (JS_IsException(o)) return o;
    JS_SetOpaque(o, el);
    return o;
}
/* new Image()/Audio()/Option(): missing constructors threw (killing the script). Return a REAL wrapped
   element (its methods/attrs work); .src is stored via the element's src setter — NOT emitted as @H, since
   an image/media load is a STATIC ASSET (magic-byte, not URL), so emitting would pollute with asset noise. */
static JSValue js_media_el_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv) {
    if (!g_dom) return JS_NewObject(ctx);
    lxb_dom_element_t *el = lxb_dom_document_create_element(lxb_dom_interface_document(g_dom), (const lxb_char_t *)"img", 3, NULL);
    return el ? el_wrap(ctx, el) : JS_NewObject(ctx);
}
struct sel_ctx { lxb_dom_element_t *first; };
static lxb_status_t sel_first_cb(lxb_dom_node_t *node, lxb_css_selector_specificity_t s, void *vp) {
    struct sel_ctx *c = vp; (void)s;
    if (!c->first) c->first = lxb_dom_interface_element(node);
    return LXB_STATUS_OK;   /* let the traversal COMPLETE so g_sel resets cleanly for the next find
                               (returning STOP mid-walk leaves internal state that breaks reuse) */
}
/* Find the first element matching a CSS selector, searching the whole parsed document. Fresh selectors
   object + cleaned parser per call — reusing them across finds carries internal state that breaks the
   next query (2nd querySelector returned null). The matched element is owned by g_dom, so tearing down
   the per-call selectors/list doesn't free it. */
/* `root` scopes the search: an ELEMENT node -> its subtree (element.querySelector), NULL -> the whole
   document (document.querySelector/getElementById). Document-scoping an element query returns matches
   OUTSIDE the receiver's subtree -> the wrong element -> a wrong learned value; the root fixes that. */
static lxb_dom_element_t *dom_select_first(lxb_dom_node_t *root, const char *sel, size_t len) {
    if (!root) root = g_dom ? lxb_dom_interface_node(g_dom) : NULL;
    if (!root) return NULL;
    lxb_css_parser_t *p = lxb_css_parser_create();
    if (!p || lxb_css_parser_init(p, NULL) != LXB_STATUS_OK) { if (p) lxb_css_parser_destroy(p, true); return NULL; }
    lxb_css_selector_list_t *list = lxb_css_selectors_parse(p, (const lxb_char_t *)sel, len);
    if (!list) { lxb_css_parser_destroy(p, true); return NULL; }
    lxb_selectors_t *s = lxb_selectors_create();
    if (!s || lxb_selectors_init(s) != LXB_STATUS_OK) { if (s) lxb_selectors_destroy(s, true); lxb_css_parser_destroy(p, true); return NULL; }
    struct sel_ctx c = { NULL };
    lxb_selectors_find(s, root, list, sel_first_cb, &c);
    lxb_selectors_destroy(s, true);
    lxb_css_parser_destroy(p, true);   /* frees the list too (parser owns it); c.first lives in g_dom */
    return c.first;
}
/* querySelectorAll / getElementsBy* : collect ALL matches into a real array so a for-of/forEach over the
   result iterates real elements (their attrs/methods drive endpoints) — an empty NodeList would starve
   an inline loop body of coverage. Same root-scoping as dom_select_first. */
struct sel_all_ctx { JSContext *ctx; JSValue arr; uint32_t n; };
static lxb_status_t sel_all_cb(lxb_dom_node_t *node, lxb_css_selector_specificity_t s, void *vp) {
    struct sel_all_ctx *c = vp; (void)s;
    JS_SetPropertyUint32(c->ctx, c->arr, c->n++, el_wrap(c->ctx, lxb_dom_interface_element(node)));
    return LXB_STATUS_OK;
}
static JSValue dom_select_all(JSContext *ctx, lxb_dom_node_t *root, const char *sel, size_t len) {
    JSValue arr = JS_NewArray(ctx);
    if (!root) root = g_dom ? lxb_dom_interface_node(g_dom) : NULL;
    if (!root) return arr;
    lxb_css_parser_t *p = lxb_css_parser_create();
    if (!p || lxb_css_parser_init(p, NULL) != LXB_STATUS_OK) { if (p) lxb_css_parser_destroy(p, true); return arr; }
    lxb_css_selector_list_t *list = lxb_css_selectors_parse(p, (const lxb_char_t *)sel, len);
    if (!list) { lxb_css_parser_destroy(p, true); return arr; }
    lxb_selectors_t *s = lxb_selectors_create();
    if (!s || lxb_selectors_init(s) != LXB_STATUS_OK) { if (s) lxb_selectors_destroy(s, true); lxb_css_parser_destroy(p, true); return arr; }
    struct sel_all_ctx c = { ctx, arr, 0 };
    lxb_selectors_find(s, root, list, sel_all_cb, &c);
    lxb_selectors_destroy(s, true);
    lxb_css_parser_destroy(p, true);
    return arr;
}
/* REAL element.matches(sel): does THIS element match the selector? A single-node Lexbor match — deterministic
   over the parsed DOM (element state is the app's own DOM, not attacker input), so COMPUTE the real bool
   (RUN, DON'T MATCH), never a stub. closest walks node->parent running the same real match. */
static lxb_status_t sel_match_cb(lxb_dom_node_t *node, lxb_css_selector_specificity_t s, void *vp) { (void)node; (void)s; *(int *)vp = 1; return LXB_STATUS_OK; }
static int dom_node_matches(lxb_dom_element_t *el, const char *sel, size_t len) {
    if (!el || !sel) return 0;
    lxb_css_parser_t *p = lxb_css_parser_create();
    if (!p || lxb_css_parser_init(p, NULL) != LXB_STATUS_OK) { if (p) lxb_css_parser_destroy(p, true); return 0; }
    lxb_css_selector_list_t *list = lxb_css_selectors_parse(p, (const lxb_char_t *)sel, len);
    if (!list) { lxb_css_parser_destroy(p, true); return 0; }
    lxb_selectors_t *s = lxb_selectors_create();
    if (!s || lxb_selectors_init(s) != LXB_STATUS_OK) { if (s) lxb_selectors_destroy(s, true); lxb_css_parser_destroy(p, true); return 0; }
    int matched = 0;
    lxb_selectors_match_node(s, lxb_dom_interface_node(el), list, sel_match_cb, &matched);
    lxb_selectors_destroy(s, true);
    lxb_css_parser_destroy(p, true);
    return matched;
}
static JSValue js_el_querySelectorAll(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc < 1) return JS_NewArray(ctx);
    const char *s = JS_ToCString(ctx, argv[0]); if (!s) return JS_NewArray(ctx);
    lxb_dom_element_t *self_el = JS_GetOpaque(this_val, g_el_class_id);
    JSValue r = dom_select_all(ctx, self_el ? lxb_dom_interface_node(self_el) : NULL, s, strlen(s));
    JS_FreeCString(ctx, s); return r;
}
static JSValue js_doc_querySelectorAll(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc < 1) return JS_NewArray(ctx);
    const char *s = JS_ToCString(ctx, argv[0]); if (!s) return JS_NewArray(ctx);
    JSValue r = dom_select_all(ctx, NULL, s, strlen(s)); JS_FreeCString(ctx, s); return r;
}
static JSValue js_doc_getByClass(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc < 1) return JS_NewArray(ctx);
    const char *s = JS_ToCString(ctx, argv[0]); if (!s) return JS_NewArray(ctx);
    char sel[256]; snprintf(sel, sizeof sel, ".%s", s); JS_FreeCString(ctx, s);
    return dom_select_all(ctx, NULL, sel, strlen(sel));   /* getElementsByClassName -> .cls */
}
static JSValue js_el_rect(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) {   /* getBoundingClientRect stub */
    JSValue o = JS_NewObject(ctx);
    const char *k[] = { "top", "left", "right", "bottom", "width", "height", "x", "y" };
    for (int i = 0; i < 8; i++) JS_SetPropertyStr(ctx, o, k[i], JS_NewInt32(ctx, 0));
    return o;
}
/* Event / CustomEvent constructors: `new CustomEvent('x',{detail})` threw when missing. An object carrying
   type + detail (attacker-influenceable in a dispatched event -> detail opaque unless the init provided it). */
static JSValue js_event_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv) {
    JSValue o = JS_NewObject(ctx);
    if (argc >= 1) JS_SetPropertyStr(ctx, o, "type", JS_DupValue(ctx, argv[0]));
    if (argc >= 2 && JS_IsObject(argv[1])) { JSValue d = JS_GetPropertyStr(ctx, argv[1], "detail"); JS_SetPropertyStr(ctx, o, "detail", JS_IsUndefined(d) ? JS_DupValue(ctx, g_opaque) : d); }
    else JS_SetPropertyStr(ctx, o, "detail", JS_DupValue(ctx, g_opaque));
    JS_SetPropertyStr(ctx, o, "preventDefault", JS_NewCFunction(ctx, js_noop, "preventDefault", 0));
    JS_SetPropertyStr(ctx, o, "stopPropagation", JS_NewCFunction(ctx, js_noop, "stopPropagation", 0));
    JS_SetPropertyStr(ctx, o, "target", JS_DupValue(ctx, g_opaque));
    return o;
}
/* DOMParser.parseFromString / Range.createContextualFragment: parse attacker HTML into a subtree that carries
   {parsedhtml} TAINT, so a later appendChild of it into the LIVE DOM is the @S sink (js_el_appendChild). The
   example carries the input (the concrete candidate on a replay flow) so solve_broke_html verifies breakout.
   Non-throwing constructors (these were undefined -> `new DOMParser()` threw, losing all coverage after). */
static JSValue js_parse_html_tainted(JSContext *ctx, JSValueConst input) {
    JSValue r = JS_NewOpaqueSourced(ctx, "{parsedhtml}", "parsedhtml");
    if (JS_IsOpaque(r)) {
        JSValue ex = JS_UNDEFINED;
        if (JS_IsOpaque(input)) ex = JS_OpaqueExample(ctx, input);                    /* concolic input keeps its example */
        else if (g_candidate && JS_IsString(input)) ex = JS_DupValue(ctx, input);     /* replay: the concrete candidate */
        if (!JS_IsUndefined(ex)) JS_SetOpaqueExample(ctx, r, ex); else JS_FreeValue(ctx, ex);
    }
    return r;
}
static JSValue js_domparser_parse(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    return argc >= 1 ? js_parse_html_tainted(ctx, argv[0]) : JS_DupValue(ctx, g_opaque);
}
static JSValue js_domparser_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv) {
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "parseFromString", JS_NewCFunction(ctx, js_domparser_parse, "parseFromString", 2));
    return o;
}
static JSValue js_range_ccf(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    return argc >= 1 ? js_parse_html_tainted(ctx, argv[0]) : JS_DupValue(ctx, g_opaque);
}
static JSValue js_doc_createrange(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "createContextualFragment", JS_NewCFunction(ctx, js_range_ccf, "createContextualFragment", 1));
    const char *noops[] = { "selectNode", "selectNodeContents", "setStart", "setEnd", "deleteContents", "insertNode", "surroundContents", "cloneContents" };
    for (size_t i = 0; i < sizeof noops / sizeof noops[0]; i++)
        JS_SetPropertyStr(ctx, o, noops[i], JS_NewCFunction(ctx, js_noop, noops[i], 1));
    return o;
}
/* DOM ATTRIBUTE SHADOW TAINT: Lexbor stores attribute values as bytes, so an OPAQUE external-input value set
   via setAttribute would be ToString'd -> taint LOST -> a source stashed in a data-attribute and read back
   (getAttribute) in a separate flow goes undetected. Keep a shadow map (element,name)->opaque so an
   OPAQUE/exploration flow reading the attr gets the opaque back (taint preserved, the sink is detected),
   while a CANDIDATE flow reads the REAL concrete attr so the payload flows through the real DOM. Populated at
   boot (baseline); candidate flows never write it (they set the real attr, isolated by the DOM COW delta). */
typedef struct { lxb_dom_element_t *el; char *name; JSValue opaque; } AttrShadow;
static AttrShadow *g_attr_shadow = NULL; static int g_attr_shadow_n = 0, g_attr_shadow_cap = 0;
static int attr_shadow_find(lxb_dom_element_t *el, const char *name) {
    for (int i = 0; i < g_attr_shadow_n; i++) if (g_attr_shadow[i].el == el && strcmp(g_attr_shadow[i].name, name) == 0) return i;
    return -1;
}
static void attr_shadow_set(JSContext *ctx, lxb_dom_element_t *el, const char *name, JSValueConst opaque) {
    int i = attr_shadow_find(el, name);
    if (JS_IsUndefined(opaque)) {   /* concrete overwrite -> clear any stale taint */
        if (i >= 0) { JS_FreeValue(ctx, g_attr_shadow[i].opaque); free(g_attr_shadow[i].name); g_attr_shadow[i] = g_attr_shadow[--g_attr_shadow_n]; }
        return;
    }
    if (i >= 0) { JS_FreeValue(ctx, g_attr_shadow[i].opaque); g_attr_shadow[i].opaque = JS_DupValue(ctx, opaque); return; }
    if (g_attr_shadow_n >= g_attr_shadow_cap) { int nc = g_attr_shadow_cap ? g_attr_shadow_cap * 2 : 16;
        AttrShadow *n = realloc(g_attr_shadow, (size_t)nc * sizeof(AttrShadow)); if (!n) return; g_attr_shadow = n; g_attr_shadow_cap = nc; }
    g_attr_shadow[g_attr_shadow_n].el = el; g_attr_shadow[g_attr_shadow_n].name = strdup(name);
    g_attr_shadow[g_attr_shadow_n].opaque = JS_DupValue(ctx, opaque); g_attr_shadow_n++;
}
static JSValue js_el_getAttribute(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    lxb_dom_element_t *el = JS_GetOpaque(this_val, g_el_class_id);
    if (!el || argc < 1) return JS_NULL;
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_NULL;
    if (!g_candidate) {   /* baseline/opaque flow: preserve taint stashed in this attr */
        int i = attr_shadow_find(el, name);
        if (i >= 0) { JS_FreeCString(ctx, name); return JS_DupValue(ctx, g_attr_shadow[i].opaque); }
    }
    size_t vlen = 0;
    const lxb_char_t *v = lxb_dom_element_get_attribute(el, (const lxb_char_t *)name, strlen(name), &vlen);
    JS_FreeCString(ctx, name);
    return v ? JS_NewStringLen(ctx, (const char *)v, vlen) : JS_NULL;   /* REAL attribute value (concrete, incl candidate) */
}
static JSValue js_el_querySelector(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc < 1) return JS_NULL;
    const char *s = JS_ToCString(ctx, argv[0]); if (!s) return JS_NULL;
    lxb_dom_element_t *self_el = JS_GetOpaque(this_val, g_el_class_id);   /* SUBTREE-scope to the receiver element */
    lxb_dom_element_t *el = dom_select_first(self_el ? lxb_dom_interface_node(self_el) : NULL, s, strlen(s));
    JS_FreeCString(ctx, s);
    return el_wrap(ctx, el);
}
static JSValue js_el_textContent(JSContext *ctx, JSValueConst this_val) {   /* .textContent / .innerText getter */
    lxb_dom_element_t *el = JS_GetOpaque(this_val, g_el_class_id);
    if (!el) return JS_NULL;
    size_t len = 0;
    lxb_char_t *txt = lxb_dom_node_text_content(lxb_dom_interface_node(el), &len);
    if (!txt) return JS_NewString(ctx, "");
    JSValue r = JS_NewStringLen(ctx, (const char *)txt, len);
    lxb_dom_document_destroy_text(lxb_dom_interface_node(el)->owner_document, txt);
    /* SSR DATA: a <script type=...json...> blob is server-injected app data (the TRUST boundary, like a
       reply) — declared DATA, not executable code (real <script> code already ran as boot). Return it
       CONCOLIC so JSON.parse of it FORKS gates on loaded values (`if(data.user.role==='admin')` -> the
       logged-in/admin surface) while fields keep real examples. Matched by the server-DECLARED MIME type
       (structural), never a bundle name. */
    size_t nl = 0; const lxb_char_t *nm = lxb_dom_element_qualified_name(el, &nl);
    if (nm && nl == 6 && !memcmp(nm, "script", 6)) {
        size_t tl = 0; const lxb_char_t *ty = lxb_dom_element_get_attribute(el, (const lxb_char_t *)"type", 4, &tl);
        int is_json = 0;
        for (size_t i = 0; ty && i + 4 <= tl; i++) if (!memcmp(ty + i, "json", 4)) { is_json = 1; break; }
        if (is_json) {
            JSValue o = JS_NewOpaqueSourced(ctx, "{ssr}", "ssr");
            if (JS_IsOpaque(o)) { JS_SetOpaqueExample(ctx, o, r); return o; }   /* consumes r */
            JS_FreeValue(ctx, o);
        }
    }
    return r;
}
/* ── Per-flow DOM COW isolation ──────────────────────────────────────────────────
   DOM mutations (attribute set, node insert) by a FORCED flow must not leak to sibling flows — the
   same invariant as the JS-heap COW. A host-side undo log records the inverse of each mutation while
   capture is active (during flow exploration, NOT boot which builds the baseline); dom_revert replays
   it to restore the post-boot DOM baseline, called alongside JS_CowRevert before each starter. */
/* PER-FLOW DOM COW DELTA (mirrors the JS heap delta): `old` = baseline value/absence (kind 0 attr) or the
   inserted node's detach position (kind 1); `cur` = the flow's value, held while parked. Swapped on
   context-switch so a DOM writer interleaves like a heap writer. */
typedef struct DomUndo {
    int kind; lxb_dom_element_t *el; char *name;
    lxb_char_t *old; size_t old_len; int had;                 /* kind 0: baseline attr */
    lxb_char_t *cur; size_t cur_len; int cur_had;             /* kind 0: flow's attr (valid while parked) */
    lxb_dom_node_t *node, *parent, *next;                     /* kind 1: inserted node + detach position */
    int detached;                                             /* kind 1: currently detached (unapplied) */
} DomUndo;
static DomUndo *g_dom_undo = NULL;
static int g_dom_undo_n = 0, g_dom_undo_cap = 0, g_dom_capture = 0;
static void dom_undo_push(DomUndo u) {
    if (g_dom_undo_n >= g_dom_undo_cap) {
        int nc = g_dom_undo_cap ? g_dom_undo_cap * 2 : 64;
        /* Skipping a DOM capture silently breaks DOM isolation (this flow's DOM write never reverts -> leaks
           into the next flow's baseline -> wrong sinks/taint). OOM is a should-never-happen: CRASH, never skip. */
        DomUndo *n = realloc(g_dom_undo, (size_t)nc * sizeof(DomUndo));
        if (!n) { fflush(stdout); fprintf(stderr, "@E {\"phase\":\"dom-cow-oom\",\"reason\":\"DOM undo-log realloc failed — DOM isolation would be silently corrupted\"}\n"); fflush(stderr); abort(); }
        g_dom_undo = n; g_dom_undo_cap = nc;
    }
    g_dom_undo[g_dom_undo_n++] = u;
}
static void dom_attr_capture(lxb_dom_element_t *el, const char *name) {
    if (!g_dom_capture) return;
    size_t vl = 0; const lxb_char_t *cur = lxb_dom_element_get_attribute(el, (const lxb_char_t *)name, strlen(name), &vl);
    DomUndo u; memset(&u, 0, sizeof u);
    u.kind = 0; u.el = el; u.name = strdup(name); u.had = cur ? 1 : 0;
    if (cur) { u.old = malloc(vl ? vl : 1); if (u.old) { memcpy(u.old, cur, vl); u.old_len = vl; } }
    dom_undo_push(u);
}
static void dom_insert_capture(lxb_dom_node_t *node) {
    if (!g_dom_capture) return;
    DomUndo u; memset(&u, 0, sizeof u); u.kind = 1; u.node = node; dom_undo_push(u);
}
/* ── The SECURITY view (@S) ──────────────────────────────────────────────────────
   REMOVED the taint tracker (opaque-reaches-sink via host_opaque/emit_sink). Taint tracing is a separate
   mechanism bolted alongside forced execution; forced execution SUBSUMES it — the security view is being
   rebuilt as forced-exec with concrete marker payloads that flow through the REAL code (transforms/filters
   apply natively) and are observed breaking out at a sink. Until that lands, sinks are ordinary host edges
   (no-op for the injected content) and emit no @S. */
static void dom_revert(void) {   /* DISCARD the running flow's DOM writes -> baseline (reverse order); empties the delta */
    for (int i = g_dom_undo_n - 1; i >= 0; i--) {
        DomUndo *u = &g_dom_undo[i];
        if (u->kind == 0) {   /* attribute: restore old value, or remove if it didn't exist */
            if (u->had && u->old) lxb_dom_element_set_attribute(u->el, (const lxb_char_t *)u->name, strlen(u->name), u->old, u->old_len);
            else lxb_dom_element_remove_attribute(u->el, (const lxb_char_t *)u->name, strlen(u->name));
            free(u->name); free(u->old); free(u->cur);
        } else if (u->kind == 1 && !u->detached) {   /* inserted node: detach it (baseline had none) */
            lxb_dom_node_remove(u->node);
        }
    }
    g_dom_undo_n = 0;
}
/* UNAPPLY (flow -> parked): save the flow's DOM values, restore the baseline, so the next flow sees the
   baseline DOM. Reverse order. */
static void dom_unapply(void) {
    for (int i = g_dom_undo_n - 1; i >= 0; i--) {
        DomUndo *u = &g_dom_undo[i];
        if (u->kind == 0) {
            size_t vl = 0; const lxb_char_t *c = lxb_dom_element_get_attribute(u->el, (const lxb_char_t *)u->name, strlen(u->name), &vl);
            free(u->cur); u->cur = NULL; u->cur_len = 0; u->cur_had = c ? 1 : 0;   /* stash the flow's attr */
            if (c) { u->cur = malloc(vl ? vl : 1); if (u->cur) { memcpy(u->cur, c, vl); u->cur_len = vl; } }
            if (u->had && u->old) lxb_dom_element_set_attribute(u->el, (const lxb_char_t *)u->name, strlen(u->name), u->old, u->old_len);
            else lxb_dom_element_remove_attribute(u->el, (const lxb_char_t *)u->name, strlen(u->name));
        } else if (u->kind == 1 && !u->detached) {
            u->parent = lxb_dom_interface_node(u->node)->parent;              /* remember re-insert position */
            u->next = lxb_dom_interface_node(u->node)->next;
            lxb_dom_node_remove(u->node); u->detached = 1;
        }
    }
}
/* APPLY (parked -> flow): restore the flow's DOM values over the baseline. Forward order. */
static void dom_apply(void) {
    for (int i = 0; i < g_dom_undo_n; i++) {
        DomUndo *u = &g_dom_undo[i];
        if (u->kind == 0) {
            if (u->cur_had && u->cur) lxb_dom_element_set_attribute(u->el, (const lxb_char_t *)u->name, strlen(u->name), u->cur, u->cur_len);
            else lxb_dom_element_remove_attribute(u->el, (const lxb_char_t *)u->name, strlen(u->name));
        } else if (u->kind == 1 && u->detached) {
            if (u->next) lxb_dom_node_insert_before(u->next, u->node);
            else if (u->parent) lxb_dom_node_insert_child(u->parent, u->node);
            u->detached = 0;
        }
    }
}
static void dom_buf_free(DomUndo *buf, int n) {   /* free a parked DOM delta buffer (its nodes stay detached, owned by the doc) */
    for (int i = 0; i < n; i++) { free(buf[i].name); free(buf[i].old); free(buf[i].cur); }
    free(buf);
}
static void *dom_buf_take(int *n, int *cap) { void *b = g_dom_undo; *n = g_dom_undo_n; *cap = g_dom_undo_cap; g_dom_undo = NULL; g_dom_undo_n = 0; g_dom_undo_cap = 0; return b; }
static void dom_buf_load(void *buf, int n, int cap) { g_dom_undo = (DomUndo *)buf; g_dom_undo_n = n; g_dom_undo_cap = cap; }

static int el_is_script(lxb_dom_element_t *el) {
    size_t nl = 0; const lxb_char_t *nm = lxb_dom_element_qualified_name(el, &nl);
    return nm && nl == 6 && memcmp(nm, "script", 6) == 0;
}
/* An inserted <script> with a src is a chunk LOAD (the URL may be JS-computed): surface it. */
static void script_maybe_load(lxb_dom_element_t *el) {
    if (!el_is_script(el)) return;
    /* A candidate-REPLAY flow (g_candidate set) VERIFIES a known sink; it must not DISCOVER chunks. Here
       `s.src` is derived from the injected candidate PAYLOAD (e.g. "javascript:X9"), NOT a real chunk URL —
       chunk_pending_add'ing it is nonsensical and drives a fetch/provide feedback that livelocks a
       multi-sink handler. Chunk discovery is a NORMAL-flow concern; skip it under a candidate. */
    if (g_candidate) return;
    size_t sl = 0; const lxb_char_t *src = lxb_dom_element_get_attribute(el, (const lxb_char_t *)"src", 3, &sl);
    if (src && sl) {
        char *u = strndup((const char *)src, sl);
        if (u) { arr_push_str(g_ctx, g_chunkurls, u); if (!has_hole(u)) chunk_pending_add(u); free(u); }   /* -> chunkUrls + fetch in place */
    }
}
static void resolve_with(JSContext *ctx, JSValueConst resolve, JSValue val) {   /* resolve borrowed; val consumed */
    JSValue r = JS_Call(ctx, resolve, JS_UNDEFINED, 1, (JSValueConst *)&val); JS_FreeValue(ctx, r); JS_FreeValue(ctx, val);
}
/* Link a dynamic-import chunk from its FETCHED source and hand back its REAL namespace (concrete exports).
   Returns 0 if not fetched yet, or if a static dep in the chunk isn't ready (retried on the next provide). */
static int dynimport_link(JSContext *ctx, const char *spec, JSValue *out_ns) {
    ModSrc *m = modsrc_get(spec);
    if (!m || !m->src) return 0;
    JSValue fn = JS_Eval(ctx, m->src, m->len, spec, JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(fn)) { JS_FreeValue(ctx, JS_GetException(ctx)); return 0; }
    JSModuleDef *md = JS_VALUE_GET_PTR(fn);
    JSValue ev = JS_EvalFunction(ctx, fn);   /* instantiate (loader resolves deps) + evaluate */
    if (JS_IsException(ev)) { JS_FreeValue(ctx, JS_GetException(ctx)); JS_FreeValue(ctx, ev); return 0; }
    { JSContext *c; while (JS_ExecutePendingJob(g_rt, &c) > 0) {} }
    JS_FreeValue(ctx, ev);
    *out_ns = JS_GetModuleNamespace(ctx, md);   /* the concrete exports */
    return 1;
}
/* import() promises parked until their chunk (and its deps) are fetched. */
typedef struct { char *spec; JSValue resolve; } DynPend;
static DynPend *g_dynpend = NULL; static int g_dynpend_n = 0, g_dynpend_cap = 0;
static void dynpend_add(const char *spec, JSValue resolve) {   /* resolve is a DUP we own */
    if (g_dynpend_n >= g_dynpend_cap) { int nc = g_dynpend_cap ? g_dynpend_cap * 2 : 8; DynPend *n = realloc(g_dynpend, (size_t)nc * sizeof(DynPend)); if (!n) { JS_FreeValue(g_ctx, resolve); return; } g_dynpend = n; g_dynpend_cap = nc; }
    g_dynpend[g_dynpend_n].spec = strdup(spec); g_dynpend[g_dynpend_n].resolve = resolve; g_dynpend_n++;
}
static void dynpend_retry(JSContext *ctx) {   /* a chunk arrived: resolve every now-linkable parked import() */
    for (int i = 0; i < g_dynpend_n; i++) {
        JSValue ns;
        if (dynimport_link(ctx, g_dynpend[i].spec, &ns)) {
            resolve_with(ctx, g_dynpend[i].resolve, ns); JS_FreeValue(ctx, g_dynpend[i].resolve); free(g_dynpend[i].spec);
            for (int j = i; j < g_dynpend_n - 1; j++) g_dynpend[j] = g_dynpend[j + 1];
            g_dynpend_n--; i--;
        }
    }
}
/* dynamic import(specifier): force-fetch + LINK the ESM chunk like a browser lazy-load (forced). The chunk's
   exports are the page's OWN code (concrete, not external input), so resolve the import() promise with the
   REAL namespace once linked — a value that flows to a fetch/sink is then solved, not lost to an opaque {}. */
static void host_dyn_import(JSContext *ctx, const char *specifier, JSValueConst resolve, JSValueConst reject) {
    (void)reject;
    if (!specifier || !specifier[0] || has_hole(specifier)) { resolve_with(ctx, resolve, JS_DupValue(ctx, g_opaque)); return; }
    arr_push_str(ctx, g_chunkurls, specifier);   /* -> @RESULT.chunkUrls */
    JSValue ns;
    if (dynimport_link(ctx, specifier, &ns)) { resolve_with(ctx, resolve, ns); return; }   /* already fetched -> real namespace now */
    chunk_pending_add(specifier); moddep_add(specifier);   /* fetch it; PARK the resolve until it (and its deps) land */
    dynpend_add(specifier, JS_DupValue(ctx, resolve));
}
/* Identity normalize: keep the specifier VERBATIM (matches the host fetch contract — chunk URLs are passed
   to qjs_provide exactly as written; the offscreen resolves relative/root-relative against the doc URL). */
static char *host_module_normalize(JSContext *ctx, const char *base, const char *name, void *opaque) {
    (void)base; (void)opaque; return js_strdup(ctx, name);
}
/* Resolve a static import: compile the dep from its FETCHED source; if not fetched yet, request it like a
   browser and fail this link (the importer is retried when the chunk arrives). quickjs dedups by name, so a
   given dep URL is compiled once and shared across the graph. */
static JSModuleDef *host_module_loader(JSContext *ctx, const char *name, void *opaque) {
    (void)opaque;
    ModSrc *m = modsrc_get(name);
    if (m && m->src) {
        JSValue v = JS_Eval(ctx, m->src, m->len, name, JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
        if (JS_IsException(v)) { JS_FreeValue(ctx, JS_GetException(ctx)); return NULL; }
        JSModuleDef *md = JS_VALUE_GET_PTR(v); JS_FreeValue(ctx, v); return md;   /* module kept alive by rt module_list */
    }
    moddep_add(name);
    if (!has_hole(name)) { chunk_pending_add(name); arr_push_str(ctx, g_chunkurls, name); }   /* fetch the graph */
    JS_ThrowReferenceError(ctx, "module not yet fetched: %s", name);
    return NULL;
}
/* Re-attempt every deferred module link (a chunk just arrived). A fresh unique name per try avoids colliding
   with a prior failed link; deps are shared by URL, so this converges in graph-depth arrivals. */
static void pendmod_retry(JSContext *ctx) {
    int progressed = 1;
    while (progressed) {
        progressed = 0;
        for (int i = 0; i < g_pendmod_n; i++) {
            char nm[32]; snprintf(nm, sizeof nm, "<mod-%d>", g_modseq++);
            JSValue v = JS_Eval(ctx, g_pendmod[i].src, g_pendmod[i].len, nm, JS_EVAL_TYPE_MODULE);
            if (JS_IsException(v)) { JS_FreeValue(ctx, JS_GetException(ctx)); JS_FreeValue(ctx, v); continue; }  /* still missing a dep */
            { JSContext *c; while (JS_ExecutePendingJob(g_rt, &c) > 0) {} }
            JS_FreeValue(ctx, v);
            free(g_pendmod[i].src);
            for (int j = i; j < g_pendmod_n - 1; j++) g_pendmod[j] = g_pendmod[j + 1];
            g_pendmod_n--; i--; progressed = 1;
        }
    }
}
static JSValue js_el_setAttribute(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    lxb_dom_element_t *el = JS_GetOpaque(this_val, g_el_class_id);
    if (!el || argc < 2) return JS_UNDEFINED;
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_UNDEFINED;
    int is_opq = JS_IsOpaque(argv[1]);
    /* baseline/opaque flow storing OPAQUE external input -> record the taint in the shadow (concrete value ->
       clear any stale taint). A candidate flow (concrete source) writes only the real attr, not the shadow. */
    if (!g_candidate) attr_shadow_set(ctx, el, name, is_opq ? argv[1] : JS_UNDEFINED);
    /* @S: a tainted value written into an EXECUTABLE attribute. on* handler => the value IS js code (js
       context); href/src/action => a javascript: URL (url context). Called unconditionally like the other
       sinks so solve_add both DETECTS (opaque flow) and VERIFIES the breakout (candidate flow). */
    if (name[0] == 'o' && name[1] == 'n') solve_add(ctx, "setAttribute", "js", argv[1]);
    else if (!strcmp(name, "href") || !strcmp(name, "src") || !strcmp(name, "action") || !strcmp(name, "formaction"))
        solve_add(ctx, "setAttribute", "url", argv[1]);
    const char *val = is_opq ? JS_OpaqueShapeC(argv[1]) : JS_ToCString(ctx, argv[1]);   /* opaque -> its @H shape (display); else the concrete string */
    if (val) { dom_attr_capture(el, name); lxb_dom_element_set_attribute(el, (const lxb_char_t *)name, strlen(name), (const lxb_char_t *)val, strlen(val)); }
    if (val && !is_opq) JS_FreeCString(ctx, val);   /* JS_OpaqueShapeC returns an internal pointer — don't free */
    JS_FreeCString(ctx, name);
    return JS_UNDEFINED;
}
/* el.insertAdjacentHTML(pos, html): DOM edge (no-op for content; the security view is forced-exec, not
   a taint check on the argument). */
static JSValue js_el_insertAdjacentHTML(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc >= 2) solve_add(ctx, "insertAdjacentHTML", "html", argv[1]);   /* @S */
    return JS_UNDEFINED;
}
static JSValue js_el_set_html(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic) {
    solve_add(ctx, magic ? "outerHTML" : "innerHTML", "html", val);         /* @S */
    return JS_UNDEFINED;
}
static JSValue js_el_get_html(JSContext *ctx, JSValueConst this_val, int magic) { return JS_DupValue(ctx, g_opaque); }
/* @H FORM SUBMISSION — a real browser, on form.submit()/requestSubmit() (or a non-prevented submit), fires a
   REQUEST to the form's action with its named controls' values. We ARE that browser (lexbor DOM + quickjs), so
   .submit() must EMIT that endpoint, never no-op. Learned by EXECUTION (the .submit() call) — never a static
   <form> harvest. A field value is the CONCOLIC shadow example/shape when JS set it to an opaque (attacker/
   computed), else the reflected `value` attribute (JS-set values are captured there). */
static void form_enc(const char *s, char *out, size_t cap) {   /* application/x-www-form-urlencoded value */
    static const char *hex = "0123456789ABCDEF"; size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p && o + 4 < cap; p++) {
        unsigned char c = *p;
        if ((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'||c=='~') out[o++]=(char)c;
        else if (c==' ') out[o++]='+';
        else { out[o++]='%'; out[o++]=hex[c>>4]; out[o++]=hex[c&15]; }
    }
    out[o] = 0;
}
static char *form_field_value(JSContext *ctx, lxb_dom_element_t *el) {   /* malloc'd; caller frees */
    int si = attr_shadow_find(el, "value");
    if (si >= 0 && JS_IsOpaque(g_attr_shadow[si].opaque)) {
        JSValue ex = JS_OpaqueExample(ctx, g_attr_shadow[si].opaque); char *r = NULL;
        if (!JS_IsUndefined(ex)) { const char *s = JS_ToCString(ctx, ex); if (s) { r = strdup(s); JS_FreeCString(ctx, s); } }
        JS_FreeValue(ctx, ex);
        if (!r) { const char *sh = JS_OpaqueShapeC(g_attr_shadow[si].opaque); r = strdup(sh ? sh : "{opaque}"); }
        return r;
    }
    size_t vl = 0; const lxb_char_t *v = lxb_dom_element_get_attribute(el, (const lxb_char_t *)"value", 5, &vl);
    if (v) { char *r = malloc(vl + 1); if (r) { memcpy(r, v, vl); r[vl] = 0; } return r; }
    return strdup("");
}
static void form_collect(JSContext *ctx, lxb_dom_node_t *node, JSValue params, uint32_t *idx,
                         char *body, size_t bcap, const char *loc) {
    for (lxb_dom_node_t *n = node->first_child; n; n = n->next) {
        if (n->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            lxb_dom_element_t *el = lxb_dom_interface_element(n);
            size_t nl = 0; const lxb_char_t *nm = lxb_dom_element_get_attribute(el, (const lxb_char_t *)"name", 4, &nl);
            if (nm && nl) {
                char nbuf[256]; size_t nn = nl < 255 ? nl : 255; memcpy(nbuf, nm, nn); nbuf[nn] = 0;
                char *val = form_field_value(ctx, el);
                JSValue po = JS_NewObject(ctx);
                JS_SetPropertyStr(ctx, po, "name", JS_NewString(ctx, nbuf));
                JS_SetPropertyStr(ctx, po, "location", JS_NewString(ctx, loc));
                JSValue vv = JS_NewArray(ctx);
                if (val && val[0]) JS_SetPropertyUint32(ctx, vv, 0, JS_NewString(ctx, val));
                JS_SetPropertyStr(ctx, po, "validValues", vv);
                JS_SetPropertyUint32(ctx, params, (*idx)++, po);
                char enc[512]; form_enc(val, enc, sizeof enc);
                size_t bl = strlen(body);
                snprintf(body + bl, bcap - bl, "%s%s=%s", bl ? "&" : "", nbuf, enc);
                free(val);
            }
        }
        form_collect(ctx, n, params, idx, body, bcap, loc);   /* nested controls (fields inside divs/fieldsets) */
    }
}
static JSValue js_form_submit(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    lxb_dom_element_t *form = JS_GetOpaque(this_val, g_el_class_id);
    if (!form) return JS_UNDEFINED;   /* not a real element (defensive) — nothing to submit */
    size_t ml = 0; const lxb_char_t *mv = lxb_dom_element_get_attribute(form, (const lxb_char_t *)"method", 6, &ml);
    char method[8] = "GET";
    if (mv && ml) { size_t mn = ml < 7 ? ml : 7; for (size_t i = 0; i < mn; i++) { char c = (char)mv[i]; method[i] = (c>='a'&&c<='z')?(char)(c-32):c; } method[mn] = 0; }
    int is_body = (strcmp(method,"POST")==0 || strcmp(method,"PUT")==0 || strcmp(method,"DELETE")==0 || strcmp(method,"PATCH")==0);
    size_t al = 0; const lxb_char_t *av = lxb_dom_element_get_attribute(form, (const lxb_char_t *)"action", 6, &al);
    char *action = NULL;
    if (av && al) { action = malloc(al + 1); if (action) { memcpy(action, av, al); action[al] = 0; } }
    char *url = url_resolve(action ? action : "", g_origin);
    if (!url) url = strdup(g_origin ? g_origin : "");
    free(action);
    JSValue params = JS_NewArray(ctx); uint32_t pidx = 0;
    char fbody[4096]; fbody[0] = 0;
    form_collect(ctx, lxb_dom_interface_node(form), params, &pidx, fbody, sizeof fbody, is_body ? "body" : "query");
    char *final_url = url;
    if (!is_body && fbody[0]) {   /* GET: the controls ARE the query string */
        size_t need = strlen(url) + 1 + strlen(fbody) + 1;
        final_url = malloc(need); if (final_url) snprintf(final_url, need, "%s?%s", url, fbody); else final_url = url;
    }
    JSValue ep = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, ep, "method", JS_NewString(ctx, method));
    JS_SetPropertyStr(ctx, ep, "url", JS_NewString(ctx, final_url ? final_url : ""));
    JS_SetPropertyStr(ctx, ep, "source", JS_NewString(ctx, "ast_analysis"));
    JS_SetPropertyStr(ctx, ep, "params", params);
    if (is_body && fbody[0]) JS_SetPropertyStr(ctx, ep, "body", JS_NewString(ctx, fbody));
    record_endpoint(ctx, ep);   /* emits @H unless a candidate flow (the sink respects g_candidate) */
    if (final_url != url) free(final_url);
    free(url);
    return JS_UNDEFINED;
}
/* el.getAttributeNames(): the element's attribute names, in order — a real DOM API frameworks use to reflect
   attrs. Missing, it threw and killed the page. */
static JSValue js_el_getattrnames(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    JSValue arr = JS_NewArray(ctx);
    lxb_dom_element_t *el = JS_GetOpaque(this_val, g_el_class_id);
    if (!el) return arr;
    uint32_t i = 0;
    for (lxb_dom_attr_t *a = lxb_dom_element_first_attribute(el); a; a = lxb_dom_element_next_attribute(a)) {
        size_t nl = 0; const lxb_char_t *nm = lxb_dom_attr_qualified_name(a, &nl);
        if (nm) JS_SetPropertyUint32(ctx, arr, i++, JS_NewStringLen(ctx, (const char *)nm, nl));
    }
    return arr;
}
static JSValue js_el_appendChild(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    lxb_dom_element_t *parent = JS_GetOpaque(this_val, g_el_class_id);
    /* @S: inserting a {parsedhtml}-TAINTED node (from DOMParser.parseFromString / Range.createContextual-
       Fragment of attacker input) into the LIVE DOM is XSS. The distinct source avoids a false positive on a
       safe text node (createTextNode is a plain opaque, not {parsedhtml}); replay verifies real breakout. */
    if (argc > 0 && JS_IsOpaque(argv[0])) {
        const char *sh = JS_OpaqueShapeC(argv[0]);
        if (sh && strcmp(sh, "{parsedhtml}") == 0) solve_add(ctx, "appendChild", "html", argv[0]);
    }
    lxb_dom_element_t *child = (argc > 0) ? JS_GetOpaque(argv[0], g_el_class_id) : NULL;
    if (parent && child) {
        lxb_dom_node_insert_child(lxb_dom_interface_node(parent), lxb_dom_interface_node(child));
        dom_insert_capture(lxb_dom_interface_node(child));
        script_maybe_load(child);   /* injected <script src> (computed URL) -> discovered as a chunk */
    }
    return (argc > 0) ? JS_DupValue(ctx, argv[0]) : JS_UNDEFINED;
}
/* reflected URL/identity properties (el.src = computedUrl / el.href / el.id) map to Lexbor attributes,
   so a bundle setting .src via PROPERTY (the common script-injection form) is captured + intercepted. */
static const char *refl_name(int magic) {   /* property magic -> the ATTRIBUTE it reflects (className -> class) */
    switch (magic) {
        case 0: return "src"; case 1: return "href"; case 2: return "action"; case 3: return "id";
        case 4: return "value"; case 5: return "name"; case 6: return "type"; case 7: return "class";
        case 8: return "alt"; case 9: return "title"; case 10: return "placeholder"; case 11: return "srcdoc";
        default: return "";
    }
}
/* BOOLEAN reflected props (checked/disabled/hidden/...): the PROPERTY is true iff the attribute is present.
   undefined was falsy (no throw) but wrong — `el.checked===true` / faithful form state needs the real bool. */
static const char *bool_name(int magic) {
    switch (magic) { case 0: return "checked"; case 1: return "disabled"; case 2: return "hidden";
        case 3: return "selected"; case 4: return "required"; case 5: return "readonly"; case 6: return "multiple"; default: return ""; }
}
static JSValue js_el_bool_get(JSContext *ctx, JSValueConst this_val, int magic) {
    lxb_dom_element_t *el = JS_GetOpaque(this_val, g_el_class_id); if (!el) return JS_FALSE;
    const char *n = bool_name(magic);
    return lxb_dom_element_has_attribute(el, (const lxb_char_t *)n, strlen(n)) ? JS_TRUE : JS_FALSE;
}
/* tagName / nodeName: the element's tag, UPPERCASE for HTML (spec) — Lexbor lowercases. Real bundles branch
   on el.tagName constantly (`if(el.tagName==='A')`); undefined broke that. */
static JSValue js_el_tagname(JSContext *ctx, JSValueConst this_val) {
    lxb_dom_element_t *el = JS_GetOpaque(this_val, g_el_class_id); if (!el) return JS_UNDEFINED;
    size_t nl = 0; const lxb_char_t *nm = lxb_dom_element_qualified_name(el, &nl);
    if (!nm) return JS_NewString(ctx, "");
    char buf[64]; size_t n = nl < 63 ? nl : 63;
    for (size_t i = 0; i < n; i++) { char c = (char)nm[i]; buf[i] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }
    buf[n] = 0;
    return JS_NewString(ctx, buf);
}
static JSValue js_el_refl_get(JSContext *ctx, JSValueConst this_val, int magic) {
    lxb_dom_element_t *el = JS_GetOpaque(this_val, g_el_class_id); if (!el) return JS_UNDEFINED;
    const char *n = refl_name(magic); size_t vl = 0;
    const lxb_char_t *v = lxb_dom_element_get_attribute(el, (const lxb_char_t *)n, strlen(n), &vl);
    return v ? JS_NewStringLen(ctx, (const char *)v, vl) : JS_NewString(ctx, "");
}
static JSValue js_el_refl_set(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic) {
    lxb_dom_element_t *el = JS_GetOpaque(this_val, g_el_class_id); if (!el) return JS_UNDEFINED;
    const char *n = refl_name(magic);
    if (magic == 1 || magic == 2) solve_add(ctx, "href", "url", val);   /* @S: el.href/.action = external -> javascript:/redirect */
    else if (magic == 11) solve_add(ctx, "srcdoc", "html", val);        /* @S: iframe.srcdoc renders attacker HTML in the frame */
    const char *v = JS_ToCString(ctx, val);
    if (v) { dom_attr_capture(el, n); lxb_dom_element_set_attribute(el, (const lxb_char_t *)n, strlen(n), (const lxb_char_t *)v, strlen(v)); JS_FreeCString(ctx, v); }
    return JS_UNDEFINED;
}
/* Common element APIs that real bundles call constantly — MISSING ones throw and kill the script (like
   addEventListener did), losing all coverage after. matches -> opaque bool (a branch FORKS, exploring both);
   closest -> the element itself (a real node whose methods/attrs work — never throw/null); style -> a plain
   object so `el.style.x=v` never throws; dataset -> the element's REAL data-* attributes (an endpoint often
   lives in data-url), camelCased, so `el.dataset.url` yields the concrete value, not undefined. */
static JSValue js_el_matches(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    lxb_dom_element_t *el = JS_GetOpaque(this_val, g_el_class_id);
    if (!el || argc < 1) return JS_FALSE;
    const char *s = JS_ToCString(ctx, argv[0]); if (!s) return JS_FALSE;
    int m = dom_node_matches(el, s, strlen(s)); JS_FreeCString(ctx, s);
    return m ? JS_TRUE : JS_FALSE;
}
static JSValue js_el_closest(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    lxb_dom_element_t *el = JS_GetOpaque(this_val, g_el_class_id);
    if (!el || argc < 1) return JS_NULL;
    const char *s = JS_ToCString(ctx, argv[0]); size_t sl = s ? strlen(s) : 0;
    JSValue r = JS_NULL;
    for (lxb_dom_node_t *n = lxb_dom_interface_node(el); n && n->type == LXB_DOM_NODE_TYPE_ELEMENT; n = lxb_dom_node_parent(n))
        if (s && dom_node_matches(lxb_dom_interface_element(n), s, sl)) { r = el_wrap(ctx, lxb_dom_interface_element(n)); break; }
    if (s) JS_FreeCString(ctx, s);
    return r;
}
static JSValue js_el_has_attr(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    lxb_dom_element_t *el = JS_GetOpaque(this_val, g_el_class_id);
    if (!el || argc < 1) return JS_FALSE;
    const char *n = JS_ToCString(ctx, argv[0]); if (!n) return JS_FALSE;
    bool h = lxb_dom_element_has_attribute(el, (const lxb_char_t *)n, strlen(n)); JS_FreeCString(ctx, n);
    return h ? JS_TRUE : JS_FALSE;
}
static JSValue js_el_contains(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    lxb_dom_element_t *self = JS_GetOpaque(this_val, g_el_class_id);
    lxb_dom_element_t *other = (argc >= 1) ? JS_GetOpaque(argv[0], g_el_class_id) : NULL;
    if (!self || !other) return JS_FALSE;
    for (lxb_dom_node_t *n = lxb_dom_interface_node(other); n; n = lxb_dom_node_parent(n))
        if (n == lxb_dom_interface_node(self)) return JS_TRUE;
    return JS_FALSE;
}
static JSValue js_el_self(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) { return JS_DupValue(ctx, this_val); }   /* cloneNode -> a real node */
static JSValue js_el_style_get(JSContext *ctx, JSValueConst this_val) { return JS_NewObject(ctx); }
static JSValue js_el_dataset_get(JSContext *ctx, JSValueConst this_val) {
    lxb_dom_element_t *el = JS_GetOpaque(this_val, g_el_class_id);
    JSValue o = JS_NewObject(ctx);
    if (!el) return o;
    for (lxb_dom_attr_t *a = lxb_dom_element_first_attribute(el); a; a = lxb_dom_element_next_attribute(a)) {
        size_t nlen; const lxb_char_t *nm = lxb_dom_attr_qualified_name(a, &nlen);
        if (!nm || nlen <= 5 || memcmp(nm, "data-", 5) != 0) continue;
        char key[128]; int ki = 0;                                   /* data-foo-bar -> fooBar */
        for (size_t i = 5; i < nlen && ki < 126; i++) {
            if (nm[i] == '-' && i + 1 < nlen) { i++; char c = (char)nm[i]; key[ki++] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }
            else key[ki++] = (char)nm[i];
        }
        key[ki] = 0;
        size_t vlen; const lxb_char_t *v = lxb_dom_attr_value(a, &vlen);
        JS_SetPropertyStr(ctx, o, key, v ? JS_NewStringLen(ctx, (const char *)v, vlen) : JS_NewString(ctx, ""));
    }
    return o;
}
/* classList: real bundles branch on `el.classList.contains('active')` constantly; undefined THREW. contains
   checks the REAL class attribute (whitespace-separated tokens) — concrete page structure, like className.
   add/remove/toggle are UI no-ops (not endpoint-relevant). */
static int class_has_token(const lxb_char_t *cls, size_t cl, const char *tok) {
    if (!cls || !tok || !tok[0]) return 0;
    size_t tk = strlen(tok), i = 0;
    while (i < cl) {
        while (i < cl && (cls[i]==' '||cls[i]=='\t'||cls[i]=='\n'||cls[i]=='\r'||cls[i]=='\f')) i++;
        size_t s = i;
        while (i < cl && !(cls[i]==' '||cls[i]=='\t'||cls[i]=='\n'||cls[i]=='\r'||cls[i]=='\f')) i++;
        if (i - s == tk && !memcmp(cls + s, tok, tk)) return 1;
    }
    return 0;
}
static JSValue js_classlist_contains(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    JSValue elw = JS_GetPropertyStr(ctx, this_val, "__el");
    lxb_dom_element_t *el = JS_GetOpaque(elw, g_el_class_id); JS_FreeValue(ctx, elw);
    if (!el || argc < 1) return JS_FALSE;
    size_t cl = 0; const lxb_char_t *cls = lxb_dom_element_get_attribute(el, (const lxb_char_t *)"class", 5, &cl);
    const char *tok = JS_ToCString(ctx, argv[0]);
    int found = class_has_token(cls, cl, tok);
    if (tok) JS_FreeCString(ctx, tok);
    return found ? JS_TRUE : JS_FALSE;
}
/* FAITHFUL classList.add/remove/toggle: mutate the REAL class attribute (COW-captured, isolated per flow) so
   `classList.add('admin'); if(classList.contains('admin')) fetch(...)` reaches the class-gated endpoint — a
   no-op mutator (the old stub) silently dropped it. magic: 0=add 1=remove 2=toggle. */
#define CL_WS(c) ((c)==' '||(c)=='\t'||(c)=='\n'||(c)=='\r'||(c)=='\f')
static JSValue js_classlist_mut(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic) {
    JSValue elw = JS_GetPropertyStr(ctx, this_val, "__el");
    lxb_dom_element_t *el = JS_GetOpaque(elw, g_el_class_id); JS_FreeValue(ctx, elw);
    if (!el || argc < 1) return magic == 2 ? JS_FALSE : JS_UNDEFINED;
    const char *tok = JS_ToCString(ctx, argv[0]);
    if (!tok || !tok[0]) { if (tok) JS_FreeCString(ctx, tok); return magic == 2 ? JS_FALSE : JS_UNDEFINED; }
    size_t cl = 0; const lxb_char_t *cls = lxb_dom_element_get_attribute(el, (const lxb_char_t *)"class", 5, &cl);
    int present = class_has_token(cls, cl, tok);
    int want_add = (magic == 0) ? 1 : (magic == 1) ? 0 : !present;
    if (want_add == present) { JS_FreeCString(ctx, tok); return magic == 2 ? (present ? JS_TRUE : JS_FALSE) : JS_UNDEFINED; }
    size_t tk = strlen(tok);
    char *nc = malloc(cl + tk + 2);
    if (!nc) { JS_FreeCString(ctx, tok); return magic == 2 ? JS_FALSE : JS_UNDEFINED; }
    size_t no = 0, i = 0;   /* copy every token except `tok`, then append it if adding */
    while (i < cl) {
        while (i < cl && CL_WS(cls[i])) i++;
        size_t s = i; while (i < cl && !CL_WS(cls[i])) i++;
        if (i == s) break;
        if (i - s == tk && !memcmp(cls + s, tok, tk)) continue;
        if (no) nc[no++] = ' ';
        memcpy(nc + no, cls + s, i - s); no += i - s;
    }
    if (want_add) { if (no) nc[no++] = ' '; memcpy(nc + no, tok, tk); no += tk; }
    dom_attr_capture(el, "class");
    lxb_dom_element_set_attribute(el, (const lxb_char_t *)"class", 5, (const lxb_char_t *)nc, no);
    free(nc); JS_FreeCString(ctx, tok);
    return magic == 2 ? (want_add ? JS_TRUE : JS_FALSE) : JS_UNDEFINED;
}
static JSValue js_el_classlist_get(JSContext *ctx, JSValueConst this_val) {
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "__el", JS_DupValue(ctx, this_val));
    JS_SetPropertyStr(ctx, o, "contains", JS_NewCFunction(ctx, js_classlist_contains, "contains", 1));
    JS_SetPropertyStr(ctx, o, "add", JS_NewCFunctionMagic(ctx, js_classlist_mut, "add", 1, JS_CFUNC_generic_magic, 0));
    JS_SetPropertyStr(ctx, o, "remove", JS_NewCFunctionMagic(ctx, js_classlist_mut, "remove", 1, JS_CFUNC_generic_magic, 1));
    JS_SetPropertyStr(ctx, o, "toggle", JS_NewCFunctionMagic(ctx, js_classlist_mut, "toggle", 1, JS_CFUNC_generic_magic, 2));
    return o;
}
/* DOM TRAVERSAL: parentNode/children/firstElementChild/nextElementSibling were undefined -> `el.children
   .length` / `el.parentNode.x` THREW, killing DOM-walking bundles. Return REAL el_wrap'd element nodes from
   Lexbor so a tree walk that reaches a fetch/sink is explored. children is a REAL array (.length/.forEach/[i]
   all work). Only ELEMENT nodes (text nodes aren't wrapped — a walker keying on .children matches the browser). */
static int node_is_element(lxb_dom_node_t *n) { return n && n->type == LXB_DOM_NODE_TYPE_ELEMENT; }
static JSValue js_el_parent(JSContext *ctx, JSValueConst this_val) {
    lxb_dom_element_t *el = JS_GetOpaque(this_val, g_el_class_id); if (!el) return JS_NULL;
    lxb_dom_node_t *p = lxb_dom_interface_node(el)->parent;
    return node_is_element(p) ? el_wrap(ctx, lxb_dom_interface_element(p)) : JS_NULL;
}
static JSValue js_el_children(JSContext *ctx, JSValueConst this_val) {
    lxb_dom_element_t *el = JS_GetOpaque(this_val, g_el_class_id);
    JSValue arr = JS_NewArray(ctx); if (!el) return arr;
    uint32_t idx = 0;
    for (lxb_dom_node_t *n = lxb_dom_interface_node(el)->first_child; n; n = n->next)
        if (node_is_element(n)) JS_SetPropertyUint32(ctx, arr, idx++, el_wrap(ctx, lxb_dom_interface_element(n)));
    return arr;
}
static JSValue js_el_first_el_child(JSContext *ctx, JSValueConst this_val) {
    lxb_dom_element_t *el = JS_GetOpaque(this_val, g_el_class_id); if (!el) return JS_NULL;
    for (lxb_dom_node_t *n = lxb_dom_interface_node(el)->first_child; n; n = n->next)
        if (node_is_element(n)) return el_wrap(ctx, lxb_dom_interface_element(n));
    return JS_NULL;
}
static JSValue js_el_next_el_sib(JSContext *ctx, JSValueConst this_val) {
    lxb_dom_element_t *el = JS_GetOpaque(this_val, g_el_class_id); if (!el) return JS_NULL;
    for (lxb_dom_node_t *n = lxb_dom_interface_node(el)->next; n; n = n->next)
        if (node_is_element(n)) return el_wrap(ctx, lxb_dom_interface_element(n));
    return JS_NULL;
}
static void el_install_methods(JSContext *ctx, JSValue proto) {
    JS_SetPropertyStr(ctx, proto, "getAttribute", JS_NewCFunction(ctx, js_el_getAttribute, "getAttribute", 1));
    JS_SetPropertyStr(ctx, proto, "setAttribute", JS_NewCFunction(ctx, js_el_setAttribute, "setAttribute", 2));
    JS_SetPropertyStr(ctx, proto, "appendChild", JS_NewCFunction(ctx, js_el_appendChild, "appendChild", 1));
    JS_SetPropertyStr(ctx, proto, "insertAdjacentHTML", JS_NewCFunction(ctx, js_el_insertAdjacentHTML, "insertAdjacentHTML", 2));
    JS_SetPropertyStr(ctx, proto, "querySelector", JS_NewCFunction(ctx, js_el_querySelector, "querySelector", 1));
    /* textContent / innerText are PROPERTIES in the real DOM (`el.textContent`), never methods — a
       getTextContent METHOD left `.textContent` UNDEFINED, so the ubiquitous label-text read and the SSR
       data pattern `JSON.parse(script.textContent)` silently returned undefined (and threw). Define the
       standard getters; the non-standard method is deleted (nothing used it). */
    for (int i = 0; i < 2; i++) {
        JSAtom a = JS_NewAtom(ctx, i ? "innerText" : "textContent");
        JS_DefinePropertyGetSet(ctx, proto, a, JS_NewCFunction2(ctx, (JSCFunction *)js_el_textContent, "get", 0, JS_CFUNC_getter, 0), JS_UNDEFINED, JS_PROP_CONFIGURABLE);
        JS_FreeAtom(ctx, a);
    }
    /* ELEMENT-LEVEL EVENT HANDLERS: most SPAs attach click/submit/change handlers to ELEMENTS (buttons, forms),
       not window. addEventListener must REGISTER them (js_add_listener -> g_handlers -> orphan-driven) — else
       the call throws (undefined method), killing the script and losing every element handler's endpoints/sinks.
       remove/dispatch are no-ops; click/focus/blur are no-ops so a call doesn't throw (the handler is
       reached by driving, not by a synthetic dispatch). submit/requestSubmit are NOT no-ops: a real browser
       fires the form's action request, so js_form_submit EMITS that @H endpoint. */
    JS_SetPropertyStr(ctx, proto, "addEventListener", JS_NewCFunction(ctx, js_add_listener, "addEventListener", 2));
    JS_SetPropertyStr(ctx, proto, "removeEventListener", JS_NewCFunction(ctx, js_noop, "removeEventListener", 2));
    JS_SetPropertyStr(ctx, proto, "dispatchEvent", JS_NewCFunction(ctx, js_noop, "dispatchEvent", 1));
    JS_SetPropertyStr(ctx, proto, "click", JS_NewCFunction(ctx, js_noop, "click", 0));
    JS_SetPropertyStr(ctx, proto, "submit", JS_NewCFunction(ctx, js_form_submit, "submit", 0));         /* @H: fires the form's action request, like a real browser */
    JS_SetPropertyStr(ctx, proto, "requestSubmit", JS_NewCFunction(ctx, js_form_submit, "requestSubmit", 1));
    JS_SetPropertyStr(ctx, proto, "focus", JS_NewCFunction(ctx, js_noop, "focus", 0));
    JS_SetPropertyStr(ctx, proto, "blur", JS_NewCFunction(ctx, js_noop, "blur", 0));
    JS_SetPropertyStr(ctx, proto, "remove", JS_NewCFunction(ctx, js_noop, "remove", 0));
    JS_SetPropertyStr(ctx, proto, "matches", JS_NewCFunction(ctx, js_el_matches, "matches", 1));
    JS_SetPropertyStr(ctx, proto, "closest", JS_NewCFunction(ctx, js_el_closest, "closest", 1));
    JS_SetPropertyStr(ctx, proto, "cloneNode", JS_NewCFunction(ctx, js_el_self, "cloneNode", 1));       /* returns a real node (self) whose methods/attrs work */
    JS_SetPropertyStr(ctx, proto, "contains", JS_NewCFunction(ctx, js_el_contains, "contains", 1));     /* REAL descendant check */
    JS_SetPropertyStr(ctx, proto, "hasAttribute", JS_NewCFunction(ctx, js_el_has_attr, "hasAttribute", 1));   /* REAL */
    JS_SetPropertyStr(ctx, proto, "getAttributeNames", JS_NewCFunction(ctx, js_el_getattrnames, "getAttributeNames", 0));   /* REAL */
    JS_SetPropertyStr(ctx, proto, "toggleAttribute", JS_NewCFunction(ctx, js_el_has_attr, "toggleAttribute", 1));
    JS_SetPropertyStr(ctx, proto, "querySelectorAll", JS_NewCFunction(ctx, js_el_querySelectorAll, "querySelectorAll", 1));
    JS_SetPropertyStr(ctx, proto, "insertBefore", JS_NewCFunction(ctx, js_el_appendChild, "insertBefore", 2));   /* intercepts <script src> like appendChild */
    JS_SetPropertyStr(ctx, proto, "replaceChild", JS_NewCFunction(ctx, js_el_appendChild, "replaceChild", 2));
    JS_SetPropertyStr(ctx, proto, "before", JS_NewCFunction(ctx, js_el_appendChild, "before", 1));
    JS_SetPropertyStr(ctx, proto, "after", JS_NewCFunction(ctx, js_el_appendChild, "after", 1));
    JS_SetPropertyStr(ctx, proto, "setAttributeNS", JS_NewCFunction(ctx, js_noop, "setAttributeNS", 3));
    JS_SetPropertyStr(ctx, proto, "removeAttribute", JS_NewCFunction(ctx, js_noop, "removeAttribute", 1));
    JS_SetPropertyStr(ctx, proto, "replaceChildren", JS_NewCFunction(ctx, js_noop, "replaceChildren", 0));
    JS_SetPropertyStr(ctx, proto, "scrollIntoView", JS_NewCFunction(ctx, js_noop, "scrollIntoView", 0));
    JS_SetPropertyStr(ctx, proto, "getBoundingClientRect", JS_NewCFunction(ctx, js_el_rect, "getBoundingClientRect", 0));
    { JSAtom a = JS_NewAtom(ctx, "style");
      JS_DefinePropertyGetSet(ctx, proto, a, JS_NewCFunction2(ctx, (JSCFunction *)js_el_style_get, "get style", 0, JS_CFUNC_getter, 0), JS_UNDEFINED, JS_PROP_CONFIGURABLE);
      JS_FreeAtom(ctx, a); }
    { JSAtom a = JS_NewAtom(ctx, "dataset");
      JS_DefinePropertyGetSet(ctx, proto, a, JS_NewCFunction2(ctx, (JSCFunction *)js_el_dataset_get, "get dataset", 0, JS_CFUNC_getter, 0), JS_UNDEFINED, JS_PROP_CONFIGURABLE);
      JS_FreeAtom(ctx, a); }
    for (int i = 0; i < 2; i++) {   /* innerHTML (magic 0) / outerHTML (magic 1) setter = XSS sink */
        JSAtom a = JS_NewAtom(ctx, i ? "outerHTML" : "innerHTML");
        JS_DefinePropertyGetSet(ctx, proto, a,
            JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)js_el_get_html, "get", 0, JS_CFUNC_getter_magic, i),
            JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)js_el_set_html, "set", 1, JS_CFUNC_setter_magic, i),
            JS_PROP_CONFIGURABLE);
        JS_FreeAtom(ctx, a);
    }
    /* Attribute-REFLECTED properties real bundles read constantly (undefined broke every read + branch). The
       PROPERTY name may differ from the attribute (className -> class); refl_name maps it. value/name/type are
       an input's shipped defaults (concrete page config). */
    static const char *refl[] = { "src", "href", "action", "id", "value", "name", "type", "className", "alt", "title", "placeholder", "srcdoc" };
    for (int i = 0; i < (int)(sizeof refl / sizeof refl[0]); i++) {
        JSAtom a = JS_NewAtom(ctx, refl[i]);
        JS_DefinePropertyGetSet(ctx, proto, a,
            JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)js_el_refl_get, "get", 0, JS_CFUNC_getter_magic, i),
            JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)js_el_refl_set, "set", 1, JS_CFUNC_setter_magic, i),
            JS_PROP_CONFIGURABLE);
        JS_FreeAtom(ctx, a);
    }
    for (int i = 0; i < 2; i++) {   /* tagName / nodeName -> the uppercase tag */
        JSAtom a = JS_NewAtom(ctx, i ? "nodeName" : "tagName");
        JS_DefinePropertyGetSet(ctx, proto, a, JS_NewCFunction2(ctx, (JSCFunction *)js_el_tagname, "get", 0, JS_CFUNC_getter, 0), JS_UNDEFINED, JS_PROP_CONFIGURABLE);
        JS_FreeAtom(ctx, a);
    }
    { JSAtom a = JS_NewAtom(ctx, "classList");
      JS_DefinePropertyGetSet(ctx, proto, a, JS_NewCFunction2(ctx, (JSCFunction *)js_el_classlist_get, "get", 0, JS_CFUNC_getter, 0), JS_UNDEFINED, JS_PROP_CONFIGURABLE);
      JS_FreeAtom(ctx, a); }
    static const char *boolp[] = { "checked", "disabled", "hidden", "selected", "required", "readOnly", "multiple" };
    for (int i = 0; i < (int)(sizeof boolp / sizeof boolp[0]); i++) {   /* boolean attribute-presence props */
        JSAtom a = JS_NewAtom(ctx, boolp[i]);
        JS_DefinePropertyGetSet(ctx, proto, a, JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)js_el_bool_get, "get", 0, JS_CFUNC_getter_magic, i), JS_UNDEFINED, JS_PROP_CONFIGURABLE);
        JS_FreeAtom(ctx, a);
    }
    /* traversal getters -> real el_wrap'd nodes (property name : backing getter) */
    struct { const char *prop; JSCFunction *fn; } trav[] = {
        { "parentNode", (JSCFunction *)js_el_parent }, { "parentElement", (JSCFunction *)js_el_parent },
        { "children", (JSCFunction *)js_el_children }, { "childNodes", (JSCFunction *)js_el_children },
        { "firstChild", (JSCFunction *)js_el_first_el_child }, { "firstElementChild", (JSCFunction *)js_el_first_el_child },
        { "nextSibling", (JSCFunction *)js_el_next_el_sib }, { "nextElementSibling", (JSCFunction *)js_el_next_el_sib },
    };
    for (int i = 0; i < (int)(sizeof trav / sizeof trav[0]); i++) {
        JSAtom a = JS_NewAtom(ctx, trav[i].prop);
        JS_DefinePropertyGetSet(ctx, proto, a, JS_NewCFunction2(ctx, trav[i].fn, "get", 0, JS_CFUNC_getter, 0), JS_UNDEFINED, JS_PROP_CONFIGURABLE);
        JS_FreeAtom(ctx, a);
    }
}
static JSValue js_doc_createElement(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (!g_dom || argc < 1) return JS_NULL;
    const char *tag = JS_ToCString(ctx, argv[0]); if (!tag) return JS_NULL;
    lxb_dom_element_t *el = lxb_dom_document_create_element(lxb_dom_interface_document(g_dom), (const lxb_char_t *)tag, strlen(tag), NULL);
    JS_FreeCString(ctx, tag);
    return el_wrap(ctx, el);
}
/* document.write: DOM edge (no-op; security is forced-exec, not a taint check). */
static JSValue js_doc_write(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    for (int i = 0; i < argc; i++) solve_add(ctx, "document.write", "html", argv[i]);   /* @S */
    return JS_UNDEFINED;
}
/* eval(concrete) -> forced-execute (dynamic code path, orphans); eval(external input) stays opaque. */
/* new Function(...args, BODY): the body is compiled as code -> an eval-class @S sink. Wrap the builtin:
   attacker/candidate body -> solve_add + a harmless callable (so `new Function(x)()` doesn't throw); anything
   else DELEGATES to the real Function (saved), so identity behaviour is preserved. wrap.prototype is set to
   the real Function.prototype so `x instanceof Function` still holds. */
static JSValue g_real_function = JS_UNDEFINED;
static JSValue js_function_ctor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv) {
    if (argc >= 1) {
        JSValueConst body = argv[argc - 1];
        if (JS_IsOpaque(body) || (g_candidate && JS_IsString(body))) {
            solve_add(ctx, "Function", "js", body);   /* @S: body is code */
            return JS_NewCFunction(ctx, js_noop, "", 0);   /* callable no-op so new Function(x)() is safe */
        }
    }
    return JS_IsUndefined(g_real_function) ? JS_NewCFunction(ctx, js_noop, "", 0)
                                           : JS_CallConstructor(ctx, g_real_function, argc, argv);   /* real compile */
}
static JSValue js_eval(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc < 1) return JS_UNDEFINED;
    if (JS_IsOpaque(argv[0])) { solve_add(ctx, "eval", "js", argv[0]); return JS_DupValue(ctx, g_opaque); }   /* @S: opaque reaches eval -> detect + spawn candidate replays */
    /* @S SOLVE: on a candidate-REPLAY flow the payload arrives here as a CONCRETE string (the real code
       transformed it). eval's arg IS the sink code, so RECORD it for the breakout check — do NOT JS_Eval it in
       the engine (that would run the X9 payload against an undefined X9 and never verify the sink). Without
       this, `eval(location.hash)` was never solved (the replay executed the payload instead of checking it). */
    if (g_candidate && JS_IsString(argv[0])) { solve_add(ctx, "eval", "js", argv[0]); return JS_DupValue(ctx, g_opaque); }
    if (JS_IsString(argv[0])) {
        size_t len = 0; const char *s = JS_ToCStringLen(ctx, &len, argv[0]);
        JSValue r = s ? JS_Eval(ctx, s, len, "<eval>", JS_EVAL_TYPE_GLOBAL) : JS_UNDEFINED;
        if (s) JS_FreeCString(ctx, s);
        return r;
    }
    return JS_DupValue(ctx, argv[0]);   /* non-string: spec returns the arg unchanged */
}
static JSValue js_doc_getElementById(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc < 1) return JS_NULL;
    const char *id = JS_ToCString(ctx, argv[0]); if (!id) return JS_NULL;
    char sel[512]; snprintf(sel, sizeof sel, "#%s", id);
    lxb_dom_element_t *el = dom_select_first(NULL, sel, strlen(sel));
    JS_FreeCString(ctx, id);
    return el_wrap(ctx, el);
}
static JSValue js_doc_querySelector(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc < 1) return JS_NULL;
    const char *s = JS_ToCString(ctx, argv[0]); if (!s) return JS_NULL;
    lxb_dom_element_t *el = dom_select_first(NULL, s, strlen(s));
    JS_FreeCString(ctx, s);
    return el_wrap(ctx, el);
}
/* Parse page HTML into the live DOM + init the CSS-selector engine. Returns 0 on success. */
static int dom_init(const char *html, size_t len) {
    g_dom = lxb_html_document_create();
    if (!g_dom) return -1;
    if (html && len && lxb_html_document_parse(g_dom, (const lxb_char_t *)html, len) != LXB_STATUS_OK) return -1;
    return 0;
}

/* Run the document's own scripts (in document order) against the real DOM — the moat runs the page's
   UNMODIFIED bundle, so the ENGINE extracts + executes them, not a bridge-side scrape. Inline <script>
   is eval'd in the global scope; external <script src> is surfaced as @CHUNK for safe-fetch + forced-
   execute (step C — computed/injected src is the same edge). */
struct scr_ctx { lxb_dom_element_t **els; int n, cap; };
static lxb_status_t scr_collect_cb(lxb_dom_node_t *node, lxb_css_selector_specificity_t s, void *vp) {
    struct scr_ctx *c = vp; (void)s;
    if (c->n >= c->cap) { int nc = c->cap ? c->cap * 2 : 16;
        lxb_dom_element_t **ne = realloc(c->els, (size_t)nc * sizeof(*ne)); if (!ne) return LXB_STATUS_OK; c->els = ne; c->cap = nc; }
    c->els[c->n++] = lxb_dom_interface_element(node);
    return LXB_STATUS_OK;   /* collect all in document order; eval AFTER traversal (eval may mutate the DOM) */
}
/* BOOT-REPLAY substrate: the page's inline <script> texts, cached at first boot. A cross-flow @S candidate
   (source stored in shared state at boot, sunk in a SEPARATELY-driven handler) needs that shared state
   re-established with the CONCRETE candidate — the handler reads the stored value, not the source directly.
   So the candidate flow re-runs boot with g_candidate pinned (source getters return concrete), then drives
   the handler; the re-run's writes are COW-captured + reverted like any flow, so isolation holds. A
   top-level const/let re-declares CLEANLY because the pre-boot unapply (boot_replay_candidate) deletes its
   captured CREATION, so no re-declaration clash. Remaining limit (full boot-as-flow): external <script src>
   chunks aren't re-run — a source stored in a fetched chunk isn't re-established under the candidate. */
static char **g_boot_scripts = NULL; static int g_boot_n = 0, g_boot_cap = 0;
static void boot_script_cache(const char *txt, size_t len) {
    if (g_boot_n >= g_boot_cap) { int nc = g_boot_cap ? g_boot_cap * 2 : 8;
        char **n = realloc(g_boot_scripts, (size_t)nc * sizeof(char *)); if (!n) return; g_boot_scripts = n; g_boot_cap = nc; }
    char *s = malloc(len + 1); if (!s) return; memcpy(s, txt, len); s[len] = 0; g_boot_scripts[g_boot_n++] = s;
}
/* Re-run the page's inline boot scripts per-script at GLOBAL scope (faithful — top-level var/let/const/function
   land exactly where the browser puts them). The CALLER unapplies g_boot_delta first, so boot's globals —
   including captured let/const CREATIONS — are ABSENT: re-declaration compiles cleanly, no block-wrap. A
   residual throw means an UNCAPTURED creation (COW gap to close at the root) or a real host-edge divergence —
   a FATAL bug: crash with the real exception, never swallow it. Branch behaviour is the caller's
   (g_boot_replay=1 fixed-arm for @S candidates; g_in_boot_flow=1 FORKING for a boot flow). */
static void boot_scripts_run(JSContext *ctx) {
    for (int i = 0; i < g_boot_n; i++) {
        JSValue v = JS_Eval(ctx, g_boot_scripts[i], strlen(g_boot_scripts[i]), "<boot-replay>", JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(v)) {   /* boot re-run must be faithful — a throw is a COW/host gap to FIX, not to log past */
            JSValue e = JS_GetException(ctx); const char *em = JS_ToCString(ctx, e);
            fflush(stdout); fprintf(stderr, "@E {\"phase\":\"boot-replay\",\"reason\":\"boot script threw on re-run: %s\"}\n", em ? em : "?"); fflush(stderr);
            abort();
        }
        JS_FreeValue(ctx, v);
    }
}
static void boot_replay(JSContext *ctx) { g_boot_replay = 1; boot_scripts_run(ctx); g_boot_replay = 0; }
/* Enqueue a BOOT FLOW: re-run boot as a FORKING starter (decision vector), so cached async replies resolve
   synchronously and their continuations' gated branches fork with the concolic example. */
static int reg_add_boot(JSContext *ctx, signed char *dec, int dec_n) {
    if (!reg_add(ctx, JS_UNDEFINED, 1.3, 0, dec, dec_n)) return 0;
    g_reg[g_reg_n - 1].is_boot = 1;
    return 1;
}
/* An EXPLORATORY attacker-session that FORKS: re-fire ALL handlers over one accumulating delta, replaying
   `dec` then forking new opaque branches. This is how a gate in handler B on state that handler A wrote from
   an OPAQUE source (the canonical "login handler sets auth flag; gated code reads it" SPA pattern) gets its
   admin arm explored — a per-handler orphan flow can't (it's COW-isolated from A's write), and a fixed-arm
   session can't (it never forks). A candidate session (verifying a breakout) stays fixed-arm; only this
   exploratory kind forks. */
static int reg_add_session(JSContext *ctx, signed char *dec, int dec_n) {
    if (!reg_add(ctx, JS_UNDEFINED, 1.2, 0, dec, dec_n)) return 0;
    g_reg[g_reg_n - 1].session = 1;
    return 1;
}
/* BOOT AS THE FIRST FLOW: the page's boot (its inline scripts) is captured as a COW DELTA — g_boot_delta —
   exactly like any flow (mutations + global CREATIONS). It stays APPLIED between opaque flows (its effects
   are the post-boot baseline). A candidate flow UNAPPLIES it to reach a TRUE pre-boot heap (boot's globals
   deleted, so a guarded init `if(!window.d){window.d=src}` re-fires), re-runs boot under the concrete
   candidate as its OWN delta, drives, then REAPPLIES g_boot_delta so the next opaque flow sees post-boot.
   No host-side property save/delete/restore — the delta IS the mechanism (heap; DOM boot stays baseline). */
static void *g_boot_delta = NULL; static int g_boot_delta_n = 0, g_boot_delta_cap = 0;
static void boot_replay_candidate(JSContext *ctx) {
    /* Seed the RUNNING candidate flow's OWN delta with the boot INVERSE (heap -> pre-boot, RECORDED so a
       suspend/revert restores the post-boot baseline), then re-run boot under the concrete candidate as more
       of the SAME delta. The whole boot-undo + candidate-replay is ONE preemptible flow delta — no host-side
       bracket, so a candidate flow yields per-opcode like any other. g_boot_delta is only READ (canonical
       post-boot baseline for non-candidate flows), never unapplied on the shared heap. */
    if (g_boot_delta) JS_CowSeedBootInverse(ctx, g_boot_delta, g_boot_delta_n);
    boot_replay(ctx);   /* re-run boot under the concrete candidate (guards re-fire), captured in the candidate's own delta */
}
/* CLOSURE cross-flow: an orphan handler captured at seed time (f.handle) closes over the BASELINE source;
   boot_replay under the candidate re-created that handler with the CANDIDATE closure. Re-resolve to the
   fresh one by SOURCE IDENTITY (same JS_OrphanHash) among the current global functions, so the candidate
   actually flows through the closure the handler reads. Returns a NEW ref (caller frees), or JS_UNDEFINED
   if none differs (then the original handle is correct — e.g. it reads a shared global boot_replay updated). */
static JSValue resolve_replayed_handler(JSContext *ctx, JSValueConst orig) {
    if (JS_IsUndefined(orig)) return JS_UNDEFINED;
    uint32_t want = JS_OrphanHash(ctx, orig);
    JSValue found = JS_UNDEFINED;
    /* FIRST the handlers boot_replay re-registered via addEventListener (a closure handler isn't on any
       global) — then the global functions (window.h = closure, module pattern). */
    for (int i = 0; i < g_replay_handler_n && JS_IsUndefined(found); i++) {
        JSValueConst v = g_replay_handlers[i];
        if (JS_VALUE_GET_PTR(v) != JS_VALUE_GET_PTR(orig) && JS_OrphanHash(ctx, v) == want) found = JS_DupValue(ctx, v);
    }
    if (JS_IsUndefined(found)) {
        JSValue g = JS_GetGlobalObject(ctx);
        JSPropertyEnum *tab = NULL; uint32_t n = 0;
        if (JS_GetOwnPropertyNames(ctx, &tab, &n, g, JS_GPN_STRING_MASK) == 0) {
            for (uint32_t i = 0; i < n && JS_IsUndefined(found); i++) {
                JSValue v = JS_GetProperty(ctx, g, tab[i].atom);
                if (JS_IsFunction(ctx, v) && JS_VALUE_GET_PTR(v) != JS_VALUE_GET_PTR(orig) && JS_OrphanHash(ctx, v) == want)
                    found = JS_DupValue(ctx, v);
                JS_FreeValue(ctx, v);
            }
            JS_FreePropertyEnum(ctx, tab, n);
        }
        JS_FreeValue(ctx, g);
    }
    return found;   /* g_replay_handlers/msg stay valid until is_msg_handler(drive) has run; cleared after the drive */
}
/* ATTACKER SESSION: fire ALL registered handlers in seed order over the CURRENT (accumulating) COW delta,
   modeling an attacker firing a sequence of events. Handler A's tainted write to shared state persists to
   handler B (no revert between them), so a cross-handler sink — source stored by A, sunk by B — is reached:
   opaque -> the sink is DETECTED (task recorded), candidate -> breakout is VERIFIED. Branches take a fixed
   arm here (per-handler branch exploration is the individual orphan flows' job); the session adds only the
   cross-handler STATE dimension. */
/* __sessionFns(): the session fire list as [fn, event] pairs — event handlers (msg handlers get the synthetic
   {pm} MessageEvent) then the collected ORPHANS (deduped vs handlers, opaque arg). Exposed so the session LOOP
   is SELF-HOSTED bytecode (like the array methods) rather than a C loop that can't yield mid-iteration and so
   runs to completion — a violation of the per-opcode-yield core. */
static JSValue js_session_fns(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    JSValue arr = JS_NewArray(ctx); uint32_t n = 0, hn = 0;
    if (!JS_IsUndefined(g_handlers)) { JSValue lv = JS_GetPropertyStr(ctx, g_handlers, "length"); JS_ToUint32(ctx, &hn, lv); JS_FreeValue(ctx, lv); }
    for (uint32_t i = 0; i < hn; i++) {
        JSValue h = JS_GetPropertyUint32(ctx, g_handlers, i);
        if (JS_IsFunction(ctx, h)) {
            JSValueConst ev = (is_msg_handler(h) && !JS_IsUndefined(g_msg_event)) ? g_msg_event : g_opaque;
            JSValue pair = JS_NewArray(ctx);
            JS_SetPropertyUint32(ctx, pair, 0, JS_DupValue(ctx, h));
            JS_SetPropertyUint32(ctx, pair, 1, JS_DupValue(ctx, ev));
            JS_SetPropertyUint32(ctx, arr, n++, pair);
        }
        JS_FreeValue(ctx, h);
    }
    for (int oi = 0; oi < g_orphan_n; oi++) {
        JSValueConst fn = g_orphan_buf[oi];
        if (!JS_IsFunction(ctx, fn)) continue;
        int is_h = 0;
        for (uint32_t i = 0; i < hn && !is_h; i++) { JSValue h = JS_GetPropertyUint32(ctx, g_handlers, i); if (JS_VALUE_GET_PTR(h) == JS_VALUE_GET_PTR(fn)) is_h = 1; JS_FreeValue(ctx, h); }
        if (is_h) continue;
        JSValue pair = JS_NewArray(ctx);
        JS_SetPropertyUint32(ctx, pair, 0, JS_DupValue(ctx, fn));
        JS_SetPropertyUint32(ctx, pair, 1, JS_DupValue(ctx, g_opaque));
        JS_SetPropertyUint32(ctx, arr, n++, pair);
    }
    return arr;
}
static JSValue js_session_drain(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    JSContext *c; while (JS_ExecutePendingJob(g_rt, &c) > 0) {}   /* per-fn microtask drain, parity with the C loop */
    return JS_UNDEFINED;
}
/* Run a page script LIKE A BROWSER. is_module is the REAL browser signal — the <script type="module">
   attribute for inline, JS_DetectModule(body) for a fetched chunk — never a parse-failure guess. A classic
   script runs GLOBAL and its runtime throw surfaces as @WHY (never swallowed); a module runs as ESM and, if
   a static-import dep isn't fetched yet, defers into g_pendmod (retried on each qjs_provide). */
static void eval_page_script(JSContext *ctx, const char *code, size_t len, const char *name, int is_module) {
    if (is_module) {
        char nm[32]; snprintf(nm, sizeof nm, "<mod-%d>", g_modseq++);    /* unique so no name collision on defer/retry */
        JSValue v = JS_Eval(ctx, code, len, nm, JS_EVAL_TYPE_MODULE);
        if (JS_IsException(v)) { JS_FreeValue(ctx, JS_GetException(ctx)); pendmod_add(code, len); }  /* dep not fetched yet: defer */
        else { JSContext *c; while (JS_ExecutePendingJob(g_rt, &c) > 0) {} }  /* module eval is async -> drive it */
        JS_FreeValue(ctx, v);
        return;
    }
    JSValue v = JS_Eval(ctx, code, len, name, JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(v)) {   /* a genuine RUNTIME throw -> surface it, never silently swallow */
        JSValue e = JS_GetException(ctx); const char *m = JS_ToCString(ctx, e);
        char rz[300]; snprintf(rz, sizeof rz, "%s: %s", name ? name : "?", m ? m : "throw");
        why_add(ctx, "script-eval", rz);
        if (m) JS_FreeCString(ctx, m); JS_FreeValue(ctx, e);
    }
    JS_FreeValue(ctx, v);
}
static void dom_run_scripts(JSContext *ctx) {
    if (!g_dom) return;
    lxb_css_parser_t *p = lxb_css_parser_create();
    if (!p || lxb_css_parser_init(p, NULL) != LXB_STATUS_OK) { if (p) lxb_css_parser_destroy(p, true); return; }
    lxb_css_selector_list_t *list = lxb_css_selectors_parse(p, (const lxb_char_t *)"script", 6);
    if (!list) { lxb_css_parser_destroy(p, true); return; }
    lxb_selectors_t *sel = lxb_selectors_create();
    if (!sel || lxb_selectors_init(sel) != LXB_STATUS_OK) { if (sel) lxb_selectors_destroy(sel, true); lxb_css_parser_destroy(p, true); return; }
    struct scr_ctx c = { NULL, 0, 0 };
    lxb_selectors_find(sel, lxb_dom_interface_node(g_dom), list, scr_collect_cb, &c);
    lxb_selectors_destroy(sel, true); lxb_css_parser_destroy(p, true);
    uint32_t bh = 2166136261u;   /* FNV-1a bundle identity over the page's OWN scripts (Lexbor DOM, not regex) */
    for (int i = 0; i < c.n; i++) {
        lxb_dom_element_t *el = c.els[i];
        size_t sl = 0;
        const lxb_char_t *src = lxb_dom_element_get_attribute(el, (const lxb_char_t *)"src", 3, &sl);
        if (src && sl) {
            char *cu = strndup((const char *)src, sl);
            /* LOAD it like a real browser: an external <script src> is FETCHED (safe-fetch chokepoint,
               cross-origin allowed — a real browser runs cross-origin scripts) and RUN through the engine,
               exactly like a dynamically-injected one. chunk_pending_add -> host NEED_FETCH -> qjs_provide
               evals + caches it, so the bundle's endpoints/handlers/cross-flow are analyzed. */
            if (cu) { arr_push_str(g_ctx, g_chunkurls, cu); if (!has_hole(cu)) chunk_pending_add(cu); free(cu); }
            for (size_t k = 0; k < sl; k++) { bh ^= src[k]; bh *= 16777619u; }     /* external src URL -> bundle id */
            bh ^= '|'; bh *= 16777619u;
            continue;
        }
        size_t tl = 0;
        lxb_char_t *txt = lxb_dom_node_text_content(lxb_dom_interface_node(el), &tl);
        if (txt && tl) {
            for (size_t k = 0; k < tl; k++) { bh ^= txt[k]; bh *= 16777619u; }     /* inline body -> bundle id */
            bh ^= '|'; bh *= 16777619u;
            boot_script_cache((const char *)txt, tl);   /* cache for cross-flow @S candidate boot-replay */
            size_t tyl = 0; const lxb_char_t *ty = lxb_dom_element_get_attribute(el, (const lxb_char_t *)"type", 4, &tyl);
            int is_mod = ty && tyl == 6 && memcmp(ty, "module", 6) == 0;   /* the browser's real signal */
            g_current_script = el_wrap(ctx, el);   /* document.currentScript during this inline script */
            eval_page_script(ctx, (const char *)txt, tl, "<script>", is_mod);
            JS_FreeValue(ctx, g_current_script); g_current_script = JS_NULL;
        }
        if (txt) lxb_dom_document_destroy_text(lxb_dom_interface_node(el)->owner_document, txt);
    }
    g_bundle_id = bh ? bh : 1;
    free(c.els);
}

/* __fork(fn, hint?): add a flow to the ONE registry (a STARTER, fresh decision vector). */
static JSValue js_fork(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) return JS_ThrowTypeError(ctx, "__fork(fn)");
    double hint = 0; if (argc > 1) JS_ToFloat64(ctx, &hint, argv[1]);
    reg_add(ctx, JS_DupValue(ctx, argv[0]), hint, 0, NULL, 0);
    return JS_UNDEFINED;
}

/* __yield(): PARK the running flow; its resolve fn becomes a RESUMER carrying the flow's value. */
static JSValue js_yield(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSValue rf[2]; JSValue promise = JS_NewPromiseCapability(ctx, rf);
    if (JS_IsException(promise)) return promise;
    reg_add(ctx, rf[0], g_cur_val, 1, NULL, 0);
    JS_FreeValue(ctx, rf[1]);
    return promise;
}
/* __isOpaque(v): CONCRETE bool (never forks), the ONE primitive the self-hosted Array.sort needs. A branch on an
   OPAQUE value forks; sort must NOT fork on the meaningless ORDER of opaque elements (O(n log n) forks explode).
   So the sort's comparators call __isOpaque to collapse an opaque compare to 0 WITHOUT an OP_if fork — the
   comparator itself still RUNS (via the trampolined OP_call, so deep comparator recursion stays unbounded and its
   emits happen); only the order bit concretizes. A leaf (holds no continuation), so it never adds C recursion. */
static JSValue js_is_opaque(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    return JS_NewBool(ctx, argc > 0 && JS_IsOpaque(argv[0]));
}
/* __opaqueExample(v): the CONCRETE example an opaque carries (config/reply loaded data), or undefined for a
   pure attacker symbol (location.hash / cross-origin postMessage — no example). Self-hosted JSON.stringify
   uses this so a CONFIG value serializes to its real value (`{"orgId":"acme-42"}`, the concolic example
   propagating through an op) while ATTACKER input stays the taint-preserving opaque. A leaf. */
static JSValue js_opaque_example(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    return argc > 0 ? JS_OpaqueExample(ctx, argv[0]) : JS_UNDEFINED;
}

/* orphan flow source — CONTINUOUS discovery, NOT a one-shot phase. Called every scheduler iteration so
   functions defined DYNAMICALLY during a forced flow (a login-gated lazy CHUNK: eval/import of fetched
   JS reached only by forcing the auth branch) become orphan flows and get driven -> we learn the
   logged-in surface while logged out. Already-executed fns are not returned by JS_CollectOrphans;
   already-queued fns are dup-skipped. Idempotent: re-running adds only NEW never-executed functions. */
static int seed_orphans(JSContext *ctx)
{
    static JSValue buf[4096];
    int n = JS_CollectOrphans(ctx, buf, 4096), seeded = 0;
    for (int i = 0; i < n; i++) {
        int dup = 0;
        for (int j = 0; j < g_reg_n; j++)
            if (!g_reg[j].is_resume && JS_VALUE_GET_PTR(g_reg[j].handle) == JS_VALUE_GET_PTR(buf[i])) { dup = 1; break; }
        /* also skip one already recorded in the stable buffer (queued/parked, not yet run) */
        if (!dup) for (int j = 0; j < g_orphan_n; j++)
            if (JS_VALUE_GET_PTR(g_orphan_buf[j]) == JS_VALUE_GET_PTR(buf[i])) { dup = 1; break; }
        if (dup) { JS_FreeValue(ctx, buf[i]); continue; }
        int idx = -1;
        if (g_orphan_n < 4096) { idx = g_orphan_n; g_orphan_buf[g_orphan_n++] = JS_DupValue(ctx, buf[i]); }  /* buffer owns a ref (stable locator) */
        if (g_resume_mode) { JS_FreeValue(ctx, buf[i]); continue; }   /* resume: build locators only; recipes are seeded explicitly */
        reg_add(ctx, buf[i], 1.0, 0, NULL, 0);
        g_reg[g_reg_n - 1].orphan_idx = idx;
        seeded++;
    }
    return seeded;   /* count surfaces in @RESULT._orphans (via g_orphan_n) — no dead @ORPHANS line */
}
static JSValue js_drive_orphans(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{ return JS_NewInt32(ctx, seed_orphans(ctx)); }

/* Per-flow isolation is the engine COW (JS_CowSetActive/JS_CowRevert): shared-state writes (var-refs =
   globals/lexicals/closures; baseline-object property mutations) are captured while a flow explores, and
   reverted to the post-boot BASELINE before the next STARTER runs — so an independent flow never sees
   another's writes, yet a flow sees its OWN writes within its run. */

/* WFQ order key (ORDER-only, never drops a work item): base + accumulated value-of-information
   (emits raise val) + an explore/fairness floor for rarely-scheduled flows - a CPU-since-emit decay
   so a monopolizer (burns CPU, emits nothing) SINKS below productive/unrun flows and gets starved.
   This is the ONE policy the host-level priority.js uses too; same formula, two levels. */
static double flow_weight(const Flow *f)
{
    return 1.0 + f->val + 0.5 / (double)(f->visits + 1) - 0.01 * f->cpu;
}

/* The value-driven yield decision, called by the engine at each loop back-edge of the running TOP flow
   frame. Ticks CPU (accounting, NOT a cap — the flow is resumable, never truncated), then yields iff a
   PARKED flow now outranks the running one. The JS heap AND Lexbor DOM are per-flow COW deltas that swap on
   context-switch (JS_CowBufTake/Load + dom delta), so a flow is preemptible mid-write to EITHER — no writer
   runs to completion, no empty-delta guard. */
static int wfq_yield(void)
{
    if (!g_cur_flow) return 0;
    g_cur_flow->cpu += 1.0;
    /* Both the JS heap AND the Lexbor DOM are per-flow COW deltas that swap on context-switch, so a flow is
       preemptible mid-write to either — no writer runs to completion. */
    double rw = flow_weight(g_cur_flow);
    for (int i = 0; i < g_reg_n; i++)
        if (flow_weight(&g_reg[i]) > rw) return 1;   /* a parked flow now outranks the running flow -> yield */
    return 0;
}

/* Emit the remaining frontier as compact REPLAY recipes (orphan_idx + decision-vector) and clear it.
   A parked flow is reconstructed next session by re-running boot + replaying its decisions — never by
   serializing a live continuation. The host persists these lines to IDB; @PARK/@PARKED are read back. */
static void park_frontier(JSContext *ctx)
{
    int parked = 0;
    for (int i = 0; i < g_reg_n; i++) {
        Flow *f = &g_reg[i];
        if (!f->candidate && f->orphan_idx >= 0 && f->orphan_idx < g_orphan_n) {   /* orphan-derived flows are replay-locatable (NOT transient @S candidate flows) */
            uint32_t oh = JS_OrphanHash(ctx, g_orphan_buf[f->orphan_idx]);   /* stable identity, not the positional index */
            if (oh) {
                char rec[80]; int o = snprintf(rec, sizeof rec, "%u,", oh);
                for (int j = 0; j < f->dec_n && o < (int)sizeof(rec) - 1; j++) rec[o++] = f->dec[j] ? '1' : '0';
                rec[o] = 0;
                arr_push_str(ctx, g_park, rec);   /* "hash,decbits" replay recipe -> @RESULT._park */
                parked++;
            }
        }
        if (f->fs) JS_FlowFree(g_rt, f->fs);
        if (f->cow) JS_CowBufFree(ctx, f->cow, f->cow_n);   /* free the parked flow's stashed heap COW delta */
        if (f->dom) dom_buf_free((DomUndo *)f->dom, f->dom_n);   /* and its DOM delta */
        JS_FreeValue(ctx, f->handle);
        free(f->dec); free(f->candidate); free(f->vtarget);
    }
    g_reg_n = 0;
    (void)parked;   /* recipes accumulated into g_park -> @RESULT._park (no separate @PARKED line) */
}

/* A BOOT FLOW is pending (reply-triggered forking re-run or a boot-fork sibling) — a RAM-pressure park must
   not drop it: it delivers an already-fetched reply's gated surface and has no replay recipe if dropped. */
static int reg_has_boot(void) {
    for (int i = 0; i < g_reg_n; i++) if (g_reg[i].is_boot) return 1;
    return 0;
}
/* The ONE scheduler loop: pick the highest-WEIGHT flow (NON-FIFO), run it as a preemptible heap-frame
   flow (per-opcode yield, no slice count), re-queue it if it suspended (interleave), repeat. BFS by value-of-information: shallow
   high-emit flows finish ahead of the deep residue, which is starved to ~0 CPU (resumable). */
static void scheduler_run(JSContext *ctx)
{
    for (;;) {
        seed_orphans(ctx);          /* CONTINUOUS: pick up functions a prior flow defined dynamically (chunks) */
        if (g_reg_n == 0) break;
        /* COLD-TIER PARK (cross-page/cross-session fairness) = RESOURCE PRESSURE on ALL work, not just
           starters: on RAM PRESSURE (host-driven, RESOURCE-based — NEVER a dispatch count; a per-N slice is
           the banned step-cap the value yield-floor below already avoids), PARK the rest of the frontier as replay
           recipes (orphan_idx + decision-vector) to the IDB cold tier and stop — resumable next burst/session.
           The host sets g_park_requested when resident RAM crosses the working-set floor. With RAM headroom (a
           lone engine, a small page) it NEVER fires, so the page runs to COMPLETION in one visit — findings are
           never lost to a clock. Not a bound: parked flows re-drive by re-running boot + decisions, and BFS has
           already extracted the productive breadth, so pressure only ever touches the least-valuable starved tail.
           EXCEPTION — never park while a BOOT FLOW is pending: a reply-triggered forking boot (reg_add_boot on
           a NEW reply) is the DELIVERY of an already-fetched reply — the moat's headline gated/logged-in surface
           — and it carries NO orphan_idx, so park_frontier would DROP it with no recipe (permanently lost, not
           deferred). The WFQ, not a clock, orders the boot-fork tree; it drains (finite reply-gated branches). */
        if (g_park_requested && !reg_has_boot()) { park_frontier(ctx); break; }
        int best = 0;
        for (int i = 1; i < g_reg_n; i++)
            if (flow_weight(&g_reg[i]) > flow_weight(&g_reg[best])) best = i;
        /* HOST-LEVEL VALUE YIELD (cross-document fairness): yield HOT to the host WFQ the moment this engine's
           BEST flow no longer outranks the runner-up ENGINE (weight set by the host in g_yield_floor) — the
           frontier stays in g_reg, the host re-ranks all live engines + the top cold recipe and resumes the
           winner. VALUE-driven, NOT a dispatch count (a per-N slice is a banned step-cap). g_made_progress
           guards against a zero-work ping-pong at the boundary. */
        if (g_made_progress && flow_weight(&g_reg[best]) <= g_yield_floor) break;
        Flow f = g_reg[best];                       /* f is a STABLE COPY: reg_add during the run may realloc g_reg */
        g_reg_n--; g_reg[best] = g_reg[g_reg_n];    /* swap-remove */
        /* REDUNDANT @S candidate: another candidate already broke out this sink (in g_verified) -> skip it,
           saving a full boot-replay/bundle re-run. Not a bound: the sink IS solved; the work is duplicate. */
        if (f.vtarget && JS_IsObject(g_verified)) {
            JSValue vv = JS_GetPropertyStr(ctx, g_verified, f.vtarget); int solved = JS_IsString(vv); JS_FreeValue(ctx, vv);
            if (solved) { JS_FreeValue(ctx, f.handle); free(f.dec); free(f.candidate); free(f.vtarget); continue; }
        }
        f.visits++;
        g_work++;                                   /* one flow got CPU (starter OR resume) — diagnostic tally */
        g_made_progress = 1;                        /* dispatched a flow this visit (progress guard, not a cap) */
        g_cur_orphan_idx = f.orphan_idx;            /* siblings forked during this flow inherit its locator */
        g_running = 1; g_cur_val = f.val; g_cur_flow = &f;
        g_candidate = f.candidate;   /* @S replay flow: source getters return this concrete candidate (else NULL=opaque) */
        g_in_session = f.session;    /* a session flow: sinks reached inside enqueue candidate sessions; cleared for every other flow */

        if (f.is_resume) {
            /* async __yield resume: drive the microtask to completion (a resolve fn, not sync-preemptible) */
            g_cur_fn = JS_UNDEFINED; g_dec_n = 0; g_c = 0;
            JS_SetFlowYieldHook(NULL);
            JSValue u = JS_UNDEFINED;
            JSValue r = JS_Call(ctx, f.handle, JS_UNDEFINED, 1, &u);
            if (JS_IsException(r)) js_std_dump_error(ctx);
            JS_FreeValue(ctx, r);
            JSContext *c1; int jr;
            while ((jr = JS_ExecutePendingJob(g_rt, &c1)) > 0) { }
            if (jr < 0) js_std_dump_error(c1 ? c1 : ctx);
            JS_CowRevert(ctx);                            /* discard the resume's heap writes -> baseline */
            { int cn, cc; void *cb = JS_CowBufTake(&cn, &cc); JS_CowBufFree(ctx, cb, cn); }
            dom_revert();                                 /* discard the resume's DOM writes -> baseline */
            { int dn, dc; void *db = dom_buf_take(&dn, &dc); dom_buf_free((DomUndo *)db, dn); }
            g_running = 0; g_cur_flow = NULL;
            JS_FreeValue(ctx, f.handle); free(f.dec);
            continue;
        }

        if (f.session) {
            /* ATTACKER SESSION as a PREEMPTIBLE FLOW: __driveSession (bytecode) fires all handlers in seed
               order over ONE accumulating COW delta (cross-handler shared state). Run as a heap-frame flow so
               it YIELDS per-opcode like any other — run-to-completion is GONE, candidate sessions included: a
               candidate's boot-undo now lives IN the flow delta (JS_CowSeedBootInverse), so a suspend's
               JS_CowUnapply restores the post-boot baseline with no host-side bracket. On suspend the
               accumulated delta unapplies/reapplies exactly like a sync flow's, so handler A's tainted write
               still reaches B after an interleave. */
            g_cur_fn = JS_UNDEFINED;
            int sess_starter = (f.fs == NULL);
            if (sess_starter) {
                g_dec_n = f.dec_n; g_dec_ensure(g_dec_n);                 /* replay this session's decisions; new opaque branches fork siblings */
                for (int i = 0; i < g_dec_n; i++) g_dec[i] = f.dec ? f.dec[i] : 0;
                g_c = 0; cons_reset();
                if (f.candidate) boot_replay_candidate(ctx);
                JSValue gg = JS_GetGlobalObject(ctx);
                JSValue ds = JS_GetPropertyStr(ctx, gg, "__driveSession");
                f.fs = JS_FlowNew(ctx, ds, JS_UNDEFINED, 0, NULL);        /* preemptible frame for the self-hosted session loop */
                JS_FreeValue(ctx, ds); JS_FreeValue(ctx, gg);
                replay_handlers_clear(ctx);                              /* candidate boot-replay's transient listeners done (session fires g_handlers) */
                if (!f.fs) why_add(ctx, "session", "__driveSession not flow-able (alloc failure) — FATAL");   /* @WHY = crash: __driveSession is guaranteed bytecode, so NULL is a real (OOM) error to surface, never skip */
            } else {                                                     /* RESUME: reload decisions + swap in this session's OWN accumulated delta */
                g_dec_n = f.dec_n; g_dec_ensure(g_dec_n);
                for (int i = 0; i < g_dec_n; i++) g_dec[i] = f.dec ? f.dec[i] : 0;
                g_c = f.saved_c; cons_reset();
                JS_CowBufLoad(f.cow, f.cow_n, f.cow_cap); JS_CowApply(ctx); f.cow = NULL; f.cow_n = f.cow_cap = 0;
                dom_buf_load(f.dom, f.dom_n, f.dom_cap); dom_apply(); f.dom = NULL; f.dom_n = f.dom_cap = 0;
            }
            JS_SetFlowYieldHook(wfq_yield);
            JSValue out = JS_UNDEFINED;
            int st = JS_FlowResume(ctx, f.fs, &out);
            JS_SetFlowYieldHook(NULL);
            JSContext *c1; int jr; while ((jr = JS_ExecutePendingJob(g_rt, &c1)) > 0) { }
            if (jr < 0) js_std_dump_error(c1 ? c1 : ctx);
            g_running = 0; g_cur_fn = JS_UNDEFINED;
            if (st == 1) {                                               /* SUSPENDED: stash accumulated delta + re-queue (interleave with other flows) */
                g_switches++;
                JS_CowUnapply(ctx); f.cow = JS_CowBufTake(&f.cow_n, &f.cow_cap);
                dom_unapply(); f.dom = dom_buf_take(&f.dom_n, &f.dom_cap);
                f.saved_c = g_c; f.val = g_cur_val;
                if (g_dec_n > f.dec_n) {
                    signed char *nd = (signed char *)malloc((size_t)(g_dec_n > 0 ? g_dec_n : 1));
                    if (nd) { for (int i = 0; i < g_dec_n; i++) nd[i] = g_dec[i]; free(f.dec); f.dec = nd; }
                } else {
                    for (int i = 0; i < g_dec_n && f.dec; i++) f.dec[i] = g_dec[i];
                }
                f.dec_n = g_dec_n;
                g_cur_flow = NULL;
                reg_readd(ctx, f);
            } else {                                                     /* COMPLETED: revert this session's delta (boot-inverse included -> post-boot baseline) */
                JS_CowRevert(ctx); { int cn, cc; void *cb = JS_CowBufTake(&cn, &cc); JS_CowBufFree(ctx, cb, cn); }
                dom_revert(); { int dn, dc; void *db = dom_buf_take(&dn, &dc); dom_buf_free((DomUndo *)db, dn); }
                if (JS_IsException(out)) js_std_dump_error(ctx);
                JS_FreeValue(ctx, out);
                g_cur_flow = NULL;
                JS_FreeValue(ctx, f.handle); free(f.dec); free(f.candidate); free(f.vtarget);
            }
            g_candidate = NULL;
            continue;
        }

        if (f.is_boot) {
            /* BOOT FLOW: re-run boot from the PRISTINE pre-boot baseline as a FORKING starter. Cached replies
               resolve synchronously (make_response injects __body), so a reply-consuming continuation runs
               IN-LINE and its gated branches FORK with the concolic example (unreachable via the non-forking
               promise-resume). The boot-undo lives IN the flow delta (JS_CowSeedBootInverse — the SAME primitive
               candidate flows use), so JS_CowRevert restores the post-boot baseline with NO host-side bracket:
               one uniform COW-delta mechanism, and g_boot_delta stays the canonical baseline (only READ). */
            g_cur_fn = JS_UNDEFINED;
            g_dec_n = f.dec_n; g_dec_ensure(g_dec_n);
            for (int i = 0; i < g_dec_n; i++) g_dec[i] = f.dec ? f.dec[i] : 0;
            g_c = 0; cons_reset();
            JS_SetFlowYieldHook(NULL);
            if (g_boot_delta) JS_CowSeedBootInverse(ctx, g_boot_delta, g_boot_delta_n);   /* seed flow delta with boot-inverse; heap -> pre-boot (globals incl let/const deleted), RECORDED */
            g_in_boot_flow = 1;
            boot_scripts_run(ctx);                                        /* re-run boot, FORKING; cached replies resolve sync */
            { JSContext *cb; int jr; while ((jr = JS_ExecutePendingJob(g_rt, &cb)) > 0) { } if (jr < 0) js_std_dump_error(cb ? cb : ctx); }   /* drain continuations (they fork too) */
            g_in_boot_flow = 0;
            JS_CowRevert(ctx); { int cn, cc; void *cb = JS_CowBufTake(&cn, &cc); JS_CowBufFree(ctx, cb, cn); }   /* revert boot-inverse + re-run writes -> post-boot baseline */
            dom_revert(); { int dn, dc; void *db = dom_buf_take(&dn, &dc); dom_buf_free((DomUndo *)db, dn); }
            g_running = 0; g_cur_fn = JS_UNDEFINED; g_cur_flow = NULL;
            JS_FreeValue(ctx, f.handle); free(f.dec); free(f.candidate); free(f.vtarget);
            continue;
        }

        /* SYNC flow: load its per-flow scheduler state (decision vector + branch cursor). __branch
           consumes/extends g_dec + forks siblings; force-invoke with OPAQUE this+args (external input
           the tool must not concretely decide) so gates fork and computed URLs are shaped. */
        g_cur_fn = f.handle;
        g_dec_n = f.dec_n; g_dec_ensure(g_dec_n);
        for (int i = 0; i < g_dec_n; i++) g_dec[i] = f.dec ? f.dec[i] : 0;
        g_c = f.saved_c;
        cons_reset();   /* rebuild the value domain as this flow re-sees its branch conditions (starter = full; resume = from saved_c) */

        int is_starter = (f.fs == NULL);
        if (is_starter) {                           /* STARTER: fresh heap frame + empty heap/DOM deltas (both left NULL by the previous flow's exit) */
            /* @S CROSS-FLOW: a candidate flow re-runs boot with the concrete candidate pinned, so shared
               state a handler reads (window.x = location.hash set at boot) holds the candidate, not the
               baseline opaque. boot_replay_candidate SEEDS this flow's delta with the boot-inverse (heap ->
               pre-boot so a guarded init re-fires) then replays boot under the candidate — all in the flow's
               OWN delta, so a suspend/revert restores the post-boot baseline with no host-side bracket. */
            if (f.candidate) boot_replay_candidate(ctx);
            /* CLOSURE cross-flow: for a candidate flow, drive the handler boot_replay RE-CREATED (candidate
               closure), located by source identity — else the ORIGINAL f.handle (baseline closure) is driven
               and the candidate never reaches a closure-captured source. Non-candidate flows drive f.handle. */
            JSValue drive = f.handle, resolved = JS_UNDEFINED;
            if (f.candidate) { resolved = resolve_replayed_handler(ctx, f.handle); if (!JS_IsUndefined(resolved)) drive = resolved; }
            JSValue oargs[8]; for (int i = 0; i < 8; i++) oargs[i] = g_opaque;
            /* A 'message' listener's first arg is a MessageEvent whose .data is attacker-controlled
               (postMessage): drive it with the {pm} source-tagged event so a sink reaching e.data reports
               {pm} and the PoC assembler builds a postMessage-delivered PoC. */
            if (!JS_IsUndefined(g_msg_event) && is_msg_handler(drive)) oargs[0] = g_msg_event;
            /* Drive an orphan METHOD with its REAL receiver instance if one exists (this.field -> concrete
               boot value, a real example) else opaque this. Args stay opaque (external input). */
            JSValue recv = JS_FindReceiver(ctx, drive);
            JSValue this_val = JS_IsUndefined(recv) ? g_opaque : recv;
            f.fs = JS_FlowNew(ctx, drive, this_val, 8, oargs);
            JS_FreeValue(ctx, recv);
            if (!f.fs) {
                /* ASYNC/generator (or non-bytecode) orphan: not sync-preemptible — it self-suspends via
                   await/yield. Run via a plain call + drain its microtask chain to completion (the awaits
                   land emits). Preemption of these is the async-per-flow-delta task, not this path. */
                JS_SetFlowYieldHook(NULL);
                JSValue r = JS_Call(ctx, drive, g_opaque, 8, oargs);
                if (JS_IsException(r)) js_std_dump_error(ctx);
                JS_FreeValue(ctx, r);
                JSContext *c2; int jr2;
                while ((jr2 = JS_ExecutePendingJob(g_rt, &c2)) > 0) { }
                if (jr2 < 0) js_std_dump_error(c2 ? c2 : ctx);
                JS_CowRevert(ctx);                            /* discard this flow's heap writes -> baseline */
                { int cn, cc; void *cb = JS_CowBufTake(&cn, &cc); JS_CowBufFree(ctx, cb, cn); }   /* free the delta buffer, globals -> NULL */
                dom_revert();                                 /* discard this flow's DOM writes -> baseline */
                { int dn, dc; void *db = dom_buf_take(&dn, &dc); dom_buf_free((DomUndo *)db, dn); }
                JS_FreeValue(ctx, resolved); replay_handlers_clear(ctx);
                g_running = 0; g_cur_fn = JS_UNDEFINED; g_cur_flow = NULL;
                JS_FreeValue(ctx, f.handle); free(f.dec); free(f.candidate); free(f.vtarget);
                continue;
            }
            JS_FreeValue(ctx, resolved); replay_handlers_clear(ctx);   /* JS_FlowNew dup'd the drive handle; drop our resolved ref + transient replay handlers */
        }
        /* RESUME: swap in the parked flow's OWN heap COW delta (unapplied while it slept) so it sees its own
           shared-state writes again, not another flow's. Starters begin with an empty delta (globals NULL). */
        if (!is_starter) {
            JS_CowBufLoad(f.cow, f.cow_n, f.cow_cap); JS_CowApply(ctx); f.cow = NULL; f.cow_n = f.cow_cap = 0;
            dom_buf_load(f.dom, f.dom_n, f.dom_cap); dom_apply(); f.dom = NULL; f.dom_n = f.dom_cap = 0;
        }

        /* EVERY sync flow is preemptible mid heap-write (per-flow COW delta), CANDIDATE flows included: the
           candidate boot-undo lives in the flow delta (JS_CowSeedBootInverse), so a suspend's JS_CowUnapply
           restores the post-boot baseline — no host-side bracket to straddle. */
        JS_SetFlowYieldHook(wfq_yield);
        JSValue out = JS_UNDEFINED;
        int st = JS_FlowResume(ctx, f.fs, &out);
        JS_SetFlowYieldHook(NULL);
        JSContext *c1; int jr;
        while ((jr = JS_ExecutePendingJob(g_rt, &c1)) > 0) { }
        if (jr < 0) js_std_dump_error(c1 ? c1 : ctx);
        g_running = 0; g_cur_fn = JS_UNDEFINED;

        if (st == 1) {
            /* SUSPENDED: UNAPPLY this flow's heap writes (baseline restored for the next flow) and STASH its
               delta buffer; re-queue. On resume it re-applies. Interleaving of heap-writers is now sound. */
            g_switches++;   /* one flow was preempted mid-run -> a real context switch (interleave), MEASURED */
            JS_CowUnapply(ctx);
            f.cow = JS_CowBufTake(&f.cow_n, &f.cow_cap);
            dom_unapply();
            f.dom = dom_buf_take(&f.dom_n, &f.dom_cap);
            f.saved_c = g_c;
            f.val = g_cur_val;
            if (g_dec_n > f.dec_n) {                /* grew (new branch decisions taken this flow-run) */
                signed char *nd = (signed char *)malloc((size_t)(g_dec_n > 0 ? g_dec_n : 1));
                if (nd) { for (int i = 0; i < g_dec_n; i++) nd[i] = g_dec[i]; free(f.dec); f.dec = nd; }
            } else {
                for (int i = 0; i < g_dec_n && f.dec; i++) f.dec[i] = g_dec[i];
            }
            f.dec_n = g_dec_n;
            g_cur_flow = NULL;
            reg_readd(ctx, f);
        } else {
            /* COMPLETED/error: discard this flow's heap writes (restore baseline) + free its delta buffer. */
            JS_CowRevert(ctx);
            { int cn, cc; void *cb = JS_CowBufTake(&cn, &cc); JS_CowBufFree(ctx, cb, cn); }
            dom_revert();
            { int dn, dc; void *db = dom_buf_take(&dn, &dc); dom_buf_free((DomUndo *)db, dn); }
            if (JS_IsException(out)) js_std_dump_error(ctx);
            JS_FreeValue(ctx, out);
            g_cur_flow = NULL;
            JS_FreeValue(ctx, f.handle); free(f.dec); free(f.candidate); free(f.vtarget);   /* candidate owned by a completed replay flow */
        }
        g_candidate = NULL;   /* clear the replay candidate; a suspended flow re-sets it from f.candidate on resume */
    }
    JS_SetFlowYieldHook(NULL);
}

/* ── Persistent-instance protocol ─────────────────────────────────────────────────
   ONE wasm instance per page, driven in steps by the offscreen: qjs_init (build runtime + env + boot +
   seed the frontier), qjs_step (advance the ONE scheduler), qjs_teardown. This replaces the old
   re-instantiate-and-re-run-per-pass model so chunks/fromReply/frontier become ONE continuous run
   (in-place suspend/resume) instead of re-running the whole page. */
static JSContext *g_ctx = NULL;
static int g_rc = 0;

KEEP int qjs_init(const char *boot, const char *html, const char *origin,
                  const char *replies, const char *recipes)
{
    JSRuntime *rt = JS_NewRuntime();
    if (!rt) { fprintf(stderr, "@E {\"phase\":\"newruntime\"}\n"); return 1; }
    g_rt = rt;
    JS_SetMaxStackSize(rt, 4 * 1024 * 1024);
    JS_UpdateStackTop(rt);
    js_std_init_handlers(rt);
    JSContext *ctx = JS_NewContext(rt);
    if (!ctx) { fprintf(stderr, "@E {\"phase\":\"newcontext\"}\n"); JS_FreeRuntime(rt); return 1; }
    g_ctx = ctx;
    /* Reset all scheduler/frontier state so ONE wasm instance can serve many page analyses (init/run/
       teardown reused) with no cross-page bleed. The arrays themselves are reused (not re-malloc'd). */
    g_reg_n = 0; g_work = 0; g_switches = 0; g_yield_floor = -1e300; g_made_progress = 0; g_emit_total = 0; g_running = 0; g_cur_flow = NULL; g_msg_handler_n = 0;
    g_cur_orphan_idx = -1; g_dec_n = 0; g_c = 0; g_resume_mode = 0; g_park_requested = 0;
    g_pending_n = 0; g_chunk_n = 0; g_orphan_n = 0; g_dom_capture = 0;
    JS_OptaintReset(ctx);   /* clear cross-flow opaque-taint set from any prior page analysis */
    /* ENDPOINT/@S/etc. accumulators + the in-engine dedup fn — the engine builds the whole structured
       result and emits ONE @RESULT json at finalize (the host JSON.parses it; no host-side parse/identity). */
    g_endpoints = JS_NewArray(ctx); g_chunkurls = JS_NewArray(ctx);
    g_park = JS_NewArray(ctx); g_solvetasks = JS_NewArray(ctx);
    g_verified = JS_NewObject(ctx); g_enqueued = JS_NewObject(ctx);   /* @S replay: working PoCs + enqueue dedup */
    g_solve_ctx = JS_NewContext(rt);   /* fresh CLEAN realm for the @S solver's candidate eval (no forced-exec/opaque overrides) */
    /* PLATFORM BUILTINS: everything compiled here (x9, dedup, the Array/String prelude) is engine-internal, not
       page code — mark it born-executed so orphan-collection NEVER force-invokes it. Without this, an uncalled
       prelude method (e.g. Array.sort, which unlike forEach permits an undefined comparator) is orphan-driven
       with OPAQUE args and its `k<L`/`cmp?` branches fork-explode. Page bundles compile with the flag OFF. */
    JS_SetBuiltinCompile(ctx, 1);
    if (g_solve_ctx) { const char *x9 = "globalThis.X9=function(){globalThis.__f9=1};globalThis.__f9=0;";
        JSValue xr = JS_Eval(g_solve_ctx, x9, strlen(x9), "<x9>", JS_EVAL_TYPE_GLOBAL); JS_FreeValue(g_solve_ctx, xr); }   /* X9 fire-tracker for the js-sink verify */
    g_dedup_fn = JS_Eval(ctx, DEDUP_JS, strlen(DEDUP_JS), "<dedup>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(g_dedup_fn)) { js_std_dump_error(ctx); g_dedup_fn = JS_UNDEFINED; }
    { JSValue pv = JS_Eval(ctx, ARRAY_PRELUDE_JS, strlen(ARRAY_PRELUDE_JS), "<array-prelude>", JS_EVAL_TYPE_GLOBAL);
      if (JS_IsException(pv)) { fprintf(stderr, "@E {\"phase\":\"array-prelude\"}\n"); js_std_dump_error(ctx); }
      JS_FreeValue(ctx, pv); }
    JS_SetBuiltinCompile(ctx, 0);   /* page bundles are NOT builtins: their unused functions stay orphans (the moat) */
    js_std_add_helpers(ctx, 0, NULL);
    js_init_module_std(ctx, "std");
    js_init_module_os(ctx, "os");

    if (origin && origin[0]) set_origin(origin);   /* real page principal (location.origin/host) */
    if (replies && replies[0]) {                   /* fromReply table: { url -> concrete reply body } */
        JSValue t = JS_ParseJSON(ctx, replies, strlen(replies), "<replies>");
        if (JS_IsException(t)) { JSValue e = JS_GetException(ctx); JS_FreeValue(ctx, e); }
        else g_reply_table = t;
    }
    (void)recipes;                                 /* recipes are seeded in phase-2 qjs_begin(), after the host reads the bundle-id */

    JSValue g = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, g, "__emit", JS_NewCFunction(ctx, js_emit, "__emit", 1));
    JS_SetPropertyStr(ctx, g, "__fork", JS_NewCFunction(ctx, js_fork, "__fork", 2));
    JS_SetPropertyStr(ctx, g, "__yield", JS_NewCFunction(ctx, js_yield, "__yield", 0));
    JS_SetPropertyStr(ctx, g, "__branch", JS_NewCFunction(ctx, js_branch, "__branch", 0));
    JS_SetPropertyStr(ctx, g, "__isOpaque", JS_NewCFunction(ctx, js_is_opaque, "__isOpaque", 1));   /* self-hosted sort: concretize a meaningless opaque order without forking */
    JS_SetPropertyStr(ctx, g, "__opaqueExample", JS_NewCFunction(ctx, js_opaque_example, "__opaqueExample", 1));   /* self-hosted stringify: a config opaque's concrete example (else undefined) */
    JS_SetPropertyStr(ctx, g, "__opaque", JS_NewCFunction(ctx, js_opaque, "__opaque", 0));
    JS_SetPropertyStr(ctx, g, "fetch", JS_NewCFunction(ctx, js_fetch, "fetch", 2));
    JS_SetPropertyStr(ctx, g, "eval", JS_NewCFunction(ctx, js_eval, "eval", 1));   /* eval(concrete) -> forced-execute */
    /* Register the OPAQUE sentinel + the branch hook: a branch whose condition IS this object forks both
       arms via the decision-vector logic (real bundles: external input reaches OP_if as opaque). */
    JS_InitOpaqueClass(ctx);             /* register the shape-carrying opaque class */
    g_opaque = JS_NewOpaqueShaped(ctx, "{}");   /* the default opaque (shape "{}"); generic propagation dups it */
    JS_SetOpaqueMarker(g_opaque);
    JS_SetBranchHook(branch_decide);
    JS_SetGateHook(gate_collect);   /* collect strings the code tests tainted input against -> search candidates */
    JS_SetDynImportHook(host_dyn_import);   /* dynamic import() -> force-fetch the ESM chunk in place */
    JS_SetModuleLoaderFunc(rt, host_module_normalize, host_module_loader, NULL);   /* static import -> fetch+link the graph like a browser */
    /* synthetic MessageEvent for driving 'message' handlers: .data is the {pm} source (magic 2) — a
       getter so a candidate-replay flow injects the concrete payload here, exactly like location.hash. */
    g_msg_event = JS_NewObject(ctx);
    def_source(ctx, g_msg_event, "data", 2);
    /* origin/source are ATTACKER-UNCONTROLLABLE (the browser stamps e.origin to the sender's REAL origin). Tag
       origin as its own source "{origin}" so an EQ gate on it (`e.origin === 'https://trusted'`) is recognized
       as a real, un-forgeable check: a sink reached ONLY by force-passing it is NOT cross-origin exploitable
       and must not emit a PoC (solve_add suppresses on cons_fixed_value("{origin}")). */
    JS_SetPropertyStr(ctx, g_msg_event, "origin", JS_NewOpaqueSourced(ctx, "{origin}", "{origin}"));
    JS_SetPropertyStr(ctx, g_msg_event, "source", JS_DupValue(ctx, g_opaque));
    JS_SetPropertyStr(ctx, g_msg_event, "ports", JS_DupValue(ctx, g_opaque));
    JS_SetPropertyStr(ctx, g, "__driveOrphans", JS_NewCFunction(ctx, js_drive_orphans, "__driveOrphans", 0));
    JS_SetPropertyStr(ctx, g, "__sessionFns", JS_NewCFunction(ctx, js_session_fns, "__sessionFns", 0));
    JS_SetPropertyStr(ctx, g, "__sessionDrain", JS_NewCFunction(ctx, js_session_drain, "__sessionDrain", 0));
    {   /* SELF-HOSTED attacker-session loop (bytecode) — fire each [fn,event] pair over the accumulating COW
           delta, per-fn microtask drain. CALLED ONCE HERE (handlers empty -> no-op) so its bytecode is marked
           EXECUTED and JS_CollectOrphans does NOT collect it: a driven-as-orphan __driveSession would fire all
           handlers outside a session and pollute the candidate-enqueue dedup (broke xss_const's boot-replay). */
        const char *sj = "var __driveSession=function(){var fns=__sessionFns();for(var i=0;i<fns.length;i++){try{fns[i][0](fns[i][1]);}catch(e){}__sessionDrain();}};";
        JSValue sr = JS_Eval(ctx, sj, strlen(sj), "<session>", JS_EVAL_TYPE_GLOBAL); JS_FreeValue(ctx, sr);
        JSValue ds = JS_GetPropertyStr(ctx, g, "__driveSession");
        if (JS_IsFunction(ctx, ds)) { JSValue r = JS_Call(ctx, ds, JS_UNDEFINED, 0, NULL); JS_FreeValue(ctx, r); }
        JS_FreeValue(ctx, ds);
    }

    /* Minimal browser environment: window/self = globalThis; OPAQUE external-input sources (location,
       document.cookie, navigator, referrer) so a real bundle's gate on external input auto-forks without
       synthetic args; addEventListener = no-op (the handler is then a never-fired orphan, driven). This
       is the seam the real Lexbor DOM + real safe-fetch plug into during the host rewire. */
    /* Real DOM: parse the page HTML (argv[2], optional) into a live Lexbor document + register the
       Element JS class. The page's own structure/config is CONCRETE; document.* reads it. */
    {
        if (dom_init(html, html ? strlen(html) : 0) != 0)
            fprintf(stderr, "@E {\"phase\":\"dom_init\"}\n");
        JS_NewClassID(rt, &g_el_class_id);
        JSClassDef el_def = { "Element" };   /* lexbor owns the nodes; no JS finalizer */
        JS_NewClass(rt, g_el_class_id, &el_def);
        JSValue el_proto = JS_NewObject(ctx);
        el_install_methods(ctx, el_proto);
        JS_SetClassProto(ctx, g_el_class_id, el_proto);
    }

    g_handlers = JS_NewArray(ctx);
    JS_SetPropertyStr(ctx, g, "__handlers", JS_DupValue(ctx, g_handlers));   /* reachable so handlers survive to orphan-collect */
    JS_SetPropertyStr(ctx, g, "window", JS_DupValue(ctx, g));
    JS_SetPropertyStr(ctx, g, "self",   JS_DupValue(ctx, g));
    JS_SetPropertyStr(ctx, g, "globalThis", JS_DupValue(ctx, g));
    /* window.location: a getset over the location object so `location = 'javascript:..'` / `= url` (a common
       navigation shorthand) is an @S nav sink; reads return the object (location.href/.hash/.assign work). */
    { JSValue loc = make_location(ctx); JS_FreeValue(ctx, g_location); g_location = JS_DupValue(ctx, loc);
      JSAtom la = JS_NewAtom(ctx, "location");
      JS_DefinePropertyGetSet(ctx, g, la,
          JS_NewCFunction2(ctx, (JSCFunction *)js_window_location_get, "get", 0, JS_CFUNC_getter, 0),
          JS_NewCFunction2(ctx, (JSCFunction *)js_window_location_set, "set", 1, JS_CFUNC_setter, 0), JS_PROP_CONFIGURABLE);
      JS_FreeAtom(ctx, la); JS_FreeValue(ctx, loc); }
    {   /* navigator: a REAL object so sendBeacon(url) emits its endpoint, with the OPAQUE sentinel as its
           PROTOTYPE so EVERY other member (userAgent/language/clipboard/…) still falls through to opaque —
           no throw on an unstubbed method, no lost analytics endpoint. */
        JSValue nav = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, nav, "sendBeacon", JS_NewCFunction(ctx, js_send_beacon, "sendBeacon", 2));
        JS_SetPrototype(ctx, nav, g_opaque);
        JS_SetPropertyStr(ctx, g, "navigator", nav);
    }
    JS_SetPropertyStr(ctx, g, "addEventListener", JS_NewCFunction(ctx, js_add_listener, "addEventListener", 2));
    JS_SetPropertyStr(ctx, g, "removeEventListener", JS_NewCFunction(ctx, js_noop, "removeEventListener", 2));
    {
        JSValue doc = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, doc, "cookie", JS_DupValue(ctx, g_opaque));      /* external/auth input: opaque */
        JS_SetPropertyStr(ctx, doc, "referrer", JS_DupValue(ctx, g_opaque));    /* external input: opaque */
        JS_SetPropertyStr(ctx, doc, "URL", JS_NewString(ctx, g_origin));        /* page identity: CONCRETE for URL building */
        JS_SetPropertyStr(ctx, doc, "domain", JS_NewString(ctx, g_host));       /* page identity: CONCRETE */
        JS_SetPropertyStr(ctx, doc, "addEventListener", JS_NewCFunction(ctx, js_add_listener, "addEventListener", 2));
        JS_SetPropertyStr(ctx, doc, "querySelector", JS_NewCFunction(ctx, js_doc_querySelector, "querySelector", 1));   /* real Lexbor DOM */
        JS_SetPropertyStr(ctx, doc, "getElementById", JS_NewCFunction(ctx, js_doc_getElementById, "getElementById", 1));
        JS_SetPropertyStr(ctx, doc, "createElement", JS_NewCFunction(ctx, js_doc_createElement, "createElement", 1));   /* real element; appendChild intercepts <script src> */
        JS_SetPropertyStr(ctx, doc, "createRange", JS_NewCFunction(ctx, js_doc_createrange, "createRange", 0));   /* createContextualFragment -> {parsedhtml} taint */
        JS_SetPropertyStr(ctx, doc, "createTextNode", JS_NewCFunction(ctx, js_opaque_stub, "createTextNode", 1));       /* text-node stub (opaque) — non-throwing */
        JS_SetPropertyStr(ctx, doc, "querySelectorAll", JS_NewCFunction(ctx, js_doc_querySelectorAll, "querySelectorAll", 1));
        JS_SetPropertyStr(ctx, doc, "getElementsByTagName", JS_NewCFunction(ctx, js_doc_querySelectorAll, "getElementsByTagName", 1));   /* tag IS a selector */
        JS_SetPropertyStr(ctx, doc, "getElementsByClassName", JS_NewCFunction(ctx, js_doc_getByClass, "getElementsByClassName", 1));
        JS_SetPropertyStr(ctx, doc, "createDocumentFragment", JS_NewCFunction(ctx, js_opaque_stub, "createDocumentFragment", 0));   /* opaque container — non-throwing (methods -> opaque) */
        JS_SetPropertyStr(ctx, doc, "write", JS_NewCFunction(ctx, js_doc_write, "write", 1));       /* DOM edge (no-op) */
        JS_SetPropertyStr(ctx, doc, "writeln", JS_NewCFunction(ctx, js_doc_write, "writeln", 1));
        JS_SetPropertyStr(ctx, doc, "head", el_wrap(ctx, g_dom ? lxb_dom_interface_element(lxb_html_document_head_element(g_dom)) : NULL));
        JS_SetPropertyStr(ctx, doc, "body", el_wrap(ctx, g_dom ? lxb_dom_interface_element(lxb_html_document_body_element(g_dom)) : NULL));
        /* Common document members real bundles read (undefined broke `documentElement.x`, a readyState gate,
           document.location.href, `for(form of document.forms)`). documentElement = <html>; readyState is
           COMPLETE (boot ran -> a ready gate takes the ready arm / init() runs); location aliases
           window.location (getset -> `document.location = url` is still a nav @S sink); forms/scripts are
           snapshots of the shipped structure. */
        JS_SetPropertyStr(ctx, doc, "documentElement", el_wrap(ctx, dom_select_first(NULL, "html", 4)));
        JS_SetPropertyStr(ctx, doc, "readyState", JS_NewString(ctx, "complete"));
        { JSAtom a = JS_NewAtom(ctx, "location");
          JS_DefinePropertyGetSet(ctx, doc, a, JS_NewCFunction2(ctx, (JSCFunction *)js_window_location_get, "get", 0, JS_CFUNC_getter, 0),
              JS_NewCFunction2(ctx, (JSCFunction *)js_window_location_set, "set", 1, JS_CFUNC_setter, 0), JS_PROP_CONFIGURABLE);
          JS_FreeAtom(ctx, a); }
        JS_SetPropertyStr(ctx, doc, "forms", dom_select_all(ctx, NULL, "form", 4));
        JS_SetPropertyStr(ctx, doc, "scripts", dom_select_all(ctx, NULL, "script", 6));
        { JSAtom a = JS_NewAtom(ctx, "currentScript");   /* getter: the executing script, changes per inline script */
          JS_DefinePropertyGetSet(ctx, doc, a, JS_NewCFunction2(ctx, (JSCFunction *)js_doc_currentscript, "get", 0, JS_CFUNC_getter, 0), JS_UNDEFINED, JS_PROP_CONFIGURABLE);
          JS_FreeAtom(ctx, a); }
        JS_SetPropertyStr(ctx, g, "document", doc);
    }
    /* WEB COMPONENTS: constructable DOM bases so `class X extends HTMLElement {…}` DEFINES -> its lifecycle
       methods (connectedCallback etc.) become uncalled methods the orphan driver reaches -> the element's
       endpoints/sinks are learned by EXECUTION (spec), not by reading a DOM attribute. customElements.define
       is a no-op: the ctor's methods are already reachable + orphan-driven. */
    def_ctor(ctx, g, "EventTarget"); def_ctor(ctx, g, "Node"); def_ctor(ctx, g, "Element");
    def_ctor(ctx, g, "HTMLElement"); def_ctor(ctx, g, "HTMLDivElement"); def_ctor(ctx, g, "HTMLInputElement");
    def_ctor(ctx, g, "HTMLButtonElement"); def_ctor(ctx, g, "HTMLFormElement"); def_ctor(ctx, g, "HTMLAnchorElement");
    def_ctor(ctx, g, "HTMLSpanElement"); def_ctor(ctx, g, "HTMLImageElement"); def_ctor(ctx, g, "SVGElement");
    {
        JSValue ce = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, ce, "define", JS_NewCFunction(ctx, js_noop, "define", 2));
        JS_SetPropertyStr(ctx, ce, "get", JS_NewCFunction(ctx, js_opaque_stub, "get", 1));
        JS_SetPropertyStr(ctx, ce, "whenDefined", JS_NewCFunction(ctx, js_opaque_stub, "whenDefined", 1));
        JS_SetPropertyStr(ctx, ce, "upgrade", JS_NewCFunction(ctx, js_noop, "upgrade", 1));
        JS_SetPropertyStr(ctx, g, "customElements", ce);
    }
    /* COMMON BROWSER APIs real bundles call constantly — a MISSING one threw and killed the script. */
    JS_SetPropertyStr(ctx, g, "XMLHttpRequest", JS_NewCFunction2(ctx, js_xhr_ctor, "XMLHttpRequest", 0, JS_CFUNC_constructor, 0));   /* primary request mechanism -> emits @H */
    JS_SetPropertyStr(ctx, g, "DOMParser", JS_NewCFunction2(ctx, js_domparser_ctor, "DOMParser", 0, JS_CFUNC_constructor, 0));   /* parseFromString -> {parsedhtml} taint -> appendChild @S */
    { JSValue realFn = JS_GetPropertyStr(ctx, g, "Function");   /* wrap Function: new Function(attacker) is an eval-class @S sink */
      if (JS_IsFunction(ctx, realFn)) {
          JS_FreeValue(ctx, g_real_function); g_real_function = JS_DupValue(ctx, realFn);
          JSValue wrap = JS_NewCFunction2(ctx, js_function_ctor, "Function", 1, JS_CFUNC_constructor, 0);
          JSValue proto = JS_GetPropertyStr(ctx, realFn, "prototype");   /* preserve so `x instanceof Function` holds */
          if (!JS_IsUndefined(proto)) JS_SetPropertyStr(ctx, wrap, "prototype", proto); else JS_FreeValue(ctx, proto);
          JS_SetPropertyStr(ctx, g, "Function", wrap);
      }
      JS_FreeValue(ctx, realFn); }
    JS_SetPropertyStr(ctx, g, "IntersectionObserver", JS_NewCFunction2(ctx, js_observer_ctor, "IntersectionObserver", 1, JS_CFUNC_constructor, 0));
    JS_SetPropertyStr(ctx, g, "MutationObserver", JS_NewCFunction2(ctx, js_observer_ctor, "MutationObserver", 1, JS_CFUNC_constructor, 0));
    JS_SetPropertyStr(ctx, g, "ResizeObserver", JS_NewCFunction2(ctx, js_observer_ctor, "ResizeObserver", 1, JS_CFUNC_constructor, 0));
    JS_SetPropertyStr(ctx, g, "PerformanceObserver", JS_NewCFunction2(ctx, js_observer_ctor, "PerformanceObserver", 1, JS_CFUNC_constructor, 0));
    JS_SetPropertyStr(ctx, g, "WebSocket", JS_NewCFunction2(ctx, js_ws_ctor, "WebSocket", 1, JS_CFUNC_constructor, 0));         /* url endpoint emitted; send/close/addEventListener present */
    JS_SetPropertyStr(ctx, g, "EventSource", JS_NewCFunction2(ctx, js_ws_ctor, "EventSource", 1, JS_CFUNC_constructor, 0));     /* SSE: url is a GET endpoint; onmessage handler driven */
    JS_SetPropertyStr(ctx, g, "Worker", JS_NewCFunction2(ctx, js_worker_ctor, "Worker", 2, JS_CFUNC_constructor, 0));           /* worker script -> chunk (fetch+analyze); onmessage driven */
    JS_SetPropertyStr(ctx, g, "SharedWorker", JS_NewCFunction2(ctx, js_worker_ctor, "SharedWorker", 2, JS_CFUNC_constructor, 0));
    JS_SetPropertyStr(ctx, g, "MessageChannel", JS_NewCFunction2(ctx, js_msg_channel_ctor, "MessageChannel", 0, JS_CFUNC_constructor, 0));
    JS_SetPropertyStr(ctx, g, "BroadcastChannel", JS_NewCFunction2(ctx, js_broadcast_ctor, "BroadcastChannel", 1, JS_CFUNC_constructor, 0));   /* real postMessage/close/addEventListener (was a webobj stub lacking postMessage) */
    {   /* Notification: constructor + static permission / requestPermission */
        JSValue nf = JS_NewCFunction2(ctx, js_notification_ctor, "Notification", 2, JS_CFUNC_constructor, 0);
        JS_SetPropertyStr(ctx, nf, "permission", JS_NewString(ctx, "default"));
        JS_SetPropertyStr(ctx, nf, "requestPermission", JS_NewCFunction(ctx, js_notif_request_perm, "requestPermission", 0));
        JS_SetPropertyStr(ctx, g, "Notification", nf);
    }
    {   /* AbortSignal: static timeout/any/abort -> a live signal (AbortController itself is a webctor) */
        JSValue as = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, as, "timeout", JS_NewCFunction(ctx, js_abortsignal_make, "timeout", 1));
        JS_SetPropertyStr(ctx, as, "any", JS_NewCFunction(ctx, js_abortsignal_make, "any", 1));
        JS_SetPropertyStr(ctx, as, "abort", JS_NewCFunction(ctx, js_abortsignal_make, "abort", 0));
        JS_SetPropertyStr(ctx, g, "AbortSignal", as);
    }
    {   /* indexedDB: non-throwing object graph; stored values opaque (state-gated code forks) */
        JSValue idb = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, idb, "open", JS_NewCFunction(ctx, js_idb_open, "open", 2));
        JS_SetPropertyStr(ctx, idb, "deleteDatabase", JS_NewCFunction(ctx, js_idb_open, "deleteDatabase", 1));
        JS_SetPropertyStr(ctx, idb, "databases", JS_NewCFunction(ctx, js_opaque_stub, "databases", 0));
        JS_SetPropertyStr(ctx, idb, "cmp", JS_NewCFunction(ctx, js_opaque_stub, "cmp", 2));
        JS_SetPropertyStr(ctx, g, "indexedDB", idb);
    }
    JS_SetPropertyStr(ctx, g, "getComputedStyle", JS_NewCFunction(ctx, js_get_computed_style, "getComputedStyle", 1));
    JS_SetPropertyStr(ctx, g, "matchMedia", JS_NewCFunction(ctx, js_match_media, "matchMedia", 1));
    JS_SetPropertyStr(ctx, g, "Image", JS_NewCFunction2(ctx, js_media_el_ctor, "Image", 2, JS_CFUNC_constructor, 0));
    JS_SetPropertyStr(ctx, g, "Audio", JS_NewCFunction2(ctx, js_media_el_ctor, "Audio", 1, JS_CFUNC_constructor, 0));
    JS_SetPropertyStr(ctx, g, "Option", JS_NewCFunction2(ctx, js_media_el_ctor, "Option", 4, JS_CFUNC_constructor, 0));
    JS_SetPropertyStr(ctx, g, "CustomEvent", JS_NewCFunction2(ctx, js_event_ctor, "CustomEvent", 2, JS_CFUNC_constructor, 0));
    JS_SetPropertyStr(ctx, g, "Event", JS_NewCFunction2(ctx, js_event_ctor, "Event", 1, JS_CFUNC_constructor, 0));
    JS_SetPropertyStr(ctx, g, "scrollTo", JS_NewCFunction(ctx, js_noop, "scrollTo", 2));
    JS_SetPropertyStr(ctx, g, "scrollBy", JS_NewCFunction(ctx, js_noop, "scrollBy", 2));
    JS_SetPropertyStr(ctx, g, "scroll", JS_NewCFunction(ctx, js_noop, "scroll", 2));
    {   /* Intl: locale formatters (constructors) — results opaque */
        JSValue intl = JS_NewObject(ctx);
        const char *cn[] = { "NumberFormat", "DateTimeFormat", "Collator", "RelativeTimeFormat", "ListFormat", "PluralRules", "Segmenter", "DisplayNames" };
        for (int i = 0; i < 8; i++) JS_SetPropertyStr(ctx, intl, cn[i], JS_NewCFunction2(ctx, js_intl_ctor, cn[i], 0, JS_CFUNC_constructor, 0));
        JS_SetPropertyStr(ctx, g, "Intl", intl);
    }
    {   /* history: pushState/replaceState/back/forward/go are no-ops (SPA routing side effects); state opaque */
        JSValue h = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, h, "pushState", JS_NewCFunction(ctx, js_noop, "pushState", 3));
        JS_SetPropertyStr(ctx, h, "replaceState", JS_NewCFunction(ctx, js_noop, "replaceState", 3));
        JS_SetPropertyStr(ctx, h, "back", JS_NewCFunction(ctx, js_noop, "back", 0));
        JS_SetPropertyStr(ctx, h, "forward", JS_NewCFunction(ctx, js_noop, "forward", 0));
        JS_SetPropertyStr(ctx, h, "go", JS_NewCFunction(ctx, js_noop, "go", 1));
        JS_SetPropertyStr(ctx, h, "state", JS_DupValue(ctx, g_opaque));
        JS_SetPropertyStr(ctx, g, "history", h);
    }
    /* Time/random are EXTERNAL INPUT -> OPAQUE: a branch on Math.random()/Date.now() must FORK both arms
       (not take a random one), and their VALUES are shapes not fabricated concretes. This is also a REPLAY
       SOUNDNESS requirement -- non-deterministic values would shift orphan-collection order between
       sessions and make a parked flow's (orphan_idx) recipe reconstruct the wrong flow. */
    {
        JSValue mo = JS_GetPropertyStr(ctx, g, "Math");
        if (JS_IsObject(mo)) JS_SetPropertyStr(ctx, mo, "random", JS_NewCFunction(ctx, js_opaque, "random", 0));
        JS_FreeValue(ctx, mo);
        JSValue dt = JS_GetPropertyStr(ctx, g, "Date");
        if (JS_IsObject(dt)) {
            JS_SetPropertyStr(ctx, dt, "now", JS_NewCFunction(ctx, js_opaque, "now", 0));
            JSValue dp = JS_GetPropertyStr(ctx, dt, "prototype");   /* new Date().getTime()/valueOf() -> opaque too */
            if (JS_IsObject(dp)) {
                JS_SetPropertyStr(ctx, dp, "getTime", JS_NewCFunction(ctx, js_opaque, "getTime", 0));
                JS_SetPropertyStr(ctx, dp, "valueOf", JS_NewCFunction(ctx, js_opaque, "valueOf", 0));
            }
            JS_FreeValue(ctx, dp);
        }
        JS_FreeValue(ctx, dt);
        JSValue perf = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, perf, "now", JS_NewCFunction(ctx, js_opaque, "now", 0));
        JS_SetPropertyStr(ctx, g, "performance", perf);
        /* Web Crypto: a MISSING host edge (crypto was undefined -> ReferenceError killed EVERY bundle that
           mints a token/UUID/id). randomUUID = external randomness -> OPAQUE (forks branches, shapes URLs,
           replay-sound); getRandomValues fills a numeric array in place (see stub); subtle = opaque (its
           digest/encrypt results are external-derived). */
        JSValue cr = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, cr, "randomUUID", JS_NewCFunction(ctx, js_opaque, "randomUUID", 0));
        JS_SetPropertyStr(ctx, cr, "getRandomValues", JS_NewCFunction(ctx, js_crypto_getrandom, "getRandomValues", 1));
        JS_SetPropertyStr(ctx, cr, "subtle", JS_DupValue(ctx, g_opaque));
        JS_SetPropertyStr(ctx, g, "crypto", cr);
        /* Timers: a deferred callback is a FLOW in the one scheduler (see js_set_timer), not a real wait.
           Missing these made every bundle that defers init in setTimeout learn nothing. */
        JS_SetPropertyStr(ctx, g, "setTimeout", JS_NewCFunction(ctx, js_set_timer, "setTimeout", 2));
        JS_SetPropertyStr(ctx, g, "setInterval", JS_NewCFunction(ctx, js_set_timer, "setInterval", 2));
        JS_SetPropertyStr(ctx, g, "requestAnimationFrame", JS_NewCFunction(ctx, js_set_timer, "requestAnimationFrame", 1));
        JS_SetPropertyStr(ctx, g, "requestIdleCallback", JS_NewCFunction(ctx, js_set_timer, "requestIdleCallback", 1));
        JS_SetPropertyStr(ctx, g, "clearTimeout", JS_NewCFunction(ctx, js_noop, "clearTimeout", 1));
        JS_SetPropertyStr(ctx, g, "clearInterval", JS_NewCFunction(ctx, js_noop, "clearInterval", 1));
        JS_SetPropertyStr(ctx, g, "cancelAnimationFrame", JS_NewCFunction(ctx, js_noop, "cancelAnimationFrame", 1));
        JS_SetPropertyStr(ctx, g, "structuredClone", JS_NewCFunction(ctx, js_structured_clone, "structuredClone", 1));
        /* Web storage: values are external input -> opaque getItem; writes no-op. */
        for (int si = 0; si < 2; si++) {
            JSValue st = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, st, "getItem", JS_NewCFunction(ctx, js_storage_get, "getItem", 1));
            JS_SetPropertyStr(ctx, st, "setItem", JS_NewCFunction(ctx, js_noop, "setItem", 2));
            JS_SetPropertyStr(ctx, st, "removeItem", JS_NewCFunction(ctx, js_noop, "removeItem", 1));
            JS_SetPropertyStr(ctx, st, "clear", JS_NewCFunction(ctx, js_noop, "clear", 0));
            JS_SetPropertyStr(ctx, st, "key", JS_NewCFunction(ctx, js_storage_get, "key", 1));
            JS_SetPropertyStr(ctx, g, si ? "sessionStorage" : "localStorage", st);
        }
        /* URL / URLSearchParams: endpoint construction (see js_url_ctor). */
        { JSValue urlctor = JS_NewCFunction2(ctx, js_url_ctor, "URL", 2, JS_CFUNC_constructor, 0);
          JS_SetPropertyStr(ctx, urlctor, "canParse", JS_NewCFunction(ctx, js_url_canparse, "canParse", 2));   /* static URL.canParse */
          JS_SetPropertyStr(ctx, g, "URL", urlctor); }
        JS_SetPropertyStr(ctx, g, "URLSearchParams", JS_NewCFunction2(ctx, js_searchparams_ctor, "URLSearchParams", 1, JS_CFUNC_constructor, 0));
        JS_SetPropertyStr(ctx, g, "Request", JS_NewCFunction2(ctx, js_request_ctor, "Request", 2, JS_CFUNC_constructor, 0));
        /* fetch-API + encoding + misc Web objects: opaque-read / no-op-write, so a bundle that constructs
           them doesn't ReferenceError and any value read out stays opaque. */
        JS_SetPropertyStr(ctx, g, "FormData", JS_NewCFunction2(ctx, js_formdata_ctor, "FormData", 0, JS_CFUNC_constructor, 0));   /* real: records fields -> POST body params */
        JS_SetPropertyStr(ctx, g, "Headers", JS_NewCFunction2(ctx, js_headers_ctor, "Headers", 1, JS_CFUNC_constructor, 0));   /* real: records header fields -> required headers */
        const char *webctors[] = { "Response", "Blob", "File", "AbortController",
                                   "TextEncoder", "TextDecoder", "FileReader" };   /* EventSource -> js_ws_ctor; FormData -> js_formdata_ctor; BroadcastChannel -> js_broadcast_ctor */
        for (size_t wi = 0; wi < sizeof webctors / sizeof webctors[0]; wi++)
            JS_SetPropertyStr(ctx, g, webctors[wi], JS_NewCFunction2(ctx, js_webobj_ctor, webctors[wi], 1, JS_CFUNC_constructor, 0));
    }
    JS_FreeValue(ctx, g);

    if (boot) {
        /* `boot` (arg0) is the offscreen's combined-script preamble; capture+cache it in the boot delta with
           the inline (dom_run_scripts) + fetched-external (qjs_provide) scripts so a candidate flow can
           UNAPPLY to pre-boot and re-run the whole page boot under the concrete candidate. */
        JS_CowSetActive(1);
        if (boot[0]) {
            boot_script_cache(boot, strlen(boot));
            JSValue v = JS_Eval(ctx, boot, strlen(boot), "<boot>", JS_EVAL_TYPE_GLOBAL);
            if (JS_IsException(v)) { js_std_dump_error(ctx); g_rc = 1; }
            JS_FreeValue(ctx, v);
        }
        dom_run_scripts(ctx);   /* run inline scripts + REQUEST external <script src> loads (fetched in qjs_step) + bundle id */
        JS_CowSetActive(0);
        g_boot_delta = JS_CowBufTake(&g_boot_delta_n, &g_boot_delta_cap);
        JS_SetFlowLocalMark(1);   /* baseline is now fixed: every object a FLOW creates hereafter is flow-private (COW/taint skip it) */
    }
    return 0;   /* boot done; g_bundle_id is now readable via qjs_bundle_id(). Host reads it, looks up the parked
                   frontier by ORIGIN|bundle-id in IDB (its only job), then calls qjs_begin(recipes) to seed. */
}

/* The document's stable bundle IDENTITY = FNV-1a over its OWN scripts, computed by the REAL Lexbor <script>
   scan in dom_run_scripts (never a host-side regex). The host uses origin|this as the frontier key. */
KEEP unsigned qjs_bundle_id(void) { return (unsigned)g_bundle_id; }

/* Phase 2 (after qjs_init + the host's frontierGet by bundle-id): seed the frontier and fix the COW baseline.
   recipes NULL/"" -> a fresh visit (drive all orphans); non-empty -> RESUME by re-creating each parked flow,
   located by its function SOURCE hash (JS_OrphanHash), decisions replayed by the scheduler. */
KEEP void qjs_begin(const char *recipes)
{
    JSContext *ctx = g_ctx;
    if (!ctx) return;
    g_resume_mode = (recipes && recipes[0]) ? 1 : 0;
    seed_orphans(ctx);      /* normal: seed fresh orphan flows. resume: build g_orphan_buf locators only. */
    if (g_resume_mode) {
        /* RESUME: each recipe "hash,dec" re-creates a flow by LOCATING the orphan whose stable SOURCE identity
           (JS_OrphanHash) matches — robust to collection order/context. A recipe whose function is absent in
           THIS context simply doesn't match (skipped) — never drives the wrong one. */
        const char *p = recipes; int resumed = 0;
        while (*p) {
            uint32_t want = (uint32_t)strtoul(p, NULL, 10);
            const char *comma = strchr(p, ','), *semi = strchr(p, ';');
            signed char *dec = NULL; int dec_n = 0;
            if (comma && (!semi || comma < semi)) {
                const char *d = comma + 1, *end = semi ? semi : d + strlen(d);
                dec_n = (int)(end - d);
                if (dec_n > 0) { dec = (signed char *)malloc((size_t)dec_n); for (int i = 0; i < dec_n; i++) dec[i] = (d[i] == '1') ? 1 : 0; }
            }
            int found = -1;
            for (int oi = 0; oi < g_orphan_n; oi++) { if (JS_OrphanHash(ctx, g_orphan_buf[oi]) == want) { found = oi; break; } }
            if (found >= 0) {
                reg_add(ctx, JS_DupValue(ctx, g_orphan_buf[found]), 1.0, 0, dec, dec_n);
                g_reg[g_reg_n - 1].orphan_idx = found; resumed++;
            } else free(dec);
            if (!semi) break; p = semi + 1;
        }
        printf("@RESUMED %d\n", resumed); fflush(stdout);
    }
    JS_CowSetActive(1);   /* baseline = post-boot state; capture shared-state writes during flow exploration */
    g_dom_capture = 1;    /* DOM baseline is now fixed too; capture flow DOM mutations for per-flow revert */
    /* Seed ONE attacker-SESSION flow when the page has >=2 handlers — it fires them in sequence over
       accumulating shared state, the sound way to reach cross-handler sinks (source stored by A, sunk by B). */
    /* Seed ONE exploratory session if >=2 entry points (handlers OR orphans) could share state: it fires them
       in sequence over an accumulating delta so a cross-handler/cross-action opaque write reaches a later
       gate (redux/vuex login-action -> thunk, or handler A -> handler B). Forks on the resulting opaque
       gates; the WFQ starves it if the page has no real cross-flow state. */
    if (!g_resume_mode && (g_handler_n + g_orphan_n) >= 2) { reg_add(ctx, JS_UNDEFINED, 1.2, 0, NULL, 0); g_reg[g_reg_n - 1].session = 1; }
    /* BOOT AS THE FIRST FLOW: enqueue a FORKING re-run of boot so its TOP-LEVEL opaque gates are EXPLORED. The
       initial boot (dom_run_scripts) ran MONOLITHICALLY before the scheduler (g_running=0 -> branch_decide took
       the false arm on every opaque gate), so a page whose auth-gated surface sits behind a boot-level
       `if(localStorage.getItem('token'))` / cookie / `if(window.__FLAGS.admin)` gate with NO fetch would NEVER
       be surfaced. A reply also enqueues one (qjs_provide, to re-run boot with the reply synchronous); this
       covers the no-reply case. The WFQ starves it if boot has no forkable gate. */
    if (!g_resume_mode && g_boot_n > 0) reg_add_boot(ctx, NULL, 0);
}

/* The distinct pending fetch urls (newline-joined) the offscreen must safe-fetch. Static buffer. */
KEEP const char *qjs_pending(void)
{
    static char *buf = NULL; static size_t cap = 0;
    size_t need = 1;
    for (int i = 0; i < g_pending_n; i++) if (g_pending[i].url) need += strlen(g_pending[i].url) + 1;
    if (need > cap) { char *n = realloc(buf, need); if (!n) return ""; buf = n; cap = need; }
    size_t off = 0;
    for (int i = 0; i < g_pending_n; i++) {
        if (!g_pending[i].url) continue;
        int dup = 0;
        for (int j = 0; j < i; j++) if (g_pending[j].url && strcmp(g_pending[j].url, g_pending[i].url) == 0) { dup = 1; break; }
        if (dup) continue;
        size_t l = strlen(g_pending[i].url);
        memcpy(buf + off, g_pending[i].url, l); off += l; buf[off++] = '\n';
    }
    buf[off] = 0;
    return buf;
}
/* Chunk urls to fetch (as JS) + eval in place (newline-joined). Static buffer. */
KEEP const char *qjs_chunks(void)
{
    static char *buf = NULL; static size_t cap = 0;
    size_t need = 1;
    for (int i = 0; i < g_chunk_n; i++) need += strlen(g_chunk_pending[i]) + 1;
    if (need > cap) { char *n = realloc(buf, need); if (!n) return ""; buf = n; cap = need; }
    size_t off = 0;
    for (int i = 0; i < g_chunk_n; i++) { size_t l = strlen(g_chunk_pending[i]); memcpy(buf + off, g_chunk_pending[i], l); off += l; buf[off++] = '\n'; }
    buf[off] = 0;
    return buf;
}
/* Resolve every pending consume of `url` with the concrete body (empty -> opaque); cache in the reply
   table so a later response of the same url is concrete too. If `url` is a pending CHUNK, eval its body
   in place — extending the page BASELINE (cow off during eval), so its defs are permanent (not a flow
   write to be reverted) and its orphans get driven on the next step. Continuations run on the next step. */
KEEP void qjs_provide(const char *url, const char *body)
{
    JSContext *ctx = g_ctx; if (!ctx || !url) return;
    for (int i = 0; i < g_chunk_n; i++) {
        if (strcmp(g_chunk_pending[i], url) != 0) continue;
        if (body && body[0]) {
            JS_CowRevert(ctx);                                   /* to baseline (parked flows have empty delta) */
            int dsv = g_dom_capture; g_dom_capture = 0; JS_CowSetActive(0); JS_SetFlowLocalMark(0);   /* a lazy CHUNK's globals are BASELINE (shared, re-run in boot-replay), not flow-local — mark 0 while it evals */
            modsrc_put(url, body, strlen(body));                 /* available to the module loader by URL */
            dynpend_retry(ctx);                                  /* resolve any parked dynamic import() now linkable */
            if (is_moddep(url)) pendmod_retry(ctx);              /* a static-import dep OR dyn-import chunk: link in-graph (don't eval standalone -> no double side effects) */
            else eval_page_script(ctx, body, strlen(body), url, JS_DetectModule(body, strlen(body))); /* classic external script: run standalone (module vs classic by the real detector) */
            JS_CowSetActive(1); JS_SetFlowLocalMark(1); g_dom_capture = dsv;
            /* CACHE the chunk so boot-replay re-runs it under a candidate — a source stored / handler
               registered in an external chunk is then re-established with the concrete input (cross-flow
               through CHUNK state). The re-run's writes are captured in the candidate flow's own COW delta
               (creations included) and reverted, so no leak. */
            boot_script_cache(body, strlen(body));
        }
        free(g_chunk_pending[i]);
        for (int j = i; j < g_chunk_n - 1; j++) g_chunk_pending[j] = g_chunk_pending[j + 1];
        g_chunk_n--;
        return;
    }
    int has = body && body[0];
    if (has) {
        /* CACHE the reply so a re-run's make_response injects __body (r.json()/r.text() -> CONCOLIC synchronously),
           creating g_reply_table if the host seeded none. On a NEW url, enqueue a FORKING BOOT FLOW: it re-runs
           boot with this reply now synchronous, so a reply-GATED continuation forks WITH the concolic example
           (the value reaches gated fetches, not just ungated). */
        if (!JS_IsObject(g_reply_table)) { JS_FreeValue(ctx, g_reply_table); g_reply_table = JS_NewObject(ctx); }
        JSValue prev = JS_GetPropertyStr(ctx, g_reply_table, url); int is_new = !JS_IsString(prev); JS_FreeValue(ctx, prev);
        JS_SetPropertyStr(ctx, g_reply_table, url, JS_NewString(ctx, body));
        if (is_new && g_boot_n > 0) reg_add_boot(ctx, NULL, 0);
    }
    for (int i = 0; i < g_pending_n; i++) {
        if (!g_pending[i].url || strcmp(g_pending[i].url, url) != 0) continue;
        JSValue v;
        if (has) { JSValue bs = JS_NewString(ctx, body); v = reply_value(ctx, bs, g_pending[i].is_json); JS_FreeValue(ctx, bs); }
        else v = JS_DupValue(ctx, g_opaque);
        JSValue r = JS_Call(ctx, g_pending[i].rf, JS_UNDEFINED, 1, (JSValueConst *)&v); JS_FreeValue(ctx, r); JS_FreeValue(ctx, v);
        JS_FreeValue(ctx, g_pending[i].rf); free(g_pending[i].url);
        g_pending[i].rf = JS_UNDEFINED; g_pending[i].url = NULL;
    }
    int w = 0; for (int i = 0; i < g_pending_n; i++) if (g_pending[i].url) g_pending[w++] = g_pending[i];
    g_pending_n = w;
}
/* Resolve ALL remaining pending with opaque (node CLI / finalize): chains continue as shapes. */
KEEP void qjs_finalize(void)
{
    JSContext *ctx = g_ctx; if (!ctx) return;
    for (int i = 0; i < g_pending_n; i++) {
        if (!g_pending[i].url) continue;
        JSValue v = JS_DupValue(ctx, g_opaque);
        JSValue r = JS_Call(ctx, g_pending[i].rf, JS_UNDEFINED, 1, (JSValueConst *)&v); JS_FreeValue(ctx, r); JS_FreeValue(ctx, v);
        JS_FreeValue(ctx, g_pending[i].rf); free(g_pending[i].url); g_pending[i].url = NULL;
    }
    g_pending_n = 0;
    for (int i = 0; i < g_chunk_n; i++) free(g_chunk_pending[i]);   /* node CLI can't fetch chunks -> drop */
    g_chunk_n = 0;
}
/* Advance the ONE scheduler; drain microtasks. Return 1 = NEED_FETCH (flows parked awaiting a real
   reply the offscreen must provide via qjs_provide), else 0 = DONE. */
/* Host cross-document WFQ enablers (Level-1): the host ranks a live engine by its best flow's weight
   (qjs_top_weight) and sets the RUNNER-UP engine's weight as this engine's yield floor; the engine runs
   until its best flow no longer outranks that floor, then yields HOT. VALUE-driven, not a slice count.
   Floor -1e300 (default / lone engine) = run to completion. */
KEEP void qjs_set_yield_floor(double w) { g_yield_floor = w; }
/* Host RAM-pressure signal (cold-tier): when resident RAM crosses the working-set floor the host raises this,
   and the scheduler parks its remaining frontier as replay recipes to IDB and yields. RESOURCE-driven, never a
   dispatch count — with headroom it is never raised and the page runs to completion in one visit. */
KEEP void qjs_request_park(void) { g_park_requested = 1; }
KEEP double qjs_top_weight(void)     /* this engine's value-of-information = its best flow's weight (0 if idle/done) */
{
    double best = 0; int seen = 0;
    for (int i = 0; i < g_reg_n; i++) { double w = flow_weight(&g_reg[i]); if (!seen || w > best) { best = w; seen = 1; } }
    return seen ? best : 0.0;
}

KEEP int qjs_step(void)
{
    if (!g_ctx) return 0;
    g_made_progress = 0;                                 /* fresh progress guard each host visit */
    scheduler_run(g_ctx);
    js_std_loop(g_ctx);
    if (g_pending_n > 0 || g_chunk_n > 0) return 1;      /* NEED_FETCH: replies and/or chunks */
    if (g_reg_n > 0) return 2;                           /* HOT work remains (value-yielded) — host re-ranks + resumes */
    return 0;                                            /* fully explored (or parked) — done */
}

KEEP void qjs_teardown(void)
{
    JSContext *ctx = g_ctx;
    if (!ctx) return;
    qjs_finalize();   /* resolve any stragglers opaque so no promise leaks */
    /* Unresolved module/dyn-import residue -> @WHY BEFORE emit_result (so it lands in resolverErrors). */
    if (g_dynpend_n) { char rz[64]; snprintf(rz, sizeof rz, "unresolved dynamic import x%d (chunk never fetched)", g_dynpend_n); why_add(ctx, "dyn-import", rz); }
    if (g_pendmod_n) { char rz[64]; snprintf(rz, sizeof rz, "unresolved module graph x%d (dep never fetched)", g_pendmod_n); why_add(ctx, "module-link", rz); }
    emit_result(ctx);   /* dedup in-engine + emit the ONE @RESULT json (endpoints/sinks/chunks/errors/park/_emit) */
    /* Clean teardown (else JS_FreeRuntime asserts gc_obj_list non-empty): stop + revert the COW log so
       its held baseline values return to their slots, and drop the opaque marker. */
    JS_CowSetActive(0);
    JS_CowRevert(ctx);
    JS_OptaintReset(ctx);   /* release the cross-flow opaque-taint set: it js_dup()s every tainted object and is
                               GLOBAL (only reset at the NEXT qjs_begin), so a run that ends in teardown without a
                               following analysis leaks every tainted object (the reply objects) and JS_FreeRuntime
                               asserts gc_obj_list non-empty. Reset here so the final run frees cleanly too. */
    g_dom_capture = 0; dom_revert();   /* drop DOM undo log (restore baseline) before teardown */
    JS_SetOpaqueMarker(JS_UNDEFINED); JS_SetBranchHook(NULL); JS_SetGateHook(NULL);
    for (int i = 0; i < g_gate_n; i++) free(g_gate_tokens[i]);
    free(g_gate_tokens); g_gate_tokens = NULL; g_gate_n = g_gate_cap = 0;
    for (int i = 0; i < g_boot_n; i++) free(g_boot_scripts[i]);
    free(g_boot_scripts); g_boot_scripts = NULL; g_boot_n = g_boot_cap = 0;
    cons_reset(); free(g_cons); g_cons = NULL; g_cons_cap = 0;   /* free the per-flow value-domain constraint set */
    for (int i = 0; i < g_modsrc_n; i++) { free(g_modsrc[i].url); free(g_modsrc[i].src); }
    free(g_modsrc); g_modsrc = NULL; g_modsrc_n = g_modsrc_cap = 0;
    for (int i = 0; i < g_moddep_n; i++) free(g_moddep[i]);
    free(g_moddep); g_moddep = NULL; g_moddep_n = g_moddep_cap = 0;
    for (int i = 0; i < g_dynpend_n; i++) { JS_FreeValue(ctx, g_dynpend[i].resolve); free(g_dynpend[i].spec); }  /* abandon the promise (teardown) — no JS run post-revert */
    free(g_dynpend); g_dynpend = NULL; g_dynpend_n = g_dynpend_cap = 0;
    for (int i = 0; i < g_pendmod_n; i++) free(g_pendmod[i].src);
    free(g_pendmod); g_pendmod = NULL; g_pendmod_n = g_pendmod_cap = 0; g_modseq = 0;
    if (g_boot_delta) JS_CowBufFree(ctx, g_boot_delta, g_boot_delta_n);   /* free the stashed boot delta */
    g_boot_delta = NULL; g_boot_delta_n = g_boot_delta_cap = 0;
    for (int i = 0; i < g_attr_shadow_n; i++) { JS_FreeValue(ctx, g_attr_shadow[i].opaque); free(g_attr_shadow[i].name); }
    free(g_attr_shadow); g_attr_shadow = NULL; g_attr_shadow_n = g_attr_shadow_cap = 0;
    replay_handlers_clear(ctx); free(g_replay_handlers); g_replay_handlers = NULL; g_replay_handler_cap = 0;
    JS_FreeValue(ctx, g_opaque); g_opaque = JS_UNDEFINED;
    JS_FreeValue(ctx, g_reply_table); g_reply_table = JS_UNDEFINED;
    JS_FreeValue(ctx, g_endpoints); g_endpoints = JS_UNDEFINED;
    JS_FreeValue(ctx, g_chunkurls); g_chunkurls = JS_UNDEFINED;
    JS_FreeValue(ctx, g_park); g_park = JS_UNDEFINED;
    JS_FreeValue(ctx, g_solvetasks); g_solvetasks = JS_UNDEFINED;
    JS_FreeValue(ctx, g_verified); g_verified = JS_UNDEFINED;
    JS_FreeValue(ctx, g_enqueued); g_enqueued = JS_UNDEFINED;
    JS_FreeValue(ctx, g_dedup_fn); g_dedup_fn = JS_UNDEFINED;
    JS_FreeValue(ctx, g_location); g_location = JS_UNDEFINED;
    for (int i = 0; i < g_orphan_n; i++) JS_FreeValue(ctx, g_orphan_buf[i]);
    g_orphan_n = 0;
    JS_FreeValue(ctx, g_handlers); g_handlers = JS_UNDEFINED;
    JS_FreeValue(ctx, g_msg_event); g_msg_event = JS_UNDEFINED; g_msg_handler_n = 0;
    js_std_free_handlers(g_rt);
    JS_FreeValue(ctx, g_real_function); g_real_function = JS_UNDEFINED;
    if (g_solve_ctx) { JS_FreeContext(g_solve_ctx); g_solve_ctx = NULL; }   /* free the solver realm before its runtime */
    JS_FreeContext(ctx);
    JS_FreeRuntime(g_rt);
    g_ctx = NULL; g_rt = NULL;
    fflush(stdout);
}

/* node CLI entry (design-narrowing only): drive the persistent protocol once. */
int main(int argc, char **argv)
{
    const char *boot    = (argc > 1) ? argv[1] : NULL;
    const char *html    = (argc > 2) ? argv[2] : NULL;
    const char *origin  = (argc > 3) ? argv[3] : NULL;
    const char *replies = (argc > 4) ? argv[4] : NULL;
    const char *recipes = (argc > 5) ? argv[5] : NULL;
    if (qjs_init(boot, html, origin, replies, recipes) != 0) return 1;
    qjs_begin(recipes);   /* phase 2: seed the frontier (fresh or resume) + fix the COW baseline */
    if (boot) { while (qjs_step() == 1) qjs_finalize(); }   /* node CLI has no network -> resolve pending opaque (shapes) */
    qjs_teardown();
    return g_rc;
}
