/* HTML §2.5.4 "CORS settings attributes". See cors_settings_attribute.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_CORS_SETTINGS_ATTRIBUTE_H
#define ENGINE_HOST_BROWSER_CORE_HTML_CORS_SETTINGS_ATTRIBUTE_H

#include "core/html/enumerated_attribute.h"
#include "core/fetch/fetch.h"   /* Fetch §2.2.5 "Requests"' credentials mode, which the two answers below are */

/* §2.5.4's states. The names are the section's own ("Anonymous", "Use Credentials", and the No CORS state its
   defaults sentence names), and No CORS is the one with NO KEYWORD — which is not an omission but the whole
   reason §2.6.1's getter has a branch for a state with no associated keyword value: `<img>` with no
   `crossorigin` attribute reflects the empty string, not a word. */
enum { CORS_NO_CORS = 0, CORS_ANONYMOUS, CORS_USE_CREDENTIALS };

/* §2.5.4's attribute as §2.3.3 defines it — two keywords, and three special states that are NOT all the same:
   "the attribute's missing value default is the No CORS state, and its invalid value default and empty value
   default are both the Anonymous state". `crossorigin=""` is therefore Anonymous and reflects "anonymous",
   while an absent attribute reflects "". */
extern const EnumeratedAttribute CORS_SETTINGS_ATTRIBUTE;

/* §2.5.4's STATE OF ONE ELEMENT'S `crossorigin` CONTENT ATTRIBUTE — §2.3.3's determine the state run over the
   definition above, so the three special states are read off the one table rather than passed by each caller.
   It exists because every consumer below wants the STATE and none of them wants the definition: a caller that
   spelled `enumerated_attribute_state(el, "crossorigin", …)` for itself would be restating the attribute name
   and the three defaults, which is the second copy this file's own header paragraph refuses. */
int cors_settings_attribute_state(const lxb_dom_element_t *el);

/* ---- §2.5.4's OTHER HALF, WHICH IS TWO ANSWERS AND NOT ONE ------------------------------------------------
 *
 * THIS FILE'S BANNER USED TO END "THIS FILE STATES THE ENUMERATION AND NOTHING ELSE", on the argument that the
 * credentials mode "is a FETCH decision, and the engine holds no network policy by construction". The premise
 * is right and the conclusion was wrong, and the distinction is the one CLAUDE.md draws in as many words: the
 * engine STATES what a request IS and the trusted zone DECIDES what to do about it. What §2.5.4 and §2.5.1
 * define is a fact about a request an ELEMENT creates, derived from that element's own attribute — the engine
 * is the only party that can state it, and a zone that was never told cannot decide from it. So the sentence
 * is retired rather than deleted: the thing it refused to state is stated here, and the reason it gave now
 * argues the other way.
 *
 * THE TWO ANSWERS DISAGREE, WHICH IS WHY THEY ARE TWO ENTRIES AND NOT ONE WITH A FLAG. They give the No CORS
 * state OPPOSITE values, and §2.5.4 says so itself: "For more modern features, where the request's mode is
 * always `cors`, certain CORS settings attributes have been repurposed to have a slightly different meaning,
 * wherein they only impact the request's credentials mode." A single function with a boolean would be one
 * predicate answering two questions, decided by whichever caller was written first — and the caller that lost
 * would send a bare `<link rel=preload>` uncredentialed, or a `<link rel=modulepreload>` credentialed, with
 * nothing anywhere to say which question had been asked. */

/* HTML §2.5.1 "Terminology"'s CREATE A POTENTIAL-CORS REQUEST, as the credentials mode it computes and no
   other part of it: "Let credentialsMode be `include`." then "If corsAttributeState is Anonymous, set
   credentialsMode to `same-origin`." So No CORS and Use Credentials are BOTH `include`, which is what makes a
   plain `<img src>` carry cookies in a real browser.
   IT ANSWERS ONE OF THE ALGORITHM'S OUTPUTS AND NAMES THE REST. §2.5.1 also computes a MODE — "Let mode be
   `no-cors` if corsAttributeState is No CORS, and `cors` otherwise" — and returns "a new request whose URL is
   url, destination is destination, mode is mode, credentials mode is credentialsMode, and whose use-URL-
   credentials flag is set". THE MODE IS CARRIED NOW AND THE USE-URL-CREDENTIALS FLAG IS NOT, which is this
   sentence's own rule coming true rather than an exception to it: it said a field a producer writes and
   nothing reads is a defect, so each arrives with its first consumer — and the mode's first consumer turned
   up — Subresource Integrity's integrity-policy check, which Fetch §4.1 "Main fetch" step 7 runs, reads it in
   a conjunction with the request's integrity metadata. The flag still has none. SECURITY.md's division is
   unchanged and is why the mode does NOT cross the wire: the trusted zone still makes the SOP/CORS decision
   from the request's own origin and the reply's headers, and the mode's only reader is inside this engine.
   Callers: HTML §4.8.4.3.5 "Updating the image data", HTML §4.2.4.3 "Fetching and processing a resource from
   a link element"'s create a link request, and HTML §9.2.2 "The EventSource interface"' constructor. */
FetchCredentialsMode cors_potential_request_credentials(int cors_attribute_state);

/* HTML §2.5.1 "Terminology"'s CREATE A POTENTIAL-CORS REQUEST, as the MODE it computes — "Let mode be
   `no-cors` if corsAttributeState is No CORS, and `cors` otherwise".
   THIS IS THE ARRIVAL THE PARAGRAPH ABOVE PREDICTED, and it is worth saying so rather than quietly adding a
   function: that paragraph says the mode is not carried "because neither has a reader … so each arrives with
   its first consumer", and the consumer turned up — Subresource Integrity §3.8.2's integrity-policy check,
   which Fetch §4.1 "Main fetch" step 7 runs, reads the request's mode in a CONJUNCTION with its integrity
   metadata. The field is carried now because that is true, and for no other reason; nothing else in this
   engine reads it, and SECURITY.md still gives the SOP/CORS decision to the trusted zone.
   IT IS A SECOND ENTRY BESIDE THE CREDENTIALS ONE AND NOT A SECOND RETURN FROM IT, for the reason those two
   are already two: one call answering two questions is read into a variable named for the other, and these
   two do not even agree about the No CORS state — it is `include` credentials and `no-cors` mode. */
FetchMode cors_potential_request_mode(int cors_attribute_state);

/* HTML §2.5.4 "CORS settings attributes"' CORS SETTINGS ATTRIBUTE CREDENTIALS MODE — "we define the CORS
   settings attribute credentials mode for a given CORS settings attribute to be determined by switching on the
   attribute's state", whose table gives No CORS and Anonymous `same-origin` and Use Credentials `include`.
   Caller: HTML §4.6.8.12 Link type "modulepreload", whose own step is "Let credentials mode be the CORS
   settings attribute credentials mode for el's crossorigin attribute". */
FetchCredentialsMode cors_settings_attribute_credentials(int cors_attribute_state);

#endif
