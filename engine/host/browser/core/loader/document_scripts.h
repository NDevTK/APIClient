/* Document script inventory + bundle IDENTITY — Blink core/loader (the document's <script> set and its stable
 * hash). A PURE Lexbor DOM scan: collect the page's <script> elements, decide executability, and compute the
 * bundle id (FNV-1a over the OWN executable scripts) WITHOUT running anything. Extracted from main.c so identity
 * is a COMPONENT with a clean interface, not scheduler-monolith state — the first step of dissolving the boot
 * entanglement (identity no longer requires execution; the host reads the frontier key from a pure scan). */
#ifndef ENGINE_HOST_BROWSER_DOCUMENT_SCRIPTS_H
#define ENGINE_HOST_BROWSER_DOCUMENT_SCRIPTS_H
#include <lexbor/html/html.h>
#include <lexbor/dom/dom.h>
#include "core/loader/script_type.h"

struct scr_ctx { lxb_dom_element_t **els; int n, cap; };   /* collected <script> elements (caller frees .els) */

/* Collect `dom`'s <script> elements into `out` (document order); caller frees out->els. Empty on any failure. */
void dom_collect_scripts(lxb_html_document_t *dom, struct scr_ctx *out);
/* HTML §4.12.1's TYPE-STRING STEPS: which of the four algorithms this <script>'s content is (script_type.h).
   The `type` attribute is stripped of leading and trailing ASCII whitespace and matched ASCII
   case-insensitively; an absent or empty one is "text/javascript", i.e. a classic script. */
ScriptType script_block_type(lxb_dom_element_t *el);
/* The document's stable bundle id = FNV-1a over its OWN executable scripts (external src URLs + inline JS
   bodies), a pure DOM scan that runs NO script. The frontier key the host reads synchronously. */
unsigned document_bundle_id(lxb_html_document_t *dom);

/* The document's OWN executable scripts IN DOCUMENT ORDER, each its own program body — NEVER concatenated (that
   would leak per-<script> let/const scope and cannot represent scripts loaded later). Entry i is EITHER inline
   (bodies[i] is its text, srcs[i] NULL) OR external (srcs[i] is its URL, bodies[i] NULL until the host supplies
   it). External scripts used to be skipped outright, with a comment calling the fetch "later" — which meant a
   real page's own bundle, always a <script src>, was never run at all: the engine explored whatever inline glue
   the page happened to have and reported that as the page's surface. They occupy their POSITION here because
   classic scripts run in document order, and an external one between two inline ones must not be reordered.
   `types[i]` is entry i's HTML §4.12.1 type — CLASSIC or MODULE, never a non-executing one — and it is what
   decides which of §8.1.3.3's two algorithms runs it. It is here rather than recomputed at the compile because
   the element is the only thing that knows: by the time the scheduler holds a body, the <script> it came from
   is behind it, and a kind recomputed from the TEXT would be a guess about a fact the DOM already stated.
   Caller frees via doc_scripts_free. */
typedef struct { char **bodies; char **srcs; ScriptType *types; int n; } DocScripts;
DocScripts document_exec_scripts(lxb_html_document_t *dom);
void       doc_scripts_free(DocScripts *ds);

#endif
