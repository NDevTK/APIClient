/* Document script inventory + bundle IDENTITY — Blink core/loader (the document's <script> set and its stable
 * hash). A PURE Lexbor DOM scan: collect the page's <script> elements, decide executability, and compute the
 * bundle id (FNV-1a over the OWN executable scripts) WITHOUT running anything. Extracted from main.c so identity
 * is a COMPONENT with a clean interface, not scheduler-monolith state — the first step of dissolving the boot
 * entanglement (identity no longer requires execution; the host reads the frontier key from a pure scan). */
#ifndef ENGINE_HOST_BROWSER_DOCUMENT_SCRIPTS_H
#define ENGINE_HOST_BROWSER_DOCUMENT_SCRIPTS_H
#include <lexbor/html/html.h>
#include <lexbor/dom/dom.h>

struct scr_ctx { lxb_dom_element_t **els; int n, cap; };   /* collected <script> elements (caller frees .els) */

/* Collect `dom`'s <script> elements into `out` (document order); caller frees out->els. Empty on any failure. */
void dom_collect_scripts(lxb_html_document_t *dom, struct scr_ctx *out);
/* Is this <script> EXECUTABLE JS (empty/js-MIME/"module" type)? A DATA block (json/ld+json/importmap/template)
   returns 0 — parsed-but-not-run, never part of the JS identity. Sets *is_mod for a module script. */
int script_is_exec(lxb_dom_element_t *el, int *is_mod);
int script_is_importmap(lxb_dom_element_t *el);   /* 1 if <script type="importmap"> (parsed, not executed) */
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
   Caller frees via doc_scripts_free. */
typedef struct { char **bodies; char **srcs; int n; } DocScripts;
DocScripts document_exec_scripts(lxb_html_document_t *dom);
void       doc_scripts_free(DocScripts *ds);

#endif
