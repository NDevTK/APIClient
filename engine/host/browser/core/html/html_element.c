/* HTMLElement AND THE PER-TAG INTERFACES — HTML §3.2.2 and the element-interface table §4 states element by
 * element ("the a element uses the HTMLAnchorElement interface").
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
 * WHAT IS DATA AND WHAT IS CODE. The table below is data: a tag, its interface name, and the reflections that
 * interface adds. Every behaviour — how a reflection reads, how a boolean one unsets, how the prototype chain is
 * built — is written once in element.c and here. An interface with no reflections is still an entry, because
 * `instanceof` is observable even when the member list is not.
 *
 * WHAT IS HONESTLY ABSENT. CSSOM VIEW §7's `partial interface HTMLElement` — offsetParent, offsetTop,
 * offsetLeft, offsetWidth, offsetHeight — is not here; the IDL audit names its members. `getBoundingClientRect`
 * has LEFT that list: it is §6's, it is on Element.prototype (core/dom/element_view.c), and every interface in
 * the table below inherits it. §4.8.11's media elements no longer belong on that list either — HTMLMediaElement
 * is a real state machine over a modelled device (core/html/media_element.c), and this file's table names it as
 * the PARENT of the two interfaces whose IDL inherits from it. A tag whose interface this table does not list
 * gets HTMLUnknownElement, which is what HTML says for an unknown element — not a shrug. */
#include <string.h>

#include <lexbor/html/html.h>   /* <template>'s content fragment — §4.12.3 */

#include "check.h"
#include "quickjs.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "core/dom/node.h"
#include "core/dom/element.h"
#include "core/dom/dom_token_list.h"
#include "core/html/hyperlink.h"
#include "core/html/html_iframe.h"
#include "core/html/html_style_element.h"
#include "core/html/html_script.h"
#include "core/events/event_target.h"
#include "core/html/custom_elements.h"
#include "core/dom/slot.h"
#include "core/html/element_internals.h"
#include "core/html/focus.h"
#include "core/html/html_dialog.h"
#include "core/html/media_element.h"
#include "core/html/html_element.h"
#include "core/css/css_style_declaration.h"
#include "core/html/form_data_event.h"
#include "core/html/html_form.h"
#include "core/html/dom_string_map.h"
#include "core/html/global_attributes.h"
#include "core/html/declarative_shadow.h"

static JSClassID g_html_class;      /* HTMLElement.prototype's per-realm slot */
static JSClassID g_unknown_class;   /* HTMLUnknownElement — HTML's answer for a tag it does not know */
static int g_html_ready;
static JSAtom  g_dataset_key = JS_ATOM_NULL;   /* the [SameObject] dataset cache slot on an element's wrapper */

/* HTMLElement's OWN reflections — §3.2.6's global attributes. */
static const ElReflect R_HTML[] = {
    { "title", "title", REFLECT_STRING }, { "lang", "lang", REFLECT_STRING },
    { "dir", "dir", REFLECT_STRING }, { "accessKey", "accesskey", REFLECT_STRING },
    { "autocapitalize", "autocapitalize", REFLECT_STRING },
    { "enterKeyHint", "enterkeyhint", REFLECT_STRING }, { "inputMode", "inputmode", REFLECT_STRING },
    { "nonce", "nonce", REFLECT_STRING }, { "popover", "popover", REFLECT_STRING },
    { "hidden", "hidden", REFLECT_BOOL }, { "inert", "inert", REFLECT_BOOL },
    { "autofocus", "autofocus", REFLECT_BOOL },
};

/* THE PER-INTERFACE REFLECTIONS. Each list is what that interface's IDL declares and nothing else — which is
   the whole point of splitting them off Element. */
/* NO `href` HERE. §4.6.3's mixin owns it, because reading it RESOLVES against the document base and
   re-serialises — `<a href="/x">` reads back as `http://host/x` — which a plain attribute mirror cannot do. */
static const ElReflect R_ANCHOR[] = {
    { "target", "target", REFLECT_STRING },
    { "rel", "rel", REFLECT_STRING }, { "download", "download", REFLECT_STRING },
    { "hreflang", "hreflang", REFLECT_STRING }, { "type", "type", REFLECT_STRING },
    { "referrerPolicy", "referrerpolicy", REFLECT_STRING },
};
static const ElReflect R_AREA[] = {   /* `href` is §4.6.3's, as on <a> */
    { "target", "target", REFLECT_STRING },
    { "rel", "rel", REFLECT_STRING }, { "alt", "alt", REFLECT_STRING },
};
static const ElReflect R_LINK[] = {
    { "href", "href", REFLECT_STRING }, { "rel", "rel", REFLECT_STRING },
    { "type", "type", REFLECT_STRING }, { "media", "media", REFLECT_STRING },
    { "as", "as", REFLECT_STRING }, { "crossOrigin", "crossorigin", REFLECT_STRING },
    { "integrity", "integrity", REFLECT_STRING }, { "referrerPolicy", "referrerpolicy", REFLECT_STRING },
    { "disabled", "disabled", REFLECT_BOOL },
};
/* NO `async` HERE. §4.12.1 defines its getter and setter steps over the element's `force async` boolean and not
   over the attribute — the getter answers true for an element that has no `async` attribute at all, and the
   setter's step 1 CLEARS that flag whatever value it is given — so a boolean reflection answers a different
   question in both directions. core/html/html_script.c owns it, beside the flag. */
static const ElReflect R_SCRIPT[] = {
    { "src", "src", REFLECT_STRING }, { "type", "type", REFLECT_STRING },
    { "integrity", "integrity", REFLECT_STRING }, { "crossOrigin", "crossorigin", REFLECT_STRING },
    { "referrerPolicy", "referrerpolicy", REFLECT_STRING },
    { "defer", "defer", REFLECT_BOOL },
    { "noModule", "nomodule", REFLECT_BOOL },
};
static const ElReflect R_IMG[] = {
    { "src", "src", REFLECT_STRING }, { "srcset", "srcset", REFLECT_STRING },
    { "sizes", "sizes", REFLECT_STRING }, { "alt", "alt", REFLECT_STRING },
    { "useMap", "usemap", REFLECT_STRING }, { "crossOrigin", "crossorigin", REFLECT_STRING },
    { "referrerPolicy", "referrerpolicy", REFLECT_STRING }, { "loading", "loading", REFLECT_STRING },
    { "decoding", "decoding", REFLECT_STRING }, { "isMap", "ismap", REFLECT_BOOL },
};
static const ElReflect R_IFRAME[] = {
    { "src", "src", REFLECT_STRING }, { "srcdoc", "srcdoc", REFLECT_STRING },
    { "name", "name", REFLECT_STRING }, { "allow", "allow", REFLECT_STRING },
    { "referrerPolicy", "referrerpolicy", REFLECT_STRING }, { "loading", "loading", REFLECT_STRING },
    { "allowFullscreen", "allowfullscreen", REFLECT_BOOL },
};
static const ElReflect R_FORM[] = {
    { "action", "action", REFLECT_STRING }, { "method", "method", REFLECT_STRING },
    { "name", "name", REFLECT_STRING }, { "target", "target", REFLECT_STRING },
    { "enctype", "enctype", REFLECT_STRING }, { "acceptCharset", "accept-charset", REFLECT_STRING },
    { "autocomplete", "autocomplete", REFLECT_STRING }, { "rel", "rel", REFLECT_STRING },
    { "noValidate", "novalidate", REFLECT_BOOL },
};
static const ElReflect R_INPUT[] = {
    { "name", "name", REFLECT_STRING }, { "type", "type", REFLECT_STRING },
    { "defaultValue", "value", REFLECT_STRING }, { "placeholder", "placeholder", REFLECT_STRING },
    { "pattern", "pattern", REFLECT_STRING }, { "accept", "accept", REFLECT_STRING },
    { "autocomplete", "autocomplete", REFLECT_STRING }, { "min", "min", REFLECT_STRING },
    { "max", "max", REFLECT_STRING }, { "step", "step", REFLECT_STRING },
    { "formAction", "formaction", REFLECT_STRING }, { "formMethod", "formmethod", REFLECT_STRING },
    { "formEnctype", "formenctype", REFLECT_STRING }, { "formTarget", "formtarget", REFLECT_STRING },
    { "src", "src", REFLECT_STRING }, { "alt", "alt", REFLECT_STRING },
    { "disabled", "disabled", REFLECT_BOOL }, { "required", "required", REFLECT_BOOL },
    { "readOnly", "readonly", REFLECT_BOOL }, { "multiple", "multiple", REFLECT_BOOL },
    { "defaultChecked", "checked", REFLECT_BOOL }, { "formNoValidate", "formnovalidate", REFLECT_BOOL },
};
static const ElReflect R_BUTTON[] = {
    { "name", "name", REFLECT_STRING }, { "type", "type", REFLECT_STRING },
    { "value", "value", REFLECT_STRING }, { "formAction", "formaction", REFLECT_STRING },
    { "formMethod", "formmethod", REFLECT_STRING }, { "disabled", "disabled", REFLECT_BOOL },
};
static const ElReflect R_TEXTAREA[] = {
    { "name", "name", REFLECT_STRING }, { "placeholder", "placeholder", REFLECT_STRING },
    { "wrap", "wrap", REFLECT_STRING }, { "autocomplete", "autocomplete", REFLECT_STRING },
    { "disabled", "disabled", REFLECT_BOOL }, { "required", "required", REFLECT_BOOL },
    { "readOnly", "readonly", REFLECT_BOOL },
};
static const ElReflect R_SELECT[] = {
    { "name", "name", REFLECT_STRING }, { "autocomplete", "autocomplete", REFLECT_STRING },
    { "disabled", "disabled", REFLECT_BOOL }, { "multiple", "multiple", REFLECT_BOOL },
    { "required", "required", REFLECT_BOOL },
};
static const ElReflect R_OPTION[] = {
    { "label", "label", REFLECT_STRING },
    { "disabled", "disabled", REFLECT_BOOL }, { "defaultSelected", "selected", REFLECT_BOOL },
};
static const ElReflect R_LABEL[]  = { { "htmlFor", "for", REFLECT_STRING } };
static const ElReflect R_META[]   = {
    { "name", "name", REFLECT_STRING }, { "content", "content", REFLECT_STRING },
    { "httpEquiv", "http-equiv", REFLECT_STRING }, { "media", "media", REFLECT_STRING },
};
static const ElReflect R_BASE[]   = { { "href", "href", REFLECT_STRING }, { "target", "target", REFLECT_STRING } };
static const ElReflect R_SOURCE[] = {
    { "src", "src", REFLECT_STRING }, { "type", "type", REFLECT_STRING },
    { "srcset", "srcset", REFLECT_STRING }, { "sizes", "sizes", REFLECT_STRING },
    { "media", "media", REFLECT_STRING },
};
static const ElReflect R_TRACK[]  = {
    { "src", "src", REFLECT_STRING }, { "srclang", "srclang", REFLECT_STRING },
    { "label", "label", REFLECT_STRING }, { "kind", "kind", REFLECT_STRING },
    { "default", "default", REFLECT_BOOL },
};
/* §4.8.9's HTMLVideoElement declares TWO reflections of its own; the six `<audio>` and `<video>` share are
   HTMLMediaElement's, and they live on THAT prototype (core/html/media_element.c) rather than being repeated
   here per tag — which is what the IDL says and what makes `video.src` one member rather than two. */
static const ElReflect R_VIDEO[]  = {
    { "poster", "poster", REFLECT_STRING }, { "playsInline", "playsinline", REFLECT_BOOL },
};
static const ElReflect R_OBJECT[] = {
    { "data", "data", REFLECT_STRING }, { "type", "type", REFLECT_STRING },
    { "name", "name", REFLECT_STRING },
};
static const ElReflect R_EMBED[]  = { { "src", "src", REFLECT_STRING }, { "type", "type", REFLECT_STRING } };
/* §4.2.6's `media` is the element's ONE plain reflection. `disabled` used to sit beside it as a
   REFLECT_BOOL and it is not a reflection at all — HTML defines no `disabled` content attribute on a style
   element, and the IDL attribute's getter and setter steps read and write the ASSOCIATED CSS STYLE SHEET's
   disabled flag. The reflection honoured `<style disabled>` (markup no browser acts on) and ignored the flag
   every page actually sets, in both directions and silently; core/html/html_style_element.c owns it now.
   `blocking` is a `[SameObject] DOMTokenList` and is absent rather than mis-spelled as a string. */
static const ElReflect R_STYLE[]  = { { "media", "media", REFLECT_STRING } };
static const ElReflect R_OUTPUT[] = { { "name", "name", REFLECT_STRING }, { "htmlFor", "for", REFLECT_STRING } };
static const ElReflect R_FIELDSET[] = { { "name", "name", REFLECT_STRING }, { "disabled", "disabled", REFLECT_BOOL } };
static const ElReflect R_OPTGROUP[] = { { "label", "label", REFLECT_STRING }, { "disabled", "disabled", REFLECT_BOOL } };
static const ElReflect R_MAP[]    = { { "name", "name", REFLECT_STRING } };
static const ElReflect R_TIME[]   = { { "dateTime", "datetime", REFLECT_STRING } };
static const ElReflect R_QUOTE[]  = { { "cite", "cite", REFLECT_STRING } };
static const ElReflect R_MOD[]    = { { "cite", "cite", REFLECT_STRING }, { "dateTime", "datetime", REFLECT_STRING } };
static const ElReflect R_OL[]     = { { "type", "type", REFLECT_STRING }, { "reversed", "reversed", REFLECT_BOOL } };
static const ElReflect R_LI[]     = { { "type", "type", REFLECT_STRING } };
static const ElReflect R_TABLE[]  = { { "summary", "summary", REFLECT_STRING } };
static const ElReflect R_TD[]     = { { "headers", "headers", REFLECT_STRING }, { "abbr", "abbr", REFLECT_STRING } };
static const ElReflect R_COL[]    = { { "span", "span", REFLECT_STRING } };
static const ElReflect R_CANVAS[] = { { "width", "width", REFLECT_STRING }, { "height", "height", REFLECT_STRING } };
static const ElReflect R_DIALOG[] = { { "open", "open", REFLECT_BOOL } };
static const ElReflect R_DETAILS[]= { { "name", "name", REFLECT_STRING }, { "open", "open", REFLECT_BOOL } };
static const ElReflect R_SLOT[]   = { { "name", "name", REFLECT_STRING } };
/* §4.12.3's THREE PLAIN reflections. The other three shadow-root attributes are not plain mirrors:
   `shadowrootmode` and `shadowrootslotassignment` are enumerated and LIMITED TO ONLY KNOWN VALUES, so they
   live in declarative_shadow.c with the parser step that reads them, and `shadowrootcustomelementregistry`
   is absent with the registry interface (SPEC_STEPS.md §17.6). */
static const ElReflect R_TEMPLATE[] = {
    { "shadowRootDelegatesFocus", "shadowrootdelegatesfocus", REFLECT_BOOL },
    { "shadowRootClonable",       "shadowrootclonable",       REFLECT_BOOL },
    { "shadowRootSerializable",   "shadowrootserializable",   REFLECT_BOOL },
};
static const ElReflect R_DATA[]   = { { "value", "value", REFLECT_STRING } };
static const ElReflect R_METER[]  = { { "min", "min", REFLECT_STRING }, { "max", "max", REFLECT_STRING } };

#define RL(a) (a), (int)(sizeof(a) / sizeof((a)[0]))
#define RNONE NULL, 0

/* HTML §4's ELEMENT-INTERFACE TABLE. `tag` is the local name; several tags share one interface, which is what
   the spec says and why the interface is named per row rather than derived from the tag. */
static const struct { const char *tag; const char *iface; const ElReflect *refl; int nrefl; } HTML_IFACE[] = {
    { "a",          "HTMLAnchorElement",     RL(R_ANCHOR) },
    { "area",       "HTMLAreaElement",       RL(R_AREA) },
    { "link",       "HTMLLinkElement",       RL(R_LINK) },
    { "script",     "HTMLScriptElement",     RL(R_SCRIPT) },
    { "img",        "HTMLImageElement",      RL(R_IMG) },
    { "iframe",     "HTMLIFrameElement",     RL(R_IFRAME) },
    { "form",       "HTMLFormElement",       RL(R_FORM) },
    { "input",      "HTMLInputElement",      RL(R_INPUT) },
    { "button",     "HTMLButtonElement",     RL(R_BUTTON) },
    { "textarea",   "HTMLTextAreaElement",   RL(R_TEXTAREA) },
    { "select",     "HTMLSelectElement",     RL(R_SELECT) },
    { "option",     "HTMLOptionElement",     RL(R_OPTION) },
    { "optgroup",   "HTMLOptGroupElement",   RL(R_OPTGROUP) },
    { "label",      "HTMLLabelElement",      RL(R_LABEL) },
    { "output",     "HTMLOutputElement",     RL(R_OUTPUT) },
    { "fieldset",   "HTMLFieldSetElement",   RL(R_FIELDSET) },
    { "legend",     "HTMLLegendElement",     RNONE },
    { "meta",       "HTMLMetaElement",       RL(R_META) },
    { "base",       "HTMLBaseElement",       RL(R_BASE) },
    { "source",     "HTMLSourceElement",     RL(R_SOURCE) },
    { "track",      "HTMLTrackElement",      RL(R_TRACK) },
    { "audio",      "HTMLAudioElement",      RNONE },
    { "video",      "HTMLVideoElement",      RL(R_VIDEO) },
    { "object",     "HTMLObjectElement",     RL(R_OBJECT) },
    { "embed",      "HTMLEmbedElement",      RL(R_EMBED) },
    { "style",      "HTMLStyleElement",      RL(R_STYLE) },
    { "map",        "HTMLMapElement",        RL(R_MAP) },
    { "time",       "HTMLTimeElement",       RL(R_TIME) },
    { "data",       "HTMLDataElement",       RL(R_DATA) },
    { "meter",      "HTMLMeterElement",      RL(R_METER) },
    { "blockquote", "HTMLQuoteElement",      RL(R_QUOTE) },
    { "q",          "HTMLQuoteElement",      RL(R_QUOTE) },
    { "ins",        "HTMLModElement",        RL(R_MOD) },
    { "del",        "HTMLModElement",        RL(R_MOD) },
    { "ol",         "HTMLOListElement",      RL(R_OL) },
    { "li",         "HTMLLIElement",         RL(R_LI) },
    { "table",      "HTMLTableElement",      RL(R_TABLE) },
    { "td",         "HTMLTableCellElement",  RL(R_TD) },
    { "th",         "HTMLTableCellElement",  RL(R_TD) },
    { "col",        "HTMLTableColElement",   RL(R_COL) },
    { "colgroup",   "HTMLTableColElement",   RL(R_COL) },
    { "canvas",     "HTMLCanvasElement",     RL(R_CANVAS) },
    { "dialog",     "HTMLDialogElement",     RL(R_DIALOG) },
    { "details",    "HTMLDetailsElement",    RL(R_DETAILS) },
    { "slot",       "HTMLSlotElement",       RL(R_SLOT) },
    { "template",   "HTMLTemplateElement",   RL(R_TEMPLATE) },
    { "html",       "HTMLHtmlElement",       RNONE },
    { "head",       "HTMLHeadElement",       RNONE },
    { "body",       "HTMLBodyElement",       RNONE },
    { "title",      "HTMLTitleElement",      RNONE },
    { "div",        "HTMLDivElement",        RNONE },
    { "span",       "HTMLSpanElement",       RNONE },
    { "p",          "HTMLParagraphElement",  RNONE },
    { "pre",        "HTMLPreElement",        RNONE },
    { "br",         "HTMLBRElement",         RNONE },
    { "hr",         "HTMLHRElement",         RNONE },
    { "ul",         "HTMLUListElement",      RNONE },
    { "dl",         "HTMLDListElement",      RNONE },
    { "picture",    "HTMLPictureElement",    RNONE },
    { "tr",         "HTMLTableRowElement",   RNONE },
    { "tbody",      "HTMLTableSectionElement", RNONE },
    { "thead",      "HTMLTableSectionElement", RNONE },
    { "tfoot",      "HTMLTableSectionElement", RNONE },
    { "caption",    "HTMLTableCaptionElement", RNONE },
    { "h1",         "HTMLHeadingElement",    RNONE },
    { "h2",         "HTMLHeadingElement",    RNONE },
    { "h3",         "HTMLHeadingElement",    RNONE },
    { "h4",         "HTMLHeadingElement",    RNONE },
    { "h5",         "HTMLHeadingElement",    RNONE },
    { "h6",         "HTMLHeadingElement",    RNONE },
};
#define HTML_IFACE_N ((int)(sizeof(HTML_IFACE) / sizeof(HTML_IFACE[0])))

/* ONE CLASS PER ROW, not one prototype: the prototypes are PER REALM (§3.7) and this table is not, so a table
   of prototypes could only ever hold one document's — the same reason node.c's type table holds class ids.
   Rows that share an interface NAME share its class, which is what makes `q instanceof HTMLQuoteElement` and
   `blockquote instanceof HTMLQuoteElement` both true in every realm. */
static JSClassID g_iface_class[HTML_IFACE_N];
/* Each row's reflection BASE index — declared once per agent, installed into every realm's prototype. */
static int g_iface_refl_base[HTML_IFACE_N];
static int g_html_refl_base;

/* The interface a TAG wears. Linear over a table this size, and the answer is a borrowed prototype. A tag the
   table does not list is HTMLUnknownElement, which is HTML's own answer for an element it does not know — the
   custom-element case (a hyphenated name) is HTMLElement, which is a different answer and is stated here rather
   than folded in. */
static JSValue html_proto_for(JSContext *ctx, lxb_dom_element_t *el)
{
    size_t n = 0;
    const lxb_char_t *tag = lxb_dom_element_local_name(el, &n);
    int i;

    if (!tag || !n)
        return JS_GetClassProto(ctx, g_unknown_class);
    for (i = 0; i < HTML_IFACE_N; i++)
        if (strlen(HTML_IFACE[i].tag) == n && memcmp(HTML_IFACE[i].tag, tag, n) == 0)
            return JS_GetClassProto(ctx, g_iface_class[i]);
    /* §4.13: a VALID CUSTOM ELEMENT NAME (one containing a hyphen) is an HTMLElement, not an unknown one — a
       page that feature-tests `x instanceof HTMLUnknownElement` to find its own components would get the
       opposite answer, and this engine executes custom elements. */
    if (memchr(tag, '-', n))
        return JS_GetClassProto(ctx, g_html_class);
    return JS_GetClassProto(ctx, g_unknown_class);   /* OWNED, like every per-realm prototype read */
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
    /* WEB IDL §3.7.5's BRAND CHECK, and it is a THROW rather than an assert. This was a DCHECK whose message
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

void html_element_init(JSContext *ctx)
{
    JSClassDef hd = { "HTMLElement" }, ud = { "HTMLUnknownElement" };
    int i, j;

    DCHECK(!g_html_ready, "html_element_init ran twice — the interfaces are declared once per AGENT");
    JS_NewClassID(JS_GetRuntime(ctx), &g_html_class);
    JS_NewClass(JS_GetRuntime(ctx), g_html_class, &hd);
    JS_NewClassID(JS_GetRuntime(ctx), &g_unknown_class);
    JS_NewClass(JS_GetRuntime(ctx), g_unknown_class, &ud);
    /* ONE CLASS PER INTERFACE NAME. A row whose interface a previous row already claimed SHARES its class —
       `q` and `blockquote` are both HTMLQuoteElement, and two classes would make one of the two `instanceof`
       answers false in every realm. */
    for (i = 0; i < HTML_IFACE_N; i++) {
        for (j = 0; j < i; j++)
            if (strcmp(HTML_IFACE[j].iface, HTML_IFACE[i].iface) == 0) break;
        if (j < i) { g_iface_class[i] = g_iface_class[j]; continue; }
        {
            JSClassDef d = { HTML_IFACE[i].iface };
            JS_NewClassID(JS_GetRuntime(ctx), &g_iface_class[i]);
            JS_NewClass(JS_GetRuntime(ctx), g_iface_class[i], &d);
        }
    }
    /* EVERY REFLECTION DECLARED ONCE, here, with the base index each row's install names them by. */
    g_html_refl_base = element_declare_reflections(ctx, R_HTML, (int)(sizeof(R_HTML) / sizeof(R_HTML[0])));
    for (i = 0; i < HTML_IFACE_N; i++) {
        for (j = 0; j < i; j++)
            if (g_iface_class[j] == g_iface_class[i]) break;
        g_iface_refl_base[i] = (j < i) ? g_iface_refl_base[j]
                             : (HTML_IFACE[i].nrefl
                                    ? element_declare_reflections(ctx, HTML_IFACE[i].refl, HTML_IFACE[i].nrefl)
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
    /* §4.8.11's media element state machine — declared here because HTMLMediaElement.prototype is the parent
       of two rows of the table above, so this file is what decides when it must exist. */
    media_element_declare(ctx);
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
    event_target_install_click(ctx, html_p);
    /* §3.2.2 `[SameObject] attribute CSSStyleDeclaration style` — the attribute is HTMLElement's, the object is
       the CSSOM's, so each side owns its half. */
    cssom_install_style_attribute(ctx, html_p);
    idl_install_accessor(ctx, html_p, "dataset", js_html_dataset, 0, -1);
    /* The six §3.2.6/§6.8 global attributes whose IDL attribute COMPUTES a value from the tree rather than
       mirroring one attribute — installed onto THIS realm's prototype like every other member. */
    global_attributes_install(ctx, html_p);
    /* §4.13.2 `ElementInternals attachInternals()` — an HTMLElement member, installed on THIS realm's
       prototype like every other. */
    element_internals_install_html_members(ctx, html_p);
    /* §6.6.6's `HTMLOrSVGOrMathMLElement` members — `focus(options)` and `blur()`, the two entry points of
       §6.6.4's processing model. They were one body returning undefined; they are now the real algorithms,
       which move the document's focused area and fire the page's focus handlers (core/html/focus.c). */
    focus_install_html_members(ctx, html_p);
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
        if (HTML_IFACE[i].nrefl)
            element_install_reflections(ctx, p, g_iface_refl_base[i], HTML_IFACE[i].nrefl);
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
        CHECK(a != JS_ATOM_NULL, "the `content` attribute name could not be interned");
        JS_DefinePropertyGetSet(ctx, tpl, a,
                                JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)js_template_content, "content", 0,
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
        html_form_install(ctx, f, in, ta, op);
        JS_FreeValue(ctx, f); JS_FreeValue(ctx, in); JS_FreeValue(ctx, ta); JS_FreeValue(ctx, op);
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
    /* §4.13.2: HTMLElement's IDL carries `[HTMLConstructor]`, which is a real fifteen-step algorithm and not
       the shared "Illegal constructor" throw — a page's `class X extends HTMLElement { constructor(){super()} }`
       reaches it through super(), and every custom element in the platform is built by it. The machine belongs
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
    for (i = 0; i < HTML_IFACE_N; i++) {
        JSValue p;
        for (j = 0; j < i; j++)
            if (g_iface_class[j] == g_iface_class[i]) break;
        if (j < i) continue;   /* one interface OBJECT per interface, however many tags name it */
        p = JS_GetClassProto(ctx, g_iface_class[i]);
        node_install_interface(ctx, global, HTML_IFACE[i].iface, p);
        JS_FreeValue(ctx, p);
    }
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
    /* THE TWO SLOTS THIS FILE CLAIMED IN OTHER COMPONENTS, GIVEN BACK FIRST. §2.9's activation behaviour is
       core/events/event_target.c's slot pointing at core/html/hyperlink.c, and the element resolver is
       core/dom/node.c's slot pointing at this file — each is a callback INTO a component the cascade around
       this line is tearing down, which is the defect core/agent_state.h found in idb_transaction. Both
       receivers assert at their own release that the claim is gone. */
    hyperlink_free();
    node_set_element_resolver(NULL);
    dom_string_map_free(rt);
    global_attributes_free();
    declarative_shadow_free();
    html_form_free(rt);
    html_dialog_free(rt);
    media_element_free(rt);
    element_internals_free(rt);
    if (g_dataset_key != JS_ATOM_NULL) { JS_FreeAtomRT(rt, g_dataset_key); g_dataset_key = JS_ATOM_NULL; }
    /* the prototypes are the REALMS' — each is released with its context; the AGENT holds only class ids */
    g_html_ready = 0;
}
