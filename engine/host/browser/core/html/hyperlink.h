/* HTMLHyperlinkElementUtils — HTML §4.6.3. See hyperlink.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_HYPERLINK_H
#define ENGINE_HOST_BROWSER_CORE_HTML_HYPERLINK_H

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

#endif
