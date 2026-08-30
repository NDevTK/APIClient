/* HTML's SUPPORTED TOKENS for a reflected DOMTokenList — the half of DOM §7.1 "Interface DOMTokenList" that
 * §7.1 deliberately does not answer.
 *
 * §7.1's validation steps begin "If set's element and attribute name does not define supported tokens, then
 * throw a TypeError", and the only thing §7.1 says about who defines them is one sentence: "Specifications may
 * define supported tokens for a DOMTokenList's element and attribute name." So the ALGORITHM is DOM's and the
 * SETS are the defining specification's, and this component is where HTML's are answered from. A token set
 * spelled inside core/dom/dom_token_list.c would put HTML link types in a DOM file and — worse — would put
 * them somewhere the components that RUN their processing models cannot correct.
 *
 * THE ANSWER IS THREE-STATE AND NOT A BOOLEAN, because §7.1's steps distinguish three outcomes and collapsing
 * two of them changes what a page sees: "this pair defines no supported tokens" is a TypeError, while "this
 * pair defines them and the token is not among them" is `false`. `classList.supports("x")` throws in a browser
 * and `link.relList.supports("stylesheet")` does not, and one boolean cannot say both.
 *
 * "SUPPORTED BY THE USER AGENT" IS A MUST, AND IT IS ASKED OF THE COMPONENT THAT IMPLEMENTS THE MODEL. HTML
 * does not say the supported tokens ARE its lists — §4.2.4 "The link element" says "rel's supported tokens
 * must only include the tokens from this list that the user agent implements the processing model for". A
 * table here naming all thirteen of §4.2.4's keywords would therefore be a WRONG answer rather than a generous
 * one: it would report a processing model this engine does not run, and the page would take the branch behind
 * it. So each row asks the component that owns the model (core/html/html_link.c for `<link rel>`,
 * core/html/hyperlink.c for `<a>`/`<area> rel`, core/frame/sandboxing.c for `<iframe sandbox>`), and the day
 * one of those gains a keyword's steps the answer follows with nothing here to edit. */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_SUPPORTED_TOKENS_H
#define ENGINE_HOST_BROWSER_CORE_HTML_SUPPORTED_TOKENS_H

#include <stdbool.h>
#include <stddef.h>

#include <lexbor/dom/interfaces/element.h>

/* The three outcomes of §7.1's validation steps, kept apart for the reason above. */
typedef enum {
    HTML_TOKENS_UNDEFINED = 0,   /* step 1: this (element, attribute name) defines no supported tokens */
    HTML_TOKENS_ABSENT,          /* step 4: they are defined and this token is not one of them */
    HTML_TOKENS_PRESENT,         /* step 3: they are defined and this token is one of them */
} HtmlSupportedToken;

/* `token` is the candidate ALREADY IN ASCII LOWERCASE — §7.1's validation step 2 is the caller's, because the
   lowercasing is the DOM algorithm's step and not a property of any HTML row. `attr_local` is the token list's
   attribute name (§7.1's "attribute name", an attribute's local name). */
HtmlSupportedToken html_supported_token(lxb_dom_element_t *el, const char *attr_local,
                                        const char *token, size_t token_len);

#endif
