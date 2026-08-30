/* See core/html/xml_fragment.h. */
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/interfaces/document_fragment.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/dom/attr_list.h"
#include "core/dom/node_ns.h"
#include "core/html/html_script.h"
#include "core/html/xml_fragment.h"
#include "core/xml/xml_ns.h"
#include "core/xml/xml_parse.h"
#include "solver/dom_cow.h"

/* §14.4's THREE SUB-WALKS. See the header on why they are this component's phases rather than the driver's
   stages: the driver has ONE rest point here and §14.4 has three things to be resting in the middle of. */
enum { XF_PARSE = 0, XF_LIFT, XF_READY };

/* ---- §14.4 STEP 4's SYNTHETIC START TAG ------------------------------------------------------------------
 *
 * "Feed the parser just created the string corresponding to the start tag of context, declaring all the
 * namespace prefixes that are in scope on that element in the DOM, as well as declaring the default namespace
 * (if any) that is in scope on that element in the DOM."
 *
 * AND THE STANDARD DEFINES "IN SCOPE" RATHER THAN LEAVING IT TO THE READER, which is what makes this a
 * composition of core/dom/node_ns.h and not a walk of its own: "A namespace prefix is in scope if the DOM
 * lookupNamespaceURI() method on the element would return a non-null value for that prefix", and "the default
 * namespace is the namespace for which the DOM isDefaultNamespace() method on the element would return true".
 * Both members are DOM §4.4 "Interface Node"'s `locate a namespace` with a different argument, so the answer
 * to "is P in scope, and to what" is one call each and never a second climb written here — a second climb
 * would be a second answer to a question the standard states has one.
 *
 * WHAT THIS FILE STILL HAS TO DO IS ENUMERATE THE CANDIDATES, because §14.4's definition is a MEMBERSHIP TEST
 * and a start tag needs a LIST. The candidate prefixes are exactly the local names of the `xmlns:*` attributes
 * on the context element and its ancestors — a prefix nothing declared cannot pass the membership test — and
 * each candidate is then put to `node_locate_namespace`, whose answer is the NEAREST binding and whose null is
 * §14.4's "not in scope". So the ordering rule (an inner declaration shadows an outer one) is never restated:
 * the candidate list may hold a prefix twice over two elements and the membership call collapses it, and this
 * file's own de-duplication is only so the tag does not carry one prefix twice, which XML §3.1 "Start-Tags,
 * End-Tags, and Empty-Element Tags"'s [WFC: Unique Att Spec] would make ill-formed.
 *
 * NOT THE `xml` PREFIX, and its absence is a positive statement. Namespaces in XML §3 "Declaring Namespaces"
 * binds `xml` by definition and says it "MAY, but need not, be declared"; DOM §4.4's locate-a-namespace has
 * no arm for it, so
 * `lookupNamespaceURI("xml")` answers null unless some element actually declared it — and §14.4's definition
 * of in-scope is that member's answer. A declaration synthesised here would therefore be one the standard's
 * own test excludes, and it is unnecessary besides: core/xml/xml_ns.h holds the same binding by definition on
 * the parser's side. */

typedef struct { char *p; size_t len, cap; } XfBuf;

static void xf_buf_reserve(XfBuf *b, size_t more)
{
    size_t want = b->cap ? b->cap : 64;

    if (b->len + more <= b->cap)
        return;
    while (want < b->len + more) want *= 2;
    b->p = (char *)realloc(b->p, want);
    CHECK(b->p != NULL, "OOM assembling HTML §14.4 \"Parsing XML fragments\" step 4's start tag — a dropped "
                        "byte is a namespace declaration the context element has and the parse would not, "
                        "which is a namespace well-formedness error reported against the page's own markup");
    b->cap = want;
}

static void xf_put(XfBuf *b, const char *s, size_t n)
{
    if (n == 0) return;
    xf_buf_reserve(b, n);
    memcpy(b->p + b->len, s, n);
    b->len += n;
}

static void xf_put_z(XfBuf *b, const char *s) { xf_put(b, s, strlen(s)); }

/* AN ATTRIBUTE VALUE, ESCAPED — XML §2.3 "Common Syntactic Constructs"'s [10] AttValue admits neither `<` nor
   the delimiter, and §2.4 "Character Data and Markup" makes a raw `&` the start of a reference. The three
   replacements use only §4.6 "Predefined Entities"' five, which is what makes them legal in an entity with no
   DOCTYPE: §14.4 step 4 says "No DOCTYPE is passed to the parser,
   and therefore no external subset is referenced, and therefore no entities will be recognized", and the five
   predefined ones are not external-subset entities. A namespace name is a URI and normally contains none of
   the three; it is an ATTRIBUTE VALUE the page wrote, so what it contains is not this file's to assume. */
static void xf_put_att_value(XfBuf *b, const char *s, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        switch (s[i]) {
        case '&':  xf_put_z(b, "&amp;");  break;
        case '<':  xf_put_z(b, "&lt;");   break;
        case '"':  xf_put_z(b, "&quot;"); break;
        default:   xf_put(b, s + i, 1);   break;
        }
    }
}

/* THE CANDIDATE PREFIXES, de-duplicated — see the block comment above for why enumeration and membership are
   two different questions and only one of them is this file's. The list holds BORROWED slices of attribute
   local names, which are owned by the document and valid for the length of this call: nothing between the walk
   and the tag assembly runs the page's code. */
typedef struct { const char **name; size_t *len; size_t n, cap; } XfPrefixes;

static bool xf_prefix_seen(const XfPrefixes *ps, const char *s, size_t n)
{
    size_t i;

    for (i = 0; i < ps->n; i++)
        if (ps->len[i] == n && memcmp(ps->name[i], s, n) == 0) return true;
    return false;
}

static void xf_prefix_add(XfPrefixes *ps, const char *s, size_t n)
{
    if (n == 0 || xf_prefix_seen(ps, s, n))
        return;
    if (ps->n == ps->cap) {
        size_t want = ps->cap ? ps->cap * 2 : 8;
        ps->name = (const char **)realloc(ps->name, want * sizeof(*ps->name));
        ps->len  = (size_t *)realloc(ps->len, want * sizeof(*ps->len));
        CHECK(ps->name != NULL && ps->len != NULL,
              "OOM collecting the namespace prefixes HTML §14.4 step 4 declares on its synthetic start tag");
        ps->cap = want;
    }
    ps->name[ps->n] = s;
    ps->len[ps->n]  = n;
    ps->n++;
}

static void xf_collect_prefixes(lxb_dom_element_t *context, XfPrefixes *ps)
{
    lxb_dom_node_t *n;

    for (n = lxb_dom_interface_node(context); n != NULL; n = n->parent) {
        lxb_dom_attr_t *a;

        if (n->type != LXB_DOM_NODE_TYPE_ELEMENT)
            continue;
        for (a = lxb_dom_element_first_attribute(lxb_dom_interface_element(n));
             a != NULL; a = lxb_dom_element_next_attribute(a)) {
            const lxb_char_t *ans, *apx, *al;
            size_t anslen = 0, apxlen = 0, allen = 0;

            ans = dom_attr_ns(a, &anslen);
            if (ans == NULL || anslen != strlen(XML_NS_XMLNS_NAMESPACE)
                || memcmp(ans, XML_NS_XMLNS_NAMESPACE, anslen) != 0)
                continue;
            /* `xmlns:P` DECLARES P, and the declared prefix is the attribute's LOCAL name — the default
               declaration `xmlns="…"` has no prefix and is step 4's other half, asked directly below. */
            apx = dom_attr_prefix(a, &apxlen);
            if (apx == NULL || apxlen != 5 || memcmp(apx, "xmlns", 5) != 0)
                continue;
            al = lxb_dom_attr_local_name(a, &allen);
            if (al != NULL) xf_prefix_add(ps, (const char *)al, allen);
        }
    }
}

/* §14.4 STEPS 4-6 AS ONE ENTITY. The three feeds are three calls in the standard because a parser is a stream;
   core/xml/xml_parse.h's `begin` takes the whole document entity and copies it, and §14.2 "Parsing XML
   documents" states the equivalence itself — "Certain algorithms in this specification spoon-feed the parser
   characters one at a time. In such cases, the XML parser must act as it would have if faced with a single
   string consisting of the concatenation of all those characters." The SUSPENSION is not lost by assembling
   them: what suspends is the parse's walk over the bytes, one item per step, which is core/xml/xml_parse.h's
   own shape and is why this component is a machine at all. */
static void xf_build_entity(lxb_dom_element_t *context, const char *input, size_t len, XfBuf *out)
{
    const lxb_char_t *qname;
    const char       *ns;
    size_t            qlen = 0, nslen = 0, i;
    XfPrefixes        ps;

    memset(&ps, 0, sizeof ps);
    qname = lxb_dom_element_qualified_name(context, &qlen);
    DCHECK(qname != NULL && qlen != 0,
           "HTML §14.4 step 4 feeds \"the string corresponding to the start tag of context\" and the context "
           "element has no qualified name — every element has one, so this is an element this engine built "
           "without a name rather than a document that lacks one");

    xf_put_z(out, "<");
    xf_put(out, (const char *)qname, qlen);

    /* THE DEFAULT NAMESPACE, "(if any)" — `isDefaultNamespace()` is DOM §4.4's locate-a-namespace with a NULL
       prefix, and its null is the "if any". A context element in no namespace under no default declaration
       therefore declares nothing, which is correct and not an omission: the synthetic element is then in no
       namespace, exactly as the context element is. */
    ns = node_locate_namespace(lxb_dom_interface_node(context), NULL, 0, &nslen);
    if (ns != NULL) {
        xf_put_z(out, " xmlns=\"");
        xf_put_att_value(out, ns, nslen);
        xf_put_z(out, "\"");
    }

    xf_collect_prefixes(context, &ps);
    for (i = 0; i < ps.n; i++) {
        size_t vlen = 0;
        const char *v = node_locate_namespace(lxb_dom_interface_node(context), ps.name[i], ps.len[i], &vlen);
        /* §14.4's OWN MEMBERSHIP TEST, and the null it answers is the whole of "in scope". A candidate that
           some ancestor declared and a nearer one UNDECLARED answers null here and is not written — which is
           the case a nearest-declaration walk written in this file would have had to remember and this one
           cannot get wrong, because it never asks the question. */
        if (v == NULL)
            continue;
        xf_put_z(out, " xmlns:");
        xf_put(out, ps.name[i], ps.len[i]);
        xf_put_z(out, "=\"");
        xf_put_att_value(out, v, vlen);
        xf_put_z(out, "\"");
    }
    xf_put_z(out, ">");

    /* STEP 5 — `input`, VERBATIM. It is the page's markup and is not escaped: the whole point of the algorithm
       is that the parser reads it as markup. */
    xf_put(out, input, len);

    /* STEP 6 — the end tag. */
    xf_put_z(out, "</");
    xf_put(out, (const char *)qname, qlen);
    xf_put_z(out, ">");

    free(ps.name);
    free(ps.len);
}

/* ---- THE MACHINE ------------------------------------------------------------------------------------------ */

void xml_fragment_begin(JSContext *ctx, XmlFragmentParse *s, lxb_dom_element_t *context,
                        const char *input, size_t len)
{
    lxb_dom_document_fragment_t *root;
    lxb_dom_document_t          *doc;
    XfBuf                        entity;

    (void)ctx;
    /* §14.4 STEP 2 — "Assert: context is non-null." Step 1 is "Let context be target if target is an Element;
       otherwise target's host", and a DocumentFragment with no host is what would make it null. */
    DCHECK(context != NULL,
           "HTML §14.4 \"Parsing XML fragments\" step 2 is \"Assert: context is non-null\" — step 1 takes the "
           "target itself when it is an Element and its HOST otherwise, so a null here is a DocumentFragment "
           "target with no host, which no member may hand to this algorithm");
    memset(s, 0, sizeof *s);
    memset(&entity, 0, sizeof entity);
    s->context = context;

    doc = lxb_dom_interface_node(context)->owner_document;
    DCHECK(doc != NULL, "HTML §14.4's context element has no node document — step 10 creates the fragment in "
                        "it, so there is no document for the result to belong to");

    /* THE PRIVATE ROOT §14.4's [1] `document` IS PARSED INTO. See the header: the parse's nodes are CREATED in
       the context's document (which is what makes the placement's move need no DOM §4.5 adopt) while [1]'s
       children go here, which is core/xml/xml_tree.h's own two-argument separation used by the one caller it
       says exists for. No creation record is taken for this fragment and none may be: it is a private ROOT,
       owned by this component and destroyed by whoever takes it — a per-node claim over it would be the
       second owner solver/dom_cow.h's ownership convention exists to refuse. */
    root = lxb_dom_document_fragment_interface_create(doc);
    CHECK(root != NULL, "OOM creating the DocumentFragment HTML §14.4 parses into — handing back a fragment "
                        "the page cannot tell from an empty parse is not an option");
    s->root = lxb_dom_interface_node(root);

    xf_build_entity(context, input, len, &entity);
    /* PRIVATE, in the strongest sense solver/dom_cow.h admits: this statement made the root and nothing
       between the two runs the page's code, so no fork can have produced a sibling that holds it. */
    s->parse = xml_parse_begin(doc, s->root, DOM_PARSE_ROOT_PRIVATE, entity.p, entity.len);
    free(entity.p);   /* core/xml/xml_parse.h: the build takes its own copy of the entity */
    s->phase = XF_PARSE;
}

int xml_fragment_step(JSContext *ctx, XmlFragmentParse *s)
{
    switch (s->phase) {
    case XF_PARSE: {
        XmlParseReport report;

        DCHECK(s->parse != NULL, "HTML §14.4's parse phase was stepped with no parse open");
        if (!xml_parse_ended(s->parse)) {
            xml_parse_step(s->parse);
            return JS_STEP_YIELD;
        }
        if (!xml_parse_finish(s->parse, &report)) {
            s->parse = NULL;   /* finish destroys the handle on every path */
            /* §14.4 STEP 7 — "If there is an XML well-formedness or XML namespace well-formedness error, then
               throw a "SyntaxError" DOMException." The message is the LAYER'S OWN SENTENCE and its position,
               which is what core/xml/xml_parse.h assembles: a "SyntaxError" that names no rule tells a page
               only that something was wrong with markup it wrote. The position is in the SYNTHETIC entity and
               says so, because a column the page cannot map onto its own string is worse than none. */
            JS_ThrowDOMException(ctx, "SyntaxError",
                                 "%s (at line %zu, column %zu of the context element's start tag, the markup "
                                 "and its end tag, as HTML §14.4 step 4-6 feed them to the parser)",
                                 xml_parse_report_message(&report), report.line, report.column);
            return JS_STEP_ABRUPT;
        }
        s->parse = NULL;
        /* §14.4 STEP 8 — "If the document element of the resulting Document has any sibling nodes, then throw
           a "SyntaxError" DOMException." The document element is the entity's one root element, which XML
           §2.1 "Well-Formed XML Documents"'s [1] `document` guarantees a successful parse produced exactly
           one of; its siblings would be §2.1's `Misc` — a comment or a processing instruction outside it. */
        s->docel = s->root->first_child;
        DCHECK(s->docel != NULL && s->docel->type == LXB_DOM_NODE_TYPE_ELEMENT,
               "a successful XML parse produced no document element — XML §2.1 \"Well-Formed XML "
               "Documents\"'s [1] `document` is "
               "`prolog element Misc*`, so an entity that matched it to the last byte has exactly one root "
               "element and a parse that reported no error cannot have built none");
        if (s->docel->next != NULL) {
            JS_ThrowDOMException(ctx, "SyntaxError",
                                 "HTML §14.4 \"Parsing XML fragments\" step 8: the document element of the "
                                 "parsed fragment has sibling nodes");
            return JS_STEP_ABRUPT;
        }
        s->phase = XF_LIFT;
        return JS_STEP_YIELD;
    }

    case XF_LIFT:
        /* §14.4 STEPS 9-11, one child per step — "Let newChildren be the resulting Document node's document
           element's children, in tree order", "let fragment be the result of creating a document fragment
           given context's node document", "for each node of newChildren, in tree order: append node to
           fragment". CHILDREN IS DOM'S TREE CONCEPT (the standard links the word), so this takes every child
           node and not only the element ones — a Text node, a Comment and a CDATASection are newChildren.
           THE FRAGMENT STEP 10 CREATES IS THE ROOT THIS COMPONENT ALREADY HAS, which is why the children move
           UP into it rather than into a second one: it was created in the context's node document, which is
           step 10's own argument, and the document element is a child of it that step 12 does not return. Both
           ends are this component's own private tree, so the move captures nothing and adopts nothing — the
           nodes were created in the context's document to begin with. */
        DCHECK(s->docel != NULL, "HTML §14.4's lift phase was reached with no document element to lift from");
        if (s->docel->first_child != NULL) {
            dom_cow_move_private(s->root, s->root, s->root, s->docel->first_child);
            return JS_STEP_YIELD;
        }
        /* The emptied document element is not part of the result — step 9 takes its CHILDREN — and nothing can
           ever reach it again, so it goes here rather than being left for the taker to find. */
        dom_cow_discard_private(s->root, s->docel);
        s->docel = NULL;
        /* HTML §14.2 "Parsing XML documents": "When an XML parser with XML scripting support enabled creates a
           script element, it must have its parser document set and its force async set to false. If the parser
           was created as part of the XML fragment parsing algorithm, then the element's already started must
           be set to true." §14.4 names no scripting mode and §14.2's default is ENABLED, so this parser is one
           — and UNCONDITIONALLY inert, which is the opposite of the answer §13.4 gives the same member: a
           `createContextualFragment` in an HTML document passes §13.2.4.5's Fragment scripting mode precisely
           so its scripts run, and in an XML document the identical call produces scripts §4.12.1 step 1 stops.
           There is no mode to consult here, so none is taken.
           AT THE PARSE BOUNDARY and before any node is placed, for core/html/html_script.h's own reason: this
           parse runs no page code, so nothing can look at a `script` element between the start tag that
           created it and this statement. */
        html_script_parsed(ctx, s->root, /*inert*/ true);
        s->phase = XF_READY;
        return 0;

    default:
        DFAIL("HTML §14.4's machine was stepped in a phase it does not have — XF_READY is answered by "
              "xml_fragment_take and the machine is finished");
        return JS_STEP_ABRUPT;
    }
}

lxb_dom_node_t *xml_fragment_take(XmlFragmentParse *s)
{
    lxb_dom_node_t *root = s->root;

    DCHECK(s->phase == XF_READY,
           "HTML §14.4's result was taken before the algorithm finished — steps 9-11 move the document "
           "element's children into the root, so a root taken before that holds the document element and not "
           "the fragment's children");
    DCHECK(root != NULL, "HTML §14.4's result was taken twice — the first take gave the claim up");
    s->root = NULL;   /* the caller owns it now; two owners of one private tree is two destroys of it */
    return root;
}

void xml_fragment_release(XmlFragmentParse *s)
{
    if (s->parse) {
        /* core/xml/xml_parse.h's own entry for "nobody is going to ask for a report", which is exactly a flow
           dropped between two steps. The tree the parse built is left standing and the destroy below is what
           takes it. */
        xml_parse_abort(s->parse);
        s->parse = NULL;
    }
    if (s->root) {
        dom_cow_destroy_private(s->root, /*with_children*/ true);
        s->root = NULL;
        s->docel = NULL;   /* it hung under the root and went with it */
    }
    DCHECK(s->docel == NULL,
           "HTML §14.4's machine was released holding a document element with no root — the element is a child "
           "of the root and cannot outlive it, so this is a claim that survived the tree it named");
}

const char *xml_fragment_unforkable(const XmlFragmentParse *s)
{
    if (s->parse != NULL)
        return "an XML fragment parse cannot be forked mid-parse — this machine holds a core/xml/xml_parse.h "
               "handle, which owns the entity bytes, the §2.11 \"End-of-Line Handling\" character reader's "
               "position, the [1] `document` walk's own state and Namespaces in XML §6.1 \"Namespace "
               "Scoping\"'s scope stack, and there is no operation that "
               "copies one. BUILD IT: every one of those is engine C with named state (unlike lexbor's "
               "tokenizer, whose 182 state functions are raw code pointers — see fragment_parse_unforkable), "
               "so the copy is a deep clone of XmlParse plus a node->node map to re-point the open element and "
               "the insertion point at the sibling's copy of the tree, which is the same map the tree-builder "
               "half already needs. The same clone is what makes this machine PARK, because every field it "
               "would copy has a name a later build can resume from";
    if (s->root != NULL)
        return "an XML fragment parse cannot be forked between its parse and its placement — this machine OWNS "
               "the private tree §14.4 steps 9-11 are moving children within, and no JSStepVisit operation "
               "declares a private DOM TREE, so the sibling arm would share one tree with the original: two "
               "arms placing the same nodes and two releases destroying them. It is the SAME missing operation "
               "core/html/fragment_parser.c's first clause names (a `v->tree` whose clone deep-copies a "
               "subtree through a node->node map), and both clauses delete together";
    return NULL;
}
