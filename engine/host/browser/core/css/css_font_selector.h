/* css-fonts-4 §5.2 "Matching font styles" over a document's `@font-face` rules: the half of the family walk
 * that looks a name up "among fonts defined via @font-face rules", and the load its procedure starts for a
 * matched face ("If the font resource has not been loaded and the range of characters defined by the
 * unicode-range descriptor value includes the character in question, load the font"). Blink's CSSFontSelector.
 *
 * The load is css-fonts-4 §4.8.2 "Font fetching requirements": css-values-4 §4.5.4 "URL Processing Model"'s
 * fetch a style resource with destination "font" and CORS mode "cors", parked on the running flow like every
 * other subresource (engine_pending_resource_url). Layout never waits for it: css-fonts-4 §3.2 "The Font
 * Display Timeline"'s render with a fallback font face is what the installed walk already lays out.
 *
 * A document's fetch state is one record per realm keyed by the absolute URL, so two rules naming one
 * resource, and a flow's later layouts, ask once. Chrome 153 requests a face's URL once however many
 * elements use it. */
/* NAMED RESIDUAL, loaded faces are not used. Not covered: a reply's bytes are never decoded, so text set in a
 * web font keeps the installed fallback's metrics after the load, and every length a page measures from it
 * (offsetWidth, getBoundingClientRect, canvas measureText) is the fallback's where Chrome's is the web
 * font's. Next diff builds: WOFF2 decoding (Brotli and the glyf/loca transform) and WOFF's zlib tables into
 * the sfnt core/fonts/open_type_metrics.h reads, a per-document face the fallback list can hold beside an
 * installed one, and the relayout a finished load owes. Absence shows as a run in a web font measuring at
 * its fallback family's advances after the font reply has arrived. */
/* NAMED RESIDUAL, where a face is needed. Not covered: the first available font (a line box's strut, an `ex`
 * length) and a collapsible space start no load, and a shadow root's `@font-face` rules and CSS Font Loading
 * §3's manually added faces are not in the walk. Next diff builds: the first-available-font ask at the
 * metrics reads that need a face, and CSS Font Loading §4.2's font source. Absence shows as no font request
 * for a block whose only use of the family is its line height. */
#ifndef ENGINE_HOST_BROWSER_CORE_CSS_CSS_FONT_SELECTOR_H
#define ENGINE_HOST_BROWSER_CORE_CSS_CSS_FONT_SELECTOR_H

#include <stdint.h>

#include <lexbor/dom/dom.h>

#include "quickjs.h"

/* Declared once per agent; the per-realm record is installed through realm.h's one list. */
void css_font_selector_init(JSContext *ctx);
void css_font_selector_install(JSContext *ctx);
void css_font_selector_free(JSRuntime *rt);

/* §5.2's family walk for one element's computed `font-family`, over its document's applying `@font-face`
   rules and the installed families between them. NULL when the list names no `@font-face` family, which is
   the walk that can start no load. Requires an entered layout job whose cascade holds `style`'s document's
   sheet set. Owned by the caller. */
typedef struct CssFontSelectorWalk CssFontSelectorWalk;
CssFontSelectorWalk *css_font_selector_walk_open(lxb_dom_element_t *style);
void css_font_selector_walk_free(CssFontSelectorWalk *w);

/* §5.2 for one character the element's text is being laid out with: walks the families in order and starts
   the load of the first `@font-face` face whose unicode-range includes `cp`, unless an earlier installed
   family covers it. `cp` is a Unicode scalar value (asserted). Requires a running flow when it loads. */
void css_font_selector_walk_character(CssFontSelectorWalk *w, uint32_t cp);

#endif
