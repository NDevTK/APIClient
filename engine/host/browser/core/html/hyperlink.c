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
#include "core/html/hyperlink.h"
#include "core/dom/element.h"
#include "core/dom/document.h"
#include "core/url/url.h"
#include "core/idl_args.h"
#include "core/dom/node.h"
#include "core/events/event_target.h"
#include "core/frame/navigable.h"
#include "core/frame/window_features.h"

/* §4.6.3 "reinitialize url": parse the href content attribute against the document's base URL. Returns false
   when there is no href or it does not parse — the url is then null, which every member has an answer for. */
static bool hyperlink_url(JSContext *ctx, JSValueConst el, UrlRecord *out, char **raw_href)
{
    char *href = element_attr_get(ctx, el, "href");
    const char *base_url = document_base_url(ctx);
    UrlRecord base;
    bool have_base, ok = false;

    if (raw_href) *raw_href = href;
    url_record_init(out);
    if (!href) return false;

    url_record_init(&base);
    have_base = base_url && *base_url && url_parse(&base, base_url, strlen(base_url), NULL);
    ok = url_parse(out, href, strlen(href), have_base ? &base : NULL);
    url_record_free(&base);
    if (!ok) url_record_free(out);
    if (!raw_href) free(href);
    return ok;
}

static JSValue js_link_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    UrlRecord u;
    char *raw = NULL;
    JSValue r;

    if (!hyperlink_url(ctx, this_val, &u, &raw)) {
        /* §4.6.3: with a null url, `href` is the ATTRIBUTE's own value — a page that wrote something
           unparseable reads back what it wrote — and every other member is the empty string. */
        r = JS_NewString(ctx, magic == URL_HREF && raw ? raw : "");
        free(raw);
        return r;
    }
    free(raw);
    {
        char *s = url_member_get(&u, magic);
        r = JS_NewString(ctx, s ? s : "");
        free(s);
    }
    url_record_free(&u);
    return r;
}

static JSValue js_link_set(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    const char *v;
    size_t vlen = 0;
    UrlRecord u;

    v = JS_ToCStringLen(ctx, &vlen, val);
    if (!v) return JS_EXCEPTION;

    /* §4.6.3: setting `href` sets the CONTENT ATTRIBUTE and nothing else — it is not parsed here, because the
       attribute is what the element holds and every getter re-parses it anyway. */
    if (magic == URL_HREF) {
        element_attr_set(ctx, this_val, "href", v);
        JS_FreeCString(ctx, v);
        return JS_UNDEFINED;
    }

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

/* §4.6.3 FOLLOWING A HYPERLINK, and DOM §2.9's activation behaviour is how it is reached.
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
 * THE NAVIGATION IS §7.4's, reached through navigable_open — the same rules for choosing a navigable that
 * `window.open()` applies. `rel="noopener"` and `rel="noreferrer"` are where this caller's noopener comes
 * from instead of a features string, which is the only difference between the two callers. */
static bool link_has_activation(JSContext *ctx, JSValueConst el)
{
    lxb_dom_node_t *n = node_of(el);
    size_t qn = 0;
    const lxb_char_t *q;
    char *href;

    if (!n || n->type != LXB_DOM_NODE_TYPE_ELEMENT) return false;
    /* §4.6.3 gives `a` and `area` an activation behaviour, and only WITH an href: `<a>` with none is not a
       hyperlink at all — it is a placeholder the spec says nothing happens for. */
    q = lxb_dom_element_qualified_name((lxb_dom_element_t *)n, &qn);
    if (!q || !((qn == 1 && q[0] == 'a') || (qn == 4 && !memcmp(q, "area", 4)))) return false;
    href = element_attr_get(ctx, el, "href");
    if (!href) return false;
    free(href);
    return true;
}

static void link_run_activation(JSContext *ctx, JSValueConst el, JSValueConst ev)
{
    char *href = element_attr_get(ctx, el, "href");
    char *target = element_attr_get(ctx, el, "target");
    char *rel = element_attr_get(ctx, el, "rel");
    WindowFeatures feat = { false, false, false };
    JSValue r;

    (void)ev;
    DCHECK(href != NULL, "a hyperlink with no href was picked as an activation target — link_has_activation "
                         "is what decides that, and it reads the same attribute");
    /* §4.6.3's "get an element's noopener". A relation LIST is space-separated and ASCII case-insensitive;
       `noreferrer` implies `noopener` for the same reason §7.4's feature does — a window with no referrer has
       no opener, because the opener is how it would have one. */
    if (rel) {
        feat.noopener = strstr(rel, "noopener") != NULL || strstr(rel, "noreferrer") != NULL;
        feat.noreferrer = strstr(rel, "noreferrer") != NULL;
    }
    /* §4.6.3 step 2: the target attribute value, and an EMPTY one is no target at all — which the rules for
       choosing a navigable read as `_self`, the navigable the link is in. */
    r = navigable_open(ctx, href, target && *target ? target : "_self", &feat);
    /* A URL THAT DOES NOT PARSE IS NOT AN ERROR HERE. §4.6.3 says to return if the url is failure, and a click
       is not a place a page can catch anything — unlike `window.open()`, whose caller gets the SyntaxError. */
    JS_FreeValue(ctx, r);
    free(href);
    free(target);
    free(rel);
}

void hyperlink_install(JSContext *ctx, JSValueConst proto)
{
    /* The mixin's members, in IDL order. `origin` is readonly; the rest are accessors whose setter re-serialises
       the URL back into the href attribute. */
    static const struct { const char *name; int member; bool readonly; } M[] = {
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
    size_t i;

    for (i = 0; i < sizeof M / sizeof M[0]; i++)
        idl_install_accessor(ctx, proto, M[i].name, js_link_get, M[i].member,
                             M[i].readonly ? -1
                                           : idl_setter_id(ctx, IDL_USVSTRING, false, js_link_set, M[i].member));
    idl_install_method(ctx, proto, "toString", 0, idl_method_id(ctx, NULL, 0, js_link_tostring, 0));
    /* §2.9 asks ONE pair for every element, so registering it from a per-prototype install sets the same two
       pointers each time — which is why event_target_set_activation asserts they arrive together rather than
       counting how often they arrive. */
    event_target_set_activation(link_has_activation, link_run_activation);
}
