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
 * WHAT IS HONESTLY ABSENT. The interfaces that need layout (offsetWidth, getBoundingClientRect), CSSOM (`style`),
 * or a media device are not here; the IDL audit names their members. A tag whose interface this table does not
 * list gets HTMLUnknownElement, which is what HTML says for an unknown element — not a shrug. */
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/idl_args.h"
#include "core/dom/node.h"
#include "core/dom/element.h"
#include "core/events/event_target.h"
#include "core/html/html_element.h"

static JSValue g_html_proto;          /* HTMLElement.prototype */
static JSValue g_unknown_proto;       /* HTMLUnknownElement.prototype — HTML's answer for a tag it does not know */

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
static const ElReflect R_ANCHOR[] = {
    { "href", "href", REFLECT_STRING }, { "target", "target", REFLECT_STRING },
    { "rel", "rel", REFLECT_STRING }, { "download", "download", REFLECT_STRING },
    { "hreflang", "hreflang", REFLECT_STRING }, { "type", "type", REFLECT_STRING },
    { "referrerPolicy", "referrerpolicy", REFLECT_STRING },
};
static const ElReflect R_AREA[] = {
    { "href", "href", REFLECT_STRING }, { "target", "target", REFLECT_STRING },
    { "rel", "rel", REFLECT_STRING }, { "alt", "alt", REFLECT_STRING },
};
static const ElReflect R_LINK[] = {
    { "href", "href", REFLECT_STRING }, { "rel", "rel", REFLECT_STRING },
    { "type", "type", REFLECT_STRING }, { "media", "media", REFLECT_STRING },
    { "as", "as", REFLECT_STRING }, { "crossOrigin", "crossorigin", REFLECT_STRING },
    { "integrity", "integrity", REFLECT_STRING }, { "referrerPolicy", "referrerpolicy", REFLECT_STRING },
    { "disabled", "disabled", REFLECT_BOOL },
};
static const ElReflect R_SCRIPT[] = {
    { "src", "src", REFLECT_STRING }, { "type", "type", REFLECT_STRING },
    { "integrity", "integrity", REFLECT_STRING }, { "crossOrigin", "crossorigin", REFLECT_STRING },
    { "referrerPolicy", "referrerpolicy", REFLECT_STRING },
    { "async", "async", REFLECT_BOOL }, { "defer", "defer", REFLECT_BOOL },
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
static const ElReflect R_MEDIA[]  = {
    { "src", "src", REFLECT_STRING }, { "crossOrigin", "crossorigin", REFLECT_STRING },
    { "preload", "preload", REFLECT_STRING },
    { "autoplay", "autoplay", REFLECT_BOOL }, { "loop", "loop", REFLECT_BOOL },
    { "controls", "controls", REFLECT_BOOL },
};
static const ElReflect R_VIDEO[]  = {
    { "src", "src", REFLECT_STRING }, { "poster", "poster", REFLECT_STRING },
    { "crossOrigin", "crossorigin", REFLECT_STRING }, { "preload", "preload", REFLECT_STRING },
    { "autoplay", "autoplay", REFLECT_BOOL }, { "loop", "loop", REFLECT_BOOL },
    { "controls", "controls", REFLECT_BOOL }, { "playsInline", "playsinline", REFLECT_BOOL },
};
static const ElReflect R_OBJECT[] = {
    { "data", "data", REFLECT_STRING }, { "type", "type", REFLECT_STRING },
    { "name", "name", REFLECT_STRING },
};
static const ElReflect R_EMBED[]  = { { "src", "src", REFLECT_STRING }, { "type", "type", REFLECT_STRING } };
static const ElReflect R_STYLE[]  = { { "media", "media", REFLECT_STRING }, { "disabled", "disabled", REFLECT_BOOL } };
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
    { "audio",      "HTMLAudioElement",      RL(R_MEDIA) },
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
    { "template",   "HTMLTemplateElement",   RNONE },
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

/* One prototype per ROW. Rows that share an interface name share its prototype, which is what makes
   `q instanceof HTMLQuoteElement` and `blockquote instanceof HTMLQuoteElement` both true. */
static JSValue g_iface_proto[HTML_IFACE_N];

/* The interface a TAG wears. Linear over a table this size, and the answer is a borrowed prototype. A tag the
   table does not list is HTMLUnknownElement, which is HTML's own answer for an element it does not know — the
   custom-element case (a hyphenated name) is HTMLElement, which is a different answer and is stated here rather
   than folded in. */
static JSValueConst html_proto_for(lxb_dom_element_t *el)
{
    size_t n = 0;
    const lxb_char_t *tag = lxb_dom_element_local_name(el, &n);
    int i;

    if (!tag || !n)
        return g_unknown_proto;
    for (i = 0; i < HTML_IFACE_N; i++)
        if (strlen(HTML_IFACE[i].tag) == n && memcmp(HTML_IFACE[i].tag, tag, n) == 0)
            return g_iface_proto[i];
    /* §4.13: a VALID CUSTOM ELEMENT NAME (one containing a hyphen) is an HTMLElement, not an unknown one — a
       page that feature-tests `x instanceof HTMLUnknownElement` to find its own components would get the
       opposite answer, and this engine executes custom elements. */
    if (memchr(tag, '-', n))
        return g_html_proto;
    return g_unknown_proto;
}

/* §3.2.2 focus() and blur(). A headless run has no focus ring, and the spec defines no scriptable result for
   either beyond moving the focus — which is a state this engine does not model — so each is a documented
   no-effect rather than a value invented for it. click() is NOT one of these: it fires a real event, so it is
   implemented where the dispatch machine is. */
static JSValue js_html_focus(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    (void)ctx; (void)this_val; (void)argc; (void)argv; (void)magic;
    return JS_UNDEFINED;
}

void html_element_init(JSContext *ctx)
{
    int i, j;

    DCHECK(!JS_IsObject(g_html_proto), "html_element_init ran twice — one instance is one document");

    /* §3.2.2 `interface HTMLElement : Element`. */
    g_html_proto = JS_NewObjectProto(ctx, element_proto());
    CHECK(!JS_IsException(g_html_proto), "HTMLElement.prototype could not be allocated");
    element_install_reflections(ctx, g_html_proto, R_HTML, (int)(sizeof(R_HTML) / sizeof(R_HTML[0])));
    /* HTML mixes GlobalEventHandlers and DocumentAndElementEventHandlers into HTMLElement, not into Element —
       so this is where they belong, and where `div.onclick = f` now reaches them. */
    event_target_install_handlers(ctx, g_html_proto, EH_GLOBAL);
    event_target_install_click(ctx, g_html_proto);
    JS_SetPropertyStr(ctx, g_html_proto, "focus",
                      JS_NewCFunctionMagic(ctx, js_html_focus, "focus", 0, JS_CFUNC_generic_magic, 0));
    JS_SetPropertyStr(ctx, g_html_proto, "blur",
                      JS_NewCFunctionMagic(ctx, js_html_focus, "blur", 0, JS_CFUNC_generic_magic, 1));

    g_unknown_proto = JS_NewObjectProto(ctx, g_html_proto);   /* §4: `interface HTMLUnknownElement : HTMLElement` */
    CHECK(!JS_IsException(g_unknown_proto), "HTMLUnknownElement.prototype could not be allocated");

    for (i = 0; i < HTML_IFACE_N; i++) {
        /* A row whose interface a previous row already built SHARES its prototype — `q` and `blockquote` are
           both HTMLQuoteElement, and two objects would make one of the two `instanceof` answers false. */
        for (j = 0; j < i; j++)
            if (strcmp(HTML_IFACE[j].iface, HTML_IFACE[i].iface) == 0) break;
        if (j < i) { g_iface_proto[i] = JS_DupValue(ctx, g_iface_proto[j]); continue; }
        g_iface_proto[i] = JS_NewObjectProto(ctx, g_html_proto);
        CHECK(!JS_IsException(g_iface_proto[i]), "a per-tag interface prototype could not be allocated");
        if (HTML_IFACE[i].nrefl)
            element_install_reflections(ctx, g_iface_proto[i], HTML_IFACE[i].refl, HTML_IFACE[i].nrefl);
    }

    /* node.c keys its prototype table by node TYPE; an element's interface is keyed by its LOCAL NAME, which is
       HTML's mapping and not the DOM's. So the base ASKS, and stays the one place a wrapper is built. */
    node_set_element_resolver(html_proto_for);
}

void html_element_install(JSContext *ctx, JSValueConst global)
{
    int i, j;

    DCHECK(JS_IsObject(g_html_proto), "the HTML interface objects were installed before their prototypes existed");
    node_install_interface(ctx, global, "HTMLElement", g_html_proto);
    node_install_interface(ctx, global, "HTMLUnknownElement", g_unknown_proto);
    for (i = 0; i < HTML_IFACE_N; i++) {
        for (j = 0; j < i; j++)
            if (strcmp(HTML_IFACE[j].iface, HTML_IFACE[i].iface) == 0) break;
        if (j < i) continue;   /* one interface OBJECT per interface, however many tags name it */
        node_install_interface(ctx, global, HTML_IFACE[i].iface, g_iface_proto[i]);
    }
}

void html_element_free(JSContext *ctx)
{
    int i;

    for (i = 0; i < HTML_IFACE_N; i++) {
        JS_FreeValue(ctx, g_iface_proto[i]);
        g_iface_proto[i] = JS_UNDEFINED;
    }
    JS_FreeValue(ctx, g_unknown_proto);
    JS_FreeValue(ctx, g_html_proto);
    g_unknown_proto = g_html_proto = JS_UNDEFINED;
}
