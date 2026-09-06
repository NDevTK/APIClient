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
#include "core/url/origin.h"
#include "core/url/url.h"

/* SECURE CONTEXTS §3.1 "Is origin potentially trustworthy?", over the URL RECORD whose origin it is.
 *
 * IT TAKES A RECORD AND NOT A SERIALIZED ORIGIN because the algorithm asks questions a serialization has
 * already thrown away: step 4 wants the host as a NUMBER (`127.0.0.0/8` matches `http://0x7f.1/`, which
 * url.c's parser resolves to 127.0.0.1) and as an IPv6 ADDRESS (`::1/128`), and step 5 wants the host as a
 * NAME (`localhost` and `*.localhost` are trusted BY NAME, never by what they resolve to — RFC6761 §6.3, and
 * §5.2's whole point). A `char *` origin would make every one of those a substring test.
 *
 * WHICH URLs HAVE AN OPAQUE ORIGIN IS URL §4.7's RULE, and core/url/origin.c owns it — this asks that
 * component for the tuple (origin_tuple_url) and reads step 1's answer off "there is none". It used to
 * serialize §4.7's answer and run the URL parser over the bytes to get the parsed host back, which was the
 * last place in the engine where a lossy serialization stood between an algorithm and the thing it reads.
 *
 * AND THE ENTRY §3.1 ACTUALLY DECLARES IS BELOW, WITH THE CONSUMER THAT FINALLY ASKED FOR IT. §3.1 says
 * "Given an origin (origin)", and the entry above is that algorithm applied to the origin OF A URL — which
 * was the only shape anything in this tree asked it in until Mixed Content §4.3 "Does settings prohibit mixed
 * security contexts?" arrived, whose step 1 reads an environment settings object's ORIGIN.
 * THIS PARAGRAPH SAID THE ENTRY HAD NO CALLER AND THAT IT MUST ARRIVE WITH ONE, and both halves stood: it was
 * refused once, on exactly that ground, and is landed now in the same diff as the algorithm that reads it. A
 * predicate with no consumer is the write-with-no-reader defect, and the reader is what decides the shape —
 * which it did: Mixed Content §4.3 holds an `Origin *` and no URL, so the shape is the record and not a
 * serialization.
 * THE TWO ENTRIES SHARE STEPS 3 THROUGH 9 AND DIFFER ONLY IN STEP 1, and they are two entries rather than one
 * because the ONE thing they do differently is how they answer "is this origin opaque": the URL form asks
 * core/url/origin.h for a TUPLE and reads step 1's answer off "there is none", and the record form asks
 * origin_is_opaque. Routing the URL form through this one would use origin_of_url, which MINTS an
 * agent-lifetime record per call — origin.h says origin_tuple_url exists FOR THIS ALGORITHM so that a
 * [SecureContext] member check does not leave one origin behind per question asked — so the sharing is of the
 * TAIL, in one static function, and never of the entry. */
bool secure_context_origin_record_potentially_trustworthy(const Origin *o);
bool secure_context_origin_potentially_trustworthy(const UrlRecord *u);

/* SECURE CONTEXTS §3.2 "Is url potentially trustworthy?" over a serialized URL. False for input that is not a
   URL at all: a string with no origin cannot have a trustworthy one. */
bool secure_context_url_potentially_trustworthy(const char *url);

/* HTML §8.1.3.5 "Secure contexts" — is the environment of `ctx` a secure context, all three of its steps.
 * THE QUESTION IS NOT IN QUOTATION MARKS AND THAT IS THE NOTATION RULE, not a style preference: double quotes
 * in this tree are a STANDARD's words, engine/citegen reads them as such, and this one was an engine
 * paraphrase carrying the identifier `ctx` inside them — so the auditor could only report it as a run that
 * leaves HTML after one word, which is a finding a reader has to adjudicate by hand every time. The sibling
 * .c states the same rule about its own retired sentence; it is stated here because this is where it broke.
 *
 * WHICH ARM ANSWERS IS DECIDED BY THE REALM'S §3.3.8 [Global] INTERFACE, which core/realm.h holds because the
 * environment is created with the realm. Step 1.2 answers a WorkerGlobalScope from its owner's own answer;
 * step 1.3 answers a WorkletGlobalScope `true`; step 2 is the top-level-creation-URL test, and it is the arm
 * this file used to be ALL of.
 *
 * THE SENTENCE THAT STOOD HERE SAID THE OTHER TWO WERE UNREACHABLE, on the ground that this engine had no
 * WorkerGlobalScope and no WorkletGlobalScope and so every environment it had was a Window one — a claim about
 * the TREE beside reasoning about the STANDARD that was exactly right. The tree moved: a realm now states which
 * interface its global object implements, so a worker realm is a realm this engine can build, and the missing
 * arms stopped being a narrowing at that moment. What made it worse than an ordinary gap is that falling
 * through to step 2 does not fail — HTML §10.2.6.2 "Script settings for workers" gives a worker environment no
 * top-level creation URL at all, so the fall-through either reads a field that is not this environment's or
 * reads none, and Web IDL §3.3.13 [SecureContext] spends the answer on whether a member EXISTS. */
bool secure_context_is(JSContext *ctx);

#endif
