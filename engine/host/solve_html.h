/* @S HTML breakout ANALYSIS — the pure half of the security solver.
 *
 * Parses a sink's OUTPUT with the REAL Lexbor parser to (a) locate the hole's parse context and (b) prove a
 * candidate actually FIRES the X9 marker in that context. It touches only the Lexbor DOM and a clean quickjs
 * eval realm — NO scheduler / registry / flow state — so it lives apart from the candidate-flow orchestration
 * (emit_cand / reg_add_cand / solve_add) that stays in main.c. This is the component split the DOM+JS analysis
 * naturally has: FACT-derivation here, flow-scheduling there.
 */
#ifndef ENGINE_HOST_SOLVE_HTML_H
#define ENGINE_HOST_SOLVE_HTML_H

#include <lexbor/html/html.h>
#include "quickjs.h"

/* The clean @S eval realm (a fresh quickjs context with a universal no-op stub `__u` + a fire flag `__f9`),
   built + owned by main.c. x9_fires/solve_broke RUN candidate handlers here to see if X9 executes. */
extern JSContext *g_solve_ctx;

/* Unique locator placed at each hole so the parse walk can find where the input's bytes landed. */
#define CTX_LOC "Lz9Qk7Wm"

/* Where the hole's bytes landed, read from the REAL parse of the sink output (never guessed):
   is_attr -> inside `tag`'s attribute value; is_comment -> inside <!-- -->; found+tag (text child) -> element
   content (rawtext or normal text). */
struct ctx_probe { int found; int is_comment; int is_attr; char tag[16]; };

/* Did the candidate's payload reach an EXECUTABLE position in the sink output `res` (a live on* handler, a
   script-executing <script>, a javascript: URL, or firing eval-code) — proven by parsing/running, not strstr. */
int solve_broke(const char *sc, const char *res);

/* Browser FACT: which elements auto-fire a handler with no interaction (img/iframe/...->onerror on a broken
   resource; body/svg/...->onload). NULL = no auto-firing handler (only a tag-injection can break out of it). */
const char *elem_fire_event(const char *tag);

/* Is `tag` a RAWTEXT element (textarea/title/style/script/...) whose content must be closed with </tag>. */
int is_rawtext_tag(const char *t);

/* Parse `shape` (holes marked with CTX_LOC internally) and report the hole's parse context into `*out`. */
void solve_ctx_detect(const char *shape, struct ctx_probe *out);

#endif
