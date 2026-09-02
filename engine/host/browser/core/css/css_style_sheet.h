/* CSSOM §6.1 — a CSS STYLE SHEET, its §6.1.1 StyleSheet base and its §6.1.2 CSSStyleSheet interface.
 *
 * WHAT THIS OWNS is the OBJECT and its state items, not where sheets come from: §6.1's state list ("type",
 * "location", "parent CSS style sheet", "owner node", "owner CSS rule", "title", "disabled flag", ...) and the
 * two interfaces that read them. WHO CREATES ONE is a different standard every time — HTML §4.2.6's update a
 * style block for a `<style>`, §4.2.4's for a `<link>`, CSSOM's own `new CSSStyleSheet()` — so each of those
 * calls a creator with the properties its own algorithm specifies rather than this file knowing about any of
 * them. There are TWO creators and the difference between them is not a parameter: §6.2's create-a-CSS-style-
 * sheet MINTS AND THEN ADDS the sheet to a document's collection, and §6.1's create-a-constructed-CSSStyleSheet
 * mints and adds it to NOTHING — a constructed sheet has no owner node, is in no tree, and reaches a document
 * only through `adoptedStyleSheets`. core/css/style_sheet_list.c's add asserts exactly that from its own side.
 *
 * THE STATE ITEMS THIS MODELS ARE THE ONES SOMETHING READS, and that is a rule about READERS rather than about
 * spec sections — a field written by a creator and read by nobody is dead weight that reads as modelled. §6.1
 * lists thirteen. The CONSTRUCTED FLAG is modelled because it has a reader: §6.1.2's `insertRule` step 5 is "If
 * parsed rule is an @import rule, and the constructed flag is set, throw a SyntaxError DOMException", and that
 * member is built. The origin-clean flag, the disallow modification flag, the constructor document and the
 * stylesheet base URL are not, and each is waiting on a DIFFERENT reader: the first two on `replace` /
 * `replaceSync`, the constructor document on DOM's `adoptedStyleSheets`, and the stylesheet base URL on any
 * resolution of a relative `url()` — of which this build has none at all, so storing it would model nothing a
 * page can observe.
 *
 * `media` IS ABSENT, not stubbed, and BOTH of its halves are one absence. §6.1.1 declares `[SameObject,
 * PutForwards=mediaText] readonly attribute MediaList media`; §4.4's MediaList IS built now
 * (core/css/media_list.h), so what is missing is §6.1's MEDIA state item and the specification of it — HTML
 * §4.2.6 The style element says a `<style>`-created sheet's media is "The media attribute of element", and says
 * in the same table cell that this "is a reference to the (possibly absent at this time) attribute, rather than
 * a copy of the attribute's current value", so it is tracked the way the title is. §6.1's constructor specifies
 * its own from `CSSStyleSheetInit`'s `(MediaList or DOMString) media` — see the residual in css_style_sheet.c,
 * which is where the two meet: that member cannot be DECLARED today, so the constructor has nothing to set the
 * state item from and a state item with one creator and no specification is the dead weight above. A
 * `[SameObject]` getter minting an empty MediaList per read would answer that every sheet applies to every
 * medium AND hand back a different object each time, which is two wrong answers where the spec computes one. */
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

/* §6.2 CSS Style Sheet Collections' "CREATE A CSS STYLE SHEET" — step 1, "Create a new CSS style sheet object
   and set its properties as specified." The arguments ARE §6.1's state items, spelled out one per parameter
   rather than gathered into a struct on purpose: a state item this engine starts modelling must become a
   compile error at every creator, and a zero-initialised struct field is the opposite of that. `location` is a
   USVString or JS_NULL (null for an embedded sheet); the other three are the wrapper or rule object, or
   JS_NULL. The disabled flag is not a parameter because §6.2 gives it no creator-specified value — §6.1 says it
   is "either set or unset. Unset by default", and the ONE algorithm that specifies it at creation is §6.1's
   constructor, which is this component's own and does not come through here.
   Step 2's "run the add a CSS style sheet steps" (also §6.2's) IS here, because it is step 2 of this very
   algorithm — what is not here is any way to skip it. §6.1's constructor mints without adding, and it does that
   by not being this function rather than by passing a flag to it. OWNED: the caller frees. */
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
   HTML §4.2.6 The style element's create-a-CSS-style-sheet table says the CSS rules are "left
   uninitialized", and its own note says that "doesn't seem right. Presumably we should be using the element's
   child text content?" (whatwg/html issue #2997). Every engine uses the child text content, and HTML §4.2.7
   Interactions of styling and scripting REQUIRES it — "the style rules must be immediately made available to
   script" — and a sheet whose rules were genuinely uninitialised would
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

/* §6.1's CONSTRUCTED FLAG — set by §6.1's create a constructed CSSStyleSheet and by nothing else. It is reached
   by name for ONE reader, core/css/css_rule.c's insert, because §6.1.2's `insertRule` step 5 is the only step
   in this build that branches on it: "If parsed rule is an @import rule, and the constructed flag is set, throw
   a SyntaxError DOMException". The PARSED RULE is what that step is about and it exists only inside §6.4's
   insert-a-CSS-rule, which is why the question travels there rather than the rule travelling back here.
   The receiver is a CSSStyleSheet or this asserts — every caller has already brand-checked it. */
bool css_style_sheet_constructed(JSValueConst sheet);

/* §6.1's CSS RULES, as the very Array §6.1.2's [SameObject] `cssRules` shares — for the CASCADE, which resolves
   the author layer from the RULE OBJECTS. That sharing is the whole point: it is what makes `insertRule`,
   `deleteRule`, `selectorText =` and `rule.style.color =` change what `getComputedStyle` answers, and what
   keeps every one of those per-flow, because the Array's mutations are property writes the delta captures.
   OWNED. */
JSValue css_style_sheet_rules(JSContext *ctx, JSValueConst sheet);

#endif
