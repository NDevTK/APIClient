/* HTML §4.2.3 "The base element" — see html_base_element.h for why the frozen base URL is STORED and why the
   `target` half's walk-per-ask is right for that member and wrong for this one. */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "quickjs.h"
#include "solver/concolic.h"
#include "core/dom/attr_list.h"
#include "core/dom/document.h"
#include "core/dom/element.h"
#include "core/dom/node.h"
#include "core/frame/csp_source_list.h"
#include "core/frame/policy_container.h"
#include "core/html/html_base_element.h"
#include "core/idl_args.h"
#include "core/url/url.h"

/* §4.2.3's `href` SETTER — `[CEReactions, ReflectSetter] attribute USVString href`, which is the ORDINARY
   reflection setter (write the content attribute) even though the getter is not the ordinary getter. It is
   declared here rather than left to the reflection table because a member is installed from one place: half a
   member from the table and half from this file would be two answers to "what is `base.href`". */
static int g_id_set_href = -1;

static bool base_element_is(const lxb_dom_node_t *n)
{
    size_t len = 0;
    const lxb_char_t *name;

    /* THE NAMESPACE IS PART OF THE QUESTION. §4.2.3's element is the HTML `base`, and a `base` in another
       namespace is a different element with a different interface — the same test core/html/html_style_element.c
       makes for `<style>`, and for the same reason. */
    if (!n || n->type != LXB_DOM_NODE_TYPE_ELEMENT || n->ns != LXB_NS_HTML) return false;
    name = lxb_dom_element_local_name(lxb_dom_interface_element((lxb_dom_node_t *)n), &len);
    return name && len == 4 && memcmp(name, "base", 4) == 0;
}

/* "A base element … that HAS AN href content attribute" — a PRESENCE question about §4.9's attribute list, not
   about the value being non-empty: `<base href="">` is a base element with an href and freezes to the fallback
   base URL stripped of its fragment, which is a different answer from having no base element at all. */
static bool base_with_href(const lxb_dom_node_t *n)
{
    return base_element_is(n) &&
           dom_attr_get_ns(lxb_dom_interface_element((lxb_dom_node_t *)n), NULL, "href") != NULL;
}

/* §2.4.3's "the FIRST base element in document that has an href attribute, IN TREE ORDER" — the walk starts at
   the DOCUMENT node and not at the document element, because that is what "in document" means and a parser can
   place a `<base>` outside `<head>`. It is the LIGHT tree: a `<base>` inside a shadow root is not in the
   document tree, so it is not this document's base element. */
static lxb_dom_element_t *first_base_with_href(lxb_dom_document_t *doc)
{
    lxb_dom_node_t *root = lxb_dom_interface_node(doc), *n;

    for (n = root; n; n = node_next_in(n, root))
        if (base_with_href(n)) return lxb_dom_interface_element(n);
    return NULL;
}

/* CSP §6.3.1.1 "Is base allowed for document?", which §4.2.3 step 3 calls: for each policy of the document's
   CSP list, if a `base-uri` directive is present and §6.7.2.7's does-url-match-source-list-in-origin answers
   "Does Not Match", the enforce policy returns "Blocked". The quantifier is `policy_should_block_request`'s
   read from the other end and for the same reason — a second policy can only narrow.
   `redirect count` IS ZERO AND STATED SO BY THE ALGORITHM, which is why it is a literal here: a base URL is
   not a request and has not been redirected. */
static bool base_allowed_for_document(const PolicyContainer *policy, const UrlRecord *base)
{
    const CspList *list = policy_container_csp_list(policy);
    size_t i;

    /* A document with no policy — the overwhelmingly common case — carries no `base-uri` directive, so
       §6.3.1.1's loop runs zero times and its step 3 answers "Allowed". */
    if (!list) return true;
    for (i = 0; i < list->n_policies; i++) {
        const CspDirective *d = csp_policy_directive(&list->policies[i], "base-uri");

        if (!d) continue;   /* "if source list is null, skip to the next policy" */
        if (csp_source_list_match_url(d, base, list->self_origin, 0) == CSP_MATCHES) continue;
        /* §6.3.1.1's violation steps, of which §5.5's REPORT is not built — the same gap
           core/frame/policy_container.c refuses at, and refused in the same words, because the endpoint is
           DECLARED IN THE POLICY and is therefore a gap this function can see coming. */
        DCHECK(!csp_policy_directive(&list->policies[i], "report-uri") &&
                   !csp_policy_directive(&list->policies[i], "report-to"),
               "a `<base href>` was BLOCKED by a policy that declares a reporting endpoint, and CSP §5.5's "
               "report a violation is not built — the page's server is owed a report it will never receive. "
               "Build §2.4.1's violation object (resource \"inline\", effective directive \"base-uri\") and "
               "§5.5's report a violation, whose two observables are the `securitypolicyviolation` event fired "
               "at the Document and the POST to the endpoints `report-to`/`report-uri` name");
        return false;   /* every policy this build parses has disposition "enforce" */
    }
    return true;
}

/* §4.2.3's "SET THE FROZEN BASE URL for an element element". Every step is here in its own order, including
   step 3's `return`, which is what makes the blocked/failed case skip step 5. */
static void set_the_frozen_base_url(lxb_dom_element_t *element)
{
    lxb_dom_node_t *node = lxb_dom_interface_node(element);
    lxb_dom_document_t *document = node->owner_document;   /* STEP 1: element's node document */
    const char *fallback;
    const lxb_char_t *href;
    size_t href_len = 0;
    UrlRecord base, url_record;
    bool have_base, parsed;
    char *serialized;

    DCHECK(base_with_href(node),
           "§4.2.3's set the frozen base URL ran for something that is not a base element with an href — the "
           "frozen base URL is a fact ABOUT that element ('a base element that is the first base element with "
           "an href content attribute in a document tree HAS a frozen base URL'), so an element without one "
           "has nothing to freeze and a document that took this answer would resolve every relative URL "
           "against a base no markup asked for");
    DCHECK(document != NULL,
           "§4.2.3's set the frozen base URL ran for an element in no document — step 1 reads the element's "
           "NODE DOCUMENT and every node has one, so this element came from neither a parse nor a create");
    fallback = document_fallback_base_url_of(document);
    href = lxb_dom_element_get_attribute(element, (const lxb_char_t *)"href", 4, &href_len);
    /* A VALUELESS ATTRIBUTE IS THE EMPTY STRING, not an absent one — the presence test above already decided
       that this element HAS an href, and the HTML parser stores `<base href>` with no value buffer. */
    if (!href) href_len = 0;

    /* STEP 2: "let urlRecord be the result of parsing the value of element's href content attribute with
       document's FALLBACK BASE URL … (Thus, the base element isn't affected by itself.)" — which is exactly
       why this reads the fallback and not document_base_url_of. */
    url_record_init(&base);
    url_record_init(&url_record);
    have_base = fallback && *fallback && url_parse(&base, fallback, strlen(fallback), NULL);
    parsed = url_parse(&url_record, href ? (const char *)href : "", href_len, have_base ? &base : NULL);

    /* STEP 3's three disjuncts: a failed parse, a `data:`/`javascript:` scheme, and CSP §6.3.1.1. All three
       land on the same answer — the document's fallback base URL — and all three RETURN, so step 5's respond
       to base URL changes does not run for them. */
    if (!parsed || (url_record.scheme && (strcmp(url_record.scheme, "data") == 0 ||
                                          strcmp(url_record.scheme, "javascript") == 0)) ||
        !base_allowed_for_document(document_policy_of(document), &url_record)) {
        document_set_frozen_base_url(document, element, fallback);
        url_record_free(&url_record);
        url_record_free(&base);
        return;
    }

    /* STEP 4: "set element's frozen base URL to urlRecord." It is STORED SERIALIZED because that is what every
       reader of a base URL in this engine takes — one representation, so the record and its serialization
       cannot disagree. */
    serialized = url_serialize(&url_record, false);
    CHECK(serialized != NULL,
          "§4.2.3 step 4: a parsed frozen base URL could not be serialized — an allocation failure, and a "
          "document that kept its old base instead would resolve every relative URL in the page against an "
          "address the markup replaced");
    document_set_frozen_base_url(document, element, serialized);
    free(serialized);
    url_record_free(&url_record);
    url_record_free(&base);

    /* STEP 5: "respond to base URL changes given document." §2.4.3 gives that algorithm four steps and this
       build has an observable for NONE of them, by construction rather than by omission: steps 1-2 are user
       interface and the CSS `:link`/`:visited` pseudo-classes (a headless engine paints neither, and the
       visited state is deliberately unobservable to script in every browser), step 3 walks for `<script>`
       elements whose RESULT is a speculation rules parse result — this engine creates no such result for any
       script, so the loop is vacuous for every document rather than merely usually — and step 4's consider
       speculative loads is a fetch a user agent MAY make. What §2.4.3's own note says is observable is exactly
       what already works here: an `img.src` read AFTER the base changed reports the new absolute URL, because
       §2.6.1's getter resolves at the read rather than caching. */
}

/* §4.2.3's TWO SITUATIONS, ASKED AS THE STATE CHANGE THEY ARE. The standard states them as facts about an
 * element ("the base element BECOMES the first …", "the base element IS the first … and its href IS CHANGED"),
 * and both are decided by comparing WHICH element is first with WHICH one the document's frozen base URL
 * currently belongs to — so one comparison answers both, plus the `href_changed` element for the second.
 *
 * WHY THE DOCUMENT REMEMBERS ONE ELEMENT AND ONE URL, AND WHY THAT IS NOT AN APPROXIMATION OF PER-ELEMENT
 * STORAGE. §4.2.3 gives a frozen base URL to an ELEMENT, but the only reader of one is §2.4.3, which reads the
 * FIRST base element with an href — so a non-first element's frozen URL is unobservable, and the moment it
 * becomes first, situation 1 sets it AGAIN rather than reading whatever it froze to earlier. One (element,
 * URL) pair on the document is therefore exactly equivalent, and it is the pair Blink keeps.
 *
 * A RE-FREEZE IS NOT SKIPPED WHEN THE ANSWER WOULD BE THE SAME, and must not be: the situations are stated
 * over the ELEMENT, not over the resulting URL. */
static void base_element_process(lxb_dom_document_t *document, lxb_dom_element_t *href_changed)
{
    lxb_dom_element_t *first, *frozen;

    /* A LEXBOR TREE IS NOT ALWAYS A Document. §4.2.3 and §2.4.3 are both stated over one — a tree the solver
       parsed for a scan, or a fragment parser's scratch root, has no record, no realm, no address and nothing
       a base URL could be frozen against. That is the standard's own domain restriction rather than a case to
       tolerate, and `document_realm_of` is this engine's EXISTING (tree -> is there a Document here) answer:
       asking it rather than growing a second predicate is what keeps the two from disagreeing. */
    if (!document_realm_of(lxb_dom_interface_node(document))) return;
    first = first_base_with_href(document);
    frozen = document_frozen_base_element(document);
    if (first == frozen && first != href_changed) return;
    /* No base element with an href left in the document — §2.4.3 step 1 then answers with the FALLBACK base
       URL, which is a live read of the document's address rather than anything frozen, so the stored pair is
       cleared instead of being frozen to today's address. */
    if (!first) {
        document_set_frozen_base_url(document, NULL, NULL);
        return;
    }
    set_the_frozen_base_url(first);
}

/* Does this subtree contain a base element with an href? The tree-steps hook is handed the node that moved and
   the whole SUBTREE moves with it, so inserting `<div><base href=x></div>` inserts a base element. Asked before
   the walk from the document root because that walk is O(document) and every tree write reaches this hook. */
static bool subtree_has_base_with_href(lxb_dom_node_t *n)
{
    lxb_dom_node_t *d;

    for (d = n; d; d = node_next_in(d, n))
        if (base_with_href(d)) return true;
    return false;
}

void html_base_element_tree_steps(JSContext *ctx, lxb_dom_node_t *n, lxb_dom_node_t *parent, int phase)
{
    (void)ctx;
    (void)parent;
    /* NODE_TREE_REMOVING is the PRE-detach phase and the node is still in the tree, so a recompute there would
       find the element that is leaving and freeze to it; NODE_TREE_REMOVED is the phase after the detach, which
       is where "the base element in front of it left, so a later one becomes first" is true. */
    if (phase == NODE_TREE_REMOVING) return;
    if (!n || !n->owner_document || !subtree_has_base_with_href(n)) return;
    /* A subtree moved WITHIN a detached tree reaches here too, and the walk from the document root simply finds
       the same first element — the recompute is the answer, and the connectedness test the spec writes ("in its
       Document") is the walk itself rather than a second predicate that could disagree with it. */
    base_element_process(n->owner_document, NULL);
}

void html_base_element_attr_changed(JSContext *ctx, lxb_dom_element_t *el, const char *ns, const char *local)
{
    lxb_dom_node_t *n = lxb_dom_interface_node(el);

    (void)ctx;
    if (ns != NULL || !local || strcmp(local, "href")) return;   /* the `href` CONTENT attribute, null namespace */
    if (!base_element_is(n)) return;
    DCHECK(n->owner_document != NULL,
           "§4.9's attribute change steps reached a base element in no document — every node has a node "
           "document, so this element belongs to a tree neither a parse nor a create built");
    /* BOTH SITUATIONS COME THROUGH HERE, and that is why the element is passed rather than a boolean: adding an
       `href` to a base element that PRECEDES the frozen one makes it the first (situation 1), removing the
       `href` from the frozen one makes a later one the first (situation 1 again), and changing the frozen
       one's own `href` is situation 2 — which is the only one the identity comparison alone cannot see. */
    base_element_process(n->owner_document, el);
}

void html_base_element_parsed(JSContext *ctx, lxb_dom_node_t *root)
{
    (void)ctx;
    DCHECK(root != NULL && root->type == LXB_DOM_NODE_TYPE_DOCUMENT,
           "§4.2.3's parsed-tree freeze was handed something other than a Document node — the walk is stated "
           "over the document ('the first base element in document … in tree order'), and a subtree root would "
           "answer for a different tree");
    base_element_process(lxb_dom_interface_document(root), NULL);
}

/* WEB IDL §3.7.6's BRAND CHECK — "if `this` does not implement the interface, throw a TypeError". A member is
   on a prototype and a page can call it on anything, so the receiver is a real question with a real spec
   answer rather than an invariant to assert; the pattern is core/html/html_script.c's, one member over. */
static lxb_dom_node_t *base_receiver(JSContext *ctx, JSValueConst this_val, const char *member)
{
    lxb_dom_node_t *n = node_of(this_val);

    if (base_element_is(n)) return n;
    JS_ThrowTypeError(ctx, "HTMLBaseElement.%s was reached on something that is not a <base> element", member);
    return NULL;
}

/* §4.2.3's `href` GETTER — its OWN algorithm, and neither of the two reflections it looks like. Step 2 parses
 * against the document's FALLBACK base URL: "(Thus, the base element isn't affected by other base elements or
 * itself.)" So `<base href="a/"><base href="b/">`'s SECOND element reports the document address plus `b/`,
 * where §2.6.1's URL reflection would have reported the FIRST element's frozen base plus `b/`.
 *
 * A CONCOLIC ATTRIBUTE VALUE STAYS CONCOLIC, the shape core/dom/element.c's el_reflect_url uses: the REAL parse
 * runs on the concrete example and the answer is DERIVED from it, so an attacker string a flow stashed in a
 * `<base href>` survives the round trip instead of being stringified away at the one member that reads it back. */
static JSValue js_base_href(JSContext *ctx, JSValueConst this_val, int magic)
{
    lxb_dom_node_t *n = base_receiver(ctx, this_val, "href");
    JSValue raw, concrete, out;
    const char *fallback, *s;
    size_t len = 0;
    UrlRecord base, url_record;
    bool have_base, parsed;
    char *serialized;

    (void)magic;
    if (!n) return JS_EXCEPTION;
    /* STEP 2: "let url be the value of the href attribute of this element, if it has one, and the empty string
       otherwise" — the empty string parses against the fallback base URL to the fallback base URL itself, which
       is what a browser answers for `<base target=x>` with no href. */
    raw = element_attr_get_value(ctx, this_val, "href");
    if (JS_IsException(raw)) return raw;
    if (JS_IsNull(raw)) {
        JS_FreeValue(ctx, raw);
        raw = JS_NewString(ctx, "");
        CHECK(!JS_IsException(raw), "§4.2.3 step 2's empty string could not be allocated");
    }
    concrete = concolic_is(raw) ? concolic_example(ctx, raw) : JS_DupValue(ctx, raw);
    /* A concolic with no example yet has no bytes to parse, and §4.2.3 has no step that invents them — step 4's
       "if urlRecord is failure, return url" is the answer, still carrying its provenance. */
    if (!JS_IsString(concrete)) { JS_FreeValue(ctx, concrete); return raw; }
    s = JS_ToCStringLen(ctx, &len, concrete);
    JS_FreeValue(ctx, concrete);
    if (!s) { JS_FreeValue(ctx, raw); return JS_EXCEPTION; }

    DCHECK(n->owner_document != NULL, "§4.2.3's `href` getter read a base element with no node document");
    fallback = document_fallback_base_url_of(n->owner_document);
    url_record_init(&base);
    url_record_init(&url_record);
    have_base = fallback && *fallback && url_parse(&base, fallback, strlen(fallback), NULL);
    parsed = url_parse(&url_record, s, len, have_base ? &base : NULL);
    JS_FreeCString(ctx, s);
    serialized = parsed ? url_serialize(&url_record, false) : NULL;
    url_record_free(&url_record);
    url_record_free(&base);

    if (!parsed) return raw;   /* STEP 4: "if urlRecord is failure, return url" — the attribute value itself */
    CHECK(serialized != NULL,
          "§4.2.3 step 5: a parsed `base.href` could not be serialized — an allocation failure, and answering "
          "the raw attribute instead would report a relative URL where the member's whole job is the absolute");
    out = JS_NewString(ctx, serialized);
    free(serialized);
    if (concolic_is(raw))
        out = concolic_builtin_hook(ctx, raw, "HTMLBaseElement.href", out);   /* consumes `out` as the example */
    JS_FreeValue(ctx, raw);
    return out;   /* STEP 5: "return the serialization of urlRecord" */
}

/* §4.2.3's `href` SETTER — the ReflectSetter, which writes the content attribute and nothing else. The freeze
   is NOT performed here: `b.href = x`, `b.setAttribute('href', x)` and `b.attributes.href.value = x` are one
   write of one attribute, and §4.9's attribute change steps are the chokepoint all three reach. A setter-side
   freeze would answer for the first spelling only — the identical reason core/html/media_element.c's `src`
   trigger is on the chokepoint rather than in its setter. */
static JSValue js_base_set_href(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    (void)magic;
    if (!base_receiver(ctx, this_val, "href")) return JS_EXCEPTION;
    element_attr_set_value(ctx, this_val, "href", val);
    return JS_UNDEFINED;
}

void html_base_element_init(JSContext *ctx)
{
    DCHECK(g_id_set_href < 0, "html_base_element_init ran twice in one runtime — the setter is declared once "
                              "per AGENT and installed once per realm");
    g_id_set_href = idl_setter_id(ctx, IDL_USVSTRING, false, js_base_set_href, 0);
}

/* THE AGENT'S HALF, UNDONE. The only agent state here is the setter's POOL ID — an int, so there is no
   JSRuntime to hand back anything to, which is why this row takes nothing. It exists all the same, because the
   id is what the init above asserts has not been declared twice: a second runtime in one process would find a
   stale id and install a member from a pool that no longer holds it. */
void html_base_element_free(void)
{
    g_id_set_href = -1;
}

void html_base_element_install(JSContext *ctx, JSValueConst proto)
{
    DCHECK(g_id_set_href >= 0,
           "§4.2.3's `href` was installed before html_base_element_init declared its setter");
    idl_install_accessor(ctx, proto, "href", js_base_href, 0, g_id_set_href);
}
