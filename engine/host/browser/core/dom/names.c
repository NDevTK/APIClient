/* See names.h. */
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
