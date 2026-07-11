/* ATTACKER-SOURCE DELIVERY — the ONE place the @S replay candidate is handed back through a source getter,
 * encoded exactly as the BROWSER delivers that source. Every attacker source (location.hash/search, cookie,
 * clipboard, localStorage, IndexedDB, ...) routes its replay delivery through here, so a source that carries
 * taint but forgets to deliver a candidate — recording a sink it can never VERIFY — is impossible by
 * construction (the old bug where cookie.c silently lacked delivery while storage/idb had it). The source's
 * intrinsic browser transform (the WHATWG per-component percent-encode SET + its leading char, or raw for a
 * non-URL source) is declared once, so a candidate arrives at the sink exactly as the real source would. */
#ifndef ENGINE_HOST_SOLVER_SOURCE_H
#define ENGINE_HOST_SOLVER_SOURCE_H
#include "quickjs.h"

/* During an @S replay (a candidate is pinned), return the concrete candidate as THIS source delivers it:
   `prefix` prepended (the source's real leading char: "#" hash, "?" search, "" for a non-prefixed source), and
   if `url_encode` the WHATWG percent-encode set applied (`enc_backtick`/`enc_squote` select the fragment vs
   special-query tail). A non-URL source (cookie/clipboard/storage/idb) passes url_encode=0 -> delivered RAW.
   Returns JS_UNDEFINED when no replay is active, so the caller returns its own concolic {tag} source. */
JSValue source_candidate(JSContext *ctx, const char *prefix, int url_encode, int enc_backtick, int enc_squote);

#endif
