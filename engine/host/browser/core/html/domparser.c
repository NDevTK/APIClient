/* HTML §8.5.1 — the `DOMParser` interface.
 *
 * WHAT IT IS. Two members over one algorithm: `new DOMParser()`, whose "constructor steps are to do nothing",
 * and `parseFromString(string, type)`, which builds a SECOND DOCUMENT — one with no browsing context, no
 * navigable, no Window and no scripts — out of a string the page hands it. DOM §4.5.1's `createHTMLDocument`
 * builds the same kind of document from a fixed skeleton and this builds one from the page's markup, which is
 * why both go through core/dom/document.h's `document_new` rather than each minting a record of its own.
 *
 * THE HTML ARM IS THE PARSE THE ENGINE ALREADY RUNS. §8.5.1's "parse HTML from a string" is
 * `html_parse_document` over a document created for it, exactly as `document_install` runs it for a
 * navigation and `xhr_set_document_response` runs it for XHR §3.6.6's HTML arm. What differs is not the parse
 * but the four PARSE-BOUNDARY seams a lexbor tree needs afterwards, and WHICH of them §8.5.1 asks for is
 * decided by the standard rather than by symmetry — see the seams at the end of `parse_html_from_a_string`.
 *
 * THE XML ARM IS A DIFFERENT PARSER AND A DIFFERENT SET OF SEAMS — core/xml/xml_parse.h, which the two other
 * entries that parse XML in this engine share (XMLHttpRequest §3.6.6 "set a document response" step 6, and
 * HTML §7.5.3 "Loading XML documents" through core/loader/xml_document.h). What is specific to §8.5.1 is the
 * SCRIPTING answer — this parser is created "with XML scripting support disabled", so HTML §14.2's script
 * steps are not owed here and ARE owed at §7.5.3, which is why the two entries differ rather than sharing a
 * flag — and the `parsererror` document, which is §8.5.1's own consequence for a well-formedness error and not
 * something the parser decides. See `parse_xml_from_a_string` for which of the HTML arm's parse-boundary seams
 * do not run and for each one's own reason.
 *
 * "THIS'S RELEVANT GLOBAL OBJECT'S ASSOCIATED DOCUMENT" IS HELD BY THE OBJECT, and that is why this interface
 * has a record at all when the standard gives it no state. §8.5.1 step 2 takes the new Document's URL from the
 * DOMParser's OWN relevant global, not from whoever called the method — and a C member runs in the realm that
 * DEFINED it (js_call_c_function takes `ctx` from the function object), so
 * `frames[0].DOMParser.prototype.parseFromString.call(myParser, …)` arrives with the FRAME's ctx while the
 * standard asks for THIS document's URL. core/html/user_activation.c has the same question and can only assert
 * against it, because a UserActivation is minted with a realm and carries nothing that says whose it is. This
 * object is CONSTRUCTED, so it can carry it: the constructor holds this realm's `document` wrapper, and step 2
 * reads that Document's address. Holding the DOCUMENT rather than the realm is what the standard says (a Window
 * has exactly one associated Document for its whole life) and is also the version with no dangling pointer —
 * the wrapper keeps the Document alive for as long as a page can still reach the parser.
 *
 * THE RECORD NEEDS NO COW CAPTURE, and that is the spec rather than an omission — the same shape as
 * core/dom/document.h's active sandboxing flag set. The field has exactly ONE write, in the constructor, before
 * the object is reachable by anything, so a per-flow delta could never hold two values of it. What is per-flow
 * is the ANSWER: the Document's address rides that document's own delta (document_set_url), so the arm that
 * called `history.pushState(s, "", "/b")` is the arm whose `parseFromString` resolves against `/b`. */
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>
#include <lexbor/html/html.h>

#include "check.h"
#include "quickjs.h"
#include "solver/concolic.h"
#include "core/agent_state.h"
#include "core/dom/attr_list.h"
#include "core/dom/document.h"
#include "core/dom/node.h"
#include "core/html/domparser.h"
#include "core/html/html_parse.h"      /* the ONE place a Document is parsed — that header owns the token bytes */
#include "core/html/html_style_element.h"
#include "core/html/html_script.h"   /* §4.12.1.1's `force async`: the stamp every parser makes */
#include "core/html/media_element.h"
#include "core/html/event_handler_attribute.h"
#include "core/html/html_image.h"
#include "core/html/trusted_types.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "core/xml/xml_parse.h"        /* the ONE place an XML document is parsed — shared with §3.6.6 and §7.5.3 */
#include "solver/dom_cow.h"
#include "core/dom/node_interface.h"   /* the ONE place a Document is made — see that header */

/* PER REALM — §3.7. The class is the AGENT's; the prototype lives in quickjs's per-context class-proto slot. */
static JSClassID g_class;
static int       g_ready;
static int       g_id_ctor = -1, g_id_parse = -1;

/* §8.5.1's `enum DOMParserSupportedType`, in the order the IDL lists it. The list IS the type: a `type` that is
   not one of these five is a TypeError thrown by the CONVERSION, before step 1, which is what
   DOMParser-parseFromString-html.html's "throws on an invalid enum value" asserts and what makes this file's
   own switch total. */
IDL_ENUM_VALUES(DOM_PARSER_SUPPORTED_TYPE,
    "text/html", "text/xml", "application/xml", "application/xhtml+xml", "image/svg+xml");

/* THE OBJECT'S WHOLE STATE — see the head comment. */
typedef struct { JSValue document; } DomParser;

/* THE COLLECTOR'S TWO ENTRIES READ NO STATIC THIS COMPONENT'S RELEASE RESETS — core/agent_state.h's rule. Both
   run AFTER core/platform.c's release column (every host's teardown is `platform_agent_free()` … `JS_RunGC` …
   `JS_FreeRuntime`), so a DOMParser a page still holds would be finalized with `g_class` already back at 0 and
   `JS_GetOpaque(val, 0)` would answer NULL for it: the record leaks, and the unmarked Document it holds keeps
   the internal reference gc_decref exists to subtract, so gc_scan reads that document's realm as rooted from
   outside the heap and never collects it. JS_GetAnyOpaque, because the collector dispatched here THROUGH the
   class — the id is a fact it already has and must not look up. */
static void domparser_finalizer(JSRuntime *rt, JSValue val)
{
    JSClassID id = 0;
    DomParser *p = JS_GetAnyOpaque(val, &id);

    (void)id;
    /* NOT `if (!p) return;`. js_domparser_ctor is the one mint and it sets the record with nothing in between
       that allocates in the JS heap. */
    DCHECK(p != NULL, "a DOMParser was finalized with no record — §8.5.1's one mint sets it with nothing in "
                      "between that could collect");
    JS_FreeValueRT(rt, p->document);
    free(p);
}

static void domparser_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    JSClassID id = 0;
    DomParser *p = JS_GetAnyOpaque(val, &id);

    (void)id;
    DCHECK(p != NULL, "a DOMParser was marked with no record — the Document it holds is a counted reference "
                      "and an unmarked child is read by gc_scan as rooted from outside the heap");
    JS_MarkValue(rt, p->document, mark_func);
}

/* `new DOMParser()` — "the constructor steps are to do nothing", plus the one fact the object has to carry so
   that step 2 can be answered out of the RIGHT realm (head comment). */
static JSValue js_domparser_ctor(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValue proto, obj;
    DomParser *p;

    (void)this_val; (void)argc; (void)argv; (void)magic;
    DCHECK(g_ready, "a DOMParser was constructed before its interface was declared");
    proto = JS_GetClassProto(ctx, g_class);
    DCHECK(!JS_IsNull(proto), "DOMParser.prototype was asked for in a realm that never ran its install");
    obj = JS_NewObjectProtoClass(ctx, proto, g_class);
    JS_FreeValue(ctx, proto);
    CHECK(!JS_IsException(obj), "a DOMParser object could not be allocated");
    p = calloc(1, sizeof *p);
    CHECK(p != NULL, "the DOMParser record allocation failed");
    /* §8.5.1 step 2's subject, captured HERE because here is where the relevant global is known. It is the
       REALM'S ACTIVE document — `[Exposed=Window]` means the constructor exists on a Window and nowhere else,
       and document_object is that Window's. Held, not borrowed. */
    p->document = JS_DupValue(ctx, document_object(ctx));
    DCHECK(JS_IsObject(p->document),
           "a DOMParser was constructed in a realm whose `document` object does not exist — §8.5.1 step 2 reads "
           "the relevant global's associated Document's URL, and a realm with no Document has no address to "
           "resolve the parsed document's relative URLs against");
    JS_SetOpaque(obj, p);
    return obj;
}

/* WEB IDL §3.7.7 Operations' BRAND CHECK. `DOMParser.prototype.parseFromString` is reachable off the
   prototype with
   `.call` on anything a page likes, so the receiver is the PAGE'S input and this is a THROW rather than an
   assert. A receiver that IS a DOMParser and has no record is an ENGINE invariant: the constructor is the only
   way one exists and it sets the record before returning. */
static DomParser *domparser_of(JSContext *ctx, JSValueConst this_val)
{
    DomParser *p;

    if (JS_GetClassID(this_val) != g_class) {
        JS_ThrowTypeError(ctx, "this is not a DOMParser");
        return NULL;
    }
    p = JS_GetOpaque(this_val, g_class);
    DCHECK(p != NULL, "a DOMParser member ran on an object with no record — the constructor is the only way one "
                      "of these exists and it installs the record before the object is reachable");
    return p;
}

/* THE BYTES AN ARGUMENT REALLY IS. Unknown external input CROSSES THE IDL BOUNDARY AS ITSELF (core/idl_args.c:
   opacity has to survive a coercion or the value stops forking control flow), so a body that reaches for
   JS_ToCString without asking would hand the parser a concolic's display shape as markup.
   THE ANSWER IS THE CONCOLIC'S OWN EXAMPLE, which is the solver's rule and not a softening of it: the engine
   RUNS THE REAL OP on the concrete, so `parseFromString(location.hash.slice(1), "text/html")` parses the
   example the source is carrying. A concolic with NO example yet parses the EMPTY STRING — the same nothing
   `el.innerHTML = <opaque>` leaves behind (core/dom/element.c returns before its parse for exactly this value),
   and never invented markup. Returns an OWNED value the caller stringifies; JS_UNDEFINED is never returned. */
static JSValue domparser_concrete(JSContext *ctx, JSValueConst v)
{
    JSValue ex;

    if (!concolic_is(v)) return JS_DupValue(ctx, v);
    ex = concolic_example(ctx, v);
    if (JS_IsString(ex)) return ex;
    JS_FreeValue(ctx, ex);
    return JS_NewStringLen(ctx, "", 0);
}

/* HTML §8.5.1's "parse HTML from a string", given a Document `document` and a string `html`.
 *
 * 1. Set document's type to "html". — the CONTENT TYPE is what records that here (DOM §4.5, and
 *    core/dom/document.h's own note on it), and step 2 of parseFromString already set it to `type`.
 * 2. A new HTML parser whose ALLOW DECLARATIVE SHADOW ROOTS is document's — which is FALSE for every document
 *    document_new builds, so this is the one parse-boundary seam that must not run (see below).
 * 3-4. Place `html` into the input stream and run the parser to the end. Lexbor's document parse is that, in
 *    one call, which is what every other document parse in this engine is.
 *
 * "Since document does not have a browsing context, SCRIPTING IS DISABLED" is not a flag this sets: a document
 * built by document_new has no navigable and no Window, and lexbor's own `scripting` is false on a document
 * created with lxb_html_document_create — which is what makes `<noscript>` parse as MARKUP and is what
 * DOMParser-parseFromString-html.html's last subtest asserts. */
static JSValue parse_html_from_a_string(JSContext *ctx, const char *url, const char *html, size_t len)
{
    lxb_html_document_t *dom = dom_document_create();
    lxb_dom_node_t *root;
    JSValue doc;

    CHECK(dom != NULL, "DOMParser: OOM building the parsed Document");
    /* HTML parsing has no failure mode over well-formedness — tag soup is a tree — so a non-OK status here is
       an allocation, exactly as it is at the engine's three other document parses. */
    /* FLOW-PRIVATE: the Document was created a statement ago and nothing between the two runs the page's code,
       so no sibling flow can hold it and §13.2.6's writes need no delta entry (solver/dom_cow.h). */
    CHECK(html_parse_document(dom, DOM_PARSE_ROOT_PRIVATE, HTML_SCRIPTING_DISABLED, (const lxb_char_t *)html, len) == LXB_STATUS_OK,
          "DOMParser: the markup handed to parseFromString could not be parsed");
    /* §8.5.1 step 2: the URL is the relevant global's associated Document's, and the content type is `type` —
       which for this arm is "text/html", the string that makes a Document an HTML document. */
    /* §8.5.1 step 2's content type is `type`, which on THIS arm is the string "text/html" that selected it —
       and step 3's first sub-step, "Set document's type to `html`", is the other half of the pair. */
    /* …and step 2's "a NEW DOCUMENT" is DOM §4.5's `Document` interface, which this arm shares with the XML one
       below — see there for why the interface is stated rather than taken from the type. */
    doc = document_new(ctx, dom, url, DOCUMENT_IFACE_DOCUMENT, document_kind(/*is_xml*/false, "text/html"));
    root = lxb_dom_interface_node(dom);

    /* THE PARSE BOUNDARY, and which seams belong to it is the STANDARD'S answer rather than a copy of
       document_install's list. Nothing here can be observed by the page: no script of this document runs, and
       the page has not been handed the Document yet.
       - The NAMESPACE correction is unconditional: HTML tree construction produces attributes in the NULL
         namespace and lexbor stamps each with its element's instead, so without it `doc.documentElement.id`
         reads nothing and every `getAttributeNS(null, …)` misses.
       - §4.2.6's `<style>` seam is unconditional too: "the element is popped off the stack of open elements of
         an HTML parser" is the trigger, and it says nothing about a browsing context —
         DOMParser-parseFromString-stylesheets.html is six subtests over exactly that.
       - §4.8.11.2's media seam likewise: "created with a src attribute" is a fact about the ELEMENT.
       - §13.2.6.4.4's DECLARATIVE SHADOW conversion is NOT run, and that is the spec: the parser's "allow
         declarative shadow roots" is the DOCUMENT'S, a Document's is false initially (DOM §4.5), and only HTML's
         "read html" and §7.4's create-and-initialize set it — neither of which a document_new document reaches.
         So a `<template shadowrootmode>` stays a `<template>`, which core/dom/document.c states by name.
       - §13.2.4.5's INERT script marking is NOT run here, and it does not have to be: this is a DOCUMENT
         parse, so its `<script>` elements are marked by §4.12.1.1's OWN step 15 at each end tag
         (core/html/html_script.c's html_script_parser_inserted, which §13.2.6.4.8 'The "text" insertion mode'
         reaches with no condition on the scripting mode), and step 18 — "If scripting is disabled for el,
         then return" — is what stops them running. A `<script>` moved out of a DOMParser document into a live
         one therefore does NOT run, which is what makes parseFromString a laundering-proof primitive.
         THIS COMMENT SAID THE OPPOSITE AND CITED BROWSERS FOR IT ("therefore runs, exactly as it does in a
         browser"), and it was false in both halves: §4.12.1.1 puts step 15 THREE STEPS AHEAD of step 18, so
         the flag is set for exactly the documents whose scripts never run. It read as authoritative and told
         the reader not to open the section — the one shape of wrong comment that stops the check it fails.
         What IS run here is §4.12.1.1's other parser stamp, `force async` — that one is unconditional ("set
         to false by the HTML parser … on script elements they insert") and says nothing about a scripting
         mode, so the `<script>` that gets moved out carries the same flag it would have carried in any other
         document. */
    dom_attr_normalize_parsed(root);
    html_script_parsed(ctx, root, /*inert*/false);
    html_style_element_parsed(ctx, root);
    media_element_parsed(ctx, root);
    /* HTML §4.8.4.3.2 for the same tree, beside the media walk and for its reason. It costs a tag test per
       node and issues nothing here: a `DOMParser` document is not FULLY ACTIVE, which is §4.8.4.3.5's own
       first step and the reason a browser's `parseFromString` does not load the images in the markup it was
       given. The walk is still made rather than skipped, because whether a document is fully active is the
       ALGORITHM's question about the element's node document and not this call site's about its own. */
    html_image_parsed(ctx, root);
    /* HTML §8.1.8.1 Event handlers for the same tree and for the media walk's reason: a lexbor parse reaches
       none of DOM §4.9's mutation chokepoint, so a `<div onclick="x">` in the markup `parseFromString` was
       handed is a handler no chokepoint ever saw. The walk is MADE rather than skipped for a document with no
       browsing context, for the reason the image walk above is made: whether the handler can act upon anything
       is §8.1.8.1's determine the target of an event handler's question about the element's node document —
       which is what answers null for a `<body onload>` here — and not this call site's about its own. */
    event_handler_attribute_parsed(ctx, root);
    return doc;
}

/* §8.5.1 step 3's OTHERWISE arm: "Create an XML parser parser, associated with document, and with XML
 * scripting support disabled. Parse compliantString using parser." — then its next step, which builds a
 * `parsererror` document when that parse reported "an XML well-formedness or XML namespace well-formedness
 * error". core/xml/xml_parse.h owns both halves and the three consumers of an XML parse share it.
 *
 * NONE OF THE HTML ARM'S PARSE-BOUNDARY SEAMS RUN HERE, and each is a different standard's answer rather than
 * one blanket "XML is different":
 *  - `dom_attr_normalize_parsed` must NOT run. It exists because HTML tree construction produces attributes in
 *    the null namespace while lexbor stamps each with its element's, and the difference is only knowable at
 *    the boundary (core/dom/attr_list.h). An XML attribute's namespace is Namespaces in XML §6.2's EXPANSION,
 *    computed from the bindings in scope and written straight through `dom_attr_write` — so there is nothing
 *    to correct, and running the correction would rewrite names the standard just decided.
 *  - The `<script>` stamps do not run because §8.5.1 names the reason itself: this parser has XML SCRIPTING
 *    SUPPORT DISABLED, so HTML §14.2's steps — the parser document, the force-async clear, the prepare at the
 *    end tag — are not owed for THIS entry. §7.5.3's loader is the entry that IS owed them, and
 *    core/loader/xml_document.c is where it asks core/xml/xml_parse.h for the end-tag boundary and reaches
 *    core/html/html_script.h's ONE preparation with it. That component parses through the same
 *    `xml_parse_document` this arm does, and never asks — which is what makes the scripting mode a fact about
 *    the CONSUMER rather than a flag inside the walk.
 *  - `<style>`, media and image are HTML §4.2.6, §4.8.11.2 and §4.8.4.3.2 element-parsed seams, whose triggers
 *    are stated over an HTML parser's stack of open elements. An `svg` or arbitrary-namespace element that
 *    happens to be spelled `style` in an XML document is not that element.
 *
 * `document has no child nodes` IS ASSERTED, which is §8.5.1's own first step for this branch — the Document
 * was created a statement ago and nothing between the two runs the page's code. */
static JSValue parse_xml_from_a_string(JSContext *ctx, const char *url, const char *type,
                                       const char *xml, size_t len)
{
    lxb_html_document_t *dom = dom_document_create();
    lxb_dom_node_t      *root;
    XmlParseReport       report;

    CHECK(dom != NULL, "DOMParser: OOM building the parsed Document");
    root = lxb_dom_interface_node(dom);
    /* FLOW-PRIVATE, for `parse_html_from_a_string`'s reason and with its argument: the Document was created a
       statement ago and no sibling flow can hold it (solver/dom_cow.h). */
    if (!xml_parse_document(lxb_dom_interface_document(dom), root, DOM_PARSE_ROOT_PRIVATE, xml, len, &report)) {
        lxb_dom_node_t *c, *next;
        /* The partial tree is discarded so that §8.5.1's "Assert: document has no child nodes" holds where the
           standard writes it — one line below, over the document this branch is about to describe.
           THROUGH THE PRIVATE-TREE SEAM AND NOT THE CAPTURING REMOVE. This Document is DOM_PARSE_ROOT_PRIVATE
           and the parse recorded no creation for anything under it, so a captured removal here would put the
           partial tree's structure in a delta that has none of it — the invariant that a delta captures only
           SHARED baseline state (CLAUDE.md §State isolation). `dom_cow_take_private` is the seam at which a
           node LEAVES the private root, and it makes the flow the owner: these ones are on their way to the
           flow's own discard rather than to the document, and the delta destroys them exactly as it destroys
           any other node the flow made. */
        for (c = root->first_child; c != NULL; c = next) { next = c->next; dom_cow_take_private(root, c); }
        DCHECK(root->first_child == NULL,
               "HTML §8.5.1 step 3's parsererror branch begins \"Assert: document has no child nodes\" and "
               "this document still has one — the partial tree an ill-formed parse left is what was just "
               "discarded, so a survivor is a node nothing in this build put there");
        xml_parse_error_document(lxb_dom_interface_document(dom), root, DOM_PARSE_ROOT_PRIVATE, &report);
    }
    /* §8.5.1 step 2: the URL is the relevant global's associated Document's, and the content type is `type` —
       which for this arm is whichever XML DOMParserSupportedType the caller passed, unchanged. */
    /* …AND ITS TYPE STAYS `xml`. Step 2's Document is a new Document, whose §4.5 default type is `xml`, and
       only the "text/html" arm's first sub-step sets it to `html` — so this arm is an XML DOCUMENT, which is
       what makes `parseFromString(…, "application/xml").createCDATASection(…)` work and what keeps HTML
       §13.2's parse-boundary correction (which this arm deliberately does not run) unreachable for it. */
    /* …AND IT IMPLEMENTS `Document`, NOT `XMLDocument`, WHICH IS THE ONE PLACE THAT DISTINCTION IS OBSERVABLE
       AND THE ONE PLACE A READER EXPECTS THE OTHER ANSWER. §8.5.1 step 2 is "Let document be a NEW DOCUMENT,
       whose content type is type and URL is this's relevant global object's associated Document's URL" — a new
       Document, where DOM §4.5.1's createDocument says "creating a document that implements XMLDocument". Same
       §4.5 type, different interface, so `new DOMParser().parseFromString("<foo/>", "text/xml") instanceof
       XMLDocument` is FALSE and `document.implementation.createDocument(ns, "")`'s is true. */
    return document_new(ctx, dom, url, DOCUMENT_IFACE_DOCUMENT, document_kind(/*is_xml*/true, type));
}

/* §8.5.1 `parseFromString(string, type)`.
 *
 * 1. `compliantString` ← get trusted type compliant string with TrustedHTML, this's relevant global object,
 *    string, "DOMParser parseFromString", "script". — 2. `document` ← a new Document whose content type is
 *    `type` and whose URL is this's relevant global object's associated Document's URL. — 3. Switch on `type`:
 *    "text/html" parses HTML from a string; otherwise an XML parser. — 4. Return `document`. */
static JSValue js_domparser_parse_from_string(JSContext *ctx, JSValueConst this_val, int argc,
                                              JSValueConst *argv, int magic)
{
    DomParser *p = domparser_of(ctx, this_val);
    JSValue compliant, concrete, doc;
    const char *type, *markup, *url;
    size_t len = 0;

    (void)magic; (void)argc;
    if (!p) return JS_EXCEPTION;                 /* §3.7.7 Operations' brand check threw */

    /* STEP 1. §2's TrustedHTML does not exist in this engine, so the "is an instance of the expected type" step
       is DECIDED rather than skipped and a document under `require-trusted-types-for 'script'` gets the
       TypeError §3.4 step 6 specifies — which is what a real browser gives a page whose default policy was
       never created. core/html/trusted_types.h states both halves. */
    compliant = trusted_types_compliant_string(ctx, TRUSTED_TYPE_HTML, argv[0], "DOMParser parseFromString");
    if (JS_IsException(compliant)) return compliant;

    /* The ENUMERATION. The declaration converted it, so a real string here is one of the five — but unknown
       external input crosses as itself (see domparser_concrete), so a concolic `type` reached this body with
       Web IDL §3.2.18's membership test never run. Its example decides which parser the page asked for, and an example
       that is not one of the five is the TypeError the conversion would have thrown for that string. */
    concrete = domparser_concrete(ctx, argv[1]);
    type = JS_ToCString(ctx, concrete);
    JS_FreeValue(ctx, concrete);
    if (!type) { JS_FreeValue(ctx, compliant); return JS_EXCEPTION; }
    {
        int i;
        for (i = 0; DOM_PARSER_SUPPORTED_TYPE[i]; i++)
            if (strcmp(type, DOM_PARSER_SUPPORTED_TYPE[i]) == 0) break;
        if (!DOM_PARSER_SUPPORTED_TYPE[i]) {
            JS_FreeCString(ctx, type);
            JS_FreeValue(ctx, compliant);
            return JS_ThrowTypeError(ctx, "parseFromString: the type is not a DOMParserSupportedType");
        }
    }

    /* STEP 2's URL — this's relevant global object's associated Document's, which the constructor captured and
       which rides THAT document's own per-flow delta. */
    url = document_url_of(lxb_dom_interface_document(node_of(p->document)));
    DCHECK(url != NULL, "a DOMParser's associated Document has no address — document_new and document_install "
                        "are the only two ways a Document exists here and both take one");

    if (strcmp(type, "text/html") != 0) {
        /* STEP 3's OTHERWISE arm — the XML parser, with XML scripting support disabled. */
        JSValue xdoc;
        concrete = domparser_concrete(ctx, compliant);
        JS_FreeValue(ctx, compliant);
        DCHECK(JS_IsString(concrete),
               "parseFromString reached its XML parse with markup that is neither a string nor unknown "
               "external input — the IDL declaration is what converts the argument, and running the page's "
               "toString from here is the drive-to-completion the flow machinery exists to avoid");
        markup = JS_ToCStringLen(ctx, &len, concrete);
        JS_FreeValue(ctx, concrete);
        if (!markup) { JS_FreeCString(ctx, type); return JS_EXCEPTION; }
        xdoc = parse_xml_from_a_string(ctx, url, type, markup, len);
        JS_FreeCString(ctx, markup);
        JS_FreeCString(ctx, type);
        return xdoc;                                 /* step 4 */
    }

    concrete = domparser_concrete(ctx, compliant);
    JS_FreeValue(ctx, compliant);
    DCHECK(JS_IsString(concrete),
           "parseFromString reached its parse with markup that is neither a string nor unknown external input — "
           "the IDL declaration is what converts the argument, and running the page's toString from here is the "
           "drive-to-completion the flow machinery exists to avoid");
    markup = JS_ToCStringLen(ctx, &len, concrete);
    JS_FreeValue(ctx, concrete);
    JS_FreeCString(ctx, type);
    if (!markup) return JS_EXCEPTION;
    doc = parse_html_from_a_string(ctx, url, markup, len);
    JS_FreeCString(ctx, markup);
    return doc;                                  /* step 4 */
}

void domparser_init(JSContext *ctx)
{
    JSClassDef d = { "DOMParser", domparser_finalizer, domparser_gc_mark };
    static const IdlArgType PARSE_ARGS[2] = { IDL_DOMSTRING, IDL_ENUM };

    DCHECK(!g_ready, "domparser_init ran twice — the interface is declared once per AGENT");
    JS_NewClassID(JS_GetRuntime(ctx), &g_class);
    JS_NewClass(JS_GetRuntime(ctx), g_class, &d);
    g_id_ctor = idl_method_id(ctx, NULL, 0, js_domparser_ctor, 0);
    /* §8.5.1: `[NewObject] Document parseFromString((TrustedHTML or DOMString) string,
       DOMParserSupportedType type)`. The union's first arm is §2's TrustedHTML, which does not exist in this
       engine — so the union IS its DOMString arm, which is the same collapse core/dom/element.c's innerHTML
       setter declares, and step 1 is still run because that is where the throw comes from. */
    g_id_parse = idl_method_id(ctx, PARSE_ARGS, 2, js_domparser_parse_from_string, 0);
    idl_arg_enum(1, DOM_PARSER_SUPPORTED_TYPE);   /* §3.2.18's values for the `type` position */
    g_ready = 1;
    /* WHAT THIS COMPONENT HOLDS FOR THE AGENT, DECLARED — core/agent_state.h. This row was on
       core/platform.c's list with an EMPTY release column and no release function existed at all, so all four
       of these survived the runtime they were made in and nothing could say so: a class id and a pool entry
       are ints, so neither of JS_FreeRuntime's censuses has anything to report, and the only reader of a stale
       one is the next agent's `domparser_init`, which consults `g_ready` precisely to decide it need not
       run. */
    agent_state_class("domparser", &g_class, "HTML §8.5.1's DOMParser class");
    agent_state_flag("domparser", &g_ready, "HTML §8.5.1's declaration latch");
    agent_state_id("domparser", &g_id_ctor, "HTML §8.5.1's DOMParser constructor declaration");
    agent_state_id("domparser", &g_id_parse, "HTML §8.5.1's parseFromString declaration");
    realm_declare_intrinsic(domparser_install_proto);
}

/* THE INVERSE OF THE DECLARATION ABOVE, which did not exist. The prototype and the interface object are the
   REALMS' and go with their contexts; what is the AGENT's is the class this runtime registered and the three
   ints beside it. The class goes back to 0 because a class is registered in a RUNTIME — core/agent_state.h's
   one policy — and the latch goes with it, since a carried latch makes the next agent's `domparser_init`
   return before re-registering anything. */
void domparser_free(void)
{
    DCHECK(g_ready, "§8.5.1's DOMParser was released in an agent that never declared it");
    g_id_ctor = g_id_parse = -1;
    g_ready = 0;
    g_class = 0;
}

void domparser_install_proto(JSContext *ctx)
{
    JSValue proto, prev;

    DCHECK(g_ready, "a realm asked for DOMParser.prototype before the interface was declared");
    prev = JS_GetClassProto(ctx, g_class);
    DCHECK(JS_IsNull(prev), "domparser_install_proto ran twice in one realm");
    JS_FreeValue(ctx, prev);
    proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(proto), "DOMParser.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "DOMParser");
    idl_install_method(ctx, proto, "parseFromString", g_id_parse);
    JS_SetClassProto(ctx, g_class, proto);
}

void domparser_install(JSContext *ctx, JSValueConst global)
{
    JSValue proto = JS_GetClassProto(ctx, g_class), ctor;

    DCHECK(g_ready, "DOMParser was installed before domparser_init declared it");
    DCHECK(!JS_IsNull(proto), "DOMParser was installed in a realm that never ran its prototype install");
    ctor = idl_step_constructor(ctx, "DOMParser", g_id_ctor);
    JS_SetConstructor(ctx, ctor, proto);
    JS_FreeValue(ctx, proto);
    JS_SetPropertyStr(ctx, (JSValue)global, "DOMParser", ctor);
}
