/* HTML §4.2.5.3 "Pragma directives"' content security policy state — see html_meta_csp.h for why the five
 * steps are a component and why step 5 is not among them.
 *
 * THE ORDER THIS ENGINE CREATES A DOCUMENT IN IS WHY STEP 5 IS THE CALLER'S. HTML §7.5.1 "Shared document
 * creation infrastructure" creates the Document — realm and record — and RETURNS it, and §7.5.2 "Loading HTML
 * documents" then associates a parser with that Document and feeds it; so in the standard a `<meta>` is
 * inserted into a Document that already exists, and step 5's "enforce the policy" appends to a CSP list the
 * Document is already holding. This engine parses FIRST and builds the record around the finished tree, so
 * there is no list to append to while the parse runs and the `<meta>` policies are instead collected by a walk
 * afterwards. That inversion is what this file's interface is shaped around rather than against: the walk and
 * the insertion step ask the identical question of one element and differ only in WHO enforces the answer, so
 * the day the record is created before the parse, this file does not change and its caller moves. */
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>
#include <lexbor/ns/ns.h>
#include <lexbor/tag/const.h>

#include "check.h"
#include "core/frame/csp_directive_list.h"
#include "core/html/html_meta_csp.h"

char *html_meta_csp_policy(const lxb_dom_node_t *node)
{
    lxb_dom_element_t *meta;
    const lxb_char_t *equiv, *content;
    size_t equiv_len = 0, content_len = 0;
    CspPolicy policy;
    char *serialized;

    DCHECK(node != NULL,
           "§4.2.5.3's content security policy state was asked of no node — the pragma is selected off an "
           "element's own `http-equiv`, so every caller is standing at a node when it asks and an absent one "
           "is a walk that reached past its own tree");
    /* THE STATE'S OWN SELECTION, WHICH IS §4.2.5.3's FIRST ACT. `meta` in the HTML namespace: pragma
       directives are defined for the HTML `meta` element, and an SVG or MathML element that happens to be
       named `meta` carries none of §4.2.5's states. */
    if (node->type != LXB_DOM_NODE_TYPE_ELEMENT || node->local_name != LXB_TAG_META || node->ns != LXB_NS_HTML)
        return NULL;
    meta = lxb_dom_interface_element((lxb_dom_node_t *)node);
    equiv = lxb_dom_element_get_attribute(meta, (const lxb_char_t *)"http-equiv", 10, &equiv_len);
    /* §4.2.5's states are keyed by an ASCII case-insensitive match on the attribute's value, which is how
       every real page spells this one — `Content-Security-Policy` far more often than the lowercase form. */
    if (!equiv || equiv_len != 23 || strncasecmp((const char *)equiv, "content-security-policy", 23) != 0)
        return NULL;

    /* STEP 1: "If the meta element is not a child of a head element, return."
       THE DIRECTION OF THIS REFUSAL IS WHY IT MATTERS HERE. A policy declared in the BODY is inert in every
       browser, so an engine that enforces it judges the page under a policy the page does not have — and this
       engine's judgement of a policy decides whether a solved breakout is reported or suppressed. */
    if (!node->parent || node->parent->type != LXB_DOM_NODE_TYPE_ELEMENT ||
        node->parent->local_name != LXB_TAG_HEAD || node->parent->ns != LXB_NS_HTML)
        return NULL;

    /* STEP 2: "If the meta element has no content attribute, or if that attribute's value is the empty
       string, then return." */
    content = lxb_dom_element_get_attribute(meta, (const lxb_char_t *)"content", 7, &content_len);
    if (!content || content_len == 0)
        return NULL;

    /* STEP 3: CSP §2.2.1 "parse a serialized CSP" over that value — ONE policy, with a source of "meta" and a
       disposition of "enforce". THE SOURCE IS THE FACT STEP 4 EXISTS TO STAND IN FOR: this build's policy
       model carries no per-policy source (core/frame/csp_directive_list.h says why — a source cannot survive
       §7.4's clone, which travels as text), and step 4 is the only place a meta-delivered policy differs from
       a header-delivered one in what it may do. Performing the removal here is therefore not an approximation
       of the source; it is what the source would have been consulted FOR.
       THE ATTRIBUTE'S VALUE IS PARSED MARKUP BY NOW, so entity decoding and quoting are the HTML parser's
       answer rather than a second one — the same reason the bundle id is a real `<script>` scan. */
    memset(&policy, 0, sizeof policy);
    csp_policy_parse(&policy, (const char *)content, content_len);

    /* STEP 4: "Remove all occurrences of the report-uri, frame-ancestors, and sandbox directives from
       policy." Each for its own reason, and `sandbox` is the one this engine could previously only ASSERT
       about: §7.1.5's CSP-derived sandboxing flags are computed from a serialized list that no longer says
       which policy came from a `<meta>`, so a `sandbox` left in here would sandbox a Document whose server
       never sandboxed it. The removal makes that state unreachable rather than merely detected. */
    csp_policy_remove_directive(&policy, "report-uri");
    csp_policy_remove_directive(&policy, "frame-ancestors");
    csp_policy_remove_directive(&policy, "sandbox");

    /* AND THE POLICY THIS ELEMENT DELIVERS, in the form step 5's caller enforces it in. NULL when step 4 took
       the last directive — `<meta http-equiv=CSP content="sandbox">` delivers nothing at all, which §2.2.2
       says of an empty directive set in as many words. */
    serialized = csp_policy_serialize(&policy);
    csp_policy_free(&policy);
    DCHECK(serialized == NULL || *serialized != 0,
           "§4.2.5.3 produced an EMPTY policy string — a policy with no directives serializes as NULL, so an "
           "empty string here is a serialization that measured a directive and wrote none, and a caller "
           "joining it into a CSP list would emit a stray delimiter");
    return serialized;
}
