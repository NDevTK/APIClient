/* SECURE CONTEXTS — the W3C standard's two trustworthiness algorithms, and HTML §8.1.3.5's use of them.
 *
 * WHAT THIS DECIDES IS WHETHER A MEMBER EXISTS, not whether it throws. Web IDL §3.3.13's [SecureContext] takes
 * a construct OUT OF THE REALM: "in a non-secure context there will be no `secretBoolean` property on
 * ExampleFeature.prototype". So a bundle's `if (navigator.deviceMemory)` routes one way on an https page and
 * the other on an http one, and an engine that answers the same in both explores half the code it was pointed
 * at. That is why this is a fact and not a flag.
 *
 * IT IS A FACT ABOUT AN ENVIRONMENT, AND THIS ENGINE'S ENVIRONMENT IS ITS REALM. HTML §8.1.3.5 does not ask
 * about the document's own address: it asks whether the environment's TOP-LEVEL CREATION URL is potentially
 * trustworthy. That is the ancestral rule Secure Contexts §4.2 exists for — a securely-delivered document
 * inside an insecurely-delivered top-level page is NOT a secure context, because otherwise an iframe plus
 * postMessage is a shim around every gated API. So the answer travels DOWN from the top-level environment at
 * creation, exactly as §7.2.6's policy container does, and the realm holds it (core/realm.h). A component that
 * asked the DOCUMENT's own URL would answer `true` for an `<iframe>` with no src inside an http page, because
 * §3.2 gives `about:blank` a free pass that only makes sense at the top.
 *
 * NOTHING HERE IS CONCOLIC AND THAT IS DELIBERATE. A document in this engine has a real address, so "is this
 * realm a secure context" is COMPUTED, never unknown external input — CLAUDE.md's line is that a value the
 * engine can compute must not be modelled as ignorance, because a fork whose sibling cannot exist is coverage
 * spent on a world that is not there. The other arm is reached by exploring a document at a DIFFERENT ADDRESS,
 * which is a real navigation and a real second document, not by making a known boolean opaque. */
#ifndef ENGINE_HOST_BROWSER_CORE_FRAME_SECURE_CONTEXT_H
#define ENGINE_HOST_BROWSER_CORE_FRAME_SECURE_CONTEXT_H

#include <stdbool.h>

#include "quickjs.h"
#include "core/url/url.h"

/* SECURE CONTEXTS §3.1 "Is origin potentially trustworthy?", over the URL RECORD whose origin it is.
 *
 * IT TAKES A RECORD AND NOT A SERIALIZED ORIGIN because the algorithm asks questions a serialization has
 * already thrown away: step 4 wants the host as a NUMBER (`127.0.0.0/8` matches `http://0x7f.1/`, which
 * url.c's parser resolves to 127.0.0.1) and as an IPv6 ADDRESS (`::1/128`), and step 5 wants the host as a
 * NAME (`localhost` and `*.localhost` are trusted BY NAME, never by what they resolve to — RFC6761 §6.3, and
 * §5.2's whole point). A `char *` origin would make every one of those a substring test. */
bool secure_context_origin_potentially_trustworthy(const UrlRecord *u);

/* SECURE CONTEXTS §3.2 "Is url potentially trustworthy?" over a serialized URL. False for input that is not a
   URL at all: a string with no origin cannot have a trustworthy one. */
bool secure_context_url_potentially_trustworthy(const char *url);

/* HTML §8.1.3.5 "is `ctx`'s environment a secure context?" — Is url potentially trustworthy? given this
   realm's environment's TOP-LEVEL CREATION URL (core/realm.h holds it, because the environment is created with
   the realm). The Worker and Worklet branches of §8.1.3.5 are not reachable here: this engine has no
   WorkerGlobalScope and no WorkletGlobalScope, so every environment it has is a Window one. */
bool secure_context_is(JSContext *ctx);

#endif
