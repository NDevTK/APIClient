/* CSS Properties and Values API 1 §5.4 "Parsing The Syntax String". See css_property_syntax.h for why this is a
 * component, why it hands out §5.4.1's syntax definition but nothing that reads a CSS value, and why a data
 * type name is matched codepoint-wise. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "core/css/css_defaulting.h"
#include "core/css/css_property_syntax.h"

/* §5.4.2 step 4's "input stream created from the code points of string". `i` is the position of the NEXT input
   code point, so "reconsume" is simply not advancing it. */
typedef struct { const char *s; size_t len, i; } SynStream;

/* THE NAME A COMPONENT IS BEING BUILT FROM. §5.4.3 and §5.4.4 both append code point by code point, and §5.4.3's
   arm appends an UNESCAPED one — `I\ dent` is the six-code-point name `I dent` — which is why this is a buffer
   and not a span of the source. */
typedef struct { char *s; size_t len, cap; } SynBuf;

static void syn_buf_add(SynBuf *b, const char *s, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 32;
        char *grown;

        while (cap < b->len + n + 1) cap *= 2;
        grown = realloc(b->s, cap);
        CHECK(grown != NULL, "cssom: OOM building a syntax component's name");
        b->s = grown;
        b->cap = cap;
    }
    memcpy(b->s + b->len, s, n);
    b->len += n;
    b->s[b->len] = '\0';
}

/* ONE SOURCE BYTE, VERBATIM. A non-ASCII ident code point arrives as the UTF-8 bytes it was written with, so
   copying the byte is what preserves it — re-encoding each byte as though it were a code point would turn one
   two-byte character into two two-byte sequences. */
static void syn_buf_add_byte(SynBuf *b, int c)
{
    char one = (char)c;

    syn_buf_add(b, &one, 1);
}

/* ONE ESCAPED CODE POINT, UTF-8 ENCODED — the ONLY place a code point is encoded rather than copied, because it
   is the only place one is produced out of something other than itself. §5.4.3's name is compared against CSS
   Values §4.2's excluded keywords and `\69 nherit` spells `inherit`: an escape is a SPELLING and not a
   different identifier, which is the same sentence CSS Cascade §6.4.2's `<layer-name>` refusal turns on. */
static void syn_buf_add_cp(SynBuf *b, uint32_t cp)
{
    char u[4];

    if (cp < 0x80) { u[0] = (char)cp; syn_buf_add(b, u, 1); return; }
    if (cp < 0x800) {
        u[0] = (char)(0xC0 | (cp >> 6));
        u[1] = (char)(0x80 | (cp & 0x3F));
        syn_buf_add(b, u, 2);
        return;
    }
    if (cp < 0x10000) {
        u[0] = (char)(0xE0 | (cp >> 12));
        u[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        u[2] = (char)(0x80 | (cp & 0x3F));
        syn_buf_add(b, u, 3);
        return;
    }
    u[0] = (char)(0xF0 | (cp >> 18));
    u[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    u[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    u[3] = (char)(0x80 | (cp & 0x3F));
    syn_buf_add(b, u, 4);
}

/* The code point at `i + k`, or -1 for EOF. §5.4.2 step 4 preprocesses the stream as CSS Syntax §3.3 does, so a
   U+000D CARRIAGE RETURN and a U+000C FORM FEED have already become U+000A LINE FEED by the time any arm below
   looks at one — which matters in exactly one place, §4.3.8's "if the second code point is a newline, return
   false", and is why the newline test names all three. */
static int syn_at(const SynStream *p, size_t k)
{
    return (p->i + k < p->len) ? (unsigned char)p->s[p->i + k] : -1;
}

static bool syn_is_ws(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }
static bool syn_is_newline(int c) { return c == '\n' || c == '\r' || c == '\f'; }

/* CSS Syntax §4.2's ident-start code point: "A letter, a non-ASCII ident code point, or U+005F LOW LINE (_)".
   Every non-ASCII code point is a non-ASCII ident code point in this build's byte view, and a UTF-8 lead or
   continuation byte is >= 0x80, so the test is on bytes and never on decoded code points. */
static bool syn_is_ident_start(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                                               c == '_' || c >= 0x80; }
/* §4.2's ident code point: "An ident-start code point, a digit, or U+002D HYPHEN-MINUS (-)". */
static bool syn_is_ident(int c) { return syn_is_ident_start(c) || (c >= '0' && c <= '9') || c == '-'; }
static bool syn_is_hex(int c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

static uint32_t syn_hex_val(int c)
{
    if (c >= '0' && c <= '9') return (uint32_t)(c - '0');
    if (c >= 'a' && c <= 'f') return (uint32_t)(c - 'a' + 10);
    return (uint32_t)(c - 'A' + 10);
}

/* CSS Syntax §4.3.8 "check if two code points are a valid escape", at `i + k`: "If the first code point is not
   U+005C REVERSE SOLIDUS (\), return false. Otherwise, if the second code point is a newline, return false.
   Otherwise, return true." EOF is not a newline, so `\` at the very end IS a valid escape and §4.3.7's EOF arm
   is what then answers it. */
static bool syn_valid_escape(const SynStream *p, size_t k)
{
    return syn_at(p, k) == '\\' && !syn_is_newline(syn_at(p, k + 1));
}

/* CSS Syntax §4.3.9 "check if three code points would start an ident sequence", at the cursor. */
static bool syn_starts_ident(const SynStream *p)
{
    int c = syn_at(p, 0);

    if (c == '-')
        return syn_is_ident_start(syn_at(p, 1)) || syn_at(p, 1) == '-' || syn_valid_escape(p, 1);
    if (syn_is_ident_start(c)) return true;
    if (c == '\\') return syn_valid_escape(p, 0);
    return false;
}

/* CSS Syntax §4.3.7 "consume an escaped code point", with the U+005C already consumed. */
static uint32_t syn_consume_escape(SynStream *p)
{
    int c = syn_at(p, 0);
    uint32_t v = 0;
    unsigned n;

    if (c < 0) return 0xFFFD;                       /* "EOF: This is a parse error. Return U+FFFD." */
    if (!syn_is_hex(c)) { p->i++; return (uint32_t)c; }
    /* "Consume as many hex digits as possible, but no more than 5 more (i.e., 6 total). If the next input code
       point is whitespace, consume it as well." */
    for (n = 0; n < 6 && syn_is_hex(syn_at(p, 0)); n++) {
        v = v * 16 + syn_hex_val(syn_at(p, 0));
        p->i++;
    }
    if (syn_is_ws(syn_at(p, 0))) p->i++;
    /* "If this number is zero, or is for a surrogate, or is greater than the maximum allowed code point, return
       U+FFFD REPLACEMENT CHARACTER." */
    if (v == 0 || (v >= 0xD800 && v <= 0xDFFF) || v > 0x10FFFF) return 0xFFFD;
    return v;
}

/* CSS Syntax §4.3.12 "Consume an ident sequence", appending into `out`. */
static void syn_consume_ident(SynStream *p, SynBuf *out)
{
    for (;;) {
        int c = syn_at(p, 0);

        if (syn_is_ident(c)) { syn_buf_add_byte(out, c); p->i++; continue; }
        if (syn_valid_escape(p, 0)) { p->i++; syn_buf_add_cp(out, syn_consume_escape(p)); continue; }
        return;
    }
}

/* §5.1 "Supported Names" — the fifteen, spelled as §5.4.4 builds them (the angle brackets included, because the
   algorithm appends both and then asks whether what it built is one of these). */
static const char *const SYN_SUPPORTED[] = {
    "<length>", "<number>", "<percentage>", "<length-percentage>", "<string>", "<color>", "<image>", "<url>",
    "<integer>", "<angle>", "<time>", "<resolution>", "<transform-function>", "<custom-ident>",
    "<transform-list>",
};

static bool syn_supported_name(const char *name)
{
    unsigned i;

    for (i = 0; i < sizeof(SYN_SUPPORTED) / sizeof(SYN_SUPPORTED[0]); i++)
        if (strcmp(SYN_SUPPORTED[i], name) == 0) return true;
    return false;
}

/* §5.4.1's PRE-MULTIPLIED DATA TYPE NAME — "a data type name that represents another syntax component with a
   multiplier already included". §5.1 names the one there is: "<transform-list> is a pre-multiplied data type
   name equivalent to <transform-function>+". §5.2 is what makes the distinction load-bearing: "any syntax
   component name EXCEPT pre-multiplied data type names may be immediately followed by a multiplier", so
   `<transform-list>+` is not a syntax string. */
static bool syn_premultiplied(const char *name) { return strcmp(name, "<transform-list>") == 0; }

/* §5.4.4 "Consume a Data Type Name", with the U+003C already consumed. Returns false for the algorithm's own
   failure; on true `out` holds the name with both angle brackets. */
static bool syn_consume_data_type(SynStream *p, SynBuf *out)
{
    syn_buf_add(out, "<", 1);
    for (;;) {
        int c = syn_at(p, 0);

        if (c == '>') {
            p->i++;
            syn_buf_add(out, ">", 1);
            return syn_supported_name(out->s);
        }
        if (!syn_is_ident(c)) return false;         /* "anything else: Return failure." (EOF included) */
        syn_buf_add_byte(out, c);
        p->i++;
    }
}

/* APPEND ONE COMPONENT to §5.4.2 step 4's "definition", which that step makes "an initially empty list of
   syntax components", TAKING OWNERSHIP of `name`. The vector is grown to its
   exact length rather than doubled: §5.3's combinator is what adds components and a syntax string names a
   handful of them, so a capacity field would be state to keep correct for no allocation saved. */
static void syn_def_push(CssSyntaxDefinition *d, char *name, CssSyntaxMultiplier m)
{
    CssSyntaxComponent *grown = realloc(d->v, (d->n + 1) * sizeof *d->v);

    CHECK(grown != NULL, "cssom: OOM appending a syntax component to a syntax definition");
    DCHECK(name != NULL && name[0] != '\0',
           "§5.4.3 produced a syntax component with no name — both of its arms append at least one code point "
           "before they can succeed, so an empty name is an arm that returned without building one");
    d->v = grown;
    d->v[d->n].name = name;
    d->v[d->n].multiplier = m;
    d->n++;
}

/* §5.4.3 "Consume a Syntax Component", appending the component it produced to `out` — which is §5.4.2 step 5's
   own second half ("otherwise, append the returned value to definition"), done here because the component's
   name buffer is owned here and handing it back would be one more transfer to get wrong. */
static bool syn_consume_component(SynStream *p, CssSyntaxDefinition *out)
{
    SynBuf name = { NULL, 0, 0 };
    CssSyntaxMultiplier mult = CSS_SYNTAX_MULT_NONE;
    int c;

    while (syn_is_ws(syn_at(p, 0))) p->i++;         /* "Consume as much whitespace as possible" */
    c = syn_at(p, 0);
    if (c == '<') {
        p->i++;
        if (!syn_consume_data_type(p, &name)) goto fail;
    } else if (syn_is_ident_start(c) || c == '\\') {
        /* "If the stream starts with an ident sequence, reconsume the current input code point from stream then
           consume an ident sequence from stream, and set component's name to the returned value. Otherwise
           return failure." The cursor has not moved, so the reconsume is nothing to undo. */
        if (!syn_starts_ident(p)) goto fail;
        syn_consume_ident(p, &name);
        DCHECK(name.s != NULL && name.s[0] != '\0',
               "a syntax component's ident sequence produced nothing — §4.3.9 answered that the stream starts "
               "with one, and §4.3.12 appends at least the code point that made it say so");
        /* "If component's name does not parse as a <custom-ident>, return failure." */
        if (css_custom_ident_excluded(name.s)) goto fail;
    } else {
        goto fail;                                  /* "anything else: Return failure." */
    }
    /* "If component's name is a pre-multiplied data type name, return component." — BEFORE the multiplier is
       looked at, which is what makes `<transform-list>#` fail at step 5.4.2's own "anything else" instead. */
    if (syn_premultiplied(name.s)) goto keep;
    /* §5.2's two, "immediately after the syntax component name being multiplied" — no whitespace is consumed
       first, which is what that sentence is for. */
    if (syn_at(p, 0) == '+') { mult = CSS_SYNTAX_MULT_SPACE; p->i++; }
    else if (syn_at(p, 0) == '#') { mult = CSS_SYNTAX_MULT_COMMA; p->i++; }

keep:
    syn_def_push(out, name.s, mult);                /* TAKES name.s */
    return true;

fail:
    free(name.s);
    return false;
}

void css_syntax_definition_free(CssSyntaxDefinition *d)
{
    size_t i;

    if (!d) return;
    for (i = 0; i < d->n; i++) free(d->v[i].name);
    free(d->v);
    d->v = NULL;
    d->n = 0;
    d->universal = false;
}

bool css_property_syntax_definition(const char *s, size_t len, CssSyntaxDefinition *out)
{
    SynStream p;
    size_t begin = 0, end = len;

    DCHECK(s != NULL, "§5.4.2's consume a syntax definition was called with no string");
    DCHECK(out != NULL,
           "§5.4.2's two outcomes were asked for through one answer — a list of syntax components and the "
           "UNIVERSAL syntax definition are different definitions, and §3.3 reads exactly that difference");
    out->v = NULL;
    out->n = 0;
    out->universal = false;
    /* Step 1: "Strip leading and trailing ASCII whitespace from string." */
    while (begin < end && syn_is_ws((unsigned char)s[begin])) begin++;
    while (end > begin && syn_is_ws((unsigned char)s[end - 1])) end--;
    /* Step 2: "If string's length is 0, return failure." */
    if (end == begin) return false;
    /* Step 3: "If string's length is 1, and the only code point in string is U+002A ASTERISK (*), return the
       universal syntax definition." A `*` beside anything else is NOT it — §5.1's note says so outright
       ("therefore, "*" may not be multiplied or combined with anything else") — and it is not a component
       either, so `* | <length>` falls through to step 5 and fails there. */
    if (end - begin == 1 && s[begin] == '*') { out->universal = true; return true; }
    p.s = s;
    p.len = end;
    p.i = begin;
    for (;;) {
        /* Step 5: "Consume a syntax component from stream. If failure was returned, return failure; otherwise,
           append the returned value to definition." */
        if (!syn_consume_component(&p, out)) goto fail;
        /* Step 6: "Consume as much whitespace as possible from stream." */
        while (syn_is_ws(syn_at(&p, 0))) p.i++;
        /* Step 7: "EOF: return definition. U+007C VERTICAL LINE (|): Repeat step 5. Anything else: Return
           failure." */
        if (syn_at(&p, 0) < 0) {
            DCHECK(out->n > 0 && !out->universal,
                   "§5.4.2 returned a syntax definition that is neither §5.4.1's universal one nor a list of "
                   "syntax components — step 5 runs at least once before step 7 can be reached, so an empty "
                   "non-universal definition is a component that was consumed and then not appended");
            return true;
        }
        if (syn_at(&p, 0) != '|') goto fail;
        p.i++;
    }

fail:
    /* THE PARTIAL DEFINITION IS TORN DOWN HERE and not left for the caller: §5.4.2's failure is not a
       definition at all, so `<length> | <bogus>` must leave nothing allocated behind its first component. */
    css_syntax_definition_free(out);
    return false;
}
