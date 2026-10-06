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
    char *r = malloc(n + 1);
    CHECK(r, "headers: OOM copying a header name — a dropped header loses what the endpoint requires");
    for (i = 0; i < n; i++)
        r[i] = (s[i] >= 'A' && s[i] <= 'Z') ? (char)(s[i] - 'A' + 'a') : s[i];
    r[n] = 0;
    return r;
}

static char *header_dup(const char *s)
{
    char *r = strdup(s ? s : "");
    CHECK(r, "headers: OOM copying a header value");
    return r;
}

void header_list_free(HeaderList *l)
{
    int i;
    if (!l) return;
    for (i = 0; i < l->n; i++) { free(l->e[i].name); free(l->e[i].value); }
    free(l->e);
    l->e = NULL; l->n = l->cap = 0;
}

void header_list_append(HeaderList *l, const char *name, const char *value)
{
    DCHECK(l != NULL && name != NULL, "a header was appended to no list");
    if (l->n >= l->cap) {
        l->cap = l->cap ? l->cap * 2 : 8;
        l->e = realloc(l->e, (size_t)l->cap * sizeof(HeaderEntry));
        CHECK(l->e, "headers: OOM growing a header list");
    }
    l->e[l->n].name = header_lower(name);
    l->e[l->n].value = header_dup(value);
    l->n++;
}

void header_list_delete(HeaderList *l, const char *name)
{
    char *lo = header_lower(name);
    int i, w = 0;
    for (i = 0; i < l->n; i++) {
        if (!strcmp(l->e[i].name, lo)) { free(l->e[i].name); free(l->e[i].value); continue; }
        l->e[w++] = l->e[i];
    }
    l->n = w;
    free(lo);
}

void header_list_set(HeaderList *l, const char *name, const char *value)
{
    header_list_delete(l, name);
    header_list_append(l, name, value);
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
    out = malloc(n);
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
        name = malloc((size_t)(colon - p) + 1);
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
            char *value = malloc((size_t)(end - vs) + 1);
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
    out = malloc(total + 1);
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

/* ---- the interface ------------------------------------------------------------------------------------ */

/* §5.1: "A Headers object has an associated GUARD." It is the object's, not the list's — the same header list
   is reachable through a Response's immutable Headers and through the fetch machinery that built it, and only
   the first refuses writes. So the class opaque is the pair. */
typedef struct { HeaderList list; uint8_t guard; } HeadersObj;

/* THE COLLECTOR RUNS AFTER THE RELEASE COLUMN, so this may not reach the record through an id its own
   release has already given back — core/agent_state.h's closing paragraph, and the reason every one of
   §5.1's slots below is declared. The class is a fact the collector ALREADY HAS: it dispatched to this
   function through it, so `JS_GetOpaque(val, g_headers_class)` asks a question whose answer is `0` by the
   time it is asked, and `JS_GetOpaque` against class 0 answers NULL for every live Headers — the whole list
   leaked, silently, on any page that built one. */
static void headers_finalizer(JSRuntime *rt, JSValue val)
{
    JSClassID id;
    HeadersObj *h = JS_GetAnyOpaque(val, &id);
    (void)rt;
    if (h) { header_list_free(&h->list); free(h); }
}

const HeaderList *headers_list_of(JSValueConst v)
{
    HeadersObj *h = JS_GetOpaque(v, g_headers_class);
    return h ? &h->list : NULL;
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

JSValue headers_new(JSContext *ctx, const HeaderList *src, HeadersGuard guard)
{
    HeadersObj *h;
    JSValue obj;
    int i;

    DCHECK(g_headers_class != 0, "a Headers was built before the class existed — headers_init runs at install");
    {
        JSValue proto = JS_GetClassProto(ctx, g_headers_class);
        DCHECK(!JS_IsNull(proto), "a Headers was minted in a realm that never ran its install");
        obj = JS_NewObjectProtoClass(ctx, proto, g_headers_class);
        JS_FreeValue(ctx, proto);
    }
    if (JS_IsException(obj))
        return obj;
    h = calloc(1, sizeof *h);
    CHECK(h, "headers: OOM building a Headers");
    h->guard = (uint8_t)guard;
    /* The list is copied ALREADY VALIDATED — it is a header list this engine built, never a page's. The guard
       governs what the PAGE may then do to it, which is why "immutable" does not block this loop. */
    for (i = 0; src && i < src->n; i++)
        header_list_append(&h->list, src->e[i].name, src->e[i].value);
    JS_SetOpaque(obj, h);
    return obj;
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
    out = malloc(n + 1);
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

/* §5.1's guard as ONE operation, because every entry point performs the same one: normalize the value, then
   reject a bad name or a bad value with a TypeError. `*pnorm` is the normalized value (caller frees), left NULL
   for the name-only members. 0 on success, -1 with a TypeError live. */
static int header_check(JSContext *ctx, const char *name, size_t name_len,
                       const char *value, size_t value_len, char **pnorm)
{
    char *norm;
    size_t norm_len;
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

/* §5.1 "remove privileged no-CORS request-headers": every append, set and delete under the "request-no-cors"
   guard ends with it — "This is called when headers are modified by unprivileged code", and §2.2.2 Headers'
   own note beside the privileged list says such headers "will be preserved if their associated request object
   is copied, but will be removed if the request is modified by unprivileged APIs". */
static void header_remove_privileged_no_cors(HeaderList *l)
{
    size_t i;
    for (i = 0; i < sizeof(HEADER_PRIVILEGED_NO_CORS) / sizeof(HEADER_PRIVILEGED_NO_CORS[0]); i++)
        header_list_delete(l, HEADER_PRIVILEGED_NO_CORS[i]);
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

/* §5.1 "APPEND A HEADER (name, value) TO A HEADERS OBJECT", steps 2-5, over the LIST and the GUARD — which is
   all the algorithm reads. ONE function because the standard states it once and TWO callers run it: the
   `append` member, and the FILL, which the standard defines as "append (key, value) to headers" for every
   entry of the init and NOT as a list write of its own. The fill ran step 2 and then wrote the list itself,
   so it skipped step 3 — `new Request(u, {mode:"no-cors", headers:{"X-Custom":"1"}})` kept a header every
   browser drops — and its record arm REPLACED a same-named entry, so `new Headers({a:"1", A:"2"}).get("a")`
   answered "2" where appending twice and combining gives "1, 2".
   `norm` is the already-normalized value (step 1). Returns 0 for written OR silently refused — two outcomes
   the spec does not let a caller distinguish — and -1 with a TypeError live. */
static int headers_append_one(JSContext *ctx, HeaderList *l, uint8_t guard, const char *name, const char *norm)
{
    char *lower = header_lower(name);
    int allow = headers_guard_allows(ctx, guard, name, norm);   /* step 2 */

    /* step 3: under "request-no-cors" the header must be no-CORS-safelisted with the value it would END UP
       with — appending a second 127-byte `accept` makes 255 bytes, which is not safelisted, so the append
       does nothing. That is why this joins and `set` below does not. */
    if (allow > 0 && guard == HEADERS_GUARD_REQUEST_NO_CORS) {
        char *existing = header_list_get(l, lower), *combined = NULL;
        const char *test = norm;
        if (existing) {
            size_t a = strlen(existing), b = strlen(norm);
            combined = malloc(a + b + 3);
            CHECK(combined, "headers: OOM joining a no-cors value");
            memcpy(combined, existing, a);
            combined[a] = ','; combined[a + 1] = ' ';
            memcpy(combined + a + 2, norm, b);
            combined[a + b + 2] = 0;
            test = combined;
        }
        free(existing);
        if (!header_is_no_cors_safelisted(lower, test)) allow = 0;
        free(combined);
    }
    if (allow > 0) {
        header_list_append(l, name, norm);                                          /* step 4 */
        if (guard == HEADERS_GUARD_REQUEST_NO_CORS) header_remove_privileged_no_cors(l);   /* step 5 */
    }
    free(lower);
    return allow < 0 ? -1 : 0;
}

/* …AND THE SAME ALGORITHM FOR A CALLER THAT HOLDS A LIST AND A GUARD RATHER THAN A `Headers` OBJECT — Fetch
   §5.4 Request class' new Request(input, init) step 33's "If headers is a Headers object, then for each header
   of its header list, APPEND header to this's headers" (step 34 stood here; 34 is "Let inputBody be input's
   request's body …", and 33 is the one that holds this sub-step). That arm is a §5.1 append per entry, under
   the NEW request's guard, and it is a different algorithm from headers_fill_run: fill resolves §3.2.25's
   union through the value's own @@iterator, which step 33 must not do for a header list the constructor is
   carrying forward. Exported rather than re-derived at the caller, because a second copy of "which headers
   this guard drops" is a second thing to keep in step with §2.2.2 Headers' two forbidden lists. */
int header_list_append_guarded(JSContext *ctx, HeaderList *l, HeadersGuard guard,
                               const char *name, const char *value)
{
    return headers_append_one(ctx, l, (uint8_t)guard, name, value);
}

/* ---- a header list a STEP MACHINE owns ------------------------------------------------------------------- */

/* THE ROOT. `ctx` is taken and unused on purpose: it is the allocator argument every other visit operation's
   copy carries, and taking it here says that this one does NOT use it — the entries stay on the C library's
   allocator, which is the whole reason the copy is delegated rather than performed by the engine. */
HeaderList *header_list_step_new(JSContext *ctx)
{
    HeaderList *l = calloc(1, sizeof(*l));
    (void)ctx;
    CHECK(l, "headers: OOM allocating the header list a step machine owns");
    return l;
}

/* THE COPY. Deep, through §5.1's own append, so the copy is built by the one function that builds every header
   list in this engine — a second spelling of "copy a pair" is the shape that drifts from the append that made
   it. Append LOWERCASES the name, which is idempotent over a list whose names arrived through append, so the
   copy is byte-for-byte the same list and not a re-normalised one.
   FATAL ON ALLOCATION FAILURE and nothing here decides that: `calloc`'s CHECK below, and append's own
   `CHECK(l->e, …)`, `header_lower`'s and `header_dup`'s, are each fatal in dev AND release. That is the clause
   JSStepTreeOps::clone states as the one a host has to MEET, and this list met it before it was asked. */
static void *header_list_step_clone(JSContext *ctx, void *root, void **cursors[], int ncursors)
{
    const HeaderList *src = root;
    HeaderList *cp;
    int i;

    (void)ctx; (void)cursors;
    /* NO CURSOR STANDS IN A HEADER LIST, and that is a fact about the holders rather than an assumption: the
       §5.1 fill cursor every one of them embeds (HeadersFill) takes the list as an ARGUMENT of
       headers_fill_run per call and stores no pointer to it, so there is nothing inside this structure for an
       arm to be left naming. A holder that grows one declares it and this stops compiling. */
    DCHECK(ncursors == 0,
           "a header list was cloned for a fork with cursors declared standing inside it — nothing holds a "
           "pointer INTO a header list, so a count above zero is a holder whose interior pointers this copy "
           "does not re-point and would leave aimed at the original arm's entries");
    cp = calloc(1, sizeof(*cp));
    CHECK(cp, "headers: OOM copying the header list a forked step machine owns");
    for (i = 0; i < src->n; i++)
        header_list_append(cp, src->e[i].name, src->e[i].value);
    /* BOTH OPERANDS NAMED, and it can fail: append does not dedupe and must not start, because §5.1 keeps
       `Set-Cookie` pairs separate and `get` is what combines them. A copy shorter than its source is a list
       whose entries two arms would disagree about. */
    DCHECKF(cp->n == src->n,
            "a header list's fork copy holds %d entries where the original holds %d — §5.1's append keeps "
            "every pair, so a copy that lost one was built by something that combines or drops names",
            cp->n, src->n);
    return cp;
}

/* THE DESTROY. The entries and then the root, because the root IS the owned thing — JSStepTreeOps::destroy
   says "everything it owns, including `root` itself", and a destroy that freed only the entries would leak one
   `HeaderList` per forked arm with nothing able to name it. */
static void header_list_step_destroy(JSContext *ctx, void *root)
{
    (void)ctx;
    header_list_free(root);
    free(root);
}

const JSStepTreeOps header_list_step_ops = { header_list_step_clone, header_list_step_destroy };

enum { HDR_APPEND = 0, HDR_SET, HDR_DELETE, HDR_GET, HDR_HAS, HDR_GETSETCOOKIE, HDR_MEMBER_N };
/* THE AGENT'S POOL ENTRIES, one per §5.1 Headers class operation — the OBJECTS they are installed as are each realm's. */
static int g_id[HDR_MEMBER_N];

static JSValue js_headers_member(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    HeadersObj *h = headers_of(ctx, this_val);
    HeaderList *l = h ? &h->list : NULL;
    const char *name = NULL, *value = NULL;
    size_t name_len = 0, value_len = 0;
    char *norm = NULL;
    JSValue r = JS_UNDEFINED;

    if (!h)
        return JS_EXCEPTION;
    if (magic == HDR_GETSETCOOKIE) {
        /* §5.1 getSetCookie(): every `set-cookie` value, each on its own — the one member for which the list's
           repeats are the answer rather than something `get` folds away. */
        JSValue arr = JS_NewArray(ctx);
        uint32_t k = 0;
        int i;
        if (JS_IsException(arr)) return arr;
        for (i = 0; i < l->n; i++)
            if (!strcmp(l->e[i].name, "set-cookie"))
                JS_SetPropertyUint32(ctx, arr, k++, JS_NewString(ctx, l->e[i].value));
        return arr;
    }
    DCHECK(argc >= 1, "a Headers member was declared with fewer arguments than its IDL lists");
    /* §5.1's TWO READ MEMBERS OVER AN UNKNOWN NAME, WHICH IS WHERE THE FLOW USED TO END. Both are two steps
       in the fetched standard's own words — `get`: "If name is not a header name, then throw a TypeError",
       then "Return the result of getting name from this's header list"; `has`: the same step 1, then "Return
       true if this's header list contains name; otherwise false" — and core/idl_args.h's
       `idl_concolic_rule` answers IDL_CONCOLIC_CROSSES for the IDL_BYTESTRING at position 0, so the
       declaration hands an unknown to this body AS ITSELF and the coercion below ABORTS at the C boundary.
       `h.get(computedName)` therefore ended the document, which is an ordinary thing for a bundle to write.
       A DERIVATION AND NOT A PROJECTION, WHICH IS THE WHOLE REASON THIS IS NOT THE `value` POSITION'S FIX ONE
       ARGUMENT OVER. The residual below states it and it is provable rather than preferred: solver/concolic.c
       composes every shape out of `{`, `}` and a derivation's brackets, and §2.2.2 Headers' name is RFC 9110
       §5.1 Field Names' `field-name = token`, whose tchar set (`header_name_is_valid` above: alnum plus
       the sixteen punctuation marks it lists) admits NONE of those bytes — so a projected name fails step 1
       for EVERY unknown and the TypeError would be decided by the SOLVER's value class rather than by the
       page's value, which is the collapse the pass-through in idl_args.c exists to prevent.
       THE PRECEDENT IS core/url/url.c's `URL.parse`/`URL.canParse` AND IT IS THE SAME SHAPE FOR THE SAME
       REASON: a validity gate a page branches on must FORK rather than die, so the member answers a value
       that is opaque for control flow and carries the REAL verdict as its example. `if (h.has(n))` then forks
       both arms — which is what reaches the gated code — and the arm the real name took keeps the answer
       this engine actually computed.
       THE EXAMPLE IS COMPUTED BY THE ONE LOOKUP AND IS NEVER INVENTED, which is §@H's rule:
       `header_list_get` is the same door both concrete arms below take, run here over the name's OWN
       example, so a member answering a known name and a member answering an unknown one's example cannot
       disagree. Where there is no example the derivation carries JS_UNDEFINED, because @H reports an absence
       rather than fabricating a miss — and a `false` from `has` or a `null` from `get` would be exactly
       such a fabrication, since those are real answers a page branches on.
       AN EXAMPLE THAT IS NOT A HEADER NAME HAS NO VALUE EITHER, and that is step 1 rather than a gap: for
       such a name §5.1 THROWS, and a throw is not a value this member returns, so there is nothing to carry
       as the example. The derivation is still the answer — the name is unknown, so whether THIS flow's name
       is a token is open — and the example is simply absent.
       NAMED RESIDUAL — THE THROW ARM IS NOT EXPLORED. NOT COVERED: the world in which the unknown name is
       not a token, where §5.1 step 1 throws a TypeError into the page's own `try`. WHAT THE NEXT DIFF
       BUILDS: that fork belongs to the BOUNDARY and not to this body, for the reason
       core/dom/dom_token_list.c's `toggle` residual gives about its own boolean — an ask performed here
       would fork a value forty-odd members share the seam for, and §C-stack forbids a builtin forking on
       its own operand — so what is owed is a declared PREDICATE position in core/idl_args.h whose rule is
       IDL_CONCOLIC_FORKS over `header_name_is_valid`, asked once at the branch seam and filed under that
       predicate's own identity. HOW ITS ABSENCE WOULD SHOW: a flow whose `h.get(n)` sits inside a `try`
       reaches the `catch` in no world, so a bundle that recovers from a bad header name has its recovery
       path unexplored, while the same flow's success path now runs to completion. */
    if (concolic_is(argv[0]) && (magic == HDR_GET || magic == HDR_HAS)) {
        JSValue ex = concolic_example(ctx, argv[0]);
        JSValue real = JS_UNDEFINED;

        if (JS_IsString(ex)) {
            const char *exn = JS_ToCString(ctx, ex);

            CHECK(exn != NULL,
                  "§5.1's read could not read the String its own example holds — the example is a concrete "
                  "value this engine minted, and a String it cannot read back is a heap it has already lost, "
                  "which is why this is fatal in release too");
            if (header_name_is_valid(exn, strlen(exn))) {
                char *v = header_list_get(l, exn);

                real = (magic == HDR_HAS) ? JS_NewBool(ctx, v != NULL)
                                          : (v ? JS_NewString(ctx, v) : JS_NULL);
                free(v);
            }
            JS_FreeCString(ctx, exn);
        }
        JS_FreeValue(ctx, ex);
        /* ONE OPERAND, WHICH IS THE DECLARATION'S NUMBER: §5.1 declares `get(ByteString name)` and
           `has(ByteString name)`, so a shape naming a second would render an expression this member's steps
           never performed — url.c's `url_operand_count` makes the same point about its own two. */
        return concolic_new_derived(ctx, magic == HDR_HAS ? "Headers.has" : "Headers.get", argv, 1, real);
    }
    /* NAMED RESIDUAL — THE `name` POSITION OF THE THREE *WRITE* MEMBERS IS NOT COVERED, and the coercion below
       ends the flow for one, AND ITS READ HALF IS BUILT — THE RETIRED WORDING IS KEPT BELOW THE VERDICT
       BECAUSE A READER WHO RE-DERIVES THE SPLIT FROM "§5.1 DECLARES ONE `name` TYPE AT POSITION 0 OF ALL FIVE
       MEMBERS" WILL WRITE IT AGAIN, which is exactly the inference that made this ONE residual for a while.
       It read: "THE `name` POSITION OVER UNKNOWN EXTERNAL INPUT IS NOT COVERED … WHAT THE NEXT DIFF BUILDS:
       the two READ members answer a DERIVED unknown over the name operand — solver/concolic.h's
       `concolic_new_derived` with the real lookup over the name's own example as the example, which is what
       core/url/url.c already does for `URL.canParse` and for the same reason (a validity gate the page
       branches on must fork rather than die); the three WRITE members need a header list key space that
       admits an unknown name". THE READ CLAUSE IS MET by the arm above this comment — and it was met
       EXACTLY as written, which is the one thing worth recording about it, because §AND-THE-"WHAT-THE-NEXT-
       DIFF-BUILDS"-CLAUSE rates such a clause a HYPOTHESIS rather than evidence and this file has already
       had one of its own refuted. What made it hold was that it named a MECHANISM THAT WAS GREPPED (`url.c`'s
       answer, confirmed present) rather than one its author pictured.
       WHAT IS STILL NOT COVERED IS THE THREE WRITES — `append`, `set` and `delete` — whose `name` position
       reaches the coercion below and ends the flow. §5.1 declares `ByteString name` at position 0 of all
       five members and core/idl_args.h's `idl_concolic_rule` answers IDL_CONCOLIC_CROSSES for
       IDL_BYTESTRING, so an unknown is placed as itself and this JS_ToCStringLen aborts at the C boundary —
       js_force_tostring's own DFAIL says why, "a `const char *` cannot carry a concolic".
       PROJECTING THE SHAPE IS STILL THE WRONG ANSWER AND IS STILL PROVABLY SO, which is why the writes did
       not come with the reads. solver/concolic.c composes every shape out of `{`, `}` and the bracketing of a
       derivation (`{location.hash}`, `{x}[{y}]`, `String({x})`), and §2.2.2 Headers' header name is RFC 9110
       §5.1 Field Names' `field-name = token`, whose tchar set admits none of those characters — so a
       projected name fails header_name_is_valid for EVERY unknown, and the TypeError that follows would be
       decided by the SOLVER's own value class rather than by the page's value, which is the collapse the
       pass-through in idl_args.c exists to prevent.
       WHY THE READS COULD BE ANSWERED AND THE WRITES CANNOT, WHICH IS THE WHOLE OF THE SPLIT AND IS NOT A
       MATTER OF EFFORT: a read ANSWERS a question and touches nothing, so a value that is opaque for control
       flow and carries the real verdict is a complete answer to it. A write MUTATES A KEYED STORE, and
       `header_list_set` and `header_list_delete` key on BYTES — a name that is not a token has none to key
       on, and a name that is unknown has no bytes this engine may choose for it without deciding the very
       thing the page left open.
       WHAT THE NEXT DIFF BUILDS: a header list key space that admits an unknown name — an entry whose name
       is a VALUE rather than a `char *`, so `set` replaces and `delete` removes the entry that names THIS
       source rather than the entry whose bytes happen to match, with the concrete path keying on bytes
       exactly as it does now. The read arm above then reads it through the same door, since a lookup over an
       unknown name must be able to find an entry an unknown name wrote.
       HOW ITS ABSENCE WOULD SHOW: a flow that WRITES a header whose name it computed ends at the line below
       with js_force_tostring's `@WHY` naming this file and this coercion, while the same flow READING one
       now runs to completion and the same flow setting a computed VALUE under a literal name always did. */
    name = JS_ToCStringLen(ctx, &name_len, argv[0]);
    if (!name) return JS_EXCEPTION;
    if (magic == HDR_APPEND || magic == HDR_SET) {
        /* THE `value` POSITION OVER UNKNOWN EXTERNAL INPUT, WHICH IS THE ONE A REAL BUNDLE PASSES. §5.1
           declares `ByteString value` and idl_concolic_rule answers IDL_CONCOLIC_CROSSES for it, so the
           declaration hands an unknown to this body AS ITSELF and a raw JS_ToCStringLen on one ABORTS — which
           ended the document at `headers.set('Authorization', 'Bearer ' + token)`, the single commonest way a
           bundle puts external input into a request, and at every `h.append(n, v)` beside it.
           THE ANSWER IS THE ONE THIS FILE ALREADY GIVES, AND THAT IS WHY IT IS A PROJECTION RATHER THAN A
           DERIVATION: the fill's record arm — `new Headers({'Authorization': 'Bearer ' + token})` and every
           `fetch(u, {headers: {…}})` through it — projects the value's SHAPE and says in its own words that
           coercing it "would either abort at the ToString boundary or, worse, quietly de-taint it into some
           concrete-looking string". A member answering differently would make `h.set(n, v)` and
           `new Headers({n: v})` store different bytes for one program, which is the two-answers-to-one-question
           shape; concolic_name_cstr composes the same bytes the record arm's JS_NewString(concolic_shape_c)
           does, so no header list content moves.
           THE LENGTH IS strlen AND THAT IS NOT AN ASSUMPTION: the shape is a C string this engine composed,
           so it holds no U+0000 — which is the one thing a header value may not contain and the reason every
           other read here is length-delimited. core/dom/dom_token_list.c's token_bytes answers the identical
           question the identical way one component over.
           §3.2.11's RANGE IS OWED HERE BECAUSE NOTHING ELSE RAN IT: the pass-through placed the value before
           any conversion, so the declared ByteString's step 2 never happened, and header_check below ASSERTS
           that it did. The fill's concolic arm takes the same refusal for the same stated reason — a header
           the engine cannot state as bytes is not one it may report. OWNED either way: JS_FreeCString, which
           is what every exit below already does with it. */
        if (concolic_is(argv[1])) {
            value = concolic_name_cstr(ctx, argv[1]);
            if (!value) { JS_FreeCString(ctx, name); return JS_EXCEPTION; }
            value_len = strlen(value);
            if (headers_bytestring(ctx, value, value_len, "value") < 0) {
                JS_FreeCString(ctx, name);
                JS_FreeCString(ctx, value);
                return JS_EXCEPTION;
            }
        } else {
            value = JS_ToCStringLen(ctx, &value_len, argv[1]);
            if (!value) { JS_FreeCString(ctx, name); return JS_EXCEPTION; }
        }
    }
    /* §5.1's guard, on EVERY member and not just the two that write. `headers.get({})` reads a name of
       "[object Object]", which is not a token, and the spec makes that a TypeError rather than a miss — wpt
       asserts it by name for get, has, delete and set alike. */
    if (header_check(ctx, name, name_len, value, value_len, value ? &norm : NULL) < 0) {
        JS_FreeCString(ctx, name);
        if (value) JS_FreeCString(ctx, value);
        return JS_EXCEPTION;
    }
    /* §5.1's three WRITES each run their own steps; a read is not guarded at all, because the page can
       already see every one of these headers and refusing to answer would hide state rather than protect it. */
    switch (magic) {
    case HDR_APPEND:
        if (headers_append_one(ctx, l, h->guard, name, norm) < 0) {
            JS_FreeCString(ctx, name);
            JS_FreeCString(ctx, value);
            free(norm);
            return JS_EXCEPTION;
        }
        break;
    case HDR_SET: {
        /* §5.1 set steps 2-5. Step 3 tests the value ON ITS OWN, because set REPLACES rather than joins. */
        char *lower = header_lower(name);
        int allow = headers_guard_allows(ctx, h->guard, name, norm);
        if (allow > 0 && h->guard == HEADERS_GUARD_REQUEST_NO_CORS &&
            !header_is_no_cors_safelisted(lower, norm))
            allow = 0;
        if (allow > 0) {
            header_list_set(l, name, norm);
            if (h->guard == HEADERS_GUARD_REQUEST_NO_CORS) header_remove_privileged_no_cors(l);
        }
        free(lower);
        if (allow < 0) {
            JS_FreeCString(ctx, name);
            JS_FreeCString(ctx, value);
            free(norm);
            return JS_EXCEPTION;
        }
        break;
    }
    case HDR_DELETE: {
        /* §5.1 delete steps 1-5, and step 2 IS NOT WHAT STOOD HERE. It reads: "If this's guard is
           'request-no-cors', name is not a no-CORS-safelisted request-header name, AND name is not a
           privileged no-CORS request-header name, then return." A privileged name is one of the two that get
           THROUGH — §2.2.2 Headers' note beside the privileged list says such headers "will be removed if
           the request is modified by unprivileged APIs", so `delete("Range")` is exactly the call that must
           succeed. This refused that one call and let every OTHER unsafelisted name through, which is both
           halves inverted.
           Step 1 validates with the DUMMY value ``, which is why the method-override names are deletable. */
        char *lower = header_lower(name);
        int allow = headers_guard_allows(ctx, h->guard, name, "");
        if (allow > 0 && h->guard == HEADERS_GUARD_REQUEST_NO_CORS &&
            !header_is_no_cors_safelisted_name(lower) && !header_is_privileged_no_cors(lower))
            allow = 0;
        if (allow > 0) {
            header_list_delete(l, name);
            if (h->guard == HEADERS_GUARD_REQUEST_NO_CORS) header_remove_privileged_no_cors(l);
        }
        free(lower);
        if (allow < 0) {
            JS_FreeCString(ctx, name);
            JS_FreeCString(ctx, value);
            free(norm);
            return JS_EXCEPTION;
        }
        break;
    }
    case HDR_HAS: {
        char *v = header_list_get(l, name);
        r = JS_NewBool(ctx, v != NULL);
        free(v);
        break;
    }
    default: {
        char *v;
        DCHECK(magic == HDR_GET, "a Headers member was declared with a magic this component does not answer");
        v = header_list_get(l, name);
        r = v ? JS_NewString(ctx, v) : JS_NULL;   /* §5.1: absent is null, not "" */
        free(v);
    }
    }
    JS_FreeCString(ctx, name);
    if (value) JS_FreeCString(ctx, value);
    free(norm);
    return r;
}

/* ---- iteration: §5.1's `iterable<ByteString, ByteString>` ------------------------------------------------ */

/* §2.2.2 Headers' "sort and combine": iteration does NOT walk the raw list. It yields each name ONCE, lowercased and in
   byte order, with that name's values joined by ", " — so `for (const [k, v] of h)` over an append-append pair
   sees one entry, not two. `Set-Cookie` is the exception the spec spells out: each of its values is yielded on
   its own, which is the same reason the list keeps pairs at all.
   Computed per call rather than cached, because the list is live: a callback that appends during forEach must
   be seen by the steps after it, which is what the spec's "value pairs to iterate over" means. */
static void header_sort_and_combine(const HeaderList *l, HeaderList *out)
{
    int i, j;
    for (i = 0; i < l->n; i++) {
        const char *name = l->e[i].name;
        int seen = 0, first = 1;
        char *joined = NULL;
        size_t total = 0;
        for (j = 0; j < i; j++) if (!strcmp(l->e[j].name, name)) { seen = 1; break; }
        if (seen) continue;
        if (!strcmp(name, "set-cookie")) {            /* each value on its own, per §2.2.2's sort and combine */
            for (j = 0; j < l->n; j++)
                if (!strcmp(l->e[j].name, name)) header_list_append(out, name, l->e[j].value);
            continue;
        }
        for (j = 0; j < l->n; j++)
            if (!strcmp(l->e[j].name, name)) total += strlen(l->e[j].value) + 2;
        joined = malloc(total + 1);
        CHECK(joined, "headers: OOM combining a header's values for iteration");
        joined[0] = 0;
        for (j = 0; j < l->n; j++) {
            if (strcmp(l->e[j].name, name)) continue;
            if (!first) strcat(joined, ", ");
            strcat(joined, l->e[j].value);
            first = 0;
        }
        header_list_append(out, name, joined);
        free(joined);
    }
    /* byte order over the names, which is what "sort" means for a header list */
    for (i = 1; i < out->n; i++) {
        HeaderEntry tmp = out->e[i];
        for (j = i - 1; j >= 0 && strcmp(out->e[j].name, tmp.name) > 0; j--)
            out->e[j + 1] = out->e[j];
        out->e[j + 1] = tmp;
    }
}

/* §3.7.9.1 Default iterator objects' DEFAULT ITERATOR OBJECT is the shared one. Headers' own copy of it — the iterator class, the
   prototype, `next`, `keys`/`values`/`entries`, `@@iterator` and the forEach machine — is deleted rather than
   kept beside it: the six things are identical for every `iterable<K, V>` interface, and what is actually
   Headers' is the two operations below. */
static int headers_pair_count(JSContext *ctx, JSValueConst target)
{
    const HeaderList *src = headers_list_of(target);
    HeaderList combined = { 0 };
    int n;
    (void)ctx;
    if (!src) return -1;
    header_sort_and_combine(src, &combined);
    n = combined.n;
    header_list_free(&combined);
    return n;
}

static void headers_pair_at(JSContext *ctx, JSValueConst target, int i, JSValue *key, JSValue *value)
{
    const HeaderList *src = headers_list_of(target);
    HeaderList combined = { 0 };
    DCHECK(src != NULL, "a Headers iterator outlived the Headers it holds a reference to");
    header_sort_and_combine(src, &combined);
    DCHECK(i < combined.n, "a Headers pair was asked for past the end of the combined list");
    *key = JS_NewString(ctx, combined.e[i].name);
    *value = JS_NewString(ctx, combined.e[i].value);
    header_list_free(&combined);
}

static const IdlPairIterOps HEADERS_PAIR_OPS = { headers_pair_count, headers_pair_at, "Headers" };
static int g_pair_handle = -1;

/* ---- the fill (HeadersInit -> a header list) ------------------------------------------------------------ */

enum { FILL_START = 0, FILL_ITER_ASKED, FILL_SEQ_PAIR, FILL_SEQ_ITEM, FILL_KEY_PAIR, FILL_VALUE_STR };

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

/* Web IDL §3.2.23 step 4.2.1's `typedKey = key converted to K`, and K is ByteString — run BEFORE the value's [[Get]] is
   issued, which is what makes a record of {a:"b", "\uFFFF":"d"} five operations and not six. A SYMBOL cannot
   be a ByteString, so an ENUMERABLE symbol key is a TypeError; skipping it was the older, wrong answer. */
static int headers_record_key_ok(JSContext *ctx, JSValueConst key, void *user)
{
    size_t kn_len = 0;
    const char *kn;
    int r;

    (void)user;
    if (JS_IsSymbol(key)) {
        JS_ThrowTypeError(ctx, "a Symbol is not a valid header name");
        return -1;
    }
    kn = JS_ToCStringLen(ctx, &kn_len, key);
    if (!kn) return -1;
    r = headers_bytestring(ctx, kn, kn_len, "name");
    JS_FreeCString(ctx, kn);
    return r;
}

int headers_fill_run(JSContext *ctx, JSStepHdr *h, HeadersFill *f, JSValueConst init, HeaderList *out,
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
            size_t kn_len = 0, kv_len = 0;
            const char *kn, *kv;
            char *norm = NULL;
            int bad;
            /* THE ELEMENT CONVERSION THIS ARM DOES NOT ISSUE. Web IDL §3.2.21.1 Creating a sequence from an
               iterable converts every element to T, and T here is ByteString: for an OBJECT that is ToString,
               which is the PAGE'S code and therefore a request (step_tostring_run), exactly as the record arm
               below issues it for a value. Coercing it from C instead drives the page to completion inside a
               step machine, which is what the preempt hook aborts on — so the shape crashes here, at the item
               that carries it, rather than deep in a driver. A CONCOLIC item is the same missing half from the
               other side: the record arm projects its shape (an external value must not be de-tainted into a
               concrete-looking header) and this arm has no such projection. */
            DCHECK(!JS_IsObject(f->item[0]) && !JS_IsObject(f->item[1]),
                   "a Headers init pair carried an OBJECT item — Web IDL §3.2.21.1's element conversion to "
                   "ByteString is a ToString, and this arm has no step_tostring_run for it");
            DCHECK(!concolic_is(f->item[0]) && !concolic_is(f->item[1]),
                   "a Headers init pair carried a CONCOLIC item — this arm has no shape projection, so the "
                   "coercion below would de-taint external input into a header the report states as observed");
            kn = JS_ToCStringLen(ctx, &kn_len, f->item[0]);
            kv = JS_ToCStringLen(ctx, &kv_len, f->item[1]);
            if (!kn || !kv) {
                if (kn) JS_FreeCString(ctx, kn);
                if (kv) JS_FreeCString(ctx, kv);
                return -1;
            }
            /* THE OTHER HALF OF §3.2.21.1's ELEMENT CONVERSION. Its "converting next to an IDL value of type
               T" is ToString — the two ToCStringLens above, which the DCHECKs make run nothing — followed by
               §3.2.11 step 2, and the element the standard converts FIRST is the one it refuses first. */
            if (headers_bytestring(ctx, kn, kn_len, "name") < 0 ||
                headers_bytestring(ctx, kv, kv_len, "value") < 0) {
                JS_FreeCString(ctx, kn);
                JS_FreeCString(ctx, kv);
                return -1;
            }
            bad = header_check(ctx, kn, kn_len, kv, kv_len, &norm) < 0;
            /* §5.1 fill appends THROUGH the append algorithm — every step of it, not just the guard: a
               Request built with {headers:{Host:"x"}} silently drops it, and a no-cors one drops every
               header that is not no-CORS-safelisted. */
            if (!bad) bad = headers_append_one(ctx, out, guard, kn, norm) < 0;
            free(norm);
            JS_FreeCString(ctx, kn);
            JS_FreeCString(ctx, kv);
            if (bad) return -1;   /* §5.1's guard already threw */
        }
        f->phase = FILL_SEQ_PAIR;
    }
    /* The RECORD arm is Web IDL §3.2.23's own cursor — [[OwnPropertyKeys]], then a descriptor and a [[Get]] per key,
       every one of them a request. It is shared with URLSearchParams because the conversion is Web IDL's and
       not this component's; what stays here is the per-pair half, which is where the ByteString key check, the
       §5.1 guard and the concolic shape live. */
    for (;;) {
        const char *kname, *kval;
        size_t kname_len = 0, kval_len = 0;

        if (f->phase == FILL_KEY_PAIR) {
            r = record_cursor_run(ctx, h, &f->rec, init, in, headers_record_key_ok, NULL, out_cb, out_argc);
            if (r > 0) return r;
            if (r < 0) return -1;
            in = JS_UNDEFINED;
            if (f->rec.done) return 0;
            JS_FreeValue(ctx, f->name);  f->name = JS_DupValue(ctx, f->rec.name);
            JS_FreeValue(ctx, f->value); f->value = JS_DupValue(ctx, f->rec.value);
            f->phase = FILL_VALUE_STR;
        }
        /* AN UNKNOWN VALUE KEEPS ITS SHAPE. A header built out of external input — `{'Authorization': 'Bearer '
           + token}` where the token is server-injected — is a CONCOLIC, and coercing it would either abort at
           the ToString boundary or, worse, quietly de-taint it into some concrete-looking string. Its shape is
           the display form the @H surface reports, and the `{hole}` in it is exactly what marks the header as a
           runtime value the reviewer has to supply. This is the same explicit projection fetch_park asks for on
           the URL, for the same reason. */
        DCHECK(f->phase == FILL_VALUE_STR, "the headers fill was re-entered at a phase it never parks in");
        /* AN UNKNOWN VALUE KEEPS ITS SHAPE. A header built out of external input — `{'Authorization': 'Bearer '
           + token}` where the token is server-injected — is a CONCOLIC, and coercing it would either abort at
           the ToString boundary or, worse, quietly de-taint it into some concrete-looking string. Its shape is
           the display form the @H surface reports, and the `{hole}` in it is exactly what marks the header as a
           runtime value the reviewer has to supply. This is the same explicit projection fetch_park asks for on
           the URL, for the same reason. */
        if (concolic_is(f->value)) {
            const char *sh = concolic_shape_c(f->value);
            JSValue sv = JS_NewString(ctx, sh ? sh : "{}");
            if (JS_IsException(sv)) return -1;
            JS_FreeValue(ctx, f->value);
            f->value = sv;
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
        kname = JS_ToCStringLen(ctx, &kname_len, f->name);
        kval = JS_ToCStringLen(ctx, &kval_len, f->value);
        if (!kname || !kval) {
            if (kname) JS_FreeCString(ctx, kname);
            if (kval) JS_FreeCString(ctx, kval);
            return -1;
        }
        /* §3.2.23 step 4.2.3's `typedValue = value converted to V`, and V is ByteString: the ToString is the
           request above, and this is the range that completes it. It is asked HERE, at the conversion, and
           not inside §5.1's syntax check, because it is the TYPE's refusal — the key's own already ran one
           step earlier, in the cursor, which is what makes `{"Ā": "v"}` refuse before the [[Get]].
           THE CONCOLIC ARM TAKES IT TOO: the projected shape is the bytes this header would carry, and a
           header the engine cannot state as bytes is not one it may report. */
        if (headers_bytestring(ctx, kval, kval_len, "value") < 0) {
            JS_FreeCString(ctx, kname);
            JS_FreeCString(ctx, kval);
            return -1;
        }
        {
            char *knorm = NULL;
            if (header_check(ctx, kname, kname_len, kval, kval_len, &knorm) < 0) {
                JS_FreeCString(ctx, kname); JS_FreeCString(ctx, kval);
                return -1;   /* §5.1's guard already threw */
            }
            /* §5.1: "Otherwise, object is a record, then for each key → value of object, APPEND (key, value)
               to headers." Web IDL §3.2.23's map semantics have ALREADY deduped the record — by its ByteString KEY,
               which is case-SENSITIVE — so the two entries of `{a:"1", A:"2"}` both arrive and both append,
               and `get("a")` combines them into "1, 2". A replace loop stood here matching on the LOWERCASED
               header name, which is a different equivalence than the record's and belongs to no step: it
               answered "2", and it is deleted rather than kept for the one shape it looked right on. */
            if (headers_append_one(ctx, out, guard, kname, knorm) < 0) {
                free(knorm);
                JS_FreeCString(ctx, kname); JS_FreeCString(ctx, kval);
                return -1;
            }
            free(knorm);
        }
        JS_FreeCString(ctx, kname);
        JS_FreeCString(ctx, kval);
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

/* `list` IS A POINTER and that is what makes this machine forkable: quickjs-step.h's `tree` operation ANSWERS
   a new root and writes it into the slot, so an inline struct could not be declared through it. See headers.h's
   header_list_step_ops. */
typedef struct { HeadersFill fill; HeaderList *list; JSValue result; } JSHeadersCtorState;

static void js_headers_ctor_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSHeadersCtorState *s = st;
    headers_fill_visit(ctx, &s->fill, v);
    v->val(ctx, &s->result);
    /* STEP 2'S LIST. This is the line that deleted this machine's fork refusal AND its `release`: the
       operation's own clone makes the sibling its own list and its own destroy frees it, so there is nothing
       left for a teardown beside the declaration to do. No cursor stands inside it — the fill holds the list
       as an argument and not as a field — so the count is zero and the array is NULL with it. */
    v->tree(ctx, (void **)&s->list, NULL, 0, &header_list_step_ops);
}

/* THE HEADER LIST ALONE — a malloc'd array of malloc'd name/value pairs, which is not a reference and which no
   declaration names. Everything else this state holds is named by js_headers_ctor_visit and released through
   that one list. */
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
        /* STEP 2'S LIST, BUILT BEFORE THE FILL THAT APPENDS TO IT. The test is idempotent because this stage
           can PARK — `new Headers()` with a Proxy `init` runs the page's code at the own-keys read — and a
           parked stage is re-entered at its first line, so an unguarded allocation here would leak one list
           per re-entry. A POINTER is a legitimate "is it built" question where a zeroed JSValue slot is not:
           js_mallocz leaves this NULL and nothing but this line writes it. */
        if (!s->list) s->list = header_list_step_new(ctx);
        hdr->stage = HDR_CTOR_FILL;
    }
    DCHECK(hdr->stage == HDR_CTOR_FILL,
           "the Headers constructor was re-entered at a stage §5.1 does not have");
    /* §5.1: `new Headers(init)` sets the guard to "none" and then fills — a page's own Headers refuses
       nothing, which is why the forbidden lists are unobservable until a Request or a Response owns one. */
    DCHECK(s->list != NULL,
           "the Headers constructor reached §5.1 step 2's fill with no header list — step 1 is the one line "
           "that builds it, so a fill without one is a stage reached without its predecessor");
    r = headers_fill_run(ctx, hdr, &s->fill, argc > 0 ? argv[0] : JS_UNDEFINED, s->list,
                         HEADERS_GUARD_NONE, cb_result, out_cb, out_argc);
    if (r > 0) return r;
    if (r < 0) return -1;
    *presult = headers_new(ctx, s->list, HEADERS_GUARD_NONE);
    return JS_IsException(*presult) ? -1 : 0;
}

/* WHY THIS MACHINE MAY NOW BE FORKED WHILE IT HOLDS STEP 2'S HEADER LIST, which is the whole of what this
 * paragraph is: the refusal `js_headers_ctor_unforkable` is DELETED and the list is declared.
 *
 * ITS REASON WAS TRUE AND IT WAS A CONSTRAINT ON THE LIST'S STORAGE AND NOT ON THIS ALGORITHM. A step state is
 * BYTE-COPIED at a deep fork and only what `visit` names is re-taken, this list's array and its name/value
 * strings are the C library's in `header_list_append`/`header_list_free`, and every JSStepVisit operation that
 * copies FOR ITSELF (`v->buf`, `v->array`, `v->props`, `v->slots`, `v->strbuf`) allocates with the ENGINE's —
 * so the two could not be reconciled by naming the list through any of them. That stated the choice as §5.1's
 * entries becoming engine-allocated slots (a change to `HeaderList`'s storage and therefore to all 31 files
 * that hold one, several of which — a class finalizer, main.c, the WPT runner, structured_fields.c — have no
 * JSContext to allocate with) or `v->tree`, which delegates BOTH the copy and the destroy to the host.
 *
 * IT IS `v->tree`, AND THE QUESTION THAT HAD TO BE SETTLED FIRST WAS WHETHER THAT OPERATION'S SUBJECT IS A
 * TREE. It is not: its three consumers in quickjs.c dereference nothing — the clone calls `ops->clone` and
 * asserts the answer, the teardown calls `ops->destroy` and clears the cursors, and the fingerprint walk folds
 * the pointers and CALLS NEITHER OPERATION — and the reason its own banner gives for it needing to exist is
 * negative and allocation-shaped ("not a JSValue and not an allocation with a size, so no other operation can
 * name one"), which is exactly true of a header list. quickjs-step.h now says so, and says why the FIELD is
 * still spelled `tree`.
 *
 * WHAT IT COST, AND IT IS ONE THING: the list is a HEAP ROOT rather than an inline struct, because the clone
 * ANSWERS a new pointer the operation writes back. No allocator moved and no file outside core/fetch/ was
 * touched.
 *
 * WHAT IT BUYS, STATED HONESTLY: a refusal answered "unforkable" and the primary arm still ran, so what was
 * lost was BREADTH and never the Headers object — `new Headers([["a","1"],["b",v]])` whose second read forks
 * now explores both arms where it used to explore one. It needs TWO pairs and not one, because both fill arms
 * APPEND and then loop back to a parking phase, so a one-key init runs its getter before any append.
 *
 * AND BEFORE THE REFUSAL EXISTED AT ALL THIS WAS A DOUBLE FREE — the fork was taken, two arms carried one
 * `HeaderList::e`, and the second teardown freed it again. The refusal was the capability's existing
 * declaration made at the machine that was missing it; this is the capability.
 *
 * HOW THE ROUTING'S ABSENCE WOULD SHOW, now that there is no abort to watch for: idl_args.c's fingerprint
 * bracket around every member's `release` folds the slots the `visit` names and asserts the `release` left
 * them alone, and `js_step_visit_fp_tree` folds this root — so a `release` re-added here that freed the list
 * would move the fold and fire that DCHECK by name. That is why this machine has no `release` at all rather
 * than an emptied one. */
static const IdlStepDecl js_headers_ctor_decl = {
    /* NO `release`. Step 2's list is declared through `visit`, so idl_args.c's one teardown discharges it and
       there is nothing a second list beside the declaration could be for. The slot was `js_headers_ctor_release`
       and that function freed the list the declaration now names; both went in the diff that routed it. */
    js_headers_ctor_step, sizeof(JSHeadersCtorState), js_headers_ctor_visit, NULL,
    "Fetch §5.1 new Headers(init)", HDR_CTOR_STEPS,
    /* `catches_abrupt` = 0: this constructor PROPAGATES — a throwing header key or value is the page's to see
       at the `new Headers` it wrote, and the epilogue re-raises it. THE REASON FOR SPELLING IT RATHER THAN
       LEAVING IT TO THE ZERO-FILL IS RETIRED — it was "a field AFTER it is declared", and that field was
       `unforkable`, which this machine no longer declares. It is kept spelled because the next field added
       after it would need it spelled again, and a reader who re-derives "nothing follows it, so drop it" has
       to re-add it then. */
    0
};

/* ---- install --------------------------------------------------------------------------------------------- */

void headers_init(JSContext *ctx)
{
    JSClassDef def = { "Headers", .finalizer = headers_finalizer };
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


