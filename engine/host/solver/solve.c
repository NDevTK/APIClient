/* @S SOLVER (forced execution, not taint tracing) — the NOVEL contribution, its own component.
 *
 * Each SINK reached by external input is recorded (solve_add); for an opaque flow it spawns candidate-replay
 * flows that drive concrete breakout payloads through the REAL code (filters/encoders/gates) and a finding is
 * emitted IFF the candidate BROKE OUT and FIRED (x9_fires) — the emitted entry IS a working, replay-verified
 * PoC. Candidate construction is context-derived (construct_ctx_breakout) + gate-token/envelope guided, never a
 * fixed table for HTML. Scheduler-COUPLED (it enqueues flows) but no longer scheduler-RESIDENT: it reaches the
 * registry only through solver/scheduler.h (reg_add/spawn_async_sibling + the running-flow context), so it is a
 * component with a narrow contract, not code lumped into the engine entry file. See solve.h. */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "solver/solve.h"        /* solve_add + the @S entry/lifecycle this TU exports */
#include "solver/scheduler.h"    /* Flow + reg_add/spawn_async_sibling + the running-flow decision context */
#include "solver/constraints.h"  /* Cons/g_cons + cons_fixed_value + g_origin_req (per-flow value domain) */
#include "solver/solve_html.h"   /* solve_ctx_detect/is_rawtext_tag/elem_fire_event/x9_fires (HTML breakout + firing) */
#include "solver/solve_js.h"     /* solve_js_ctx_detect — the JS lexical-context detector (derived JS breakout) */
#include "solver/envelope.h"     /* envelope_build/detect/handles_token (structured-source delivery) */
#include "core/frame/csp.h"      /* effective CSP -> a PoC's cspBlocked/cspBypass reproduction envelope */
#include "core/dom/dom_select.h" /* the per-flow Lexbor document the PoC's context is re-checked against */
#include "core/trustedtypes/trusted_types.h"   /* enforced Trusted Types policy -> a PoC's TT reproduction envelope */

extern lxb_html_document_t *g_dom;   /* the live parsed document (main.c) — the @S emitter reads it for the CSP nonce scan */

/* ── @S SOLVER (forced execution, not taint tracing) ─────────────────────────────
   Each SINK reached by external input is collected as a task {sink, ctx, expr} — expr is the evaluable
   transform chain with a {source} hole. At finalize the solver substitutes candidate breakout payloads
   into the hole and RUNS THE REAL CHAIN in a CLEAN JS REALM (g_solve_ctx — a fresh context with real
   eval/String methods, no forced-exec/opaque overrides). A candidate whose payload survives into an
   EXECUTABLE position after the real transforms IS the PoC (verified because the real filters ran); if
   none survives, the flow is PROVEN safe for the tried payloads. No taint label, no chain inversion. */
static JSValue g_solvetasks = JS_UNDEFINED;   /* JS array of {sink, ctx, expr} (the finalize expr-eval pre-filter) */
static JSValue g_verified = JS_UNDEFINED;     /* "sink|ctx" -> concrete PoC candidate that a REPLAY flow drove through the real code+branches to the sink where it broke out. The ONLY @S output: a working PoC is self-verifying; absence is NOT a safe verdict, only search-not-yet-solved. */
static JSValue g_reached = JS_UNDEFINED;      /* "sink|ctx" -> 1: a concrete candidate REACHED this sink but did not break out — OBSERVED fitness (feasible path, breakout unsolved), a near-miss stronger than never-reached */
static JSValue g_enqueued = JS_UNDEFINED;     /* "orphanidx|sink|ctx" -> 1: candidate-replay flows already enqueued for this sink (dedup, not truncation) */
JSContext *g_solve_ctx = NULL;         /* fresh realm for clean candidate eval */
/* url/js sinks: the breakout VECTOR is fixed by the context itself — a URL sink executes a `javascript:`
   scheme, an eval/Function/setTimeout sink executes a JS-string/expression escape — so these are the
   context-determined bases (not an HTML-payload guess-list; the HTML-context breakout is CONSTRUCTED from the
   observed sink structure by construct_ctx_breakout). x9_fires proves each actually executes. */
static const char *CAND_URL[]  = { "javascript:X9", "javascript:X9//", NULL };   /* URL sink: the vector IS javascript: — one fixed context, nothing to derive */
static const char *CAND_SCRIPTURL[] = { "//X9/x.js", "https://X9/x.js", NULL };   /* <script src>: attacker-host origins — one fixed context */
static const char **cand_set(const char *sc) {   /* only url/scripturl have a single fixed context; js is DERIVED via construct_js_breakout */
    if (sc && strcmp(sc, "scripturl") == 0) return CAND_SCRIPTURL;
    return CAND_URL;
}
/* GATE TOKENS: concrete strings the REAL code tested tainted input against (startsWith('cmd:'), =='x'…).
   The forced-exec search prefixes/suffixes each base payload with them so a gated sink is solved by the
   concrete input the gate requires — no symbolic solver, just what the code itself demanded. Deduped
   (identical token -> identical candidates, pure waste); no length/count bound (a gate may require a long
   exact prefix, and the WFQ starves low-value search flows rather than a cap dropping them). */
static char **g_gate_tokens = NULL; static int g_gate_n = 0, g_gate_cap = 0;
void gate_collect(const char *token, const char *src) {
    if (!token || !token[0]) return;
    /* An {origin}/{source} constraint bounds the ATTACKER'S ORIGIN (`e.origin.indexOf('trusted')`), NOT the
       data payload — feeding it to the data-candidate search would build nonsense candidates like
       'trusted<img..>'. Drop it here (the data search is unaffected; the same string, if ALSO a real data gate
       elsewhere, is collected there with a data src). Per-flow origin-constraint SURFACING for delivery is a
       separate concern needing per-flow attribution. */
    /* origin/source constraints never feed the DATA-payload candidate search (that would build nonsense like
       'trusted<img..>'). But the attacker DOES control origin (by registering a domain), so a FORGEABLE origin
       string-check — endsWith('victim.com') (NON-dotted: https://attackervictim.com passes), includes,
       startsWith, indexOf — is a solvable DELIVERY constraint: record it as the required-origin so the reported
       PoC is COMPLETE. The UNFORGEABLE gates (=== exact, endsWith('.subdomain')) suppress the whole finding via
       cons_fixed_value / the EQ pin, so they never reach a reported sink and are skipped here. */
    if (src && (strncmp(src, "{origin}", 8) == 0 || strncmp(src, "{source}", 8) == 0)) {
        const char *m = strrchr(src, '.');   /* the method: "{origin}.endsWith" -> "endsWith" */
        if (m && m[1] && !(strcmp(m + 1, "endsWith") == 0 && token[0] == '.')) {   /* skip the unforgeable dotted suffix */
            /* SOLVE the origin: construct a concrete VALID origin the attacker registers that satisfies the
               check (origin is always scheme://host, so the bypass must be a well-formed origin). */
            const char *method = m + 1; char bypass[160];
            if (strcmp(method, "startsWith") == 0)         snprintf(bypass, sizeof bypass, "%s.attacker.example", token);           /* startsWith('https://victim') -> https://victim.attacker.example */
            else if (strcmp(method, "endsWith") == 0)      snprintf(bypass, sizeof bypass, "https://attacker%s", token);            /* endsWith('victim.com') -> https://attackervictim.com */
            else                                           snprintf(bypass, sizeof bypass, "https://%s.attacker.example", token);   /* includes/indexOf('victim.com') -> https://victim.com.attacker.example */
            snprintf(g_origin_req, sizeof g_origin_req, "%s (a registered attacker origin that passes %s('%s'))", bypass, method, token);
        }
        return;
    }
    for (int i = 0; i < g_gate_n; i++) if (strcmp(g_gate_tokens[i], token) == 0) return;
    if (g_gate_n >= g_gate_cap) { int nc = g_gate_cap ? g_gate_cap * 2 : 32;
        char **n = realloc(g_gate_tokens, (size_t)nc * sizeof(char *)); if (!n) return; g_gate_tokens = n; g_gate_cap = nc; }
    g_gate_tokens[g_gate_n++] = strdup(token);
}
/* enqueue an @S REPLAY flow: re-run the CURRENT orphan with `cand` as the concrete source, driven by the
   ONE scheduler (high initial value so the search runs soon; transient — never parked as a recipe). */
/* fitness = how many OBSERVED gate tokens this candidate already embeds (a distance-to-firing estimate the WFQ
   reads): a candidate carrying the concrete strings the real code tested tainted input against is CLOSER to
   surviving the gates en route to the sink, so it runs sooner. ORDER-only (never drops a candidate) — the seed
   of the distance-directed search CLAUDE.md mandates over flat enumerate-and-verify. */
static void reg_add_cand(JSContext *ctx, JSValueConst fn, const char *cand_in, const char *target, double fitness) {
    double w = 2.0 + fitness;
    char *env = envelope_build(ctx, cand_in);   /* structured-source sink -> deliver the destructuring envelope (JSON/query/delim) */
    const char *cand = env ? env : cand_in;
    if (g_in_session) {   /* sink reached inside a session -> a candidate SESSION flow re-fires ALL handlers with the candidate (cross-handler verify) */
        signed char *sdec = NULL;   /* inherit THIS session's decision vector so the candidate replays the SAME arms that reached the sink (an exploratory session now forks) */
        if (g_dec_n > 0) { sdec = (signed char *)malloc((size_t)g_dec_n); if (sdec) for (int i = 0; i < g_dec_n; i++) sdec[i] = g_dec[i]; }
        Flow *f = reg_add(ctx, JS_UNDEFINED, w, sdec, sdec ? g_dec_n : 0);   /* reg_add takes sdec ownership + never fails (OOM aborts) */
        f->candidate = strdup(cand); f->session = 1;
        free(env);
        return;   /* a session verifies MANY sinks -> not tagged with one vtarget */
    }
    if (g_cur_flow && g_cur_flow->is_async) {   /* ASYNC-FLOW sink: the source was read BEFORE an await and
        delivered to this continuation via the promise, so re-driving the continuation never re-reads it. Re-run
        the async RECIPE (func+args) with the candidate + this flow's decision vector (branch+await outcomes) so
        the source getter returns the candidate and it flows THROUGH the await to the sink, VERIFIED — the
        boot-time/handler-time async XSS (getFile().text()->innerHTML). Mirrors the sibling-fork taxonomy (~1134):
        an async sink forks the async recipe, never a bare fn re-drive. */
        signed char *adec = NULL;
        if (g_dec_n > 0) { adec = (signed char *)malloc((size_t)g_dec_n); if (adec) for (int i = 0; i < g_dec_n; i++) adec[i] = g_dec[i]; }
        Flow *sib = spawn_async_sibling(ctx, g_cur_flow, adec, adec ? g_dec_n : 0);   /* transfers adec ownership */
        if (sib) sib->candidate = strdup(cand);
        free(env);
        return;
    }
    if (JS_IsUndefined(fn)) { free(env); return; }
    { Flow *f = reg_add(ctx, JS_DupValue(ctx, fn), w, NULL, 0);
      f->candidate = strdup(cand);
      f->orphan_idx = g_cur_orphan_idx;
      if (target) f->vtarget = strdup(target); }
    free(env);
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
/* ── CONTEXT-AWARE @S CONSTRUCTION (the frontier: derive the breakout, don't pick from a fixed table) ──
   The sink SHAPE encodes the literal HTML around the {source} hole (`<textarea>{hash}</textarea>`). PARSE it
   with the REAL browser parser, find the hole's context, and CONSTRUCT the minimal escape — the RCDATA /
   RAWTEXT (<textarea>/<title>/<style>/<xmp>/<iframe>/<noembed>/<noframes>) and COMMENT contexts a flat
   candidate list can NEVER reach, because their breakout is the CLOSING token (</textarea>, -->) which is
   knowable ONLY from the surrounding structure. */
/* Emit ONE candidate + its GATE-satisfying variants: each observed gate token as a PREFIX and a SUFFIX, and
   an adjacent-pair for correlated gates (startsWith('a')&&endsWith('b')) — the concrete input the REAL code
   demanded, so a gated sink is reached. ONE home for both the constructed HTML-context candidates and the
   url/js base candidates; x9_fires proves each actually executes. */
static void emit_cand(JSContext *ctx, JSValueConst hitfn, const char *cand, const char *vt) {
    reg_add_cand(ctx, hitfn, cand, vt, 0.0);   /* bare base: satisfies no observed gate -> lowest fitness */
    size_t lc = strlen(cand);
    for (int g = 0; g < g_gate_n; g++) {
        if (envelope_handles_token(g_gate_tokens[g])) continue;   /* sibling EQ gate already in the query envelope -> don't double-place */
        size_t lt = strlen(g_gate_tokens[g]);
        char *pre = malloc(lt + lc + 1), *suf = malloc(lt + lc + 1);
        if (pre) { memcpy(pre, g_gate_tokens[g], lt); memcpy(pre + lt, cand, lc + 1); reg_add_cand(ctx, hitfn, pre, vt, 1.0); free(pre); }   /* embeds 1 gate token -> closer */
        if (suf) { memcpy(suf, cand, lc); memcpy(suf + lc, g_gate_tokens[g], lt + 1); reg_add_cand(ctx, hitfn, suf, vt, 1.0); free(suf); }
    }
    for (int g = 0; g + 1 < g_gate_n; g++) {
        if (envelope_handles_token(g_gate_tokens[g]) || envelope_handles_token(g_gate_tokens[g + 1])) continue;
        size_t l0 = strlen(g_gate_tokens[g]), l1 = strlen(g_gate_tokens[g + 1]), lc2 = strlen(cand);
        char *comb = malloc(l0 + lc2 + l1 + 1);
        if (comb) { memcpy(comb, g_gate_tokens[g], l0); memcpy(comb + l0, cand, lc2); memcpy(comb + l0 + lc2, g_gate_tokens[g + 1], l1 + 1); reg_add_cand(ctx, hitfn, comb, vt, 2.0); free(comb); }   /* embeds 2 correlated tokens -> closest */
    }
}
/* CONSTRUCT the @S breakout for an HTML-context sink FROM THE OBSERVED SINK STRUCTURE — never a fixed list.
   The sink's OWN output shape (`<img src="{}">`, `<!--{}-->`, `<textarea>{}`, or bare `{}`) is parsed by the
   REAL Lexbor parser to locate the hole's parse context, and the char immediately before the hole gives the
   quoting; the minimal ESCAPE into an executable position is DERIVED from that context, and the firing VECTOR
   is a browser-semantic AUTO-FIRING element (`<svg onload>`/`<img onerror>`), not a guessed payload. x9_fires
   then proves each actually executes in this exact context. */
static void construct_ctx_breakout(JSContext *ctx, const char *shape, JSValueConst hitfn, const char *vt) {
    if (!shape || !strchr(shape, '{')) { emit_cand(ctx, hitfn, "<svg onload=X9>", vt); return; }   /* whole output IS the input -> HTML-text context */
    struct ctx_probe cp = { 0 };
    solve_ctx_detect(shape, &cp);   /* the REAL Lexbor parse locates the hole's context (solve_html.c) */
    if (cp.is_comment) { emit_cand(ctx, hitfn, "--><svg onload=X9>", vt); return; }   /* inside <!-- --> : close the comment first */
    if (cp.found && is_rawtext_tag(cp.tag)) { char c[48]; snprintf(c, sizeof c, "</%s><svg onload=X9>", cp.tag); emit_cand(ctx, hitfn, c, vt); return; }   /* rawtext element: close it */
    /* Derive the breakout from the REAL parse FACTS, not a per-context payload guess. The quote to close comes
       from the sink output (the char Lexbor's attribute value was wrapped in); the firing VECTOR for a `<`-free
       injection comes from the ELEMENT Lexbor said the hole sits in (elem_fire_event). */
    const char *hole = strchr(shape, '{'); char q = (hole && hole > shape) ? hole[-1] : 0;
    const char *qs = (q == '"') ? "\"" : (q == '\'') ? "'" : "";
    if (cp.is_attr) {
        char b[96];
        snprintf(b, sizeof b, "%s><svg onload=X9>", qs); emit_cand(ctx, hitfn, b, vt);   /* TAG-injection: close the quote+tag, inject a known auto-firing element (uses `<`) */
        const char *ev = elem_fire_event(cp.tag);   /* the element's OWN auto-firing event — the `<`-free vector when `<` is filtered */
        if (ev) {   /* add a handler to the element the hole already sits in: a bad value breaks its resource so the event fires; `y=` swallows the template's closing quote */
            if (qs[0]) snprintf(b, sizeof b, "x%s %s=X9 y=%s", qs, ev, qs);
            else       snprintf(b, sizeof b, "x %s=X9 ", ev);   /* unquoted attribute */
            emit_cand(ctx, hitfn, b, vt);
        }
        /* UNIVERSAL `<`-free vector: an INTERACTION handler (onmouseover) fires on ANY element — no auto-firing
           event required — so it covers a hole in a NON-resource element (`<b title="…">`, where ev is NULL). The
           canonical bypass of a filter that entity-encodes `<` but leaves `"`: break the quoted attribute and
           inject a handler with no `<`. Interaction-required, tried AFTER auto-firing vectors (verified-dedup
           prefers an auto-firing one when it fires). */
        if (qs[0]) snprintf(b, sizeof b, "x%s onmouseover=X9 y=%s", qs, qs);
        else       snprintf(b, sizeof b, "x onmouseover=X9 ");
        emit_cand(ctx, hitfn, b, vt);
        return;
    }
    /* HTML-text context (or the probe couldn't place it): a firing element IS the breakout. */
    emit_cand(ctx, hitfn, "<svg onload=X9>", vt);
    emit_cand(ctx, hitfn, "<img src=x onerror=X9>", vt);
}
/* CONSTRUCT the @S breakout for a JS-context sink (eval/Function/setTimeout(string)) FROM THE DERIVED LEXICAL
   CONTEXT — the JS-side analogue of construct_ctx_breakout. solve_js_ctx_detect (solve_js.c) locates whether the
   hole sits in an expression, a '/"/`-string, or a comment, and the MINIMAL escape is DERIVED from that context
   (close the exact delimiter + run), not tried from a fixed all-contexts list. Every candidate still funnels
   through emit_cand -> gate-token variants + solve_broke's eval-firing verify, so a mis-detection is a miss, never
   a false positive. */
static void construct_js_breakout(JSContext *ctx, const char *shape, JSValueConst hitfn, const char *vt) {
    switch (solve_js_ctx_detect(shape)) {
    case JHC_SQ:            emit_cand(ctx, hitfn, "';X9();//", vt); break;
    case JHC_DQ:            emit_cand(ctx, hitfn, "\";X9();//", vt); break;
    case JHC_TEMPLATE:      emit_cand(ctx, hitfn, "${X9()}", vt);      /* interpolation fires with no delimiter char */
                            emit_cand(ctx, hitfn, "`;X9();//", vt); break;   /* or close the template literal */
    case JHC_LINE_COMMENT:  emit_cand(ctx, hitfn, "\n;X9();//", vt); break;   /* newline ends the // comment */
    case JHC_BLOCK_COMMENT: emit_cand(ctx, hitfn, "*/;X9();//", vt); break;   /* close the block comment first */
    case JHC_EXPR: default: emit_cand(ctx, hitfn, "1;X9();//", vt);           /* statement position */
                            emit_cand(ctx, hitfn, ");X9();//", vt); break;    /* or close an enclosing call: f(<hole>) */
    }
}
/* Sink reached. DUAL-MODE:
   - REPLAY flow (g_candidate set): `val` is the CONCRETE transformed candidate that ran through the REAL
     code+branches to get here. If it breaks out, this PoC is PATH+BREAKOUT verified (reachability proven by
     the real branches) -> record it under "sink|ctx".
   - NORMAL flow (opaque val): record the finalize task (pre-filter/proven-safe display) AND enqueue
     candidate-replay flows once per (orphan,sink,ctx) into the ONE scheduler. */
void solve_add(JSContext *ctx, const char *sink, const char *sctx, JSValueConst val) {
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
        JSValue exv = JS_IsConcolic(val) ? JS_ConcolicExample(ctx, val) : JS_UNDEFINED;
        const char *cv = !JS_IsUndefined(exv) ? JS_ToCString(ctx, exv) : JS_ToCString(ctx, val);
        JS_FreeValue(ctx, exv);
        char key[300]; snprintf(key, sizeof key, "%s|%s", sink, sctx);
        if (cv && solve_broke(sctx, cv) && JS_IsObject(g_verified)) {
            JS_SetPropertyStr(ctx, g_verified, key, JS_NewString(ctx, g_candidate));
        } else if (JS_IsObject(g_reached)) {
            /* OBSERVED FITNESS + MUTATION: a CONCRETE candidate drove the real code+branches to this sink but did
               NOT break out — the PATH is concretely feasible (gates solved, sink reached), so a FILTER defeated
               the breakout, not the reachability. That measured near-miss (distinct from the opaque flow forking
               here) DIRECTS the search: mutate the INPUT with filter-evasion OPERATORS (case-flip, space->slash)
               and re-drive — a sanitizer matching only lowercase tags / spaced attributes is broken by the
               variant. ONCE per sink (dedup on g_reached), and only for a non-enveloped sink where g_candidate is
               the pure breakout (an envelope's gate parts must not be case-mutated or the gate stops passing). */
            JSValue prev = JS_GetPropertyStr(ctx, g_reached, key);
            int first = JS_IsUndefined(prev); JS_FreeValue(ctx, prev);
            JS_SetPropertyStr(ctx, g_reached, key, JS_NewInt32(ctx, 1));
            /* Mutate ONLY the breakout — the chars inside a <...> tag span — with filter-evasion operators
               (case-flip, space->slash). An envelope's gate parts (query "mode=preview", JSON field names, split
               parts) contain no '<', so they stay intact and the gate still passes; only the payload tag mutates.
               The result is already delivery-shaped (a mutated envelope), so enqueue it DIRECT (reg_add, not
               reg_add_cand — no re-envelope). Once per sink; HTML context; outside a session. */
            if (first && !g_in_session && g_candidate
                && (strcmp(sctx, "html") == 0 || strcmp(sctx, "htmls") == 0)) {
                JSValueConst hitfn = JS_CurrentScriptFn(ctx);
                if (!JS_IsUndefined(hitfn)) {
                    size_t n = strlen(g_candidate);
                    for (int pass = 0; pass < 2; pass++) {   /* 0 = case-flip, 1 = space->slash (both WITHIN tag spans only) */
                        char *m = malloc(n + 1); if (!m) continue;
                        int intag = 0;
                        for (size_t i = 0; i < n; i++) { char c = g_candidate[i], o = c;
                            if (c == '<') intag = 1;
                            if (intag) { if (pass == 0 && c >= 'a' && c <= 'z') o = (char)(c - 32); else if (pass == 1 && c == ' ') o = '/'; }
                            if (c == '>') intag = 0;
                            m[i] = o; }
                        m[n] = 0;
                        if (strcmp(m, g_candidate)) {
                            Flow *f = reg_add(ctx, JS_DupValue(ctx, hitfn), 3.0, NULL, 0);
                            f->candidate = strdup(m);   /* already delivery-shaped: enqueue DIRECT (no re-envelope) */
                            f->orphan_idx = g_cur_orphan_idx;
                            f->vtarget = strdup(key);
                        }
                        free(m);
                    }
                }
            }
        }
        if (cv) JS_FreeCString(ctx, cv);
        return;
    }
    if (!JS_IsConcolic(val) || !JS_IsArray(g_solvetasks)) return;
    const char *shape = JS_ConcolicShapeC(val);   /* @H-style display: which source(s) reach this sink, transforms flattened */
    JSValue t = JS_NewObject(ctx);
    JS_CowExempt(t);   /* the task is HOST analysis state written during a flow (often the document flow); its
                          property writes must NOT ride that flow's COW delta, else they are PARKED with it and the
                          task reads back undefined at the teardown emit — exempt it exactly like the ledgers. */
    JS_SetPropertyStr(ctx, t, "sink", JS_NewString(ctx, sink));
    JS_SetPropertyStr(ctx, t, "ctx", JS_NewString(ctx, sctx));
    JS_SetPropertyStr(ctx, t, "expr", JS_NewString(ctx, shape ? shape : "{}"));
    if (g_origin_req[0]) JS_SetPropertyStr(ctx, t, "requiredOrigin", JS_NewString(ctx, g_origin_req));   /* forgeable origin gate on this path -> the PoC's delivery origin */
    envelope_detect(ctx, val);   /* fill the structured-source descriptor (JSON field / query param / split index) -> envelope.c */
    { const char *sp = JS_ConcolicSrcC(val);   /* @S structured delivery: the source LEAF path ("{pm}.html"), for srcpath + gate-field merge */
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
       concrete candidate. NOT a seen-set: the enqueue ledger stores the GATE-TOKEN COUNT at emit time and
       RE-ENQUEUES when it has GROWN — a later path through the SAME sink that discovers a new filter token
       (startsWith/indexOf/==) the candidate must satisfy gets fresh candidates carrying it, instead of being
       truncated out. Re-emitted duplicates emit nothing new and are WFQ-starved, so this never truncates work. */
    /* The function that reached this sink. JS_CurrentScriptFn is now authoritative for EVERY flow — including an
       async continuation resumed from a heap frame off the C stack (it consults the flow's tracked top frame) —
       so there is no longer a fallback to the scheduler's flow handle here. reg_add_cand's is_async branch still
       re-runs the async RECIPE so the source re-reads through the await. */
    JSValueConst hitfn = JS_CurrentScriptFn(ctx);
    JSValueConst keyfn = hitfn;
    if (JS_IsObject(g_enqueued) && !JS_IsUndefined(keyfn)) {
        char ek[320]; snprintf(ek, sizeof ek, "%u|%s|%s", JS_OrphanHash(ctx, keyfn), sink, sctx);
        JSValue e = JS_GetPropertyStr(ctx, g_enqueued, ek);
        int32_t prev_gate = -1; if (JS_IsNumber(e)) JS_ToInt32(ctx, &prev_gate, e);   /* token count at last emit, or -1 = never */
        JS_FreeValue(ctx, e);
        if (prev_gate < g_gate_n) {   /* never enqueued, OR new gate tokens learned since -> (re)enqueue with them */
            JS_SetPropertyStr(ctx, g_enqueued, ek, JS_NewInt32(ctx, g_gate_n));
            char vt[300]; snprintf(vt, sizeof vt, "%s|%s", sink, sctx);   /* the sink|ctx these candidates verify -> skip once one breaks out */
            /* HTML-context sinks: CONSTRUCT the breakout from the observed sink STRUCTURE (parse context +
               quoting), never a fixed HTML payload list. url/js sinks: the vector is itself context-fixed
               (javascript: scheme for a URL sink, a JS-string/expression escape for eval/Function/setTimeout),
               so drive those bases. Both funnel through emit_cand -> gate-token variants + firing verify. */
            if (sctx && (strcmp(sctx, "html") == 0 || strcmp(sctx, "htmls") == 0)) {
                construct_ctx_breakout(ctx, shape, hitfn, vt);            /* HTML: derived from the Lexbor parse context */
            } else if (sctx && strcmp(sctx, "js") == 0) {
                construct_js_breakout(ctx, shape, hitfn, vt);            /* JS: derived from the lexical context (solve_js.c) */
            } else {
                const char **cands = cand_set(sctx);                    /* url/scripturl: one fixed context each */
                for (int i = 0; cands[i]; i++) emit_cand(ctx, hitfn, cands[i], vt);
            }
        }
    }
}
/* build securitySinks[] by solving each collected task (dedup by sink+ctx+expr). */
JSValue solve_all(JSContext *ctx) {
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
                    { JSValue rov = JS_GetPropertyStr(ctx, t, "requiredOrigin");   /* forgeable origin gate -> the PoC's delivery origin (part of the reproduction envelope) */
                      if (JS_IsString(rov)) JS_SetPropertyStr(ctx, rec, "requiredOrigin", rov); else JS_FreeValue(ctx, rov); }
                    JS_SetPropertyStr(ctx, rec, "source", JS_NewString(ctx, "ast_analysis"));
                    JS_SetPropertyStr(ctx, rec, "poc", JS_NewString(ctx, rpoc));
                    if ((ex && strstr(ex, "{ls}")) || (srcpath && strstr(srcpath, "{ls}")))   /* stored/second-order: the sink reads attacker-PLANTED web storage, so the PoC is a TWO-STAGE artifact (plant then fire), not a reflected URL */
                        JS_SetPropertyStr(ctx, rec, "secondOrder", JS_NewString(ctx, "requires an attacker-planted localStorage/sessionStorage value: two-stage PoC (plant the key, then a later load reads+sinks it)"));
                    if (g_csp && g_csp[0]) {   /* POLICY-RELATIVE, PER SINK CLASS: the model broke out; SOLVE the page's CSP for the concrete bypass path (not a dumbed-down boolean) */
                        int is_eval = sink && (strcmp(sink, "eval") == 0 || strcmp(sink, "Function") == 0 || strcmp(sink, "setTimeout") == 0);
                        CspBypass bp; csp_bypass(is_eval, g_dom, &bp);
                        JS_SetPropertyStr(ctx, rec, "csp", JS_NewString(ctx, g_csp));
                        JS_SetPropertyStr(ctx, rec, "cspBlocked", JS_NewBool(ctx, bp.blocked));       /* the inline/eval vector is blocked... */
                        if (bp.blocked) {
                            JS_SetPropertyStr(ctx, rec, "cspBypass", JS_NewString(ctx, bp.via));      /* ...but HERE is the concrete bypass path the attacker uses */
                            JS_SetPropertyStr(ctx, rec, "cspReason", JS_NewString(ctx, bp.detail));
                            if (bp.hosts[0]) JS_SetPropertyStr(ctx, rec, "cspGadgetHosts", JS_NewString(ctx, bp.hosts));      /* allowlisted hosts to find a JSONP endpoint on */
                            if (bp.gadget_lib[0]) JS_SetPropertyStr(ctx, rec, "cspScriptGadget", JS_NewString(ctx, bp.gadget_lib));  /* a loaded gadget library (bypasses even 'self') */
                            if (bp.strict_dynamic) JS_SetPropertyStr(ctx, rec, "cspStrictDynamic", JS_TRUE);
                            if (bp.nonce_required) JS_SetPropertyStr(ctx, rec, "cspNonceRequired", JS_TRUE);                  /* inline blocked; nonce is protective, not trivially reusable */
                            if (bp.nonce[0]) JS_SetPropertyStr(ctx, rec, "cspObservedNonce", JS_NewString(ctx, bp.nonce));    /* a nonce seen in THIS response — a static-misconfig / CSS-side-channel-leak hint, NOT a plain reuse */
                        }
                        if (bp.trusted_types) {   /* TT enforced: the HTML sink throws unless a policy stringifies — report the REAL policy state OBSERVED by running the bundle's createPolicy calls */
                            JS_SetPropertyStr(ctx, rec, "trustedTypes",
                                JS_NewString(ctx, tt_default_exists()
                                                ? (tt_default_weak() == 1 ? "enforced; a 'default' policy is defined AND its createHTML was RUN on an XSS probe — the payload SURVIVED (weak/identity policy), so TT does not stop this sink: confirmed exploitable"
                                                 : tt_default_weak() == 0 ? "enforced; a 'default' policy is defined and its createHTML SANITIZED the probe (run-verified) — TT neutralises this sink unless the sanitizer itself has a bypass"
                                                 : "enforced; a 'default' policy is defined (auto-applies to every sink) — createHTML not probed")
                                                : tt_any_policy() ? "enforced; named policy(ies) defined but no 'default' — the sink needs the payload wrapped by a reachable policy's createHTML"
                                                : "enforced; NO policy defined in the bundle — every string sink assignment throws (no TT-abuse surface unless a gadget creates a policy)"));
                        }
                    }
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

/* The @S accumulators' lifecycle — called from the engine's qjs_init / qjs_teardown. The CowExempt marks make
   the verified-PoC + enqueue-dedup ledgers SURVIVE a candidate flow's COW delta unapply (host analysis state,
   not page state). g_solve_ctx (the clean candidate-eval realm) is created by the engine (it needs the runtime)
   and only referenced here. */
/* Has a candidate already broken out this "sink|ctx" (in the verified ledger)? The scheduler asks before
   dispatching a candidate flow tagged with that vtarget — a redundant re-run to skip. Keeps g_verified private. */
int solve_is_verified(JSContext *ctx, const char *vtarget) {
    if (!vtarget || !JS_IsObject(g_verified)) return 0;
    JSValue vv = JS_GetPropertyStr(ctx, g_verified, vtarget);
    int solved = JS_IsString(vv); JS_FreeValue(ctx, vv);
    return solved;
}
void solve_init(JSContext *ctx) {
    g_solvetasks = JS_NewArray(ctx);
    g_verified = JS_NewObject(ctx); g_enqueued = JS_NewObject(ctx);
    g_reached = JS_NewObject(ctx);
    /* ALL @S ledgers are HOST analysis state, not page state — writes to them (during the document flow, a
       session, or a candidate flow) must NOT ride the flow's COW delta, else they are PARKED with that delta and
       vanish at the teardown emit (the document flow's delta is now parked as g_doc_base, not left applied like
       the old boot). g_solvetasks/g_reached were missing this and so the sink task written during the document
       flow never reached solve_all. */
    JS_CowExempt(g_verified); JS_CowExempt(g_enqueued); JS_CowExempt(g_solvetasks); JS_CowExempt(g_reached);
}
void solve_free(JSContext *ctx) {
    for (int i = 0; i < g_gate_n; i++) free(g_gate_tokens[i]);
    free(g_gate_tokens); g_gate_tokens = NULL; g_gate_n = g_gate_cap = 0;
    JS_FreeValue(ctx, g_solvetasks); g_solvetasks = JS_UNDEFINED;
    JS_FreeValue(ctx, g_verified); g_verified = JS_UNDEFINED;
    JS_FreeValue(ctx, g_reached); g_reached = JS_UNDEFINED;
    JS_FreeValue(ctx, g_enqueued); g_enqueued = JS_UNDEFINED;
}
