/* HTML §14.4 "Parsing XML fragments" — THE XML FRAGMENT PARSING ALGORITHM, as one step machine. See
 * core/html/xml_fragment.c.
 *
 * WHO REACHES IT, AND WHY IT IS NOT A BRANCH INSIDE THE HTML ONE. HTML §8.5.4 "The innerHTML property"'s
 * fragment parsing algorithm steps step 2 is "If target's node document is an XML document, then return the
 * result of invoking the XML fragment parsing algorithm given target and markup" — one dispatch, asked at the
 * one point every member that parses markup converges on, so `el.innerHTML =` in an XHTML document, an
 * `insertAdjacentHTML`, a `createContextualFragment` and the two `setHTML` members all arrive here without any
 * of them asking. What differs from §13.4 "Parsing HTML fragments" is the PARSER and the seams around it;
 * where the parsed nodes GO is identical, which is why core/html/fragment_parser.c keeps its placement and
 * this component hands it a private root of exactly the same shape.
 *
 * WHAT IT PRODUCES: a private root whose children are §14.4 step 9's `newChildren` — "the resulting Document
 * node's document element's children, in tree order". CHILDREN IS THE TREE CONCEPT AND NOT THE IDL ATTRIBUTE:
 * the standard links that word to DOM's `child` concept, so a Text node, a Comment and a CDATASection are all
 * newChildren, and reading it as `Element.children` would silently drop every non-element the markup
 * contained — a fragment that looks right for `<a/><b/>` and loses the text of `x<a/>y`.
 *
 * THE NODES BELONG TO THE CONTEXT'S DOCUMENT AND NOT TO A DOCUMENT OF THEIR OWN, which is what lets the
 * placement move them with no DOM §4.5 adopt — the same property core/html/fragment_parser.c states about
 * §13.4's temporary document. §14.4 step 3 says "Create a new XML parser" and step 10 creates the fragment in
 * `context`'s node document, so the standard's own reading has an adopt in step 11's append. This engine takes
 * core/xml/xml_tree.h's own separation instead — that entry takes the Document a node is CREATED in and the
 * parent [1] `document`'s children are appended to as TWO arguments, "so that the one caller who ever needs
 * otherwise does not have to reach past this" — and this is that caller: the nodes are created in the context
 * element's document and the parse's [1] children go into a private DocumentFragment. The resulting tree is
 * the one step 11 would have produced, node document included, with no adopt walk to suspend inside.
 *
 * ITS SCRIPTS ARE INERT, AND THAT IS THE OPPOSITE OF THE HTML ANSWER FOR THE SAME MEMBER. §14.4 names no
 * scripting mode, and §14.2 "Parsing XML documents" says XML parsers are invoked with XML scripting support
 * ENABLED "except where otherwise specified" — so this parser has it enabled, and §14.2's very next sentence
 * is the one that decides the outcome: "If the parser was created as part of the XML fragment parsing
 * algorithm, then the element's already started must be set to true." §4.12.1 "The script element"'s prepare
 * step 1 returns for an already-started element, so every script this algorithm produces is dead. §13.4's
 * Fragment scripting mode exists precisely so that `createContextualFragment` produces LIVE scripts; in an XML
 * document the identical call produces dead ones, and the mode the member passed is not consulted at all.
 *
 * WHAT IT DOES NOT RUN, and each is a different standard's answer rather than one blanket "XML is different" —
 * the same three core/html/domparser.c's XML arm names for the same reasons: the parse-boundary ATTRIBUTE
 * NAMESPACE correction (an XML attribute's namespace is Namespaces in XML §6.2 "Namespace Defaulting"'s
 * expansion, computed from the bindings in scope and already written, so the correction would rewrite names
 * the standard just decided),
 * §13.2.6.4.4's declarative-shadow conversion, and the §4.8.11.2 media walk — the last two being triggers
 * stated over an HTML parser's stack of open elements. */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_XML_FRAGMENT_H
#define ENGINE_HOST_BROWSER_CORE_HTML_XML_FRAGMENT_H

#include <stdbool.h>
#include <stddef.h>

#include <lexbor/dom/dom.h>

#include "quickjs.h"
#include "quickjs-step.h"
#include "core/xml/xml_parse.h"

/* WHERE §14.4 RESTS BETWEEN TWO STEPS. `phase` is this component's own and not a stage of the machine that
   drives it: core/html/fragment_parser.h's X-list is the DRIVER's rest points, and §14.4's three sub-walks
   (the parse's items, the lift of step 9's children, the finish) are one rest point of that machine and three
   of this one — the same arrangement core/loader/xml_document.h states for §7.5.3's load. */
typedef struct {
    uint8_t            phase;
    lxb_dom_element_t *context;   /* §14.4 step 1's `context`, resolved by the caller — see xml_fragment_begin */
    /* The private DocumentFragment §14.4's [1] `document` is parsed into, and the root this component hands
       back. OWNED until `xml_fragment_take` gives it up; deep-destroyed by xml_fragment_release otherwise. */
    lxb_dom_node_t    *root;
    /* §14.4 step 9's DOCUMENT ELEMENT — the synthetic `context` start tag step 4 fed the parser, whose children
       steps 9-11 move. NULL until the parse has finished and step 8 has been answered. */
    lxb_dom_node_t    *docel;
    XmlParse          *parse;     /* owned while open; core/xml/xml_parse.h's pull handle */
} XmlFragmentParse;

/* §14.4 STEPS 3-5's SETUP, run in one call because none of it can suspend: step 3 creates the parser, step 4
 * feeds it the synthetic start tag, step 5 feeds it `input` and step 6 the end tag — and core/xml/xml_parse.h's
 * `begin` takes the whole entity and COPIES it, so the assembled bytes do not outlive this call.
 *
 * `context` IS §14.4 STEP 1'S ANSWER AND NOT ITS ARGUMENT. Step 1 is "Let context be target if target is an
 * Element; otherwise target's host", over a target that is "an Element or DocumentFragment node" — so a
 * ShadowRoot reaches §14.4 and resolves to its host. That resolution happens where the member picks its parse
 * context, which is the same place §13.4's context comes from, so this entry receives the Element and step 2's
 * "Assert: context is non-null" is the DCHECK below.
 *
 * `input` is §14.4 step 5's string and is BORROWED for the length of this call only. */
void xml_fragment_begin(JSContext *ctx, XmlFragmentParse *s, lxb_dom_element_t *context,
                        const char *input, size_t len);

/* ONE STEP — one item of core/xml/xml_parse.h's walk, or one child of step 9's `newChildren` moved. Returns
   JS_STEP_YIELD while there is more, 0 when the root is ready to be taken, or JS_STEP_ABRUPT having thrown
   §14.4 step 7's or step 8's "SyntaxError" DOMException. */
int xml_fragment_step(JSContext *ctx, XmlFragmentParse *s);

/* THE ROOT, once `xml_fragment_step` has answered 0 — its children are step 9's `newChildren` in tree order.
   The caller becomes the owner, which is why this NULLs the field rather than leaving a second claim behind:
   two owners of one private tree is two destroys of it. */
lxb_dom_node_t *xml_fragment_take(XmlFragmentParse *s);

/* WHAT THIS RECORD OWNS, declared once — the private tree §14.4 [1] parses into, with `docel` as the cursor
   standing in it. The machine that holds this record names this in its own `visit`, so the fork copies both
   through the ONE list and the teardown destroys both through it. There is no second half: `release` is only
   what a declaration cannot name, which here is the XML parse handle.
   ITS TREE IS THE ONE A PRIVATE-TREE COPY CAN ANSWER FOR IN FULL, and that is a fact about §14.4 rather than a
   convenience: this component runs neither §13.2.6.4.4's declarative-shadow conversion nor §4.8.11.2's media
   walk (the banner above says why for each), so no node of it is a shadow host and none is a media element —
   the two cases solver/dom_cow.c's copy crashes on by name. What it DOES run is §14.2's already-started stamp,
   and that is carried by DOM §4.4's cloning steps, which that copy performs per node. */
void xml_fragment_visit(JSContext *ctx, XmlFragmentParse *s, JSStepVisit *v);

/* THE ABANDONED PARSE — a flow dropped between two steps. The XML handle is aborted (core/xml/xml_parse.h's
   own entry for "nobody is going to ask for a report"). It does NOT destroy the tree: that is declared, so the
   one teardown frees it after this returns, and freeing it here as well is the second list the teardown's own
   fingerprint bracket measures. Idempotent, because a machine's release runs on the throw path too. */
void xml_fragment_release(XmlFragmentParse *s);

/* WHY A FLOW STANDING HERE CANNOT FORK, or NULL when it can — the same question core/html/fragment_parser.h's
   `fragment_parse_unforkable` answers for the machine as a whole, asked of this half so that the driver does
   not have to know what this component owns. */
const char *xml_fragment_unforkable(const XmlFragmentParse *s);

#endif
