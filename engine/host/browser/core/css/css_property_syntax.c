/* CSS Properties and Values API 1 §5.4 "Parsing The Syntax String". See css_property_syntax.h for why this is a
 * component, why it hands out §5.4.1's syntax definition but nothing that reads a CSS value, and why a data
 * type name is matched codepoint-wise. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "core/css/css_code_point.h"
#include "core/css/css_defaulting.h"
#include "core/css/css_property_syntax.h"

/* §5.4.2 step 4's "input stream created from the code points of string". `i` is the BYTE offset of the NEXT
   input code point — this engine holds decoded source as UTF-8 rather than as an array of code points, so a
   position is an offset and the code point at one is core/css/css_code_point.h's answer — and "reconsume" is
   simply not advancing it. */
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

/* ONE CODE POINT, UTF-8 ENCODED — EVERY code point this file appends goes through here, and the byte-copying
   sibling that stood beside it is deleted. That sibling copied one SOURCE BYTE on the argument that a
   non-ASCII ident code point arrives as the bytes it was written with, which was true only while the stream
   was read as bytes; §3.3 is what retires it, because a filtered code point and the bytes it stands for are
   not the same text — a CRLF is two bytes and one U+000A, a U+0000 NULL is one byte and a U+FFFD, and an
   ill-formed sequence is bytes that spell no code point at all. Re-encoding the ANSWER is byte-identical to
   the source wherever §3.3 changed nothing, and is the browser's text wherever it did.
     IT IS ALSO WHAT §5.4.3's ESCAPE ARM NEEDS: the name is compared against CSS Values §4.2's excluded
   keywords and `\69 nherit` spells `inherit`, so an escape is a SPELLING and not a different identifier —
   the same sentence CSS Cascade §6.4.2's `<layer-name>` refusal turns on. */
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

/* The code point `k` code points ahead of the cursor, or §4.2's EOF code point past the end of the string,
   with `*n_out` — when it is not NULL — the number of BYTES that answer stands for.
     §5.4.2 step 4 preprocesses the stream as CSS Syntax §3.3 "Preprocessing the input stream" does, and that
   is now a fact about this walk rather than a claim about its callers: css_cp_at applies §3.3, so a U+000D
   CARRIAGE RETURN and a U+000C FORM FEED ARE a U+000A LINE FEED here, a CRLF pair is ONE of them standing for
   two bytes, and a U+0000 NULL is a U+FFFD. Reading past the end answers EOF again without moving. */
static uint32_t syn_at_n(const SynStream *p, size_t k, size_t *n_out)
{
    const char *end = p->s + p->len;
    const char *q = p->s + p->i;
    uint32_t cp;
    size_t n = 0;

    DCHECK(p->i <= p->len,
           "§5.4.2's stream cursor is past the end of its own string — every advance moves by the bytes a "
           "filtered code point stood for, so an overshoot is a byte count that did not come from the walk");
    for (;;) {
        cp = css_cp_at(q, end, &n);
        if (k == 0 || cp == CSS_CP_EOF) break;
        q += n;
        k--;
    }
    if (n_out) *n_out = n;
    return cp;
}

static uint32_t syn_at(const SynStream *p, size_t k) { return syn_at_n(p, k, NULL); }

/* CONSUME the next input code point. The cursor moves by the bytes the ANSWER stood for, which is not always
   the bytes at the cursor — §3.3's CRLF is two of them for one U+000A. */
static void syn_adv(SynStream *p)
{
    size_t n;
    uint32_t cp = syn_at_n(p, 0, &n);

    DCHECK(cp != CSS_CP_EOF,
           "§5.4.2's stream was advanced past the end of the syntax string — every consume below is guarded "
           "by the code point it consumes, so an EOF here is an arm that decided on a code point it had not "
           "looked at");
    p->i += n;
}

/* Infra's ASCII whitespace, which is what §5.4.2 step 1 strips. It is asked BOTH of a filtered code point and
   of a raw byte (step 1 runs over the string before a stream exists), and it is exact for both: all five are
   ASCII, so each is one byte in UTF-8 and neither can occur as a continuation byte. */
static bool syn_is_ws(uint32_t c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }
/* §4.2's newline: "U+000A LINE FEED." and nothing else. §4.2's own note says why the other two are absent —
   "Note that U+000D CARRIAGE RETURN and U+000C FORM FEED are not included in this definition, as they are
   converted to U+000A LINE FEED during preprocessing" — and this file's only caller is §4.3.8, which asks it
   of a code point css_cp_at has already filtered. Naming all three here would be a second copy of §3.3 in the
   one place §3.3 has already run. */
static bool syn_is_newline(uint32_t c) { return c == '\n'; }

static bool syn_is_hex(uint32_t c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

static uint32_t syn_hex_val(uint32_t c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return c - 'A' + 10;
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
    uint32_t c = syn_at(p, 0);

    if (c == '-')
        return css_cp_is_ident_start(syn_at(p, 1)) || syn_at(p, 1) == '-' || syn_valid_escape(p, 1);
    if (css_cp_is_ident_start(c)) return true;
    if (c == '\\') return syn_valid_escape(p, 0);
    return false;
}

/* CSS Syntax §4.3.7 "consume an escaped code point", with the U+005C already consumed. */
static uint32_t syn_consume_escape(SynStream *p)
{
    uint32_t c = syn_at(p, 0);
    uint32_t v = 0;
    unsigned n;

    if (c == CSS_CP_EOF) return 0xFFFD;             /* "EOF: This is a parse error. Return U+FFFD." */
    /* "anything else: Return the current input code point." — the CODE POINT, so `\é` is one U+00E9 and not
       the first byte of its encoding, which is what the byte-view stream returned here. */
    if (!syn_is_hex(c)) { syn_adv(p); return c; }
    /* "Consume as many hex digits as possible, but no more than 5 more (i.e., 6 total). If the next input code
       point is whitespace, consume it as well." */
    for (n = 0; n < 6 && syn_is_hex(syn_at(p, 0)); n++) {
        v = v * 16 + syn_hex_val(syn_at(p, 0));
        syn_adv(p);
    }
    if (syn_is_ws(syn_at(p, 0))) syn_adv(p);
    /* "If this number is zero, or is for a surrogate, or is greater than the maximum allowed code point, return
       U+FFFD REPLACEMENT CHARACTER." */
    if (v == 0 || (v >= 0xD800 && v <= 0xDFFF) || v > 0x10FFFF) return 0xFFFD;
    return v;
}

/* CSS Syntax §4.3.12 "Consume an ident sequence", appending into `out`. */
static void syn_consume_ident(SynStream *p, SynBuf *out)
{
    for (;;) {
        uint32_t c = syn_at(p, 0);

        if (css_cp_is_ident(c)) { syn_buf_add_cp(out, c); syn_adv(p); continue; }
        if (syn_valid_escape(p, 0)) { syn_adv(p); syn_buf_add_cp(out, syn_consume_escape(p)); continue; }
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
        uint32_t c = syn_at(p, 0);

        if (c == '>') {
            syn_adv(p);
            syn_buf_add(out, ">", 1);
            return syn_supported_name(out->s);
        }
        if (!css_cp_is_ident(c)) return false;      /* "anything else: Return failure." (EOF included) */
        syn_buf_add_cp(out, c);
        syn_adv(p);
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
    uint32_t c;

    while (syn_is_ws(syn_at(p, 0))) syn_adv(p);     /* "Consume as much whitespace as possible" */
    c = syn_at(p, 0);
    if (c == '<') {
        syn_adv(p);
        if (!syn_consume_data_type(p, &name)) goto fail;
    } else if (css_cp_is_ident_start(c) || c == '\\') {
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
    if (syn_at(p, 0) == '+') { mult = CSS_SYNTAX_MULT_SPACE; syn_adv(p); }
    else if (syn_at(p, 0) == '#') { mult = CSS_SYNTAX_MULT_COMMA; syn_adv(p); }

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
        while (syn_is_ws(syn_at(&p, 0))) syn_adv(&p);
        /* Step 7: "EOF: return definition. U+007C VERTICAL LINE (|): Repeat step 5. Anything else: Return
           failure." */
        if (syn_at(&p, 0) == CSS_CP_EOF) {
            DCHECK(out->n > 0 && !out->universal,
                   "§5.4.2 returned a syntax definition that is neither §5.4.1's universal one nor a list of "
                   "syntax components — step 5 runs at least once before step 7 can be reached, so an empty "
                   "non-universal definition is a component that was consumed and then not appended");
            return true;
        }
        if (syn_at(&p, 0) != '|') goto fail;
        syn_adv(&p);
    }

fail:
    /* THE PARTIAL DEFINITION IS TORN DOWN HERE and not left for the caller: §5.4.2's failure is not a
       definition at all, so `<length> | <bogus>` must leave nothing allocated behind its first component. */
    css_syntax_definition_free(out);
    return false;
}
