/* CSS Conditional 5 §5.1, §5.2 and §5.3's grammars. See css_container.h. */
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "check.h"
#include "core/css/css_code_point.h"
#include "core/css/css_container.h"
#include "core/css/css_defaulting.h"
#include "core/css/css_serialize.h"
#include "solver/reclaim.h"

/* §5.1's `[ size | inline-size ]`, one member of the `||`. */
#define CN_SIZES (CSS_CONTAINER_TYPE_SIZE | CSS_CONTAINER_TYPE_INLINE_SIZE)

static bool cn_word_is(const char *p, size_t len, const char *word)
{
    return strlen(word) == len && strncasecmp(p, word, len) == 0;
}

/* The next whitespace-separated word of `*pp` into `*w`/`*len`, advancing past it; false at the end. */
static bool cn_word(const char **pp, const char **w, size_t *len)
{
    const char *p = *pp;
    size_t n;

    while (*p != '\0' && isspace((unsigned char)*p)) p++;
    if (*p == '\0') { *pp = p; return false; }
    for (n = 0; p[n] != '\0' && !isspace((unsigned char)p[n]); n++) { }
    *w = p;
    *len = n;
    *pp = p + n;
    return true;
}

bool css_container_type_parse(const char *text, unsigned *bits)
{
    const char *p = text, *w;
    size_t len;
    unsigned out = 0, words = 0;
    bool normal = false;

    DCHECK(text != NULL && bits != NULL, "a `container-type` value was parsed with no text or nowhere to put it");
    while (cn_word(&p, &w, &len)) {
        unsigned bit;

        /* `normal` is an alternative of the whole value, so it stands alone. */
        if (normal) return false;
        if (words == 0 && cn_word_is(w, len, "normal")) normal = true;
        else {
            if (cn_word_is(w, len, "size")) bit = CSS_CONTAINER_TYPE_SIZE;
            else if (cn_word_is(w, len, "inline-size")) bit = CSS_CONTAINER_TYPE_INLINE_SIZE;
            else if (cn_word_is(w, len, "scroll-state")) bit = CSS_CONTAINER_TYPE_SCROLL_STATE;
            else return false;
            /* `||` takes each member at most once, and `size` and `inline-size` are one member's two
               alternatives. */
            if (out & bit) return false;
            out |= bit;
            if ((out & CN_SIZES) == CN_SIZES) return false;
        }
        words++;
    }
    if (words == 0) return false;
    *bits = out;
    return true;
}

char *css_container_type_serialize(unsigned bits)
{
    char buf[48];
    char *out;

    DCHECK((bits & ~(CSS_CONTAINER_TYPE_SIZE | CSS_CONTAINER_TYPE_INLINE_SIZE | CSS_CONTAINER_TYPE_SCROLL_STATE)) == 0 &&
               (bits & CN_SIZES) != CN_SIZES,
           "a `container-type` set holds a member §5.1's grammar has none of, or both of `size` and `inline-size`");
    if (bits == 0) snprintf(buf, sizeof buf, "normal");
    else
        snprintf(buf, sizeof buf, "%s%s%s",
                 (bits & CSS_CONTAINER_TYPE_SIZE) ? "size" : (bits & CSS_CONTAINER_TYPE_INLINE_SIZE) ? "inline-size" : "",
                 (bits & CN_SIZES) && (bits & CSS_CONTAINER_TYPE_SCROLL_STATE) ? " " : "",
                 (bits & CSS_CONTAINER_TYPE_SCROLL_STATE) ? "scroll-state" : "");
    out = reclaim_strdup(buf);
    CHECK(out != NULL, "cssom: OOM serializing a `container-type` value");
    return out;
}

/* CSS Syntax §4.3.10 "Check if three code points would start an ident sequence" and §4.3.11's ident code
   points over the whole word, which is what makes it one `<ident-token>`.
   Named residual. Not covered: an escaped code point (§4.3.7 "Consume an escaped code point"), which this word
   test refuses as not an identifier. Next: the name decoded through CSS Syntax's tokenizer, as
   core/css/css_at_rule_prelude.c decodes an `@container` name. How its absence shows: a `container-name`
   written with a backslash escape is dropped, so no `@container` rule naming it selects that element. */
static bool cn_ident(const char *w, size_t len)
{
    const char *end = w + len, *p = w;
    size_t n;
    uint32_t cp = css_cp_at(p, end, &n);

    if (cp == '-') {
        uint32_t next = css_cp_at(p + n, end, NULL);

        if (next != '-' && !css_cp_is_ident_start(next)) return false;
    } else if (!css_cp_is_ident_start(cp)) {
        return false;
    }
    for (; p < end; p += n) {
        cp = css_cp_at(p, end, &n);
        if (!css_cp_is_ident(cp)) return false;
    }
    return true;
}

char *css_container_name_specified(const char *text)
{
    const char *p = text, *w;
    size_t len, at = 0, cap = strlen(text) * 4 + 8;
    char *out;
    bool none = false;

    DCHECK(text != NULL, "a `container-name` value was parsed with no text");
    out = reclaim_malloc(cap);
    CHECK(out != NULL, "cssom: OOM serializing a `container-name` value");
    while (cn_word(&p, &w, &len)) {
        char *raw, *ser;
        bool excluded;

        if (none) goto refuse;
        if (at == 0 && cn_word_is(w, len, "none")) {
            none = true;
            continue;
        }
        if (!cn_ident(w, len)) goto refuse;
        raw = reclaim_strndup(w, len);
        CHECK(raw != NULL, "cssom: OOM reading a `container-name` value");
        /* CSS Values 4 §4.2's exclusions, and §5.2's own: "The keywords none, and, not, and or are excluded
           from this <custom-ident>." */
        excluded = css_custom_ident_excluded(raw) || cn_word_is(w, len, "none") || cn_word_is(w, len, "and") ||
                   cn_word_is(w, len, "not") || cn_word_is(w, len, "or");
        ser = excluded ? NULL : css_serialize_identifier(raw, len);
        free(raw);
        if (ser == NULL) goto refuse;
        DCHECK(at + strlen(ser) + 2 <= cap, "a serialized `container-name` outgrew four bytes per source byte");
        at += (size_t)snprintf(out + at, cap - at, "%s%s", at ? " " : "", ser);
        free(ser);
    }
    if (none) {
        snprintf(out, cap, "none");
        return out;
    }
    if (at == 0) goto refuse;
    return out;
refuse:
    free(out);
    return NULL;
}

bool css_container_name_has(const char *names, const char *name)
{
    const char *p = names, *w;
    size_t len, n;

    DCHECK(names != NULL && name != NULL && name[0] != '\0',
           "a `<container-name>` was looked up with no computed `container-name` or no name");
    if (strcmp(names, "none") == 0) return false;
    n = strlen(name);
    while (cn_word(&p, &w, &len))
        if (len == n && memcmp(w, name, n) == 0) return true;
    return false;
}

char *css_container_shorthand_component(const char *value, const char *longhand)
{
    const char *slash;
    char *raw, *name;
    unsigned bits = 0;

    DCHECK(value != NULL && longhand != NULL, "the `container` shorthand was expanded with no value or longhand");
    DCHECKF(strcmp(longhand, "container-name") == 0 || strcmp(longhand, "container-type") == 0,
            "`%s` is not one of §5.3's two longhands", longhand);
    slash = strchr(value, '/');
    if (slash != NULL && strchr(slash + 1, '/') != NULL) return NULL;
    raw = reclaim_strndup(value, slash ? (size_t)(slash - value) : strlen(value));
    CHECK(raw != NULL, "cssom: OOM expanding a `container` value");
    name = css_container_name_specified(raw);
    free(raw);
    if (name == NULL) return NULL;
    if (slash != NULL && !css_container_type_parse(slash + 1, &bits)) { free(name); return NULL; }
    if (strcmp(longhand, "container-name") == 0) return name;
    free(name);
    return css_container_type_serialize(bits);
}

char *css_container_shorthand_serialize(const char *const *values)
{
    unsigned bits = 0;
    size_t n;
    char *out;
    bool ok;

    DCHECK(values != NULL && values[0] != NULL && values[1] != NULL,
           "the `container` shorthand was serialized without both longhands' values");
    ok = css_container_type_parse(values[1], &bits);
    DCHECKF(ok, "`container-type: %s` is not in the grammar its own expansion writes", values[1]);
    /* §6.7.2's omission: a `<'container-type'>` at its initial `normal` is what an omitted one resets to. */
    if (bits == 0) {
        out = reclaim_strdup(values[0]);
        CHECK(out != NULL, "cssom: OOM serializing a `container` value");
        return out;
    }
    n = strlen(values[0]) + strlen(values[1]) + 4;
    out = reclaim_malloc(n);
    CHECK(out != NULL, "cssom: OOM serializing a `container` value");
    snprintf(out, n, "%s / %s", values[0], values[1]);
    return out;
}
