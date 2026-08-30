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

/* ---- §4.2.3 Parsing an Item ------------------------------------------------------------------------------ */

/* §4.2.3, WITHOUT §4.2's wrapper — "parse bare item, parse parameters, return the tuple", and NOT the leading
   SP skip or the trailing "input_string is not empty, fail" that belong to §4.2 steps 2 and 5. It is factored
   out because §4.2.1.1 and §4.2.1.2 both run §4.2.3 in the MIDDLE of a string, where a trailing check would
   fail every dictionary with more than one member. */
static bool sf_item_in(SfIn *in, SfItem *out)
{
    memset(out, 0, sizeof *out);
    if (!sf_parse_bare(in, &out->item)) return false;
    if (!sf_parse_params(in, out))      return false;
    return true;
}

/* ---- the entry points ------------------------------------------------------------------------------------ */

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
    if (!sf_item_in(&in, out)) { sf_item_free(out); return false; }
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

/* ---- §4.2.1.2 Parsing an Inner List, §4.2.1.1 Parsing an Item or Inner List, §4.2.2 Parsing a Dictionary -- */

static void sf_member_free(SfMember *m)
{
    int k;

    if (!m) return;
    sf_item_free(&m->tuple);
    for (k = 0; k < m->n_items; k++)
        sf_item_free(&m->items[k]);
    free(m->items);
    memset(m, 0, sizeof *m);
}

void sf_dictionary_free(SfDictionary *d)
{
    int k;

    if (!d) return;
    for (k = 0; k < d->n_members; k++) {
        free(d->members[k].key);
        sf_member_free(&d->members[k].value);
    }
    free(d->members);
    memset(d, 0, sizeof *d);
}

/* §4.2.1.2, verbatim. Its step 3.5 — "if the first character of input_string is not SP or ')', fail parsing" —
   is what makes `(a,b)` a malformed inner list rather than a one-element one: the members of an inner list are
   SP-delimited and a comma there is not a separator, it is a syntax error that §4.2.2 then propagates to the
   whole field. */
/* EVERY FAILURE ARM FREES THE PARTIAL MEMBER, and that is not tidiness — it is the ownership contract this
   file already states for sf_parse_item ("on failure nothing is left allocated"). A parse that fails mid-inner-
   list has already allocated a token's text and a parameter map, and the caller's sf_dictionary_free can only
   reach members that were SET; the one being built is reachable from nowhere else. THE COUNTER IS THEREFORE
   INCREMENTED BEFORE THE ITEM IS PARSED, so the slot is inside the member the moment it exists. */
static bool sf_inner_list_in(SfIn *in, SfMember *out)
{
    memset(out, 0, sizeof *out);
    out->inner_list = true;
    if (sf_eof(in) || sf_take(in) != '(') { sf_member_free(out); return false; }   /* step 1 */
    while (!sf_eof(in)) {                                            /* step 3 */
        sf_skip_sp(in);                                              /* step 3.1 */
        if (!sf_eof(in) && sf_peek(in) == ')') {                     /* step 3.2 */
            sf_take(in);
            /* step 3.2.2: the INNER LIST's own parameters, which is where §5.2's `report-to` lands when the
               Member Value is written as `camera=(self);report-to="x"`. They go on `tuple` for the reason
               structured_fields.h gives — one field for §4.2.1.1's `parameters` in both arms. */
            if (!sf_parse_params(in, &out->tuple)) { sf_member_free(out); return false; }
            return true;
        }
        if (out->n_items >= out->cap_items) {                        /* step 3.3 / 3.4 */
            out->cap_items = out->cap_items ? out->cap_items * 2 : 4;
            out->items = realloc(out->items, (size_t)out->cap_items * sizeof *out->items);
            CHECK(out->items != NULL, "structured fields: OOM growing §3.1.1's inner list");
        }
        memset(&out->items[out->n_items], 0, sizeof out->items[0]);
        out->n_items++;
        if (!sf_item_in(in, &out->items[out->n_items - 1])) { sf_member_free(out); return false; }
        /* step 3.5: "if the first character of input_string is not SP or ')', fail parsing" */
        if (sf_eof(in) || (sf_peek(in) != ' ' && sf_peek(in) != ')')) { sf_member_free(out); return false; }
    }
    sf_member_free(out);
    return false;                                                    /* step 4: end of inner list not found */
}

/* §4.2.1.1: "if the first character of input_string is '(', return the result of running Parsing an Inner
   List … return the result of running Parsing an Item". */
static bool sf_item_or_inner_list_in(SfIn *in, SfMember *out)
{
    if (!sf_eof(in) && sf_peek(in) == '(')
        return sf_inner_list_in(in, out);
    memset(out, 0, sizeof *out);
    if (!sf_item_in(in, &out->tuple)) { sf_member_free(out); return false; }
    return true;
}

/* §4.2.2 step 2.4/2.5's ORDERED MAP SET — "if dictionary already contains a key this_key … overwrite its value
   with member. Otherwise, append key this_key with value member". OVERWRITE IN PLACE, which is the note the
   section ends on ("when duplicate Dictionary keys are encountered, all but the last instance are ignored")
   and is not the same as appending a second entry: §9.2 iterates this map once per member, so a duplicate that
   appended would run the LAST declaration and then be unable to say it had ever seen the first. */
static void sf_dict_set(SfDictionary *d, char *key, const SfMember *value)
{
    int k;

    for (k = 0; k < d->n_members; k++) {
        if (!strcmp(d->members[k].key, key)) {
            sf_member_free(&d->members[k].value);
            d->members[k].value = *value;
            free(key);
            return;
        }
    }
    if (d->n_members >= d->cap_members) {
        d->cap_members = d->cap_members ? d->cap_members * 2 : 4;
        d->members = realloc(d->members, (size_t)d->cap_members * sizeof *d->members);
        CHECK(d->members != NULL, "structured fields: OOM growing §3.2's dictionary");
    }
    d->members[d->n_members].key = key;
    d->members[d->n_members].value = *value;
    d->n_members++;
}

/* §4.2's OWS — "optional whitespace", SP or HTAB. It is NOT §4.2 step 2's leading-SP skip and the two are
   deliberately different: §4.2.2 steps 2.6 and 2.9 discard OWS around the ',' between members, so
   `a=1,\tb=2` is well formed while a TAB anywhere an SP is called for is not. */
static void sf_skip_ows(SfIn *in)
{
    while (in->i < in->n && (in->s[in->i] == ' ' || in->s[in->i] == '\t')) in->i++;
}

static bool sf_dictionary_in(SfIn *in, SfDictionary *out)
{
    memset(out, 0, sizeof *out);
    while (!sf_eof(in)) {                                            /* step 2 */
        char    *key = sf_parse_key(in);                             /* step 2.1 */
        SfMember member;

        if (!key) return false;
        if (!sf_eof(in) && sf_peek(in) == '=') {                     /* step 2.2 */
            sf_take(in);
            if (!sf_item_or_inner_list_in(in, &member)) { free(key); return false; }
        } else {                                                     /* step 2.3 */
            /* "let value be Boolean true" — a bare Member Name IS a member whose value is the boolean true,
               which is why `Origin-Agent-Cluster`-shaped spellings survive here and why §5.2's "Member Values
               of any other form will cause the entire Dictionary Member to be ignored" is a PROCESSING rule
               rather than a parse rule: this parses, and §9.2 then declines to recognise it. */
            memset(&member, 0, sizeof member);
            member.tuple.item.kind = SF_BOOLEAN;
            member.tuple.item.boolean = true;
            if (!sf_parse_params(in, &member.tuple)) { sf_member_free(&member); free(key); return false; }
        }
        sf_dict_set(out, key, &member);                              /* steps 2.4 and 2.5 */
        sf_skip_ows(in);                                             /* step 2.6 */
        if (sf_eof(in)) return true;                                 /* step 2.7 */
        if (sf_take(in) != ',') return false;                        /* step 2.8 */
        sf_skip_ows(in);                                             /* step 2.9 */
        if (sf_eof(in)) return false;                                /* step 2.10: a trailing comma */
    }
    return true;                                                     /* step 3: an empty dictionary */
}

bool sf_parse_dictionary(const char *input, size_t len, SfDictionary *out)
{
    SfIn in;

    DCHECK(out != NULL, "a structured field dictionary was parsed into nothing");
    memset(out, 0, sizeof *out);
    if (!input) return false;
    in.s = input;
    in.n = len;
    in.i = 0;
    sf_skip_sp(&in);                                                 /* §4.2 step 2 */
    if (!sf_dictionary_in(&in, out)) { sf_dictionary_free(out); return false; }
    sf_skip_sp(&in);                                                 /* §4.2 step 4 */
    /* §4.2 step 5: "if input_string is not empty, fail parsing". §4.2.2 step 2.7 already returns at the end of
       a well-formed dictionary, so anything left here is input §4.2.2 stopped on without failing — which the
       grammar makes unreachable, and the check stays because §4.2 states it and because an unreachable check
       costs one comparison while a missing one is a field this parser accepts and no browser does. */
    if (!sf_eof(&in)) { sf_dictionary_free(out); return false; }
    return true;
}

bool sf_header_dictionary(const HeaderList *l, const char *name, SfDictionary *out)
{
    char *value;
    bool  ok;

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
    /* Fetch §2.2.2 step 2's join with 0x2C 0x20 — and for a DICTIONARY that join is not a hazard the way it is
       for an item: two `Permissions-Policy` headers combine into one well-formed dictionary, which is exactly
       what §4.2.2's overwrite rule is written for. */
    value = header_list_get(l, name);
    if (!value) return false;   /* step 3 */
    ok = sf_parse_dictionary(value, strlen(value), out);
    free(value);
    return ok;                  /* step 5 */
}

const SfBareItem *sf_member_bare(const SfMember *m)
{
    DCHECK(m != NULL, "a structured field member's bare item was read off nothing");
    DCHECK(!m->inner_list,
           "§4.2.1.1's BARE ITEM was read off a member that is an INNER LIST — the two arms of that tuple are "
           "different shapes and only one of them has a bare item, so a reader that did not ask which arm it "
           "holds is about to read the zero this struct was allocated with as a Member Value the sender wrote");
    return &m->tuple.item;
}

const SfBareItem *sf_member_param(const SfMember *m, const char *key)
{
    DCHECK(m != NULL, "a structured field member's parameters were looked up on nothing");
    return sf_item_param(&m->tuple, key);
}
