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

/* THE `rel` SUPPORTED TOKENS OF §4.6.2 "Links created by a and area elements" AND §4.10.3 "The form element",
   one keyword at a time — the answer DOM §7.1 "Interface DOMTokenList"'s validation steps ask for, reached
   through core/html/supported_tokens.c.
   IT IS ANSWERED HERE BECAUSE THOSE TWO HTML SENTENCES ARE ONE SENTENCE about two element kinds: each names
   the same three possible keywords, "noreferrer, noopener, and opener", and filters them to those that
   "impact the processing model, and are supported by the user agent" (HTML §4.6.2, §4.10.3). That processing
   model is HTML §4.6.5 "Following hyperlinks"'s get-an-element's-noopener, which is ONE algorithm defined over
   "an a, area, or form element" and is this component's — so a per-row copy would be the second answer that is
   always subtly wrong.
   `token` is ONE keyword, already in ASCII lowercase (DOM §7.1's validation step 2). */
bool hyperlink_rel_supported(const char *token, size_t len);

/* HTML §4.6.8 "Link types"' DETERMINATION, asked one keyword at a time: "To determine which link types apply to
   a link, a, area, or form element, the element's rel attribute must be split on ASCII whitespace. The
   resulting tokens are the keywords for the link types that apply to that element." The comparison is that
   section's own — "Keywords are always ASCII case-insensitive, and must be compared as such."
   IT IS AN ENTRY RATHER THAN A PRIVATE HELPER because several standards read this ONE set: §4.6.5's
   get-an-element's-noopener below, §4.6.5 step 11's referrer policy, and §4.10.22.3 "Form submission
   algorithm"'s plan-to-navigate ("If the form element's link types include the noreferrer keyword, then set
   referrerPolicy to `no-referrer`"). `keyword` is ASCII lowercase. An element with no `rel` attribute has the
   empty set of link types and answers false for every keyword. */
bool hyperlink_link_types_include(JSContext *ctx, JSValueConst element, const char *keyword);

/* HTML §4.6.5 "Following hyperlinks"' GET AN ELEMENT'S NOOPENER, "given an a, area, or form element element, a
   URL record url, and a string target". It returns a boolean, and it is ONE algorithm with THREE element kinds
   and TWO callers in the standard — §4.6.5's own follow-the-hyperlink and §4.10.22.3 step 21 — which is why it
   is a component here and not an `if` at either of them.
   `target` is the string §4.2.3's GET AN ELEMENT'S TARGET already answered with, borrowed, and it is NOT
   NUL-terminated by contract: a navigable name is a DOMString and both callers carry it with a length.
   THE `url` ARGUMENT IS NOT HERE YET — see the residual at the definition. */
bool hyperlink_element_noopener(JSContext *ctx, JSValueConst element, const char *target, size_t target_len);

#endif
