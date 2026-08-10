/* See names.h. */
#include <string.h>

#include "check.h"
#include "core/dom/names.h"

/* Infra's three character classes, which is what the DOM's steps are written in terms of. */
static bool ascii_whitespace(unsigned char c)
{
    return c == 0x09 || c == 0x0A || c == 0x0C || c == 0x0D || c == 0x20;
}

static bool ascii_alpha(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static bool ascii_digit(unsigned char c)
{
    return c >= '0' && c <= '9';
}

bool dom_valid_element_local_name(const char *name, size_t len)
{
    size_t i;

    if (!name || len == 0) return false;                  /* step 1 */
    if (ascii_alpha((unsigned char)name[0])) {            /* step 2 — the HTML-ish arm */
        for (i = 0; i < len; i++) {
            unsigned char c = (unsigned char)name[i];
            /* step 2.1: only what the tokenizer could not read back is excluded. */
            if (ascii_whitespace(c) || c == 0x00 || c == '/' || c == '>') return false;
        }
        return true;                                      /* step 2.2 */
    }
    {                                                     /* step 3 — the XML-ish arm's first code point */
        unsigned char c0 = (unsigned char)name[0];
        if (!(c0 == ':' || c0 == '_' || c0 >= 0x80)) return false;
    }
    for (i = 1; i < len; i++) {                           /* step 4 */
        unsigned char c = (unsigned char)name[i];
        if (ascii_alpha(c) || ascii_digit(c) ||
            c == '-' || c == '.' || c == ':' || c == '_' || c >= 0x80) continue;
        return false;
    }
    return true;                                          /* step 5 */
}

/* §1.4's two "no character from this set" predicates. They differ by ONE code point — U+003D (=), which an
   attribute local name forbids because `a=b="c"` would re-parse as two attributes and a prefix does not — so
   they share the walk and differ in a parameter, which is the only way that difference stays visible. */
static bool no_forbidden(const char *name, size_t len, bool ban_equals)
{
    size_t i;

    if (!name || len == 0) return false;
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)name[i];
        if (ascii_whitespace(c) || c == 0x00 || c == '/' || c == '>') return false;
        if (ban_equals && c == '=') return false;
    }
    return true;
}

bool dom_valid_namespace_prefix(const char *name, size_t len) { return no_forbidden(name, len, false); }
bool dom_valid_attribute_local_name(const char *name, size_t len) { return no_forbidden(name, len, true); }

/* THE TWO NAMESPACES §1.4 NAMES BY IDENTITY. Written out rather than reached through lexbor's table because
   these are the standard's own constants: the steps below compare against them, and a table lookup would make
   the comparison depend on whether the document happened to have interned them. */
#define NS_XML   "http://www.w3.org/XML/1998/namespace"
#define NS_XMLNS "http://www.w3.org/2000/xmlns/"

static bool str_is(const char *s, size_t len, const char *lit)
{
    return s && len == strlen(lit) && memcmp(s, lit, len) == 0;
}

bool dom_validate_and_extract(JSContext *ctx, const char *ns, size_t ns_len,
                              const char *qname, size_t qname_len, DomNameKind kind, DomQName *out)
{
    const char *colon;

    if (ns && ns_len == 0) ns = NULL;                             /* step 1: "" IS the null namespace */
    out->ns = ns; out->ns_len = ns ? ns_len : 0;
    out->prefix = NULL; out->prefix_len = 0;                      /* step 2 */
    out->local = qname; out->local_len = qname_len;
    colon = qname ? memchr(qname, ':', qname_len) : NULL;
    if (colon) {                                                  /* step 3 */
        out->prefix = qname; out->prefix_len = (size_t)(colon - qname);
        out->local = colon + 1; out->local_len = qname_len - out->prefix_len - 1;
        if (!dom_valid_namespace_prefix(out->prefix, out->prefix_len)) {   /* step 3.3 */
            JS_ThrowDOMException(ctx, "InvalidCharacterError",
                                 "the namespace prefix is not a valid namespace prefix");
            return false;
        }
    }
    /* step 4: the assertion the spec states, which holds because step 3.3 is the only way a prefix is set. */
    DCHECK(!out->prefix || dom_valid_namespace_prefix(out->prefix, out->prefix_len),
           "validate-and-extract carried a prefix its own step 3 would have rejected");
    /* steps 5 and 6: WHICH predicate the local name must satisfy is what the context decides, and it is a real
       difference — `a=b` is a valid element local name and not a valid attribute one. */
    if (!(kind == DOM_NAME_ATTRIBUTE ? dom_valid_attribute_local_name(out->local, out->local_len)
                                     : dom_valid_element_local_name(out->local, out->local_len))) {
        JS_ThrowDOMException(ctx, "InvalidCharacterError",
                             kind == DOM_NAME_ATTRIBUTE ? "not a valid attribute local name"
                                                        : "not a valid element local name");
        return false;
    }
    if (out->prefix && !ns) {                                     /* step 7 */
        JS_ThrowDOMException(ctx, "NamespaceError", "a prefixed name needs a namespace");
        return false;
    }
    if (str_is(out->prefix, out->prefix_len, "xml") && !str_is(ns, ns_len, NS_XML)) {   /* step 8 */
        JS_ThrowDOMException(ctx, "NamespaceError", "the \"xml\" prefix belongs to the XML namespace");
        return false;
    }
    /* step 9: EITHER the whole qualified name or the prefix being "xmlns" binds it to the XMLNS namespace —
       `xmlns` with no prefix at all is the one that matters, and reading only the prefix misses it. */
    if ((str_is(qname, qname_len, "xmlns") || str_is(out->prefix, out->prefix_len, "xmlns"))
        && !str_is(ns, ns_len, NS_XMLNS)) {
        JS_ThrowDOMException(ctx, "NamespaceError", "the \"xmlns\" name belongs to the XMLNS namespace");
        return false;
    }
    if (str_is(ns, ns_len, NS_XMLNS)                              /* step 10 — and its converse */
        && !(str_is(qname, qname_len, "xmlns") || str_is(out->prefix, out->prefix_len, "xmlns"))) {
        JS_ThrowDOMException(ctx, "NamespaceError", "the XMLNS namespace only holds \"xmlns\" names");
        return false;
    }
    return true;                                                  /* step 11 */
}
