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
 * THE XML ARM CRASHES AND NAMES WHAT TO BUILD, because no component here yet parses a DOCUMENT — core/xml/
 * holds the character, name, reference, markup, declaration, literal and namespace layers, §3.1's three tags,
 * §3's [39] element walk over §3.1's [43] content, and §2.1's [1] document over §2.8's [22] prolog — so what
 * stands between them and a Document is a TREE BUILDER and the report it fails into, which is the crash's own
 * list, in spec order. Lexbor ships no
 * `xml` module — `engine/build.mjs` states which release this tree pins and `ls source/lexbor` in the vendored
 * checkout is the whole of the check — so CLAUDE.md's bind-before-build order has nothing at the "existing
 * Lexbor module" rung and what is owed is a faithful spec port. THE RELEASE NUMBER THAT STOOD IN THIS SENTENCE
 * IS GONE ON PURPOSE: it was a version this build no longer uses, and a citation that stays true about the
 * standard while going wrong about this tree is precisely the stale `DFAIL` CLAUDE.md describes — the claim to
 * make is the one a reader can re-run, not the one that was measured once.
 * XMLHttpRequest §3.6.6 already stands at the same wall and says so in the same words
 * (core/xhr/xml_http_request.c, "set a document response" step 6) — checked, not assumed, before this DFAIL was
 * written to point at it. Answering the XML arm with an empty document, or with §8.5.1's `parsererror`
 * document, would be a claim about a parse that never happened: `parsererror` is what a WELL-FORMEDNESS ERROR
 * produces, and a build with no parser has not found one.
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
#include "core/dom/attr_list.h"
#include "core/dom/document.h"
#include "core/dom/node.h"
#include "core/html/domparser.h"
#include "core/html/html_parse.h"      /* the ONE place a Document is parsed — that header owns the token bytes */
#include "core/html/html_style_element.h"
#include "core/html/html_script.h"   /* §4.12.1.1's `force async`: the stamp every parser makes */
#include "core/html/media_element.h"
#include "core/html/html_image.h"
#include "core/html/trusted_types.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "core/dom/node_interface.h"   /* the ONE place a Document is made — see that header */

/* PER REALM — §3.7. The class is the AGENT's; the prototype lives in quickjs's per-context class-proto slot. */
static JSClassID g_class;
static int       g_ready;
static int       g_id_ctor = -1, g_id_parse = -1;

/* §8.5.1's `enum DOMParserSupportedType`, in the order the IDL lists it. The list IS the type: a `type` that is
   not one of these five is a TypeError thrown by the CONVERSION, before step 1, which is what
   DOMParser-parseFromString-html.html's "throws on an invalid enum value" asserts and what makes this file's
   own switch total. */
static const char *const DOM_PARSER_SUPPORTED_TYPE[] = {
    "text/html", "text/xml", "application/xml", "application/xhtml+xml", "image/svg+xml", NULL
};

/* THE OBJECT'S WHOLE STATE — see the head comment. */
typedef struct { JSValue document; } DomParser;

static void domparser_finalizer(JSRuntime *rt, JSValue val)
{
    DomParser *p = JS_GetOpaque(val, g_class);

    if (!p) return;
    JS_FreeValueRT(rt, p->document);
    free(p);
}

static void domparser_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    DomParser *p = JS_GetOpaque(val, g_class);

    if (p) JS_MarkValue(rt, p->document, mark_func);
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

/* WEB IDL §3.7.5's BRAND CHECK. `DOMParser.prototype.parseFromString` is reachable off the prototype with
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
    CHECK(html_parse_document(dom, DOM_PARSE_ROOT_PRIVATE, (const lxb_char_t *)html, len) == LXB_STATUS_OK,
          "DOMParser: the markup handed to parseFromString could not be parsed");
    /* §8.5.1 step 2: the URL is the relevant global's associated Document's, and the content type is `type` —
       which for this arm is "text/html", the string that makes a Document an HTML document. */
    doc = document_new(ctx, dom, url, "text/html");
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
       - §13.2.6.4.4's INERT script marking is NOT run either, and that is the spec from the other direction:
         "already started" is set for the FRAGMENT case, and this is a document parse. A `<script>` moved out of
         a DOMParser document into a live one therefore runs, exactly as it does in a browser. What IS run is
         §4.12.1.1's other parser stamp, `force async` — that one is unconditional ("set to false by the HTML
         parser … on script elements they insert") and says nothing about a scripting mode, so the `<script>`
         that gets moved out carries the same flag it would have carried in any other document. */
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
    return doc;
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
    if (!p) return JS_EXCEPTION;                 /* §3.7.5's brand check threw */

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
        JS_FreeCString(ctx, type);
        JS_FreeValue(ctx, compliant);
        /* STEP 3's OTHERWISE arm. Building the parsererror document here would be a claim about a parse that
           never ran: §8.5.1 produces one for an XML WELL-FORMEDNESS ERROR, which a build with no parser has not
           found. The release build throws for the same reason — a capability that is not supportable outside
           development fails rather than fabricating a Document the moat would then report on. */
        DFAIL("HTML §8.5.1 parseFromString reached its XML arm and this build has no XML DOCUMENT parser. "
              "Lexbor ships no xml module — `ls engine/lexbor/source/lexbor` is the whole of that check and it "
              "is the same answer upstream, so CLAUDE.md's bind-before-build order has nothing at the "
              "existing-module rung and what is owed is a faithful spec port. XMLHttpRequest §3.6.6 step 6 "
              "stands at the same wall in core/xhr/xml_http_request.c. BUILD ONE COMPONENT FOR BOTH: a "
              "namespace-aware XML parser producing a Lexbor tree and reporting XML and XML-namespace "
              "well-formedness errors. MOST OF IT IS ALREADY IN core/xml/ AND MUST NOT BE REBUILT — read those "
              "headers before writing a line. The LEAVES: xml_char.h is XML 1.0 5e §2.2's [2] Char, §2.3's [3] "
              "S and §2.11's end-of-line normalization as the reader every production reads through, plus the "
              "§4.3.3 encode §3.3.3 needs; xml_name.h is §2.3's [5] Name with Namespaces in XML's NCName and "
              "QName; xml_ref.h is §4.1's [66] CharRef and [68] EntityRef with §4.6's five predefined "
              "entities; xml_markup.h is §2.5's [15] Comment, §2.6's [16] PI and §2.7's [18] CDSect — the "
              "three constructs §2.4 names as the places a literal < or & may stand, so none of their content "
              "ever reaches xml_ref.h; xml_decl.h is §2.8's [23] XMLDecl with §2.9's [32] SDDecl and §4.3.1's "
              "[77] TextDecl; xml_literal.h is §2.3's [11] SystemLiteral, [12] PubidLiteral and [13] "
              "PubidChar; xml_ns.h is that standard's §6 scope stack with every §3 and §5 constraint as a "
              "returned error. AND THE FIRST GRAMMAR RULE IS BUILT TOO: xml_tag.h is §3.1's [40] STag, [42] "
              "ETag and [44] EmptyElemTag over [41] Attribute, [25] Eq and [10] AttValue, with §3.3.3 "
              "Attribute-Value Normalization and with [WFC: Unique Att Spec], [WFC: No < in Attribute Values] "
              "and [WFC: Entity Declared] decided — so DO NOT WRITE A SECOND TAG SCANNER. AND SO IS THE "
              "PRODUCTION THAT PUTS ONE TAG INSIDE ANOTHER: xml_element.h is §3's [39] element ::= "
              "EmptyElemTag | STag content ETag over §3.1's [43] content, the ELEMENT STACK, and it owns "
              "[WFC: Element Type Match] — the constraint xml_tag.c does not check because §3 writes it on "
              "[39] and it is about a PAIR of tags. It is a PULL walk over an explicit heap stack (§C-stack: "
              "[43] contains [39], so C recursion would put a page's own nesting depth on the C stack) and it "
              "reports [43]'s five alternatives plus the two boundaries of [39] as items — so DO NOT WRITE A "
              "SECOND CONTENT WALK EITHER. AND SO IS THE WHOLE OF WHAT AN ENTITY IS: xml_document.h is "
              "§2.1's [1] document ::= prolog element Misc* with §2.8's [22] prolog ::= XMLDecl? Misc* "
              "(doctypedecl Misc*)? and its [27] Misc ::= Comment | PI | S, delegating [39] whole, holding "
              "the [23] XMLDecl for a caller to ask by name, consuming the white space that becomes no node, "
              "and deciding §2.1's `there is exactly one element, called the root`. "
              "WHAT IS STILL OWED, IN SPEC ORDER: (1) THE DOM "
              "CONSTRUCTION: each [39] element becomes a Lexbor node, its attributes are expanded through "
              "xml_ns.h's scope (push at the STag, pop at the ETag) and Namespaces in XML 1.0 3e §6.3 "
              "Uniqueness of Attributes' expanded-name half is checked there, since xml_tag.c can only answer "
              "§3.1's literal-Name half. THAT is the component that must be a STEP MACHINE — a parse over "
              "attacker-length input is unbounded, and the walks beneath it are re-enterable at every "
              "construct because their whole state is the caller's POD reader plus stacks whose owner they "
              "assert. It drives xml_document.h's pull walk: one item per call, until xml_document_ended. (2) "
              "§8.5.1 step 3's parsererror element in the "
              "http://www.mozilla.org/newlayout/xml/parsererror.xml namespace, built from the error record — "
              "every component in core/xml/ already reports its sentence as a message, so nothing new has to "
              "be worded — xml_document_error_message and the layers its detail chains down to already word "
              "every sentence. (3) Route this arm to it, and route §3.6.6's arm to it too. "
              "THE DOCTYPE IS A SECURITY DECISION AND MUST CRASH RATHER THAN BE SKIPPED. Nothing here reads "
              "§2.8's [28] doctypedecl, which is why an [68] EntityRef outside §4.6's five is answered "
              "[WFC: Entity Declared] — in a document without any DTD that IS the standard's answer, and it is "
              "answered at TWO sites on purpose: xml_tag.c for §3.1's [10] AttValue and xml_element.c for "
              "§3.1's [43] content. Those two must stay two, because §4.4 Entity Type Table gives 'Reference "
              "in Content' and 'Reference in Attribute Value' different rows and §4.4.4 Forbidden's third "
              "bullet makes a reference to an EXTERNAL entity fatal in a value while content includes it — one "
              "site cannot answer for both the moment declarations exist. "
              "The moment [28] is read that stops being true, so whoever builds it owes §4.2's [70] EntityDecl "
              "WITH §3.1's [WFC: No External Entity References] and §4.4.4 Forbidden's third bullet in the "
              "same diff: an attribute value MUST NOT reference an external entity, and a parser that resolves "
              "one has put an XXE inside a security tool. Until then a [28] doctypedecl is an unbuilt "
              "capability, and the prolog walk is where it crashes: xml_document.c holds the `'<!DOCTYPE'` "
              "delimiter and the CHECK_FAIL beside it, so building [28] moves the peek and the crash together "
              "and cannot leave one behind. It is a CHECK and not a DCHECK because a release build with the "
              "crash compiled out would report the declaration as [22]'s `matches none of these constructs`, "
              "which is a plausible diagnosis of the wrong thing about a document that matches [22] exactly. "
              "This is also why xml_element.c reports a `<!DOCTYPE` standing in [43] "
              "content as a fatal error rather than reading one: §2.8 says the document type declaration MUST "
              "appear before the first element, so in content it is a document's mistake and not this build's "
              "missing capability");
        return JS_ThrowInternalError(ctx, "parseFromString: this build has no XML parser");
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
    idl_enum_values(DOM_PARSER_SUPPORTED_TYPE);
    g_ready = 1;
    realm_declare_intrinsic(domparser_install_proto);
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
    idl_install_method(ctx, proto, "parseFromString", 2, g_id_parse);
    JS_SetClassProto(ctx, g_class, proto);
}

void domparser_install(JSContext *ctx, JSValueConst global)
{
    JSValue proto = JS_GetClassProto(ctx, g_class), ctor;

    DCHECK(g_ready, "DOMParser was installed before domparser_init declared it");
    DCHECK(!JS_IsNull(proto), "DOMParser was installed in a realm that never ran its prototype install");
    ctor = idl_step_constructor(ctx, "DOMParser", 0, g_id_ctor);
    JS_SetConstructor(ctx, ctor, proto);
    JS_FreeValue(ctx, proto);
    JS_SetPropertyStr(ctx, (JSValue)global, "DOMParser", ctor);
}
