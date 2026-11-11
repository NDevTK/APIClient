/* THE HEADERS INTERFACE — WHATWG Fetch §5, and the header list behind it.
 *
 * WHY IT EXISTS HERE. The tool's headline output is what a request NEEDS, and a header is half of that: an
 * endpoint reached only with `Authorization` and `X-Api-Version` is not usable without them, and the popup has
 * had a "Required Headers" section reading a `requiredHeaders` record for as long as it has existed — which the
 * engine never emitted, because `fetch` read `init.method` and `init.url` and nothing else. This is the first of
 * the three things that closes: the LIST, and the interface a page builds one with.
 *
 * THE LIST IS NOT A MAP, AND THE LIST IS NOT THE CLASS. Fetch §2.2.2 Headers defines the header list — "a
 * specialized multimap: an ordered list of key-value pairs with potentially duplicate keys" — so it keeps
 * (name, value) PAIRS and appends rather than replacing, because `Set-Cookie` is genuinely repeated and §5.1
 * Headers class' `getSetCookie` reads those repeats back. §2.2.2's own `get` is what combines, returning the
 * matching values "separated from each other by 0x2C 0x20, in order" (§2.2.4 is Bodies, and stood here for
 * that join). A map keyed by name would answer `get` correctly and lose every repeat, which is exactly the
 * header the difference exists for.
 *
 * THE FILL IS A REQUEST SEQUENCE, not a C walk. `new Headers({'X-Api-Key': k})` converts a Web IDL
 * `record<ByteString, ByteString>`, which is [[OwnPropertyKeys]] followed by a [[Get]] per key — on a Proxy the
 * page's `ownKeys` and `get` traps, and from C that is the drive-to-completion this engine aborts on. It is
 * written as a sub-sequence rather than inside the constructor because `fetch(u, {headers: ...})` performs the
 * SAME conversion, and the spec states it once. */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/agent_state.h"
#include "core/fetch/headers.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "core/idl_iter.h"
#include "solver/concolic.h"
#include <lexbor/core/base.h>   /* lexbor_realloc: the allocator the engine gives its refusal edge */
#include "solver/reclaim.h"

static JSClassID g_headers_class;
static int       g_ctor_stepid = -1;
static JSRuntime *g_headers_rt;

/* ---- the header list ---------------------------------------------------------------------------------- */

/* THE LIST THIS ENGINE KEEPS IS LOWERCASE, AND NO PART OF FETCH SAYS TO MAKE IT SO. What stood here cited
   §5.1 for a header-NAME normalization; §5.1 Headers class has no such step, and neither does anything else in
   the standard. Fetch §2.2.2 Headers defines a name that KEEPS ITS CASE and comparisons that ignore it: "A
   header name is a byte sequence that matches the field-name token production", with no case rule; `contains`,
   `get` and `delete` match "byte-case-insensitive"; and §2.2.2's own append REUSES the stored spelling — "If
   list contains name, then set name to the first such header's name." The single place the standard
   lowercases is §2.2.2's "sort and combine", via "convert header names to a sorted-lowercase set", and that is
   what §5.1's iteration yields.
   SO THE SENTENCE WAS TRUE ABOUT THIS ENGINE AND FALSE ABOUT THE STANDARD, which is the worse of the two
   failures — a number a reader can look up and a claim they cannot. Lowercasing on the way IN is still right
   for every READ: it makes each comparison below a plain strcmp, which is precisely the byte-case-insensitive
   match §2.2.2 asks for, and iteration lowercases regardless.
   NAMED RESIDUAL. NOT COVERED: the CASE a name was appended with. §2.2.2's append preserves it and this list
   discards it, so a stored name is a canonicalization rather than the bytes the page wrote. That is not wrong
   on the wire — RFC 9110 §5.1 Field Names: "Field names are case-insensitive" — and no §5.1 read can observe
   it, which is why this is a residual and not a DCHECK. WHAT THE NEXT DIFF BUILDS: an entry that stores the
   name AS GIVEN, compares byte-case-insensitively, and runs §2.2.2's append rule of adopting the first
   matching header's spelling. HOW ITS ABSENCE SHOWS: the emitted `requiredHeaders` record is built from these
   very bytes, so a bundle that sends `X-Api-Key` is reported as requiring `x-api-key` — a header the run
   canonicalized, presented where the report otherwise states what it observed. */
static char *header_lower(const char *s)
{
    size_t i, n = strlen(s);
    char *r = lexbor_malloc(n + 1);
    CHECK(r, "headers: OOM copying a header name — a dropped header loses what the endpoint requires");
    for (i = 0; i < n; i++)
        r[i] = (s[i] >= 'A' && s[i] <= 'Z') ? (char)(s[i] - 'A' + 'a') : s[i];
    r[n] = 0;
    return r;
}

static char *header_dup(const char *s)
{
    char *r = reclaim_strdup(s ? s : "");
    CHECK(r, "headers: OOM copying a header value");
    return r;
}

void header_list_free(HeaderList *l)
{
    int i;
    if (!l) return;
    for (i = 0; i < l->n; i++) {
        free(l->e[i].name); free(l->e[i].value); free(l->e[i].root); free(l->e[i].example);
    }
    free(l->e);
    l->e = NULL; l->n = l->cap = 0;
}

void header_list_append(HeaderList *l, const char *name, const char *value)
{
    header_list_append_unknown(l, name, value, NULL);
}

void header_list_append_unknown(HeaderList *l, const char *name, const char *value, const HeaderUnknown *unk)
{
    const char *root = unk ? unk->root : NULL;

    DCHECK(l != NULL && name != NULL, "a header was appended to no list");
    DCHECK(root == NULL || *root, "a header value's root was stated as the empty name");
    if (l->n >= l->cap) {
        l->cap = l->cap ? l->cap * 2 : 8;
        l->e = lexbor_realloc(l->e, (size_t)l->cap * sizeof(HeaderEntry));
        CHECK(l->e, "headers: OOM growing a header list");
    }
    l->e[l->n].name = header_lower(name);
    l->e[l->n].value = header_dup(value);
    l->e[l->n].root = root ? header_dup(root) : NULL;
    l->e[l->n].example = unk && unk->example ? header_dup(unk->example) : NULL;
    l->e[l->n].shape = unk != NULL;
    l->n++;
}

const HeaderUnknown *header_entry_unknown(const HeaderEntry *e, HeaderUnknown *tmp)
{
    DCHECK(e->shape || (e->root == NULL && e->example == NULL),
           "a header entry that is bytes states an unknown's root or example");
    if (!e->shape) return NULL;
    tmp->root = e->root;
    tmp->example = e->example;
    return tmp;
}

const char *header_value_class_token(const HeaderEntry *e)
{
    return !e->shape ? "concrete" : e->example ? "example" : "unknown";
}

const char *header_entry_wire_value(const HeaderEntry *e)
{
    return e->shape && e->example ? e->example : e->value;
}

char *header_list_field_lines(const HeaderList *l)
{
    size_t n = 1;
    char *out, *w;
    int i;

    DCHECK(l != NULL, "a header list was serialized from nothing — an EMPTY list is the response that carried "
                      "no headers, and it is a list; NULL is a caller that has none to serialize");
    for (i = 0; i < l->n; i++)
        n += strlen(l->e[i].name) + 2 + strlen(l->e[i].value) + 1;   /* "name: value\n" */
    out = lexbor_malloc(n);
    CHECK(out != NULL, "headers: OOM serializing a response's field lines");
    w = out;
    for (i = 0; i < l->n; i++) {
        /* RFC 9112 forbids CR and LF inside a field value and a `Headers` object enforces it, so a value
           carrying one did not come off a response — and splitting it here would present the parse on the
           other side with headers nobody delivered. Asserted where the name of the offending header is still
           in hand, which is the same place extension/bridge.js asserts it for the other producer. */
        DCHECK(!strchr(l->e[i].value, '\n') && !strchr(l->e[i].value, '\r'),
               "a response header value carries CR or LF — RFC 9112 forbids both inside a field value, so this "
               "value did not come off a response, and serializing it would deliver field lines the server "
               "never sent");
        {
            size_t nn = strlen(l->e[i].name), vn = strlen(l->e[i].value);

            memcpy(w, l->e[i].name, nn); w += nn;
            *w++ = ':'; *w++ = ' ';
            memcpy(w, l->e[i].value, vn); w += vn;
            *w++ = '\n';
        }
    }
    *w = 0;
    return out;
}

void header_list_parse_field_lines(HeaderList *l, const char *block)
{
    const char *p = block;

    DCHECK(l != NULL, "a field block was parsed into no header list");
    DCHECK(l->n == 0, "a field block was parsed into a header list that already holds headers — a response has "
                      "ONE header list and it is built once, so a second parse into the same list would make "
                      "one response look like two and Fetch's `get` would join the pair");
    if (!block) return;   /* a response with no headers at all — a real answer, and an empty list is it */
    while (*p) {
        const char *eol = strchr(p, '\n');
        const char *end = eol ? eol : p + strlen(p);
        const char *colon;
        const char *vs;
        char *name;

        /* RFC 9112's field lines end CRLF, so a CR before the LF is the TERMINATOR and not part of the value.
           Taking it as one is how a `Content-Security-Policy` acquires a trailing carriage return and stops
           matching anything the parser below compares it against. */
        if (end > p && end[-1] == '\r') end--;
        colon = memchr(p, ':', (size_t)(end - p));

        if (end == p) { p = eol ? eol + 1 : end; continue; }   /* a blank line separates nothing here */
        /* THE ZONE THAT WROTE THIS BLOCK HAD A `Headers` OBJECT, so every line it wrote came from a real
           response and has a name and a value. A line without a colon is that zone's bug, and it is one this
           engine must not paper over: dropping it silently would make a `Content-Security-Policy` that the
           server DID send disappear, and the sink it kills would be reported as a working exploit. */
        CHECK(colon != NULL, "a response field block carries a line with no colon — the trusted zone builds "
                             "this from a `Headers` object, where every entry is a (name, value) pair, so a "
                             "line that is not one means the serialization and this parse disagree. ALWAYS "
                             "fatal rather than dev-only: what follows this test is a read through the colon, "
                             "and a release build that skipped the line would drop a policy the server sent");
        vs = colon + 1;
        while (vs < end && (*vs == ' ' || *vs == '\t')) vs++;   /* RFC 9112's OWS after the colon */
        name = lexbor_malloc((size_t)(colon - p) + 1);
        CHECK(name != NULL, "headers: OOM reading a response field line");
        memcpy(name, p, (size_t)(colon - p));
        name[colon - p] = 0;
        /* Fetch §2.2.2 Headers' grammar for a header name, asked of input that came from outside this
           engine. A name that is not a TOKEN could not have come off a `Headers` object, so it is the same
           disagreement the colon check names. */
        DCHECK(header_name_valid(name, strlen(name)),
               "a response field block carries a line whose name is not an HTTP token — a header list stores "
               "what a response delivered, and a name Fetch would have rejected never was one");
        {
            char *value = lexbor_malloc((size_t)(end - vs) + 1);
            CHECK(value != NULL, "headers: OOM reading a response field value");
            memcpy(value, vs, (size_t)(end - vs));
            value[end - vs] = 0;
            /* APPEND, NEVER SET: Fetch §2.2.2 Headers' header list keeps repeats, and §7.1.4.1's own table
               turns on them — two `Cross-Origin-Embedder-Policy: require-corp` headers must combine into a
               value that FAILS to parse as an item, which a list that replaced would have quietly turned into
               one that succeeds. */
            header_list_append(l, name, value);
            free(value);
        }
        free(name);
        p = eol ? eol + 1 : end;
    }
}

char *header_list_get(const HeaderList *l, const char *name)
{
    char *lo = header_lower(name), *out = NULL;
    size_t total = 0;
    int i, first = 1;

    for (i = 0; i < l->n; i++)
        if (!strcmp(l->e[i].name, lo))
            total += strlen(l->e[i].value) + 2;   /* ", " between, never after the last */
    if (!total) { free(lo); return NULL; }
    out = lexbor_malloc(total + 1);
    CHECK(out, "headers: OOM joining a header's values");
    out[0] = 0;
    for (i = 0; i < l->n; i++) {
        if (strcmp(l->e[i].name, lo)) continue;
        if (!first) strcat(out, ", ");
        strcat(out, l->e[i].value);
        first = 0;
    }
    free(lo);
    return out;
}

/* Fetch §2.2.2's collect an HTTP quoted string with extract-value false: the code points from the opening quote
   through the closing one (or the end), escapes included. `*pos` is at the `"` on entry and past the string on
   return. */
static void header_collect_quoted(const char *in, size_t len, size_t *pos, char *out, size_t *o)
{
    DCHECK(*pos < len && in[*pos] == '"', "an HTTP quoted string was collected at a position that is no quote");
    out[(*o)++] = in[(*pos)++];
    while (*pos < len) {
        char c = in[(*pos)++];

        out[(*o)++] = c;
        if (c == '"') return;
        if (c == '\\' && *pos < len) out[(*o)++] = in[(*pos)++];
    }
}

char **header_list_get_decode_split(const HeaderList *l, const char *name, size_t *n)
{
    char *value = header_list_get(l, name);
    char **values = NULL, *tmp;
    size_t len, pos = 0, o = 0, cap = 0;

    *n = 0;
    if (!value) return NULL;
    len = strlen(value);
    tmp = lexbor_malloc(len + 1);
    CHECK(tmp != NULL, "headers: OOM splitting a header value");
    for (;;) {
        size_t s, e;

        while (pos < len && value[pos] != '"' && value[pos] != ',') tmp[o++] = value[pos++];
        if (pos < len && value[pos] == '"') {
            header_collect_quoted(value, len, &pos, tmp, &o);
            if (pos < len) continue;
        }
        /* Remove all HTTP tab or space from the start and end of temporaryValue. */
        for (s = 0; s < o && (tmp[s] == ' ' || tmp[s] == '\t'); s++) {}
        for (e = o; e > s && (tmp[e - 1] == ' ' || tmp[e - 1] == '\t'); e--) {}
        if (*n == cap) {
            char **grown = lexbor_realloc(values, (cap = cap ? cap * 2 : 4) * sizeof *values);
            CHECK(grown != NULL, "headers: OOM splitting a header value");
            values = grown;
        }
        values[*n] = reclaim_strndup(tmp + s, e - s);
        CHECK(values[*n] != NULL, "headers: OOM splitting a header value");
        (*n)++;
        o = 0;
        if (pos >= len) break;
        DCHECK(value[pos] == ',', "a header value's split stopped at a code point that is not a comma");
        pos++;
    }
    free(tmp);
    free(value);
    return values;
}

void header_values_free(char **values, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) free(values[i]);
    free(values);
}

/* THE LAST VALUE, UNJOINED — see headers.h. It is a different question from the join above and not a
   convenience over it: the join produces a string that is a LIST, and the algorithm that wants this one
   (MIME Sniffing §5.1's supplied MIME type detection) compares its answer byte for byte against four literal
   `Content-Type` values, which a joined list can never equal. */
const char *header_list_get_last(const HeaderList *l, const char *name)
{
    char *lo = header_lower(name);
    const char *out = NULL;
    int i;

    for (i = 0; i < l->n; i++)
        if (!strcmp(l->e[i].name, lo)) out = l->e[i].value;
    free(lo);
    return out;
}

/* Fetch §3.6 "`X-Content-Type-Options` header"'s DETERMINE NOSNIFF — see headers.h. */
bool header_list_determine_nosniff(const HeaderList *l)
{
    char *v = header_list_get(l, "x-content-type-options");
    size_t s, e;
    bool yes;

    if (!v) return false;                        /* step 2: "If values is null, then return false." */
    /* "getting, DECODING AND SPLITTING": the value list split on U+002C, of which only values[0] is read, with
       Fetch §2.2's leading and trailing HTTP whitespace removed from it. A substring test over the whole
       header is a DIFFERENT algorithm — it answers true for `foo, nosniff`, where this answers false. */
    e = 0;
    while (v[e] && v[e] != ',') e++;
    s = 0;
    while (s < e && (v[s] == 0x09 || v[s] == 0x0A || v[s] == 0x0D || v[s] == 0x20)) s++;
    while (e > s && (v[e - 1] == 0x09 || v[e - 1] == 0x0A || v[e - 1] == 0x0D || v[e - 1] == 0x20)) e--;
    /* Step 3's ASCII case-insensitive match, against the seven bytes of `nosniff`. */
    yes = (e - s == 7);
    if (yes) {
        static const char N[] = "nosniff";
        size_t i;
        for (i = 0; i < 7; i++) {
            char c = v[s + i];
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            if (c != N[i]) { yes = false; break; }
        }
    }
    free(v);
    return yes;
}

/* §5.1's members DECLARE `ByteString` — `append(ByteString name, ByteString value)` and the four beside it —
   so Web IDL §3.2.11 ByteString's refusal is the TYPE'S and runs inside the conversion: step 1 "Let x be ?
   ToString(V)" and step 2 "If the value of any element of x is greater than 255, then throw a TypeError."
   §3.6 Overload resolution algorithm converts the arguments "from left to right", one position finishing
   before the next begins, so an out-of-range name refuses BEFORE the value's own ToString is issued — which
   is a page's own code, and running it where the standard runs nothing is an extra chance for that code to
   fork, emit or throw inside an operation the spec treats as inert. NOTHING BELOW REPEATS THAT RANGE FOR AN
   ARGUMENT; what is left here is Fetch §2.2.2 Headers' SYNTAX, a different refusal on an already-typed string.
   THE FILL'S THREE CONVERSIONS ARE NOT ARGUMENTS AND STILL OWE IT. `HeadersInit`'s `record<ByteString,
   ByteString>` converts a key (§3.2.23 Records step 4.2.1) and a value (step 4.2.3), and its
   `sequence<sequence<ByteString>>` converts each element (§3.2.21.1 Creating a sequence from an iterable), and
   no declaration reaches any of the three — so each performs §3.2.11 step 2 itself, at its own conversion.
   headers_bytestring is that step, stated once for the three of them. */
/* EVERY CHECK HERE IS LENGTH-DELIMITED, because U+0000 IS A ByteString CHARACTER. These read a C string and
   stopped at the first NUL, so `new Headers({"set-cookie": "\0"})` presented an EMPTY value, normalized to
   empty, validated clean, and was stored — where the spec forbids 0x00 in a header value and wpt asserts the
   TypeError. A NUL cannot be excluded by the shape of the buffer; it has to be looked for.
   What survives validation is provably NUL-free (a name is a token, a value forbids 0x00), which is why the
   LIST may still hold plain C strings — and header_check DCHECKs exactly that before handing one over. */


/* WEB IDL §3.2.11 ByteString STEP 2, over the UTF-8 the engine hands out: "If the value of any element of x
   is greater than 255, then throw a TypeError." `what` names which half of the pair it refused, because a
   fill converts three things and a message that did not say which named no site at all.
   IT IS UNREACHABLE FROM A MEMBER'S ARGUMENT UNLESS THAT ARGUMENT IS UNKNOWN EXTERNAL INPUT, AND THAT
   CLAUSE READ `UNREACHABLE` FULL STOP — it is rewritten rather than deleted because the reasoning behind it is
   sound and a reader will re-derive it: a member's positions ARE converted by the declaration, in §3.6's
   order, so for every value this engine can spell the range has already run and repeating it here would be a
   second copy. What the sentence did not say is that core/idl_args.h's `idl_concolic_rule` answers
   IDL_CONCOLIC_CROSSES for IDL_BYTESTRING, so a concolic is PLACED AS ITSELF and reaches no conversion at
   all — neither `step_tostring_run` nor idl_args.c's argument-side range. The member path below therefore
   owes §3.2.11 step 2 on the bytes it projects for one, exactly as the fill's three conversions owe it, which
   is why this is still a helper here and is now reached from four roads rather than three. */
static int headers_bytestring(JSContext *ctx, const char *utf8, size_t len, const char *what)
{
    if (idl_is_bytestring(utf8, len))
        return 0;
    JS_ThrowTypeError(ctx, "a header %s is not a ByteString", what);
    return -1;
}

/* Fetch §2.2 HTTP's "HTTP whitespace" — these FOUR and not isspace()'s set. \f is not one of them, which is
   what makes wpt's "\t\f\tnewLine\n" normalize to "\f\tnewLine" rather than to "newLine". */
static int header_is_ws(unsigned char c) { return c == 0x09 || c == 0x0a || c == 0x0d || c == 0x20; }

/* Fetch §2.2.2 Headers' normalize, whose whole text is "remove any leading and trailing HTTP whitespace bytes
   from potentialValue" — leading and trailing, never inner. §5.1's append and set are the two callers that
   invoke it, which is why the algorithm reads as theirs and is defined here. Caller frees.
   `*pn` is the normalized LENGTH, which is not strlen(out) when the value carries an embedded NUL — the case
   the validation below exists to reject. */
static char *header_normalize_value(const char *v, size_t len, size_t *pn)
{
    const char *b = v, *e = v + len;
    char *out;
    size_t n;
    while (b < e && header_is_ws((unsigned char)*b)) b++;
    while (e > b && header_is_ws((unsigned char)e[-1])) e--;
    n = (size_t)(e - b);
    out = lexbor_malloc(n + 1);
    CHECK(out, "headers: OOM normalizing a header value");
    memcpy(out, b, n);
    out[n] = 0;
    *pn = n;
    return out;
}

/* Fetch §2.2.2 Headers: "A header name is a byte sequence that matches the field-name token production" —
   and the production it links is RFC 9110 §5.1 Field Names' `field-name = token`, one or more tchar and
   nothing else. (RFC 7230 stood here; 9110 obsoletes it and is what Fetch cites.) `{}` reaches this as
   "[object Object]", and the space and brackets are what make it a TypeError rather than a header. */
static int header_name_is_valid(const char *s, size_t len)
{
    const unsigned char *p = (const unsigned char *)s, *end = p + len;
    if (len == 0) return 0;
    for (; p < end; p++) {
        unsigned char c = *p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) continue;
        if (c && strchr("!#$%&'*+-.^_`|~", (char)c)) continue;   /* c == 0 is not a tchar */
        return 0;
    }
    return 1;
}

/* Fetch §2.2.2 Headers' header value: "Contains no 0x00 (NUL) or HTTP newline bytes" — no NUL, CR or LF
   anywhere. The leading/trailing whitespace the definition also forbids is what normalization has already
   removed. */
static int header_value_is_valid(const char *s, size_t len)
{
    size_t i;
    for (i = 0; i < len; i++)
        if (s[i] == 0x00 || s[i] == 0x0a || s[i] == 0x0d) return 0;
    return 1;
}

bool header_name_valid(const char *name, size_t len) { return header_name_is_valid(name, len) != 0; }

char *header_value_normalize_valid(const char *value, size_t len, size_t *pn)
{
    size_t n = 0;
    char *norm = header_normalize_value(value, len, &n);

    if (!header_value_is_valid(norm, n)) { free(norm); return NULL; }
    if (pn) *pn = n;
    return norm;
}

/* The value half of header_check: normalize `value`, then refuse one that is not a header value with a
   TypeError. `*pnorm` is the normalized value (caller frees). 0 on success, -1 with a TypeError live. Called
   alone only where the name half has already been decided, by a fork over an unknown name. */
static int header_value_check(JSContext *ctx, const char *value, size_t value_len, char **pnorm)
{
    char *norm;
    size_t norm_len;

    *pnorm = NULL;
    DCHECK(idl_is_bytestring(value, value_len),
           "a header value reached Fetch §2.2.2 Headers' syntax check without Web IDL §3.2.11 ByteString's range");
    norm = header_normalize_value(value, value_len, &norm_len);
    if (!header_value_is_valid(norm, norm_len)) {
        free(norm);
        JS_ThrowTypeError(ctx, "invalid header value");
        return -1;
    }
    /* THE LIST MAY HOLD A C STRING because of this: a value that got here has no 0x00 in it, so nothing is
       lost by dropping the length. The assert is what keeps that true if the rule above ever changes. */
    DCHECK(strlen(norm) == norm_len, "a header value passed validation while carrying an embedded NUL");
    *pnorm = norm;
    return 0;
}

/* §5.1's guard as ONE operation, because every entry point performs the same one: normalize the value, then
   reject a bad name or a bad value with a TypeError. `*pnorm` is the normalized value (caller frees), left NULL
   for the name-only members. 0 on success, -1 with a TypeError live. */
static int header_check(JSContext *ctx, const char *name, size_t name_len,
                       const char *value, size_t value_len, char **pnorm)
{
    if (pnorm) *pnorm = NULL;
    /* §3.2.11's RANGE HAS ALREADY RUN, AT WHATEVER CONVERTED THESE BYTES — the member's declared ByteString
       for an argument, headers_bytestring for each of the fill's three. This ASSERTS that instead of
       repeating it, so a fourth road into §2.2.2's syntax cannot quietly put a code point above U+00FF into a
       header list whose entries are bytes. The condition reads two buffers and writes nothing. */
    DCHECK(idl_is_bytestring(name, name_len) && (!value || idl_is_bytestring(value, value_len)),
           "a header name or value reached Fetch §2.2.2 Headers' syntax check without Web IDL §3.2.11 "
           "ByteString's range — every road here converts to ByteString first (the member's declared type, or "
           "headers_bytestring at the fill's own conversion)");
    if (!header_name_is_valid(name, name_len)) {
        JS_ThrowTypeError(ctx, "invalid header name");
        return -1;
    }
    DCHECK(strlen(name) == name_len, "a header name passed validation while carrying an embedded NUL");
    if (!value) return 0;
    return header_value_check(ctx, value, value_len, pnorm);
}

/* ---- §5.1's guard: which writes a PAGE is allowed to make -------------------------------------------------
 *
 * A header list this engine builds is trusted; what the guard governs is what the page may do to it afterwards.
 * The three answers are distinct and the spec is explicit about which is which: "immutable" THROWS a TypeError,
 * a forbidden name under "request"/"response" is a SILENT no-op (validating returns false and the member simply
 * returns), and everything else writes. Collapsing the silent case into a throw would break every page that
 * sets `Host` defensively; collapsing it into a write would let a page forge headers the browser owns. */

static int header_ci_eq(const char *lower_name, const char *lit)
{
    /* names arrive lowercased or are lowercased by the caller; the literal is written lowercase */
    return !strcmp(lower_name, lit);
}

/* Fetch §2.2.1 Methods' "forbidden method": the three a page may never send, however it spells them. */
static int header_is_forbidden_method(const char *m, size_t len)
{
    static const char *const METHODS[] = { "connect", "trace", "track" };
    size_t i, k;
    char buf[8];
    if (len >= sizeof buf) return 0;
    for (i = 0; i < len; i++)
        buf[i] = (m[i] >= 'A' && m[i] <= 'Z') ? (char)(m[i] - 'A' + 'a') : m[i];
    buf[len] = 0;
    for (k = 0; k < sizeof(METHODS) / sizeof(METHODS[0]); k++)
        if (!strcmp(buf, METHODS[k])) return 1;
    return 0;
}

/* Fetch §2.2.2 Headers' "get, decode, and split" a header value, for the method-override headers: split on
   ",", strip HTTP whitespace around each token, and ask whether ANY of them is a forbidden method.
   `X-HTTP-Method: ",TRACE,"` is forbidden for the same reason `X-HTTP-Method: TRACE` is — the server would
   see both. */
static int header_value_has_forbidden_method(const char *v)
{
    const char *p = v;
    for (;;) {
        const char *comma = strchr(p, ',');
        const char *b = p, *e = comma ? comma : p + strlen(p);
        while (b < e && header_is_ws((unsigned char)*b)) b++;
        while (e > b && header_is_ws((unsigned char)e[-1])) e--;
        if (header_is_forbidden_method(b, (size_t)(e - b))) return 1;
        if (!comma) return 0;
        p = comma + 1;
    }
}

/* FETCH §2.2.2 Headers' OWN list, in the standard's own order and containing nothing else — so a name added
   to or removed from this array is a diff against the spec text and can be read as one. Fetch's list is not
   the whole of the
   platform's, and the way a reader tells the difference is that the other standards' entries are BELOW, each
   under the sentence that adds it. */
static const char *const FORBIDDEN_REQUEST_FETCH[] = {
    "accept-charset", "accept-encoding", "access-control-request-headers",
    "access-control-request-method", "connection", "content-length", "cookie", "cookie2", "date", "dnt",
    "expect", "host", "keep-alive", "origin", "referer", "set-cookie", "te", "trailer",
    "transfer-encoding", "upgrade", "via",
};

/* PRIVATE NETWORK ACCESS §3.4.4 "Forbidden header names", in full: "A new entry is added to the list of
   forbidden request-header names: `Access-Control-Request-Private-Network`." (Draft Community Group Report,
   26 September 2024, https://wicg.github.io/private-network-access/#forbidden-header-names.) The note under it
   gives the reason this component cares: "The user agent should have full control over this header, just as it
   does over other CORS headers."
 *
 * IT IS KEPT, AND THIS IS THE DECISION RATHER THAN AN ACCIDENT. `private-network` does not occur anywhere in
 * fetch.spec.whatwg.org, and an entry that matched only Chrome's behaviour would be exactly the kind of
 * hardcoded string this codebase deletes. It is not one: the platform's list is the union of Fetch's plus every
 * standard that adds to it, and this is a live specification adding to it in those words — so keeping it IS
 * "implement the spec at the root", and Chrome shipping it is the CONFIRMATION, in that order and not the
 * other. The array it lives in is separate so that stays legible without a reader having to trust a comment:
 * two lists, two standards, and the Fetch one still diffs clean against §2.2.2 Headers.
 *
 * The consequence this engine is built for follows the same way. What a request CARRIES is the report's claim,
 * and a header the user agent strips is a header the report must not carry — so a bundle that sets this name
 * gets the browser's answer (dropped, `get` answers null) rather than a fabricated one. The rest of Private
 * Network Access is a network-layer feature (target IP address space, the preflight that would send this
 * header); none of it is scriptable and none of it changes what this list answers. */
static const char *const FORBIDDEN_REQUEST_PNA[] = {
    "access-control-request-private-network",
};

/* Fetch §2.2.2 Headers' "forbidden request-header". The name arrives LOWERCASED (header_lower is what every
   entry point runs first), so these comparisons are the spec's byte-case-insensitive match. */
static int header_is_forbidden_request(const char *lower_name, const char *value)
{
    size_t i;
    for (i = 0; i < sizeof(FORBIDDEN_REQUEST_FETCH) / sizeof(FORBIDDEN_REQUEST_FETCH[0]); i++)
        if (header_ci_eq(lower_name, FORBIDDEN_REQUEST_FETCH[i])) return 1;
    for (i = 0; i < sizeof(FORBIDDEN_REQUEST_PNA) / sizeof(FORBIDDEN_REQUEST_PNA[0]); i++)
        if (header_ci_eq(lower_name, FORBIDDEN_REQUEST_PNA[i])) return 1;
    /* the two PREFIXES the spec reserves for the browser and for the platform */
    if (!strncmp(lower_name, "proxy-", 6) || !strncmp(lower_name, "sec-", 4)) return 1;
    /* the method-override family is forbidden only for the VALUES that would smuggle a forbidden method */
    if (header_ci_eq(lower_name, "x-http-method") || header_ci_eq(lower_name, "x-http-method-override") ||
        header_ci_eq(lower_name, "x-method-override"))
        return value && header_value_has_forbidden_method(value);
    return 0;
}

bool header_forbidden_request(const char *lower_name, const char *value)
{
    return header_is_forbidden_request(lower_name, value) != 0;
}

/* Fetch §2.2.2 Headers' "forbidden response-header name": the two a page may not put on a response it did not
   receive. */
static int header_is_forbidden_response(const char *lower_name)
{
    return header_ci_eq(lower_name, "set-cookie") || header_ci_eq(lower_name, "set-cookie2");
}

/* Fetch §2.2.2 Headers' "CORS-unsafe request-header byte": what a safelisted value may not contain. */
static int header_is_cors_unsafe_byte(unsigned char c)
{
    return (c < 0x20 && c != 0x09) || c == 0x7f || !!strchr("\"():<>?@[\\]{}", (char)c);
}

/* Fetch §2.2.2 Headers' "CORS-safelisted request-header". The names each have their OWN value rule — this is
   not a name list with a length cap bolted on, and treating it as one would let
   `Content-Type: application/json` through as safelisted, which is the whole difference between a preflighted
   request and one that is not. */
static int header_is_cors_safelisted(const char *lower_name, const char *value)
{
    size_t i, n = strlen(value);

    /* §2.2.2 HAS A FIFTH ARM THIS COMPONENT HAS NOT BUILT: "`range`: Let rangeValue be the result of parsing a
       single range header value given value and false. If rangeValue is failure, then return false. If
       rangeValue[0] is null, then return false." — a suffix range like `bytes=-500` is deliberately NOT
       safelisted. Every caller today reaches this through the no-CORS safelist, whose four names do not
       include `range`, so the arm is unreachable; this is what makes it CRASH the day the CORS-unsafe
       request-header names (the preflight computation) asks the question for real, rather than quietly
       answering false and preflighting a request Chrome sends directly. */
    DCHECK(strcmp(lower_name, "range") != 0,
           "Fetch §2.2.2 Headers' CORS-safelisted request-header was asked about `range`, whose arm parses a "
           "single range header value and is not built — build it here rather than letting this answer false");
    if (n > 128) return 0;
    if (!strcmp(lower_name, "accept")) {
        for (i = 0; i < n; i++) if (header_is_cors_unsafe_byte((unsigned char)value[i])) return 0;
        return 1;
    }
    if (!strcmp(lower_name, "accept-language") || !strcmp(lower_name, "content-language")) {
        for (i = 0; i < n; i++) {
            unsigned char c = (unsigned char)value[i];
            if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) continue;
            if (c == 0x20 || c == '*' || c == ',' || c == '-' || c == '.' || c == ';' || c == '=') continue;
            return 0;
        }
        return 1;
    }
    if (!strcmp(lower_name, "content-type")) {
        const char *semi;
        size_t essence;
        for (i = 0; i < n; i++) if (header_is_cors_unsafe_byte((unsigned char)value[i])) return 0;
        /* the MIME type's ESSENCE — everything before the first `;`, trimmed — must be one of three */
        semi = strchr(value, ';');
        essence = semi ? (size_t)(semi - value) : n;
        while (essence > 0 && header_is_ws((unsigned char)value[essence - 1])) essence--;
        {
            static const char *const OK[] = { "application/x-www-form-urlencoded", "multipart/form-data",
                                              "text/plain" };
            size_t k;
            for (k = 0; k < sizeof(OK) / sizeof(OK[0]); k++)
                if (essence == strlen(OK[k]) && !strncasecmp(value, OK[k], essence)) return 1;
        }
        return 0;
    }
    return 0;
}

/* Fetch §2.2.2 Headers' "no-CORS-safelisted request-header NAME": one of four. It is its own predicate
   because `delete` asks the NAME question on its own — with no value to ask the value question about. */
static int header_is_no_cors_safelisted_name(const char *lower_name)
{
    return !strcmp(lower_name, "accept") || !strcmp(lower_name, "accept-language") ||
           !strcmp(lower_name, "content-language") || !strcmp(lower_name, "content-type");
}

/* Fetch §2.2.2 Headers' "no-CORS-safelisted request-header": one of those four names, and then the CORS rule
   for its value. */
static int header_is_no_cors_safelisted(const char *lower_name, const char *value)
{
    return header_is_no_cors_safelisted_name(lower_name) && header_is_cors_safelisted(lower_name, value);
}

/* Fetch §2.2.2 Headers' "privileged no-CORS request-header name" — stated ONCE, because two things read it:
   §5.1's `delete`, which lets one THROUGH (the browser's header is exactly what unprivileged code is allowed
   to drop), and §5.1's "remove privileged no-CORS request-headers", which strips every one of them after any
   unprivileged write. The NAMES are §2.2.2's; both algorithms that consult them are the class's. */
static const char *const HEADER_PRIVILEGED_NO_CORS[] = { "range" };

static int header_is_privileged_no_cors(const char *lower_name)
{
    size_t i;
    for (i = 0; i < sizeof(HEADER_PRIVILEGED_NO_CORS) / sizeof(HEADER_PRIVILEGED_NO_CORS[0]); i++)
        if (!strcmp(lower_name, HEADER_PRIVILEGED_NO_CORS[i])) return 1;
    return 0;
}

/* §5.1 "validating (name, value) for headers", AFTER header_check has done step 1's name/value syntax half.
   Returns 1 to write, 0 to silently do nothing, -1 with a TypeError live.
   STEP 3 IS "request" AND NOT "request-no-cors". The spec says so twice — the step names one guard, and the
   note under it says "Steps for 'request-no-cors' are not shared as you cannot have a fake value (for
   delete()) that always succeeds in CORS-safelisted request-header". This asked the forbidden question for
   both, which made `delete` under "request-no-cors" answer out of the forbidden list instead of out of that
   guard's own step. */
static int headers_guard_allows(JSContext *ctx, uint8_t guard, const char *name, const char *value)
{
    char *lower = header_lower(name);
    int r = 1;
    if (guard == HEADERS_GUARD_IMMUTABLE) {
        JS_ThrowTypeError(ctx, "the headers are immutable");
        r = -1;
    } else if (guard == HEADERS_GUARD_REQUEST && header_is_forbidden_request(lower, value)) {
        r = 0;
    } else if (guard == HEADERS_GUARD_RESPONSE && header_is_forbidden_response(lower)) {
        r = 0;
    }
    free(lower);
    return r;
}


char *header_value_example(JSContext *ctx, JSValueConst unknown)
{
    JSValue ex = concolic_example_string(ctx, unknown);
    const char *bytes;
    size_t len = 0, n = 0;
    char *norm;

    if (!JS_IsString(ex)) { JS_FreeValue(ctx, ex); return NULL; }
    bytes = JS_ToCStringLen(ctx, &len, ex);
    JS_FreeValue(ctx, ex);
    CHECK(bytes != NULL, "headers: OOM reading an unknown header value's example");
    norm = idl_is_bytestring(bytes, len) ? header_value_normalize_valid(bytes, len, &n) : NULL;
    JS_FreeCString(ctx, bytes);
    return norm;
}

/* ---- the list a page writes ------------------------------------------------------------------------------ */

/* A Headers object's header list, and the list §5.1's fill builds for one: a JS Array of pairs [name, value].
   `name` is the lowercased name's String. `value` is the normalized value's String, or the unknown the value
   was written from, so a read answers the unknown and never its display shape. It is a JS value because a
   flow's writes to it are the flow's own: an append is a property write the running flow's COW delta captures,
   and a step machine holds the list as a declared slot. A pair is never edited once it is appended. */
JSValue header_pairs_new(JSContext *ctx)
{
    JSValue pairs = JS_NewArray(ctx);

    CHECK(!JS_IsException(pairs), "headers: OOM allocating a header list");
    return pairs;
}

static uint32_t pairs_len(JSContext *ctx, JSValueConst pairs)
{
    int64_t n = -1;

    CHECK(JS_GetLength(ctx, pairs, &n) >= 0 && n >= 0 && n <= INT32_MAX, "a header list has no length");
    return (uint32_t)n;
}

/* Pair `i`: its name's bytes (JS_FreeCString) and, where `value` is asked, its value (owned). */
static const char *pairs_at(JSContext *ctx, JSValueConst pairs, uint32_t i, JSValue *value)
{
    JSValue pair = JS_GetPropertyUint32(ctx, pairs, i);
    JSValue nv = JS_GetPropertyUint32(ctx, pair, 0);
    const char *name;

    DCHECK(JS_IsString(nv), "a header list entry holds no name — only pairs_push writes one");
    name = JS_ToCString(ctx, nv);
    CHECK(name != NULL, "headers: OOM reading a header's name");
    if (value) *value = JS_GetPropertyUint32(ctx, pair, 1);
    JS_FreeValue(ctx, nv);
    JS_FreeValue(ctx, pair);
    return name;
}

/* §2.2.2 append over a name already lowercased. `value` is borrowed. */
static void pairs_push(JSContext *ctx, JSValueConst pairs, const char *lower_name, JSValueConst value)
{
    JSValue pair = JS_NewArray(ctx), name = JS_NewString(ctx, lower_name);

    DCHECK(JS_IsString(value) || concolic_is(value),
           "a header value was stored that is neither bytes nor an unknown");
    CHECK(!JS_IsException(pair) && !JS_IsException(name), "headers: OOM building a header");
    CHECK(JS_DefinePropertyValueUint32(ctx, pair, 0, name, JS_PROP_C_W_E) >= 0 &&
          JS_DefinePropertyValueUint32(ctx, pair, 1, JS_DupValue(ctx, value), JS_PROP_C_W_E) >= 0 &&
          JS_DefinePropertyValueUint32(ctx, pairs, pairs_len(ctx, pairs), pair, JS_PROP_C_W_E) >= 0,
          "headers: a header list refused an entry");
}

/* §2.2.2 delete: every pair named `lower_name` leaves and the rest keep their order. */
static void pairs_delete(JSContext *ctx, JSValueConst pairs, const char *lower_name)
{
    uint32_t n = pairs_len(ctx, pairs), i, w = 0;

    for (i = 0; i < n; i++) {
        JSValue pair = JS_GetPropertyUint32(ctx, pairs, i);
        const char *name = pairs_at(ctx, pairs, i, NULL);
        bool hit = !strcmp(name, lower_name);

        JS_FreeCString(ctx, name);
        if (hit || w == i)
            JS_FreeValue(ctx, pair);
        else
            CHECK(JS_DefinePropertyValueUint32(ctx, pairs, w, pair, JS_PROP_C_W_E) >= 0,
                  "headers: a header list refused an entry it already held");
        if (!hit) w++;
    }
    if (w != n)
        CHECK(JS_SetLength(ctx, pairs, w) >= 0, "headers: a header list refused its new length");
}

static bool pairs_contains(JSContext *ctx, JSValueConst pairs, const char *lower_name)
{
    uint32_t n = pairs_len(ctx, pairs), i;
    bool yes = false;

    for (i = 0; i < n && !yes; i++) {
        const char *name = pairs_at(ctx, pairs, i, NULL);

        yes = !strcmp(name, lower_name);
        JS_FreeCString(ctx, name);
    }
    return yes;
}

/* The bytes a C reader takes for a value: a String's own, or an unknown's display shape, which is shown and
   never handed to the page. JS_FreeCString. */
static const char *pair_value_bytes(JSContext *ctx, JSValueConst value)
{
    const char *b = concolic_name_cstr(ctx, value);

    CHECK(b != NULL, "headers: OOM reading a header value's bytes");
    return b;
}

/* Fetch §2.2.2 "get a header name name from a header list list" as a value: JS_UNINITIALIZED where the list
   has none, the value where it has one, and otherwise the values joined by 0x2C 0x20 through §22.1.3.5's
   concatenation, so a join holding an unknown is derived from its parts. Owned. */
static JSValue pairs_get_value(JSContext *ctx, JSValueConst pairs, const char *lower_name)
{
    JSValue out = JS_UNINITIALIZED;
    uint32_t n = pairs_len(ctx, pairs), i;

    for (i = 0; i < n; i++) {
        JSValue v;
        const char *name = pairs_at(ctx, pairs, i, &v);
        bool hit = !strcmp(name, lower_name);

        JS_FreeCString(ctx, name);
        if (!hit) { JS_FreeValue(ctx, v); continue; }
        if (JS_IsUninitialized(out)) { out = v; continue; }
        out = concolic_string_concat(ctx, concolic_string_concat(ctx, out, JS_NewString(ctx, ", ")), v);
    }
    return out;
}

char *header_pairs_get(JSContext *ctx, JSValueConst pairs, const char *name)
{
    char *lo = header_lower(name), *out = NULL;
    uint32_t n = pairs_len(ctx, pairs), i;
    size_t len = 0;

    for (i = 0; i < n; i++) {
        JSValue v;
        const char *nm = pairs_at(ctx, pairs, i, &v);

        if (!strcmp(nm, lo)) {
            const char *b = pair_value_bytes(ctx, v);
            size_t add = strlen(b), sep = out ? 2 : 0;

            out = lexbor_realloc(out, len + sep + add + 1);
            CHECK(out, "headers: OOM joining a header's values");
            if (sep) memcpy(out + len, ", ", 2);
            memcpy(out + len + sep, b, add + 1);
            len += sep + add;
            JS_FreeCString(ctx, b);
        }
        JS_FreeCString(ctx, nm);
        JS_FreeValue(ctx, v);
    }
    free(lo);
    return out;
}

void header_pairs_append(JSContext *ctx, JSValueConst pairs, const char *name, const char *value)
{
    char *lo = header_lower(name);
    JSValue v = JS_NewString(ctx, value);

    CHECK(!JS_IsException(v), "headers: OOM holding a header value");
    pairs_push(ctx, pairs, lo, v);
    JS_FreeValue(ctx, v);
    free(lo);
}

void header_pairs_state(JSContext *ctx, JSValueConst pairs, HeaderList *out)
{
    uint32_t n = pairs_len(ctx, pairs), i;

    for (i = 0; i < n; i++) {
        JSValue v;
        const char *name = pairs_at(ctx, pairs, i, &v);
        const char *bytes = pair_value_bytes(ctx, v);

        if (concolic_is(v)) {
            char *ex = header_value_example(ctx, v);
            HeaderUnknown unk = { concolic_root_c(v), ex };

            header_list_append_unknown(out, name, bytes, &unk);
            free(ex);
        } else {
            header_list_append(out, name, bytes);
        }
        JS_FreeCString(ctx, bytes);
        JS_FreeCString(ctx, name);
        JS_FreeValue(ctx, v);
    }
}

/* The value an unknown header value is stored as: itself, or, where §2.2.2's normalize changes the bytes of
   its example, the derivation that normalizes it, holding the normalized bytes for its example. Owned. */
static JSValue header_unknown_stored(JSContext *ctx, JSValueConst unknown)
{
    JSValue ex = concolic_example_string(ctx, unknown), out;
    size_t len = 0, n = 0;
    const char *b;
    char *norm;

    if (!JS_IsString(ex)) { JS_FreeValue(ctx, ex); return JS_DupValue(ctx, unknown); }
    b = JS_ToCStringLen(ctx, &len, ex);
    CHECK(b != NULL, "headers: OOM reading an unknown header value's example");
    norm = header_normalize_value(b, len, &n);
    out = n == len ? JS_DupValue(ctx, unknown)
                   : concolic_new_derived(ctx, "Headers.normalize", &unknown, 1, JS_NewStringLen(ctx, norm, n));
    free(norm);
    JS_FreeCString(ctx, b);
    JS_FreeValue(ctx, ex);
    return out;
}

/* A written value as the list stores it (owned): for bytes, Web IDL §3.2.11 ByteString step 2, §5.1 validate
   step 1 over (name, value) and §2.2.2's normalize; for an unknown, step 1's name half and the value itself,
   whose own validity is header_value_example's residual. `name_decided` says a fork already decided the name
   half. JS_EXCEPTION with the TypeError live. */
static JSValue header_stored_value(JSContext *ctx, const char *name, size_t name_len, JSValueConst value,
                                   bool name_decided)
{
    const char *bytes;
    char *norm = NULL;
    size_t len = 0;
    JSValue out;
    int bad;

    if (concolic_is(value)) {
        if (!name_decided && header_check(ctx, name, name_len, NULL, 0, NULL) < 0) return JS_EXCEPTION;
        return header_unknown_stored(ctx, value);
    }
    bytes = JS_ToCStringLen(ctx, &len, value);
    if (!bytes) return JS_EXCEPTION;
    bad = headers_bytestring(ctx, bytes, len, "value") < 0 ||
          (name_decided ? header_value_check(ctx, bytes, len, &norm)
                        : header_check(ctx, name, name_len, bytes, len, &norm)) < 0;
    JS_FreeCString(ctx, bytes);
    if (bad) return JS_EXCEPTION;
    out = JS_NewString(ctx, norm);
    free(norm);
    return out;
}

/* §5.1 "remove privileged no-CORS request-headers": every append, set and delete under the "request-no-cors"
   guard ends with it, because the write was made by unprivileged code. */
static void pairs_remove_privileged_no_cors(JSContext *ctx, JSValueConst pairs)
{
    size_t i;

    for (i = 0; i < sizeof(HEADER_PRIVILEGED_NO_CORS) / sizeof(HEADER_PRIVILEGED_NO_CORS[0]); i++)
        pairs_delete(ctx, pairs, HEADER_PRIVILEGED_NO_CORS[i]);
}

/* §5.1 "append a header (name, value) to a Headers object", steps 2-5, over the list and the guard, which is
   all the algorithm reads; the `append` member, the fill and §5.4's carried list are its callers. `value` is
   as the list stores it (header_stored_value). The guard reads a value's bytes, and an unknown's are its
   display shape. 0 for written or silently refused, which the spec does not let a caller distinguish, and -1
   with a TypeError live. */
static int headers_append_one(JSContext *ctx, JSValueConst pairs, uint8_t guard, const char *name,
                              JSValueConst value)
{
    char *lower = header_lower(name);
    const char *bytes = pair_value_bytes(ctx, value);
    int allow = headers_guard_allows(ctx, guard, name, bytes);   /* step 2 */

    /* Step 3: under "request-no-cors" the header must be no-CORS-safelisted with the value it would end up
       with, so the test joins where `set`'s does not. */
    if (allow > 0 && guard == HEADERS_GUARD_REQUEST_NO_CORS) {
        char *existing = header_pairs_get(ctx, pairs, lower), *combined = NULL;
        const char *test = bytes;

        if (existing) {
            size_t a = strlen(existing), b = strlen(bytes);

            combined = lexbor_malloc(a + b + 3);
            CHECK(combined, "headers: OOM joining a no-cors value");
            memcpy(combined, existing, a);
            combined[a] = ','; combined[a + 1] = ' ';
            memcpy(combined + a + 2, bytes, b + 1);
            test = combined;
        }
        free(existing);
        if (!header_is_no_cors_safelisted(lower, test)) allow = 0;
        free(combined);
    }
    if (allow > 0) {
        pairs_push(ctx, pairs, lower, value);                                                    /* step 4 */
        if (guard == HEADERS_GUARD_REQUEST_NO_CORS) pairs_remove_privileged_no_cors(ctx, pairs);   /* step 5 */
    }
    JS_FreeCString(ctx, bytes);
    free(lower);
    return allow < 0 ? -1 : 0;
}

int header_pairs_append_guarded(JSContext *ctx, JSValueConst pairs, HeadersGuard guard, JSValueConst from)
{
    uint32_t n = pairs_len(ctx, from), i;

    for (i = 0; i < n; i++) {
        JSValue v;
        const char *name = pairs_at(ctx, from, i, &v);
        int r = headers_append_one(ctx, pairs, (uint8_t)guard, name, v);

        JS_FreeCString(ctx, name);
        JS_FreeValue(ctx, v);
        if (r < 0) return -1;
    }
    return 0;
}

/* ---- the interface ------------------------------------------------------------------------------------ */

/* §5.1: "A Headers object has an associated guard." The guard is the object's and the header list is the
   pairs above; the runtime releases, marks and writes the record by its layout. */
typedef struct { JSValue pairs; uint8_t guard; } HeadersObj;
static const uint16_t HEADERS_VALS[] = { offsetof(HeadersObj, pairs) };
static const JSRecordLayout HEADERS_LAYOUT = { sizeof(HeadersObj), HEADERS_VALS, 1, NULL, 0, NULL };

JSValueConst headers_pairs_of(JSValueConst v)
{
    HeadersObj *h = JS_GetOpaque(v, g_headers_class);

    DCHECK(h != NULL, "the header list of something that is not a Headers was asked for");
    return h->pairs;
}

HeadersGuard headers_guard_of(JSValueConst v)
{
    HeadersObj *h = JS_GetOpaque(v, g_headers_class);
    DCHECK(h != NULL, "the guard of something that is not a Headers was asked for");
    return (HeadersGuard)h->guard;
}

static HeadersObj *headers_of(JSContext *ctx, JSValueConst v)
{
    HeadersObj *h = JS_GetOpaque(v, g_headers_class);
    if (!h) JS_ThrowTypeError(ctx, "not a Headers");
    return h;
}

/* A Headers with an empty list and `guard`; `*ph` is its record. */
static JSValue headers_alloc(JSContext *ctx, HeadersGuard guard, HeadersObj **ph)
{
    HeadersObj *h;
    JSValue obj, proto;

    DCHECK(g_headers_class != 0, "a Headers was built before the class existed — headers_init runs at install");
    proto = JS_GetClassProto(ctx, g_headers_class);
    DCHECK(!JS_IsNull(proto), "a Headers was minted in a realm that never ran its install");
    obj = JS_NewObjectProtoClass(ctx, proto, g_headers_class);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj))
        return obj;
    h = lexbor_calloc(1, sizeof *h);
    CHECK(h, "headers: OOM building a Headers");
    h->guard = (uint8_t)guard;
    h->pairs = header_pairs_new(ctx);
    JS_SetOpaque(obj, h);
    *ph = h;
    return obj;
}

JSValue headers_new(JSContext *ctx, const HeaderList *src, HeadersGuard guard)
{
    HeadersObj *h;
    JSValue obj = headers_alloc(ctx, guard, &h);
    int i;

    if (JS_IsException(obj))
        return obj;
    /* The list is one this engine built, already validated; the guard governs what the page may then do. */
    for (i = 0; src && i < src->n; i++) {
        DCHECK(!src->e[i].shape, "a Headers was built over a statement of an unknown's display shape — a list "
                                 "that holds unknowns is built as pairs (headers_new_pairs)");
        header_pairs_append(ctx, h->pairs, src->e[i].name, src->e[i].value);
    }
    return obj;
}

JSValue headers_new_pairs(JSContext *ctx, JSValueConst pairs, HeadersGuard guard)
{
    HeadersObj *h;
    JSValue obj = headers_alloc(ctx, guard, &h);
    uint32_t n, i;

    if (JS_IsException(obj))
        return obj;
    n = pairs_len(ctx, pairs);
    for (i = 0; i < n; i++) {
        JSValue v;
        const char *name = pairs_at(ctx, pairs, i, &v);

        pairs_push(ctx, h->pairs, name, v);
        JS_FreeCString(ctx, name);
        JS_FreeValue(ctx, v);
    }
    return obj;
}

enum { HDR_APPEND = 0, HDR_SET, HDR_DELETE, HDR_GET, HDR_HAS, HDR_GETSETCOOKIE, HDR_MEMBER_N };
/* THE AGENT'S POOL ENTRIES, one per §5.1 Headers class operation — the OBJECTS they are installed as are each realm's. */
static int g_id[HDR_MEMBER_N];

/* §5.1's two reads over an unknown name. Web IDL places an unknown ByteString argument as itself
   (core/idl_args.h's idl_concolic_rule), and a name has no bytes to look up, so the member answers a value
   derived from the name, whose example is the one lookup run on the name's own example: absent where the name
   has no example or its example is no header name (step 1 throws there, which is not a value). Where the
   lookup finds an unknown value the answer is derived from it too.
   Named residual. Not covered: the world in which the unknown name is not a token, where §5.1 step 1 throws a
   TypeError into the page's own `try`. Next diff builds: a declared predicate position in core/idl_args.h
   whose rule forks over header_name_is_valid at the branch seam. Absence shows as a flow whose `h.get(n)`
   sits inside a `try` reaching its `catch` in no world. */
static JSValue headers_read_unknown_name(JSContext *ctx, JSValueConst pairs, JSValueConst name, int magic)
{
    JSValue ex = concolic_example(ctx, name);
    JSValue real = JS_UNDEFINED, held = JS_UNDEFINED, out;
    JSValueConst operands[2];
    int nop = 0;

    if (JS_IsString(ex)) {
        const char *exn = JS_ToCString(ctx, ex);

        CHECK(exn != NULL, "headers: OOM reading the String an unknown header name's example holds");
        if (header_name_is_valid(exn, strlen(exn))) {
            char *lo = header_lower(exn);

            if (magic == HDR_HAS) {
                real = JS_NewBool(ctx, pairs_contains(ctx, pairs, lo));
            } else {
                JSValue v = pairs_get_value(ctx, pairs, lo);

                if (JS_IsUninitialized(v)) real = JS_NULL;
                else if (concolic_is(v)) { held = v; real = concolic_example(ctx, v); }
                else real = v;
            }
            free(lo);
        }
        JS_FreeCString(ctx, exn);
    }
    JS_FreeValue(ctx, ex);
    operands[nop++] = name;
    if (!JS_IsUndefined(held)) operands[nop++] = held;
    out = concolic_new_derived(ctx, magic == HDR_HAS ? "Headers.has" : "Headers.get", operands, nop, real);
    JS_FreeValue(ctx, held);
    return out;
}

static JSValue js_headers_member(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    HeadersObj *h = headers_of(ctx, this_val);
    JSValue value = JS_UNDEFINED, r = JS_UNDEFINED;
    const char *name;
    size_t name_len = 0;
    char *lower;
    int allow = 1;

    if (!h)
        return JS_EXCEPTION;
    if (magic == HDR_GETSETCOOKIE) {
        /* §5.1 getSetCookie(): every `set-cookie` value on its own, the one member for which the list's
           repeats are the answer. */
        JSValue arr = JS_NewArray(ctx);
        uint32_t n, i, k = 0;

        if (JS_IsException(arr)) return arr;
        n = pairs_len(ctx, h->pairs);
        for (i = 0; i < n; i++) {
            JSValue v;
            const char *nm = pairs_at(ctx, h->pairs, i, &v);

            if (!strcmp(nm, "set-cookie"))
                CHECK(JS_DefinePropertyValueUint32(ctx, arr, k++, v, JS_PROP_C_W_E) >= 0,
                      "headers: getSetCookie()'s list refused a value");
            else
                JS_FreeValue(ctx, v);
            JS_FreeCString(ctx, nm);
        }
        return arr;
    }
    DCHECK(argc >= 1, "a Headers member was declared with fewer arguments than its IDL lists");
    if (concolic_is(argv[0]) && (magic == HDR_GET || magic == HDR_HAS))
        return headers_read_unknown_name(ctx, h->pairs, argv[0], magic);
    /* Named residual. Not covered: an unknown `name` at append, set and delete, where the coercion below ends
       the flow (a `const char *` cannot carry an unknown, and its display shape is no token, so projecting it
       would let the solver's spelling decide step 1's TypeError). Next diff builds: a pair whose name is the
       unknown, with set and delete matching the entry that names that source. Absence shows as a flow that
       writes a header whose name it computed ending at this coercion. */
    name = JS_ToCStringLen(ctx, &name_len, argv[0]);
    if (!name) return JS_EXCEPTION;
    /* §5.1 validate step 1 runs for every member: `headers.get({})` reads a name of "[object Object]", which
       is no token, and the spec makes that a TypeError rather than a miss. */
    if (magic == HDR_APPEND || magic == HDR_SET) {
        value = header_stored_value(ctx, name, name_len, argv[1], false);
        if (JS_IsException(value)) { JS_FreeCString(ctx, name); return JS_EXCEPTION; }
    } else if (header_check(ctx, name, name_len, NULL, 0, NULL) < 0) {
        JS_FreeCString(ctx, name);
        return JS_EXCEPTION;
    }
    lower = header_lower(name);
    switch (magic) {
    case HDR_APPEND:
        allow = headers_append_one(ctx, h->pairs, h->guard, name, value) < 0 ? -1 : 1;
        break;
    case HDR_SET: {
        /* §5.1 set steps 2-5. Step 3 tests the value on its own, because set replaces rather than joins. */
        const char *bytes = pair_value_bytes(ctx, value);

        allow = headers_guard_allows(ctx, h->guard, name, bytes);
        if (allow > 0 && h->guard == HEADERS_GUARD_REQUEST_NO_CORS && !header_is_no_cors_safelisted(lower, bytes))
            allow = 0;
        JS_FreeCString(ctx, bytes);
        if (allow > 0) {
            pairs_delete(ctx, h->pairs, lower);
            pairs_push(ctx, h->pairs, lower, value);
            if (h->guard == HEADERS_GUARD_REQUEST_NO_CORS) pairs_remove_privileged_no_cors(ctx, h->pairs);
        }
        break;
    }
    case HDR_DELETE:
        /* §5.1 delete steps 1-5. Step 1 validates with the dummy value ``, which is why the method-override
           names are deletable; step 2 lets a privileged no-CORS name through, since §2.2.2's note says such
           headers "will be removed if the request is modified by unprivileged APIs". */
        allow = headers_guard_allows(ctx, h->guard, name, "");
        if (allow > 0 && h->guard == HEADERS_GUARD_REQUEST_NO_CORS &&
            !header_is_no_cors_safelisted_name(lower) && !header_is_privileged_no_cors(lower))
            allow = 0;
        if (allow > 0) {
            pairs_delete(ctx, h->pairs, lower);
            if (h->guard == HEADERS_GUARD_REQUEST_NO_CORS) pairs_remove_privileged_no_cors(ctx, h->pairs);
        }
        break;
    case HDR_HAS:
        r = JS_NewBool(ctx, pairs_contains(ctx, h->pairs, lower));
        break;
    default:
        DCHECK(magic == HDR_GET, "a Headers member was declared with a magic this component does not answer");
        r = pairs_get_value(ctx, h->pairs, lower);
        if (JS_IsUninitialized(r)) r = JS_NULL;   /* §5.1: absent is null, not "" */
    }
    free(lower);
    JS_FreeCString(ctx, name);
    JS_FreeValue(ctx, value);
    return allow < 0 ? JS_EXCEPTION : r;
}

/* ---- iteration: §5.1's `iterable<ByteString, ByteString>` ------------------------------------------------ */

/* §2.2.2 "sort and combine" over `pairs`: the count of pairs iteration yields and, for 0 <= at < count, the
   at-th one, its name in `*key` and its value in `*value` (owned). Each name is yielded once, lowercased and
   in byte order, with its values combined as §2.2.2's get combines them; `set-cookie` yields each of its
   values on its own. Computed per call, because the list is live under the page's callback. */
static int pairs_sort_combine(JSContext *ctx, JSValueConst pairs, int at, JSValue *key, JSValue *value)
{
    typedef struct { char *name; uint32_t at; } Row;   /* `at`: the first pair of the name, or the cookie's */
    uint32_t n = pairs_len(ctx, pairs), i;
    Row *rows = n ? lexbor_malloc(n * sizeof *rows) : NULL;
    int nr = 0, j;

    CHECK(!n || rows, "headers: OOM combining a header list for iteration");
    for (i = 0; i < n; i++) {
        const char *name = pairs_at(ctx, pairs, i, NULL);
        bool seen = false;

        if (strcmp(name, "set-cookie"))
            for (j = 0; j < nr && !seen; j++) seen = !strcmp(rows[j].name, name);
        if (!seen) {
            /* After its equals, so the values of `set-cookie` keep the list's order. */
            for (j = nr; j > 0 && strcmp(rows[j - 1].name, name) > 0; j--) rows[j] = rows[j - 1];
            rows[j].name = header_dup(name);
            rows[j].at = i;
            nr++;
        }
        JS_FreeCString(ctx, name);
    }
    if (at >= 0) {
        DCHECK(at < nr, "a Headers pair was asked for past the end of the combined list");
        *key = JS_NewString(ctx, rows[at].name);
        if (!strcmp(rows[at].name, "set-cookie"))
            JS_FreeCString(ctx, pairs_at(ctx, pairs, rows[at].at, value));
        else
            *value = pairs_get_value(ctx, pairs, rows[at].name);
    }
    for (j = 0; j < nr; j++) free(rows[j].name);
    free(rows);
    return nr;
}

/* Web IDL §3.7.9.1 "Default iterator objects" is the shared one (core/idl_iter.h); what is Headers' own is
   the two operations below. */
static int headers_pair_count(JSContext *ctx, JSValueConst target)
{
    HeadersObj *h = JS_GetOpaque(target, g_headers_class);

    return h ? pairs_sort_combine(ctx, h->pairs, -1, NULL, NULL) : -1;
}

static void headers_pair_at(JSContext *ctx, JSValueConst target, int i, JSValue *key, JSValue *value)
{
    HeadersObj *h = JS_GetOpaque(target, g_headers_class);

    DCHECK(h != NULL, "a Headers iterator outlived the Headers it holds a reference to");
    DCHECK(i >= 0, "a Headers pair was asked for at a negative index");
    pairs_sort_combine(ctx, h->pairs, i, key, value);
}

static const IdlPairIterOps HEADERS_PAIR_OPS = { headers_pair_count, headers_pair_at, "Headers" };
static int g_pair_handle = -1;

/* ---- the fill (HeadersInit -> a header list) ------------------------------------------------------------ */

enum { FILL_START = 0, FILL_ITER_ASKED, FILL_SEQ_PAIR, FILL_SEQ_ITEM, FILL_KEY_PAIR, FILL_VALUE_STR,
       FILL_NAME_VALID };

/* ---- the fill's own state ---------------------------------------------------------------------------------- */

void headers_fill_init(HeadersFill *f)
{
    memset(f, 0, sizeof *f);
    f->name = f->value = JS_UNDEFINED;
    record_cursor_init(&f->rec);
    f->item[0] = f->item[1] = JS_UNDEFINED;
    iter_cursor_init(&f->outer);
    iter_cursor_init(&f->inner);
}

void headers_fill_visit(JSContext *ctx, HeadersFill *f, JSStepVisit *v)
{
    record_cursor_visit(ctx, &f->rec, v);
    v->val(ctx, &f->name);
    v->val(ctx, &f->value);
    v->val(ctx, &f->item[0]);
    v->val(ctx, &f->item[1]);
    iter_cursor_visit(ctx, &f->outer, v);
    iter_cursor_visit(ctx, &f->inner, v);
}

/* The two questions the fill asks of an unknown name. Completion 0 is the name that passes, 1 the TypeError. */
#define HDR_KEY_BYTESTRING_OP "Web IDL §3.2.11 ByteString step 2 (no element of x is greater than 255), over a " \
                              "Headers record key"
#define HDR_NAME_OP "Fetch §5.1 Headers class, validate a header step 1 (name is a header name)"

static int headers_is_bytestring(const char *s, size_t n) { return idl_is_bytestring(s, n); }

/* Which completion `valid` reaches on the unknown `v`'s example: 0 when its bytes pass, 1 when they fail, and
   JS_OUTCOME_REAL_UNSTATED when the example is not a String. */
static int headers_example_real(JSContext *ctx, JSValueConst v, int (*valid)(const char *s, size_t n))
{
    JSValue ex = concolic_example(ctx, v);
    int real = JS_OUTCOME_REAL_UNSTATED;

    if (JS_IsString(ex)) {
        size_t n = 0;
        const char *b = JS_ToCStringLen(ctx, &n, ex);

        CHECK(b != NULL, "headers: could not read the bytes of a String example this engine minted");
        real = valid(b, n) ? 0 : 1;
        JS_FreeCString(ctx, b);
    }
    JS_FreeValue(ctx, ex);
    return real;
}

/* Web IDL §3.2.23 step 4.2.1's `typedKey = key converted to K`, and K is ByteString — run BEFORE the value's [[Get]] is
   issued, which is what makes a record of {a:"b", "\uFFFF":"d"} five operations and not six. A SYMBOL cannot
   be a ByteString, so an ENUMERABLE symbol key is a TypeError; skipping it was the older, wrong answer.
   An unknown key's §3.2.11 step 2 has both completions over its domain, so it forks, the real one being step 2
   run on the key's example; the arm that passes hands the key on as itself, and the fill asks §5.1's
   header-name question of it. `key` is the cursor's own slot, which the fork's snapshot carries. */
static int headers_record_key_ok(JSContext *ctx, JSStepHdr *h, JSValueConst key, void *user)
{
    size_t kn_len = 0;
    const char *kn;
    int r;

    (void)user;
    if (JS_IsSymbol(key)) {
        JS_ThrowTypeError(ctx, "a Symbol is not a valid header name");
        return -1;
    }
    if (concolic_is(key)) {
        int arm = 0;

        r = step_fork_run(ctx, h, key, HDR_KEY_BYTESTRING_OP, 2,
                          headers_example_real(ctx, key, headers_is_bytestring), &arm);
        if (r) return r;
        DCHECKF(arm == 0 || arm == 1, "the Headers record key's ByteString fork answered completion %d", arm);
        if (arm == 1) {
            JS_ThrowTypeError(ctx, "a header name is not a ByteString");
            return -1;
        }
        return 0;
    }
    kn = JS_ToCStringLen(ctx, &kn_len, key);
    if (!kn) return -1;
    r = headers_bytestring(ctx, kn, kn_len, "name");
    JS_FreeCString(ctx, kn);
    return r;
}

int headers_fill_run(JSContext *ctx, JSStepHdr *h, HeadersFill *f, JSValueConst init, JSValueConst out,
                     HeadersGuard guard, JSValue in, JSValue **out_cb, int *out_argc)
{
    int r;

    if (f->phase == FILL_START) {
        /* UNDEFINED IS "NOT GIVEN"; NULL IS NOT. `HeadersInit` is a union of two object types and Web IDL does
           not make it nullable, so `new Headers(null)` and `{headers: null}` are TypeErrors — while an absent
           optional argument, and an init object with no `headers` member, are simply no init. Treating the two
           alike accepted null silently, which wpt's headers-basic asserts against by name. */
        if (JS_IsUndefined(init)) { JS_FreeValue(ctx, in); return 0; }
        if (!JS_IsObject(init)) {
            JS_FreeValue(ctx, in);
            JS_ThrowTypeError(ctx, "a Headers init is not an object");
            return -1;
        }
        /* THERE IS NO "IT IS ALREADY A HEADERS" CASE. A shortcut copying the list stood here while the iterator
           was unbuilt, and it was WRONG in two ways the spec is explicit about: `new Headers(h)` resolves the
           `HeadersInit` union like any other object, so it takes the SEQUENCE arm through h's own @@iterator —
           which the page may have replaced (wpt installs a generator and expects its pairs, not h's list), and
           which COMBINES duplicate names the way iteration does. The route it dodged now exists, so it is gone
           rather than kept as the fast path for the common shape. */
        JS_FreeValue(ctx, in);   /* nothing here asked for it; the arm below starts its own request */
        in = JS_UNDEFINED;
        f->phase = FILL_ITER_ASKED;
    }
    /* WHICH ARM: Web IDL §3.2.25 step 11.2 picks `sequence<sequence<ByteString>>` over
       `record<ByteString, ByteString>` by `? GetMethod(V, %Symbol.iterator%)`, whose [[Get]] is an accessor or a
       Proxy trap away from being the page's code — so it is a request like every other read here. It was
       JS_IsArray, which is a DIFFERENT question: `new Headers(new Map(...))` is iterable and is not an array, so
       it took the record arm, found no own string keys, and produced an EMPTY header list — the request would
       have gone out missing exactly the headers the page set. Then it was the [[Get]] with `JS_IsFunction` on
       the result, which is a different question AGAIN: GetMethod makes a PRESENT non-callable a TypeError, so
       `new Headers({[Symbol.iterator]: 1})` walked the record arm — quietly, and to an empty list — where the
       standard throws. The three steps that decide that are ECMAScript's and are stated once, in idl_iter.c.
       AND THE METHOD IS HANDED ON, NOT RE-READ. §3.2.25 Union types' sequence arm is "Let method be
       ? GetMethod(V, %Symbol.iterator%). If method is not undefined, return the result of creating a sequence
       of that type from V AND METHOD", and §3.2.21.1 Creating a sequence from an iterable takes "an iterable
       iterable and an iterator getter method". This freed the answer and let the cursor read @@iterator again,
       which is TWO [[Get]]s of the page's value for ONE conversion: a Proxy `get` trap counts them, and an
       accessor may hand the second call a different function — so the sequence would be built by an iterator
       the union never inspected and never type-checked. idl_iter.h declares iter_cursor_init_from_method for
       exactly this entry and idl_args.c's `(DOMString or sequence<DOMString>)` union already uses it; this was
       the one union in the tree that did not. */
    if (f->phase == FILL_ITER_ASKED) {
        JSValue itf;
        r = step_getprop_run(ctx, h, init, JS_WellKnownSymbolAtom(JS_WKS_ITERATOR), in, &itf, out_cb, out_argc);
        if (r > 0) return r;
        if (r < 0) return -1;
        in = JS_UNDEFINED;
        r = idl_get_method(ctx, itf, "a Headers init's @@iterator");
        if (r <= 0) {
            JS_FreeValue(ctx, itf);
            if (r < 0) return -1;
            f->phase = FILL_KEY_PAIR;
        } else {
            iter_cursor_init_from_method(ctx, &f->outer, itf);   /* CONSUMES `itf` */
            f->phase = FILL_SEQ_PAIR;
        }
    }

    /* THE SEQUENCE ARM: `sequence<sequence<ByteString>>`. The outer cursor yields one PAIR per turn and the
       inner one yields that pair's items — Web IDL nests the protocol, so this nests the cursor rather than
       assuming the pair is an array. §5.1: a pair that does not hold exactly two items is a TypeError, which is
       why the inner runs one step PAST the second item rather than stopping at it. */
    while (f->phase == FILL_SEQ_PAIR || f->phase == FILL_SEQ_ITEM) {
        if (f->phase == FILL_SEQ_PAIR) {
            r = iter_cursor_run(ctx, h, &f->outer, init, in, out_cb, out_argc);
            if (r > 0) return r;
            if (r < 0) return -1;
            in = JS_UNDEFINED;
            if (f->outer.done) return 0;
            if (!JS_IsObject(f->outer.value)) {
                JS_ThrowTypeError(ctx, "a Headers init pair is not a sequence");
                return -1;
            }
            /* The previous pair's cursor released itself when it answered `done` (idl_iter.c's IT_GET_DONE),
               and reaching this line at all requires that: the only other way out of the item loop below is
               the three-or-more break, which throws before it can come back here. So this plants on a clear
               slot, and the release that used to stand in front of it named a state that cannot arrive. */
            iter_cursor_init(&f->inner);
            JS_FreeValue(ctx, f->item[0]); JS_FreeValue(ctx, f->item[1]);
            f->item[0] = f->item[1] = JS_UNDEFINED;
            f->nitem = 0;
            f->phase = FILL_SEQ_ITEM;
        }
        for (;;) {
            r = iter_cursor_run(ctx, h, &f->inner, f->outer.value, in, out_cb, out_argc);
            if (r > 0) return r;
            if (r < 0) return -1;
            in = JS_UNDEFINED;
            if (f->inner.done)
                break;
            if (f->nitem >= 2) { f->nitem = 3; break; }   /* three or more: the same TypeError as one */
            f->item[f->nitem] = f->inner.value;
            f->inner.value = JS_UNDEFINED;
            f->nitem++;
        }
        if (f->nitem != 2) {
            JS_ThrowTypeError(ctx, "a Headers init pair does not contain exactly two items");
            return -1;
        }
        {
            size_t kn_len = 0;
            const char *kn;
            JSValue stored;
            int bad;
            /* Web IDL §3.2.21.1 "Creating a sequence from an iterable" converts each element to ByteString.
               For an object that is ToString, the page's code, which this arm has no step_tostring_run for;
               an unknown name has no fork here as the record arm's has. An unknown value is stored as
               itself, which is what a Headers object's own iterator yields for one. */
            DCHECK(!concolic_is(f->item[0]),
                   "a Headers init pair carried an unknown name — this arm has no fork over §5.1 validate "
                   "step 1 for one; build the record arm's here");
            DCHECK(!JS_IsObject(f->item[0]) && (concolic_is(f->item[1]) || !JS_IsObject(f->item[1])),
                   "a Headers init pair carried an object item — Web IDL §3.2.21.1's element conversion to "
                   "ByteString is a ToString, and this arm has no step_tostring_run for it");
            kn = JS_ToCStringLen(ctx, &kn_len, f->item[0]);
            if (!kn) return -1;
            /* §3.2.11 step 2 for the element the standard converts first, before the value's own. */
            if (headers_bytestring(ctx, kn, kn_len, "name") < 0) {
                JS_FreeCString(ctx, kn);
                return -1;
            }
            stored = header_stored_value(ctx, kn, kn_len, f->item[1], false);
            /* §5.1 fill appends through the append algorithm, every step of it: a Request built with
               {headers:{Host:"x"}} silently drops it, and a no-cors one drops every header that is not
               no-CORS-safelisted. */
            bad = JS_IsException(stored) || headers_append_one(ctx, out, guard, kn, stored) < 0;
            JS_FreeValue(ctx, stored);
            JS_FreeCString(ctx, kn);
            if (bad) return -1;   /* the TypeError is live */
        }
        f->phase = FILL_SEQ_PAIR;
    }
    /* The RECORD arm is Web IDL §3.2.23's own cursor — [[OwnPropertyKeys]], then a descriptor and a [[Get]] per key,
       every one of them a request. It is shared with URLSearchParams because the conversion is Web IDL's and
       not this component's; what stays here is the per-pair half, which is where the ByteString key check, the
       §5.1 guard and the concolic shape live. */
    for (;;) {
        const char *kname;
        size_t kname_len = 0;
        bool name_decided = false;
        JSValue stored;
        int bad;

        if (f->phase == FILL_KEY_PAIR) {
            r = record_cursor_run(ctx, h, &f->rec, init, in, headers_record_key_ok, NULL, out_cb, out_argc);
            if (r > 0) return r;
            if (r < 0) return -1;
            in = JS_UNDEFINED;
            if (f->rec.done) return 0;
            JS_FreeValue(ctx, f->name);  f->name = JS_DupValue(ctx, f->rec.key);
            JS_FreeValue(ctx, f->value); f->value = JS_DupValue(ctx, f->rec.value);
            f->phase = FILL_VALUE_STR;
        }
        /* An unknown value is not converted: it stays the unknown and is stored as itself, so the entry and
           every read of it carry its provenance. */
        DCHECK(f->phase == FILL_VALUE_STR || f->phase == FILL_NAME_VALID,
               "the headers fill was re-entered at a phase it never parks in");
        if (f->phase == FILL_NAME_VALID) {
            /* a fork's re-entry: its `in` is the driver's filler, and the value is already converted */
        } else if (concolic_is(f->value)) {
            JS_FreeValue(ctx, in);
            in = JS_UNDEFINED;
        } else {
            /* Otherwise it is ByteString, so ToString on it is the page's code AGAIN — a third request per key. */
            JSValue s;
            r = step_tostring_run(ctx, h, f->value, in, &s, out_cb, out_argc);
            if (r > 0) return r;
            if (r < 0) return -1;
            in = JS_UNDEFINED;
            JS_FreeValue(ctx, f->value);
            f->value = s;
        }
        f->phase = FILL_NAME_VALID;
        JS_FreeValue(ctx, in);
        in = JS_UNDEFINED;
        /* §5.1 Headers class's validate a header, step 1's name half, over an unknown name (one the key's ByteString fork passed)
           has both completions, so it forks, the real one being the header-name test on the name's example.
           The arm that passes stores the name's display shape.
           Named residual. Not covered: the guard (validate steps 3-4, and append step 3 under
           "request-no-cors") over an unknown name, which reads the shape's bytes, so such a header is never
           forbidden and never no-CORS-safelisted. Next diff: a fork over the guard's verdict, real on the
           example. How its absence shows: no flow sends a request without a header whose name is unknown,
           and none sends one in "no-cors" mode with it. */
        if (concolic_is(f->name)) {
            int arm = 0;

            r = step_fork_run(ctx, h, f->name, HDR_NAME_OP, 2,
                              headers_example_real(ctx, f->name, header_name_is_valid), &arm);
            if (r) return r;
            DCHECKF(arm == 0 || arm == 1, "the Headers fill's header-name fork answered completion %d", arm);
            if (arm == 1) {
                JS_ThrowTypeError(ctx, "invalid header name");
                return -1;
            }
            name_decided = true;
            kname = concolic_name_cstr(ctx, f->name);
            kname_len = kname ? strlen(kname) : 0;
        } else {
            kname = JS_ToCStringLen(ctx, &kname_len, f->name);
        }
        if (!kname) return -1;
        /* §3.2.23 step 4.2.3's `typedValue = value converted to V`: the ToString is the request above and
           header_stored_value runs the range that completes it. Then §5.1: "for each key → value of object,
           append (key, value) to headers". Web IDL's record is keyed case-sensitively, so both entries of
           `{a:"1", A:"2"}` arrive and both append, and `get("a")` combines them into "1, 2". */
        stored = header_stored_value(ctx, kname, kname_len, f->value, name_decided);
        bad = JS_IsException(stored) || headers_append_one(ctx, out, guard, kname, stored) < 0;
        JS_FreeValue(ctx, stored);
        JS_FreeCString(ctx, kname);
        if (bad) return -1;   /* the TypeError is live */
        f->phase = FILL_KEY_PAIR;
    }
}

/* ---- the constructor ------------------------------------------------------------------------------------ */

/* WHERE THIS MACHINE RESTS. §5.1's constructor is two steps, and they split exactly where the page's code
   starts: step 1 is an own-slot write and step 2 is the whole fill, whose every [[Get]], @@iterator read and
   ToString is the page's. The `stage` byte this state carried named neither — and the comment beside it
   explained that hdr->stage was the ARGUMENT CURSOR, which stopped being true when idl_args.c handed the stage
   over to the body at IDL_STEP_FIRST. */
#define HDR_CTOR_STAGES(X) \
    X(HDR_CTOR_GUARD = IDL_STEP_FIRST, \
      "Fetch §5.1 new Headers(init) step 1 (this's guard is \"none\"; Web IDL §3.7.1's `new` requirement " \
      "precedes it)") \
    X(HDR_CTOR_FILL, "Fetch §5.1 new Headers(init) step 2 (fill this with init)")
enum { HDR_CTOR_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const HDR_CTOR_STEPS[] = { HDR_CTOR_STAGES(JS_STEP_STAGE_LABEL) NULL };

/* `list` is step 2's header list, the pairs the fill appends to: a declared value, so a deep fork's two arms
   hold the one list and each arm's appends are its own flow's writes. */
typedef struct { HeadersFill fill; JSValue list; JSValue result; } JSHeadersCtorState;

static void js_headers_ctor_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSHeadersCtorState *s = st;
    headers_fill_visit(ctx, &s->fill, v);
    v->val(ctx, &s->result);
    v->val(ctx, &s->list);
}

static int js_headers_ctor_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                                JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    JSHeadersCtorState *s = st;
    int r;

    if (hdr->stage == HDR_CTOR_GUARD) {
        /* JS_CFUNC_step_ctor delivers NEW_TARGET in the receiver slot and undefined for a plain call, which is
           how `Headers()` is told apart from `new Headers()` — the IDL declares a constructor, so it throws. */
        if (JS_IsUndefined(hdr->this_val)) {
            JS_FreeValue(ctx, cb_result);
            JS_ThrowTypeError(ctx, "constructor Headers requires 'new'");
            return -1;
        }
        headers_fill_init(&s->fill);
        s->result = JS_UNDEFINED;
        /* Step 2's list, built before the fill that appends to it. This stage hands over to the fill before
           anything can park, so it runs once per state. */
        s->list = header_pairs_new(ctx);
        hdr->stage = HDR_CTOR_FILL;
    }
    DCHECK(hdr->stage == HDR_CTOR_FILL,
           "the Headers constructor was re-entered at a stage §5.1 does not have");
    /* §5.1: `new Headers(init)` sets the guard to "none" and then fills — a page's own Headers refuses
       nothing, which is why the forbidden lists are unobservable until a Request or a Response owns one. */
    r = headers_fill_run(ctx, hdr, &s->fill, argc > 0 ? argv[0] : JS_UNDEFINED, s->list,
                         HEADERS_GUARD_NONE, cb_result, out_cb, out_argc);
    if (r > 0) return r;
    if (r < 0) return -1;
    *presult = headers_new_pairs(ctx, s->list, HEADERS_GUARD_NONE);
    return JS_IsException(*presult) ? -1 : 0;
}

static const IdlStepDecl js_headers_ctor_decl = {
    /* No `release`: everything this state owns is declared through `visit`. */
    js_headers_ctor_step, sizeof(JSHeadersCtorState), js_headers_ctor_visit, NULL,
    "Fetch §5.1 new Headers(init)", HDR_CTOR_STEPS,
    /* `catches_abrupt` = 0: a throwing header key or value is the page's to see at its `new Headers`. */
    0
};

/* ---- install --------------------------------------------------------------------------------------------- */

void headers_init(JSContext *ctx)
{
    JSClassDef def = { "Headers", .record = &HEADERS_LAYOUT };
    JSRuntime *rt = JS_GetRuntime(ctx);
    /* Fetch §5.1 Headers class writes `ByteString` at every one of these positions, and the type is the whole
       of where §3.2.11's range refusal happens — see the note above header_normalize_value. It was
       IDL_DOMSTRING, which converts and does NOT refuse, so the refusal fell to the body: `append` then ran
       the value's ToString — the page's own code — for a name §3.2.11 had already made a TypeError. */
    static const IdlArgType TWO_BYTESTR[2] = { IDL_BYTESTRING, IDL_BYTESTRING };
    static const IdlArgType ONE_ANY[1] = { IDL_ANY };   /* HeadersInit: a union the fill converts, not the machine */

    DCHECK(g_headers_rt == NULL || g_headers_rt == rt,
           "Headers was installed into a second runtime — its class id and step ids belong to the first, and one "
           "WASM instance is one document");
    if (g_headers_rt == rt)
        return;
    g_headers_rt = rt;
    JS_NewClassID(rt, &g_headers_class);
    JS_NewClass(rt, g_headers_class, &def);
    g_id[HDR_APPEND]       = idl_method_id(ctx, TWO_BYTESTR, 2, js_headers_member, HDR_APPEND);
    g_id[HDR_SET]          = idl_method_id(ctx, TWO_BYTESTR, 2, js_headers_member, HDR_SET);
    g_id[HDR_DELETE]       = idl_method_id(ctx, TWO_BYTESTR, 1, js_headers_member, HDR_DELETE);
    g_id[HDR_GET]          = idl_method_id(ctx, TWO_BYTESTR, 1, js_headers_member, HDR_GET);
    g_id[HDR_HAS]          = idl_method_id(ctx, TWO_BYTESTR, 1, js_headers_member, HDR_HAS);
    g_id[HDR_GETSETCOOKIE] = idl_method_id(ctx, TWO_BYTESTR, 0, js_headers_member, HDR_GETSETCOOKIE);
    g_ctor_stepid = idl_method_id_step(ctx, ONE_ANY, 1, NULL, 0, &js_headers_ctor_decl, 0);
    idl_optional_from(0);   /* §5.1: `constructor(optional HeadersInit init)` */

    /* §5.1's `iterable<ByteString, ByteString>` — the shared default iterator object over the two operations
       above, so the six members it defines exist once for every such interface rather than once per. */
    g_pair_handle = idl_pair_iter_declare(ctx, "headers", &HEADERS_PAIR_OPS);
    realm_declare_intrinsic(headers_install_proto);

    /* EVERY STATIC ABOVE IS THIS AGENT'S, DECLARED BESIDE THE LINE THAT SETS IT (core/agent_state.h). This
       component held ALL of them past its own release: `headers_free` reset the runtime latch and the
       constructor's id and left the CLASS ID, the six member declarations and §5.1's pair-iterator handle
       exactly as this function had set them. A carried class id is not the cautious half of a tie — the next
       agent's runtime restarts JS_NewClassID at JS_CLASS_INIT_COUNT, so the number names a class in a runtime
       that is gone, and the latch above returns before re-registering it. */
    {
        int i;

        agent_state_ptr("headers", &g_headers_rt, "the runtime §5.1's class and machines were declared in");
        agent_state_class("headers", &g_headers_class, "Fetch §5.1 Headers class's class");
        for (i = 0; i < HDR_MEMBER_N; i++)
            agent_state_id("headers", &g_id[i], "one of §5.1's six member declarations");
        agent_state_id("headers", &g_ctor_stepid, "§5.1's `constructor(optional HeadersInit init)` machine");
        agent_state_id("headers", &g_pair_handle,
                       "§5.1's `iterable<ByteString, ByteString>` default-iterator handle");
    }
}

/* FETCH §5.1 "Headers class"' INTERFACE PROTOTYPE OBJECT *AND* ITS INTERFACE OBJECT, FOR ONE REALM.
   §5.1 declares `[Exposed=(Window,Worker)] interface Headers`, and Web IDL §3.8 "Platform objects implementing
   interfaces"' `define the global property references` is "To define the global property references on target,
   given realm realm" whose step 1 is "Let interfaces be a list that contains every interface that is exposed
   in realm" — a REALM, with no Document anywhere in the algorithm. The interface object used to be placed from
   core/platform.c's per-DOCUMENT column through a `headers_install`, so a realm that reaches no
   platform_document_install got no `Headers` at all — a worker realm always, and a Window realm until a
   Document was installed over it. It is minted here instead, beside the prototype this function has already
   built, which is also what removes the JS_GetClassProto re-read that entry made: a second answer to a
   question settled four lines up. */
void headers_install_proto(JSContext *ctx)
{
    JSValue proto, prev, ctor, global;

    DCHECK(g_headers_class != 0, "a realm asked for Headers.prototype before the class was declared");
    DCHECK(g_ctor_stepid >= 0, "a realm asked for Headers before headers_init declared its constructor");
    prev = JS_GetClassProto(ctx, g_headers_class);
    DCHECK(JS_IsNull(prev), "headers_install_proto ran twice in one realm");
    JS_FreeValue(ctx, prev);

    proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(proto), "Headers.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "Headers");
    idl_install_method(ctx, proto, "append", g_id[HDR_APPEND]);
    idl_install_method(ctx, proto, "set", g_id[HDR_SET]);
    idl_install_method(ctx, proto, "delete", g_id[HDR_DELETE]);
    idl_install_method(ctx, proto, "get", g_id[HDR_GET]);
    idl_install_method(ctx, proto, "has", g_id[HDR_HAS]);
    idl_install_method(ctx, proto, "getSetCookie", g_id[HDR_GETSETCOOKIE]);
    idl_pair_iter_install(ctx, proto, g_pair_handle);

    ctor = idl_step_constructor(ctx, "Headers", g_ctor_stepid);
    CHECK(!JS_IsException(ctor), "the Headers interface object could not be allocated");
    JS_SetConstructor(ctx, ctor, proto);
    /* THE HANDOVER IS LAST: JS_SetClassProto TAKES the reference, so `proto` is this function's until the realm
       owns it, and the Web IDL §3.7.1 Interface object pairing above reads a local rather than a class slot it has
       given away. */
    JS_SetClassProto(ctx, g_headers_class, proto);
    global = JS_GetGlobalObject(ctx);
    idl_define_global_property_reference(ctx, global, "Headers", ctor);
    JS_FreeValue(ctx, global);
}

/* The prototype and the interned name are this component's for the runtime's life, so they are released WITH
   it. Without this the prototype is a GC object nobody drops and JS_FreeRuntime's gc_obj_list walk reports it —
   which is exactly how it was found, on the first run of this file. */
/* FETCH §5.1's AGENT-LIFETIME STATE, GIVEN BACK — AND IT IS ON core/platform.h's RELEASE COLUMN NOW RATHER
   THAN BEING A LINE IN THREE HOST TEARDOWNS. engine/host/main.c, engine/host/test_forced.c and
   engine/host/wpt_runner.c each called this by hand AFTER platform_agent_free had already run that whole
   column, so out there NONE of this component's state could be declared to core/agent_state.h at all — a row
   with agent state and no release is what platform_check_agent_state fires on, and a release that runs after
   agent_state_check_released is not on the column in the sense that check means. The class id, the six member
   declarations and the pair handle were therefore carried past the release by every host, and §5.1's
   finalizer read the class id the release had not reset.
   IT TAKES NO JSContext ANY MORE and it never read the one it took: the prototype and the interface object
   are the REALMS', released with their contexts, so what is left here is the AGENT's, which is this column's
   entry condition. The parameter was the last thing making this look like a per-realm component in the wrong
   column — the same reading that moved §4's streams group and DOM §3.1/§3.2's pair.
   THE UNDO IS THE ONE RESET AND IT IS LAST. Two hand-written lines stood here, kept in step with an init
   three lines above by whoever remembered, and they were already short by five slots; a declaration added to
   that init now owes this function nothing. */
void headers_free(void)
{
    if (!g_headers_rt)
        return;
    /* the prototypes are the REALMS' — released with their contexts, so this component owns no reference and
       there is nothing to free here. Free, assert, then undo. */
    /* §5.1'S ITERATOR CLASS IS DECLARED UNDER THIS ROW FROM core/idl_iter.c, so the undo below REFUSES to put
       it back until that file has said the cascade reached it. The claim cannot be made here: it is matched by
       the file the macro is expanded in, and this one declares no slot of core/idl_iter.c's. Before the undo,
       which is also before this row's own `g_pair_handle` goes back to -1. */
    idl_pair_iter_release(g_pair_handle);
    agent_state_undo("headers");
}


