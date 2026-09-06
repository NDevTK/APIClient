/* DOM Parsing and Serialization §5.2.1 "XML Serialization" — see xml_serialize.h for what it is, how a member
 * embeds it, and which of §5.2.1's three lists a bare step number names. This file is the algorithm and the
 * sub-algorithms it dispatches to, in the order the standard numbers them:
 *   §5.2.1.1   XML serializing an Element node
 *   §5.2.1.1.1 Recording the namespace
 *   §5.2.1.1.2 The Namespace Prefix Map          (copy / retrieve a preferred prefix string / found / add)
 *   §5.2.1.1.3 Serializing an Element's attributes  (and `serializing an attribute value`)
 *   §5.2.1.1.4 Generating namespace prefixes
 *   §5.2.1.2   XML serializing a Document node
 *   §5.2.1.3   XML serializing a Comment node
 *   §5.2.1.4   XML serializing a CDATASection node
 *   §5.2.1.5   XML serializing a Text node
 *   §5.2.1.6   XML serializing a DocumentFragment node
 *   §5.2.1.7   XML serializing a DocumentType node
 *   §5.2.1.8   XML serializing a ProcessingInstruction node
 *
 * THE SWITCH IS AN IS-A DISPATCH, AND THAT IS THE ONLY READING UNDER WHICH IT WORKS. §5.2.1's dispatch opens
 * "If node's interface is" and then gives one arm each to Element, Document, Comment, CDATASection, Text,
 * DocumentFragment, DocumentType, ProcessingInstruction and Attr; read as an interface-IDENTITY test that
 * switch serializes nothing at all — DOM
 * §4.9 makes every element in an HTML document an `HTMLDivElement` or the like, not an `Element`, so the first
 * arm would never be taken and every document would fall to "Anything else: Throw a TypeError". So it is an
 * IS-A test, which is also how HTML §13.3 Serializing HTML fragments words the same dispatch ("If current node
 * is a Text node"). Two consequences follow and both are visible in this file: `Attr` matches its own arm (DOM
 * §4.9.2 Interface Attr makes an Attr a Node, so `serializeToString(document.createAttribute("x"))` reaches the
 * algorithm at all and returns the empty string §5.2.1 says it does). The reading used to carry a second
 * consequence — a CDATASection is-a Text — and it no longer does: §5.2.1 gives CDATASection an arm of its own
 * AHEAD of Text, so the is-a fallthrough that used to reach the Text arm is unreachable by construction.
 *
 * THE THREE PLACES THIS FILE RECORDED AS DIVERGENCES ARE NOW THE STANDARD, AND THAT IS WHY THEY ARE WRITTEN
 * DOWN RATHER THAN DELETED. Each was a case where the draft as printed contradicted itself and this engine
 * took the reading that round-trips; the maintained edition has since adopted all three, so what stood here as
 * "we diverge" is a claim about a document that no longer says it — and a reader who re-derived the old
 * argument would edit the engine BACK toward a draft nobody maintains. The three, with the sentence that now
 * settles each:
 *
 *   (1) A PREFIX IS GENERATED ONLY WHEN NO CANDIDATE WAS FOUND. The guard is §5.2.1.1.3 "Serializing an
 *       Element's attributes" step 3.6.4's own first clause — "Otherwise (attribute namespace is not the XMLNS
 *       namespace), if candidate prefix is null" — where the older draft ran that sub-step unconditionally
 *       after retrieving a preferred prefix, so an attribute in a namespace an ancestor already bound got a
 *       freshly generated `nsN`. wpt/domparsing/XMLSerializer-serializeToString.html measures it directly, in
 *       the subtest named `Check if an attribute with namespace and no prefix is serialized with the
 *       nearest-declared prefix`.
 *
 *   (2) TAB, LINE FEED AND CARRIAGE RETURN IN AN ATTRIBUTE VALUE ARE WRITTEN AS XML §4.1 Character and Entity
 *       References' [66] CharRef, because a literal one does not survive a re-parse: XML §3.3.3
 *       Attribute-Value Normalization step 3c says an XML processor MUST replace "a white space character
 *       (#x20, #xD, #xA, #x9)" with a space, while its own note says "if the unnormalized attribute value
 *       contains a character reference to a white space character other than space (#x20), the normalized
 *       value contains the referenced character itself (#xD, #xA or #x9)". §5.2.1.1.3's `serializing an
 *       attribute value` now lists exactly those three replacements beside `&`, `"` and `<`.
 *
 *   (3) A CDATASection IS SERIALIZED AS XML §2.7 CDATA Sections' [18] CDSect. §5.2.1's dispatch now carries a
 *       `CDATASection` arm of its own, ahead of `Text`, and §5.2.1.4 "XML serializing a CDATASection node" is
 *       the two-line algorithm this file already ran — where the older draft named no such arm, so the section
 *       fell to the Text arm and was ESCAPED, turning `<![CDATA[<b>]]>` into `&lt;b&gt;`, a different tree on
 *       re-parse.
 *
 * WHAT STILL DIVERGES, AND IT IS ONE THING. §5.2.1.1.3's `serializing an attribute value` lists `&`, `"`, `<`
 * and the three whitespace characters and NOT `>`, while the Note directly beneath that same list says the
 * algorithm "goes above and beyond the grammar requirement in the XML specification's AttValue production by
 * also replacing '>' characters". The list and its own note disagree; this file replaces `>`, which is what
 * the note describes and what browsers do, and what an XML parser reading the result cannot tell apart from
 * the literal either way.
 *
 * AND TWO PLACES IT DOES NOT DIVERGE, recorded because both are measured and both look like bugs from the
 * outside. A null-namespace attribute whose localName is `xmlns` (what `el.setAttribute("xmlns", u)` makes) is
 * SERIALIZED, because §5.2.1.1.3's step 3 has no arm that drops it — the only step that mentions it is step
 * 3.9's require-well-formed THROW, which says what the draft thinks of it and is not reachable with the flag
 * unset. And §5.2.1.1 step 11.1 sets `ignore namespace definition attribute` whenever a local default
 * namespace declaration exists, which drops the element's own `xmlns=""` even where the element really is in
 * no namespace. Both are the draft applied literally; neither is forced into a contradiction by any other
 * sentence of it, so neither is this file's to decide. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <lexbor/dom/dom.h>
#include <lexbor/html/html.h>

#include "check.h"
#include "cutils.h"       /* utf8_decode_len — the engine's own UTF-8 reader, as core/indexeddb/idb_key_path.c uses it */
#include "quickjs.h"
#include "core/dom/attr_list.h"
#include "core/dom/element.h"
#include "core/dom/node.h"
#include "core/xml/xml_char.h"
#include "core/xml/xml_literal.h"
#include "core/xml/xml_name.h"
#include "core/xml/xml_serialize.h"

enum { XML_SERIALIZE_ALGO_STAGES(JS_STEP_STAGE_ENUM, XS_PHASE, "") XS_PHASE_N };

/* §5.2.1's `null`, as the one value that is never a pool id. The empty string IS interned, so "" and null stay
   two different answers — which §5.2.1.1.1's "The empty string is a legitimate return value and is not
   converted to null"
   depends on and which every comparison in this file therefore has to be able to make. */
#define XS_NULL 0u

/* THE THREE NAMESPACE URIs THIS ALGORITHM NAMES, spelled once. DOM §1.4 Namespaces defines the first two by
   these bytes and Namespaces in XML 1.0 §3 Declaring Namespaces defines the third; every comparison below is
   an id comparison against the pool entry made from them at `xml_serialize_start`. */
#define XS_NS_HTML  "http://www.w3.org/1999/xhtml"
#define XS_NS_XML   "http://www.w3.org/XML/1998/namespace"
#define XS_NS_XMLNS "http://www.w3.org/2000/xmlns/"

/* §5.2.1.1 step 14's list, VERBATIM AND IN THE DRAFT'S OWN ORDER, and it is NOT HTML §13.3's "serializes as
   void": §13.3's list has thirteen names and this one has nineteen — `basefont`, `bgsound`, `frame`, `keygen`,
   `menuitem` and `param` are here and not there. The condition is also different in kind (it fires only when
   the element has no children, where §13.3's fires regardless), so `lxb_html_node_is_void` would answer a
   related question rather than this one. */
static const char *const XS_VOID_ELEMENTS[] = {
    "area", "base", "basefont", "bgsound", "br", "col", "embed", "frame", "hr", "img", "input", "keygen",
    "link", "menuitem", "meta", "param", "source", "track", "wbr", NULL
};

/* ---- the accumulator ------------------------------------------------------------------------------------- */

/* THE RUNTIME'S ALLOCATOR, BECAUSE THE DECLARATION'S IS — xml_serialize_visit_state hands a forked arm a
   js_malloc'd copy of every buffer here and the teardown discharges them with js_free, so growing one through
   the C library would hand the runtime a block it never issued. */
static void xs_out(JSContext *ctx, XmlSerializeState *s, const char *b, size_t n)
{
    if (s->out_len + n + 1 > s->out_cap) {
        size_t want = s->out_cap ? s->out_cap * 2 : 256;
        char *p;
        while (want < s->out_len + n + 1) want *= 2;
        p = js_realloc(ctx, s->out, want);
        CHECK(p != NULL, "the XML serializer could not grow its markup accumulator");
        s->out = p;
        s->out_cap = want;
    }
    memcpy(s->out + s->out_len, b, n);
    s->out_len += n;
}

static void xs_lit(JSContext *ctx, XmlSerializeState *s, const char *lit)
{
    xs_out(ctx, s, lit, strlen(lit));
}

/* ---- the string pool ------------------------------------------------------------------------------------- */

/* Entries are (offset, length) so a NUL inside a name — which DOM §4.5's element local names admit and XML's
   [5] Name does not — is a byte like any other rather than a string that ends early. */
static void xs_pool_put(JSContext *ctx, XmlSerializeState *s, const char *b, size_t n)
{
    if (s->pool_len + n > s->pool_cap) {
        size_t want = s->pool_cap ? s->pool_cap * 2 : 256;
        char *p;
        while (want < s->pool_len + n) want *= 2;
        p = js_realloc(ctx, s->pool, want);
        CHECK(p != NULL, "the XML serializer could not grow its name pool");
        s->pool = p;
        s->pool_cap = want;
    }
    if (n) memcpy(s->pool + s->pool_len, b, n);
    s->pool_len += n;
}

/* The bytes written since `start` become an id — the id of an EQUAL earlier entry when there is one, and the
   pool is rewound in that case. Ids are 1-based so that XS_NULL can be a value no string ever has. */
static uint32_t xs_pool_finish(JSContext *ctx, XmlSerializeState *s, size_t start)
{
    size_t n = s->pool_len - start;
    int i;

    for (i = 0; i < s->pool_n; i++) {
        uint32_t off = s->pool_ent[2 * i], len = s->pool_ent[2 * i + 1];

        if (len != n || (n && memcmp(s->pool + off, s->pool + start, n) != 0)) continue;
        s->pool_len = start;
        return (uint32_t)(i + 1);
    }
    if (s->pool_n == s->pool_ecap) {
        int want = s->pool_ecap ? s->pool_ecap * 2 : 16;
        uint32_t *p = js_realloc(ctx, s->pool_ent, sizeof(uint32_t) * 2 * (size_t)want);

        CHECK(p != NULL, "the XML serializer could not grow its name index");
        s->pool_ent = p;
        s->pool_ecap = want;
    }
    s->pool_ent[2 * s->pool_n] = (uint32_t)start;
    s->pool_ent[2 * s->pool_n + 1] = (uint32_t)n;
    s->pool_n++;
    return (uint32_t)s->pool_n;
}

static uint32_t xs_intern(JSContext *ctx, XmlSerializeState *s, const char *b, size_t n)
{
    size_t start = s->pool_len;

    xs_pool_put(ctx, s, b, n);
    return xs_pool_finish(ctx, s, start);
}

/* APPEND AN ALREADY-INTERNED NAME to the string being built, WITHOUT ever holding a pointer into the pool
   across the write. `xs_pool_put` may realloc, so `xs_pool_put(ctx, s, xs_str(s, id, &n), n)` is a use of a
   pointer the very call that consumes it can move — the argument is evaluated first and the block is gone by
   the time memcpy reads it. An OFFSET survives the realloc, which is why the copy is made here from the
   post-growth base rather than by the caller. */
static void xs_pool_put_id(JSContext *ctx, XmlSerializeState *s, uint32_t id)
{
    size_t off, n, dst;

    DCHECK(id != XS_NULL && (int)id <= s->pool_n, "an un-interned name was appended to the XML serializer's pool");
    off = s->pool_ent[2 * (id - 1)];
    n = s->pool_ent[2 * (id - 1) + 1];
    dst = s->pool_len;
    if (!n) return;
    if (dst + n > s->pool_cap) {
        size_t want = s->pool_cap ? s->pool_cap * 2 : 256;
        char *p;

        while (want < dst + n) want *= 2;
        p = js_realloc(ctx, s->pool, want);
        CHECK(p != NULL, "the XML serializer could not grow its name pool");
        s->pool = p;
        s->pool_cap = want;
    }
    memmove(s->pool + dst, s->pool + off, n);   /* memmove: source and destination are one block */
    s->pool_len = dst + n;
}

static const char *xs_str(const XmlSerializeState *s, uint32_t id, size_t *len)
{
    DCHECK(id != XS_NULL && (int)id <= s->pool_n,
           "the XML serializer asked for the bytes of a name it never interned — XS_NULL is the algorithm's "
           "`null` and has no bytes, so a caller reaching here has not made the null test the spec makes");
    *len = s->pool_ent[2 * (id - 1) + 1];
    return s->pool + s->pool_ent[2 * (id - 1)];
}

static void xs_out_id(JSContext *ctx, XmlSerializeState *s, uint32_t id)
{
    size_t n = 0;
    const char *b = xs_str(s, id, &n);

    xs_out(ctx, s, b, n);
}

/* ---- §5.2.1.1.2 The Namespace Prefix Map ------------------------------------------------------------------ */

/* "To add a string prefix to a namespace prefix map map given a namespace ns" — which the standard states as
   "Let candidates be « prefix »" when the key is absent and "Append prefix to candidates" when it is present.
   Both are this one append: the candidates list for a key IS the
   subsequence of pairs carrying it, in insertion order. */
static void xs_map_add(JSContext *ctx, XmlSerializeState *s, uint32_t ns, uint32_t prefix)
{
    if (s->map_n == s->map_cap) {
        int want = s->map_cap ? s->map_cap * 2 : 8;
        XmlSerPair *p = js_realloc(ctx, s->map, sizeof(XmlSerPair) * (size_t)want);

        CHECK(p != NULL, "the XML serializer could not grow its namespace prefix map");
        s->map = p;
        s->map_cap = want;
    }
    s->map[s->map_n].key = ns;
    s->map[s->map_n].val = prefix;
    s->map_n++;
}

/* "To retrieve a preferred prefix string preferred prefix (a string or null) from the namespace prefix map map
   given a namespace ns": the candidates list for `ns`, walked from beginning to end, returning `preferred` if
   it occurs and otherwise "candidates[size of candidates - 1]" — which is the standard's own MRU rule ("The
   last seen prefix for a given namespace is at the end of its respective list"). XS_NULL when `ns` is not a
   key at all, and a `preferred` of XS_NULL (an element or attribute with no prefix) matches nothing, so such
   a caller always gets the last item. */
static uint32_t xs_map_retrieve(const XmlSerializeState *s, uint32_t ns, uint32_t preferred)
{
    uint32_t last = XS_NULL;
    int i;

    for (i = 0; i < s->map_n; i++) {
        if (s->map[i].key != ns) continue;
        if (preferred != XS_NULL && s->map[i].val == preferred) return preferred;
        last = s->map[i].val;
    }
    return last;
}

/* "To check if a string prefix is found in a namespace prefix map map given a namespace ns." */
static bool xs_map_found(const XmlSerializeState *s, uint32_t ns, uint32_t prefix)
{
    int i;

    for (i = 0; i < s->map_n; i++)
        if (s->map[i].key == ns && s->map[i].val == prefix) return true;
    return false;
}

/* §5.2.1.1.4 Generating namespace prefixes — "Let generated prefix be the concatenation of "ns" and the
   current numerical value of prefix index", increment the index, add the generated prefix to the map.
   THE INDEX IS THE WHOLE SERIALIZATION'S, which is why §5.2.1 step 5 hands the walk "a mutable reference to
   prefix index": two attributes on two different elements get `ns1` and `ns2`, never `ns1` twice. It is
   deliberately NOT checked against the prefixes already declared on the element — §5.2.1.1.4 does not check,
   says so against itself in Issue 44 ("The algorithm should have a loop until a generated prefix is not
   found"), and the corpus pins the unchecked behaviour ("Check if \"ns1\" is generated even if the element
   already has xmlns:ns1"). */
static uint32_t xs_generate_prefix(JSContext *ctx, XmlSerializeState *s, uint32_t ns)
{
    char buf[32];
    uint32_t id;
    int n = snprintf(buf, sizeof buf, "ns%d", s->prefix_index);

    DCHECK(n > 0 && (size_t)n < sizeof buf, "a generated namespace prefix did not fit its own buffer");
    s->prefix_index++;
    id = xs_intern(ctx, s, buf, (size_t)n);
    xs_map_add(ctx, s, ns, id);
    return id;
}

/* §5.2.1.1's `local prefixes map` — prefix → namespace, with the null namespace stored as the empty string.
   XS_NULL is "not a key", which is the distinction §5.2.1.1.3 step 3.6.3 asks for by name: "prefix is not null
   and either local prefixes map does not contain attr's local name, or local prefixes map does contain attr's
   local name, but" its value is not the attribute's own. */
static void xs_local_add(JSContext *ctx, XmlSerializeState *s, uint32_t prefix, uint32_t ns_string)
{
    if (s->local_n == s->local_cap) {
        int want = s->local_cap ? s->local_cap * 2 : 8;
        XmlSerPair *p = js_realloc(ctx, s->local, sizeof(XmlSerPair) * (size_t)want);

        CHECK(p != NULL, "the XML serializer could not grow its local prefixes map");
        s->local = p;
        s->local_cap = want;
    }
    s->local[s->local_n].key = prefix;
    s->local[s->local_n].val = ns_string;
    s->local_n++;
}

static uint32_t xs_local_get(const XmlSerializeState *s, uint32_t prefix)
{
    int i;

    for (i = 0; i < s->local_n; i++)
        if (s->local[i].key == prefix) return s->local[i].val;
    return XS_NULL;
}

/* §5.2.1.1.3's `localname set` — "tuples of unique attribute (namespace, local name) pairs", read only by
   the require-well-formed duplicate check. */
static bool xs_lnset_has(const XmlSerializeState *s, uint32_t ns, uint32_t local)
{
    int i;

    for (i = 0; i < s->lnset_n; i++)
        if (s->lnset[i].key == ns && s->lnset[i].val == local) return true;
    return false;
}

static void xs_lnset_add(JSContext *ctx, XmlSerializeState *s, uint32_t ns, uint32_t local)
{
    if (s->lnset_n == s->lnset_cap) {
        int want = s->lnset_cap ? s->lnset_cap * 2 : 8;
        XmlSerPair *p = js_realloc(ctx, s->lnset, sizeof(XmlSerPair) * (size_t)want);

        CHECK(p != NULL, "the XML serializer could not grow its localname set");
        s->lnset = p;
        s->lnset_cap = want;
    }
    s->lnset[s->lnset_n].key = ns;
    s->lnset[s->lnset_n].val = local;
    s->lnset_n++;
}

/* ---- the level stack ------------------------------------------------------------------------------------- */

static void xs_push(JSContext *ctx, XmlSerializeState *s, lxb_dom_node_t *node)
{
    if (s->sp == s->scap) {
        int want = s->scap ? s->scap * 2 : 8;
        XmlSerLevel *p = js_realloc(ctx, s->stack, sizeof(XmlSerLevel) * (size_t)want);

        CHECK(p != NULL, "the XML serializer could not grow its level stack");
        s->stack = p;
        s->scap = want;
    }
    s->stack[s->sp].node = node;
    s->stack[s->sp].limit = s->limit;
    s->stack[s->sp].qname = XS_NULL;
    s->stack[s->sp].ctx_ns = s->ctx_ns;
    /* §5.2.1.1 step 6's "copy a namespace prefix map", AS A LENGTH — see xml_serialize.h. Restoring it on the
       way out is what keeps a SIBLING from inheriting the declarations this element added. */
    s->stack[s->sp].map_n = s->map_n;
    s->sp++;
}

/* §5.2.1.1 step 18's SUBSTITUTION, AS ONE QUESTION WITH TWO ASKERS. "If ns is the HTML namespace, and node's
   local name is "template" (this is a template element), append to markup the result of XML serializing a
   DocumentFragment node given node's template contents (a DocumentFragment)" — so a template's
   ORDINARY children are never serialized, and the node whose children the walk descends into is not always the
   node the walk is standing on. Both askers are descents into a node's children and they differ only in where
   the element's namespace was computed: step 18 already holds step 10's `ns` for the element it is opening,
   while xml_serialize_start's XML_SERIALIZE_CHILDREN entry never runs step 10 at all, because the node it is
   entered on contributes no tags to the output and therefore has no start tag to name. Asked at one of them
   only, a `template` reached through the other answers with the children `t.appendChild(x)` created and with
   its template contents absent — a real subtree replaced by a real subtree, with nothing to say so. */
static lxb_dom_node_t *xs_children_container(lxb_dom_node_t *n, uint32_t elem_ns, uint32_t ns_html)
{
    size_t llen = 0;
    const char *loc;
    lxb_dom_node_t *content;

    if (n->type != LXB_DOM_NODE_TYPE_ELEMENT || elem_ns != ns_html) return n;
    loc = (const char *)lxb_dom_element_local_name(lxb_dom_interface_element(n), &llen);
    DCHECK(loc != NULL, "an element in the tree has no local name");
    if (llen != 8 || memcmp(loc, "template", 8) != 0) return n;
    content = node_template_content(n);
    DCHECK(content != NULL,
           "an HTML-namespace `template` element has no template contents — HTML §4.12.3 The template "
           "element establishes them when the element is created, so an element without them was made "
           "some other way");
    return content;
}

/* §5.2.1.2 XML serializing a Document node's and §5.2.1.6 XML serializing a DocumentFragment node's shared
   sentence — "For each child of node's children: Append to markup the result of running the XML serialization
   algorithm given child, inherited ns, map, prefix index, and require well-formed" — as the DESCENT it is. The level
   carries a NULL node, so the walk ENDS when that level is popped rather than closing a tag nothing opened.
   Returns whether there is a first child to dispatch on; a container with none finishes at §5.2.1's step 5
   with the markup it already has. */
static bool xs_enter_children(JSContext *ctx, XmlSerializeState *s, lxb_dom_node_t *container)
{
    xs_push(ctx, s, NULL);
    s->limit = container;
    s->cur = container->first_child;
    return s->cur != NULL;
}

/* ---- the productions the require-well-formed flag reads --------------------------------------------------- */

/* THE PREDICATES ARE core/xml's AND THE WALK IS THE ENGINE'S, and the split is deliberate rather than a second
 * decoder. core/xml/xml_char.h's READER is for an ENTITY — it applies XML §2.11 End-of-Line Handling, latches
 * §1.2 Terminology's fatal errors, and asserts §4.3.3 Character Encoding in Entities' precondition that the
 * Byte Order Mark has already been consumed. None of that is true of a DOM string: a Text node may legitimately
 * hold U+FEFF as its first character (`document.createTextNode("﻿")` is an ordinary thing for a page to
 * do), so handing one to that reader would abort the engine on the page's own data. What IS shared is the
 * PRODUCTION — `xml_char_is_char` and `xml_literal_is_pubid_char` decide every code point here — and the walk
 * over the bytes is cutils.h's `utf8_decode_len`, the same reader core/indexeddb/idb_key_path.c uses for the
 * same reason: these bytes are the JS string encoder's output. */
static bool xs_all_cp(const char *b, size_t n, bool pubid)
{
    const uint8_t *p = (const uint8_t *)b, *end = p + n, *next;

    while (p < end) {
        uint32_t cp = utf8_decode_len(p, (size_t)(end - p), &next);

        DCHECK(next != p, "the XML serializer walked an ill-formed UTF-8 sequence in a DOM string — these bytes "
                          "are the JS string encoder's output, so a broken sequence is this engine's own "
                          "mistake and never the page's data");
        if (next == p) return false;
        p = next;
        if (!xml_char_is_char(cp)) return false;
        if (pubid && !xml_literal_is_pubid_char(cp)) return false;
    }
    return true;
}

/* XML §2.2 Characters' [2] `Char` over a whole string. */
static bool xs_all_char(const char *b, size_t n) { return xs_all_cp(b, n, false); }
/* XML §2.3 Common Syntactic Constructs' [13] `PubidChar` over a whole string. */
static bool xs_all_pubid(const char *b, size_t n) { return xs_all_cp(b, n, true); }

static bool xs_has_byte(const char *b, size_t n, char c)
{
    size_t i;

    for (i = 0; i < n; i++)
        if (b[i] == c) return true;
    return false;
}

/* A byte substring. The three needles this file looks for — `--`, `]]>` and `?>` — are all ASCII, and an ASCII
   byte can never occur as a UTF-8 continuation byte (every continuation byte is 0x80..0xBF), so asking the
   question of bytes and of characters is the same question. */
/* DOM Parsing and Serialization §5.2.1.7 XML serializing a DocumentType node's "the serialization of the id":
   "If id contains U+0022 QUOTATION MARK, let q be U+0027 APOSTROPHE and let q be U+0022 QUOTATION MARK
   otherwise. Return the concatenation of q, id and q."

   IT IS NOT GATED ON `require well formed`, WHICH IS WHY BOTH IDS GO THROUGH IT. The well-formedness step
   above refuses a systemId holding BOTH marks, because then no delimiter exists — and that check is the
   reason a hardcoded `"` looked safe here. It is not the same question: an id holding ONLY a quotation mark
   passes that check and still cannot be written inside quotation marks. The publicId is safe from a
   different direction and only while `require well formed` is true, since XML §2.3's [13] PubidChar
   production admits an apostrophe and not a quotation mark; with the flag false that check does not run, so
   this entry decides for both ids rather than for the one that is wrong under the flag this engine happens
   to be called with. */
static char xs_id_quote(const char *b, size_t n) { return xs_has_byte(b, n, '"') ? '\'' : '"'; }

static bool xs_has_bytes(const char *b, size_t n, const char *needle, size_t m)
{
    size_t i;

    if (m > n) return false;
    for (i = 0; i + m <= n; i++)
        if (memcmp(b + i, needle, m) == 0) return true;
    return false;
}

/* §5.2.1 step 5: "If an exception occurs during the execution of the algorithm, then catch that exception and
   throw an 'InvalidStateError' DOMException." Every inner throw of §5.2.1.1 through §5.2.1.8 is observable only
   as that one exception, so it is thrown HERE, at the site that detected the ill-formedness, carrying the
   sentence that was violated — an inner exception class would be a value no caller can ever see. */
static int xs_ill_formed(JSContext *ctx, const char *why)
{
    JS_ThrowDOMException(ctx, "InvalidStateError", "%s", why);
    return JS_STEP_ABRUPT;
}

/* ---- §5.2.1.1.3's `serializing an attribute value` -------------------------------------------------------- */

/* "If attribute value is null, then return the empty string. Otherwise, return attribute value, first
   replacing any occurrences of the following: `&` with `&amp;`, `"` with `&quot;`, `<` with `&lt;`,
   U+0009 CHARACTER TABULATION with `&#9;`, U+000A LINE FEED (LF) with `&#xA;`, U+000D CARRIAGE RETURN (CR)
   with `&#xD;`" — the three whitespace characters being the ones XML §3.3.3 Attribute-Value Normalization
   would otherwise destroy. The list does not name `>` and the Note directly beneath it does; this writes it,
   which is the one place this algorithm still diverges — see the head comment.
   `v` NULL is the standard's null. Returns false HAVING THROWN §5.2.1 step 5's exception. */
static bool xs_attr_value(JSContext *ctx, XmlSerializeState *s, const char *v, size_t n, int *err)
{
    size_t i, run = 0;

    *err = 0;
    if (s->require_well_formed && v && !xs_all_char(v, n)) {
        *err = xs_ill_formed(ctx, "DOM Parsing and Serialization §5.2.1.1.3 serializing an attribute value: the "
                                  "value holds a character that is not matched by the XML §2.2 Characters [2] "
                                  "Char production, so this element's serialization would not be well-formed");
        return false;
    }
    if (!v) return true;                       /* the standard's null: the empty string */
    for (i = 0; i < n; i++) {
        const char *rep = NULL;

        switch (v[i]) {
        case '&':  rep = "&amp;";  break;
        case '"':  rep = "&quot;"; break;
        case '<':  rep = "&lt;";   break;
        case '>':  rep = "&gt;";   break;
        case '\t': rep = "&#9;";   break;
        case '\n': rep = "&#xA;";  break;
        case '\r': rep = "&#xD;";  break;
        default:   break;
        }
        if (!rep) { run++; continue; }
        if (run) xs_out(ctx, s, v + i - run, run);
        run = 0;
        xs_lit(ctx, s, rep);
    }
    if (run) xs_out(ctx, s, v + n - run, run);
    return true;
}

/* §5.2.1.5 XML serializing a Text node's replacements: `&`, `<` and `>` and nothing else — an attribute value's
   `"` is not escaped in character data, which is the whole difference between the two escapes. */
static void xs_text_escaped(JSContext *ctx, XmlSerializeState *s, const char *b, size_t n)
{
    size_t i, run = 0;

    for (i = 0; i < n; i++) {
        const char *rep = NULL;

        if (b[i] == '&')      rep = "&amp;";
        else if (b[i] == '<') rep = "&lt;";
        else if (b[i] == '>') rep = "&gt;";
        if (!rep) { run++; continue; }
        if (run) xs_out(ctx, s, b + i - run, run);
        run = 0;
        xs_lit(ctx, s, rep);
    }
    if (run) xs_out(ctx, s, b + n - run, run);
}

/* ---- the tree reads ---------------------------------------------------------------------------------------
 * Each is the DOM attribute §5.2.1 names, read out of lexbor's storage. Every one of them hands back BORROWED
 * bytes that are NOT NUL-terminated, so every caller carries the length. */

static const char *xs_node_ns(const lxb_dom_node_t *n, size_t *len)
{
    const lxb_char_t *b = lxb_ns_by_id(n->owner_document->ns, n->ns, len);

    if (!b || !*len) { *len = 0; return NULL; }     /* DOM §1.4's null namespace, not the empty string */
    return (const char *)b;
}

static const char *xs_cdata(const lxb_dom_node_t *n, size_t *len)
{
    const lxb_dom_character_data_t *cd = (const lxb_dom_character_data_t *)n;

    *len = cd->data.length;
    return cd->data.data ? (const char *)cd->data.data : "";
}

/* The namespace of a node as an interned id, XS_NULL for the null namespace. */
static uint32_t xs_ns_id(JSContext *ctx, XmlSerializeState *s, const lxb_dom_node_t *n)
{
    size_t len = 0;
    const char *b = xs_node_ns(n, &len);

    return b ? xs_intern(ctx, s, b, len) : XS_NULL;
}

/* ---- §5.2.1.1.1 Recording the namespace ------------------------------------------------------------------- */

/* ONE ATTRIBUTE of Main, so the walk rests once per attribute of a list the page chose the length of. `a` is
   the attribute; the caller has already advanced its cursor. */
static void xs_record_one(JSContext *ctx, XmlSerializeState *s, lxb_dom_attr_t *a)
{
    size_t nslen = 0, plen = 0, llen = 0, vlen = 0;
    const char *ans = (const char *)dom_attr_ns(a, &nslen);
    const char *pfx = (const char *)dom_attr_prefix(a, &plen);
    const char *loc, *val;
    uint32_t pd, nd_str, nd_ns;

    /* "Only attributes in the XMLNS namespace are considered (e.g., attributes made to look like namespace
       declarations via setAttribute("xmlns:pretend-prefix", "pretend-namespace") are not included)." */
    if (!ans || xs_intern(ctx, s, ans, nslen) != s->ns_xmlns) return;
    val = (const char *)lxb_dom_attr_value(a, &vlen);
    if (!val) { val = ""; vlen = 0; }
    if (!pfx || !plen) {
        /* "attr is a default namespace declaration. Set the default namespace attr value to attr's value" —
           and the standard's own note: "the empty string is a legitimate return value and is not converted to
           null", which is why this interns rather than testing for emptiness. */
        s->local_default = xs_intern(ctx, s, val, vlen);
        return;
    }
    loc = (const char *)lxb_dom_attr_local_name(a, &llen);
    DCHECK(loc != NULL, "an attribute in the XMLNS namespace has no local name");
    pd = xs_intern(ctx, s, loc, llen);                     /* `prefix definition` */
    nd_str = xs_intern(ctx, s, val, vlen);                 /* `namespace definition`, as written */
    /* "If namespace definition is the XML namespace, then stop running these steps" — the note explains it:
       XML namespace definitions in prefixes are ignored so that §5.2.1.1 step 11.2's unconditional `xml:`
       prefixing cannot conflict with one. */
    if (nd_str == s->ns_xml) return;
    /* "If namespace definition is the empty string (the declarative form of having no namespace), then let
       namespace definition be null instead." */
    nd_ns = vlen ? nd_str : XS_NULL;
    /* "If prefix definition is found in map given the namespace namespace definition, then stop" — which is
       what stops a descendant from re-declaring a prefix an ancestor already bound to the same namespace. */
    if (xs_map_found(s, nd_ns, pd)) return;
    xs_map_add(ctx, s, nd_ns, pd);
    /* "...with the namespace definition as the key's value replacing the value of null with the empty
       string if applicable" — `nd_str` is exactly that, since the empty string is what null was written as. */
    xs_local_add(ctx, s, pd, nd_str);
}

/* ---- §5.2.1.1.3 step 3, one attribute ---------------------------------------------------------------------
 * Returns JS_STEP_YIELD, or JS_STEP_ABRUPT having thrown. */
static int xs_attr_one(JSContext *ctx, XmlSerializeState *s, lxb_dom_attr_t *a)
{
    size_t nslen = 0, plen = 0, llen = 0, vlen = 0;
    const char *ansb = (const char *)dom_attr_ns(a, &nslen);
    const char *pfxb = (const char *)dom_attr_prefix(a, &plen);
    const char *loc = (const char *)lxb_dom_attr_local_name(a, &llen);
    const char *val = (const char *)lxb_dom_attr_value(a, &vlen);
    uint32_t ans = ansb ? xs_intern(ctx, s, ansb, nslen) : XS_NULL;
    uint32_t pid = (pfxb && plen) ? xs_intern(ctx, s, pfxb, plen) : XS_NULL;
    uint32_t lid, cand = XS_NULL;
    int err = 0;

    DCHECK(loc != NULL, "an attribute in the tree has no local name");
    if (!val) { val = ""; vlen = 0; }
    lid = xs_intern(ctx, s, loc, llen);

    /* Steps 3.2-3.4: the tuple, the well-formed uniqueness constraint over (namespace, local name), and the
       append to localname set. */
    if (s->require_well_formed && xs_lnset_has(s, ans, lid))
        return xs_ill_formed(ctx, "DOM Parsing and Serialization §5.2.1.1.3 Serializing an Element's "
                                  "attributes: two attributes of this element share a namespace and a local "
                                  "name, so its serialization would not be well-formed");
    xs_lnset_add(ctx, s, ans, lid);

    if (ans != XS_NULL) {
        uint32_t vstr = xs_intern(ctx, s, val, vlen);      /* the value as a STRING ("" stays "") */
        uint32_t vns = vlen ? vstr : XS_NULL;              /* the value as a NAMESPACE ("" is null) */

        cand = xs_map_retrieve(s, ans, pid);
        if (ans == s->ns_xmlns) {
            /* "If any of the following are true, then stop running these steps and goto Loop." */
            if (vstr == s->ns_xml) return JS_STEP_YIELD;
            if (pid == XS_NULL && s->ignore_nsdef) return JS_STEP_YIELD;
            if (pid != XS_NULL) {
                uint32_t lv = xs_local_get(s, lid);

                if ((lv == XS_NULL || lv != vstr) && xs_map_found(s, vns, lid)) return JS_STEP_YIELD;
            }
            if (s->require_well_formed && vstr == s->ns_xmlns)
                return xs_ill_formed(ctx, "DOM Parsing and Serialization §5.2.1.1.3 Serializing an Element's "
                                          "attributes: a namespace declaration whose value is the XMLNS "
                                          "namespace would produce invalid XML, because that namespace is "
                                          "reserved and cannot be applied as an element's namespace by an XML "
                                          "parser");
            if (s->require_well_formed && vlen == 0)
                return xs_ill_formed(ctx, "DOM Parsing and Serialization §5.2.1.1.3 Serializing an Element's "
                                          "attributes: a namespace prefix declaration cannot be used to "
                                          "undeclare a namespace (a default namespace declaration is what "
                                          "does that)");
            if (pid == s->s_xmlns) cand = s->s_xmlns;
        } else if (cand == XS_NULL) {
            /* §5.2.1.1.3 step 3.6.4, whose own first clause is this guard: "Otherwise (attribute namespace is
               not the XMLNS namespace), if candidate prefix is null" — see the head comment, (1), for the
               older draft that ran the sub-step unconditionally and what that cost.
               NOT COVERED: step 3.6.4's FIRST arm, "If prefix is not null and local prefixes map does not
               contain prefix, set candidate prefix to prefix", so an attribute carrying a prefix for a
               namespace nothing in scope declares gets a generated `nsN` here where the standard would keep
               the attribute's own prefix. THE NEXT DIFF builds that arm: a lookup of `pid` in the local
               prefixes map (xs_local_get already answers it) selecting `pid` over xs_generate_prefix, with
               the map add and the local-prefixes-map set that follow it applying to whichever was chosen.
               ITS ABSENCE SHOWS as `ns1:` in place of the authored prefix on exactly that attribute —
               wpt/domparsing/XMLSerializer-serializeToString.html compares the serialization as a string, so
               the arm is observable there and nowhere in this file's own asserts. */
            cand = xs_generate_prefix(ctx, s, ans);
            xs_lit(ctx, s, " xmlns:");
            xs_out_id(ctx, s, cand);
            xs_lit(ctx, s, "=\"");
            if (!xs_attr_value(ctx, s, ansb, nslen, &err)) return err;
            xs_lit(ctx, s, "\"");
        }
    }

    xs_lit(ctx, s, " ");
    if (cand != XS_NULL) { xs_out_id(ctx, s, cand); xs_lit(ctx, s, ":"); }
    if (s->require_well_formed &&
        (xs_has_byte(loc, llen, ':') || !xml_name_is_name(loc, llen) || (lid == s->s_xmlns && ans == XS_NULL)))
        return xs_ill_formed(ctx, "DOM Parsing and Serialization §5.2.1.1.3 Serializing an Element's "
                                  "attributes: this attribute's local name holds a colon, is not an XML §2.3 "
                                  "Common Syntactic Constructs [5] Name, or is `xmlns` in no namespace, so the "
                                  "serialization of this attribute would not be well-formed");
    xs_out(ctx, s, loc, llen);
    xs_lit(ctx, s, "=\"");
    if (!xs_attr_value(ctx, s, val, vlen, &err)) return err;
    xs_lit(ctx, s, "\"");
    return JS_STEP_YIELD;
}

/* ---- §5.2.1.1 steps 9-12 ---------------------------------------------------------------------------------- */

/* The element's `qualified name` and the ONE namespace declaration its start tag may have to carry. Returns
   JS_STEP_YIELD, or JS_STEP_ABRUPT having thrown. */
static int xs_element_name(JSContext *ctx, XmlSerializeState *s, lxb_dom_element_t *el)
{
    size_t llen = 0, plen = 0, nslen = 0;
    const char *loc = (const char *)lxb_dom_element_local_name(el, &llen);
    const char *pfxb = element_prefix(el, &plen);
    const char *nsb = xs_node_ns(lxb_dom_interface_node(el), &nslen);
    uint32_t pid = (pfxb && plen) ? xs_intern(ctx, s, pfxb, plen) : XS_NULL;
    size_t start;
    int err = 0;

    DCHECK(loc != NULL, "an element in the tree has no local name");
    s->inherited_ns = s->ctx_ns;                                    /* step 9 */
    s->elem_ns = nsb ? xs_intern(ctx, s, nsb, nslen) : XS_NULL;     /* step 10 */

    /* THE QUALIFIED NAME IS BUILT AT THE POOL'S CURRENT END, so `start` is taken in each arm IMMEDIATELY
       before the first byte of it: `generate a prefix` interns a name of its own, and a `start` captured
       before that would fold the generated prefix into the qualified name it precedes. */
    start = s->pool_len;
    if (s->inherited_ns == s->elem_ns) {                            /* step 11 */
        if (s->local_default != XS_NULL) s->ignore_nsdef = true;
        /* Steps 11.2-11.3: "If ns is the XML namespace, then append the concatenation of "xml:" and node's
           local name to qualified name." / "Otherwise, append node's local name to qualified name." The
           node's prefix, if it has one, is dropped either way. */
        if (s->elem_ns == s->ns_xml) xs_pool_put(ctx, s, "xml:", 4);
        xs_pool_put(ctx, s, loc, llen);
        s->qname = xs_pool_finish(ctx, s, start);
        xs_out_id(ctx, s, s->qname);
        return JS_STEP_YIELD;
    }

    {                                                               /* step 12 */
        uint32_t cand = xs_map_retrieve(s, s->elem_ns, pid);

        if (pid == s->s_xmlns) {
            /* "An Element with prefix 'xmlns' will not legally round-trip in a conforming XML parser." */
            if (s->require_well_formed)
                return xs_ill_formed(ctx, "DOM Parsing and Serialization §5.2.1.1 XML serializing an Element "
                                          "node step 12: an element whose prefix is `xmlns` will not legally "
                                          "round-trip through a conforming XML parser");
            cand = pid;
        }
        if (cand != XS_NULL) {
            /* "Found a suitable namespace prefix" — the element is written with a prefix an ancestor (or this
               element's own recording) already bound to its namespace. */
            xs_pool_put_id(ctx, s, cand);
            xs_pool_put(ctx, s, ":", 1);
            xs_pool_put(ctx, s, loc, llen);
            s->qname = xs_pool_finish(ctx, s, start);
            if (s->local_default != XS_NULL && s->local_default != s->ns_xml)
                s->inherited_ns = (s->local_default == s->s_empty) ? XS_NULL : s->local_default;
            xs_out_id(ctx, s, s->qname);
            return JS_STEP_YIELD;
        }
        if (pid != XS_NULL) {
            /* No declaration of this namespace is in scope, but the element HAS a prefix: declare that prefix
               here, renaming it first if this element's own attributes already gave it another meaning. */
            if (xs_local_get(s, pid) != XS_NULL) pid = xs_generate_prefix(ctx, s, s->elem_ns);
            xs_map_add(ctx, s, s->elem_ns, pid);
            start = s->pool_len;                 /* AFTER `generate a prefix` interned its own name */
            xs_pool_put_id(ctx, s, pid);
            xs_pool_put(ctx, s, ":", 1);
            xs_pool_put(ctx, s, loc, llen);
            s->qname = xs_pool_finish(ctx, s, start);
            xs_out_id(ctx, s, s->qname);
            xs_lit(ctx, s, " xmlns:");
            xs_out_id(ctx, s, pid);
            xs_lit(ctx, s, "=\"");
            if (!xs_attr_value(ctx, s, nsb, nslen, &err)) return err;
            xs_lit(ctx, s, "\"");
            if (s->local_default != XS_NULL)
                s->inherited_ns = (s->local_default == s->s_empty) ? XS_NULL : s->local_default;
            return JS_STEP_YIELD;
        }
        /* "Otherwise, if local default namespace is null, or local default namespace is not null and its value
           is not equal to ns" — the two clauses are kept apart because they are not the same test when BOTH are
           null: an element in no namespace whose attributes declare no default takes THIS arm (and emits
           `xmlns=""`), where a single inequality would send it to the one below. */
        if (s->local_default == XS_NULL || s->local_default != s->elem_ns) {
            s->ignore_nsdef = true;
            xs_pool_put(ctx, s, loc, llen);
            s->qname = xs_pool_finish(ctx, s, start);
            s->inherited_ns = s->elem_ns;
            xs_out_id(ctx, s, s->qname);
            xs_lit(ctx, s, " xmlns=\"");
            if (!xs_attr_value(ctx, s, nsb, nslen, &err)) return err;
            xs_lit(ctx, s, "\"");
            return JS_STEP_YIELD;
        }
        /* "Otherwise, the node has a local default namespace that matches ns." */
        xs_pool_put(ctx, s, loc, llen);
        s->qname = xs_pool_finish(ctx, s, start);
        s->inherited_ns = s->elem_ns;
        xs_out_id(ctx, s, s->qname);
        return JS_STEP_YIELD;
    }
}

/* ---- the leaf sub-algorithms ------------------------------------------------------------------------------ */

static int xs_leaf(JSContext *ctx, XmlSerializeState *s, lxb_dom_node_t *n)
{
    size_t len = 0;
    const char *d;

    switch (n->type) {
    case LXB_DOM_NODE_TYPE_COMMENT:                       /* §5.2.1.3 */
        d = xs_cdata(n, &len);
        if (s->require_well_formed &&
            (!xs_all_char(d, len) || xs_has_bytes(d, len, "--", 2) || (len && d[len - 1] == '-')))
            return xs_ill_formed(ctx, "DOM Parsing and Serialization §5.2.1.3 XML serializing a Comment node: "
                                      "the comment's data holds a character outside XML §2.2 Characters' [2] "
                                      "Char production, holds two adjacent hyphen-minus characters, or ends "
                                      "with one, so its serialization would not be well-formed");
        xs_lit(ctx, s, "<!--");
        xs_out(ctx, s, d, len);
        xs_lit(ctx, s, "-->");
        return JS_STEP_YIELD;

    case LXB_DOM_NODE_TYPE_CDATA_SECTION:                 /* §5.2.1.4 */
        /* §5.2.1.4 XML serializing a CDATASection node, whose whole algorithm is "Let markup be the
           concatenation of `<![CDATA[`, node's data, and `]]>`" — the arm this file wrote before the standard
           had one, now met by the standard rather than diverging from it (head comment, (3)).
           THE THROW IS THIS FILE'S AND NOT §5.2.1.4'S: the section states no require-well-formed step at all,
           so data holding `]]>` would be concatenated into markup that closes the section early and re-parses
           as a DIFFERENT TREE. That is the same self-contradiction the head comment's surviving divergence
           names, and refusing is the only answer that keeps `require well-formed` meaning what it says. */
        d = xs_cdata(n, &len);
        if (s->require_well_formed && (!xs_all_char(d, len) || xs_has_bytes(d, len, "]]>", 3)))
            return xs_ill_formed(ctx, "DOM Parsing and Serialization §5.2.1.4 XML serializing a CDATASection "
                                      "node: the section's data holds a character outside "
                                      "XML §2.2 Characters' [2] Char production, or holds the CDEnd string "
                                      "`]]>`, which [20] CData excludes — so it cannot be written as a CDATA "
                                      "section at all");
        xs_lit(ctx, s, "<![CDATA[");
        xs_out(ctx, s, d, len);
        xs_lit(ctx, s, "]]>");
        return JS_STEP_YIELD;

    case LXB_DOM_NODE_TYPE_TEXT:                          /* §5.2.1.5 */
        d = xs_cdata(n, &len);
        if (s->require_well_formed && !xs_all_char(d, len))
            return xs_ill_formed(ctx, "DOM Parsing and Serialization §5.2.1.5 XML serializing a Text node: the "
                                      "node's data holds a character that is not matched by XML §2.2 "
                                      "Characters' [2] Char production, so its serialization would not be "
                                      "well-formed");
        xs_text_escaped(ctx, s, d, len);
        return JS_STEP_YIELD;

    case LXB_DOM_NODE_TYPE_PROCESSING_INSTRUCTION: {      /* §5.2.1.8 */
        lxb_dom_processing_instruction_t *pi = lxb_dom_interface_processing_instruction(n);
        size_t tlen = pi->target.length;
        const char *t = pi->target.data ? (const char *)pi->target.data : "";

        d = xs_cdata(n, &len);
        if (s->require_well_formed &&
            (xs_has_byte(t, tlen, ':') || (tlen == 3 && (t[0] | 0x20) == 'x' && (t[1] | 0x20) == 'm' &&
                                           (t[2] | 0x20) == 'l')))
            return xs_ill_formed(ctx, "DOM Parsing and Serialization §5.2.1.8 XML serializing a "
                                      "ProcessingInstruction node: the target holds a colon or is an ASCII "
                                      "case-insensitive match for `xml`, so its serialization would not be "
                                      "well-formed");
        if (s->require_well_formed && (!xs_all_char(d, len) || xs_has_bytes(d, len, "?>", 2)))
            return xs_ill_formed(ctx, "DOM Parsing and Serialization §5.2.1.8 XML serializing a "
                                      "ProcessingInstruction node: the data holds a character outside XML "
                                      "§2.2 Characters' [2] Char production or holds the string `?>`, so its "
                                      "serialization would not be well-formed");
        xs_lit(ctx, s, "<?");
        xs_out(ctx, s, t, tlen);
        xs_lit(ctx, s, " ");
        xs_out(ctx, s, d, len);
        xs_lit(ctx, s, "?>");
        return JS_STEP_YIELD;
    }

    case LXB_DOM_NODE_TYPE_DOCUMENT_TYPE: {               /* §5.2.1.7 */
        lxb_dom_document_type_t *dt = lxb_dom_interface_document_type(n);
        size_t nlen = 0, publen = dt->public_id.length, syslen = dt->system_id.length;
        const char *nm = (const char *)lxb_dom_document_type_name(dt, &nlen);
        const char *pub = dt->public_id.data ? (const char *)dt->public_id.data : "";
        const char *sys = dt->system_id.data ? (const char *)dt->system_id.data : "";

        if (s->require_well_formed && !xs_all_pubid(pub, publen))
            return xs_ill_formed(ctx, "DOM Parsing and Serialization §5.2.1.7 XML serializing a DocumentType "
                                      "node: the publicId holds a character that is not matched by XML §2.3 "
                                      "Common Syntactic Constructs' [13] PubidChar production, so this "
                                      "document type declaration would not be well-formed");
        if (s->require_well_formed &&
            (!xs_all_char(sys, syslen) ||
             (xs_has_byte(sys, syslen, '"') && xs_has_byte(sys, syslen, '\''))))
            return xs_ill_formed(ctx, "DOM Parsing and Serialization §5.2.1.7 XML serializing a DocumentType "
                                      "node: the systemId holds a character outside XML §2.2 Characters' [2] "
                                      "Char production, or holds both a quotation mark and an apostrophe, so "
                                      "there is no delimiter this literal can be written with");
        xs_lit(ctx, s, "<!DOCTYPE ");
        xs_out(ctx, s, nm ? nm : "", nm ? nlen : 0);
        if (publen) {
            char q = xs_id_quote(pub, publen);
            xs_lit(ctx, s, " PUBLIC ");
            xs_out(ctx, s, &q, 1);
            xs_out(ctx, s, pub, publen);
            xs_out(ctx, s, &q, 1);
        }
        if (syslen && !publen) xs_lit(ctx, s, " SYSTEM");
        if (syslen) {
            char q = xs_id_quote(sys, syslen);
            xs_lit(ctx, s, " ");
            xs_out(ctx, s, &q, 1);
            xs_out(ctx, s, sys, syslen);
            xs_out(ctx, s, &q, 1);
        }
        xs_lit(ctx, s, ">");
        return JS_STEP_YIELD;
    }

    default:
        DFAIL("the XML serializer reached its leaf step on a node kind DOM Parsing and Serialization §5.2.1's "
              "dispatch does not name — the dispatch is what routes here and it has an arm for every node kind "
              "the DOM defines, so this is a node this engine created some other way");
    }
    return JS_STEP_ABRUPT;
}

/* ---- the machine ------------------------------------------------------------------------------------------ */

void xml_serialize_start(JSContext *ctx, JSStepHdr *hdr, XmlSerializeState *s, lxb_dom_node_t *node,
                         XmlSerializeEntry entry, bool require_well_formed, int base, int after)
{
    DCHECK(node != NULL, "the XML serialization algorithm was started on no node");
    DCHECK(entry == XML_SERIALIZE_NODE || entry == XML_SERIALIZE_CHILDREN,
           "the XML serialization algorithm was started at an entry it does not have — the two are the "
           "interface dispatch of §5.2.1 step 5 and the child concatenation of §5.2.1.6, and a caller with a "
           "third shape in mind is asking for a walk this algorithm does not define");
    DCHECK(s->cur == NULL, "a second XML serialization was started while one was still walking — the state "
                           "holds ONE walk, and the caller resumes at its `after` stage with the markup on it");
    s->cur = node;
    s->limit = NULL;
    s->require_well_formed = require_well_formed;
    s->after = after;

    /* §5.2.1 step 1: "Let namespace be null." */
    s->ctx_ns = XS_NULL;
    /* The constants every comparison in this file is against, interned before anything reads one. */
    s->s_empty = xs_intern(ctx, s, "", 0);
    s->s_xml   = xs_intern(ctx, s, "xml", 3);
    s->s_xmlns = xs_intern(ctx, s, "xmlns", 5);
    s->ns_html  = xs_intern(ctx, s, XS_NS_HTML, strlen(XS_NS_HTML));
    s->ns_xml   = xs_intern(ctx, s, XS_NS_XML, strlen(XS_NS_XML));
    s->ns_xmlns = xs_intern(ctx, s, XS_NS_XMLNS, strlen(XS_NS_XMLNS));
    /* Steps 2-3: "Let prefix map be «» (a namespace prefix map)." / "Add "xml" to prefix map given the XML
       namespace." That one entry is why §5.2.1.1 step 12 can always answer `xml:` for an element in the XML
       namespace without any declaration being in scope. */
    xs_map_add(ctx, s, s->ns_xml, s->s_xml);
    /* Step 4: "Let prefix index be 1." */
    s->prefix_index = 1;

    if (entry == XML_SERIALIZE_CHILDREN) {
        /* §5.2.1.6's concatenation over `node`'s children, with §5.2.1.1 step 18's substitution asked at the
           one place both descents ask it. The context namespace stays null across the descent, which is what
           puts a default namespace declaration on the FIRST CHILD rather than on a container whose tags are
           not in the output — web-platform-tests `domparsing/innerhtml-03.xhtml`'s first assertion. */
        hdr->stage = base + (xs_enter_children(ctx, s, xs_children_container(node, xs_ns_id(ctx, s, node),
                                                                            s->ns_html))
                             ? XS_PHASE_DISPATCH : XS_PHASE_NEXT);
        return;
    }
    hdr->stage = base + XS_PHASE_DISPATCH;
}

JSValue xml_serialize_result(JSContext *ctx, const XmlSerializeState *s)
{
    return JS_NewStringLen(ctx, s->out ? s->out : "", s->out_len);
}

int xml_serialize_run(JSContext *ctx, JSStepHdr *hdr, XmlSerializeState *s, int base)
{
    int phase = hdr->stage - base;

    DCHECK(phase >= 0 && phase < XS_PHASE_N,
           "the XML serialization algorithm was resumed at a stage outside the block its caller declared");

    if (phase == XS_PHASE_DISPATCH) {
        lxb_dom_node_t *n = s->cur;

        DCHECK(n != NULL, "the XML serialization algorithm's dispatch was reached with no node");
        switch (n->type) {
        case LXB_DOM_NODE_TYPE_ELEMENT: {
            /* §5.2.1.1 steps 1-7. */
            lxb_dom_element_t *el = lxb_dom_interface_element(n);
            size_t llen = 0;
            const char *loc = (const char *)lxb_dom_element_local_name(el, &llen);

            DCHECK(loc != NULL, "an element in the tree has no local name");
            if (s->require_well_formed && (xs_has_byte(loc, llen, ':') || !xml_name_is_name(loc, llen)))
                return xs_ill_formed(ctx, "DOM Parsing and Serialization §5.2.1.1 XML serializing an Element "
                                          "node step 1: this element's local name holds a colon or is not an "
                                          "XML §2.3 Common Syntactic Constructs [5] Name, so its "
                                          "serialization would not be a well-formed element");
            xs_lit(ctx, s, "<");                                    /* step 2 */
            s->qname = XS_NULL;                                     /* step 3 */
            s->skip_end_tag = false;                                /* step 4 */
            s->ignore_nsdef = false;                                /* step 5 */
            /* Step 6's copy, and step 7's empty local prefixes map. The copy is the LEVEL — see the level
               stack in xml_serialize.h — so it is pushed here, before anything can add to the map. */
            xs_push(ctx, s, n);
            s->local_n = 0;
            s->lnset_n = 0;
            s->local_default = XS_NULL;                             /* §5.2.1.1.1's `default namespace attr value` */
            s->attr = el->first_attr;                               /* step 8 */
            STEP_GOTO(hdr->stage, base + XS_PHASE_RECORD, NULL);
            return JS_STEP_YIELD;
        }

        case LXB_DOM_NODE_TYPE_DOCUMENT:
        case LXB_DOM_NODE_TYPE_DOCUMENT_FRAGMENT:
        case LXB_DOM_NODE_TYPE_SHADOW_ROOT:
            /* §5.2.1.2's require-well-formed check, which is the ONE thing it has that §5.2.1.6 does not:
               "this node has no documentElement". Everything after it is the same walk over the children, so
               the two algorithms share the arm rather than being one arm falling into another. */
            if (n->type == LXB_DOM_NODE_TYPE_DOCUMENT && s->require_well_formed) {
                lxb_dom_node_t *c;

                for (c = n->first_child; c; c = c->next)
                    if (c->type == LXB_DOM_NODE_TYPE_ELEMENT) break;
                if (!c)
                    return xs_ill_formed(ctx, "DOM Parsing and Serialization §5.2.1.2 XML serializing a "
                                              "Document node: the document has no documentElement, so its "
                                              "serialization would not be a well-formed document");
            }
            /* §5.2.1.2 / §5.2.1.6's descent — xs_enter_children. A Document and a DocumentFragment are never
               `template` elements, so the container is the node itself and no substitution is asked for. */
            STEP_GOTO(hdr->stage, base + (xs_enter_children(ctx, s, n) ? XS_PHASE_DISPATCH : XS_PHASE_NEXT),
                      NULL);
            return JS_STEP_YIELD;

        case LXB_DOM_NODE_TYPE_ATTRIBUTE:
            /* "An Attr object: Return an empty string." DOM §4.9.2 Interface Attr makes an Attr a Node, so it
               can only ever be the node the algorithm was ENTERED on — nothing in a tree has one as a child. */
            DCHECK(s->sp == 0 && s->limit == NULL,
                   "the XML serialization algorithm reached an Attr inside a tree walk — an Attr is not a "
                   "child of anything, so it can only be the node the algorithm was entered on");
            STEP_GOTO(hdr->stage, s->after, NULL);
            return JS_STEP_YIELD;

        case LXB_DOM_NODE_TYPE_COMMENT:
        case LXB_DOM_NODE_TYPE_TEXT:
        case LXB_DOM_NODE_TYPE_CDATA_SECTION:
        case LXB_DOM_NODE_TYPE_PROCESSING_INSTRUCTION:
        case LXB_DOM_NODE_TYPE_DOCUMENT_TYPE:
            STEP_GOTO(hdr->stage, base + XS_PHASE_LEAF, NULL);
            return JS_STEP_YIELD;

        default:
            /* §5.2.1's "Anything else: Throw a TypeError" arm. Every node kind the DOM defines is named above,
               and Web IDL §3.2.15's brand on the member's `Node root` argument is what keeps a non-Node out —
               so what reaches here is one of lexbor's legacy DTD node kinds, which no DOM member creates. */
            DFAIL("the XML serialization algorithm was handed a node kind the DOM does not define — the "
                  "declared `Node` argument brands out everything that is not a node, and every node kind the "
                  "DOM has is dispatched above");
        }
        return JS_STEP_ABRUPT;
    }

    if (phase == XS_PHASE_RECORD) {
        /* §5.2.1.1.1's per-attribute loop, ONE attribute. The cursor advances before the body runs, so every
           exit of `xs_record_one` — including the three the standard spells "continue" — leaves the walk on
           the next one. */
        lxb_dom_attr_t *a = s->attr;

        if (!a) {
            STEP_GOTO(hdr->stage, base + XS_PHASE_NAME, NULL);
            return JS_STEP_YIELD;
        }
        s->attr = a->next;
        xs_record_one(ctx, s, a);
        return JS_STEP_YIELD;
    }

    if (phase == XS_PHASE_NAME) {
        lxb_dom_node_t *n = s->cur;
        int r;

        DCHECK(n != NULL && n->type == LXB_DOM_NODE_TYPE_ELEMENT,
               "§5.2.1.1 steps 9-12 were reached on something that is not an element");
        r = xs_element_name(ctx, s, lxb_dom_interface_element(n));
        if (r != JS_STEP_YIELD) return r;
        s->attr = lxb_dom_interface_element(n)->first_attr;          /* step 13's §5.2.1.1.3 step 3 */
        STEP_GOTO(hdr->stage, base + XS_PHASE_ATTRS, NULL);
        return JS_STEP_YIELD;
    }

    if (phase == XS_PHASE_ATTRS) {
        lxb_dom_attr_t *a = s->attr;

        if (!a) {
            STEP_GOTO(hdr->stage, base + XS_PHASE_OPEN, NULL);
            return JS_STEP_YIELD;
        }
        s->attr = a->next;
        return xs_attr_one(ctx, s, a);
    }

    if (phase == XS_PHASE_OPEN) {
        lxb_dom_node_t *n = s->cur, *container;
        lxb_dom_element_t *el = lxb_dom_interface_element(n);
        size_t llen = 0;
        const char *loc = (const char *)lxb_dom_element_local_name(el, &llen);
        bool no_children = n->first_child == NULL;

        DCHECK(loc != NULL, "an element in the tree has no local name");
        if (s->elem_ns == s->ns_html && no_children) {               /* step 14 */
            int i;

            for (i = 0; XS_VOID_ELEMENTS[i]; i++)
                if (llen == strlen(XS_VOID_ELEMENTS[i]) && memcmp(loc, XS_VOID_ELEMENTS[i], llen) == 0) {
                    xs_lit(ctx, s, " /");
                    s->skip_end_tag = true;
                    break;
                }
        }
        if (s->elem_ns != s->ns_html && no_children) {               /* step 15 */
            xs_lit(ctx, s, "/");
            s->skip_end_tag = true;
        }
        xs_lit(ctx, s, ">");                                         /* step 16 */
        if (s->skip_end_tag) {                                       /* step 17: the node is a leaf node */
            XmlSerLevel lv;

            DCHECK(s->sp > 0, "§5.2.1.1 step 17 was reached with no level for the element it is returning from");
            lv = s->stack[--s->sp];
            s->limit = lv.limit;
            s->ctx_ns = lv.ctx_ns;
            s->map_n = lv.map_n;
            STEP_GOTO(hdr->stage, base + XS_PHASE_NEXT, NULL);
            return JS_STEP_YIELD;
        }
        /* Step 18's template substitution, asked through xs_children_container so that the entry which never
           reaches this step asks the SAME question — see that function. */
        container = xs_children_container(n, s->elem_ns, s->ns_html);
        s->stack[s->sp - 1].qname = s->qname;                        /* step 20's end tag, held for the pop */
        s->limit = container;
        s->ctx_ns = s->inherited_ns;
        s->cur = container->first_child;                             /* step 19 */
        STEP_GOTO(hdr->stage, base + (s->cur ? XS_PHASE_DISPATCH : XS_PHASE_NEXT), NULL);
        return JS_STEP_YIELD;
    }

    if (phase == XS_PHASE_LEAF) {
        int r = xs_leaf(ctx, s, s->cur);

        if (r != JS_STEP_YIELD) return r;
        STEP_GOTO(hdr->stage, base + XS_PHASE_NEXT, NULL);
        return JS_STEP_YIELD;
    }

    if (phase == XS_PHASE_NEXT) {
        XmlSerLevel lv;

        if (s->limit != NULL) {
            if (s->cur && s->cur->next) {
                s->cur = s->cur->next;
                STEP_GOTO(hdr->stage, base + XS_PHASE_DISPATCH, NULL);
                return JS_STEP_YIELD;
            }
            DCHECK(s->sp > 0, "the XML serializer exhausted a container's children with no level to leave — "
                              "a level is pushed for every container the walk descends into");
            lv = s->stack[--s->sp];
            s->limit = lv.limit;
            s->ctx_ns = lv.ctx_ns;
            DCHECK(lv.map_n <= s->map_n,
                   "the namespace prefix map is SHORTER than it was when this element copied it — §5.2.1.1.2's "
                   "only mutation is `add a prefix`, which appends, and the copy is that append point");
            s->map_n = lv.map_n;
            if (lv.node == NULL) {                                   /* §5.2.1.2 / §5.2.1.6's root container */
                STEP_GOTO(hdr->stage, s->after, NULL);
                return JS_STEP_YIELD;
            }
            s->cur = lv.node;
            s->qname = lv.qname;
            STEP_GOTO(hdr->stage, base + XS_PHASE_CLOSE, NULL);
            return JS_STEP_YIELD;
        }
        /* No container: the algorithm was entered on this node and its markup is complete. */
        DCHECK(s->sp == 0, "the XML serializer finished its root node with levels still on the stack");
        STEP_GOTO(hdr->stage, s->after, NULL);
        return JS_STEP_YIELD;
    }

    DCHECK(phase == XS_PHASE_CLOSE, "the XML serializer resumed into a stage §5.2.1 does not have");
    /* §5.2.1.1 step 20. */
    DCHECK(s->qname != XS_NULL, "§5.2.1.1 step 20 has no qualified name to close with — step 12 is what sets "
                                "it and the walk only descends after that");
    xs_lit(ctx, s, "</");
    xs_out_id(ctx, s, s->qname);
    xs_lit(ctx, s, ">");
    STEP_GOTO(hdr->stage, base + XS_PHASE_NEXT, NULL);
    return JS_STEP_YIELD;
}

void xml_serialize_visit_state(JSContext *ctx, XmlSerializeState *s, JSStepVisit *v)
{
    /* Every one of these is plain storage a forked arm must not share: two arms append their own remaining
       nodes to their own markup, intern their own names and unwind their own level stack. The DOM pointers
       inside are per-flow COW nodes, which every arm reaches by the same address. */
    v->buf(ctx, (void **)&s->out, s->out_cap);
    v->buf(ctx, (void **)&s->pool, s->pool_cap);
    v->buf(ctx, (void **)&s->pool_ent, sizeof(uint32_t) * 2 * (size_t)s->pool_ecap);
    v->buf(ctx, (void **)&s->map, sizeof(XmlSerPair) * (size_t)s->map_cap);
    v->buf(ctx, (void **)&s->local, sizeof(XmlSerPair) * (size_t)s->local_cap);
    v->buf(ctx, (void **)&s->lnset, sizeof(XmlSerPair) * (size_t)s->lnset_cap);
    v->buf(ctx, (void **)&s->stack, sizeof(XmlSerLevel) * (size_t)s->scap);
}
