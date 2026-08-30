/* See core/xml/xml_tree.h. */
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/interfaces/cdata_section.h>
#include <lexbor/dom/interfaces/comment.h>
#include <lexbor/dom/interfaces/processing_instruction.h>
#include <lexbor/dom/interfaces/text.h>

#include "core/dom/attr_list.h"
#include "core/dom/element.h"
#include "core/dom/node.h"
#include "core/xml/xml_char.h"
#include "core/xml/xml_element.h"
#include "core/xml/xml_name.h"
#include "core/xml/xml_tree.h"
#include "check.h"

struct XmlTreeBuild {
    lxb_dom_document_t *doc;
    lxb_dom_node_t     *root_parent;   /* where [1] `document`'s own children go */
    lxb_dom_node_t     *current;       /* the innermost open element, or `root_parent` at depth 0 */
    lxb_dom_node_t     *closed;        /* the element the LAST step's end tag closed, or NULL — see the header */
    DomParseRootKind    kind;
    XmlCharReader       r;
    XmlDocumentWalk    *walk;
    XmlNsScope         *scope;
    char               *text;          /* the §2.4/§4.1 run being coalesced — see the header */
    size_t              text_len, text_cap;
    /* HTML §14.2's SECOND TREES, innermost last — the template contents the insertion point is inside. See the
       header on why this is not the second stack of open elements it refuses. */
    lxb_dom_node_t    **tpl;
    size_t              tpl_n, tpl_cap;
    bool                failed;
};

/* THE TREE THIS BUILD IS CURRENTLY APPENDING INTO — `root_parent`, or the innermost `<template>`'s template
   contents, which HTML §14.2 makes a root of its own with no parent link back down. It is what the placement
   below DECLARES to solver/dom_cow.h, whose private-tree family checks the declaration against the tree the
   parent is actually in ("THE DECLARED ROOT IS THE IMMEDIATE TREE and not the whole of what the caller is
   building"), which is the same re-declaration core/dom/node.c's §4.4 clone walk makes when it descends into
   one. */
static lxb_dom_node_t *xml_tree_root(const XmlTreeBuild *b)
{
    return b->tpl_n ? b->tpl[b->tpl_n - 1] : b->root_parent;
}

/* See the header — the one point `kind` is asked, and the reason it is one point rather than seven. */
void xml_tree_place_created(lxb_dom_node_t *root, DomParseRootKind kind,
                            lxb_dom_node_t *parent, lxb_dom_node_t *node)
{
    lxb_dom_node_t *contents;

    DCHECK(root != NULL && parent != NULL && node != NULL,
           "an XML parse placed a node with no tree, no parent or no node — every construct this walk builds "
           "names all three, so a NULL is a caller that is not this walk");
    /* HTML §14.2 "Parsing XML documents": "When an XML parser would append a node to a `template` element, it
       must instead append it to the `template` element's template contents (a DocumentFragment node)." The
       spec calls it a willful violation of XML and says why — "XML is not formally extensible in the manner
       that is needed for template processing" — so there is no XML-side rule to reconcile it with, and this is
       the one place an XML parse appends, which is why the sentence is discharged here and not at seven sites.
       THE NAMESPACE CONDITION IS NOT A TEST WRITTEN HERE and must not become one: §14.2's `template` is HTML's
       element, and DOM §4.5 "Interface Document" says "The element interface for any name and namespace is
       Element, unless stated otherwise" — so `template` in the SVG or MathML namespace is a plain Element with
       no template contents at all, and core/dom/node.h's `node_template_content` answers NULL for it because
       it asks the HTML-namespace question. A namespace compare spelled out here would be a second copy of that
       rule, one edit from disagreeing with the interface the element actually has. */
    contents = node_template_content(parent);
    if (contents != NULL) {
        DCHECK(root == contents,
               "HTML §14.2 redirected a node into a <template>'s template contents while the parse declared "
               "some OTHER tree as the one it is building — solver/dom_cow.h's private-tree family checks the "
               "declaration against the tree the parent is in, and a template's contents are a root with no "
               "parent, so the caller's insertion point and its declared root have drifted apart");
        /* §14.2's OTHER sentence, which composes with the one above: "When an XML parser creates a Node
           object, its node document must be set to the node document of the node into which the newly created
           node is to be inserted." It is asserted rather than performed because it is currently a no-op: this
           build creates every node in the parse's own Document, and lexbor's template constructor stamps the
           contents fragment with the document holding the ELEMENT.
           NAMED RESIDUAL. NOT COVERED: the write itself. THE NEXT DIFF that makes HTML §4.12.3's "establish
           the template contents" run at element CREATION — giving the fragment the appropriate template
           contents owner document, which core/dom/node.c's §4.5 adopt step 3.4 is otherwise the only writer of
           — owes this site §14.2's node-document write through that one writer. HOW ITS ABSENCE WOULD SHOW:
           this DCHECK fires the instant the fragment stops naming the parse's Document, because a node created
           in one document would then be appended into another. */
        DCHECK(node->owner_document == contents->owner_document,
               "HTML §14.2 requires a node an XML parser creates to have the node document of the node it is "
               "inserted into, and this <template>'s contents no longer name the document this parse creates "
               "its nodes in — route the created node through DOM §4.5 adopt's one node-document writer");
        parent = contents;
    }
    switch (kind) {
    case DOM_PARSE_ROOT_PRIVATE:
        /* No creation record: the ROOT owns this tree until a placement moves a node out of it through
           dom_cow_take_private, which is the one seam that makes a per-node owner. */
        dom_cow_insert_private(root, parent, node);
        return;
    case DOM_PARSE_ROOT_SHARED:
        /* Recorded BEFORE the insert, over a node that is in no tree — which is where an ownership claim is
           exact, and why solver/dom_cow.h's §13.2.6 writer takes its claim at the node factory rather than at
           the insert that could run more than once for one node. */
        dom_cow_note_created(node);
        node_insert_at(parent, node, NULL);
        return;
    }
    /* NOT A `default:` ARM — the switch is exhaustive over DomParseRootKind, so a third disposition makes this
       not compile rather than silently take one of the two answers it is not. */
    DFAIL("a DomParseRootKind outside the enum reached the XML tree builder's placement");
}

const char *xml_tree_error_message(XmlTreeError err)
{
    switch (err) {
    case XML_TREE_OK:
        return "the construct was built into the tree";
    case XML_TREE_ERR_DOCUMENT:
        return "XML 1.0 5e §2.1's [1] document reported a violation — ask core/xml/xml_document.h, whose "
               "sentence it is";
    case XML_TREE_ERR_QNAME:
        return "Namespaces in XML 1.0 3e §4's [7] QName: a name in this document is not a qualified name — "
               "every element type and attribute name in a namespace-well-formed document MUST match [7]";
    case XML_TREE_ERR_NAMESPACE:
        return "Namespaces in XML 1.0 3e reported a namespace constraint violation — ask core/xml/xml_ns.h, "
               "whose sentence it is";
    case XML_TREE_ERR_ATTRIBUTES_UNIQUE:
        return "Namespaces in XML 1.0 3e §6.3 Namespace constraint: Attributes Unique — \"no tag may contain "
               "two attributes which: have identical names, or have qualified names with the same local part "
               "and with prefixes which have been bound to namespace names that are identical\"";
    }
    /* NOT A `default:` ARM — the switch is exhaustive over the enum, so adding a value makes this not compile
       rather than fall into a generic sentence that names no rule. */
    DFAIL("an XmlTreeError outside the enum reached its message — a value this component never returns");
    return "";
}

/* ENTER a `<template>`'s template contents — HTML §14.2, called with the fragment the element's own interface
   holds, so this records where the placement is ALREADY going rather than deciding it. Grows geometrically;
   nothing is bounded, because the depth is the document's own template nesting. */
static void xml_tree_template_push(XmlTreeBuild *b, lxb_dom_node_t *contents)
{
    if (b->tpl_n == b->tpl_cap) {
        size_t want = b->tpl_cap ? b->tpl_cap * 2 : 4;
        lxb_dom_node_t **grown = (lxb_dom_node_t **)realloc(b->tpl, want * sizeof(*grown));

        CHECK(grown != NULL, "OOM recording a <template>'s template contents for HTML §14.2 — without the "
                             "entry this parse would declare the wrong tree to the per-flow delta, which is a "
                             "node placed into a document with no record of who frees it");
        b->tpl = grown;
        b->tpl_cap = want;
    }
    b->tpl[b->tpl_n++] = contents;
}

/* A NUL-TERMINATED COPY OF A BORROWED SLICE, because core/dom/attr_list.h's writer takes three NUL-terminated
   names while every name this walk reports is a borrowed slice of the entity. Never returns NULL for a
   non-NULL slice; NULL in is NULL out, which is how §1.4's null namespace and null prefix pass through. */
static char *xml_tree_dup(const char *s, size_t len)
{
    char *out;
    if (s == NULL) return NULL;
    out = (char *)malloc(len + 1);
    CHECK(out != NULL, "OOM copying an XML name for the DOM — a dropped name is an element or attribute the "
                       "document contains and the tree would not");
    memcpy(out, s, len);
    out[len] = '\0';
    return out;
}

/* APPEND TO THE CHARACTER RUN BEING COALESCED. Grows geometrically; the run is released when it becomes a Text
   node or when the build is destroyed, so there is exactly ONE free site per allocation. */
static void xml_tree_text_append(XmlTreeBuild *b, const char *s, size_t len)
{
    if (len == 0) return;
    if (b->text_len + len > b->text_cap) {
        size_t want = b->text_cap ? b->text_cap * 2 : 64;
        char  *grown;
        while (want < b->text_len + len) want *= 2;
        grown = (char *)realloc(b->text, want);
        CHECK(grown != NULL, "OOM growing an XML character run — dropping it would give a Text node fewer "
                             "characters than the document contains, with nothing to say so");
        b->text = grown;
        b->text_cap = want;
    }
    memcpy(b->text + b->text_len, s, len);
    b->text_len += len;
}

/* THE NODE THE RUN BECOMES, if there is a run. Called before every item that is not part of one and once at
   the end — see the header on why §2.7's CDSect ends a run rather than joining it. */
static void xml_tree_flush_text(XmlTreeBuild *b)
{
    lxb_dom_text_t *t;
    if (b->text_len == 0) return;
    t = lxb_dom_document_create_text_node(b->doc, (const lxb_char_t *)b->text, b->text_len);
    CHECK(t != NULL, "OOM creating a Text node for an XML character run");
    xml_tree_place_created(xml_tree_root(b), b->kind, b->current, lxb_dom_interface_node(t));
    b->text_len = 0;
}

/* §2.11 End-of-Line Handling OVER A BORROWED SLICE, into the run. core/xml/xml_char.h owns the rule and states
   why a second spelling has to exist at all: a content run is BYTES the reader never read, and §2.11 stands
   between those bytes and the characters the standard says the production matched. Sizing first and writing
   second is that component's own two-call contract. */
static void xml_tree_text_append_normalized(XmlTreeBuild *b, const char *s, size_t len)
{
    size_t need, wrote;
    if (len == 0) return;
    need = xml_char_normalized_len(s, len);
    DCHECK(need <= len, "XML §2.11 End-of-Line Handling made a run LONGER than the bytes it normalized — the "
                        "rule only ever collapses #xD#xA to #xA and rewrites #xD, so the result is never "
                        "larger than its input and a longer one is a bug in that component, not in the "
                        "document");
    /* §2.11's writer MUST NOT overlap its source, and the run's buffer may be REALLOCATED by the append —
       so the rule is applied into a scratch of its own exact size and the result goes through the one
       appender, which is where the single growth policy lives. */
    {
        char *scratch = (char *)malloc(need);
        CHECK(scratch != NULL, "OOM normalizing an XML character run for §2.11 End-of-Line Handling");
        wrote = xml_char_normalize(s, len, scratch);
        DCHECK(wrote == need, "XML §2.11's sizing call and its writing call disagreed about the same slice — "
                              "core/xml/xml_char.h states that _len answers what _normalize will write, and "
                              "two answers to one question is that contract broken");
        xml_tree_text_append(b, scratch, wrote);
        free(scratch);
    }
}

XmlTreeBuild *xml_tree_build_create(lxb_dom_document_t *doc, lxb_dom_node_t *parent, DomParseRootKind kind,
                                    const char *text, size_t len)
{
    XmlTreeBuild *b;
    DCHECK(doc != NULL && parent != NULL,
           "an XML tree build was opened without both halves of what it builds into — a Document the nodes "
           "belong to and the node [1] document's children are appended to");
    DCHECK(text != NULL,
           "an XML tree build was opened over a NULL pointer — an EMPTY entity is a zero LENGTH over a real "
           "pointer, and XML §2.1's [1] document has an answer about one (there is no element)");
    b = (XmlTreeBuild *)calloc(1, sizeof(*b));
    CHECK(b != NULL, "OOM opening an XML tree build");
    b->doc = doc;
    b->root_parent = parent;
    b->current = parent;
    b->kind = kind;
    xml_char_reader_init(&b->r, text, len);
    b->walk = xml_document_walk_create();
    CHECK(b->walk != NULL, "OOM creating the XML §2.1 [1] document walk");
    b->scope = xml_ns_scope_create();
    CHECK(b->scope != NULL, "OOM creating the Namespaces in XML §6 scope stack");
    return b;
}

void xml_tree_build_destroy(XmlTreeBuild *b)
{
    if (b == NULL) return;
    DCHECK(!b->failed && xml_document_ended(b->walk),
           "an XML tree build that did not match [1] `document` to the last byte of the entity with no error "
           "at any layer was DESTROYED rather than abandoned — see core/xml/xml_tree.h on the two teardowns. "
           "A build whose own step reported a Namespaces in XML §6 constraint is one of these: the walks under "
           "it answered that start-tag successfully and are still standing inside [39] element");
    DCHECK(b->text_len == 0,
           "a completed XML parse left a character run that never became a Text node — §2.1's [1] `document` "
           "is `prolog element Misc*` and §2.8's [27] `Misc` has no [14] CharData, so every run this build can "
           "open is inside an element and is flushed by that element's end-tag item. A survivor means a run "
           "was opened somewhere [1] does not admit one, and the characters would be silently missing from "
           "the tree");
    DCHECK(b->tpl_n == 0,
           "a completed XML parse is still inside a `<template>`'s template contents — every push at a start "
           "tag is popped at that element's end tag, and §2.1's [1] `document` cannot end with [39] `element` "
           "unmatched, so a survivor is HTML §14.2's redirect still in force over a tree nobody is appending "
           "to and a declared root that no longer names the document");
    xml_document_walk_destroy(b->walk);
    xml_ns_scope_destroy(b->scope);
    free(b->text);
    free(b->tpl);
    free(b);
}

void xml_tree_build_abandon(XmlTreeBuild *b)
{
    if (b == NULL) return;
    /* NEITHER RESIDUE IS ASSERTED HERE AND BOTH ARE EXPECTED: an unflushed run is a [14] CharData whose
       element never closed, and the open element and namespace scopes below are what stopping mid-[39]
       leaves. `text` is freed exactly as it is above — the run is this build's own buffer and no node names
       it, so there is nothing an abandoned build leaks that a finished one does not. HTML §14.2's record of the
       template contents the insertion point was inside is a third such residue — stopping mid-[39] inside a
       `<template>` leaves entries on it — and it is freed here for the same reason: it names fragments the
       ROOT owns, never nodes of its own. */
    xml_document_walk_abandon(b->walk);
    xml_ns_scope_abandon(b->scope);
    free(b->text);
    free(b->tpl);
    free(b);
}

bool xml_tree_build_ended(const XmlTreeBuild *b)
{
    DCHECK(b != NULL, "xml_tree_build_ended was asked of no build");
    return b->failed || xml_document_ended(b->walk);
}

size_t xml_tree_build_line(const XmlTreeBuild *b)
{
    DCHECK(b != NULL, "xml_tree_build_line was asked of no build");
    return b->r.line;
}

size_t xml_tree_build_column(const XmlTreeBuild *b)
{
    DCHECK(b != NULL, "xml_tree_build_column was asked of no build");
    return b->r.column;
}

XmlCharError xml_tree_build_character_error(const XmlTreeBuild *b)
{
    DCHECK(b != NULL, "xml_tree_build_character_error was asked of no build");
    return b->r.fatal;
}

lxb_dom_node_t *xml_tree_build_closed_element(const XmlTreeBuild *b)
{
    DCHECK(b != NULL, "xml_tree_build_closed_element was asked of no build");
    DCHECK(b->closed == NULL || b->closed->type == LXB_DOM_NODE_TYPE_ELEMENT,
           "an XML end-tag item named a closed node that is not an element — this is taken from the insertion "
           "point, which only a start-tag item can move onto a node, so a non-element here means some other "
           "construct wrote the open element and §3's [39] `element` boundary is not what this reports");
    return b->closed;
}

/* ONE EXPANDED NAME, held only long enough to decide §6.3 over a single tag's attribute list. The slices are
   borrowed from the entity and from the scope stack, both of which outlive the step that reads them. */
typedef struct { const char *ns; size_t ns_len; const char *local; size_t local_len; } XmlTreeExpanded;

/* Namespaces in XML §6.3's "Attributes Unique", stated over EXPANDED names: "This constraint is equivalent to
   requiring that no element have two attributes with the same expanded name." §3.1's [WFC: Unique Att Spec]
   has already decided the literal-Name half in core/xml/xml_tag.c, which is the only half a scanner can see;
   this is the half that needs the bindings in force, which is why the standard writes it in Namespaces and why
   core/xml/xml_ns.h names it as owed here rather than deciding it over one name at a time. */
static bool xml_tree_expanded_equal(const XmlTreeExpanded *a, const XmlTreeExpanded *c)
{
    if (a->local_len != c->local_len || memcmp(a->local, c->local, a->local_len) != 0) return false;
    if ((a->ns == NULL) != (c->ns == NULL)) return false;
    if (a->ns == NULL) return true;   /* §6.2: two unprefixed attribute names, both with no namespace name */
    return a->ns_len == c->ns_len && memcmp(a->ns, c->ns, a->ns_len) == 0;
}

/* §6 APPLIED TO ONE START-TAG: push the scope, record its declarations, expand its element type, build the
   element, then expand and write its attributes with §6.3 decided over the results. The order is the
   standard's: §6.1 puts a declaration in scope "from the beginning of the start-tag in which it appears", so
   every `xmlns` on this tag binds before this tag's OWN name is expanded — which is what makes
   `<edi:price xmlns:edi='…'/>` legal. */
static XmlTreeError xml_tree_start_element(XmlTreeBuild *b, const XmlTag *tag, XmlTreeDetail *detail)
{
    XmlQName         qn;
    XmlExpandedName  ex;
    XmlNsError       nserr;
    lxb_dom_element_t *el;
    XmlTreeExpanded *seen;
    char *ns_z, *prefix_z, *local_z;
    size_t i, j;

    xml_ns_push(b->scope);

    /* PASS ONE: every [1] NSAttName on this tag binds before anything is expanded. */
    for (i = 0; i < tag->att_n; i++) {
        if (!xml_name_parse_qname(tag->atts[i].name, tag->atts[i].name_len, &qn)) return XML_TREE_ERR_QNAME;
        if (xml_ns_att_kind(&qn) == XML_NS_ATT_NONE) continue;
        nserr = xml_ns_declare(b->scope, &qn, tag->atts[i].value, tag->atts[i].value_len);
        if (nserr != XML_NS_OK) { detail->ns = nserr; return XML_TREE_ERR_NAMESPACE; }
    }

    /* THE ELEMENT TYPE. §6.2: a default declaration DOES apply to an unprefixed element name, which is why the
       kind is named rather than inferred — passing the other one is a spec bug no test of this one can see. */
    if (!xml_name_parse_qname(tag->name, tag->name_len, &qn)) return XML_TREE_ERR_QNAME;
    nserr = xml_ns_expand(b->scope, &qn, XML_NS_NAME_ELEMENT, &ex);
    if (nserr != XML_NS_OK) { detail->ns = nserr; return XML_TREE_ERR_NAMESPACE; }
    el = element_create_ns(b->doc, ex.ns, ex.ns_len, ex.local, ex.local_len, ex.prefix, ex.prefix_len);
    DCHECK(el != NULL, "core/dom/element.h's element_create_ns states it never returns NULL — an allocation "
                       "failure there is fatal — so a NULL here is that contract broken and not an OOM this "
                       "site may report");
    xml_tree_place_created(xml_tree_root(b), b->kind, b->current, lxb_dom_interface_node(el));
    b->current = lxb_dom_interface_node(el);
    /* HTML §14.2: from here until this element's end tag, everything this parse appends goes into the
       element's TEMPLATE CONTENTS and not under the element — so the tree the placement declares changes with
       the insertion point, and both move at exactly this one line. `node_template_content` is NULL for every
       element that is not an HTML-namespace `<template>`, which is the whole of the namespace condition. */
    {
        lxb_dom_node_t *contents = node_template_content(b->current);
        if (contents != NULL) xml_tree_template_push(b, contents);
    }

    if (tag->att_n == 0) return XML_TREE_OK;
    seen = (XmlTreeExpanded *)calloc(tag->att_n, sizeof(*seen));
    CHECK(seen != NULL, "OOM deciding Namespaces in XML §6.3 Attributes Unique over a start-tag's attributes");

    /* PASS TWO: expand, decide §6.3, write. */
    for (i = 0; i < tag->att_n; i++) {
        XmlNsAttKind akind;
        if (!xml_name_parse_qname(tag->atts[i].name, tag->atts[i].name_len, &qn)) {
            free(seen);
            return XML_TREE_ERR_QNAME;
        }
        akind = xml_ns_att_kind(&qn);
        if (akind == XML_NS_ATT_NONE) {
            nserr = xml_ns_expand(b->scope, &qn, XML_NS_NAME_ATTRIBUTE, &ex);
            if (nserr != XML_NS_OK) { free(seen); detail->ns = nserr; return XML_TREE_ERR_NAMESPACE; }
        } else {
            /* THE ONE PLACE THE DOM AND Namespaces in XML DISAGREE, AND core/xml/xml_ns.h SENDS IT HERE.
               §6.2's "the namespace name for an unprefixed attribute name always has no value" has no
               exception for `xmlns`, so that is what xml_ns_expand answers — while DOM §1.4 "Namespaces"'
               validate-and-extract step 10 makes the name `xmlns` REQUIRE the XMLNS namespace on the Attr it
               constructs. That is a rule about a NODE, so it is applied where the Attr is built and is not
               smuggled into the expansion, where it would make that function disagree with the section it
               implements. `xmlns` is (XMLNS, no prefix, "xmlns"); `xmlns:p` is (XMLNS, "xmlns", "p") — the
               declared prefix is the LOCAL part, which is the same fact xml_ns_declare takes the whole QName
               to avoid making the caller extract twice. */
            ex.ns = XML_NS_XMLNS_NAMESPACE;
            ex.ns_len = strlen(XML_NS_XMLNS_NAMESPACE);
            if (akind == XML_NS_ATT_DEFAULT) {
                ex.prefix = NULL; ex.prefix_len = 0;
                ex.local = qn.local; ex.local_len = qn.local_len;
            } else {
                ex.prefix = qn.prefix; ex.prefix_len = qn.prefix_len;
                ex.local = qn.local;   ex.local_len = qn.local_len;
            }
        }
        seen[i].ns = ex.ns; seen[i].ns_len = ex.ns_len;
        seen[i].local = ex.local; seen[i].local_len = ex.local_len;
        for (j = 0; j < i; j++) {
            if (xml_tree_expanded_equal(&seen[i], &seen[j])) { free(seen); return XML_TREE_ERR_ATTRIBUTES_UNIQUE; }
        }
        ns_z     = xml_tree_dup(ex.ns, ex.ns_len);
        prefix_z = xml_tree_dup(ex.prefix, ex.prefix_len);
        local_z  = xml_tree_dup(ex.local, ex.local_len);
        DCHECK(local_z != NULL, "Namespaces in XML §4's [7] QName has a [11] LocalPart on both its arms, so an "
                                "expanded name with no local name is core/xml/xml_ns.h's contract broken");
        (void)dom_attr_write(el, ns_z, prefix_z, local_z,
                             tag->atts[i].value, tag->atts[i].value_len, NULL);
        free(ns_z); free(prefix_z); free(local_z);
    }
    free(seen);
    return XML_TREE_OK;
}

XmlTreeError xml_tree_build_step(XmlTreeBuild *b, XmlTreeDetail *detail)
{
    XmlContentItem   item;
    XmlDocumentError derr;
    XmlTreeError     terr;

    DCHECK(b != NULL && detail != NULL, "xml_tree_build_step was called without a build or without the detail "
                                        "record it writes on every call");
    DCHECK(!b->failed, "an XML tree build was stepped after it reported an error — XML §1.2 \"Terminology\" "
                       "says a processor that has detected a fatal error MUST NOT continue normal processing, "
                       "and every consumer of this build discards the tree at the first one");
    DCHECK(!xml_document_ended(b->walk), "an XML tree build was stepped after [1] document matched to the last "
                                         "byte of the entity — xml_tree_build_ended is what a caller's loop "
                                         "tests, and asking past it is a caller that did not");

    memset(detail, 0, sizeof(*detail));
    /* THE END-TAG REPORT IS ABOUT *THIS* STEP AND IS CLEARED BEFORE IT RUNS, which is what makes reading it a
       statement rather than a latch: a consumer that asks after a [14] `CharData` item must be told that no
       element closed, not told again about the one that closed two constructs ago. */
    b->closed = NULL;
    derr = xml_document_next(b->walk, &b->r, &item, &detail->within);
    detail->document = derr;
    if (derr != XML_DOCUMENT_OK) {
        b->failed = true;
        return XML_TREE_ERR_DOCUMENT;
    }

    switch (item.kind) {
    case XML_CONTENT_CHARDATA:
        xml_tree_text_append_normalized(b, item.text.raw, item.text.raw_len);
        return XML_TREE_OK;

    case XML_CONTENT_REFERENCE:
        /* §4.1's [66] CharRef and the five [68] EntityRefs §4.6 predefines are ALREADY the character they
           refer to — core/xml/xml_ref.h resolved them — so what joins the run is that character's own §4.3.3
           bytes. An XML_REF_ENTITY cannot arrive: with no [28] doctypedecl read, core/xml/xml_element.c
           answers a reference outside those five with §4.1's [WFC: Entity Declared], which is this build's
           XML_TREE_ERR_DOCUMENT and never an item. */
        DCHECK(item.ref.kind != XML_REF_ENTITY,
               "an unresolved [68] EntityRef reached the tree builder as an item — with no §2.8 [28] "
               "doctypedecl in this build, core/xml/xml_element.c answers every reference outside §4.6's five "
               "with [WFC: Entity Declared], so an XML_REF_ENTITY item is that layer's contract broken. "
               "Whoever builds [28] owes this site the §4.5 replacement text in the same diff");
        DCHECK(xml_char_is_char(item.ref.cp),
               "a [67] Reference item carried a code point outside XML §2.2's [2] Char — [66] CharRef's "
               "[WFC: Legal Character] is what core/xml/xml_ref.h enforces, so a value outside it is that "
               "contract broken and not a document this build may put in a Text node");
        {
            char buf[XML_CHAR_ENCODE_MAX];
            size_t n = xml_char_encode(item.ref.cp, buf);
            xml_tree_text_append(b, buf, n);
        }
        return XML_TREE_OK;

    case XML_CONTENT_ELEMENT_START:
        xml_tree_flush_text(b);
        terr = xml_tree_start_element(b, &item.tag, detail);
        if (terr != XML_TREE_OK) b->failed = true;
        return terr;

    case XML_CONTENT_ELEMENT_END:
        xml_tree_flush_text(b);
        DCHECK(b->current != b->root_parent,
               "an XML end-tag item arrived with no element open — §3's [WFC: Element Type Match] and the "
               "element stack are core/xml/xml_element.h's, and it reports the two boundaries of [39] in "
               "pairs, so an unmatched end reaching here is that walk's contract broken and not a document's "
               "mistake");
        /* THE CLOSED ELEMENT, TAKEN BEFORE THE INSERTION POINT MOVES OFF IT — the open element IS the tree
           here (see the header), so once `current` climbs to the parent the element §3's [39] just finished is
           no longer named by anything this build holds. */
        b->closed = b->current;
        xml_ns_pop(b->scope);
        /* HTML §14.2: if this element is a `<template>`, everything since its start tag went into its template
           contents and the parse is now leaving that second tree. The pop is checked against the element's OWN
           contents rather than merely counted, which is what stops this record and the tree from drifting: a
           mismatch is a `<template>` whose start tag pushed one fragment and whose end tag is leaving another. */
        {
            lxb_dom_node_t *contents = node_template_content(b->closed);
            if (contents != NULL) {
                DCHECK(b->tpl_n > 0 && b->tpl[b->tpl_n - 1] == contents,
                       "an XML `<template>`'s end tag is leaving template contents this parse never entered — "
                       "HTML §14.2's redirect and this record move at the one line where the insertion point "
                       "does, so a disagreement means a node was appended into a tree the placement did not "
                       "declare and the per-flow delta names the wrong root for it");
                b->tpl_n--;
            }
        }
        b->current = b->current->parent;
        DCHECK(b->current != NULL,
               "closing an XML element left no insertion point — the open element IS the tree here, so a NULL "
               "parent means a node this build appended was detached from under it between two steps");
        /* AND §14.2'S SECOND TREE HAS NO PARENT LINK OUT OF IT. A direct child of a `<template>` was appended
           to the CONTENTS, so climbing by `parent` lands on the fragment rather than on the element that
           contains it; DOM §4.7 "Interface DocumentFragment"'s host is the only way back, and core/dom/node.h
           owns that round trip. Without this the next sibling would still be placed correctly and the
           template's OWN end tag would report a DocumentFragment as the element §3's [39] just closed. */
        {
            lxb_dom_node_t *host = node_template_content_host(b->current);
            if (host != NULL) b->current = host;
        }
        return XML_TREE_OK;

    case XML_CONTENT_CDSECT: {
        lxb_dom_cdata_section_t *c;
        size_t need;
        char  *norm;
        xml_tree_flush_text(b);
        need = xml_char_normalized_len(item.text.raw, item.text.raw_len);
        norm = (char *)malloc(need + 1);
        CHECK(norm != NULL, "OOM building a CDATASection node for XML §2.7's [18] CDSect");
        (void)xml_char_normalize(item.text.raw, item.text.raw_len, norm);
        c = lxb_dom_document_create_cdata_section(b->doc, (const lxb_char_t *)norm, need);
        free(norm);
        CHECK(c != NULL, "OOM creating a CDATASection node");
        xml_tree_place_created(xml_tree_root(b), b->kind, b->current, lxb_dom_interface_node(c));
        return XML_TREE_OK;
    }

    case XML_CONTENT_COMMENT: {
        lxb_dom_comment_t *cm;
        size_t need;
        char  *norm;
        xml_tree_flush_text(b);
        need = xml_char_normalized_len(item.text.raw, item.text.raw_len);
        norm = (char *)malloc(need + 1);
        CHECK(norm != NULL, "OOM building a Comment node for XML §2.5's [15] Comment");
        (void)xml_char_normalize(item.text.raw, item.text.raw_len, norm);
        cm = lxb_dom_document_create_comment(b->doc, (const lxb_char_t *)norm, need);
        free(norm);
        CHECK(cm != NULL, "OOM creating a Comment node");
        xml_tree_place_created(xml_tree_root(b), b->kind, b->current, lxb_dom_interface_node(cm));
        return XML_TREE_OK;
    }

    case XML_CONTENT_PI: {
        lxb_dom_processing_instruction_t *pi;
        size_t need;
        char  *norm;
        xml_tree_flush_text(b);
        need = xml_char_normalized_len(item.pi.data.raw, item.pi.data.raw_len);
        norm = (char *)malloc(need + 1);
        CHECK(norm != NULL, "OOM building a ProcessingInstruction node for XML §2.6's [16] PI");
        (void)xml_char_normalize(item.pi.data.raw, item.pi.data.raw_len, norm);
        pi = lxb_dom_document_create_processing_instruction(b->doc,
                                                           (const lxb_char_t *)item.pi.target, item.pi.target_len,
                                                           (const lxb_char_t *)norm, need);
        free(norm);
        CHECK(pi != NULL, "OOM creating a ProcessingInstruction node");
        xml_tree_place_created(xml_tree_root(b), b->kind, b->current, lxb_dom_interface_node(pi));
        return XML_TREE_OK;
    }
    }
    /* NOT A `default:` ARM — the switch is exhaustive over core/xml/xml_element.h's XmlContentKind, so a
       construct added to [43] `content` makes this not compile rather than be silently dropped from the tree. */
    DFAIL("an XmlContentKind outside the enum reached the XML tree builder");
    return XML_TREE_OK;
}
