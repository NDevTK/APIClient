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

/* The document's OWN inline executable scripts, each as its OWN program body (document order) — NEVER
   concatenated (that would leak per-<script> let/const scope and cannot represent scripts loaded later). The
   scheduler runs each as its own code flow, sharing globals through the COW baseline. Caller frees via
   doc_scripts_free. This is browser-layer script inventory feeding the one flow executor; there is no boot. */
typedef struct { char **bodies; int n; } DocScripts;
DocScripts document_exec_scripts(lxb_html_document_t *dom);
void       doc_scripts_free(DocScripts *ds);

#endif
