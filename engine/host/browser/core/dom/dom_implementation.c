/* THE DOMImplementation INTERFACE — DOM §4.5.1, and the three algorithms that BUILD A DOCUMENT.
 *
 * WHY THIS WAS THE BLOCKING PIECE and not three small members: every one of `createDocumentType`,
 * `createDocument` and `createHTMLDocument` returns a SECOND DOCUMENT, and this engine's components held one
 * per realm — `document.c`'s record hung off the JSContext opaque, so "which document" and "which realm" were
 * the same question and there was no way to ask the first one twice. The record is per DOCUMENT now (see
 * document.c), which is what makes these three members writable at all.
 *
 * WHAT A SECOND DOCUMENT IS, EXACTLY: a document with NO BROWSING CONTEXT. It has no navigable, no Window, no
 * WindowProxy and no scripts; §3.1.1's `location` is null on it, which the corpus checks. It is NOT a second
 * realm and NOT a second instance — HTML's similar-origin window agent is one heap, and the nodes this makes
 * are ordinary objects of the realm that asked, with this realm's prototypes. `world_doc_*` and `navigable.c`
 * are about NAVIGABLES, and a document created here has none, so none of that machinery is involved.
 *
 * THE ASSOCIATED DOCUMENT is the object's own state: §4.5.1's algorithms are all "given THIS's associated
 * document", and a page holds `document.implementation` and calls it later. It is the Lexbor document rather
 * than a held JSValue because the wrapper is already immortal in the identity map and a held reference would be
 * a second owner of it; document.c DETACHES it when the document's record goes, so a member reached through a
 * stale object crashes at the read instead of walking freed memory. */
#include <string.h>

#include <lexbor/dom/dom.h>
#include <lexbor/html/html.h>

#include "check.h"
#include "quickjs.h"
#include "solver/dom_cow.h"
#include "core/dom/document.h"
#include "core/dom/dom_implementation.h"
#include "core/dom/node.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "core/dom/node_interface.h"   /* the ONE place a Document is made — see that header */
#include "core/html/html_parse.h"      /* …and the ONE place one is parsed, which owns the tokens it produces */

/* PER REALM — §3.7. The class is the agent's; the prototype lives in its per-context slot. */
static JSClassID g_impl_class;
static int       g_ready;

static const IdlArgType IDL_3STR[3] = { IDL_DOMSTRING, IDL_DOMSTRING, IDL_DOMSTRING };
static int g_id_doctype = -1, g_id_document = -1, g_id_html_document = -1, g_id_has_feature = -1;

/* THE ASSOCIATED DOCUMENT of the receiver — the whole of this interface's own state. */
static lxb_dom_document_t *impl_doc(JSContext *ctx, JSValueConst this_val)
{
    lxb_dom_document_t *d;

    /* THE TWO FAILURES ARE DIFFERENT THINGS AND THE CLASS ID IS WHAT TELLS THEM APART. A receiver that is not a
       DOMImplementation is WEB IDL §3.7.5's brand check — a TypeError thrown at the call, which the corpus asks
       for deliberately with `DOMImplementation.prototype.createDocument.call({})`, and never an assert. A
       receiver that IS one whose associated document is gone is an ENGINE invariant, because the record detaches
       it at teardown precisely so a stale object crashes rather than walking freed memory. Reading the opaque
       alone answers NULL to both and would have made the first of them an abort. */
    if (JS_GetClassID(this_val) != g_impl_class) {
        JS_ThrowTypeError(ctx, "this is not a DOMImplementation");
        return NULL;
    }
    d = JS_GetOpaque(this_val, g_impl_class);
    DCHECK(d != NULL, "a DOMImplementation member ran on an object whose associated document is gone — the "
                      "document's record detaches it at teardown precisely so this is a crash rather than a "
                      "read of freed memory");
    return d;
}

/* §4.5.1 `createDocumentType(name, publicId, systemId)`.
 *
 * 1. Validate `name`. — 2. Return a new doctype with these three and its node document set to THIS'S ASSOCIATED
 * DOCUMENT. Lexbor implements exactly this algorithm (its own comment cites the same section), including the
 * XML `Name` production the validation is over — which is why the two names the corpus expects to be rejected
 * (`edi:>` and `edi:a `) are rejected and the fifty-odd it expects to be accepted are accepted, rather than a
 * hand-rolled character class that would get some other row of that table wrong. */
static JSValue js_impl_create_doctype(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                      int magic)
{
    lxb_dom_document_t *doc = impl_doc(ctx, this_val);
    const char *name = NULL, *pub = NULL, *sys = NULL;
    lxb_dom_document_type_t *dt;
    lxb_dom_exception_code_t code = LXB_DOM_EXCEPTION_OK;
    JSValue r;

    (void)magic; (void)argc;
    if (!doc) return JS_EXCEPTION;   /* §3.7.5's brand check threw */
    name = JS_ToCString(ctx, argv[0]);
    pub  = name ? JS_ToCString(ctx, argv[1]) : NULL;
    sys  = pub  ? JS_ToCString(ctx, argv[2]) : NULL;
    if (!sys) {
        if (pub) JS_FreeCString(ctx, pub);
        if (name) JS_FreeCString(ctx, name);
        return JS_EXCEPTION;
    }
    dt = lxb_dom_document_type_create(doc, (const lxb_char_t *)name, strlen(name),
                                      (const lxb_char_t *)pub, strlen(pub),
                                      (const lxb_char_t *)sys, strlen(sys), &code);
    JS_FreeCString(ctx, name); JS_FreeCString(ctx, pub); JS_FreeCString(ctx, sys);
    if (!dt) {
        /* §4.5.1 step 1's only failure, and the one the corpus asserts by name. */
        DCHECK(code == LXB_DOM_EXCEPTION_INVALID_CHARACTER_ERR,
               "createDocumentType failed for a reason its algorithm does not have — step 1 is a name "
               "validation and there is no other way for it to fail");
        return JS_ThrowDOMException(ctx, "InvalidCharacterError", "not a valid document type name");
    }
    /* A node this flow made, in a document the BASELINE may own: the delta owns it and destroys it. */
    dom_cow_note_created(lxb_dom_interface_node(dt));
    r = node_wrap(ctx, lxb_dom_interface_node(dt));
    return r;
}

/* §4.5.1 `createHTMLDocument(title)` — the algorithm, step by step, over a REAL PARSE of the markup its steps
 * 3-5 and 7 spell out.
 *
 * 1. `doc` ← a new document that is an HTML document. — 2. content type "text/html". — 3. append a doctype
 * named "html". — 4. append an `html` element. — 5. append a `head` to it. — 6. if `title` was GIVEN, append a
 * `title` element to the head and a Text node with the title's data to that. — 7. append a `body` to the html
 * element. — 8. `doc`'s origin is the associated document's. — 9. return `doc`.
 *
 * STEPS 3-5 AND 7 ARE THE PARSE and not four hand-built nodes, because the tree they describe is exactly what
 * the HTML parser produces for that markup — and building it by hand is a second implementation of tree
 * construction that would have to agree with the first about `head`, `body` and the doctype's own fields. Step
 * 6 is the only part that depends on the argument, so it is the only part built node by node.
 *
 * THE TITLE IS NOT INTERPOLATED INTO THE MARKUP. It is the page's string — `createHTMLDocument("<script>")` is
 * an ordinary call — so putting it in the source text would parse the page's data as markup, which is the
 * injection this whole engine exists to find rather than to commit.
 *
 * "IF TITLE IS GIVEN" IS `undefined` MEANING ABSENT, which §3.6 says for an optional argument with no
 * default and which the declaration states with idl_optional_from: `createHTMLDocument()` and
 * `createHTMLDocument(undefined)` both leave the head EMPTY, while `createHTMLDocument("")` puts an empty
 * `<title>` in it. */
static JSValue js_impl_create_html_document(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                            int magic)
{
    /* §4.5.1 steps 3-5 and 7, as the markup that produces exactly them. */
    static const char SKELETON[] = "<!DOCTYPE html><html><head></head><body></body></html>";
    lxb_html_document_t *dom;
    JSValue doc;

    (void)magic;
    /* §3.7.5's brand check. The associated document is not read past it — §4.5.1 step 8 gives the new document
       that one's ORIGIN, and an origin-keyed agent cluster has exactly one, which is this realm's. */
    if (!impl_doc(ctx, this_val)) return JS_EXCEPTION;
    dom = dom_document_create();
    CHECK(dom != NULL, "createHTMLDocument: OOM building a second Document");
    /* FLOW-PRIVATE for the reason every §4.5.1 creation is: this operation made the Document. */
    CHECK(html_parse_document(dom, DOM_PARSE_ROOT_PRIVATE, HTML_SCRIPTING_DISABLED,
                              (const lxb_char_t *)SKELETON, sizeof SKELETON - 1) == LXB_STATUS_OK,
          "createHTMLDocument: the skeleton its own steps 3-5 and 7 describe did not parse");
    /* §4.5.1 createHTMLDocument step 2 STATES BOTH FACTS IN ONE SENTENCE — "Set doc's type to `html` and
       content type to `text/html`" — which is why they travel as one value; the address is §4.5's default
       `about:blank`. The record has to exist before any node of this tree is wrapped — node_wrap resolves a
       node's prototype through its document's realm. */
    doc = document_new(ctx, dom, "about:blank", document_kind(/*is_xml*/false, "text/html"));

    if (argc >= 1 && !JS_IsUndefined(argv[0])) {   /* step 6, and only when the title was GIVEN */
        lxb_dom_node_t *head = NULL, *n, *root = node_of(doc)->first_child;
        lxb_dom_element_t *title;
        lxb_dom_text_t *text;
        const char *s;
        size_t len = 0;

        for (; root; root = root->next)
            if (root->type == LXB_DOM_NODE_TYPE_ELEMENT) break;
        DCHECK(root != NULL, "createHTMLDocument's skeleton parsed without a document element");
        for (n = root->first_child; n; n = n->next) {
            size_t qn = 0;
            const lxb_char_t *q;
            if (n->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
            q = lxb_dom_element_qualified_name(lxb_dom_interface_element(n), &qn);
            if (q && qn == 4 && !memcmp(q, "head", 4)) { head = n; break; }
        }
        DCHECK(head != NULL, "createHTMLDocument's skeleton parsed without a head element");
        s = JS_ToCStringLen(ctx, &len, argv[0]);
        /* The DOCUMENT is the delta's (or the realm's) from document_new, so it is owned either way; the
           WRAPPER is this function's own reference and has to go back. */
        if (!s) { JS_FreeValue(ctx, doc); return JS_EXCEPTION; }
        title = lxb_dom_document_create_element(lxb_dom_interface_document(dom),
                                                (const lxb_char_t *)"title", 5, NULL);
        CHECK(title != NULL, "createHTMLDocument: OOM building step 6's title element");
        text = lxb_dom_document_create_text_node(lxb_dom_interface_document(dom), (const lxb_char_t *)s, len);
        CHECK(text != NULL, "createHTMLDocument: OOM building step 6's Text node");
        JS_FreeCString(ctx, s);
        dom_cow_note_created(lxb_dom_interface_node(title));
        dom_cow_note_created(lxb_dom_interface_node(text));
        dom_cow_append_child(lxb_dom_interface_node(title), lxb_dom_interface_node(text));
        dom_cow_append_child(head, lxb_dom_interface_node(title));
    }
    return doc;
}

/* §4.5.1 `createDocument(namespace, qualifiedName, doctype)`.
 *
 * 1. `document` ← a new XMLDocument. — 2. `element` ← null. — 3. if `qualifiedName` is not "", `element` ← the
 * INTERNAL createElementNS steps over `document`. — 4. if `doctype` is non-null, append it. — 5. if `element`
 * is non-null, append it. — 6. `document`'s origin is the associated document's. — 7. content type from the
 * namespace: `image/svg+xml` for SVG, `application/xhtml+xml` for HTML, `application/xml` otherwise. —
 * 8. return `document`.
 *
 * STEP 3 IS Document's OWN createElementNS, called on the NEW document, which is what "internal createElementNS
 * steps, GIVEN DOCUMENT" says — one implementation of validate-and-extract and of element creation, rather
 * than a second one here that could disagree about a prefix.
 *
 * `[LegacyNullToEmptyString] DOMString qualifiedName` means a `null` argument is the EMPTY STRING and therefore
 * step 3's "not the empty string" is false — `createDocument(null, null)` is a document with no document
 * element, which is exactly the shape dom/common.js builds. */
static JSValue js_impl_create_document(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                       int magic)
{
    lxb_html_document_t *dom;
    JSValue doc, element = JS_UNDEFINED;
    lxb_dom_node_t *doctype = argc > 2 ? node_of(argv[2]) : NULL;
    const char *ns = NULL, *type;

    (void)magic;
    if (!impl_doc(ctx, this_val)) return JS_EXCEPTION;   /* §3.7.5's brand check; see createHTMLDocument */
    if (argc > 0 && !JS_IsNull(argv[0]) && !JS_IsUndefined(argv[0])) {
        ns = JS_ToCString(ctx, argv[0]);
        if (!ns) return JS_EXCEPTION;
    }
    /* Step 7, decided before anything is built because it is a property of the arguments and not of the tree. */
    type = ns && !strcmp(ns, "http://www.w3.org/2000/svg")   ? "image/svg+xml"
         : ns && !strcmp(ns, "http://www.w3.org/1999/xhtml") ? "application/xhtml+xml"
                                                             : "application/xml";
    if (ns) JS_FreeCString(ctx, ns);

    dom = dom_document_create();
    CHECK(dom != NULL, "createDocument: OOM building a second Document");
    /* Step 1's document has NO tree at all — not even a document element — which is why this is a create and
       not a parse. §4.5's own steps 4 and 5 are what put nodes in it, if the arguments name any. */
    /* AND IT IS AN XML DOCUMENT — step 1 is "creating a document that implements XMLDocument", and §4.5's
       default type `xml` is what that leaves in place (only createHTMLDocument beside it sets `html`). So
       `createDocument(…).createCDATASection(…)` succeeds where an HTML document's must throw, which is the
       one observable this pair decides here. */
    doc = document_new(ctx, dom, "about:blank", document_kind(/*is_xml*/true, type));

    if (argc > 1 && !JS_IsNull(argv[1]) && !JS_IsUndefined(argv[1])) {
        const char *qname = JS_ToCString(ctx, argv[1]);
        if (!qname) { JS_FreeValue(ctx, doc); return JS_EXCEPTION; }
        if (*qname) {                                    /* step 3's "is not the empty string" */
            JSValueConst a[2];
            a[0] = argc > 0 ? argv[0] : JS_NULL;
            a[1] = argv[1];
            element = document_create_element_ns(ctx, doc, 2, a);
            if (JS_IsException(element)) { JS_FreeCString(ctx, qname); JS_FreeValue(ctx, doc); return element; }
        }
        JS_FreeCString(ctx, qname);
    }
    if (doctype) {                                       /* step 4 */
        DCHECK(doctype->type == LXB_DOM_NODE_TYPE_DOCUMENT_TYPE,
               "createDocument's third argument is a `DocumentType?` and the declaration brands it, so anything "
               "else cannot reach here");
        dom_cow_append_child(node_of(doc), doctype);
    }
    if (!JS_IsUndefined(element)) {                      /* step 5 */
        dom_cow_append_child(node_of(doc), node_of(element));
        JS_FreeValue(ctx, element);
    }
    return doc;
}

/* §4.5.1 `hasFeature()` — "return true". The spec says so in those words, and its own note explains why: it is
   kept alive only because pages test it and every implementation answers true. */
static JSValue js_impl_has_feature(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    (void)ctx; (void)this_val; (void)argc; (void)argv; (void)magic;
    return JS_TRUE;
}

JSValue dom_implementation_new(JSContext *ctx, JSValueConst doc_obj)
{
    lxb_dom_node_t *n = node_of(doc_obj);
    JSValue proto, obj;

    DCHECK(g_ready, "a DOMImplementation was built before its interface was declared");
    DCHECK(n != NULL && n->type == LXB_DOM_NODE_TYPE_DOCUMENT,
           "a DOMImplementation was built for something that is not a document");
    proto = JS_GetClassProto(ctx, g_impl_class);
    DCHECK(!JS_IsNull(proto), "DOMImplementation.prototype was asked for in a realm that never ran its install");
    obj = JS_NewObjectProtoClass(ctx, proto, g_impl_class);
    JS_FreeValue(ctx, proto);
    CHECK(!JS_IsException(obj), "a DOMImplementation object could not be allocated");
    JS_SetOpaque(obj, lxb_dom_interface_document(n));
    return obj;
}

void dom_implementation_detach(JSContext *ctx, JSValueConst impl)
{
    (void)ctx;
    if (JS_IsObject(impl))
        JS_SetOpaque((JSValue)impl, NULL);
}

void dom_implementation_init(JSContext *ctx)
{
    JSClassDef d = { "DOMImplementation" };

    DCHECK(!g_ready, "dom_implementation_init ran twice — the interface is declared once per AGENT");
    JS_NewClassID(JS_GetRuntime(ctx), &g_impl_class);
    JS_NewClass(JS_GetRuntime(ctx), g_impl_class, &d);
    g_id_doctype = idl_method_id(ctx, IDL_3STR, 3, js_impl_create_doctype, 0);
    {
        /* §4.5.1: `(DOMString? namespace, [LegacyNullToEmptyString] DOMString qualifiedName,
           optional DocumentType? doctype = null)`. The third is an interface type, branded against the node
           class — anything that is not a node is a TypeError before step 1. */
        static const IdlArgType CREATE_DOC[3] = { IDL_DOMSTRING_NULLABLE, IDL_DOMSTRING_NULLABLE, IDL_INTERFACE };
        g_id_document = idl_method_id(ctx, CREATE_DOC, 3, js_impl_create_document, 0);
        idl_iface_brand(node_class_id());
        idl_optional_from(2);
    }
    {
        static const IdlArgType TITLE[1] = { IDL_DOMSTRING };
        g_id_html_document = idl_method_id(ctx, TITLE, 1, js_impl_create_html_document, 0);
        /* §3.6: an `undefined` for an optional argument with NO DEFAULT means the argument is ABSENT, which
           is the difference between an empty `<title>` and no head child at all. */
        idl_optional_from(0);
    }
    g_id_has_feature = idl_method_id(ctx, NULL, 0, js_impl_has_feature, 0);
    g_ready = 1;
    realm_declare_intrinsic(dom_implementation_install_proto);
}

void dom_implementation_install_proto(JSContext *ctx)
{
    JSValue proto, prev;

    prev = JS_GetClassProto(ctx, g_impl_class);
    DCHECK(JS_IsNull(prev), "dom_implementation_install_proto ran twice in one realm");
    JS_FreeValue(ctx, prev);
    proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(proto), "DOMImplementation.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "DOMImplementation");
    idl_install_method(ctx, proto, "createDocumentType", 3, g_id_doctype);
    idl_install_method(ctx, proto, "createDocument", 2, g_id_document);
    idl_install_method(ctx, proto, "createHTMLDocument", 0, g_id_html_document);
    idl_install_method(ctx, proto, "hasFeature", 0, g_id_has_feature);
    JS_SetClassProto(ctx, g_impl_class, proto);
}

void dom_implementation_install(JSContext *ctx, JSValueConst global)
{
    JSValue proto;

    DCHECK(g_ready, "the DOMImplementation interface was installed before its prototype was built");
    proto = JS_GetClassProto(ctx, g_impl_class);
    DCHECK(!JS_IsNull(proto), "DOMImplementation.prototype was asked for in a realm that never ran its install");
    /* §4.5.1 declares no constructor: the interface object exists to be what `instanceof` names. */
    JS_SetPropertyStr(ctx, (JSValue)global, "DOMImplementation",
                      idl_interface_object(ctx, "DOMImplementation", proto));
    JS_FreeValue(ctx, proto);
}

/* RELEASED BY ITS DECLARER — §4.5.1 is declared from document_init, so document_agent_free gives it back.
   IT TAKES NO RUNTIME: the prototype is each REALM's and every DOMImplementation object is a GC object of the
   realm that minted it, detached by its document's record. What a C static holds for the AGENT is the class,
   the four pool entries and the latch, and all six go — a release that frees and then keeps its handles is
   core/agent_state.h's fetch_free, whose four atom ids read as valid to the next agent and answered `<null>`. */
void dom_implementation_free(void)
{
    DCHECK(g_ready, "§4.5.1's DOMImplementation was released in an agent that never declared it");
    g_ready = 0;
    g_impl_class = 0;
    g_id_doctype = g_id_document = g_id_html_document = g_id_has_feature = -1;
}
