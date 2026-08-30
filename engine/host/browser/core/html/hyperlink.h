/* HTMLHyperlinkElementUtils — HTML §4.6.3. See hyperlink.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_HYPERLINK_H
#define ENGINE_HOST_BROWSER_CORE_HTML_HYPERLINK_H

#include <stdbool.h>
#include <stddef.h>

#include "quickjs.h"

/* Install §4.6.3's members on an interface prototype that includes the mixin — HTMLAnchorElement and
   HTMLAreaElement. `href` RESOLVES against the document base and re-serialises, which is why it is not the
   plain attribute reflection it used to be. */
/* Declared once per AGENT; hyperlink_install then names the cached ids for each realm's prototype. */
void hyperlink_declare(JSContext *ctx);
void hyperlink_install(JSContext *ctx, JSValueConst proto);
/* Agent teardown, from the DOM group's cascade (core/html/html_element.c). It gives back §2.9's ACTIVATION
   BEHAVIOUR, which this file registered with the events layer: that slot is event_target.c's state pointing at
   this file's code, so the claimant is what releases it, and event_target_free asserts that it did. */
void hyperlink_free(void);

/* §4.6.2's `rel` SUPPORTED TOKENS, one keyword at a time — DOM §7.1 "Interface DOMTokenList"'s validation
   steps, reached through core/html/supported_tokens.c. It is answered here because §4.6.2 filters its three
   possible keywords to those that "impact the processing model, and are supported by the user agent", and
   §4.6.5's get-an-element's-noopener — the model that reads them — is this component's.
   `token` is ONE keyword, already in ASCII lowercase (§7.1's validation step 2). */
bool hyperlink_rel_supported(const char *token, size_t len);

#endif
