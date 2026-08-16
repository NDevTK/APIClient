/* @S solver — see solve.h. Forced-exec candidate: derive the breakout from the sink's lexical CONTEXT, inject
   it at the source, re-run the REAL code, and verify it FIRES. */
#include "solver/solve.h"
#include "core/json_buf.h"
#include "core/frame/policy_container.h"
#include "core/html/trusted_types.h"
#include "core/dom/document.h"
#include "solver/concolic.h"
#include "solver/endpoint.h"
#include "solver/engine.h"
#include "solver/flow.h"
#include "check.h"
#include <lexbor/html/html.h>
#include <lexbor/dom/dom.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

enum { SINK_EVAL = 0, SINK_HTML = 1, SINK_URL = 2 };   /* JS / HTML / URL context -> different candidate set + fire oracle */

/* THE RUNNING FLOW's fire flag and candidate mode. These were file-scope globals, which is only correct while
   one candidate runs start-to-finish with nothing else scheduled — the shape the verify driver has and the BFS
   does not. Reached through the running flow so a preemption cannot cross them. A NULL flow (baseline setup)
   has neither, and the accessors say so rather than inventing a value. */
/* EVERY @S CANDIDATE FLOW SEEDED. A candidate RE-RUNS the page, so this number times the page's cost is most of
   what an @S search spends — and it is what says whether a run got slower because there were more searches or
   because each search grew. Reported beside the switch count for that reason: one number cannot decompose. */
static int g_cands_seeded;
int solve_candidate_count(void) { return g_cands_seeded; }

static int *fired_slot(void)     { Flow *f = flow_running(); return f ? &f->cand_fired : NULL; }
static int  is_verifying(void)   { Flow *f = flow_running(); return f && f->cand_verifying; }
static void set_fired(void)      { int *p = fired_slot(); if (p) *p = 1; }

/* A detected sink awaiting fire-verification. `seeded` is per SINK, not per session: a sink discovered late —
   inside a lazily-imported chunk, inside an injected <script src> — is discovered after the frontier has already
   drained once, and a one-shot "the candidates are seeded" latch meant it never got any. That latch was a cap:
   it bounded verification by WHEN a sink was found rather than by whether it had been searched. */
/* `tried` is the COUNT of breakouts this sink's search has run, not a bit: a sink with no PoC is REPORTED as a
   parked search, and "parked after 0 candidates" and "parked after 5" are different states of the search that a
   flag cannot tell apart. Non-zero also means seeded, so the idempotence the re-seed depends on is unchanged. */
typedef struct { char *src; int sink; int tried; } Cand;
static Cand *g_pending = NULL; static int g_pending_n = 0, g_pending_cap = 0;

/* THE SINK CLASS TABLE, defined below its candidate sets. Everything a report says about a sink is a row of
   it, so these two are how the rest of the file reaches one. */
typedef struct SinkClass SinkClass;
static const SinkClass *sink_class(int sink);
static const char      *sink_name(int sink);

/* A FIRE-VERIFIED PoC. The sink is held as its CLASS, not as its display name: every fact the reproduction
   envelope states — the CSP question, the Trusted Types question, what makes the breakout run — is a fact
   about the class, and holding the name meant asking for them with a `strcmp` chain over display text at emit
   time. That chain computed the CSP question and threw the other two away. */
typedef struct { int cls; char *source; char *poc; } Finding;   /* verified PoCs only */
static Finding *g_sinks = NULL; static int g_sinks_n = 0, g_sinks_cap = 0;

static JSValue js_x9(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) { (void)ctx; (void)t; (void)c; (void)v; set_fired(); return JS_UNDEFINED; }

/* THE FIRE. A sink executes attacker-shaped code — `eval(s)`, a `javascript:` navigation, an auto-firing event
   handler in re-parsed HTML — and that code is the PAGE's, so it can hold a loop, an await, a recursion. Running
   it with JS_Eval from C entered a bytecode body below the live candidate flow, where it cannot suspend: the
   engine's own DFAIL named it a drive-to-completion, and the whole @S verification was built on one.
   The sunk code is simply MORE CODE IN THIS FLOW — the same thing a lazy chunk is — so it is queued as another
   program of the running flow and the ONE BFS runs it, preemptible and parkable like every other. The candidate
   re-run drains its queue before finishing, so the flow's own fire flag still answers when it completes. */
static void fire_js(const char *src, size_t len) {
    char *body = malloc(len + 1);
    CHECK(body, "solve: OOM queueing a fired PoC body");
    memcpy(body, src, len); body[len] = 0;
    engine_queue_candidate(body);
    free(body);
}

/* Breakout CANDIDATES — one per common JS context. We do NOT statically detect which context applies (that
   needs a JS lexer + loses to filters/encoders the code may apply); instead we try each through the REAL code
   and the one that FIRES is the verified PoC. Re-execution is the oracle, so this survives any quoting/filter
   the code imposes and needs no lexer. */
static const char *CANDS_JS[] = {
    "X9()",           /* statement / expression position — the input runs directly */
    "';X9();//",      /* single-quoted string context */
    "\";X9();//",     /* double-quoted string context */
    "`;X9();//",      /* template-literal context */
    "${X9()}",        /* template interpolation */
    NULL
};
static const char *CANDS_HTML[] = {
    "<svg onload=X9()>",           /* HTML-text context: an auto-firing element */
    "<img src=x onerror=X9()>",    /* HTML-text: img onerror fires on the bad src */
    "\"><svg onload=X9()>",        /* break out of a double-quoted attribute, then a firing element */
    "'><svg onload=X9()>",         /* single-quoted attribute */
    "></textarea><svg onload=X9()>", /* rawtext element (textarea/title/...) — close it first */
    NULL
};
static const char *CANDS_URL[] = {
    "javascript:X9()",     /* URL context: the vector IS the javascript: scheme (one fixed context) */
    "javascript:X9()//",
    NULL
};
/* THE SINK CLASSES — one row per lexical context the solver breaks out of, and the row holds everything that
   is a fact about THE SINK rather than about a run. They are one row because they are one thing: the sink's
   FIRE ORACLE. Before this they were four scattered statements of it, and one of the four did not exist:
   the breakout set was a function keyed by the class, the CSP question was a `strcmp` chain over the class's
   DISPLAY NAME written at emit time, the Trusted Types question was asked by nobody at all though
   trusted_types.c answers it, and what makes a fired breakout actually RUN was never stated anywhere — so a
   reader of a PoC could not tell one that fires at parse time from one that needs a navigation, which §S(d)
   requires every emitted PoC to carry.
   `fires_on` IS THE ORACLE'S OWN SEMANTICS, read off the oracle rather than chosen beside it:
     - the eval oracle hands the sink's own argument to the flow's program queue, because that is what the sink
       itself does with it — so a fired eval PoC runs the instant the page reaches that call;
     - html_fire_walk runs `onload` and `onerror` and NOTHING else (the AUTO-firing handlers — `onmouseover`
       needs interaction) over markup it has just parsed, and CANDS_HTML is auto-firing elements for exactly
       that reason, so a fired HTML PoC runs at insertion and never needs a click;
     - url_fire runs a URL's JS only when the scheme is `javascript:`, which is code that runs when the
       NAVIGATION happens — for this engine's URL sink, a form whose action the page wrote, on submission.
   THE TRUSTED TYPES COLUMN IS THE SPEC'S, NOT THIS ENGINE'S CODE PATH: TT §3.8 makes the markup sinks
   TrustedHTML sinks and `eval` a TrustedScript sink, and navigating to a `javascript:` URL is not a TT sink at
   all — which is why the URL row declares none, and why its absence from the record is a positive statement
   rather than a gap.
   The cold tier's parked-candidate DCHECK names this table as where a parked candidate's sink re-binds BY NAME
   on resume; `sink_class_of_name` is that binding. */
struct SinkClass {
    const char       *name;      /* the display name a report and a parked entry carry */
    const char      **cands;     /* the breakout set this lexical context needs */
    PolicyScriptKind  policy;    /* which of CSP's four script questions a fired breakout turns on */
    int               tt;        /* the TrustedTypeKind gating this sink, or -1 — the spec makes it no TT sink */
    const char       *fires_on;  /* what makes the fired breakout RUN, from the oracle above */
};
static const SinkClass SINKS[] = {
    [SINK_EVAL] = { "eval",      CANDS_JS,   POLICY_EVAL,            TRUSTED_TYPE_SCRIPT, "sink-evaluates" },
    [SINK_HTML] = { "innerHTML", CANDS_HTML, POLICY_INLINE_HANDLER,  TRUSTED_TYPE_HTML,   "parse-insert"   },
    [SINK_URL]  = { "location",  CANDS_URL,  POLICY_JAVASCRIPT_URL,  -1,                  "navigation"     },
};
#define SINK_CLASS_N ((int)(sizeof SINKS / sizeof SINKS[0]))

/* CHECK rather than DCHECK: every row of the report is written straight out of this table, so an index it does
   not have is the report reading past its own data. */
static const SinkClass *sink_class(int sink) {
    CHECK(sink >= 0 && sink < SINK_CLASS_N,
          "an @S record named a sink class this table does not have — the whole report is written from it");
    return &SINKS[sink];
}
static const char *sink_name(int sink) { return sink_class(sink)->name; }

/* THE NAME A CANDIDATE FLOW CARRIES, BACK TO ITS CLASS. A flow holds `cand_sink` as the table's OWN pointer,
   so the binding is identity and not a string compare — and a name from anywhere else is a candidate this
   table did not seed, which is precisely what a cold resume that rebinds by text must not produce. */
static int sink_class_of_name(const char *name) {
    int i;
    for (i = 0; i < SINK_CLASS_N; i++) if (SINKS[i].name == name) return i;
    DFAIL("a candidate flow carried a sink name that is not one of the sink classes' own — the name is this "
          "table's pointer, so a flow holding another was built somewhere that does not go through it (a cold "
          "resume rebinding a parked candidate BY NAME must land back on this table's row)");
    return -1;
}

/* Installed AFTER the table so the one point every session goes through can assert it — see the loop. */
void solve_init(JSContext *ctx) {
    g_pending = NULL; g_pending_n = g_pending_cap = 0;
    g_cands_seeded = 0;
    g_sinks = NULL; g_sinks_n = g_sinks_cap = 0;
    /* EVERY SINK CLASS DECLARES ITS WHOLE ROW. A row IS the sink's contract — the breakout set a candidate is
       seeded with, the CSP question, the Trusted Types question, and the fire semantics a PoC states — and a
       row added with a field left out does not fail: it emits a finding missing exactly that fact, which is
       the silent half-envelope this record exists to end.
       (`tt` is deliberately not asserted non-negative: -1 is the POSITIVE statement that the standard makes
       this sink no Trusted Types sink at all, which is what the URL row means.) */
    for (int i = 0; i < SINK_CLASS_N; i++) {
        DCHECK(SINKS[i].name && SINKS[i].cands && SINKS[i].cands[0] && SINKS[i].fires_on,
               "a sink class was declared without its display name, its breakout set or the fire semantics its "
               "oracle gives it — a PoC cannot state how it reproduces without that row being whole");
    }
    JSValue g = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, g, "X9", JS_NewCFunction(ctx, js_x9, "X9", 0));
    JS_FreeValue(ctx, g);
}

static void add_pending(const char *src, int sink) {
    for (int i = 0; i < g_pending_n; i++) if (g_pending[i].sink == sink && !strcmp(g_pending[i].src, src)) return;   /* dedup */
    if (g_pending_n >= g_pending_cap) { g_pending_cap = g_pending_cap ? g_pending_cap * 2 : 8; g_pending = realloc(g_pending, (size_t)g_pending_cap * sizeof(Cand)); CHECK(g_pending, "solve: OOM pending"); }
    /* EVERY field, because the array is realloc'd and never zeroed: leaving `tried` as whatever the allocator
       held made a sink look already-searched and it got no candidate at all. */
    g_pending[g_pending_n].src = strdup(src);
    g_pending[g_pending_n].sink = sink;
    g_pending[g_pending_n].tried = 0;
    g_pending_n++;
    flow_credit_emit(1.0);   /* a NEW attacker-source-reaches-sink: value-of-information for the running flow */
}

static void record_sink(int cls, const char *source, const char *poc) {
    /* A finding is a pending sink that SOLVED, so the two lists are one list in two states — the parked-search
       emit subtracts one from the other by (sink, source) and a finding with no pending twin would report as
       both fired and parked. Asserted at the origin because a future detector that records a PoC without first
       calling add_pending would otherwise corrupt the report rather than crash. */
    {
        int have = 0;
        for (int i = 0; i < g_pending_n; i++)
            if (g_pending[i].sink == cls && !strcmp(g_pending[i].src, source)) { have = 1; break; }
        DCHECK(have, "an @S finding was recorded for a sink that was never detected as pending");
    }
    sink_class(cls);   /* the row exists before anything is stored against it */
    for (int i = 0; i < g_sinks_n; i++) if (g_sinks[i].cls == cls && !strcmp(g_sinks[i].source, source)) return;
    if (g_sinks_n >= g_sinks_cap) { g_sinks_cap = g_sinks_cap ? g_sinks_cap * 2 : 8; g_sinks = realloc(g_sinks, (size_t)g_sinks_cap * sizeof(Finding)); CHECK(g_sinks, "solve: OOM @S store"); }
    Finding *f = &g_sinks[g_sinks_n++];
    f->cls = cls; f->source = strdup(source ? source : "?"); f->poc = strdup(poc);
}

void solve_eval_sink(JSContext *ctx, JSValueConst arg) {
    if (is_verifying()) {                          /* candidate run: the arg is the injected+wrapped CONCRETE code */
        if (concolic_is(arg)) return;           /* injection didn't reach this read -> not our candidate */
        const char *code = JS_ToCString(ctx, arg);
        if (code) { fire_js(code, strlen(code)); JS_FreeCString(ctx, code); }
        return;                                 /* g_fired reflects the PoC once this flow drains its queue */
    }
    if (!concolic_is(arg)) return;              /* detection: not an attacker source -> not a sink */
    const char *shape = concolic_shape_c(arg);
    const char *src = concolic_src_c(arg);
    add_pending(src ? src : (shape ? shape : "?"), SINK_EVAL);   /* record the source; breakout SEARCHED at verify */
}

/* HTML firing oracle: re-parse the sink output with the REAL Lexbor parser and FIRE the auto-firing event
   handlers (svg/body onload, img/script onerror) by eval'ing their JS — X9 fires iff a breakout placed
   executable JS in an auto-firing position. innerHTML does NOT run <script>, so those never fire (correct). */
/* THE TREE'S DEPTH IS THE CANDIDATE'S DATA — a breakout that nests `<div>` a million times is exactly the kind
   of input this walk exists to run — so descending by C frame made the oracle's own depth attacker-controlled.
   Lexbor's nodes carry `parent`, so the traversal needs no stack at all: descend to first_child, else take
   `next`, else climb until a `next` exists, never above the level the walk started at. */
static void html_fire_walk(lxb_dom_node_t *node) {
    lxb_dom_node_t *top = node->parent;   /* the level the walk must not climb above */
    lxb_dom_node_t *n = node;

    while (n) {
        if (n->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            lxb_dom_element_t *el = lxb_dom_interface_element(n);
            static const char *H[] = { "onload", "onerror", NULL };   /* AUTO-firing only (onmouseover needs interaction) */
            for (int h = 0; H[h]; h++) {
                size_t vl = 0;
                const lxb_char_t *v = lxb_dom_element_get_attribute(el, (const lxb_char_t *)H[h], strlen(H[h]), &vl);
                if (v && vl) fire_js((const char *)v, vl);
            }
        }
        if (n->first_child) { n = n->first_child; continue; }
        while (n && !n->next) {
            n = n->parent;
            if (n == top) n = NULL;
        }
        if (n) n = n->next;
    }
}
static void html_fire(const char *html) {
    lxb_html_document_t *doc = lxb_html_document_create();
    if (!doc) return;
    if (lxb_html_document_parse(doc, (const lxb_char_t *)html, strlen(html)) == LXB_STATUS_OK) {
        lxb_dom_element_t *root = lxb_dom_document_element(&doc->dom_document);
        if (root) html_fire_walk(lxb_dom_interface_node(root));
    }
    lxb_html_document_destroy(doc);
}

/* URL firing oracle: navigating to a `javascript:` URL executes its JS. So the "fire" is: if the URL scheme is
   javascript:, eval the part after the colon — X9 fires iff the breakout made the URL a javascript: one. */
static void url_fire(JSContext *ctx, const char *url) {
    while (*url == ' ' || *url == '\t' || *url == '\n') url++;   /* leading whitespace is ignored by the URL parser */
    if (!strncasecmp(url, "javascript:", 11)) {
        const char *js = url + 11;
        fire_js(js, strlen(js));
    }
}
/* location = arg (or el.href = arg): a URL-context sink. */
void solve_url_sink(JSContext *ctx, JSValueConst arg) {
    if (is_verifying()) {
        if (concolic_is(arg)) return;
        const char *url = JS_ToCString(ctx, arg);
        if (url) { url_fire(ctx, url); JS_FreeCString(ctx, url); }
        return;
    }
    if (!concolic_is(arg)) return;
    const char *shape = concolic_shape_c(arg);
    const char *src = concolic_src_c(arg);
    add_pending(src ? src : (shape ? shape : "?"), SINK_URL);
}

/* innerHTML = arg: an HTML-context sink. Detection records the source; the candidate run re-parses the injected
   HTML and fires its handlers. */
void solve_html_sink(JSContext *ctx, JSValueConst arg) {
    if (is_verifying()) {
        if (concolic_is(arg)) return;   /* injection didn't reach this write */
        const char *html = JS_ToCString(ctx, arg);
        if (html) {
            /* ONLY THE MARKER CAN FIRE, so a string without it cannot — and this is EXACT rather than a
               heuristic: html_fire's only path to a report is an auto-firing handler whose code calls X9, and a
               parse cannot invent the marker out of bytes that do not contain it.
               It matters because "the injection did not reach this write" was being tested by "the value is not
               concolic", which is also true of every literal the page writes. So each candidate flow built a
               whole document and parsed EVERY innerHTML in the page — the fixture's own markup, once per
               candidate — and the cost is the page's markup times the number of breakouts tried. */
            if (strstr(html, "X9")) html_fire(html);
            JS_FreeCString(ctx, html);
        }
        return;
    }
    if (!concolic_is(arg)) return;
    const char *shape = concolic_shape_c(arg);
    const char *src = concolic_src_c(arg);
    add_pending(src ? src : (shape ? shape : "?"), SINK_HTML);
}

/* Fire-verify every pending source: SEARCH the candidate breakouts — inject each at the source, re-run the
   REAL program as a flow, and the FIRST that makes X9 fire is the replay-verified PoC (re-execution is the
   oracle, so no static context detection is needed). The re-run is a FLOW on the one frontier,
   the same path the scheduler uses — there is no separate boot re-runner. */
/* SEED the candidate flows: one per (detected sink, breakout), each an ordinary member of the ONE frontier.
   The scheduler runs them preemptibly and parkably like every other flow, which is what §solver requires — a
   driver that runs a candidate start-to-finish cannot park an unbounded loop inside it. */
/* Seed a candidate flow per (sink, breakout) for every sink NOT YET SEEDED, and answer how many were added.
   Idempotent by construction, so the scheduler can ask again every time the frontier drains — which is what a
   sink found by code that only loaded after the first drain needs. */
/* A PARKED CANDIDATE COMING BACK — see solve.h for why the re-binding and the bookkeeping are ONE call. */
const char *solve_resume_candidate(const char *src, const char *sink_name) {
    int i, cls = -1;

    DCHECK(src && *src && sink_name && *sink_name,
           "a parked @S candidate was rebuilt without a source or without a sink class — its identity IS the "
           "substitution it carries, so either one missing makes it an exploration flow wearing a payload");
    for (i = 0; i < SINK_CLASS_N; i++)
        if (!strcmp(SINKS[i].name, sink_name)) { cls = i; break; }
    if (cls < 0) {
        DFAIL("a parked @S candidate named a sink class this build's table does not have — the class crosses "
              "the tier by NAME exactly so it survives a pointer that cannot, so a name nothing matches is a "
              "residue from a build whose sink classes this one has dropped. Add the class back or drop the "
              "record; resuming it as an ordinary flow would report a search that never ran");
        return NULL;
    }
    /* PENDING FIRST, then the count on the entry it just guaranteed exists. add_pending dedups, so a session
       that resumes five candidates for one sink registers it once and raises `tried` five times — which is
       exactly the number of breakouts that sink's search has run, and exactly what the parked-search entry
       reports. */
    add_pending(src, cls);
    for (i = 0; i < g_pending_n; i++)
        if (g_pending[i].sink == cls && !strcmp(g_pending[i].src, src)) { g_pending[i].tried++; break; }
    DCHECK(i < g_pending_n,
           "the sink a resumed candidate names is not on the pending list after being added to it — the count "
           "that keeps seeding idempotent has nowhere to land, so this sink is about to be searched twice");
    /* IT COSTS WHAT A FRESH ONE COSTS, so it counts as one. This number is what says whether a run got slower
       because there were more searches or because each search grew, and a resumed candidate re-runs the whole
       page exactly as a newly-seeded one does. */
    g_cands_seeded++;
    return SINKS[cls].name;
}

int solve_seed_candidates(JSContext *ctx) {
    int added = 0;
    for (int i = 0; i < g_pending_n; i++) {
        const char **cands;
        if (g_pending[i].tried) continue;
        cands = sink_class(g_pending[i].sink)->cands;
        for (int c = 0; cands[c]; c++) {
            Flow *f = flow_add(ctx, JS_UNDEFINED, WORLD_NONE);   /* a candidate session runs from the baseline */
            f->cand_src     = strdup(g_pending[i].src);
            f->cand_payload = strdup(cands[c]);
            f->cand_sink    = sink_name(g_pending[i].sink);
            CHECK(f->cand_src && f->cand_payload, "solve: OOM seeding a candidate flow");
            added++;
            g_cands_seeded++;
            g_pending[i].tried++;
        }
        DCHECK(g_pending[i].tried > 0, "a detected sink was marked searched with no candidate seeded");
    }
    return added;
}

/* THE SUBSTITUTION MIRRORS THE RUNNING FLOW — it is not a bracket someone opens and closes.
   Written as a bracket it was WRONG, and silently: the entry returned early for a flow with no candidate, so
   switching from a candidate flow to an ordinary one left the previous candidate's payload installed and
   endpoint recording suppressed. The exploring flow then read the attacker's concrete string where its concolic
   source belonged — so it stopped forking at the gates that value feeds, its sinks stopped being detected (a
   concrete value is not a concolic one), and every endpoint it learned was dropped. The comment that used to
   sit here asserted the opposite ("an ordinary flow scheduled in between is unaffected"), which is exactly the
   sort of claim that survives because nothing tests it.
   The scheduler calls this on EVERY switch-in, so the fix is for it to install the incoming flow's state
   unconditionally — a flow with no candidate installs "no candidate", which is the clearing that was missing.
   There is then no close to forget, and no ordering between two calls to get wrong. */
void solve_flow_begin(Flow *f) {
    concolic_set_candidate(f ? f->cand_src : NULL, f ? f->cand_payload : NULL);
    endpoint_suppress(f && f->cand_src ? 1 : 0);
    if (f && f->cand_src) f->cand_verifying = 1;
}

/* FINISHING is a different event from switching out, and only it records. The globals are deliberately NOT
   cleared here: the next switch-in installs the next flow's state, and clearing in two places is how the
   asymmetry above got in. */
void solve_flow_end(Flow *f) {
    if (!f || !f->cand_src) return;
    f->cand_verifying = 0;
    if (f->cand_fired)
        record_sink(sink_class_of_name(f->cand_sink), f->cand_src, f->cand_payload);
}

/* ── @S JSON emit (C-native) ── the writer is core/json_buf.h's, which this file and endpoint.c each used to
   carry a private copy of. */
static int solved(int cls, const char *src) {
    for (int i = 0; i < g_sinks_n; i++)
        if (g_sinks[i].cls == cls && !strcmp(g_sinks[i].source, src)) return 1;
    return 0;
}

/* THE SOURCE'S DECLARED BROWSER DELIVERY, written identically into BOTH entry shapes because it is ONE
   declaration and the two entries need different halves of it: `sourceEncodes` is what a BREAKOUT had to
   survive (the parked entry's constraint), and the mechanism plus its address component are what a
   REPRODUCTION has to perform (§S(d)'s envelope). Splitting them across two writers is how one of them went
   missing before.
   AN UNDECLARED SOURCE WRITES NOTHING, and that silence is the fact that there IS no declaration — server-
   injected page state is written by the attacker directly, no component carries or transforms it, and a
   consumer must say exactly that rather than invent a vector. The vocabulary is the engine's: the delivery
   layer switches on these tokens and states its own inability to perform one, but it never decides which
   source uses which — the component that owns the source already did. */
static void emit_delivery(JsonBuf *b, const char *src) {
    const char *enc = concolic_source_encodes(src), *kind = NULL;
    char prefix = 0;

    if (enc) { json_buf_puts(b, ",\"sourceEncodes\":"); json_buf_str(b, enc); }
    if (concolic_source_delivery(src, &kind, &prefix) && kind) {
        json_buf_puts(b, ",\"delivery\":"); json_buf_str(b, kind);
        if (prefix) {
            char p[2] = { prefix, 0 };
            json_buf_puts(b, ",\"deliveryPrefix\":"); json_buf_str(b, p);
        }
    }
}

/* EVERY DETECTED SINK IS REPORTED — the ones with a fire-verified PoC, AND the ones whose search has not solved.
   Emitting only the solved ones made the report say nothing at all about a sink an attacker source demonstrably
   REACHES, and a reader cannot tell that silence apart from "no attacker input gets here" — which is the
   "safe"/"verified:false" verdict solve.h forbids, arrived at by omission instead of by claim. A sink without a
   PoC is a PARKED SEARCH: reached, searched this far, not broken out of YET.
   It carries the two facts that make it actionable rather than a shrug: how many breakouts have been run, and
   the bytes the source's own component percent-encodes — the constraint every candidate had to survive. For
   `innerHTML` fed from `location.hash` that set contains `<`, which is why no HTML-context candidate can fire
   and why the same source's JS-context sink does; the entry states the constraint, it does not claim the sink
   is safe, because an app that percent-DECODES its fragment would break out with the same candidate. */
char *solve_json_array(JSContext *ctx) {
    JsonBuf b = { 0 };
    int n = 0;
    json_buf_puts(&b, "[");
    for (int i = 0; i < g_sinks_n; i++) {
        const SinkClass *sc = sink_class(g_sinks[i].cls);
        const PolicyContainer *pc = document_policy(ctx);

        if (n++) json_buf_puts(&b, ",");
        json_buf_puts(&b, "{\"sink\":"); json_buf_str(&b, sc->name);
        json_buf_puts(&b, ",\"source\":"); json_buf_str(&b, g_sinks[i].source);
        json_buf_puts(&b, ",\"poc\":"); json_buf_str(&b, g_sinks[i].poc);
        /* §S(d): EVERY PoC CARRIES ITS REPRODUCTION ENVELOPE. What makes it RUN is the first thing a reader
           needs and the last thing this record carried — the vector was decided one line below, for the CSP
           question, and thrown away. It comes off the sink's own fire oracle (see the table). */
        json_buf_puts(&b, ",\"firesOn\":"); json_buf_str(&b, sc->fires_on);
        /* §S: A FIRING BREAKOUT IN THE MODEL IS NOT YET A WORKING EXPLOIT. The PoC has to run under the page's
           ACTUAL policy, and an inline `onerror` is dead under `script-src 'self'`. Reporting a bare XSS there
           is a false positive a reader cannot tell from a real one; reporting nothing would hide a sink that IS
           real. So the finding stays, and it CARRIES what blocks it — "sink REAL, CSP blocks", which is the
           standard's own distinction and the only one that survives being read by someone else. */
        if (!policy_allows(pc, sc->policy)) {
            json_buf_puts(&b, ",\"cspBlocks\":");
            json_buf_str(&b, policy_container_csp(pc));
        }
        /* AND THE SAME RULE ONE ALGORITHM EARLIER. Under `require-trusted-types-for 'script'` an innerHTML
           assignment THROWS before the markup is ever parsed, so the model's breakout is real and the write
           that carries it never happens on the real page. trusted_types.c has answered this question all
           along and nothing asked it. The emitted value is the SINK GROUP the CSP names, because that is what
           the directive is written in terms of and what a reader has to add a policy for. */
        if (sc->tt >= 0 && trusted_types_required(ctx, (TrustedTypeKind)sc->tt)) {
            json_buf_puts(&b, ",\"trustedTypes\":");
            json_buf_str(&b, "script");
        }
        /* THE DELIVERY — including whether this is §S(b)'s TWO-STAGE plant-then-load PoC. There is deliberately
           no separate `stored` boolean: "is it stored" is not a second fact beside the mechanism, it IS the
           mechanism (a `plant` delivery is two-stage and every other one is a single load), and two fields for
           one fact is precisely the drift that made five names on this record mean nothing. */
        emit_delivery(&b, g_sinks[i].source);
        json_buf_puts(&b, "}");
    }
    for (int i = 0; i < g_pending_n; i++) {
        char t[32];
        if (solved(g_pending[i].sink, g_pending[i].src)) continue;
        if (n++) json_buf_puts(&b, ",");
        json_buf_puts(&b, "{\"sink\":"); json_buf_str(&b, sink_name(g_pending[i].sink));
        json_buf_puts(&b, ",\"source\":"); json_buf_str(&b, g_pending[i].src);
        json_buf_puts(&b, ",\"search\":\"parked\",\"tried\":");
        snprintf(t, sizeof t, "%d", g_pending[i].tried); json_buf_puts(&b, t);
        /* The parked entry carries the DECLARATION, not the envelope: a search that has not solved has no
           vector to state and no PoC to reproduce, so `firesOn`/`cspBlocks`/`trustedTypes` would be claims
           about a PoC that does not exist. What it does carry is the whole source declaration — the bytes a
           candidate must survive AND how the attacker would have to reach the victim if one ever fires. */
        emit_delivery(&b, g_pending[i].src);
        json_buf_puts(&b, "}");
    }
    json_buf_puts(&b, "]");
    return json_buf_take(&b);
}

int solve_count(void) { return g_sinks_n; }

void solve_free(void) {
    for (int i = 0; i < g_pending_n; i++) free(g_pending[i].src);
    free(g_pending); g_pending = NULL; g_pending_n = g_pending_cap = 0;
    for (int i = 0; i < g_sinks_n; i++) { free(g_sinks[i].source); free(g_sinks[i].poc); }
    free(g_sinks); g_sinks = NULL; g_sinks_n = g_sinks_cap = 0;
}
