/* HTMLHyperlinkElementUtils — HTML §4.6.3, the URL members of `<a>` and `<area>`.
 *
 * WHAT IT IS. `a.protocol`, `a.host`, `a.pathname`, `a.search`, `a.hash` and the rest are the SAME eleven
 * algorithms the URL interface exposes, over a URL that lives in the element's `href` CONTENT ATTRIBUTE rather
 * than in a URL object. So they are not implemented here: url_member_get and url_member_set are, and this
 * component supplies the record and decides where it comes from and where it goes back to. Two copies of "what
 * is this URL's host" would be two answers, and the second one is always the one that is subtly wrong.
 *
 * REINITIALISE, EVERY TIME. §4.6.3's "reinitialize url" re-parses the href attribute against the element's node
 * document base URL on every access, and that is load-bearing rather than lazy: the attribute is ordinary
 * markup and anything can have written it — `setAttribute('href', …)`, the parser, another flow's DOM delta —
 * so a URL cached on the element would answer for a document nobody is in. Re-parsing also makes these members
 * correct under time travel for free: the attribute read goes through the DOM chokepoint, so each flow reads
 * its own href and gets its own answer with nothing cached to invalidate.
 *
 * A FAILED PARSE IS NOT AN ERROR. §4.6.3 says the url is then null, and every member answers "" — `a.href`
 * alone answers the raw attribute value, because a page that wrote garbage into href must read that garbage
 * back. None of these throw; a browser does not, and a page that sets `a.port = "notaport"` simply keeps the
 * port it had.
 *
 * WHY IT IS NOT A REFLECTION. `href` was declared as a plain string reflection, which is what an attribute
 * MIRROR is — and href is not one: reading it RESOLVES against the document base and re-serialises, so
 * `<a href="/x">` reads back as `http://host/x`, not as `/x`. That is the whole of what url/toascii and
 * url/url-setters-a-area measure, and a mirror answers every one of them wrong. */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "solver/concolic.h"   /* href is a slot a SOURCE can be stashed in — the members keep its provenance */
#include "core/html/hyperlink.h"
#include "core/html/html_base_element.h"   /* §4.2.3's get an element's target — see §4.6.5 step 3 below */
#include "core/dom/element.h"
#include "core/dom/document.h"
#include "core/url/url.h"
#include "core/idl_args.h"
#include "core/dom/node.h"
#include "core/events/event_target.h"
#include "core/frame/navigable.h"
#include "core/frame/window_proxy.h"   /* the brand on what §7.3.1.7's rules answer with — see §4.6.5 step 9 */
#include "core/frame/window_features.h"
#include "quickjs-step.h"

/* §4.6.3 "reinitialize url": "set element's url to the result of encoding-parsing a URL given element's href
   content attribute's value, RELATIVE TO ELEMENT'S NODE DOCUMENT" — which is §2.4.3's DOCUMENT BASE URL of the
   document the element is IN. Both halves of that changed here: it asked the RUNNING REALM's active document
   (the wrong document for an `<a>` in a DOMParser tree, or in a second Document of this agent), and that
   function answered with the document's ADDRESS rather than its base URL (wrong for every page carrying a
   `<base href>`, which is exactly the markup that exists to move where `a.href` points).
   Returns false when there is no href or it does not parse — the url is then null, which every member has an
   answer for.
   `raw_href` receives the attribute's VALUE (owned; JS_NULL when absent), which is a CONCOLIC when a flow
   stashed an attacker string there — the parse then runs on its concrete EXAMPLE and js_link_get DERIVES the
   member from it, keeping the provenance. Reading it as BYTES here is what dropped the triple. */
static bool hyperlink_url(JSContext *ctx, JSValueConst el, UrlRecord *out, JSValue *raw_href)
{
    JSValue href = element_attr_get_value(ctx, el, "href");
    lxb_dom_node_t *node = node_of(el);
    JSValue concrete;
    const char *base_url;
    const char *s;
    size_t len = 0;
    UrlRecord base;
    bool have_base, ok = false;

    url_record_init(out);
    DCHECK(node != NULL && node->owner_document != NULL,
           "§4.6.3's reinitialize url ran for something that is not a node, or a node with no node document — "
           "the members are installed on HTMLAnchorElement/HTMLAreaElement prototypes, so a receiver that is "
           "neither reached them through a path that did not brand it");
    base_url = document_base_url_of(node->owner_document);
    if (JS_IsNull(href) || JS_IsException(href)) goto done;

    /* A concolic with no example yet has no bytes to parse, and §4.6.3 has no step that invents them: the url
       is null, so the href getter's step 4 answers with the attribute value itself — still carrying its
       provenance, which is the whole point of not stringifying it. */
    concrete = concolic_is(href) ? concolic_example(ctx, href) : JS_DupValue(ctx, href);
    if (!JS_IsString(concrete)) { JS_FreeValue(ctx, concrete); goto done; }
    s = JS_ToCStringLen(ctx, &len, concrete);
    JS_FreeValue(ctx, concrete);
    if (!s) goto done;

    url_record_init(&base);
    have_base = base_url && *base_url && url_parse(&base, base_url, strlen(base_url), NULL);
    ok = url_parse(out, s, len, have_base ? &base : NULL);
    url_record_free(&base);
    JS_FreeCString(ctx, s);
    if (!ok) url_record_free(out);

done:
    if (raw_href) *raw_href = href; else JS_FreeValue(ctx, href);
    return ok;
}

/* The mixin's members, in IDL order. `origin` is readonly; the rest are accessors whose setter re-serialises
   the URL back into the href attribute. IT IS INDEXED BY THE MEMBER ID, which the row's own `member` field
   asserts at every use — the getter below needs the member's NAME to tell one derivation from another, and a
   table that merely happened to be in enum order would answer with a neighbour's name the day a member is
   inserted. */
static const struct { const char *name; int member; bool readonly; } HL_M[] = {
    { "href",     URL_HREF,     false },
    { "origin",   URL_ORIGIN,   true  },
    { "protocol", URL_PROTOCOL, false },
    { "username", URL_USERNAME, false },
    { "password", URL_PASSWORD, false },
    { "host",     URL_HOST,     false },
    { "hostname", URL_HOSTNAME, false },
    { "port",     URL_PORT,     false },
    { "pathname", URL_PATHNAME, false },
    { "search",   URL_SEARCH,   false },
    { "hash",     URL_HASH,     false },
};
#define HL_N ((int)(sizeof HL_M / sizeof HL_M[0]))

static JSValue js_link_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    UrlRecord u;
    JSValue raw = JS_NULL, r;

    DCHECK(magic >= 0 && magic < HL_N && HL_M[magic].member == magic,
           "§4.6.3's member table is not indexed by the member id it names — the getter reads the member's "
           "name out of it to compose a derived value's identity, and a shifted row gives two members one");

    if (!hyperlink_url(ctx, this_val, &u, &raw)) {
        /* §4.6.3's href getter steps 3 and 4: with a null url, `href` is the ATTRIBUTE's own value — a page
           that wrote something unparseable reads back what it wrote, and an attacker string reads back STILL
           TAINTED — and every other member is the empty string. */
        if (magic == URL_HREF && !JS_IsNull(raw)) return raw;
        JS_FreeValue(ctx, raw);
        return JS_NewStringLen(ctx, "", 0);
    }
    {
        char *s = url_member_get(&u, magic);   /* step 5, "url, serialized", and each member's own getter */
        r = JS_NewString(ctx, s ? s : "");
        free(s);
    }
    url_record_free(&u);
    /* A MEMBER OF A TAINTED URL IS TAINTED. The href attribute held unknown external input, so everything
       computed off it is attacker-controlled: `a.href = location.hash.slice(1)` then `a.search` is the
       fragment's own bytes, and answering with a bare string is how the sink downstream reads as clean. The
       REAL parse already ran on the concrete example above — the way `+` really concatenates — so what is
       needed here is a DERIVATION carrying it, which is §Solver-half's triple kept whole across the DOM round
       trip.
       IT IS THE DERIVATION SEAM AND NOT concolic_source_wrap, for two reasons that are the same reason.
       source_wrap MINTS AT A SOURCE: it asserts that a declared source's shape is its provenance in braces, and
       provenance is INHERITED through a derivation — so `location.hash.slice(1)` arrives carrying the declared
       source `location.hash` with a derived shape, and wrapping it would abort on that assert. And it would
       give every member of one href the SAME identity, so a flow that branched on `a.search` would have
       DECIDED `a.pathname`, pruning arms nothing contradicts — the defect concolic.h names for the ordering
       hook. Deriving per member keeps the eleven apart, which is what a constraint key has to do. */
    if (concolic_is(raw))
        r = concolic_builtin_hook(ctx, raw, HL_M[magic].name, r);   /* consumes `r` as the example */
    JS_FreeValue(ctx, raw);
    return r;
}

static JSValue js_link_set(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    const char *v;
    size_t vlen = 0;
    UrlRecord u;

    /* §4.6.3: setting `href` sets the CONTENT ATTRIBUTE and nothing else — it is not parsed here, because the
       attribute is what the element holds and every getter re-parses it anyway.
       IT IS SET AS A VALUE AND NEVER AS BYTES, and this is FIRST because it is upstream of the getter above: a
       read cannot recover provenance a write never recorded. `a.href = location.hash.slice(1)` assigns unknown
       external input, and asking it for a C string is the ToString this engine has no concolic semantics for —
       it aborts in JS_ToStringInternal, and in a release build throws a TypeError that this component then
       dropped, so the write never reached §@S's (element, name) shadow at all. §4.9's attribute write already
       knows what a concolic is: it records the taint and stores the shape. Hand it the value. */
    if (magic == URL_HREF) {
        element_attr_set_value(ctx, this_val, "href", val);
        return JS_UNDEFINED;
    }

    /* §4.6.3's OTHER setters modify a URL RECORD, which is bytes — so unknown external input assigned to
       `a.protocol`/`a.host`/`a.search`/`a.hash` has no modelled answer yet and must not be quietly stringified
       into one. What to build: url_member_set over a concolic component, re-serialising into an href that keeps
       the provenance the way js_link_get's getter above now does. */
    DCHECK(!concolic_is(val),
           "a URL member of a hyperlink was assigned unknown external input — §4.6.3's setter writes it into a "
           "URL record as bytes, which takes the source identity and the domain off the triple. Build "
           "url_member_set over a concolic component and re-serialise under the same provenance");
    v = JS_ToCStringLen(ctx, &vlen, val);
    if (!v) return JS_EXCEPTION;

    /* Every other member: reinitialise, apply §4.4's setter, and write the whole URL back. A null url means
       there is nothing to modify, which §4.6.3 states as "return" — not as an error. */
    if (!hyperlink_url(ctx, this_val, &u, NULL)) {
        JS_FreeCString(ctx, v);
        return JS_UNDEFINED;
    }
    /* url_member_set only fails for href, which this branch never passes, so its result carries no case the
       spec has an answer for here. */
    DCHECK(magic != URL_HREF, "the href setter must not reach the URL-record path");
    if (magic == URL_ORIGIN) {
        /* §4.4: `origin` is readonly. Reaching a setter for it means one was declared, which is a mistake in
           the declaration below rather than anything a page can cause. */
        DFAIL("a setter was declared for the readonly `origin` member");
    }
    url_member_set(&u, magic, v, vlen);
    JS_FreeCString(ctx, v);
    {
        char *s = url_serialize(&u, false);
        element_attr_set(ctx, this_val, "href", s ? s : "");
        free(s);
    }
    url_record_free(&u);
    return JS_UNDEFINED;
}

/* §4.6.3's stringifier is `href`. */
static JSValue js_link_tostring(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    (void)argc; (void)argv; (void)magic;
    return js_link_get(ctx, this_val, URL_HREF);
}

/* §4.6.5 "Following hyperlinks" — FOLLOW THE HYPERLINK CREATED BY AN ELEMENT, and DOM §2.9's activation
 * behaviour is how it is reached.
 *
 * THE NUMBER WAS §4.6.3 IN EVERY LINE OF THIS HALF OF THE FILE AND §4.6.3 IS A DIFFERENT SECTION. It is "API
 * for hyperlink elements" — `href`, `protocol`, `host`, `hash` — which the TOP half of this file cites
 * correctly, so the file was right about its own members and wrong about its own algorithm, in the failure mode
 * CLAUDE.md rates worse than no citation at all: a number that reads as authoritative and sends the reader to
 * a clause that does not mention the thing. Two sections are meant here and they are not the same one:
 * §4.6.5 "Following hyperlinks" owns follow-the-hyperlink and get-an-element's-noopener; §4.6.2 "Links created
 * by a and area elements" owns the ACTIVATION BEHAVIOUR that reaches them. Only this half is corrected — the
 * §4.6.3 citations above are about the URL members and are right.
 *
 * WHAT WAS MISSING. This component was the URL members and nothing else, so `<a href>` was a rich set of
 * accessors over a link that could not be followed: clicking one dispatched a real §2.9 click, ran every
 * listener, and then dropped the event. `e.preventDefault()` on such a click suppressed nothing, because there
 * was nothing after the walk to suppress.
 *
 * IT IS THE ELEMENT'S OWN BEHAVIOUR, not a listener, which is why it is registered here rather than installed
 * as one: a page cannot remove it, cannot see it in a listener list, and cannot run it out of order. §2.9 picks
 * the activation target while it builds the path and runs it after the walk only if nothing cancelled.
 *
 * THE NAVIGATION IS §7.2.2.1's, reached through navigable_open — the same §7.3.1.7 rules for choosing a
 * `window.open()` applies. `rel="noopener"` and `rel="noreferrer"` are where this caller's noopener comes
 * from instead of a features string, which is the only difference between the two callers. */
static bool link_has_activation(JSContext *ctx, JSValueConst el)
{
    lxb_dom_node_t *n = node_of(el);
    size_t qn = 0;
    const lxb_char_t *q;
    JSValue href;
    bool has;

    if (!n || n->type != LXB_DOM_NODE_TYPE_ELEMENT) return false;
    /* §4.6.2 "Links created by a and area elements" gives `a` and `area` an activation behaviour, and its
       STEP 1 is this test: "If element has no href attribute, then return." `<a>` with none is not a hyperlink
       at all — it is a placeholder the spec says nothing happens for. */
    q = lxb_dom_element_qualified_name((lxb_dom_element_t *)n, &qn);
    if (!q || !((qn == 1 && q[0] == 'a') || (qn == 4 && !memcmp(q, "area", 4)))) return false;
    /* PRESENCE, so it asks for the VALUE and not for bytes — a link whose href a flow tainted is still a link,
       and stringifying it merely to learn that it exists is the de-taint this component is being cured of. */
    href = element_attr_get_value(ctx, el, "href");
    has = !JS_IsNull(href) && !JS_IsException(href);
    JS_FreeValue(ctx, href);
    return has;
}

static int link_run_activation(JSContext *ctx, JSValueConst el, JSValueConst ev, uint8_t *phase, uint32_t *req)
{
    JSValue hrefv = element_attr_get_value(ctx, el, "href");
    char *rel = element_attr_get(ctx, el, "rel");
    const char *href, *tv;
    size_t tlen = 0;
    char *target;
    WindowFeatures feat = { false, false, false };
    JSValue r;
    /* §4.6.5 step 6 takes only the FIRST return value of §7.3.1.7's rules, so this exists to be READ ONCE —
       by the navigate below, and only for the reason stated there. */
    WindowType window_type;

    (void)ev;
    /* FOLLOWING A TAINTED LINK IS A SINK, AND IT IS NOT BUILT. `a.href = location.hash.slice(1)` then a click
       navigates to a destination the attacker names — a `javascript:` URL is the @S vector — and navigable_open
       takes BYTES, so the destination would arrive at §7.4 as a plain string with its provenance gone and the
       finding would be silently absent rather than parked. Asserted HERE and not left to the generic assert in
       element_attr_get, because the mechanism to build is this site's and not that accessor's: navigable_open
       over a concolic destination, forking the feasible arm and carrying the source into the navigation. */
    DCHECK(!concolic_is(hrefv),
           "a hyperlink whose href holds unknown external input was followed — §4.6.5's navigation takes the "
           "destination as bytes, so the attacker-controlled URL reaches §7.4 untainted and the sink is never "
           "recognised. Build navigable_open over a concolic destination");
    DCHECK(JS_IsString(hrefv), "a hyperlink with no href was picked as an activation target — "
                               "link_has_activation is what decides that, and it reads the same attribute");
    /* Borrowed from `hrefv`, which outlives the navigation below — one read of the attribute, not two. */
    href = JS_ToCString(ctx, hrefv);
    /* §4.6.5's "get an element's noopener". A relation LIST is space-separated and ASCII case-insensitive;
       `noreferrer` implies `noopener` for the same reason §7.4's feature does — a window with no referrer has
       no opener, because the opener is how it would have one. */
    if (rel) {
        feat.noopener = strstr(rel, "noopener") != NULL || strstr(rel, "noreferrer") != NULL;
        feat.noreferrer = strstr(rel, "noreferrer") != NULL;
    }
    /* §4.6.5 "Following hyperlinks" steps 2-3: "Let targetAttributeValue be the empty string. If subject is an
       `a` or `area` element, then set targetAttributeValue to the result of GETTING AN ELEMENT'S TARGET given
       subject." Both words of that are the algorithm's and neither was here: this read the `target` attribute
       raw, so it never looked for the document's `<base target>` (§4.2.3 step 1.2), and it never applied
       §4.2.3 step 2's reset — a `target` carrying both an ASCII tab/newline and a `<` is the tail of an
       unterminated attribute in an HTML injection, and this engine named a window after it where a browser
       refuses to. That is a smuggling primitive on the security half, and it is fixed by CALLING the algorithm
       rather than by adding its steps here: `window.open` must NOT reset (§7.2.2.1 does not run this), so the
       two paths differ by which algorithm supplies the name and by nothing else.
       It is still handed over AS IT IS — this line used to substitute the literal "_self" for an absent or
       empty target, which is HTML §7.3.1.7 "Navigable target names" step 4 written a second time in a caller.
       The rules for choosing a navigable answer the current navigable for the empty string themselves.
       COPIED BECAUSE THE POINTER IS BORROWED from a Lexbor attribute buffer that the navigation below outlives,
       and NUL-terminated because navigable_open takes a C string — which is where a name holding an embedded
       U+0000 loses its tail today (`targeting-with-embedded-null-in-target.html` reads `abc` for `abc\0def`).
       That is the navigable NAME's representation and not this call's: it is a DOMString the whole way down and
       the fix is to hold it as a JSValue, which the same length is already carried past here for. */
    tv = html_base_element_get_target(node_of(el), NULL, 0, &tlen);
    target = malloc(tlen + 1);
    CHECK(target != NULL, "§4.6.5 step 3: OOM copying a hyperlink's target");
    memcpy(target, tv, tlen);
    target[tlen] = '\0';
    /* §7.4 STEPS 6 AND 14. `phase` and `req` are this behaviour's suspension, held by the dispatch machine that
       called it: the navigate inside here FETCHES, and when that fetch becomes the host request it already has
       to be (navigable.h), it is these two words the wait lives in and this line that returns JS_STEP_YIELD.
       It does not park yet — navigable_open reaches the host's synchronous fetcher — so this always finishes in
       one entry, and the contract is what makes the change to child_document a change to child_document. */
    /* §4.6.5 step 11 navigates "with … sourceElement set to subject" — the element is what makes this an
       ELEMENT-initiated navigation, which is the one thing §7.3.1.7's rules cannot ask of the name alone and
       the fact the assert at their entry is about. */
    r = navigable_open(ctx, href, target, &feat, node_of(el), &window_type);
    /* §4.6.5 STEP 9: "Navigate targetNavigable to urlString using subject's node document, with referrerPolicy
       set to subject's hyperlink referrer policy, userInvolvement set to userInvolvement, and sourceElement set
       to subject." IT IS THIS CALLER'S STEP AND IT IS UNCONDITIONAL — there is no urlRecord-is-null test here,
       because §4.6.5 step 4 RETURNS when the parse fails rather than carrying a null forward, and no windowType
       branch, because step 6 takes only the FIRST return value of the rules. That is precisely where this
       algorithm and §7.2.2.1's part company: an empty `href` resolves against the document's own address and
       so a click on `<a href="">` reloads, while `window.open("")` on an existing navigable does nothing at
       all. The one navigate that used to live inside navigable_open was THIS one, imposed on both callers.
       ONLY THE EXISTING-NAVIGABLE ARM IS NAVIGATED HERE, and that is an artefact of this engine rather than of
       §4.6.5: a navigable the rules CREATED has already been navigated to `href` by §7.4 step 14 inside the
       create (navigable.h states that contract), so navigating again would load one address into two documents
       of one navigable. When step 8's create stops taking a url — which is what §7.3.1.7 actually says — this
       test goes and the step becomes unconditional, as it reads. */
    if (window_type == WINDOW_TYPE_EXISTING_OR_NONE && window_proxy_is(r)) {
        JSValue nav = navigable_navigate(ctx, r, href);
        JS_FreeValue(ctx, r);
        r = nav;
    }
    JS_FreeCString(ctx, href);
    JS_FreeValue(ctx, hrefv);
    free(target);
    free(rel);
    (void)phase; (void)req;
    /* A URL THAT DOES NOT PARSE IS NOT AN ERROR HERE. §4.6.5 step 4 says to return if the url is failure, and a
       click is not a place a page can catch anything — unlike `window.open()`, whose caller gets the
       SyntaxError. */
    JS_FreeValue(ctx, r);
    return JS_STEP_DONE;
}

/* DECLARED ONCE PER AGENT, INSTALLED PER REALM — the IDL pool is sealed after agent init, so a helper that
   mints inline works for the first realm and aborts on the second. */
static int g_hl_set[HL_N];
static int g_hl_tostring = -1;

void hyperlink_declare(JSContext *ctx)
{
    int i;
    for (i = 0; i < HL_N; i++)
        g_hl_set[i] = HL_M[i].readonly ? -1
                                       : idl_setter_id(ctx, IDL_USVSTRING, false, js_link_set, HL_M[i].member);
    g_hl_tostring = idl_method_id(ctx, NULL, 0, js_link_tostring, 0);
    /* §2.9 asks ONE pair for every element, so it is registered with the declarations rather than from a
       per-prototype install that would set the same two pointers once per interface per realm. */
    event_target_set_activation(link_has_activation, link_run_activation);
}

void hyperlink_free(void)
{
    int i;

    DCHECK(g_hl_tostring >= 0, "§4.6.3 was released in an agent that never declared it");
    /* §2.9's ACTIVATION BEHAVIOUR IS GIVEN BACK BY WHOEVER CLAIMED IT, which is this file. The two pointers
       live in core/events/event_target.c and point at code in THIS one, so a release that kept them would
       leave the events layer holding a callback into a component the DOM group's cascade has already torn
       down — the shape core/agent_state.h found in idb_transaction. event_target_free asserts it. */
    event_target_set_activation(NULL, NULL);
    for (i = 0; i < HL_N; i++) g_hl_set[i] = -1;
    g_hl_tostring = -1;
}

void hyperlink_install(JSContext *ctx, JSValueConst proto)
{
    int i;

    DCHECK(g_hl_tostring >= 0, "HTMLHyperlinkElementUtils was installed before it was declared");
    for (i = 0; i < HL_N; i++)
        idl_install_accessor(ctx, proto, HL_M[i].name, js_link_get, HL_M[i].member, g_hl_set[i]);
    idl_install_method(ctx, proto, "toString", 0, g_hl_tostring);
}
