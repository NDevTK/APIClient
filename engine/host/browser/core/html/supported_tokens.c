/* HTML's supported tokens for a reflected DOMTokenList — see supported_tokens.h for why the sets live here and
 * the algorithm lives in core/dom/dom_token_list.c.
 *
 * THE ROWS ARE THE (ELEMENT, ATTRIBUTE NAME) PAIRS A SPECIFICATION DEFINES SUPPORTED TOKENS FOR, and naming
 * them as a closed set is what makes the DEFAULT correct rather than lenient: an unnamed pair defines none, so
 * it throws, which is what `classList.supports("x")` and `link.sizes.supports("any")` do in a browser. A row
 * that answered "not supported" for a pair with no definition would turn a TypeError into a `false` and hide
 * the very fact DOM §7.1 "Interface DOMTokenList"'s validation step 1 exists to state. */
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "core/frame/sandboxing.h"
#include "core/html/html_link.h"
#include "core/html/hyperlink.h"
#include "core/html/supported_tokens.h"

/* WHICH DEFINITION ANSWERS. One value per sentence in a standard that defines supported tokens, so a reader
   can go from a row to the paragraph it came out of. */
typedef enum {
    ROW_NONE = 0,
    ROW_LINK_REL,        /* §4.2.4 "The link element" */
    ROW_HYPERLINK_REL,   /* §4.6.2 "Links created by a and area elements" */
    ROW_FORM_REL,        /* §4.10.3 "The form element" */
    ROW_IFRAME_SANDBOX,  /* §4.8.5 "The iframe element" */
} SupportedTokenRow;

static bool local_is(const lxb_char_t *name, size_t len, const char *want)
{
    size_t n = strlen(want);
    return len == n && memcmp(name, want, n) == 0;
}

/* A SUPPORTED TOKEN IS ONE KEYWORD. §7.1's validation steps never say so because they never have to — they ask
   for membership in a SET OF KEYWORDS, and no keyword any of the rows below names contains ASCII whitespace or
   is empty. The check is here rather than in the rows because it is the same fact for every one of them, and
   because without it a row that answers by walking a token list would say `true` for
   `sandbox.supports("allow-scripts allow-forms")` — a string that is two allowed values and no allowed value. */
static bool is_one_token(const char *token, size_t len)
{
    size_t i;

    if (len == 0) return false;
    for (i = 0; i < len; i++) {
        char c = token[i];
        /* INFRA's ASCII whitespace: TAB, LF, FF, CR, SPACE. */
        if (c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r') return false;
    }
    return true;
}

HtmlSupportedToken html_supported_token(lxb_dom_element_t *el, const char *attr_local,
                                        const char *token, size_t token_len)
{
    const lxb_char_t *local;
    size_t local_len = 0;
    SupportedTokenRow row = ROW_NONE;
    bool present = false;

    DCHECK(el != NULL,
           "DOM §7.1 \"Interface DOMTokenList\"'s supported-token question was asked with no element — the "
           "answer is keyed by \"set's element and attribute name\", so an absent element is a caller that "
           "has not resolved the list it is asking about");
    DCHECK(attr_local != NULL,
           "DOM §7.1 \"Interface DOMTokenList\"'s supported-token question was asked with no attribute name "
           "— a token list is a view over ONE attribute and that attribute is half the key");
    DCHECK(token != NULL,
           "DOM §7.1 \"Interface DOMTokenList\"'s supported-token question was asked with no token — "
           "validation step 2 has already lowercased it by here, so a null is a caller that skipped it");

    /* §7.1 keys the answer on the ELEMENT, and every definition below is a definition for an HTML element:
       an `a` in the SVG namespace is not §4.6.2's `a` and has no `rel` processing model at all. */
    if (lxb_dom_interface_node(el)->ns != LXB_NS_HTML) return HTML_TOKENS_UNDEFINED;
    local = lxb_dom_element_local_name(el, &local_len);
    DCHECK(local != NULL && local_len > 0,
           "an element in the HTML namespace has no local name — DOM §4.9 \"Interface Element\" gives every "
           "element an associated local name, so a token list whose owner has none is an element this engine "
           "did not build");

    if (!strcmp(attr_local, "rel")) {
        if (local_is(local, local_len, "link"))                                    row = ROW_LINK_REL;
        else if (local_is(local, local_len, "a") || local_is(local, local_len, "area"))
                                                                                   row = ROW_HYPERLINK_REL;
        else if (local_is(local, local_len, "form"))                               row = ROW_FORM_REL;
    } else if (!strcmp(attr_local, "sandbox")) {
        if (local_is(local, local_len, "iframe"))                                  row = ROW_IFRAME_SANDBOX;
    }
    /* Every other pair — `class` on any element, `sizes` on `<link>`, `rel` on an element whose IDL declares no
       `relList` — has no specification defining supported tokens for it, which is §7.1 validation step 1. */
    if (row == ROW_NONE) return HTML_TOKENS_UNDEFINED;

    /* DEFINEDNESS IS DECIDED BEFORE THE TOKEN IS LOOKED AT, because §7.1's step 1 is. A malformed candidate on
       a pair that defines tokens is `false`, not a TypeError. */
    if (!is_one_token(token, token_len)) return HTML_TOKENS_ABSENT;

    switch (row) {
    case ROW_LINK_REL:
        /* §4.2.4 "The link element": "rel's supported tokens are the keywords defined in HTML link types which
           are allowed on link elements, impact the processing model, and are supported by the user agent. …
           rel's supported tokens must only include the tokens from this list that the user agent implements
           the processing model for." */
        present = html_link_rel_supported((const char *)token, token_len);
        break;
    case ROW_HYPERLINK_REL:
        /* §4.6.2 "Links created by a and area elements": "rel's supported tokens are the keywords defined in
           HTML link types which are allowed on a and area elements, impact the processing model, and are
           supported by the user agent. The possible supported tokens are noreferrer, noopener, and opener." */
        present = hyperlink_rel_supported((const char *)token, token_len);
        break;
    case ROW_FORM_REL:
        /* §4.10.3 "The form element": "rel's supported tokens are the keywords defined in HTML link types
           which are allowed on form elements, impact the processing model, and are supported by the user
           agent. The possible supported tokens are noreferrer, noopener, and opener." The model that reads
           them is the same §4.6.5 "Following hyperlinks" get-an-element's-noopener §4.6.2's row answers
           through, and a form reaches it from §4.10.22.3 "Form submission algorithm" step 21 ("Let noopener be
           the result of getting an element's noopener with form, parsed action, and target").
           SO THE TWO ROWS ARE ONE ANSWER, and they always were: §4.6.5 defines that algorithm over "an a,
           area, or form element", so the set of keywords that impact its processing model cannot depend on
           which of the three is asking. This row answered with the EMPTY set for as long as the algorithm was
           an `if` inside the hyperlink caller — not because a form's keywords differ, but because the model
           was not a component anything else could reach. It is one now (core/html/hyperlink.c), so this row
           asks it exactly as §4.6.2's does. */
        present = hyperlink_rel_supported((const char *)token, token_len);
        break;
    case ROW_IFRAME_SANDBOX:
        /* §4.8.5 "The iframe element": "The supported tokens for sandbox's DOMTokenList are the allowed values
           defined in the sandbox attribute and supported by the user agent." */
        present = sandbox_keyword_supported((const char *)token, token_len);
        break;
    case ROW_NONE:
        DFAIL("a supported-token row was selected and then not answered — the row is decided and consumed in "
              "one function, so a pair that reached the answer without one is a row added above and not here");
        break;
    }
    return present ? HTML_TOKENS_PRESENT : HTML_TOKENS_ABSENT;
}
