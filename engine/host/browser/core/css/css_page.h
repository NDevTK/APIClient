/* WHAT AN `@page` RULE MAY CONTAIN — CSS Paged Media Level 3's two closed membership questions, which are the
 * two facts CSSOM §6.4.7's CSSPageRule and §6.4.8's CSSMarginRule are built out of and which nothing else in
 * this engine can answer.
 *
 * THEY ARE ONE COMPONENT BECAUSE THEY ARE ONE SENTENCE. §4.3 states the whole of it: "The @page rule can only
 * contain page properties and margin at-rules. The margin at-rules can only contain page-margin properties."
 * That is a rule ABOUT THE CONTENTS of a page context, asked from two places that are otherwise unrelated —
 * core/css/css_rule.c decides which child rules a page rule keeps, and core/css/css_style_declaration.c
 * decides which declarations its block keeps — and a copy in each is a copy that can disagree about a name.
 * The PRELUDE is a different problem and lives in core/css/css_at_rule_prelude.h, because a prelude is a
 * GRAMMAR over a token stream and this is a table.
 *
 * A CLOSED LIST IS THE SPEC'S OWN SHAPE HERE, NOT A SHORTCUT. Appendix A is titled "Applicable CSS2.1
 * Properties" and says of its first list "this list defines the page properties" and of its second "this list
 * defines the page-margin properties"; §6 adds "other properties defined by [CSS2] do not apply in these
 * contexts" and then "behavior for properties not included in CSS 2.1 is undefined". So the sixty-odd names
 * are normative and everything else is the undefined tail — and the undefined tail is where every engine and
 * css/cssom/rule-restrictions.html agree: `@page { transform: scale(1) }` declares NOTHING, `rule.style.length`
 * is 0, and `setProperty("transform", …)` on the block does not take. Waving the tail through instead would
 * put a declaration in a block that no user agent has, which is a wrong `length`, a wrong `cssText` and a
 * wrong `getPropertyValue` — three answers that read exactly like right ones.
 *
 * A CUSTOM PROPERTY IS NOT IN EITHER LIST AND IS NOT REFUSED BY THEM. `--x` is not a CSS 2.1 property, so
 * Appendix A cannot name it and §6's "other properties defined by [CSS2]" cannot exclude it; CSS Variables §2
 * makes a custom property valid wherever a declaration is, and its whole point is that the user agent has no
 * opinion about it. The caller therefore asks this only about real properties — see the call site. */
#ifndef ENGINE_HOST_BROWSER_CORE_CSS_CSS_PAGE_H
#define ENGINE_HOST_BROWSER_CORE_CSS_CSS_PAGE_H

#include <stdbool.h>

/* WHICH CONTEXT a declaration is being read in. §6 gives the page context and the margin context different
   property lists and they are not nested one in the other — `content` and `z-index` apply in a margin box and
   not on the page box, and `size` is the other way round — so this is two answers and not a depth. */
typedef enum {
    CSS_PAGE_CONTEXT_PAGE = 0,      /* inside `@page` itself */
    CSS_PAGE_CONTEXT_MARGIN,        /* inside one of §5's margin at-rules */
} CssPageContext;

/* CSS Paged Media §4.3's sixteen MARGIN AT-RULES (`@top-left-corner` … `@right-bottom`), by the name §6.4.8's
   `name` attribute returns: the at-rule's identifier with NO `@`, ASCII-lowercased. `false` for every other
   at-keyword, which inside an `@page` means CSS Syntax drops the rule and outside one means the at-rule is
   unknown and is dropped for that reason instead. */
bool css_page_margin_at_rule(const char *name);

/* Does `name` — a property name, ASCII-lowercased, never a custom property — apply in `context`? Appendix A's
   two lists, plus the descriptors css-page-3 defines for the page context alone (§7's `size`, §7.1.2's
   `page-orientation`, §9's `marks` and `bleed`), which are why §6 says "this specification additionally
   defines the size property that only applies in the page context". */
bool css_page_property_applies(CssPageContext context, const char *name);

#endif
