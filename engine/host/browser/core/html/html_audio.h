/* THE `audio` ELEMENT'S LEGACY FACTORY FUNCTION — HTML §4.8.9 "The audio element", Web IDL §3.7.2 "Legacy
 * factory functions".
 *
 * WHY THIS IS ITS OWN COMPONENT AND NOT A LINE IN THE MEDIA ELEMENT. §4.8.9's IDL is
 * `[Exposed=Window, LegacyFactoryFunction=Audio(optional DOMString src)] interface HTMLAudioElement :
 * HTMLMediaElement { [HTMLConstructor] constructor(); };` — two ways to construct, and they are two different
 * algorithms. The `[HTMLConstructor]` one is HTML §3.2.3 "HTML element constructors", shared and overridden
 * across every HTML element interface, and it lives with the custom-element machinery that owns the definition
 * set and the construction stack it reads. `Audio(src)` is not that algorithm and is not a constructor at all:
 * it is a global NAME carrying five steps of §4.8.9's own, and HTML §4.8.11 "Media elements"'s state machine
 * is not where a global name's list of steps belongs. The `img` twin says the same thing by standing in
 * core/html/html_image.h rather than in the element-interface table.
 *
 * A LEGACY FACTORY FUNCTION IS NOT AN ALIAS FOR createElement, AND THE DIFFERENCES ARE OBSERVABLE. §4.8.9's
 * steps set `preload` to `auto` on every element the factory makes, which `document.createElement("audio")`
 * does not — so `new Audio().preload` is "auto" and `document.createElement("audio").preload` is the empty
 * string, and the two elements go on to make different network requests. The document is the CURRENT global
 * object's, and §3.7.2 binds the realm AT CREATION — "Let F be CreateBuiltinFunction(steps, length, id, « »,
 * realm)" — which for a C function in this engine is the realm that DEFINED it, the realm `js_callee_realm`
 * answers. So the function object is minted per realm rather than held in a static, and every realm's `Audio`
 * answers with its own document. And §3.7.2 gives the object a `prototype` property that is neither writable
 * nor configurable and carries no `constructor` back-pointer, so `Audio.prototype.constructor` stays
 * `HTMLAudioElement`; a call without `new` is a TypeError from §3.7.2's own first step.
 *
 * WHAT `new Audio(url)` IS FOR, WHICH IS WHY IT MATTERS TO THIS ENGINE AND NOT ONLY TO CONFORMANCE. It is how
 * a page loads audio it never inserts into the tree, so the address it names reaches no parser, no attribute
 * in the serialized markup and no static read of the document — it exists only in the running code. That is
 * exactly the surface forced execution is for: the `src` argument crosses as the VALUE the page computed
 * rather than as a C string this file formats, so a URL built out of a config field or an API reply keeps its
 * provenance into HTML §4.8.11.5 "Loading the media resource"'s resource selection algorithm and out to the
 * @H endpoint surface.
 *
 * WHAT IS ABSENT AND IS NOT THIS FILE'S TO ANSWER: the two OTHER legacy factory functions HTML declares are
 * `Image` (core/html/html_image.h, built) and `Option` (HTML §4.10.10 "The option element", not built). The
 * `Option` steps need two things this build does not have — an internal create-a-Text-node-and-append that is
 * not the `createTextNode` member's step machine, and §4.10.10's SELECTEDNESS as a slot separate from the
 * `selected` content attribute, which core/html/html_form.c records is the same boolean here only because
 * nothing in this build can make an option's dirtiness true. A page writing `new Option(...)` gets its own
 * TypeError, which is the forcing function; there is nothing here that pretends otherwise. */
#ifndef APICLIENT_CORE_HTML_HTML_AUDIO_H
#define APICLIENT_CORE_HTML_HTML_AUDIO_H

#include "quickjs.h"

/* §4.8.9's AGENT-WIDE DECLARATION — the argument list of the legacy factory function, which is one optional
   `DOMString` whose ToString may run the page's own `toString` before the body is entered. Called once per
   agent from core/html/html_element.c's declare, which is the file that owns the element-interface table
   HTMLAudioElement is a row of and the list of global names this build carries. */
void html_audio_declare(JSContext *ctx);

/* Web IDL §3.7.2's legacy factory function object for `Audio`, on THIS REALM's global. `proto` is this realm's
   HTMLAudioElement.prototype, which §3.7.2 makes the factory's non-configurable `prototype` property; it is a
   parameter because core/html/html_element.c's per-tag loop is what owns that object, exactly as
   html_image_install_global is handed one. There is no prototype-member install beside this, because §4.8.9
   declares no member of its own: HTMLAudioElement's whole surface is HTMLMediaElement's. */
void html_audio_install_global(JSContext *ctx, JSValueConst global, JSValueConst proto);

/* The agent's declaration, given back at teardown — core/platform.h's release column. */
void html_audio_free(JSRuntime *rt);

#endif /* APICLIENT_CORE_HTML_HTML_AUDIO_H */
