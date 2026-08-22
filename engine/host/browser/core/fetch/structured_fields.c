/* RFC 9651 structured field values, and Fetch §2.2.2's get over a header list. See structured_fields.h. */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "core/fetch/structured_fields.h"

/* THE CURSOR THE RFC IS WRITTEN OVER. Every §4.2 algorithm is stated as "while input_string is not empty:
   char = consume the first character", so the parser is one cursor handed down and never a set of slices —
   which is also why a sub-parse that fails leaves the cursor wherever it stopped: the whole parse fails with
   it and nothing reads a position afterwards. */
typedef struct { const char *s; size_t n, i; } SfIn;

static bool sf_eof(const SfIn *in) { return in->i >= in->n; }
static char sf_peek(const SfIn *in) { return in->s[in->i]; }
static char sf_take(SfIn *in) { return in->s[in->i++]; }

/* §4.2 steps 2 and 4 "discard any leading SP characters" — SP, and NOT the HTTP whitespace set. A leading tab
   makes the field fail to parse, which is the RFC's answer and not an oversight of this parser. */
static void sf_skip_sp(SfIn *in) { while (in->i < in->n && in->s[in->i] == ' ') in->i++; }

static bool sf_digit(char c)   { return c >= '0' && c <= '9'; }
static bool sf_alpha(char c)   { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static bool sf_lcalpha(char c) { return c >= 'a' && c <= 'z'; }
static bool sf_lchex(char c)   { return sf_digit(c) || (c >= 'a' && c <= 'f'); }

/* RFC 9110's tchar, which §4.2.6's token is built from together with ":" and "/". */
static bool sf_tchar(char c)
{
    return sf_digit(c) || sf_alpha(c) || c == '!' || c == '#' || c == '$' || c == '%' || c == '&' ||
           c == '\'' || c == '*' || c == '+' || c == '-' || c == '.' || c == '^' || c == '_' ||
           c == '`' || c == '|' || c == '~';
}

/* RFC 4648 §4's alphabet, which §4.2.7 rejects anything outside of. */
static bool sf_b64char(char c)
{
    return sf_digit(c) || sf_alpha(c) || c == '+' || c == '/';
}

/* ---- an owned, growable byte buffer, for the two kinds whose value is BUILT rather than spanned ---------- */

typedef struct { char *p; size_t n, cap; } SfBuf;

static void sf_buf_put(SfBuf *b, char c)
{
    if (b->n + 2 > b->cap) {
        b->cap = b->cap ? b->cap * 2 : 32;
        b->p = realloc(b->p, b->cap);
        CHECK(b->p != NULL, "structured fields: OOM building a field value");
    }
    b->p[b->n++] = c;
    b->p[b->n] = 0;
}

static char *sf_span_dup(const char *s, size_t n)
{
    char *out = malloc(n + 1);
    CHECK(out != NULL, "structured fields: OOM copying a field value");
    memcpy(out, s, n);
    out[n] = 0;
    return out;
}

/* ---- §4.2.4 Parsing an Integer or Decimal ---------------------------------------------------------------- */

static bool sf_parse_number(SfIn *in, SfBareItem *out)
{
    char buf[24];
    size_t len = 0;
    int sign = 1;
    bool is_decimal = false;

    if (!sf_eof(in) && sf_peek(in) == '-') { sf_take(in); sign = -1; }
    if (sf_eof(in) || !sf_digit(sf_peek(in))) return false;

    while (!sf_eof(in)) {
        char c = sf_peek(in);
        if (sf_digit(c)) {
            sf_take(in);
            if (len >= sizeof buf - 1) return false;
            buf[len++] = c;
        } else if (!is_decimal && c == '.') {
            /* §4.2.4: "if input_number contains more than 12 characters, fail parsing" — the integer part of
               a decimal is bounded before the point, not after it. */
            if (len > 12) return false;
            sf_take(in);
            buf[len++] = '.';
            is_decimal = true;
        } else {
            break;   /* the RFC's "prepend char to input_string" — this cursor simply did not consume it */
        }
        if (!is_decimal && len > 15) return false;
        if (is_decimal && len > 16) return false;
    }
    buf[len] = 0;

    if (!is_decimal) {
        out->kind = SF_INTEGER;
        out->integer = (int64_t)sign * (int64_t)strtoll(buf, NULL, 10);
        return true;
    }
    /* §4.2.4: "if the final character of input_number is '.', fail" and "if the number of characters after
       '.' is greater than three, fail". */
    if (buf[len - 1] == '.') return false;
    {
        const char *dot = strchr(buf, '.');
        DCHECK(dot != NULL, "a decimal was parsed with no decimal point — the flag and the buffer disagree");
        if (strlen(dot + 1) > 3) return false;
    }
    out->kind = SF_DECIMAL;
    out->decimal = (double)sign * strtod(buf, NULL);
    return true;
}

/* ---- §4.2.5 Parsing a String ----------------------------------------------------------------------------- */

static bool sf_parse_string(SfIn *in, SfBareItem *out)
{
    SfBuf b;

    memset(&b, 0, sizeof b);
    DCHECK(!sf_eof(in) && sf_peek(in) == '"', "the string parser was entered on something that is not a DQUOTE");
    sf_take(in);
    while (!sf_eof(in)) {
        char c = sf_take(in);
        if (c == '\\') {
            char d;
            if (sf_eof(in)) { free(b.p); return false; }
            d = sf_take(in);
            /* §4.2.5: the ONLY two escapable characters. `\n` is not one of them — a string carrying it fails
               to parse rather than carrying a newline, which is what keeps a header value one line. */
            if (d != '"' && d != '\\') { free(b.p); return false; }
            sf_buf_put(&b, d);
        } else if (c == '"') {
            out->kind = SF_STRING;
            out->text = b.p ? b.p : sf_span_dup("", 0);
            return true;
        } else if ((unsigned char)c <= 0x1f || (unsigned char)c >= 0x7f) {
            free(b.p);
            return false;
        } else {
            sf_buf_put(&b, c);
        }
    }
    free(b.p);
    return false;   /* the closing DQUOTE never arrived */
}

/* ---- §4.2.6 Parsing a Token ------------------------------------------------------------------------------ */

static bool sf_parse_token(SfIn *in, SfBareItem *out)
{
    size_t start;

    if (sf_eof(in) || !(sf_alpha(sf_peek(in)) || sf_peek(in) == '*')) return false;
    start = in->i;
    while (!sf_eof(in) && (sf_tchar(sf_peek(in)) || sf_peek(in) == ':' || sf_peek(in) == '/')) in->i++;
    out->kind = SF_TOKEN;
    out->text = sf_span_dup(in->s + start, in->i - start);
    return true;
}

/* ---- §4.2.7 Parsing a Byte Sequence ---------------------------------------------------------------------- */

static bool sf_parse_byte_sequence(SfIn *in, SfBareItem *out)
{
    size_t start, end, pad = 0, k;

    DCHECK(!sf_eof(in) && sf_peek(in) == ':', "the byte-sequence parser was entered on something that is not a colon");
    sf_take(in);
    start = in->i;
    while (!sf_eof(in) && sf_peek(in) != ':') in->i++;
    if (sf_eof(in)) return false;   /* §4.2.7: "if input_string does not contain a ':', fail" */
    end = in->i;
    sf_take(in);   /* the closing colon */

    /* §4.2.7 validates the ALPHABET and then requires the base64 to DECODE. The decode itself has no consumer
       (see structured_fields.h), so what is checked here is everything that decides whether it WOULD decode:
       the alphabet, padding only as a trailing run of at most two, and a length that is not 4n+1 — the one
       residue no group of base64 characters can produce. */
    for (k = start; k < end; k++) {
        char c = in->s[k];
        if (c == '=') { pad++; continue; }
        if (pad || !sf_b64char(c)) return false;   /* a character after padding, or outside the alphabet */
    }
    if (pad > 2) return false;
    if ((end - start - pad) % 4 == 1) return false;

    out->kind = SF_BYTE_SEQUENCE;
    out->text = sf_span_dup(in->s + start, end - start);
    return true;
}

/* ---- §4.2.9 Parsing a Boolean ---------------------------------------------------------------------------- */

static bool sf_parse_boolean(SfIn *in, SfBareItem *out)
{
    char c;

    DCHECK(!sf_eof(in) && sf_peek(in) == '?', "the boolean parser was entered on something that is not a '?'");
    sf_take(in);
    if (sf_eof(in)) return false;
    c = sf_take(in);
    if (c != '0' && c != '1') return false;
    out->kind = SF_BOOLEAN;
    out->boolean = c == '1';
    return true;
}

/* ---- §4.2.8 Parsing a Date ------------------------------------------------------------------------------- */

static bool sf_parse_date(SfIn *in, SfBareItem *out)
{
    SfBareItem n;

    DCHECK(!sf_eof(in) && sf_peek(in) == '@', "the date parser was entered on something that is not an '@'");
    sf_take(in);
    memset(&n, 0, sizeof n);
    if (!sf_parse_number(in, &n)) return false;
    /* §4.2.8: "if output_date is not an Integer, fail parsing" — a Date is seconds, never a fraction. */
    if (n.kind != SF_INTEGER) { free(n.text); return false; }
    out->kind = SF_DATE;
    out->integer = n.integer;
    return true;
}

/* ---- §4.2.10 Parsing a Display String -------------------------------------------------------------------- */

/* The decoded octets must be valid UTF-8 (§4.2.10's last step runs UTF-8 decode without BOM and fails on an
   error). This is the walk that decides it, over the bytes the escapes produced. */
static bool sf_utf8_valid(const unsigned char *p, size_t n)
{
    size_t i = 0;

    while (i < n) {
        unsigned char c = p[i];
        size_t need;
        unsigned long cp;

        if (c < 0x80) { i++; continue; }
        if (c >= 0xc2 && c <= 0xdf)      { need = 1; cp = c & 0x1fu; }
        else if (c >= 0xe0 && c <= 0xef) { need = 2; cp = c & 0x0fu; }
        else if (c >= 0xf0 && c <= 0xf4) { need = 3; cp = c & 0x07u; }
        else return false;
        if (i + need >= n) return false;   /* the continuation bytes run past the end */
        {
            size_t k;
            for (k = 1; k <= need; k++) {
                unsigned char t = p[i + k];
                if ((t & 0xc0) != 0x80) return false;
                cp = (cp << 6) | (t & 0x3fu);
            }
        }
        if (need == 2 && cp < 0x800) return false;            /* overlong */
        if (need == 3 && cp < 0x10000) return false;          /* overlong */
        if (cp >= 0xd800 && cp <= 0xdfff) return false;       /* a lone surrogate is not a scalar value */
        if (cp > 0x10ffff) return false;
        i += need + 1;
    }
    return true;
}

static bool sf_parse_display_string(SfIn *in, SfBareItem *out)
{
    SfBuf b;

    memset(&b, 0, sizeof b);
    DCHECK(!sf_eof(in) && sf_peek(in) == '%', "the display-string parser was entered on something that is not a '%'");
    sf_take(in);
    if (sf_eof(in) || sf_peek(in) != '"') return false;
    sf_take(in);
    while (!sf_eof(in)) {
        char c = sf_take(in);
        if ((unsigned char)c <= 0x1f || (unsigned char)c >= 0x7f) { free(b.p); return false; }
        if (c == '%') {
            char h1, h2;
            if (in->i + 1 >= in->n) { free(b.p); return false; }
            h1 = sf_take(in);
            h2 = sf_take(in);
            /* §4.2.10 requires LOWERCASE hex — an uppercase escape fails to parse rather than being folded. */
            if (!sf_lchex(h1) || !sf_lchex(h2)) { free(b.p); return false; }
            {
                int v = (sf_digit(h1) ? h1 - '0' : h1 - 'a' + 10) * 16 +
                        (sf_digit(h2) ? h2 - '0' : h2 - 'a' + 10);
                sf_buf_put(&b, (char)v);
            }
        } else if (c == '"') {
            if (!sf_utf8_valid((const unsigned char *)(b.p ? b.p : ""), b.n)) { free(b.p); return false; }
            out->kind = SF_DISPLAY_STRING;
            out->text = b.p ? b.p : sf_span_dup("", 0);
            return true;
        } else {
            sf_buf_put(&b, c);
        }
    }
    free(b.p);
    return false;
}

/* ---- §4.2.3.1 Parsing a Bare Item ------------------------------------------------------------------------ */

static bool sf_parse_bare(SfIn *in, SfBareItem *out)
{
    char c;

    memset(out, 0, sizeof *out);
    if (sf_eof(in)) return false;
    c = sf_peek(in);
    if (c == '-' || sf_digit(c)) return sf_parse_number(in, out);
    if (c == '"')  return sf_parse_string(in, out);
    if (c == ':')  return sf_parse_byte_sequence(in, out);
    if (c == '?')  return sf_parse_boolean(in, out);
    if (c == '@')  return sf_parse_date(in, out);
    if (c == '%')  return sf_parse_display_string(in, out);
    if (sf_alpha(c) || c == '*') return sf_parse_token(in, out);
    return false;
}

/* ---- §4.2.3.3 Parsing a Key, and §4.2.3.2 Parsing Parameters --------------------------------------------- */

static bool sf_key_char(char c)
{
    return sf_lcalpha(c) || sf_digit(c) || c == '_' || c == '-' || c == '.' || c == '*';
}

static char *sf_parse_key(SfIn *in)
{
    size_t start;

    if (sf_eof(in) || !(sf_lcalpha(sf_peek(in)) || sf_peek(in) == '*')) return NULL;
    start = in->i;
    while (!sf_eof(in) && sf_key_char(sf_peek(in))) in->i++;
    return sf_span_dup(in->s + start, in->i - start);
}

/* §4.2.3.2 stores parameters in an ORDERED MAP, so a repeated key REPLACES rather than appending. */
static void sf_params_set(SfItem *it, char *key, const SfBareItem *value)
{
    int k;

    for (k = 0; k < it->n_params; k++) {
        if (!strcmp(it->params[k].key, key)) {
            free(it->params[k].value.text);
            it->params[k].value = *value;
            free(key);
            return;
        }
    }
    if (it->n_params >= it->cap_params) {
        it->cap_params = it->cap_params ? it->cap_params * 2 : 4;
        it->params = realloc(it->params, (size_t)it->cap_params * sizeof *it->params);
        CHECK(it->params != NULL, "structured fields: OOM growing a parameter map");
    }
    it->params[it->n_params].key = key;
    it->params[it->n_params].value = *value;
    it->n_params++;
}

static bool sf_parse_params(SfIn *in, SfItem *it)
{
    while (!sf_eof(in) && sf_peek(in) == ';') {
        char *key;
        SfBareItem value;

        sf_take(in);
        sf_skip_sp(in);
        key = sf_parse_key(in);
        if (!key) return false;
        memset(&value, 0, sizeof value);
        value.kind = SF_BOOLEAN;
        value.boolean = true;   /* §4.2.3.2: a parameter with no "=" has the value Boolean true */
        if (!sf_eof(in) && sf_peek(in) == '=') {
            sf_take(in);
            if (!sf_parse_bare(in, &value)) { free(key); return false; }
        }
        sf_params_set(it, key, &value);
    }
    return true;
}

/* ---- the two entry points -------------------------------------------------------------------------------- */

void sf_item_free(SfItem *it)
{
    int k;

    if (!it) return;
    free(it->item.text);
    for (k = 0; k < it->n_params; k++) {
        free(it->params[k].key);
        free(it->params[k].value.text);
    }
    free(it->params);
    memset(it, 0, sizeof *it);
}

bool sf_parse_item(const char *input, size_t len, SfItem *out)
{
    SfIn in;

    DCHECK(out != NULL, "a structured field was parsed into nothing");
    memset(out, 0, sizeof *out);
    if (!input) return false;
    in.s = input;
    in.n = len;
    in.i = 0;
    sf_skip_sp(&in);
    if (!sf_parse_bare(&in, &out->item)) { sf_item_free(out); return false; }
    if (!sf_parse_params(&in, out))      { sf_item_free(out); return false; }
    sf_skip_sp(&in);
    /* §4.2 step 5: "if input_string is not empty, fail parsing" — this is the step that makes two identical
       `Cross-Origin-Embedder-Policy: require-corp` headers fail, because Fetch's get joined them into
       `require-corp, require-corp` and a comma is not part of an item. §7.1.4.1 prints that row on purpose. */
    if (!sf_eof(&in)) { sf_item_free(out); return false; }
    return true;
}

const SfBareItem *sf_item_param(const SfItem *it, const char *key)
{
    int k;

    DCHECK(it != NULL && key != NULL, "a structured field parameter was looked up on nothing");
    for (k = 0; k < it->n_params; k++)
        if (!strcmp(it->params[k].key, key))
            return &it->params[k].value;
    return NULL;
}

bool sf_header_item(const HeaderList *l, const char *name, SfItem *out)
{
    char *value;
    bool ok;

    DCHECK(l != NULL && name != NULL && out != NULL, "a structured field value was got from nothing");
    {
        const char *p;
        for (p = name; *p; p++)
            DCHECK(!(*p >= 'A' && *p <= 'Z'),
                   "a header name given to Fetch's get-a-structured-field-value carries an uppercase letter — "
                   "a header list stores names lowercased, so a constant spelled any other way is a name this "
                   "list can never contain");
    }
    memset(out, 0, sizeof *out);
    /* Fetch §2.2.2 step 2: "let value be the result of getting name from list" — which is the join of EVERY
       header of that name with 0x2C 0x20, and is where a repeated header becomes a list. */
    value = header_list_get(l, name);
    if (!value) return false;   /* step 3: "if value is null, then return null" */
    ok = sf_parse_item(value, strlen(value), out);
    free(value);
    return ok;   /* step 5: "if parsing failed, then return null" — indistinguishable from absence, by design */
}
