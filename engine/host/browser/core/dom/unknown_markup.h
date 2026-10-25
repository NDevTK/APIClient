/* The regions of a document whose children are the parse of unknown markup.
 *
 * A fragment sink (HTML §8.5.4 innerHTML and its siblings over HTML §13.4 "Parsing HTML fragments") handed an
 * unknown string parses its example, or nothing, into the target (fragment_markup_cstr). The resulting children
 * are one world of that markup; the region record says so, so a reader whose answer depends on what the markup
 * holds (HTML §7.2.2.3 "Named access on the Window object") asks the markup rather than the example's tree.
 *
 * The list is a per-realm JS array on the copy-on-write heap, so every flow sees the regions its own writes made.
 * Each entry is [region node's wrapper, markup]. */
#ifndef ENGINE_HOST_BROWSER_CORE_DOM_UNKNOWN_MARKUP_H
#define ENGINE_HOST_BROWSER_CORE_DOM_UNKNOWN_MARKUP_H

#include <lexbor/dom/dom.h>

#include "quickjs.h"

/* Declared once per agent from document_init; the per-realm list is built by unknown_markup_install_document. */
void unknown_markup_init(JSContext *ctx);
void unknown_markup_install_document(JSContext *ctx);
void unknown_markup_free(void);

/* The children `region` holds are now the parse of the unknown `markup`, replacing any earlier record for it.
   `markup` is borrowed and must be a concolic. */
void unknown_markup_note(JSContext *ctx, lxb_dom_node_t *region, JSValueConst markup);
/* Known markup replaced `region`'s children, so no record stands for it any more. */
void unknown_markup_forget(JSContext *ctx, lxb_dom_node_t *region);

/* How many regions the realm's document holds, and the i-th: its markup (owned) and its node. */
int     unknown_markup_count(JSContext *ctx);
JSValue unknown_markup_at(JSContext *ctx, int i, lxb_dom_node_t **region);

#endif
