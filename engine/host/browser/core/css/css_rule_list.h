/* CSSOM §6.4.1 — CSSRuleList, "an ordered collection of CSS rules".
 *
 * IT IS A VIEW, NOT A STORE. The rules themselves belong to whatever holds them — a CSSStyleSheet's CSS rules
 * today, a CSSGroupingRule's nested ones when §6.4.5 lands — and this is the object §6.1.2's `cssRules` hands
 * back over that holder's Array. §6.1.2 says so in as many words: "even though the returned CSSRuleList object
 * is read-only, it can nevertheless change over time due to its liveness status. For example, invoking the
 * insertRule() or deleteRule() methods can result in mutations reflected in the returned object." Sharing the
 * Array OBJECT is what makes that true; copying it would freeze every collection ever handed out. */
#ifndef ENGINE_HOST_BROWSER_CORE_CSS_CSS_RULE_LIST_H
#define ENGINE_HOST_BROWSER_CORE_CSS_CSS_RULE_LIST_H

#include "quickjs.h"

void css_rule_list_init(JSContext *ctx);
/* §6.4.1's prototype for ONE realm — declared into core/realm.h's list. */
void css_rule_list_install_proto(JSContext *ctx);
/* `CSSRuleList` as a global. */
void css_rule_list_install(JSContext *ctx, JSValueConst global);
void css_rule_list_free(JSContext *ctx);

/* A collection over `rules`, which must be the very Array its holder keeps — see the header. CONSUMES `rules`.
   OWNED: the caller frees the collection. */
JSValue css_rule_list_new(JSContext *ctx, JSValue rules);

#endif
