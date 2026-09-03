/* The author presentational hint origin — see css_presentational_hints.h for why it is an origin. */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "quickjs.h"
#include "core/css/css_presentational_hints.h"
#include "core/css/css_shorthand.h"   /* a hint declares the property HTML writes, which may be a shorthand */
#include "core/dom/document.h"
#include "core/dom/node.h"
#include "core/frame/window_proxy.h"
#include "core/html/enumerated_attribute.h"   /* the ASCII match HTML §15.3.8's rules are stated over */

static char *hint_strdup(const char *s)
{
    char *out = strdup(s);

    CHECK(out != NULL, "cssom: OOM building a presentational hint's value — a dropped one reads as the property "
                       "being undeclared by this origin, which for a body margin is the initial value 0 and not "
                       "the 8px HTML §15.3.2 gives a page with no margin attributes at all");
    return out;
}

/* THE DECLARATION `property: value` AS THIS ORIGIN'S ANSWER FOR THE LONGHAND `name`, or NULL where that
   declaration does not set it. NULL is a real answer here and the common one — a hint sets one property and
   the cascade asks this component about every property it resolves.
   THE PROPERTY IS THE ONE HTML'S RENDERING SECTION WRITES, WHICH IS ROUTINELY A SHORTHAND, AND THE CASCADE IS
   OVER LONGHANDS ONLY. `cssom_cascaded_value` asserts that, so a row spelling a shorthand would be a
   declaration no read can ever reach — the write-with-no-reader shape, silent because the property still has
   an initial value to answer with. The expansion is therefore DERIVED from the component that owns each
   shorthand's grammar (core/css/css_shorthand.h) rather than restated here as a list of longhand names: a
   second copy of `text-align: center` sets `text-align-all` AND resets `text-align-last` would be one fact
   with two sources, and the one that drifts is the copy no grammar runs against.
   AN EXPANSION THAT ANSWERS NOTHING FOR A LONGHAND THE SHORTHAND SETS IS THIS FILE'S OWN BUG AND CRASHES.
   `css_shorthand_component` returns NULL for a value its grammar rejects, which for an AUTHOR declaration is
   css-cascade-5 §6's dropped declaration — but every value reaching here is one this file typed out of a
   standard, so a rejection means the ROW is wrong and the hint silently declares nothing. */
static char *hint_declaration(const char *property, const char *value, const char *name)
{
    const char *const *longhands;
    unsigned n = 0, i;
    char *out;

    if (strcmp(property, name) == 0) return hint_strdup(value);
    longhands = css_shorthand_longhands(property, &n);
    if (longhands == NULL) return NULL;   /* a longhand row that is simply not the property being asked for */
    for (i = 0; i < n; i++)
        if (strcmp(longhands[i], name) == 0) break;
    if (i == n) return NULL;
    out = css_shorthand_component(property, value, name);
    DCHECKF(out != NULL,
            "a presentational hint row declares `%s: %s`, which is a shorthand of `%s`, and the shorthand's own "
            "grammar rejects that value — so this origin declares NOTHING for a property HTML's rendering "
            "section says it declares, and the cascade answers the initial value with nothing to say the hint "
            "was never expanded. The ROW is what is wrong: fix its value to one the property's grammar admits",
            property, value, name);
    return out;
}

/* HTML's RULES FOR PARSING NON-NEGATIVE INTEGERS, which §15.2's "maps to the pixel length property" is stated
   over: the rules for parsing integers, and then an error for a negative result. The integer rules skip leading
   ASCII whitespace, accept one optional sign, require an ASCII DIGIT next, and then "collect a sequence of code
   points that are ASCII digits" — so TRAILING CONTENT IS NOT AN ERROR and `marginwidth="10px"` is ten.
   WHAT COMES BACK IS THE DIGIT RUN, not a number, and that is deliberate: "interpret the resulting sequence as
   a base-ten integer" has no upper bound, so accumulating into a C integer would need a range check, and a
   range check that rejects is a CAP on what a page may write. The digits ARE the value — the property this maps
   to is a `<length>`, and the caller writes them straight into one. Leading zeros are dropped so the specified
   value serializes canonically; `*plen` is at least 1 on success. */
static bool html_parse_non_negative_integer(const lxb_char_t *s, size_t n, const lxb_char_t **pd, size_t *plen)
{
    size_t i = 0, start;

    while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\f' || s[i] == '\r')) i++;
    if (i == n) return false;
    if (s[i] == '-') return false;   /* "if value is less than zero, return an error" */
    if (s[i] == '+') i++;
    if (i == n || s[i] < '0' || s[i] > '9') return false;
    start = i;
    while (i < n && s[i] >= '0' && s[i] <= '9') i++;
    while (start + 1 < i && s[start] == '0') start++;
    *pd = s + start;
    *plen = i - start;
    return true;
}

/* §15.3.2's TABLE, entire. Its two rows each name a pair of properties and the attributes that source them, IN
   THE ORDER "the first attribute that exists" reads them. The table's THIRD source is the same ATTRIBUTE NAME
   as the first — `marginheight` for the margins of the block axis, `marginwidth` for the inline one — read off
   a different ELEMENT, the body's container frame element, which is why there is no third column here and why
   the third source is a question about a NAVIGABLE rather than about a name. */
static const struct {
    const char *properties[2];
    const char *own;       /* the body element's own attribute, and the container frame element's */
    const char *legacy;    /* the body element's legacy alias for it */
} PAGE_MARGIN[] = {
    { { "margin-top",  "margin-bottom" }, "marginheight", "topmargin"  },
    { { "margin-left", "margin-right"  }, "marginwidth",  "leftmargin" },
};

/* "If none of the attributes for a property are found, or if the value of the attribute that was found cannot
   be parsed successfully, then a default value of 8px is expected to be used for that property instead." It is
   the DEFAULT OF THE HINT and not a UA rule, and the difference is visible in both directions: an author
   `body { margin: 0 }` overrides it because the author origin outranks this one, and `<body marginheight=20>`
   replaces it because it is the same origin's own first source. A UA-origin row would lose the first and ignore
   the second. IT IS ALSO THE 16px that makes `document.body`'s used width 1264 in a 1280 viewport. */
static const char PAGE_MARGIN_DEFAULT[] = "8px";

/* IS THIS DOCUMENT'S NODE NAVIGABLE A CHILD NAVIGABLE — §15.3.2's own condition for there being a container
   frame element at all ("if the body element's node document's node navigable is a child navigable, and the
   container of that navigable is a frame or iframe element"). It is asked of the realm whose ACTIVE document
   this is, which is NULL for a document that has no navigable (a `createHTMLDocument`, a DOMParser parse, an
   XHR `responseXML`), and those have no container by definition. */
static bool document_is_child_navigable(const lxb_dom_node_t *doc)
{
    JSContext *ctx = document_active_realm_of(doc);
    JSValue parent;
    bool child;

    if (!ctx) return false;
    /* A realm whose navigable has no WindowProxy yet is one nothing has asked a navigable question of, and a
       navigable with no proxy has no parent to be a child of. */
    if (!JS_IsObject(document_window_proxy(ctx))) return false;
    parent = window_proxy_parent_navigable(ctx, document_window_proxy(ctx));
    child = !JS_IsUndefined(parent);
    JS_FreeValue(ctx, parent);
    return child;
}

/* HTML §15.3.2 "The page"'s CONTAINER FRAME ELEMENT, in the section's own two conjuncts: "if the body
   element's node document's node navigable is a CHILD NAVIGABLE, and the container of that navigable is a
   FRAME OR IFRAME element, then the container frame element of the body element is that frame or iframe
   element. Otherwise, there is no container frame element."
   THE SECOND CONJUNCT IS NOT DECORATION. §7.3.1.3's navigable containers are `iframe`, `object`, `embed` and
   `fencedframe`, and §15.3.2 names two of them — so a `<body marginheight=…>`-less document inside an
   `<object>` has NO container frame element and takes the 8px default, while the same document inside an
   `<iframe marginheight=20>` takes 20. A test that only asked whether the navigable has a container would
   read an `object`'s attribute that HTML never routes here.
   NULL IS TWO DIFFERENT ANSWERS AND ONLY ONE OF THEM IS A GAP. There is genuinely no container frame element
   for a top-level traversable and for a container HTML does not name, and §15.3.2 states the default for
   exactly those. A container that EXISTS and cannot be reached is the third thing, and it crashes: §7.3.1.3
   gives every child navigable a container, so a child navigable whose container reads back null is one whose
   container element lives in another agent — the cross-origin case §15.3.2's own note says is intentional
   ("a page can change the margins of another page, including one from another origin"). */
static lxb_dom_element_t *hint_container_frame_element(const lxb_dom_node_t *doc)
{
    JSContext *ctx;
    JSValue container;
    lxb_dom_node_t *n;
    const lxb_char_t *tag;
    size_t taglen = 0;

    if (!document_is_child_navigable(doc)) return NULL;
    ctx = document_active_realm_of(doc);
    DCHECK(ctx != NULL && JS_IsObject(document_window_proxy(ctx)),
           "§15.3.2's container-frame walk found a document that IS a child navigable and has no realm or no "
           "WindowProxy — the predicate above establishes both before answering true, so the two tests have "
           "come apart");
    container = window_proxy_container(ctx, document_window_proxy(ctx));
    if (JS_IsNull(container)) {
        JS_FreeValue(ctx, container);
        DFAIL("HTML §15.3.2 \"The page\"'s THIRD SOURCE for a body margin is the body element's CONTAINER "
              "FRAME ELEMENT's `marginheight`/`marginwidth` attribute, this document's navigable IS a child "
              "navigable — so §7.3.1.3 \"Child navigables\" gives it a container — and that container reads "
              "back NULL. THE REVERSE EDGE IS NOT WHAT IS MISSING: `window_proxy_container` answers it, "
              "recorded by create-a-new-child-navigable and confirmed against the element's own content "
              "navigable so it is per-flow. What is missing is a container in ANOTHER AGENT: a cross-origin "
              "parent holds the `iframe` element in a different WASM instance, so there is no wrapper in this "
              "heap to read an attribute off. HTML §15.3.2's own note says that read is intentional rather "
              "than an oversight — \"a page can change the margins of another page (including one from another "
              "origin)\" — and answering 8px here is a REAL NUMBER for a page whose embedder wrote "
              "`<iframe marginheight=20>`, with nothing to say the attribute was never read. BUILD the "
              "SUSPEND-AT-THE-BOUNDARY read (CLAUDE.md §Security: a synchronous cross-instance read is a "
              "suspend point) and ask the peer for the container's attribute");
        return NULL;
    }
    n = node_of(container);
    JS_FreeValue(ctx, container);
    DCHECK(n != NULL && n->type == LXB_DOM_NODE_TYPE_ELEMENT,
           "§7.3.1.3's container of a navigable is a NAVIGABLE CONTAINER ELEMENT, and the wrapper this "
           "navigable holds is not an element — the create that recorded it is handed the element itself, so "
           "anything else is a slot written by something that is not that algorithm");
    /* The node is owned by the parent document's tree, not by the wrapper, so it outlives the reference freed
       above — an element that is a navigable's container is by definition still in that tree. */
    tag = lxb_dom_element_local_name(lxb_dom_interface_element(n), &taglen);
    DCHECK(tag != NULL, "a navigable container element has no local name");
    if (taglen == 6 && memcmp(tag, "iframe", 6) == 0) return lxb_dom_interface_element(n);
    if (taglen == 5 && memcmp(tag, "frame", 5) == 0) return lxb_dom_interface_element(n);
    return NULL;
}

/* ---- HTML §15.3.8 Tables' `align` ATTRIBUTE ---------------------------------------------------------------
 *
 * HTML §15.3.8 Tables STATES THIS ONE MAPPING TWICE AND DIFFERENTLY, AND NEITHER HALF IS THE WHOLE OF IT.
 * Its hint block — "The following rules are also expected to apply, as presentational hints" — carries
 * `thead[align=absmiddle i], tbody[align=absmiddle i], … { text-align: center; }`, and `absmiddle` is the ONLY
 * value that block spells for a table part. The four prose paragraphs beneath it carry the other five, each in
 * the same shape: the six elements "are expected to center text within themselves, as if they had their
 * 'text-align' property set to 'center' in a presentational hint, and to align descendants to the center",
 * that paragraph being reached by "an align attribute whose value is an ASCII case-insensitive match for
 * either the string "center" or the string "middle"", and likewise "are expected to right-align text within
 * themselves, as if they had their 'text-align' property set to 'right' in a presentational hint" for `right`,
 * and the matching sentences for `left` and `justify`. Transcribing either half alone gives a page an answer
 * for some of the values it writes and the initial value for the rest, which is the same silent wrongness a
 * missing row is: `start`, with a real number to show for it.
 *
 * THE `p` AND `h1`–`h6` RULES ARE IN THIS SECTION AND NOT IN HTML §15.3.3 Flow content. That is surprising
 * and it is where the standard puts them — the same hint block states `p[align=left i], h1[align=left i], …`
 * beside the table ones, one rule per value — so they are transcribed here, from there. THEIR VALUE SET IS NOT THE
 * TABLE PARTS' and that is why there are two keyword tables rather than one list of tags: the block gives the
 * block-level elements `left`, `right`, `center` and `justify` and gives them no `middle` and no `absmiddle`,
 * while the table parts reach `center` by three spellings.
 *
 * THE NAMESPACE IS THE BLOCK'S OWN `@namespace "http://www.w3.org/1999/xhtml";`, so an element named `td` in
 * any other namespace takes no hint from here. It is asked because these rules are ATTRIBUTE-gated and a
 * foreign-namespace element can carry an `align` attribute — unlike the type-name rules of the UA sheet, where
 * a foreign element with a matching local name is the only way in.
 *
 * WHAT THIS DOES NOT COVER — a NAMED RESIDUAL, because the `text-align` half is right rather than unfinished:
 * each of those four paragraphs asks for a SECOND thing beside the hint, the centering one ending
 * "and to align descendants to the center" and the other three ending the same way naming their own side (the
 * `justify` paragraph's side is the left, not the justification). HTML §15.2 The CSS user agent style sheet and
 * presentational hints defines that second thing over USED VALUES, not as a declaration:
 * "When a user agent is to align descendants of a node, the user agent is expected
 * to align only those descendants that have both their 'margin-inline-start' and 'margin-inline-end'
 * properties computing to a value other than 'auto', that are over-constrained and that have one of those two
 * margins with a used value forced to a greater value, and that do not themselves have an applicable align
 * attribute", and "Aligned elements are expected to be aligned by having the used values of their margins on
 * the line-left and line-right sides be set accordingly". No cascade declaration can express that — the
 * condition is on a margin's USED value and the effect is on two other used values — so there is nothing here
 * to be wrong, only a rule that lives one layer down. AFTER THE NEXT DIFF the used-value margin resolution
 * must, for a block-level box whose two inline margins both compute to non-`auto` and whose over-constraint
 * was resolved by forcing one of them larger, take that pair from the nearest ancestor carrying an applicable
 * `align` attribute (the most deeply nested such ancestor winning) instead of from CSS 2.1 §10.3.3's own
 * over-constraint rule. ITS ABSENCE SHOWS as a fixed-width `<div>` with two zero margins inside a
 * `<td align=center>`: this hint centres the TEXT in that div and the div itself stays at the line-left edge,
 * where a browser centres the box too — so the cell renders with centred prose in a left-hung box.
 */
typedef struct { const char *keyword; const char *value; } HintKeyword;

/* HTML §15.3.8 Tables' SIX TABLE-PART ELEMENTS, which are the selector list of its `[align=absmiddle i]`
   rule and the element list its four prose paragraphs repeat verbatim. `table`, `caption`, `col` and
   `colgroup` are NOT among them — `table[align=…]` is a different rule in the same block, mapping to
   `float` and `margin-inline` rather than to `text-align`, and no row here may answer for it. */
static const char *const ALIGN_TABLE_PART_TAGS[] = { "thead", "tbody", "tfoot", "tr", "td", "th", NULL };

/* HTML §15.3.8 Tables' BLOCK-LEVEL `align` ELEMENTS, the selector list its four `p[align=…], h1[align=…], …`
   rules share. */
static const char *const ALIGN_BLOCK_TAGS[] = { "p", "h1", "h2", "h3", "h4", "h5", "h6", NULL };

/* THREE SPELLINGS REACH `center` FOR A TABLE PART and they come from two places, which is the whole reason
   this table exists rather than a transcription of the CSS block: `absmiddle` is the block's, and `center` and
   `middle` are the prose's "either the string "center" or the string "middle"". */
static const HintKeyword ALIGN_TABLE_PART_VALUES[] = {
    { "center", "center" }, { "middle", "center" }, { "absmiddle", "center" },
    { "left", "left" }, { "right", "right" }, { "justify", "justify" },
    { NULL, NULL }
};

static const HintKeyword ALIGN_BLOCK_VALUES[] = {
    { "left", "left" }, { "right", "right" }, { "center", "center" }, { "justify", "justify" },
    { NULL, NULL }
};

/* The two rule sets, each an element list crossed with a keyword table. `property` is the one HTML §15.3.8
   writes — `text-align`, which css-text-4 §7.1 "Text Alignment: the text-align shorthand" makes a SHORTHAND of
   `text-align-all` and `text-align-last`; hint_declaration is what turns that into the longhand the cascade
   asked for. */
static const struct {
    const char *const *tags;
    const HintKeyword *values;
    const char *property;
} ALIGN_HINT[] = {
    { ALIGN_TABLE_PART_TAGS, ALIGN_TABLE_PART_VALUES, "text-align" },
    { ALIGN_BLOCK_TAGS,      ALIGN_BLOCK_VALUES,      "text-align" },
};

char *css_presentational_hint(lxb_dom_element_t *el, const char *name)
{
    lxb_dom_node_t *node = lxb_dom_interface_node(el);
    const lxb_char_t *tag;
    size_t taglen = 0;
    unsigned i, k;

    DCHECK(el != NULL && name != NULL,
           "the presentational hint origin was asked to declare with no element or no property name");
    for (i = 0; i < sizeof(PAGE_MARGIN) / sizeof(PAGE_MARGIN[0]); i++) {
        lxb_dom_node_t *doc;

        if (strcmp(PAGE_MARGIN[i].properties[0], name) != 0 &&
            strcmp(PAGE_MARGIN[i].properties[1], name) != 0)
            continue;
        /* "given a body element" — §4.3.1's body element OF ITS DOCUMENT, which is what a page means by one: a
           `<body>` left detached, a second one an author wrote, and the `<frameset>` document_body_of also
           answers for are none of them that element. */
        tag = lxb_dom_element_local_name(el, &taglen);
        if (!tag || taglen != 4 || memcmp(tag, "body", 4) != 0) return NULL;
        if (!node->owner_document) return NULL;
        doc = lxb_dom_interface_node(node->owner_document);
        if (document_body_of(doc) != node) return NULL;
        /* §15.3.2's THREE SOURCES PER PROPERTY, in the table's own order: the body element's own attribute,
           its legacy spelling, and the body element's CONTAINER FRAME ELEMENT's attribute — which carries the
           first spelling again, on a different element. One loop, because "the FIRST attribute that exists
           maps to the pixel length property" is one rule over all three and a third source tested outside the
           loop would be that rule written twice.
           THE THIRD SOURCE IS RESOLVED ONLY WHEN IT IS REACHED, which is what keeps its cross-origin crash
           where the spec puts the read: a page inside a cross-origin frame that writes `<body marginheight=0>`
           has its answer from the first source and must never pay for a container it does not consult. */
        for (k = 0; k < 3; k++) {
            const char *attr = k == 1 ? PAGE_MARGIN[i].legacy : PAGE_MARGIN[i].own;
            lxb_dom_element_t *src = el;
            const lxb_char_t *v, *digits;
            size_t vlen = 0, dlen = 0;

            if (k == 2) {
                src = hint_container_frame_element(doc);
                /* "Otherwise, there is no container frame element" — so there is no third attribute to be the
                   first that exists, and §15.3.2's default is the answer. */
                if (src == NULL) break;
            }
            v = lxb_dom_element_get_attribute(src, (const lxb_char_t *)attr, strlen(attr), &vlen);
            if (!v) continue;
            /* "The FIRST attribute that exists maps to the pixel length property", so the search stops at the
               one that is PRESENT whether or not it parses, and an unparseable one falls to the default rather
               than on to the next source. */
            if (!html_parse_non_negative_integer(v, vlen, &digits, &dlen))
                return hint_strdup(PAGE_MARGIN_DEFAULT);
            {
                char *out = malloc(dlen + 3);

                CHECK(out != NULL, "cssom: OOM building a presentational hint's pixel length");
                memcpy(out, digits, dlen);
                memcpy(out + dlen, "px", 3);
                return out;
            }
        }
        /* "If none of the attributes for a property are found, or if the value of the attribute that was
           found cannot be parsed successfully, then a default value of 8px is expected to be used." */
        return hint_strdup(PAGE_MARGIN_DEFAULT);
    }
    /* HTML §15.3.8 Tables' `align` HINT. The `@namespace` line of the block these rules are in is asked first
       because they are attribute-gated: a `td` in another namespace can carry an `align` attribute, where a
       foreign element only ever meets a type-name UA rule by its local name. */
    if (node->ns != LXB_NS_HTML) return NULL;
    tag = lxb_dom_element_local_name(el, &taglen);
    if (tag == NULL) return NULL;
    {
        const lxb_char_t *v;
        size_t vlen = 0;

        /* Lexbor answers NULL both for an ABSENT attribute and for one present with no value, and HTML §15.3.8's
           rules do not tell those apart either: no keyword it names is the empty string, so `align=""` takes
           no hint by the same route an absent `align` does. */
        v = lxb_dom_element_get_attribute(el, (const lxb_char_t *)"align", 5, &vlen);
        if (v == NULL) return NULL;
        for (i = 0; i < sizeof(ALIGN_HINT) / sizeof(ALIGN_HINT[0]); i++) {
            const char *const *t;
            const HintKeyword *kw;

            for (t = ALIGN_HINT[i].tags; *t != NULL; t++)
                if (strlen(*t) == taglen && memcmp(*t, tag, taglen) == 0) break;
            if (*t == NULL) continue;
            for (kw = ALIGN_HINT[i].values; kw->keyword != NULL; kw++)
                /* Infra's ASCII CASE-INSENSITIVE MATCH, which is what both halves of HTML §15.3.8 are stated
                   over — the CSS block by Selectors' `i` flag and the prose by naming it outright. It is
                   HTML §2.3.3 Keywords and enumerated attributes' comparison because that is where this engine
                   writes it ONCE; a fold spelled out again here would be the hand-rolled `strcasecmp`-shaped
                   loop that component's own header records itself as having replaced in four places. */
                if (enumerated_attribute_keyword_match(kw->keyword, (const char *)v, vlen))
                    return hint_declaration(ALIGN_HINT[i].property, kw->value, name);
            /* HTML §15.3.8 states one rule per value and no fallback, so an `align` whose value it does not
               name is an element with no hint rather than one with a defaulted hint. The element cannot be
               in the other rule set — the two tag lists are disjoint — so the search is over. */
            return NULL;
        }
    }
    /* Every other hint HTML defines is a row this file does not have yet, and a property no row names is not
       declared by this origin — which is a different statement from being declared with a default. */
    return NULL;
}
