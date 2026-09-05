/* HTMLElement AND THE PER-TAG INTERFACES — HTML §3.2.2 "Elements in the DOM" states the rule ("The nodes
 * representing HTML elements in the DOM must implement, and expose to scripts, the interfaces listed for them
 * in the relevant sections of this specification") and HTML §4's per-element definitions carry the list, one
 * `DOM interface:` line each — `a` names `HTMLAnchorElement`.
 *
 * WHY THIS LAYER EXISTS AT ALL. Everything an element carries used to sit on Element.prototype, which is the DOM
 * layer, and the DOM layer does not know what HTML is. Three concrete things were wrong because of it.
 * `src` was a reflection on EVERY element, so `div.src = u` set an attribute on a div and `script.src` and
 * `img.src` were the same property — while `a.href`, `form.action`, `input.value`, `iframe.srcdoc` and
 * `meta.content` did not exist at all, and those are the attributes this tool exists to find: an endpoint a
 * bundle writes into `form.action` was invisible. GlobalEventHandlers sat on Element although the IDL mixes it
 * into HTMLElement. And `x instanceof HTMLAnchorElement` — how a page tells a link from a button — had nothing
 * to name, because reading the global THREW (every one of these is on the platform-names list).
 *
 * THE TAG DECIDES THE INTERFACE, which is why node.c cannot answer it. node.c owns wrapper identity and keys its
 * prototype table by node TYPE; an element's interface is keyed by its LOCAL NAME, and that mapping is HTML's,
 * not the DOM's. So node.c asks this file, through one resolver, and stays the ONE place a wrapper is built.
 *
 * WHAT IS DATA AND WHAT IS CODE, AND WHICH HALF OF THE DATA IS GENERATED. The rows — a local name, its
 * interface name, and which of §3.2.2's steps decided the pair — are a fact the STANDARD states and this
 * codebase does not, so they are GENERATED into core/html/element_interfaces.h by engine/elemgen.mjs and a
 * hand-edit of them is a second answer that goes wrong the day the standard adds an element. The REFLECTIONS
 * are not: a reflection is a pair of names AND a type AND a range AND a default, which no two-column index
 * states, so generating one would be inventing it — they stay here, in IFACE_REFL, joined onto the rows by
 * interface name at init. An interface with no reflections is still an entry, because `instanceof` is
 * observable even when the member list is not; most of the seventy-odd interfaces are exactly that. Every
 * behaviour — how a reflection reads, how a boolean one unsets, how the prototype chain is built — is written
 * once in element.c and here.
 *
 * WHAT IS HONESTLY ABSENT. CSSOM VIEW §7's `partial interface HTMLElement` has LEFT this list: offsetParent,
 * offsetTop, offsetLeft, offsetWidth and offsetHeight are real members of core/html/html_element_view.c,
 * installed on the prototype this file builds. What remains of §7 is its SIXTH member, `scrollParent`, and it
 * is absent for one named reason rather than for want of a layout — its walk terminates by returning "the
 * scrollingElement for the element's document", and CSSOM VIEW §5's `scrollingElement` does not exist in this
 * engine. The IDL audit names it. `getBoundingClientRect` left the list before that: it is §6's, it is on
 * Element.prototype (core/dom/element_view.c), and every interface in
 * the table below inherits it. §4.8.11's media elements no longer belong on that list either — HTMLMediaElement
 * is a real state machine over a modelled device (core/html/media_element.c), and this file's table names it as
 * the PARENT of the two interfaces whose IDL inherits from it. A local name no row names is decided by
 * §3.2.2's LAST TWO steps and not by this table — HTMLElement if it is a valid custom element name, and
 * HTMLUnknownElement otherwise, which is what HTML says for an element it does not know rather than a shrug. */
#include <string.h>

#include <lexbor/html/html.h>   /* <template>'s content fragment — §4.12.3 */

#include "check.h"
#include "quickjs.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "core/dom/node.h"
#include "core/dom/document.h"   /* §8.1.8.1 step 3's active document and step 4's relevant global object */
#include "core/dom/element.h"
#include "core/dom/dom_token_list.h"
#include "core/html/hyperlink.h"
#include "core/html/html_iframe.h"
#include "core/html/html_style_element.h"
#include "core/html/html_script.h"
#include "core/html/html_image.h"
#include "core/html/html_audio.h"
#include "core/html/html_option.h"
#include "core/html/html_link.h"
#include "core/html/html_base_element.h"
#include "core/html/autofill.h"   /* §4.10.19.7.2's IDL-exposed autofill value */
#include "core/html/form_submission_attributes.h"   /* §4.10.19.6's `action` and `formAction` */
#include "core/html/nonce_attribute.h"   /* §2.5.6's `nonce`, whose getter is not a reflection */
#include "core/html/html_meter.h"
#include "core/html/html_progress.h"
#include "core/events/event_target.h"
#include "core/html/custom_elements.h"
#include "core/dom/slot.h"
#include "core/html/element_internals.h"
#include "core/html/focus.h"
#include "core/html/html_dialog.h"
#include "core/html/popover.h"
#include "core/html/media_element.h"
#include "core/html/html_element.h"
#include "core/html/element_interfaces.h"   /* §3.2.2's rows, generated — see engine/elemgen.mjs */
#include "core/css/css_style_declaration.h"
#include "core/html/form_data_event.h"
#include "core/html/html_form.h"
#include "core/html/dom_string_map.h"
#include "core/html/global_attributes.h"
#include "core/html/cors_settings_attribute.h"     /* §2.5.4, reflected by <link>, <script>, <img> */
#include "core/html/lazy_loading_attribute.h"      /* §2.5.7, reflected by <img> and <iframe> */
#include "core/html/referrer_policy_attribute.h"   /* §2.5.5, reflected by five of the tables below */
#include "core/html/directionality.h"              /* §3.2.6.4's `dir` states, shared with its own walks */
#include "core/html/html_element_view.h"
#include "core/html/declarative_shadow.h"

static JSClassID g_html_class;      /* HTMLElement.prototype's per-realm slot */
static JSClassID g_unknown_class;   /* HTMLUnknownElement — HTML's answer for a tag it does not know */
static int g_html_ready;
static JSAtom  g_dataset_key = JS_ATOM_NULL;   /* the [SameObject] dataset cache slot on an element's wrapper */

/* §6.8.9's `inputmode`. The section's table has a Keyword column and NO State column and states NO defaults at
   all, so each keyword maps to a state of its own name and §2.3.3's two "return no state" arms are what an
   absent or unrecognised value reaches — which is why `<div>.inputMode` and `<div inputmode=banana>.inputMode`
   are both the empty string under §2.6.1's getter and not the word the page wrote. */
enum { IM_NONE = 0, IM_TEXT, IM_TEL, IM_URL, IM_EMAIL, IM_NUMERIC, IM_DECIMAL, IM_SEARCH };
static const EnumeratedKeyword INPUTMODE_KW[] = {
    { "none", IM_NONE }, { "text", IM_TEXT }, { "tel", IM_TEL }, { "url", IM_URL },
    { "email", IM_EMAIL }, { "numeric", IM_NUMERIC }, { "decimal", IM_DECIMAL }, { "search", IM_SEARCH },
    { NULL, 0 }
};
static const EnumeratedAttribute INPUTMODE_ATTR = {
    INPUTMODE_KW, ENUMERATED_NO_STATE, ENUMERATED_NO_STATE, ENUMERATED_NO_STATE
};

/* §6.8.10's `enterkeyhint` — the same shape as `inputmode` above, keywords with no declared defaults. */
enum { EKH_ENTER = 0, EKH_DONE, EKH_GO, EKH_NEXT, EKH_PREVIOUS, EKH_SEARCH, EKH_SEND };
static const EnumeratedKeyword ENTERKEYHINT_KW[] = {
    { "enter", EKH_ENTER }, { "done", EKH_DONE }, { "go", EKH_GO }, { "next", EKH_NEXT },
    { "previous", EKH_PREVIOUS }, { "search", EKH_SEARCH }, { "send", EKH_SEND },
    { NULL, 0 }
};
static const EnumeratedAttribute ENTERKEYHINT_ATTR = {
    ENTERKEYHINT_KW, ENUMERATED_NO_STATE, ENUMERATED_NO_STATE, ENUMERATED_NO_STATE
};

/* §6.12's `popover` is NOT declared here. Its keyword table and its three special states are
   core/html/popover.h's `POPOVER_ATTRIBUTE`, for the reason `dir`'s definition is core/html/directionality.h's:
   the component that owns an attribute's ALGORITHMS owns its §2.3.3 definition, and §6.12's show popover, hide
   a popover and check popover validity all read that state. This file owns the table of which interface a tag
   wears and which reflections each carries, and the row below points at the one definition.
   THE THREE SPECIAL STATES ARE THREE DIFFERENT STATES — "the attribute's missing value default is the No
   Popover state, its invalid value default is the Manual state, and its empty value default is the Auto state"
   — which is the case core/html/enumerated_attribute.c's header records as having defeated an implementation
   that collapsed missing and invalid into one field. All three are observable through §2.6.1's getter: `<div>`
   reflects "" (No Popover has no keyword), `<div popover>` reflects "auto", `<div popover=x>` "manual". */

/* §4.6.8.20's PRELOAD DESTINATION and §4.6.8.12's MODULE PRELOAD DESTINATION, whose UNION §4.2.4 makes the
   keyword set of `<link as>`: "Each of the union of preload destinations and module preload destinations is a
   keyword for this attribute, mapping to a state of the same name." A preload destination is "fetch", "font",
   "image", "script", "style" or "track"; a module preload destination is "json", "style", "text", or a
   SCRIPT-LIKE destination, which Fetch §2.2.5 Requests defines as "audioworklet", "paintworklet", "script",
   "serviceworker", "sharedworker" or "worker". §4.2.4 declares no defaults, so an absent or unrecognised value
   is §2.3.3's no state and `link.as` reflects the empty string.
   THE LIST IS NOT FETCH'S DESTINATIONS. "document", "embed", "frame", "iframe", "manifest", "object",
   "report", "webidentity" and "xslt" are destinations and are NOT in either union, so a row for one of them
   would make `<link as=iframe>.as` answer "iframe" where the spec answers "". */
enum { LINK_AS_FETCH = 0, LINK_AS_FONT, LINK_AS_IMAGE, LINK_AS_SCRIPT, LINK_AS_STYLE, LINK_AS_TRACK,
       LINK_AS_JSON, LINK_AS_TEXT, LINK_AS_AUDIOWORKLET, LINK_AS_PAINTWORKLET, LINK_AS_SERVICEWORKER,
       LINK_AS_SHAREDWORKER, LINK_AS_WORKER };
static const EnumeratedKeyword LINK_AS_KW[] = {
    { "fetch", LINK_AS_FETCH }, { "font", LINK_AS_FONT }, { "image", LINK_AS_IMAGE },
    { "script", LINK_AS_SCRIPT }, { "style", LINK_AS_STYLE }, { "track", LINK_AS_TRACK },
    { "json", LINK_AS_JSON }, { "text", LINK_AS_TEXT }, { "audioworklet", LINK_AS_AUDIOWORKLET },
    { "paintworklet", LINK_AS_PAINTWORKLET }, { "serviceworker", LINK_AS_SERVICEWORKER },
    { "sharedworker", LINK_AS_SHAREDWORKER }, { "worker", LINK_AS_WORKER },
    { NULL, 0 }
};
static const EnumeratedAttribute LINK_AS_ATTR = {
    LINK_AS_KW, ENUMERATED_NO_STATE, ENUMERATED_NO_STATE, ENUMERATED_NO_STATE
};

/* §4.8.4.3.4 Decoding images' IMAGE DECODING HINT keywords, which §4.8.3 makes `<img decoding>`'s: "This
   attribute's missing value default and invalid value default are both the Auto state." */
enum { DECODING_AUTO = 0, DECODING_SYNC, DECODING_ASYNC };
static const EnumeratedKeyword DECODING_KW[] = {
    { "sync", DECODING_SYNC }, { "async", DECODING_ASYNC }, { "auto", DECODING_AUTO }, { NULL, 0 }
};
static const EnumeratedAttribute DECODING_ATTR = {
    DECODING_KW, DECODING_AUTO, DECODING_AUTO, DECODING_AUTO
};

/* §4.8.10's `<track kind>`: "The attribute's missing value default is the subtitles state, and its invalid
   value default is the metadata state" — two different states, so `<track>` reflects "subtitles" and
   `<track kind=bogus>` reflects "metadata". */
enum { TRACK_SUBTITLES = 0, TRACK_CAPTIONS, TRACK_DESCRIPTIONS, TRACK_CHAPTERS, TRACK_METADATA };
static const EnumeratedKeyword TRACK_KIND_KW[] = {
    { "subtitles", TRACK_SUBTITLES }, { "captions", TRACK_CAPTIONS },
    { "descriptions", TRACK_DESCRIPTIONS }, { "chapters", TRACK_CHAPTERS },
    { "metadata", TRACK_METADATA }, { NULL, 0 }
};
static const EnumeratedAttribute TRACK_KIND_ATTR = {
    TRACK_KIND_KW, TRACK_SUBTITLES, TRACK_METADATA, TRACK_METADATA
};

/* HTMLElement's OWN reflections — §3.2.6's global attributes.
   NO `autocapitalize` HERE. §6.8.7 gives it GETTER STEPS rather than a reflection: they read the element's OWN
   AUTOCAPITALIZATION HINT, which falls through an empty attribute value to the element's FORM OWNER, and they
   name the answer for two of the five states explicitly because two keywords map to each of them ("off"/"none"
   and "on"/"sentences"), which is precisely the case §2.3.3's first canonical-keyword arm does not decide. It
   is a member of core/html/global_attributes.c beside the other globals whose getters walk the tree. */
static const ElReflect R_HTML[] = {
    { "title", "title", REFLECT_STRING }, { "lang", "lang", REFLECT_STRING },
    { "dir", "dir", REFLECT_ENUM, .en = &DIR_ATTRIBUTE },
    { "accessKey", "accesskey", REFLECT_STRING },
    { "enterKeyHint", "enterkeyhint", REFLECT_ENUM, .en = &ENTERKEYHINT_ATTR },
    { "inputMode", "inputmode", REFLECT_ENUM, .en = &INPUTMODE_ATTR },
    /* NO `nonce` HERE. §2.5.6 Nonce attributes gives the member its own two sentences and neither is a
       reflection: the getter returns the element's `[[CryptographicNonce]]` and the setter writes that slot,
       and the section adds a note that the row here made false — "Note how the setter for the nonce IDL
       attribute does not update the corresponding content attribute." Wrong in BOTH directions, which is why
       it is core/html/nonce_attribute.c and not a new kind in the reflection enum. */
    { "popover", "popover", REFLECT_ENUM, .en = &POPOVER_ATTRIBUTE },
    { "hidden", "hidden", REFLECT_BOOL }, { "inert", "inert", REFLECT_BOOL },
    { "autofocus", "autofocus", REFLECT_BOOL },
    /* §2.6.2 `[Reflect, ReflectRange=(0,8)] unsigned long` — no [ReflectDefault], so an unparseable or absent
       `headingoffset` answers the range's MINIMUM (0) and not a default. HTMLElement is inherited by every
       element interface, so this one row is the member on 62 of them. */
    { "headingOffset", "headingoffset", REFLECT_ULONG, 0, false, 0, 8, true },
    { "headingReset", "headingreset", REFLECT_BOOL },
};

/* THE PER-INTERFACE REFLECTIONS. Each list is what that interface's IDL declares and nothing else — which is
   the whole point of splitting them off Element.
   THE URL ROWS ARE REFLECT_URL AND WERE REFLECT_STRING. §2.6.2's `[ReflectURL]` is a `USVString` whose getter
   encoding-parses and serializes against the element's node document, so `<img src="/x">` reads back
   `https://host/x`; the mirror answered `/x` for twelve members across as many interfaces. The gap auditor
   could not see it — every one of them was INSTALLED and scored complete, and only the declared kind said what
   value it would answer with. A reflection is a pair of names AND a type; two of the three were right. */
/* NO `href` HERE, and the reason is not that a mirror cannot resolve one — core/dom/element.c's REFLECT_URL
   does exactly that. It is that §4.6.3's `href` is `[ReflectSetter]` and not `[ReflectURL]`: its getter is the
   URL RECORD's serialization, shared with the ten sibling members (`protocol`, `host`, `pathname`, …) that
   re-parse and write the same attribute back, and it falls back to the RAW attribute where §2.6.1 falls back to
   a scalar value string. Same for `<base href>` and `form.action`, which the IDL also declares
   `[ReflectSetter]` and whose getters have their own fallbacks (the fallback base URL, the document's URL). */
static const ElReflect R_ANCHOR[] = {
    { "target", "target", REFLECT_STRING },
    { "rel", "rel", REFLECT_STRING }, { "download", "download", REFLECT_STRING },
    { "hreflang", "hreflang", REFLECT_STRING }, { "type", "type", REFLECT_STRING },
    { "referrerPolicy", "referrerpolicy", REFLECT_ENUM, .en = &REFERRER_POLICY_ATTRIBUTE },
};
static const ElReflect R_AREA[] = {   /* `href` is §4.6.3's, as on <a> */
    { "target", "target", REFLECT_STRING },
    { "rel", "rel", REFLECT_STRING }, { "alt", "alt", REFLECT_STRING },
};
static const ElReflect R_LINK[] = {
    { "href", "href", REFLECT_URL }, { "rel", "rel", REFLECT_STRING },
    { "type", "type", REFLECT_STRING }, { "media", "media", REFLECT_STRING },
    { "as", "as", REFLECT_ENUM, .en = &LINK_AS_ATTR },
    { "crossOrigin", "crossorigin", REFLECT_ENUM_NULLABLE, .en = &CORS_SETTINGS_ATTRIBUTE },
    { "integrity", "integrity", REFLECT_STRING },
    { "referrerPolicy", "referrerpolicy", REFLECT_ENUM, .en = &REFERRER_POLICY_ATTRIBUTE },
    { "disabled", "disabled", REFLECT_BOOL },
};
/* NO `async` HERE. §4.12.1 defines its getter and setter steps over the element's `force async` boolean and not
   over the attribute — the getter answers true for an element that has no `async` attribute at all, and the
   setter's step 1 CLEARS that flag whatever value it is given — so a boolean reflection answers a different
   question in both directions. core/html/html_script.c owns it, beside the flag. */
static const ElReflect R_SCRIPT[] = {
    { "src", "src", REFLECT_URL }, { "type", "type", REFLECT_STRING },
    { "integrity", "integrity", REFLECT_STRING },
    { "crossOrigin", "crossorigin", REFLECT_ENUM_NULLABLE, .en = &CORS_SETTINGS_ATTRIBUTE },
    { "referrerPolicy", "referrerpolicy", REFLECT_ENUM, .en = &REFERRER_POLICY_ATTRIBUTE },
    { "defer", "defer", REFLECT_BOOL },
    { "noModule", "nomodule", REFLECT_BOOL },
};
static const ElReflect R_IMG[] = {
    { "src", "src", REFLECT_URL }, { "srcset", "srcset", REFLECT_STRING },
    { "sizes", "sizes", REFLECT_STRING }, { "alt", "alt", REFLECT_STRING },
    { "useMap", "usemap", REFLECT_STRING },
    { "crossOrigin", "crossorigin", REFLECT_ENUM_NULLABLE, .en = &CORS_SETTINGS_ATTRIBUTE },
    { "referrerPolicy", "referrerpolicy", REFLECT_ENUM, .en = &REFERRER_POLICY_ATTRIBUTE },
    { "loading", "loading", REFLECT_ENUM, .en = &LAZY_LOADING_ATTRIBUTE },
    { "decoding", "decoding", REFLECT_ENUM, .en = &DECODING_ATTR }, { "isMap", "ismap", REFLECT_BOOL },
};
/* `srcdoc` IS A REFLECT_STRING ROW AND STAYS ONE, WHICH IS WORTH WRITING DOWN BECAUSE ITS IDL SAYS OTHERWISE.
   §4.8.5 The iframe element spells the member's steps out instead of declaring it `[Reflect]`, and the IDL type
   is the union `(TrustedHTML or DOMString)` — so an audit that reads the IDL flags this row, and the flag is a
   false one in BOTH directions. The getter steps ARE §2.6.1's `DOMString` getter, verbatim: "Let attribute be
   the result of getting an attribute by namespace and local name given null, srcdoc's local name, and this. If
   attribute is null, then return the empty string. Return attribute's value." The setter steps are "Let
   compliantString be the result of invoking the get trusted type compliant string algorithm with TrustedHTML,
   this's relevant global object, the given value, "HTMLIFrameElement srcdoc", and "script". Set an attribute
   value given this, srcdoc's local name, and compliantString" — which is what js_el_reflect_set already does
   for every row, through core/html/trusted_types.c's §3.8 table, whose (html, iframe, srcdoc) entry carries
   that same sink name. WHAT THE UNION WOULD BUY IS AN ARGUMENT TYPE, AND IT HAS NOTHING TO DISTINGUISH: §2's
   TrustedHTML object is honestly absent from this agent, so every value the member can be handed crosses
   `DOMString`, which is exactly what the row declares. The day TrustedHTML exists this row becomes a component
   — its setter must brand-check before stringifying, and IDL_DOMSTRING would run the page's `toString` on a
   TrustedHTML and hand §3.4 a plain string it then refuses. */
static const ElReflect R_IFRAME[] = {
    { "src", "src", REFLECT_URL }, { "srcdoc", "srcdoc", REFLECT_STRING },
    { "name", "name", REFLECT_STRING }, { "allow", "allow", REFLECT_STRING },
    { "referrerPolicy", "referrerpolicy", REFLECT_ENUM, .en = &REFERRER_POLICY_ATTRIBUTE },
    { "loading", "loading", REFLECT_ENUM, .en = &LAZY_LOADING_ATTRIBUTE },
    { "allowFullscreen", "allowfullscreen", REFLECT_BOOL },
};
/* NO `action` AND NO `formAction` IN THESE THREE TABLES. HTML §4.10.19.6 Form submission attributes
   states a getter algorithm for each — an absent-or-empty attribute answers the node document's URL and
   a present one is encoding-parsed-and-serialized against that document — so they are
   core/html/form_submission_attributes.c and not mirrors. All three were REFLECT_STRING rows answering
   the raw attribute, which reported `/x` where a browser reports the absolute URL a page is about to
   post to, and "" where it reports the document's own address. The section's other four members ARE
   rows and stay below: each answers from the attribute alone, which is core/dom/element.h's test. */
static const ElReflect R_FORM[] = {
    { "method", "method", REFLECT_ENUM, .en = &HTML_FORM_METHOD_ATTRIBUTE },
    { "name", "name", REFLECT_STRING }, { "target", "target", REFLECT_STRING },
    { "enctype", "enctype", REFLECT_ENUM, .en = &HTML_FORM_ENCTYPE_ATTRIBUTE },
    { "acceptCharset", "accept-charset", REFLECT_STRING },
    { "autocomplete", "autocomplete", REFLECT_ENUM, .en = &HTML_FORM_AUTOCOMPLETE_ATTRIBUTE },
    { "rel", "rel", REFLECT_STRING },
    { "noValidate", "novalidate", REFLECT_BOOL },
};
/* NO `autocomplete` IN R_INPUT, R_TEXTAREA OR R_SELECT. HTML §4.10.19.7.2 Processing model ends "The
   autocomplete IDL attribute, on getting, must return the element's IDL-exposed autofill value" — a value
   that section DERIVES from the attribute through a 35-step algorithm over a 57-row token table, so the
   mirror was a wrong VALUE: its DEFAULT branch sets the IDL-exposed autofill value to the EMPTY STRING,
   and `<input autocomplete="banana">` reads "" in every browser and read "banana" here. It is
   core/html/autofill.c. §4.10.3's `autocomplete` ON A `form` ELEMENT is a DIFFERENT attribute — an
   enumerated on/off — and its REFLECT_ENUM row in R_FORM above is correct and stays. */
static const ElReflect R_INPUT[] = {
    { "name", "name", REFLECT_STRING },
    { "type", "type", REFLECT_ENUM, .en = &HTML_INPUT_TYPE_ATTRIBUTE },
    { "defaultValue", "value", REFLECT_STRING }, { "placeholder", "placeholder", REFLECT_STRING },
    { "pattern", "pattern", REFLECT_STRING }, { "accept", "accept", REFLECT_STRING },
    { "min", "min", REFLECT_STRING },
    { "max", "max", REFLECT_STRING }, { "step", "step", REFLECT_STRING },
    { "formMethod", "formmethod", REFLECT_ENUM, .en = &HTML_FORM_FORMMETHOD_ATTRIBUTE },
    { "formEnctype", "formenctype", REFLECT_ENUM, .en = &HTML_FORM_FORMENCTYPE_ATTRIBUTE },
    { "formTarget", "formtarget", REFLECT_STRING },
    { "src", "src", REFLECT_URL }, { "alt", "alt", REFLECT_STRING },
    { "disabled", "disabled", REFLECT_BOOL }, { "required", "required", REFLECT_BOOL },
    { "readOnly", "readonly", REFLECT_BOOL }, { "multiple", "multiple", REFLECT_BOOL },
    { "defaultChecked", "checked", REFLECT_BOOL }, { "formNoValidate", "formnovalidate", REFLECT_BOOL },
};
static const ElReflect R_BUTTON[] = {
    /* NO `type` HERE: §4.10.6 The button element gives it getter steps that read the SUBMIT-BUTTON
       predicate — the `command` and `commandfor` attributes and the PARENT — so it answers from more than
       this attribute and is core/html/html_form.c's beside that predicate. The mirror read "" for
       `<button>` where a browser reads "submit". */
    { "name", "name", REFLECT_STRING },
    { "value", "value", REFLECT_STRING },
    { "formMethod", "formmethod", REFLECT_ENUM, .en = &HTML_FORM_FORMMETHOD_ATTRIBUTE },
    { "disabled", "disabled", REFLECT_BOOL },
};
static const ElReflect R_TEXTAREA[] = {
    { "name", "name", REFLECT_STRING }, { "placeholder", "placeholder", REFLECT_STRING },
    { "wrap", "wrap", REFLECT_STRING },
    { "disabled", "disabled", REFLECT_BOOL }, { "required", "required", REFLECT_BOOL },
    { "readOnly", "readonly", REFLECT_BOOL },
};
static const ElReflect R_SELECT[] = {
    { "name", "name", REFLECT_STRING },
    { "disabled", "disabled", REFLECT_BOOL }, { "multiple", "multiple", REFLECT_BOOL },
    { "required", "required", REFLECT_BOOL },
};
/* NO `label` HERE: §4.10.10 The option element's getter falls through an absent attribute to the element's
   TEXT, so the mirror read "" for `<option>Blue</option>` where a browser reads "Blue". §4.10.9 The optgroup
   element's member of the same name over the same attribute IS a plain `[CEReactions, Reflect]` mirror and
   its row in R_OPTGROUP is correct — the same two-interfaces-one-name shape as `htmlFor`. */
static const ElReflect R_OPTION[] = {
    { "disabled", "disabled", REFLECT_BOOL }, { "defaultSelected", "selected", REFLECT_BOOL },
};
static const ElReflect R_LABEL[]  = { { "htmlFor", "for", REFLECT_STRING } };
static const ElReflect R_META[]   = {
    { "name", "name", REFLECT_STRING }, { "content", "content", REFLECT_STRING },
    { "httpEquiv", "http-equiv", REFLECT_STRING }, { "media", "media", REFLECT_STRING },
};
/* NO `href` HERE. §4.2.3 defines its getter as its own algorithm — parse the content attribute against the
   document's FALLBACK base URL, "thus, the base element isn't affected by other base elements or itself" —
   which is neither a string reflection (what stood here, answering the raw attribute) nor §2.6.1's URL
   reflection (which resolves against the document BASE url, so the second of two `<base href>` elements
   would report the FIRST one's answer). core/html/html_base_element.c owns it, beside the freeze. */
static const ElReflect R_BASE[]   = { { "target", "target", REFLECT_STRING } };
static const ElReflect R_SOURCE[] = {
    { "src", "src", REFLECT_URL }, { "type", "type", REFLECT_STRING },
    { "srcset", "srcset", REFLECT_STRING }, { "sizes", "sizes", REFLECT_STRING },
    { "media", "media", REFLECT_STRING },
};
static const ElReflect R_TRACK[]  = {
    { "src", "src", REFLECT_URL }, { "srclang", "srclang", REFLECT_STRING },
    { "label", "label", REFLECT_STRING },
    { "kind", "kind", REFLECT_ENUM, .en = &TRACK_KIND_ATTR },
    { "default", "default", REFLECT_BOOL },
};
/* §4.8.9's HTMLVideoElement declares TWO reflections of its own; the six `<audio>` and `<video>` share are
   HTMLMediaElement's, and they live on THAT prototype (core/html/media_element.c) rather than being repeated
   here per tag — which is what the IDL says and what makes `video.src` one member rather than two. */
static const ElReflect R_VIDEO[]  = {
    { "poster", "poster", REFLECT_URL }, { "playsInline", "playsinline", REFLECT_BOOL },
};
static const ElReflect R_OBJECT[] = {
    { "data", "data", REFLECT_URL }, { "type", "type", REFLECT_STRING },
    { "name", "name", REFLECT_STRING },
};
static const ElReflect R_EMBED[]  = { { "src", "src", REFLECT_URL }, { "type", "type", REFLECT_STRING } };
/* §4.2.6's `media` is the element's ONE plain reflection. `disabled` used to sit beside it as a
   REFLECT_BOOL and it is not a reflection at all — HTML defines no `disabled` content attribute on a style
   element, and the IDL attribute's getter and setter steps read and write the ASSOCIATED CSS STYLE SHEET's
   disabled flag. The reflection honoured `<style disabled>` (markup no browser acts on) and ignored the flag
   every page actually sets, in both directions and silently; core/html/html_style_element.c owns it now.
   `blocking` is a `[SameObject] DOMTokenList` and is absent rather than mis-spelled as a string. */
static const ElReflect R_STYLE[]  = { { "media", "media", REFLECT_STRING } };
/* NO `htmlFor` HERE, and the row that used to be is why the note is: §4.10.12 The output element declares
   `[SameObject, PutForwards=value, Reflect="for"] readonly attribute DOMTokenList htmlFor`, so it is a §7.1
   token list beside `relList` and `sandbox` and not a string mirror. §4.10.4 The label element's member of the
   SAME NAME over the SAME ATTRIBUTE is `[CEReactions, Reflect="for"] attribute DOMString`, and its row below
   is correct — which is the whole reason a reflection is declared per INTERFACE and never per member name. */
static const ElReflect R_OUTPUT[] = { { "name", "name", REFLECT_STRING } };
static const ElReflect R_FIELDSET[] = { { "name", "name", REFLECT_STRING }, { "disabled", "disabled", REFLECT_BOOL } };
static const ElReflect R_OPTGROUP[] = { { "label", "label", REFLECT_STRING }, { "disabled", "disabled", REFLECT_BOOL } };
static const ElReflect R_MAP[]    = { { "name", "name", REFLECT_STRING } };
static const ElReflect R_TIME[]   = { { "dateTime", "datetime", REFLECT_STRING } };
static const ElReflect R_QUOTE[]  = { { "cite", "cite", REFLECT_URL } };
static const ElReflect R_MOD[]    = { { "cite", "cite", REFLECT_URL }, { "dateTime", "datetime", REFLECT_STRING } };
static const ElReflect R_OL[]     = { { "type", "type", REFLECT_STRING }, { "reversed", "reversed", REFLECT_BOOL } };
static const ElReflect R_LI[]     = { { "type", "type", REFLECT_STRING } };
static const ElReflect R_TABLE[]  = { { "summary", "summary", REFLECT_STRING } };
/* §4.9.11's two SPANS. Both carry `[ReflectDefault=1]` and both carry a `[ReflectRange]`, and the pair is what
   makes the two steps distinguishable: `colSpan`'s range starts at 1 where its default also is, but
   `rowSpan`'s starts at 0 while its default is 1 — so `<td rowspan="0">` is 0 (in range) and `<td rowspan="x">`
   is 1 (unparseable, so the default). A single "fallback" number cannot express that. */
static const ElReflect R_TD[]     = {
    { "headers", "headers", REFLECT_STRING }, { "abbr", "abbr", REFLECT_STRING },
    { "colSpan", "colspan", REFLECT_ULONG, 1, true, 1, 1000, true },
    { "rowSpan", "rowspan", REFLECT_ULONG, 1, true, 0, 65534, true },
};
/* `span` was REFLECT_STRING, so `col.span` answered "" where the IDL declares `unsigned long` and a browser
   answers 1 — the thirteenth member found by diffing declared kinds against the IDL rather than by the audit,
   which counted it installed. */
static const ElReflect R_COL[]    = { { "span", "span", REFLECT_ULONG, 1, true, 1, 1000, true } };
/* §4.12.5 The canvas element: "The `width` and `height` IDL attributes must reflect the respective content
   attributes of the same name, with the same defaults." The defaults are the section's own two numbers —
   "The `width` attribute defaults to 300, and the `height` attribute defaults to 150" — and they are the whole
   of what a `[ReflectDefault]` carries here: the IDL declares `[CEReactions] attribute unsigned long` with NO
   `[ReflectRange]`, so §2.6.1's minimum stays 0 and its maximum stays 2147483647 and there is no clamp arm.
   BOTH ROWS WERE REFLECT_STRING, and that is a wrong VALUE in both directions rather than a lenient reading of
   one. The getter answered the attribute's bytes, so `<canvas>` read "" where a browser reads the NUMBER 300
   and `<canvas width="640">` read the STRING "640" — a page doing `c.width / 2` got NaN from the first and 320
   from the second, and `typeof c.width` was "string" for every canvas there has ever been. The setter wrote
   whatever it was handed, so `c.width = "640px"` stored those five characters where §2.6.1's unsigned long
   setter writes the DEFAULT (the given value is not in [0, 2147483647] once §3.2.4 has crossed it), and the
   getter then agreed with the attribute instead of with the browser. A member-presence audit scores both rows
   COMPLETE — the names are installed — and only the declared KIND says what value they answer with. */
static const ElReflect R_CANVAS[] = {
    { "width",  "width",  REFLECT_ULONG, 300, true },
    { "height", "height", REFLECT_ULONG, 150, true },
};
static const ElReflect R_DIALOG[] = { { "open", "open", REFLECT_BOOL } };
static const ElReflect R_DETAILS[]= { { "name", "name", REFLECT_STRING }, { "open", "open", REFLECT_BOOL } };
static const ElReflect R_SLOT[]   = { { "name", "name", REFLECT_STRING } };
/* HTML §16.3.2 Frames — `interface HTMLFrameSetElement : HTMLElement` declares exactly these two, both
   `[CEReactions, Reflect] attribute DOMString`. The interface exists here because §8.1.8.1's determine the
   target of an event handler names the FRAMESET element beside the body element, and §16.3.2 states
   `HTMLFrameSetElement includes WindowEventHandlers` for it: a `<frameset onload=…>` acts upon the Window
   exactly as a `<body onload=…>` does. Without the row the tag was HTMLUnknownElement, which has no place to
   put those eighteen members. */
static const ElReflect R_FRAMESET[] = {
    { "cols", "cols", REFLECT_STRING }, { "rows", "rows", REFLECT_STRING },
};
/* §4.12.3's FOUR PLAIN reflections. The other two shadow-root attributes are not plain mirrors:
   `shadowrootmode` and `shadowrootslotassignment` are enumerated and LIMITED TO ONLY KNOWN VALUES, so they
   live in declarative_shadow.c with the parser step that reads them.
   `shadowRootCustomElementRegistry` IS one, and the reason it used to be listed beside them was a claim about
   this tree rather than about the IDL: it was recorded as "absent with the registry interface", and that
   interface is here — `CustomElementRegistry` is constructible in core/html/custom_elements.c and
   declarative_shadow.c already reads this very content attribute for HTML §13.2.6.4.4's registry argument. The
   IDL never made it enumerated either: §4.12.3 declares `[CEReactions, Reflect] attribute DOMString
   shadowRootCustomElementRegistry` with NO "limited to only known values" line, and the spec says why in its
   own sentence — the member "intentionally does not have a boolean type so it can be extended" — so a plain
   `DOMString` mirror is the whole of its behaviour and REFLECT_STRING is that mirror. */
static const ElReflect R_TEMPLATE[] = {
    { "shadowRootDelegatesFocus", "shadowrootdelegatesfocus", REFLECT_BOOL },
    { "shadowRootClonable",       "shadowrootclonable",       REFLECT_BOOL },
    { "shadowRootSerializable",   "shadowrootserializable",   REFLECT_BOOL },
    { "shadowRootCustomElementRegistry", "shadowrootcustomelementregistry", REFLECT_STRING },
};
static const ElReflect R_DATA[]   = { { "value", "value", REFLECT_STRING } };
/* §4.10.14's `meter` HAS NO ROW HERE, and its absence is the point rather than an omission. It had one, holding
   `min` and `max` as REFLECT_STRING, and both were WRONG members rather than partial ones: §4.10.14 declares
   `attribute double min` whose getter steps are "return this's minimum value" — the NUMBER 0 for an absent
   attribute, where a string reflection answered "". A member-presence audit cannot see that, because the name
   was installed and only its type and its value were another thing. All six of the element's numbers are
   core/html/html_meter.c's, over §4.10.14's own six-point algorithm; `progress` is core/html/html_progress.c's
   for the same reason, and neither element reflects anything as a string. */

/* THE FOUR GlobalEventHandlers MEMBERS THIS USER AGENT MUST NOT HAVE. Touch Events Level 2 puts them behind a
   condition it states in PROSE and not in its IDL — "User agents have an associated boolean `expose legacy
   touch event APIs` whose value is implementation-defined", and then, at `Extensions to the GlobalEventHandlers
   mixin`: "For user agents where expose legacy touch event APIs is false, this mixin must not be implemented."
   The published `partial interface mixin GlobalEventHandlers` carries no [Exposed] and no marker, so the four
   reach @webref/idl unconditionally and an absent one is indistinguishable from an unbuilt one — which is
   exactly what idl_members_excluded exists to say. (The sections are cited by TITLE with no number because the
   Level 2 draft numbers none of its sections, and the 2013 Recommendation has no GlobalEventHandlers section at
   all; both are checkable at w3c.github.io/touch-events/ and www.w3.org/TR/touch-events/.)
   THIS AGENT'S FLAG IS FALSE, and not by preference: TouchEvent, Touch and TouchList are absent (they are names
   on browser/platform_names.h, so reading one is the ReferenceError that names the component to write), so
   there is nothing for a touch handler to be handed. Building Touch Events is what flips it, and deleting this
   declaration is part of that build — the auditor fails if the corpus drops a name here, and idl_members_excluded
   asserts per realm that the prototype indeed lacks each one.
   The same four are absent on Document and on Window, which include the same mixin (core/dom/document.c and
   core/frame/window.c each install EH_GLOBAL); each needs this declaration at its own install site, with its
   own interface name, because the auditor reads the name as a literal at the call. */
static const char *const TOUCH_EXCLUDED[] = { "ontouchstart", "ontouchend", "ontouchmove", "ontouchcancel" };

#define RL(a) (a), (int)(sizeof(a) / sizeof((a)[0]))

/* THE REFLECTIONS, BY INTERFACE — the half of the element-interface table that is NOT generated, and the join
   key is the interface NAME because that is what the IDL declares a member on. An interface with no row here
   has no reflections of its own, which is most of them: sixty-odd of §4's interfaces add nothing but their
   identity, and `instanceof` is observable even when the member list is not.
   engine/elemgen.mjs checks this list in the one direction that can rot silently — a set written for an
   interface no element-interface row wears installs members on a prototype no element can have, which no
   compiler and no IDL audit can see. */
static const struct { const char *iface; const ElReflect *refl; int nrefl; } IFACE_REFL[] = {
    { "HTMLAnchorElement",     RL(R_ANCHOR) },
    { "HTMLAreaElement",       RL(R_AREA) },
    { "HTMLLinkElement",       RL(R_LINK) },
    { "HTMLScriptElement",     RL(R_SCRIPT) },
    { "HTMLImageElement",      RL(R_IMG) },
    { "HTMLIFrameElement",     RL(R_IFRAME) },
    { "HTMLFormElement",       RL(R_FORM) },
    { "HTMLInputElement",      RL(R_INPUT) },
    { "HTMLButtonElement",     RL(R_BUTTON) },
    { "HTMLTextAreaElement",   RL(R_TEXTAREA) },
    { "HTMLSelectElement",     RL(R_SELECT) },
    { "HTMLOptionElement",     RL(R_OPTION) },
    { "HTMLOptGroupElement",   RL(R_OPTGROUP) },
    { "HTMLLabelElement",      RL(R_LABEL) },
    { "HTMLOutputElement",     RL(R_OUTPUT) },
    { "HTMLFieldSetElement",   RL(R_FIELDSET) },
    { "HTMLMetaElement",       RL(R_META) },
    { "HTMLBaseElement",       RL(R_BASE) },
    { "HTMLSourceElement",     RL(R_SOURCE) },
    { "HTMLTrackElement",      RL(R_TRACK) },
    { "HTMLVideoElement",      RL(R_VIDEO) },
    { "HTMLObjectElement",     RL(R_OBJECT) },
    { "HTMLEmbedElement",      RL(R_EMBED) },
    { "HTMLStyleElement",      RL(R_STYLE) },
    { "HTMLMapElement",        RL(R_MAP) },
    { "HTMLTimeElement",       RL(R_TIME) },
    { "HTMLDataElement",       RL(R_DATA) },
    { "HTMLQuoteElement",      RL(R_QUOTE) },
    { "HTMLModElement",        RL(R_MOD) },
    { "HTMLOListElement",      RL(R_OL) },
    { "HTMLLIElement",         RL(R_LI) },
    { "HTMLTableElement",      RL(R_TABLE) },
    { "HTMLTableCellElement",  RL(R_TD) },
    { "HTMLTableColElement",   RL(R_COL) },
    { "HTMLCanvasElement",     RL(R_CANVAS) },
    { "HTMLDialogElement",     RL(R_DIALOG) },
    { "HTMLDetailsElement",    RL(R_DETAILS) },
    { "HTMLSlotElement",       RL(R_SLOT) },
    { "HTMLTemplateElement",   RL(R_TEMPLATE) },
    { "HTMLFrameSetElement",   RL(R_FRAMESET) },
};
#define IFACE_REFL_N ((int)(sizeof(IFACE_REFL) / sizeof(IFACE_REFL[0])))

/* HTML §3.2.2 Elements in the DOM's ELEMENT INTERFACE, AS ROWS — GENERATED, because they are a fact the
   STANDARD states and this codebase does not. `tag` is the local name; several tags share one interface, which
   is what the spec says and why the interface is named per row rather than derived from the tag. `step` is
   which of §3.2.2's seven steps decided the row, and it is carried rather than dropped because the ORDER of the
   first four steps is the algorithm: §3.2.2 step 1 answers HTMLUnknownElement for eight names a later step
   would otherwise decide, and step 3 answers HTMLPreElement for two that are not `pre`.
   THE TABLE USED TO BE HAND-WRITTEN AND HELD 71 OF THESE 141 ROWS. Every missing one answered
   HTMLUnknownElement — `<section>`, `<nav>`, `<main>`, `<article>` and thirty-five more where the spec says
   HTMLElement, and `<progress>`, `<datalist>`, `<menu>` and `<selectedcontent>` where it names an interface of
   their own — so `x instanceof HTMLElement` answered false for a third of the elements a page builds, and a
   stack trace named HTMLUnknownElement for a `<section>`. That is not a shortfall a member audit can see:
   engine/idlgen.mjs asks which MEMBERS an interface installs and has nothing to say about which TAG wears it.
   engine/elemgen.mjs is the instrument for that axis, and engine/elemgen.mjs --regen is the only writer of
   the header below. */
static const struct { const char *tag; const char *iface; int step; } HTML_IFACE[] = {
#define X(tag, iface, step) { tag, iface, step },
    HTML_ELEMENT_INTERFACES(X)
#undef X
};
#define HTML_IFACE_N ((int)(sizeof(HTML_IFACE) / sizeof(HTML_IFACE[0])))

/* ONE CLASS PER ROW, not one prototype: the prototypes are PER REALM (§3.7) and this table is not, so a table
   of prototypes could only ever hold one document's — the same reason node.c's type table holds class ids.
   Rows that share an interface NAME share its class, which is what makes `q instanceof HTMLQuoteElement` and
   `blockquote instanceof HTMLQuoteElement` both true in every realm. */
static JSClassID g_iface_class[HTML_IFACE_N];
/* Each row's reflection BASE index and the set it was declared from — the JOIN between the generated rows and
   the hand-written IFACE_REFL above, resolved ONCE per agent by interface name rather than at every install. */
static int g_iface_refl_base[HTML_IFACE_N];
static const ElReflect *g_iface_refl[HTML_IFACE_N];
static int g_iface_nrefl[HTML_IFACE_N];
static int g_html_refl_base;

/* Which of the two BASE interfaces a row names, or 0. Rows naming them are not built by the per-interface loops
   — HTMLElement.prototype and HTMLUnknownElement.prototype are built and installed explicitly, once each — so
   every loop over the table asks this before it builds or installs anything. */
static int iface_is_base(const char *iface)
{
    if (!strcmp(iface, "HTMLElement")) return 1;
    if (!strcmp(iface, "HTMLUnknownElement")) return 2;
    return 0;
}

/* HTML §3.2.2 Elements in the DOM — "the element interface for an element with name name in the HTML
 * namespace", all seven steps, KEYED ON THE NAME ALONE. The answer is a BORROWED prototype read out of the
 * realm's own class-proto slot, so two documents get their own; Web IDL §3.7 Interfaces' per-realm
 * interface prototype object is why this file holds class ids and never prototypes.
 *
 * IT IS A NAME QUESTION AND NOT AN ELEMENT QUESTION, which is why it takes a string. §3.2.2 is asked twice by
 * the platform about names no element carries yet: HTML §4.13.4 The CustomElementRegistry interface step 7.3
 * asks it of `options.extends` before any element with that name exists ("if the element interface for extends
 * and the HTML namespace is HTMLUnknownElement … throw a NotSupportedError"), and HTML §3.2.3 HTML element
 * constructors step 8.2 asks the INVERSE of it — whether a definition's local name is one of the names this
 * interface serves. Both compare the ANSWER, which is one object per interface per realm, so neither needs an
 * interface NAME and neither can be fooled by a page reassigning one.
 *
 * THE PROTOTYPE IS THE INTERFACE'S IDENTITY HERE, deliberately. Web IDL §3.7.3 Interface prototype object gives
 * each interface exactly one per realm, so "the same interface" is a pointer comparison and never a strcmp on a
 * name a page can shadow — and html_element_install's own DCHECK already holds two rows naming one interface to
 * one class, which is what makes the pointer the whole answer. */
JSValue html_element_interface_proto(JSContext *ctx, const char *name, size_t n)
{
    int i;

    /* §3.2.2 STEPS 1 THROUGH 4, which are the generated rows. The steps are ORDERED and the rows are emitted in
       that order, so the first match is the standard's answer; the table's own tags are disjoint across the
       four steps, which the DCHECK in html_element_init asserts rather than leaves to the reader. */
    for (i = 0; i < HTML_IFACE_N; i++)
        if (strlen(HTML_IFACE[i].tag) == n && memcmp(HTML_IFACE[i].tag, name, n) == 0)
            return JS_GetClassProto(ctx, g_iface_class[i]);
    /* §3.2.2 STEP 5 — "If other applicable specifications define an appropriate interface for name, then return
       the interface they define." No specification this agent implements defines an HTML-NAMESPACE element
       interface of its own; the ones that define element interfaces at all (SVG, MathML) define them for their
       own namespaces, which the caller's namespace gate has already sent elsewhere. A component that adds one
       adds it here.
       §3.2.2 STEP 6 — a VALID CUSTOM ELEMENT NAME is HTMLElement and not HTMLUnknownElement, so that a later
       upgrade moves the element DOWN its prototype chain rather than sideways. The predicate is HTML §4.13.3 Core
       concepts', asked of the component that owns it: a hyphen is only the fourth of its five requirements, so a `-`
       search answered HTMLElement for `foo-BAR` (an ASCII upper alpha), for `å-bar` (not a valid element local
       name) and for `annotation-xml` (one of the eight reserved names) — three answers WPT
       html/semantics/interfaces.html asserts the other way. */
    if (custom_elements_name_is_valid(name, n))
        return JS_GetClassProto(ctx, g_html_class);
    return JS_GetClassProto(ctx, g_unknown_class);   /* §3.2.2 STEP 7 */
}

/* THE SAME QUESTION ASKED OF AN ELEMENT — the namespace gate, then the name.
 *
 * THE NAMESPACE IS PART OF THE QUESTION, and it used to be missing: node.c hands this resolver EVERY element
 * node it wraps, whatever namespace it is in, and this answered out of HTML's table regardless — so
 * an `<a>` inside an `<svg>` was handed HTMLAnchorElement.prototype and answered `instanceof HTMLAnchorElement`
 * true, which is how a page tells a link from a shape. DOM §4.5 Interface Document states the default this
 * falls back to in its own sentence: "The element interface for any name and namespace is Element, unless
 * stated otherwise."
 *
 * NAMED RESIDUAL — SVG AND MATHML. An element in the SVG or MathML namespace is answered with Element, which is
 * DOM §4.5's default and is what the sentence above says for a namespace no other specification covers; it is
 * NARROWER than the platform, where SVG defines SVGAElement for that `<a>` and MathML defines MathMLElement.
 * What the next diff must leave standing is a core/svg/svg_element.c owning SVG's own name-to-interface rows,
 * and a node.c resolver seam that admits MORE THAN ONE resolver keyed by namespace — node_set_element_resolver
 * today holds exactly one and asserts it is claimed once, so widening it is part of that diff and not a thing
 * to reach for. Its absence shows as `document.createElementNS(SVG_NS, "a").constructor.name`
 * answering "Element" where a browser answers "SVGAElement" — and, before that, as the ReferenceError a page
 * gets for reading the global `SVGElement`, which is on browser/platform_names.h and is the forcing function
 * that names the component to write. */
static JSValue html_proto_for(JSContext *ctx, lxb_dom_element_t *el)
{
    size_t n = 0;
    const lxb_char_t *tag = lxb_dom_element_local_name(el, &n);

    if (lxb_dom_interface_node(el)->ns != LXB_NS_HTML)
        return element_proto(ctx);   /* DOM §4.5's default, and OWNED like every per-realm prototype read */
    DCHECK(tag != NULL && n != 0,
           "an HTML-namespace element carries no local name — DOM §1.4 Name validation's valid element "
           "local name is one or more code points, so an element with an empty one was built by something "
           "that is not the parser and not createElement");
    return html_element_interface_proto(ctx, (const char *)tag, n);
}

/* The prototype an interface NAME was built for. A lookup rather than a stored handful, because this table is
   the single source of which interfaces exist and a second list of them could disagree with it. */
static JSValue html_iface_proto(JSContext *ctx, const char *iface)
{
    int i;
    for (i = 0; i < HTML_IFACE_N; i++)
        if (strcmp(HTML_IFACE[i].iface, iface) == 0) return JS_GetClassProto(ctx, g_iface_class[i]);
    DFAIL("an interface prototype was asked for by a name the element-interface table does not list");
    return JS_UNDEFINED;
}

JSValue html_element_proto(JSContext *ctx)
{
    JSValue proto = JS_GetClassProto(ctx, g_html_class);
    DCHECK(!JS_IsNull(proto), "HTMLElement.prototype was asked for in a realm that never ran its install");
    return proto;   /* OWNED */
}

JSValue html_unknown_element_proto(JSContext *ctx)
{
    JSValue proto = JS_GetClassProto(ctx, g_unknown_class);
    DCHECK(!JS_IsNull(proto), "HTMLUnknownElement.prototype was asked for in a realm that never ran its install");
    return proto;
}

/* §3.2.2 `[SameObject] readonly attribute DOMStringMap dataset`. [SameObject] is an IDENTITY the IDL states, so
   the map is cached on the element's own wrapper: a page that stashes `el.dataset` and a later `el.dataset` must
   be holding the same object, and a fresh one per read makes every `===` against the stash false. The cache
   lives on the wrapper rather than in this component because the wrapper is per-flow state the COW already
   isolates — two arms that each read `dataset` get their own map over their own attributes. */
static JSValue js_html_dataset(JSContext *ctx, JSValueConst this_val, int magic)
{
    lxb_dom_node_t *n = node_of(this_val);
    JSValue cached;

    (void)magic;
    if (!n || n->type != LXB_DOM_NODE_TYPE_ELEMENT) return JS_UNDEFINED;
    if (JS_GetOwnSlot(ctx, &cached, this_val, g_dataset_key) > 0 && JS_IsObject(cached))
        return cached;
    JS_FreeValue(ctx, cached);
    cached = dom_string_map_new(ctx, lxb_dom_interface_element(n));
    JS_DefinePropertyValue(ctx, (JSValue)this_val, g_dataset_key, JS_DupValue(ctx, cached), 0);
    return cached;
}

/* §4.12.3 HTMLTemplateElement.content — the fragment the parser filled. Lexbor keeps it on the element's own
   interface rather than in its child list, which is exactly what the DOM says: only the parser and `content`
   itself reach those children. A pure read of the component's own tree. */
static JSValue js_template_content(JSContext *ctx, JSValueConst this_val, int magic)
{
    lxb_dom_node_t *n = node_of(this_val);
    lxb_html_template_element_t *t;

    (void)magic;
    /* WEB IDL §3.7.6 Attributes' BRAND CHECK, and it is a THROW rather than an assert. This was a DCHECK whose
       message
       said that reaching it meant the interface table handed out the wrong prototype — which is FALSE: a page
       reaches an accessor off the prototype with `.call` on anything at all, so the receiver is the PAGE's
       input and an abort there is this engine crashing on a line of ordinary JavaScript. */
    if (!n || n->type != LXB_DOM_NODE_TYPE_ELEMENT || !lxb_html_tree_node_is(n, LXB_TAG_TEMPLATE))
        return JS_ThrowTypeError(ctx, "HTMLTemplateElement.content read on something that is not a <template> "
                                      "element");
    t = lxb_html_interface_template(n);
    DCHECK(t->content != NULL, "a <template> element has no content fragment — lexbor builds one in the "
                               "template interface's constructor, so an element without it was made some "
                               "other way");
    return node_wrap(ctx, &t->content->node);
}

/* HTML §8.1.8.1 Event handlers' DEFINED TERMS — the SIX questions that section's algorithms ask of the tree,
 * answered here because they are HTML's and the DOM cannot answer the first of them: which ELEMENT TYPE a
 * local name is, is exactly what this file's table IS. The algorithms that compose them stay in
 * core/events/event_target.c, where their step numbers are visible. The first three below are DETERMINE THE
 * TARGET OF AN EVENT HANDLER's; the last three are GET THE CURRENT VALUE OF THE EVENT HANDLER's — step 3.1's
 * partition and the two objects step 3.9's scope layers over.
 *
 * Step 1's test is the ELEMENT TYPE and not `document.body`. §8.1.8.1's own note is explicit that "a body
 * element created in an active document (perhaps with document.createElement()) but not connected will also
 * have its corresponding Window object as the target", and that the check "does not necessarily prevent body
 * and frameset elements that are not the body element of their node document from reaching the next step" — so
 * a `document.body ===` comparison, or a parent test, answers no for cases HTML answers yes for. The NAMESPACE
 * is part of it: `<body>` in the SVG namespace is not a body element, which lxb_html_tree_node_is asks. */
static bool eh_is_body_or_frameset(JSContext *ctx, JSValueConst target)
{
    lxb_dom_node_t *n = node_of(target);

    (void)ctx;
    return n != NULL && n->type == LXB_DOM_NODE_TYPE_ELEMENT &&
           (lxb_html_tree_node_is(n, LXB_TAG_BODY) || lxb_html_tree_node_is(n, LXB_TAG_FRAMESET));
}

/* Step 3's — HTML §7.3.1 Navigables' ACTIVE DOCUMENT, asked of the element's NODE DOCUMENT. core/dom/document.c
   owns it: a realm can hold several Documents (`createHTMLDocument`, a DOMParser parse, XHR's `responseXML`)
   and only one of them is the realm's active document, which is what document_active_realm_of answers. A body
   element in any of the others has no Window to act upon and §8.1.8.1 says so by answering null. */
static bool eh_node_document_is_active(JSContext *ctx, JSValueConst target)
{
    lxb_dom_node_t *n = node_of(target);

    (void)ctx;
    DCHECK(n != NULL && n->owner_document != NULL,
           "§8.1.8.1's determine the target of an event handler reached step 3 for a node with no node "
           "document — step 1 has already established that this is a body or frameset ELEMENT, and every "
           "element lexbor builds carries the document that created it");
    return document_active_realm_of(lxb_dom_interface_node(n->owner_document)) != NULL;
}

/* Step 4's answer — "eventTarget's node document's relevant global object". BORROWED; the realm owns it. It is
   a fact about THAT DOCUMENT and not about whoever is running, which is why it is not read off `ctx`: two
   same-origin documents are one agent, so a body element reached across a frame boundary would otherwise
   delegate its handler to the RUNNING realm's Window instead of its own. */
static JSValueConst eh_node_document_global(JSContext *ctx, JSValueConst target)
{
    lxb_dom_node_t *n = node_of(target);

    (void)ctx;
    return document_window_of(n);
}

/* §8.1.8.1's GET THE CURRENT VALUE OF THE EVENT HANDLER step 3.1's FIRST ARM, asked positively — "If
   eventTarget is an element". The events layer has `is_window` and its negation is a DIFFERENT question: a
   Document is neither, and step 3.1 has no arm for one, so the compile asserts the partition with this rather
   than inferring an element from the absence of a Window. It is DOM's node type and not an HTML tag list —
   every element there is can carry an event handler content attribute, which is §8.1.8.1's own second table. */
static bool eh_is_element(JSContext *ctx, JSValueConst target)
{
    lxb_dom_node_t *n = node_of(target);

    (void)ctx;
    return n != NULL && n->type == LXB_DOM_NODE_TYPE_ELEMENT;
}

/* Step 3.1's `document` on that arm — "document be element's node document" — as the OBJECT step 3.9's scope
   substep 3 layers, which is why this answers a wrapper and eh_node_document_global answers a Window. OWNED.
   NOT the running realm's document: an origin-keyed agent cluster holds more than one Document per realm
   (createHTMLDocument, a DOMParser parse, XHR's responseXML), so a handler on a node from another of this
   agent's documents must resolve its unqualified names against ITS document and not against whoever is
   running — the same reason eh_node_document_global does not read `ctx`. */
static JSValue eh_node_document(JSContext *ctx, JSValueConst target)
{
    lxb_dom_node_t *n = node_of(target);

    (void)ctx;
    DCHECK(n != NULL && n->owner_document != NULL,
           "§8.1.8.1 step 3.1's `document` was asked of a node with no node document — the caller has already "
           "established this is an ELEMENT, and every element lexbor builds carries the document that "
           "created it");
    return node_wrap(ctx, lxb_dom_interface_node(n->owner_document));
}

/* STEP 3.5 — "If element is not null and element has a form owner, let form owner be that form owner.
   Otherwise, let form owner be null." OWNED, and JS_NULL is the standard's own second arm rather than an
   absence: step 3.9's scope substep 4 reads it as "this layer is not added".
   THE TAG IS ASKED FIRST, and that is §4.10.18.3's own shape rather than a fast path: html_form_owner_of
   answers for a FORM-ASSOCIATED element, and "has a form owner" is false for every element that could not be
   one — a `<div>`, a `<span>`, the `<img>` that carries most of the web's inline handlers. */
static JSValue eh_form_owner(JSContext *ctx, JSValueConst target)
{
    lxb_dom_node_t *n = node_of(target);

    if (n == NULL || !html_form_maybe_associated(n))
        return JS_NULL;
    return html_form_owner_of(ctx, target);
}

static const EventHandlerTerms EH_TERMS = {
    eh_is_body_or_frameset, eh_node_document_is_active, eh_node_document_global,
    eh_is_element, eh_node_document, eh_form_owner,
};

/* HTML §6.5 Activation behavior of elements' click() STEP 1: "If this element is a form control that is
 * disabled, then return." — the question core/events/event_target.c asks of this file, because click() is
 * installed on HTMLElement.prototype and every HTML element there is can therefore reach it.
 *
 * BOTH CONJUNCTS, AND THE FIRST IS THE LOAD-BEARING ONE. §4.10.19.5 Enabling and disabling form controls: the
 * disabled attribute defines the second — "A form control is disabled if any of the following are true" — and
 * its SECOND bullet names no element list at all: "the element is a descendant of a fieldset element whose
 * disabled attribute is specified, and the element is not a descendant of that fieldset element's first legend
 * element child". Read alone, that is TRUE of a `<div>` inside `<fieldset disabled>`, so a step 1 asking only
 * "is it disabled" would stop `div.click()` firing — and it is the definition's SUBJECT ("a form control")
 * that excludes it, which is exactly why §6.5 spells the subject out in its own sentence.
 *
 * THE SUBJECT IS §4.10.2 Categories' SUBMITTABLE SET, and that is not a third reading of the term: it is
 * character-for-character the list §4.10.19.5's FIRST bullet enumerates — "the element is a button, input,
 * select, textarea, or form-associated custom element" — so the two halves are asked of one set that
 * html_form.c already spells once. `fieldset`, `object` and `output` carry the `disabled` content attribute and
 * are NOT in it, which is the standard's own answer rather than a narrowing chosen here.
 *
 * ONE PREDICATE OVER TWO THAT ALREADY EXIST, NEVER A SECOND COPY OF EITHER: html_form_control_is_disabled is
 * read by §4.10.21.1's candidacy, §4.10.22.4's entry list, §4.10.5.1's mutability and §4.13's form-associated
 * custom elements, and a §6.5-shaped copy of it here would be the second answer to one question. */
static bool html_element_is_disabled_form_control(JSContext *ctx, JSValueConst el)
{
    return html_form_is_submittable(ctx, el) && html_form_control_is_disabled(ctx, el);
}

void html_element_init(JSContext *ctx)
{
    JSClassDef hd = { "HTMLElement" }, ud = { "HTMLUnknownElement" };
    int i, j;

    DCHECK(!g_html_ready, "html_element_init ran twice — the interfaces are declared once per AGENT");
    JS_NewClassID(JS_GetRuntime(ctx), &g_html_class);
    JS_NewClass(JS_GetRuntime(ctx), g_html_class, &hd);
    JS_NewClassID(JS_GetRuntime(ctx), &g_unknown_class);
    JS_NewClass(JS_GetRuntime(ctx), g_unknown_class, &ud);
    /* THE TABLE'S OWN INVARIANT, ASSERTED BEFORE ANYTHING IS BUILT FROM IT. §3.2.2's first four steps are
       ORDERED, and this file collapses them into ONE first-match walk — which is the standard's answer only
       while no local name is decided twice. The generator refuses to emit a duplicate; this is the same claim
       made where the walk that depends on it is, because a hand-edit of the generated header (which its own
       banner forbids and nothing enforces) would otherwise make one row silently unreachable. */
    for (i = 0; i < HTML_IFACE_N; i++)
        for (j = 0; j < i; j++)
            DCHECK(strcmp(HTML_IFACE[j].tag, HTML_IFACE[i].tag) != 0,
                   "one local name has two rows in the element-interface table, so §3.2.2's ordered steps 1-4 "
                   "collapse to whichever row comes first and the other is unreachable");
    /* ONE CLASS PER INTERFACE NAME. A row whose interface a previous row already claimed SHARES its class —
       `q` and `blockquote` are both HTMLQuoteElement, and two classes would make one of the two `instanceof`
       answers false in every realm. The two BASE interfaces are not built here: HTMLElement and
       HTMLUnknownElement have their classes above, and a row naming one of them (§3.2.2 step 2's twelve
       obsolete names, and the thirty-nine of §4's elements whose interface IS HTMLElement) takes that class
       rather than a second one wearing the same name. */
    for (i = 0; i < HTML_IFACE_N; i++) {
        int base = iface_is_base(HTML_IFACE[i].iface);
        if (base) { g_iface_class[i] = base == 1 ? g_html_class : g_unknown_class; continue; }
        for (j = 0; j < i; j++)
            if (strcmp(HTML_IFACE[j].iface, HTML_IFACE[i].iface) == 0) break;
        if (j < i) { g_iface_class[i] = g_iface_class[j]; continue; }
        {
            JSClassDef d = { HTML_IFACE[i].iface };
            JS_NewClassID(JS_GetRuntime(ctx), &g_iface_class[i]);
            JS_NewClass(JS_GetRuntime(ctx), g_iface_class[i], &d);
        }
    }
    /* THE JOIN — each row's reflection set, looked up by interface NAME in the hand-written IFACE_REFL. The two
       halves are separate because only one of them is generated, and this is where they meet: a set naming an
       interface no row wears would install members on a prototype no element can have, which engine/elemgen.mjs
       checks and which this DCHECK states at the site that would silently do it. */
    for (i = 0; i < IFACE_REFL_N; i++) {
        for (j = 0; j < HTML_IFACE_N; j++)
            if (strcmp(HTML_IFACE[j].iface, IFACE_REFL[i].iface) == 0) break;
        DCHECK(j < HTML_IFACE_N,
               "a reflection set is declared for an interface no element-interface row wears, so its members "
               "would be installed on a prototype no element can ever have");
        DCHECK(!iface_is_base(IFACE_REFL[i].iface),
               "a reflection set is declared for HTMLElement or HTMLUnknownElement under the per-interface "
               "join — HTMLElement's own reflections are R_HTML, declared once below, and HTMLUnknownElement "
               "adds none");
    }
    for (i = 0; i < HTML_IFACE_N; i++) {
        g_iface_refl[i] = NULL;
        g_iface_nrefl[i] = 0;
        for (j = 0; j < IFACE_REFL_N; j++)
            if (strcmp(IFACE_REFL[j].iface, HTML_IFACE[i].iface) == 0) {
                g_iface_refl[i] = IFACE_REFL[j].refl;
                g_iface_nrefl[i] = IFACE_REFL[j].nrefl;
                break;
            }
    }
    /* §2.5.6's `nonce` setter, declared beside the reflections it is deliberately not one of. */
    nonce_attribute_init(ctx);
    /* §4.10.19.6's two `[ReflectSetter]` setters, declared beside them for the same reason. */
    form_submission_attributes_init(ctx);
    /* §4.10.19.7's `autocomplete` setter — one declaration for the three interfaces that share it. */
    autofill_init(ctx);
    /* EVERY REFLECTION DECLARED ONCE, here, with the base index each row's install names them by. */
    g_html_refl_base = element_declare_reflections(ctx, "HTMLElement", R_HTML,
                                                  (int)(sizeof(R_HTML) / sizeof(R_HTML[0])));
    for (i = 0; i < HTML_IFACE_N; i++) {
        for (j = 0; j < i; j++)
            if (g_iface_class[j] == g_iface_class[i]) break;
        g_iface_refl_base[i] = (j < i) ? g_iface_refl_base[j]
                             : (g_iface_nrefl[i]
                                    ? element_declare_reflections(ctx, HTML_IFACE[i].iface,
                                                                  g_iface_refl[i], g_iface_nrefl[i])
                                    : -1);
    }
    /* THE MIXINS' AND SUB-INTERFACES' DECLARATIONS, once per agent — their installs run per realm below. */
    hyperlink_declare(ctx);
    iframe_declare(ctx);
    html_form_declare(ctx);
    /* §4.11.4's dialog state and CLOSE THE DIALOG — declared here because its `returnValue` member goes on
       HTMLDialogElement.prototype, which is what this file owns the table of, and because §4.10.22.3 step 11.6
       (a `method=dialog` submission) is its caller. It comes AFTER html_form_declare for no ordering reason of
       its own; §4.11 is simply the next section this file reaches. */
    html_dialog_declare(ctx);
    /* §6.12's three members and the state machine behind them, plus CSS Positioned Layout Level 4 §3's top
       layer that its show popover fills — declared here because `showPopover`, `hidePopover` and
       `togglePopover` are HTMLElement members, which is what this file owns the table of, and because the
       `popover` reflection row above points at that component's §2.3.3 attribute definition. */
    popover_declare(ctx);
    /* §4.8.11's media element state machine — declared here because HTMLMediaElement.prototype is the parent
       of two rows of the table above, so this file is what decides when it must exist. */
    media_element_declare(ctx);
    /* §4.8.3's image requests, §4.8.4.3.5's update the image data, and Web IDL §3.7.2's `Image` — declared
       here because HTMLImageElement is a row of the table above and `Image` is a global NAME this file owns
       the list of, exactly as §4.8.11's three interface objects are. */
    html_image_declare(ctx);
    /* §4.8.9's `Audio` — the second of the three names HTML declares with Web IDL §3.7.2's
       `[LegacyFactoryFunction]`, declared beside `Image` because HTMLAudioElement is a row of the table above
       and because a legacy factory function is a global NAME, and this file owns the list of those. */
    html_audio_declare(ctx);
    /* §4.10.10's SELECTEDNESS and DIRTINESS, the `selected` member they decide, and `Option` — the THIRD of
       Web IDL §3.7.2's names, declared beside `Image` and `Audio` for the same two reasons: HTMLOptionElement
       is a row of the table above, and a legacy factory function is a global NAME this file owns the list of. */
    html_option_declare(ctx);
    /* §4.2.4's link processing model — its per-element state key and the task that fires a preload's
       `load`/`error` — declared here because HTMLLinkElement is a row of the table above, exactly as §4.8.3's
       image requests are. */
    html_link_declare(ctx);
    /* §4.10.14's six gauge points and §4.10.13's three progress numbers — declared here because
       HTMLMeterElement and HTMLProgressElement are rows of the table above, and because every one of those
       members is a `[ReflectSetter]` whose SETTER is §2.6.1's and whose GETTER is the section's own algorithm,
       so each needs a setter id of its own rather than a reflection row. */
    html_meter_declare(ctx);
    html_progress_declare(ctx);
    /* §4.13.7 — declared here because `attachInternals` is an HTMLElement member, which is what this file
       owns the table of; the algorithms are element_internals.c's. */
    element_internals_declare(ctx);
    /* HTML §4.12.3's two enumerated `<template>` reflections and §13.2.6.4.4's parser step — declared here
       because the members are HTMLTemplateElement's, which is what this file owns the table of; the algorithm
       is declarative_shadow.c's. */
    declarative_shadow_init(ctx);
    /* HTML's ENUMERATED global attributes — §3.2.6.3's `translate`, §6.8.5's `spellcheck`, §6.8.6's
       `writingSuggestions`, §6.8.8's `autocorrect` and §6.8.1's two. Declared here because every one of them is
       an HTMLElement member, which is what this file owns the table of; the §2.3.3 state algorithm and the
       ancestor walks are core/html/global_attributes.c's. */
    global_attributes_declare(ctx);
    /* §3.2.2 dataset — on HTMLElement, which is where the IDL puts it. */
    dom_string_map_init(ctx);
    g_dataset_key = JS_NewAtom(ctx, "__datasetSlot");
    CHECK(g_dataset_key != JS_ATOM_NULL, "the dataset slot key could not be interned");
    g_html_ready = 1;
    /* HTML §8.1.8.1's determine the target of an event handler, whose step 1 asks which element type this is —
       this file's table is what knows. Claimed per AGENT beside the element resolver below, and given back at
       html_element_free, which core/platform.c's reverse-declaration order runs before event_target_free. */
    event_target_set_handler_terms(&EH_TERMS);
    /* HTML §6.5's click() step 1, whose subject is a form control — this file's table is what knows which
       elements those are, and click() is installed from here. Claimed per AGENT beside the terms above and
       given back at html_element_free for the same reason. */
    event_target_set_click_terms(html_element_is_disabled_form_control);
    realm_declare_intrinsic(html_element_install_protos);
    /* node.c keys its prototype table by node TYPE; an element's interface is keyed by its LOCAL NAME, which is
       HTML's mapping and not the DOM's. So the base ASKS, and stays the one place a wrapper is built. */
    node_set_element_resolver(html_proto_for);
}

/* §3.2.2 AND §4's INTERFACE PROTOTYPE OBJECTS, FOR ONE REALM — HTMLElement, HTMLUnknownElement, and one per
   interface the tag table names. */
void html_element_install_protos(JSContext *ctx)
{
    JSValue html_p, unknown_p, base, prev;
    int i, j;

    DCHECK(g_html_ready, "a realm asked for HTMLElement.prototype before the interfaces were declared");
    prev = JS_GetClassProto(ctx, g_html_class);
    DCHECK(JS_IsNull(prev), "html_element_install_protos ran twice in one realm");
    JS_FreeValue(ctx, prev);

    /* §3.2.2 `interface HTMLElement : Element`. */
    base = element_proto(ctx);
    html_p = JS_NewObjectProto(ctx, base);
    JS_FreeValue(ctx, base);
    CHECK(!JS_IsException(html_p), "HTMLElement.prototype could not be allocated");
    idl_interface_tag(ctx, html_p, "HTMLElement");
    element_install_reflections(ctx, html_p, g_html_refl_base, (int)(sizeof(R_HTML) / sizeof(R_HTML[0])));
    /* HTML mixes GlobalEventHandlers and DocumentAndElementEventHandlers into HTMLElement, not into Element —
       so this is where they belong, and where `div.onclick = f` now reaches them. */
    event_target_install_handlers(ctx, html_p, EH_GLOBAL);
    idl_members_excluded(ctx, html_p, "HTMLElement", TOUCH_EXCLUDED,
                         (int)(sizeof(TOUCH_EXCLUDED) / sizeof(TOUCH_EXCLUDED[0])),
                         "Touch Events Level 2, `Extensions to the GlobalEventHandlers mixin`: \"For user "
                         "agents where expose legacy touch event APIs is false, this mixin must not be "
                         "implemented.\" This agent's `expose legacy touch event APIs` is false — TouchEvent, "
                         "Touch and TouchList are absent, so a touch handler would have nothing to be handed");
    event_target_install_click(ctx, html_p);
    /* §3.2.2 `[SameObject] attribute CSSStyleDeclaration style` — the attribute is HTMLElement's, the object is
       the CSSOM's, so each side owns its half. */
    cssom_install_style_attribute(ctx, html_p);
    idl_install_accessor(ctx, html_p, "dataset", js_html_dataset, 0, -1);
    /* The six §3.2.6/§6.8 global attributes whose IDL attribute COMPUTES a value from the tree rather than
       mirroring one attribute — installed onto THIS realm's prototype like every other member. */
    global_attributes_install(ctx, html_p);
    /* §4.13.7 "Element internals"'s `ElementInternals attachInternals()` — an HTMLElement member, installed
       on THIS realm's prototype like every other. */
    element_internals_install_html_members(ctx, html_p);
    /* §2.5.6 Nonce attributes' `nonce` — the third `HTMLOrSVGOrMathMLElement` member on this prototype, beside
       the two below, and here rather than in R_HTML because its getter reads an internal slot and its setter
       deliberately does not write the content attribute. */
    nonce_attribute_install(ctx, html_p);
    /* §6.6.6's `HTMLOrSVGOrMathMLElement` members — `focus(options)` and `blur()`, the two entry points of
       §6.6.4's processing model, and §6.6.3's `tabIndex`. They were one body returning undefined; they are now
       the real algorithms, which move the document's focused area and fire the page's focus handlers
       (core/html/focus.c). */
    focus_install_html_members(ctx, html_p);
    /* CSSOM VIEW §7's `partial interface HTMLElement` — the offset family, installed on THIS realm's prototype
       like every other member. It is per realm because its answers are: every used value it reports bottoms out
       in CSS 2.1 §10.1's initial containing block, which is 300 CSS pixels wide in a child navigable and 1280
       in the top-level traversable, and a C member runs in the realm that DEFINED it. */
    html_element_view_install(ctx, html_p);
    /* HTML §6.12 The popover attribute's `showPopover(options)`, `hidePopover()` and `togglePopover(options)`
       — HTMLElement members, installed on THIS realm's prototype like every other. */
    popover_install(ctx, html_p);
    JS_SetClassProto(ctx, g_html_class, JS_DupValue(ctx, html_p));

    /* §4.8.11's `interface HTMLMediaElement : HTMLElement`, built BEFORE the per-tag loop because the two
       interfaces below inherit from it rather than from HTMLElement — `audio.play` is a property of
       HTMLMediaElement.prototype, and a table that parented both to html_p would have made it two. */
    media_element_install_proto(ctx, html_p);

    unknown_p = JS_NewObjectProto(ctx, html_p);   /* §4: `interface HTMLUnknownElement : HTMLElement` */
    CHECK(!JS_IsException(unknown_p), "HTMLUnknownElement.prototype could not be allocated");
    idl_interface_tag(ctx, unknown_p, "HTMLUnknownElement");
    JS_SetClassProto(ctx, g_unknown_class, unknown_p);

    for (i = 0; i < HTML_IFACE_N; i++) {
        JSValue p;
        /* THE TWO BASE INTERFACES ARE ALREADY BUILT — html_p and unknown_p above are their prototypes, so a row
           naming one of them must not reach the body below: it would allocate a SECOND object, tag it with the
           same interface name and JS_SetClassProto it over the one every other prototype in this realm already
           inherits from. */
        if (iface_is_base(HTML_IFACE[i].iface)) continue;
        /* A row sharing a previous row's CLASS shares its prototype — it is the same slot. */
        for (j = 0; j < i; j++)
            if (g_iface_class[j] == g_iface_class[i]) break;
        if (j < i) continue;
        /* §4.8.9 and §4.8.10: `HTMLVideoElement : HTMLMediaElement` and `HTMLAudioElement : HTMLMediaElement`.
           The parent is decided by the INTERFACE, which is what the IDL states, rather than by the tag. */
        {
            bool media = !strcmp(HTML_IFACE[i].iface, "HTMLAudioElement") ||
                         !strcmp(HTML_IFACE[i].iface, "HTMLVideoElement");
            JSValue parent = media ? media_element_proto(ctx) : JS_DupValue(ctx, html_p);

            p = JS_NewObjectProto(ctx, parent);
            JS_FreeValue(ctx, parent);
        }
        CHECK(!JS_IsException(p), "a per-tag interface prototype could not be allocated");
        idl_interface_tag(ctx, p, HTML_IFACE[i].iface);
        if (g_iface_nrefl[i])
            element_install_reflections(ctx, p, g_iface_refl_base[i], g_iface_nrefl[i]);
        /* §4.6.3's HTMLHyperlinkElementUtils, which the IDL says HTMLAnchorElement and HTMLAreaElement
           INCLUDE. Named by interface rather than by tag because that is how the IDL states it, and because
           two tags share one of them. */
        if (!strcmp(HTML_IFACE[i].iface, "HTMLAnchorElement") ||
            !strcmp(HTML_IFACE[i].iface, "HTMLAreaElement"))
            hyperlink_install(ctx, p);
        /* §4.8.5's `contentWindow` — the child navigable its insertion steps queued. */
        if (!strcmp(HTML_IFACE[i].iface, "HTMLIFrameElement"))
            iframe_install(ctx, p);
        /* §4.2.6's `disabled` and CSSOM §6.3.2's LinkStyle `sheet`, which HTMLStyleElement includes. Handed the
           prototype for the same reason §4.10's members are: this file owns the table of which interface a tag
           wears, that one owns the algorithm that decides whether there is a sheet to answer with. */
        if (!strcmp(HTML_IFACE[i].iface, "HTMLStyleElement"))
            html_style_element_install(ctx, p);
        /* §4.12.1's `async`, which reads and writes the element's `force async` boolean rather than mirroring
           an attribute — handed the prototype for the reason §4.2.6's `disabled` is. */
        if (!strcmp(HTML_IFACE[i].iface, "HTMLScriptElement"))
            html_script_install(ctx, p);
        /* §4.8.3's `complete`, `currentSrc` and the two natural dimensions, which are computed from the
           element's IMAGE REQUESTS rather than mirrored from an attribute — handed the prototype for the
           reason §4.12.1's `async` is. */
        if (!strcmp(HTML_IFACE[i].iface, "HTMLImageElement"))
            html_image_install(ctx, p);
        /* §4.2.3's `href`, which parses against the document's FALLBACK base URL rather than mirroring an
           attribute or resolving against the document base URL — handed the prototype for the reason
           §4.12.1's `async` is. */
        if (!strcmp(HTML_IFACE[i].iface, "HTMLBaseElement"))
            html_base_element_install(ctx, p);
        /* §4.10.19.6 Form submission attributes' two URL-valued members, each on the interfaces whose IDL
           declares it — `action` on HTMLFormElement, `formAction` on HTMLInputElement and HTMLButtonElement.
           Handed the prototype for the reason §4.2.3's `href` above is: this file owns the table of which
           interface a tag wears, that one owns the algorithm behind the member. */
        if (!strcmp(HTML_IFACE[i].iface, "HTMLFormElement"))
            form_submission_attributes_install(ctx, p, "action");
        if (!strcmp(HTML_IFACE[i].iface, "HTMLInputElement") ||
            !strcmp(HTML_IFACE[i].iface, "HTMLButtonElement"))
            form_submission_attributes_install(ctx, p, "formAction");
        /* §4.10.19.7.2's `autocomplete`, on the three interfaces whose controls have an autofill field.
           Not a reflection row: the member answers a value the section derives from the attribute rather
           than the attribute. `form` is NOT one of them — §4.10.3's same-named attribute is an enumerated
           on/off and stays a row above. */
        if (!strcmp(HTML_IFACE[i].iface, "HTMLInputElement") ||
            !strcmp(HTML_IFACE[i].iface, "HTMLSelectElement") ||
            !strcmp(HTML_IFACE[i].iface, "HTMLTextAreaElement"))
            autofill_install(ctx, p);
        /* §4.10.14's six numbers and §4.10.13's three, each an algorithm over the element's attributes rather
           than a mirror of one — handed the prototype for the reason §4.12.1's `async` is. */
        if (!strcmp(HTML_IFACE[i].iface, "HTMLMeterElement"))
            html_meter_install(ctx, p);
        if (!strcmp(HTML_IFACE[i].iface, "HTMLProgressElement"))
            html_progress_install(ctx, p);
        /* THE `[SameObject] readonly attribute DOMTokenList` REFLECTIONS, each on the interface whose IDL
           declares it. They are not reflections in R_* above because those produce a STRING: `link.rel` is the
           attribute's value and `link.relList` is §7.1's token list over the same attribute, and a page uses
           the second one to ask `contains('stylesheet')`. Named per interface here rather than by attribute in
           the token-list component, because which interface declares a member is what this table IS. */
        if (!strcmp(HTML_IFACE[i].iface, "HTMLAnchorElement") ||
            !strcmp(HTML_IFACE[i].iface, "HTMLAreaElement") ||
            !strcmp(HTML_IFACE[i].iface, "HTMLLinkElement") ||
            !strcmp(HTML_IFACE[i].iface, "HTMLFormElement"))
            dom_token_list_install_reflection(ctx, p, "relList");
        if (!strcmp(HTML_IFACE[i].iface, "HTMLLinkElement"))
            dom_token_list_install_reflection(ctx, p, "sizes");
        if (!strcmp(HTML_IFACE[i].iface, "HTMLIFrameElement"))
            dom_token_list_install_reflection(ctx, p, "sandbox");
        /* §4.10.12 The output element's `htmlFor`, which is a TOKEN LIST and was a string row in R_OUTPUT. It
           is installed here and not there for the reason the four above are, and the reason it was missed for
           as long as it was is that HTMLLabelElement declares a member of the same name over the same content
           attribute whose type really is `DOMString` — so the pair reads as one member until the IDL is. */
        if (!strcmp(HTML_IFACE[i].iface, "HTMLOutputElement"))
            dom_token_list_install_reflection(ctx, p, "htmlFor");
        /* HTML §4.3.1 The body element's `HTMLBodyElement includes WindowEventHandlers` and §16.3.2 Frames'
           `HTMLFrameSetElement includes WindowEventHandlers` — the eighteen of §8.1.8.2's third table, which
           "must be supported by Window objects … and with corresponding event handler content attributes and
           event handler IDL attributes exposed on all body and frameset elements that are owned by that Window
           object's associated Document". Every one of them ACTS UPON the Window: §8.1.8.1's determine the
           target of an event handler is what routes `document.body.onunload = f` there, and this install is
           what gives that determination an accessor to run in — without it the assignment was an ordinary JS
           property on the element and nothing ever read it.
           The other six a body delegates — §8.1.8.2's Window-reflecting body element event handler set — are
           NOT installed here and must not be: §8.1.8.2.1's IDL declares all six in GlobalEventHandlers, so
           HTMLElement.prototype already carries them, and §4.3.1's "replace the generic event handlers with the
           same names" is a statement about the TARGET they act upon rather than about where the accessor
           lives. A second own copy would be a member the IDL does not declare on this interface. */
        if (!strcmp(HTML_IFACE[i].iface, "HTMLBodyElement") ||
            !strcmp(HTML_IFACE[i].iface, "HTMLFrameSetElement"))
            event_target_install_handlers(ctx, p, EH_WINDOW);
        JS_SetClassProto(ctx, g_iface_class[i], p);
    }
    JS_FreeValue(ctx, html_p);

    /* §4.12.3 `[SameObject] readonly attribute DocumentFragment content`. It goes on HTMLTemplateElement and
       nowhere else, which is why it is here rather than on HTMLElement: a template's children are NOT under it,
       and `content` is the only way a page reaches the ones the parser put in the fragment. */
    {
        /* The atom is BORROWED by JS_DefinePropertyGetSet, so this owns it — an interned name nobody releases
           survives the runtime and the gc_obj_list walk counts it. */
        JSAtom a = JS_NewAtom(ctx, "content");
        JSValue tpl = html_iface_proto(ctx, "HTMLTemplateElement");
        char nb[IDL_ACCESSOR_NAME_MAX];
        CHECK(a != JS_ATOM_NULL, "the `content` attribute name could not be interned");
        /* Web IDL §3.7.6 Attributes names the GETTER "get content" — the atom above is the property key and is
           a different string. Written out here, it was the key twice; it comes from the one composer now. */
        JS_DefinePropertyGetSet(ctx, tpl, a,
                                JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)js_template_content,
                                                     idl_accessor_name(nb, sizeof nb, "content",
                                                                       IDL_ACCESSOR_GET), 0,
                                                     JS_CFUNC_getter_magic, 0),
                                JS_UNDEFINED, JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
        JS_FreeAtom(ctx, a);
        /* §4.12.3's `shadowRootMode` and `shadowRootSlotAssignment` go on the same prototype and nowhere else,
           handed it for the same reason the slot and forms members are: this file owns the table, that one
           owns the enumerated-attribute states the parser reads. */
        declarative_shadow_install_template_members(ctx, tpl);
        JS_FreeValue(ctx, tpl);
    }

    /* HTML §4.12.4's three members go on HTMLSlotElement and nowhere else, handed the prototype for the same
       reason the forms component is: this file owns the table, that one owns §4.2.2's algorithms. */
    {
        JSValue sp = html_iface_proto(ctx, "HTMLSlotElement");
        slot_install_slot_members(ctx, sp);
        JS_FreeValue(ctx, sp);
    }

    /* §4.10's members go on the interfaces that DECLARE them, which is why the forms component is handed the
       prototypes rather than reaching for them: this file owns the table, that one owns the algorithms. */
    {
        JSValue f = html_iface_proto(ctx, "HTMLFormElement"), in = html_iface_proto(ctx, "HTMLInputElement");
        JSValue ta = html_iface_proto(ctx, "HTMLTextAreaElement"), op = html_iface_proto(ctx, "HTMLOptionElement");
        JSValue bt = html_iface_proto(ctx, "HTMLButtonElement");
        html_form_install(ctx, f, in, ta, op, bt);
        /* §4.10.10's `selected`, on the same prototype and handed it for the same reason — this file owns the
           table, core/html/html_option.c owns the SELECTEDNESS the member reads and writes. It is not part of
           html_form_install because it is not a fact about a form: an `option` in a `datalist` has one too. */
        html_option_install_members(ctx, op);
        JS_FreeValue(ctx, f); JS_FreeValue(ctx, in); JS_FreeValue(ctx, ta); JS_FreeValue(ctx, op);
        JS_FreeValue(ctx, bt);
    }

    /* §4.11.4's `returnValue` goes on HTMLDialogElement and nowhere else, handed the prototype for the same
       reason §4.10's members are: this file owns the table, that one owns the algorithm that writes it. */
    {
        JSValue dp = html_iface_proto(ctx, "HTMLDialogElement");
        html_dialog_install(ctx, dp);
        JS_FreeValue(ctx, dp);
    }
}

void html_element_install(JSContext *ctx, JSValueConst global)
{
    int i, j;

    JSValue hp, up;

    DCHECK(g_html_ready, "the HTML interface objects were installed before their interfaces were declared");
    hp = JS_GetClassProto(ctx, g_html_class);
    up = JS_GetClassProto(ctx, g_unknown_class);
    DCHECK(!JS_IsNull(hp), "the HTML interface objects were installed in a realm with no HTMLElement.prototype");
    /* §3.2.3 "HTML element constructors": HTMLElement's IDL carries `[HTMLConstructor]`, a real sixteen-step
       algorithm and not the shared "Illegal constructor" throw — a page's
       `class X extends HTMLElement { constructor(){super()} }` reaches it through super(), and every custom
       element in the platform is built by it. The machine belongs
       to the custom-elements component because it reads that component's definition set and construction
       stack; what belongs here is WHICH interface carries it. */
    node_install_interface_ctor(ctx, global, "HTMLElement", hp, custom_elements_html_constructor(ctx));
    dom_string_map_install(ctx, global);   /* §3.2.2 DOMStringMap, which `dataset` is one of */
    /* §4.10.22.1 FormDataEvent — a §4.10 interface object, so it goes up where §4's interface objects do. Its
       algorithm is form_entry_list.c's; this file owns which names the global carries. */
    form_data_event_install(ctx, global);
    node_install_interface(ctx, global, "HTMLUnknownElement", up);
    /* §4.8.11's three interface objects — HTMLMediaElement, MediaError and TimeRanges. They go up where §4's
       do because this file owns which names the global carries; the algorithms are media_element.c's. */
    media_element_install(ctx, global);
    JS_FreeValue(ctx, hp);
    JS_FreeValue(ctx, up);
    /* ONE INTERFACE OBJECT PER INTERFACE, DEDUPED ON THE NAME COLUMN — the SAME key, in the same spelling, as
       the class-assignment loop above. It used to compare `g_iface_class[]` instead, which is the same
       partition (that loop assigns a shared class exactly when the names are equal) reached by a DIFFERENT
       question, and one nothing stated: a reader of this loop alone could not tell whether the filter drops a
       NAME, and two rows sharing a class under different names would silently install one of them. Asking the
       name here makes the rule readable where it is applied instead of fifteen lines away in another
       vocabulary; the DCHECK below is what keeps the two answers from drifting apart. */
    for (i = 0; i < HTML_IFACE_N; i++) {
        JSValue p;
        /* THE SECOND FILTER, AND IT REMOVES NO NAME EITHER: `HTMLElement` went up at the [HTMLConstructor]
           install above and `HTMLUnknownElement` two lines under it, both by their own names, so a row naming
           one of them has nothing left to do — and letting it through would install `HTMLElement` a second
           time, OVER the constructor §3.2.3 requires it to carry. */
        if (iface_is_base(HTML_IFACE[i].iface)) continue;
        for (j = 0; j < i; j++)
            if (strcmp(HTML_IFACE[j].iface, HTML_IFACE[i].iface) == 0) break;
        DCHECK(j >= i || g_iface_class[j] == g_iface_class[i],
               "two rows of the element-interface table name one interface and were given different classes — "
               "`instanceof` would then answer false in every realm for one of the two tags, and this loop "
               "would install the interface object twice");
        if (j < i) continue;
        p = JS_GetClassProto(ctx, g_iface_class[i]);
        /* HTML §3.2.3 "HTML element constructors": "To support the custom elements feature, ALL HTML ELEMENTS
           have special constructor behavior. This is indicated via the [HTMLConstructor] IDL extended
           attribute." Every interface this table names carries it, so every one of them is minted with the
           SAME sixteen-step machine — the shared "Illegal constructor" throw was right for `new
           HTMLButtonElement()` (which is step 1's TypeError) and wrong for the one call that is not that: the
           implicit `super()` inside a CUSTOMIZED BUILT-IN's constructor, whose active function object is
           HTMLButtonElement and whose NewTarget is the page's class.
           THE MACHINE IS NOT PARAMETERISED BY THE INTERFACE and there is no per-interface arm: the one step
           whose answer differs, 8.1's "the list of local names … that use the active function object as their
           element interface", is asked of HTML §3.2.2 through this file's own name-to-interface table. So this
           is one mechanism reached from sixty-nine interface objects rather than sixty-nine algorithms, which
           is what §3.2.3's own "have the following OVERRIDDEN constructor steps" says it is. */
        node_install_interface_ctor(ctx, global, HTML_IFACE[i].iface, p,
                                    custom_elements_element_constructor(ctx, HTML_IFACE[i].iface));
        /* Web IDL §3.7.2's LEGACY FACTORY FUNCTIONS, which are global names of their own beside the interface
           objects this loop installs and whose non-configurable `prototype` is the interface prototype object
           it is holding. They go here rather than beside the interface object's own install because this is
           where that object is in hand and because this file owns which names the global carries.
           A LEGACY FACTORY FUNCTION IS NOT AN INTERFACE OBJECT AND MUST NOT BE MISTAKEN FOR ONE: it has no
           `constructor` back-pointer, so `Image.prototype.constructor` is `HTMLImageElement` — which is why
           each is minted by its own component and hung on the global here, rather than by the shared
           node_install_interface_ctor above.
           HTML DECLARES EXACTLY THREE — `Image` (§4.8.3), `Audio` (§4.8.9) and `Option` (§4.10.10) — and that
           is a fact about the IDL rather than about this list, so it is checkable: the extended attribute is
           `[LegacyFactoryFunction=...]` and a fourth would appear there first. All three are here. */
        if (!strcmp(HTML_IFACE[i].iface, "HTMLImageElement"))
            html_image_install_global(ctx, global, p);
        if (!strcmp(HTML_IFACE[i].iface, "HTMLAudioElement"))
            html_audio_install_global(ctx, global, p);
        if (!strcmp(HTML_IFACE[i].iface, "HTMLOptionElement"))
            html_option_install_global(ctx, global, p);
        JS_FreeValue(ctx, p);
    }
    /* WHAT THE LOOP ABOVE LEAVES ON THE GLOBAL, said as a claim about the global rather than about the loop.
       The Web IDL gap audit reads `node_install_interface(ctx, global, HTML_IFACE[i].iface, p)` and finds a
       `continue` between it and the top of the loop; it cannot tell a dedup from a filter that drops names, so
       it refuses to credit the column at all — and refusing is right, because crediting a name this loop never
       installed would report sixty-odd interface objects present with nothing to contradict it. The filter is a
       dedup OF THIS VERY COLUMN, so it removes rows and no NAMES, and that is the fact stated here and asserted
       per realm against the object itself. Break the dedup — key it on anything else, drop a row for any other
       reason — and this fires naming the interface object the audit would have credited. */
    idl_install_covers_column(ctx, global, IDL_NAME_COLUMN(HTML_IFACE, iface),
                              "the loop's two row filters remove ROWS and no NAME: the first is a "
                              "first-occurrence dedup on this same name column, and the second skips the rows "
                              "naming HTMLElement and HTMLUnknownElement, which this same function installs "
                              "above under those very names because one of them must carry §3.2.3's "
                              "[HTMLConstructor]");
}

bool html_element_is(JSValueConst v)
{
    lxb_dom_node_t *n = node_of(v);

    /* §4's element-interface table maps every HTML-namespace element to an interface INHERITING HTMLElement —
       including HTMLUnknownElement, which is what an unrecognised HTML tag is. So the namespace is the whole
       test, and an element in the SVG or MathML namespace (or any other) is not one. */
    return n != NULL && n->type == LXB_DOM_NODE_TYPE_ELEMENT && n->ns == LXB_NS_HTML;
}

void html_element_free(JSRuntime *rt)
{
    /* THE FOUR SLOTS THIS FILE CLAIMED IN OTHER COMPONENTS, GIVEN BACK FIRST. §2.9's activation behaviour is
       core/events/event_target.c's slot pointing at core/html/hyperlink.c, the element resolver is
       core/dom/node.c's slot pointing at this file, and §8.1.8.1's determine-the-target terms and HTML §6.5's
       click() step 1 predicate are two more event_target.c slots pointing at this file — each is a callback
       INTO a component the cascade around this line is tearing down, which is the defect core/agent_state.h
       found in idb_transaction. All four receivers assert at their own release that the claim is gone. */
    hyperlink_free();
    node_set_element_resolver(NULL);
    event_target_set_handler_terms(NULL);
    event_target_set_click_terms(NULL);
    dom_string_map_free(rt);
    nonce_attribute_free();   /* §2.5.6's setter id, reset like core/html/html_base_element.c's */
    form_submission_attributes_free();   /* §4.10.19.6's two, likewise */
    autofill_free();   /* §4.10.19.7's one, likewise */
    global_attributes_free();
    declarative_shadow_free();
    html_form_free(rt);
    html_dialog_free(rt);
    popover_free(rt);   /* §6.12's slot keys and member ids, and the top layer's two slot keys with them */
    media_element_free(rt);
    html_image_free(rt);
    html_audio_free(rt);
    html_option_free(rt);
    html_link_free(rt);
    element_internals_free(rt);
    if (g_dataset_key != JS_ATOM_NULL) { JS_FreeAtomRT(rt, g_dataset_key); g_dataset_key = JS_ATOM_NULL; }
    /* the prototypes are the REALMS' — each is released with its context; the AGENT holds only class ids */
    g_html_ready = 0;
}
