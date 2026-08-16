/* CSSOM §6.1 — a CSS STYLE SHEET, its §6.1.1 StyleSheet base and its §6.1.2 CSSStyleSheet interface.
 *
 * WHAT THIS OWNS is the OBJECT and its state items, not where sheets come from: §6.1's state list ("type",
 * "location", "parent CSS style sheet", "owner node", "owner CSS rule", "title", "disabled flag", ...) and the
 * two interfaces that read them. WHO CREATES ONE is a different standard every time — HTML §4.2.6's update a
 * style block for a `<style>`, §4.2.4's for a `<link>`, CSSOM's own `new CSSStyleSheet()` — so each of those
 * calls `css_style_sheet_create` with the properties its own algorithm specifies rather than this file knowing
 * about any of them.
 *
 * THE STATE ITEMS THIS MODELS ARE THE ONES SOMETHING READS. §6.1 lists thirteen; the origin-clean flag, the
 * constructed flag, the disallow modification flag, the constructor document and the stylesheet base URL are
 * every one of them read by `cssRules` / `insertRule` / `deleteRule` / `replace` / the constructor, and none of
 * those exist yet — a field written by a creator and read by nobody is dead weight that reads as modelled. They
 * arrive with the member that asks them.
 *
 * `media` IS ABSENT, not stubbed, and the reason moved. §6.1.1 declares `[SameObject, PutForwards=mediaText]
 * readonly attribute MediaList media`; §4.4's MediaList IS built now (core/css/media_list.h), so what is
 * missing is no longer the object but §6.1's MEDIA state item itself and the specification of it — HTML
 * §4.2.6 says a `<style>`-created sheet's media is "the media attribute of element", tracked the way the
 * title is, and §6.1.2's `replace` and the constructor specify their own. A `[SameObject]` getter minting an
 * empty MediaList per read would answer that every sheet applies to every medium AND hand back a different
 * object each time, which is two wrong answers where the spec computes one. It arrives with the state item,
 * exactly as the other eight §6.1 items do — see the paragraph above. */
#ifndef ENGINE_HOST_BROWSER_CORE_CSS_CSS_STYLE_SHEET_H
#define ENGINE_HOST_BROWSER_CORE_CSS_CSS_STYLE_SHEET_H

#include <stdbool.h>
#include <stddef.h>

#include <lexbor/dom/dom.h>

#include "quickjs.h"

void css_style_sheet_init(JSContext *ctx);
/* §6.1's two INTERFACE PROTOTYPE OBJECTS for ONE realm — declared into core/realm.h's list. */
void css_style_sheet_install_proto(JSContext *ctx);
/* `StyleSheet` and `CSSStyleSheet` as globals. */
void css_style_sheet_install(JSContext *ctx, JSValueConst global);
void css_style_sheet_free(JSRuntime *rt);

/* §6.1's "CREATE A CSS STYLE SHEET" — step 1, "create a new CSS style sheet object and set its properties as
   specified". The arguments ARE §6.1's state items, spelled out one per parameter rather than gathered into a
   struct on purpose: a state item this engine starts modelling must become a compile error at every creator,
   and a zero-initialised struct field is the opposite of that.
   `location` is a USVString or JS_NULL (null for an embedded sheet); the other three are the wrapper or rule
   object, or JS_NULL. The disabled flag is not a parameter because §6.1 gives it no creator-specified value —
   it is "either set or unset. Unset by default", and every creator leaves it there.
   Step 2's "run the add a CSS style sheet steps" is NOT here: that adds the sheet to the document's list, which
   belongs to the component that owns the list. OWNED: the caller frees. */
JSValue css_style_sheet_create(JSContext *ctx, JSValueConst owner_node, JSValueConst parent_style_sheet,
                               JSValueConst owner_rule, JSValueConst location);

/* §6.2's "REMOVE A CSS STYLE SHEET". Step 2 — "set the CSS style sheet's parent CSS style sheet, owner node and
   owner CSS rule to null" — is the whole of what this engine can do today; step 1 removes it from the list of
   document CSS style sheets, and there is no such list, which this asserts against rather than states. */
void css_style_sheet_remove(JSContext *ctx, JSValueConst sheet);

/* Is `v` a CSSStyleSheet? The class brand, for a caller holding something it took off a slot. */
bool css_style_sheet_is(JSValueConst v);

/* §6.1's OWNER NODE, as a node — for §6.2's "at the appropriate location", which is tree order over the owner
   nodes, and for nothing else. BORROWED; NULL for a sheet that has none (one that has been removed, or one no
   node created). */
lxb_dom_node_t *css_style_sheet_owner_node(JSValueConst sheet);

/* CSS Syntax's "PARSE A STYLESHEET'S CONTENTS" over `text`, with the result becoming this sheet's CSS RULES —
   the operation §6.1.2's `replaceSync` is stated over, minus the constructed-flag check that member adds.
   HTML §4.2.6's create-a-CSS-style-sheet table says the CSS rules are "left uninitialized", and its own note
   says that "doesn't seem right. Presumably we should be using the element's child text content?" (whatwg/html
   issue #2997). Every engine uses the child text content, §4.2.6 further down REQUIRES it — "the style rules
   must be immediately made available to script" — and a sheet whose rules were genuinely uninitialised would
   make `document.styleSheets[0].cssRules` empty for every page. So this is what the creator calls. */
void css_style_sheet_set_rules_from_text(JSContext *ctx, JSValueConst sheet, const char *text, size_t len);

/* §6.1's TITLE, the CONCEPT — not §6.1.1's `title` attribute, which turns the empty string into null. §6.2's
   add-a-CSS-style-sheet steps branch on whether it is the empty string, which is why the concept is what is
   exposed. OWNED (a JS string). */
JSValue css_style_sheet_title(JSContext *ctx, JSValueConst sheet);

/* §6.1's DISABLED FLAG, reached by name because HTML §4.2.6's `HTMLStyleElement.disabled` is defined as a
   forwarding to it and not as a content-attribute reflection. Both go through this component's capture
   accessor, so the flag time-travels whichever member wrote it. */
bool css_style_sheet_disabled(JSValueConst sheet);
void css_style_sheet_set_disabled(JSValueConst sheet, bool disabled);

/* §6.1's CSS RULES, as the very Array §6.1.2's [SameObject] `cssRules` shares — for the CASCADE, which resolves
   the author layer from the RULE OBJECTS. That sharing is the whole point: it is what makes `insertRule`,
   `deleteRule`, `selectorText =` and `rule.style.color =` change what `getComputedStyle` answers, and what
   keeps every one of those per-flow, because the Array's mutations are property writes the delta captures.
   OWNED. */
JSValue css_style_sheet_rules(JSContext *ctx, JSValueConst sheet);

#endif
