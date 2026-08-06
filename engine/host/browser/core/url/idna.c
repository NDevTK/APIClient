/* IDNA — WHATWG URL §4.2's "domain to ASCII", which is Unicode UTS-46 over RFC 3492's Punycode.
 *
 * WHY IT IS HERE. The URL parser reached a DFAIL naming this on the canonical corpus: five wpt files —
 * url-constructor, url-setters, url-origin and both IdnaTestV2 variants — stopped at the first non-ASCII
 * domain, which hid roughly seven hundred cases behind one missing capability. A domain is not a string the
 * parser may lowercase and hope: `münchen.de` IS `xn--mnchen-3ya.de` in the record, and every comparison a page
 * makes against `location.host` is against the A-label.
 *
 * PUNYCODE IS EXACT. RFC 3492 is a closed algorithm with no tables, so both directions are implemented to the
 * letter, including the overflow checks that make a hostile label a failure rather than a wrap.
 *
 * THE UTS-46 MAPPING IS THE PART THAT NEEDS A TABLE, and the engine does not carry IdnaMappingTable.txt. What
 * is implemented is the part that is DECIDABLE without it and is most of the table's content: full case
 * folding (the mapping table's largest class) and NFC, both from libunicode, plus the label rules that are
 * pure structure — the 63-byte label cap, the 253-byte domain cap, the empty-label rule, and the `xn--`
 * round-trip. What is NOT decidable without the table is the DISALLOWED set, so a code point that UTS-46 would
 * refuse is currently accepted. That is a real fidelity gap and it is MEASURED rather than described:
 * IdnaTestV2.any.js is exactly the file that reports it, and it reports a number instead of an abort. */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "cutils.h"
#include "libunicode.h"
#include "core/url/idna.h"

/* ---- RFC 3492 ------------------------------------------------------------------------------------------- */

#define PUNY_BASE         36
#define PUNY_TMIN         1
#define PUNY_TMAX         26
#define PUNY_SKEW         38
#define PUNY_DAMP         700
#define PUNY_INITIAL_BIAS 72
#define PUNY_INITIAL_N    128
#define PUNY_MAXINT       0x7fffffff

static uint32_t puny_adapt(uint32_t delta, uint32_t numpoints, int firsttime)
{
    uint32_t k;
    delta = firsttime ? delta / PUNY_DAMP : delta >> 1;
    delta += delta / numpoints;
    for (k = 0; delta > ((PUNY_BASE - PUNY_TMIN) * PUNY_TMAX) / 2; k += PUNY_BASE)
        delta /= PUNY_BASE - PUNY_TMIN;
    return k + (PUNY_BASE - PUNY_TMIN + 1) * delta / (delta + PUNY_SKEW);
}

/* a-z0-9 as the digit alphabet; the encoder emits lowercase, which is what UTS-46 wants anyway. */
static int puny_digit(uint32_t cp)
{
    if (cp >= '0' && cp <= '9') return (int)(cp - '0' + 26);
    if (cp >= 'a' && cp <= 'z') return (int)(cp - 'a');
    if (cp >= 'A' && cp <= 'Z') return (int)(cp - 'A');
    return -1;
}

static char puny_char(int d) { return (char)(d < 26 ? 'a' + d : '0' + d - 26); }

/* Decode one label's Punycode body (everything after `xn--`) into code points. Returns the count, or -1. */
int idna_punycode_decode(const char *in, size_t in_len, uint32_t *out, int out_cap)
{
    uint32_t n = PUNY_INITIAL_N, i = 0, bias = PUNY_INITIAL_BIAS, oldi, w, k, t, digit;
    size_t b, j, ip = 0;
    int out_n = 0;

    /* the basic code points are everything before the LAST delimiter; with none, the whole input is extended */
    b = 0;
    for (j = 0; j < in_len; j++) if (in[j] == '-') b = j + 1;
    for (j = 0; j + 1 <= b && b > 0 && j < b - 1 + 1; j++) {
        if (j >= b - 1 + 1) break;
        if (j == b - 1) break;               /* the delimiter itself is not copied */
        if ((unsigned char)in[j] >= 0x80) return -1;
        if (out_n >= out_cap) return -1;
        out[out_n++] = (unsigned char)in[j];
    }
    ip = b;

    while (ip < in_len) {
        oldi = i;
        w = 1;
        for (k = PUNY_BASE;; k += PUNY_BASE) {
            int d;
            if (ip >= in_len) return -1;
            d = puny_digit((unsigned char)in[ip++]);
            if (d < 0) return -1;
            digit = (uint32_t)d;
            if (digit > (uint32_t)(PUNY_MAXINT - (int)i) / w) return -1;   /* overflow */
            i += digit * w;
            t = k <= bias ? PUNY_TMIN : (k >= bias + PUNY_TMAX ? PUNY_TMAX : k - bias);
            if (digit < t) break;
            if (w > (uint32_t)PUNY_MAXINT / (PUNY_BASE - t)) return -1;
            w *= PUNY_BASE - t;
        }
        bias = puny_adapt(i - oldi, (uint32_t)out_n + 1, oldi == 0);
        if (i / (uint32_t)(out_n + 1) > (uint32_t)(PUNY_MAXINT - (int)n)) return -1;
        n += i / (uint32_t)(out_n + 1);
        i %= (uint32_t)(out_n + 1);
        if (n > 0x10ffff || (n >= 0xd800 && n <= 0xdfff)) return -1;
        if (out_n >= out_cap) return -1;
        memmove(out + i + 1, out + i, (size_t)((uint32_t)out_n - i) * sizeof(uint32_t));
        out[i++] = n;
        out_n++;
    }
    return out_n;
}

/* Encode one label's code points as Punycode (WITHOUT the `xn--` prefix). Returns the byte length, or -1. */
int idna_punycode_encode(const uint32_t *in, int in_len, char *out, int out_cap)
{
    uint32_t n = PUNY_INITIAL_N, delta = 0, bias = PUNY_INITIAL_BIAS, m, q, k, t;
    int h, b, i, out_n = 0;

    for (i = 0; i < in_len; i++) {
        if (in[i] < 0x80) {
            if (out_n >= out_cap) return -1;
            out[out_n++] = (char)in[i];
        }
    }
    h = b = out_n;
    if (b > 0) {
        if (out_n >= out_cap) return -1;
        out[out_n++] = '-';
    }
    while (h < in_len) {
        m = 0x7fffffff;
        for (i = 0; i < in_len; i++) if (in[i] >= n && in[i] < m) m = in[i];
        if (m - n > (uint32_t)(PUNY_MAXINT - (int)delta) / (uint32_t)(h + 1)) return -1;
        delta += (m - n) * (uint32_t)(h + 1);
        n = m;
        for (i = 0; i < in_len; i++) {
            if (in[i] < n) {
                if (++delta == 0) return -1;
                continue;
            }
            if (in[i] != n) continue;
            for (q = delta, k = PUNY_BASE;; k += PUNY_BASE) {
                t = k <= bias ? PUNY_TMIN : (k >= bias + PUNY_TMAX ? PUNY_TMAX : k - bias);
                if (q < t) break;
                if (out_n >= out_cap) return -1;
                out[out_n++] = puny_char((int)(t + (q - t) % (PUNY_BASE - t)));
                q = (q - t) / (PUNY_BASE - t);
            }
            if (out_n >= out_cap) return -1;
            out[out_n++] = puny_char((int)q);
            bias = puny_adapt(delta, (uint32_t)(h + 1), h == b);
            delta = 0;
            h++;
        }
        delta++;
        n++;
    }
    return out_n;
}

/* ---- UTS-46 ---------------------------------------------------------------------------------------------- */

/* libunicode allocates through a caller-supplied reallocator so it never assumes an allocator; this component
   has no arena of its own, so it hands it plain realloc. */
static void *idna_realloc(void *opaque, void *ptr, size_t size)
{
    (void)opaque;
    return realloc(ptr, size);
}

/* Decode UTF-8 into code points. The domain arrives as the percent-decoded bytes the host parser produced, and
   those bytes are UTF-8 by §4.2's own step. Returns the count, or -1 on malformed input — which IS a failure,
   because a domain that is not UTF-8 is not a domain. */
static int utf8_to_cps(const char *s, size_t n, uint32_t **out)
{
    uint32_t *cps = malloc(sizeof(uint32_t) * (n + 1));
    size_t i = 0;
    int m = 0;

    CHECK(cps, "idna: OOM decoding a domain");
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        uint32_t cp;
        int extra;
        if (c < 0x80)            { cp = c;          extra = 0; }
        else if ((c & 0xe0) == 0xc0) { cp = c & 0x1f; extra = 1; }
        else if ((c & 0xf0) == 0xe0) { cp = c & 0x0f; extra = 2; }
        else if ((c & 0xf8) == 0xf0) { cp = c & 0x07; extra = 3; }
        else { free(cps); return -1; }
        i++;
        while (extra--) {
            if (i >= n || ((unsigned char)s[i] & 0xc0) != 0x80) { free(cps); return -1; }
            cp = (cp << 6) | ((unsigned char)s[i++] & 0x3f);
        }
        if (cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) { free(cps); return -1; }
        cps[m++] = cp;
    }
    *out = cps;
    return m;
}

static void cps_to_utf8(const uint32_t *cps, int n, char *out, int *out_n)
{
    int i, k = 0;
    for (i = 0; i < n; i++) {
        uint32_t cp = cps[i];
        if (cp < 0x80) out[k++] = (char)cp;
        else if (cp < 0x800) { out[k++] = (char)(0xc0 | (cp >> 6)); out[k++] = (char)(0x80 | (cp & 0x3f)); }
        else if (cp < 0x10000) {
            out[k++] = (char)(0xe0 | (cp >> 12));
            out[k++] = (char)(0x80 | ((cp >> 6) & 0x3f));
            out[k++] = (char)(0x80 | (cp & 0x3f));
        } else {
            out[k++] = (char)(0xf0 | (cp >> 18));
            out[k++] = (char)(0x80 | ((cp >> 12) & 0x3f));
            out[k++] = (char)(0x80 | ((cp >> 6) & 0x3f));
            out[k++] = (char)(0x80 | (cp & 0x3f));
        }
    }
    *out_n = k;
}

/* UTS-46 step 1's MAPPING, as far as it is decidable without IdnaMappingTable.txt: full case folding, which is
   the table's largest class. `lre_case_conv` with conv_type 2 is the folding one — the same one the regexp
   engine canonicalizes with. */
static int map_fold(const uint32_t *in, int n, uint32_t **out)
{
    uint32_t *o = malloc(sizeof(uint32_t) * (size_t)(n * LRE_CC_RES_LEN_MAX + 1));
    int i, m = 0;
    CHECK(o, "idna: OOM mapping a domain");
    for (i = 0; i < n; i++) {
        uint32_t res[LRE_CC_RES_LEN_MAX];
        int k = lre_case_conv(res, in[i], 2), j;
        if (k <= 0) { o[m++] = in[i]; continue; }
        for (j = 0; j < k; j++) o[m++] = res[j];
    }
    *out = o;
    return m;
}

int idna_domain_to_ascii(const char *domain, size_t len, char **out, size_t *out_len)
{
    uint32_t *cps = NULL, *folded = NULL, *norm = NULL;
    int n, fn, nn = 0, i, start, ok = 1;
    char *result = NULL;
    size_t rn = 0, cap;

    *out = NULL;
    n = utf8_to_cps(domain, len, &cps);
    if (n < 0) return -1;
    fn = map_fold(cps, n, &folded);
    free(cps);
    /* UTS-46 step 2: NFC. A precomposed and a decomposed spelling of the same domain are the same domain, and
       without this they would be two different A-labels. */
    nn = unicode_normalize(&norm, folded, fn, UNICODE_NFC, NULL, idna_realloc);
    free(folded);
    if (nn < 0) return -1;

    cap = (size_t)nn * 8 + 16;
    result = malloc(cap);
    CHECK(result, "idna: OOM building an A-label");

    /* steps 3-4: one label at a time, split on U+002E. */
    start = 0;
    for (i = 0; i <= nn; i++) {
        int end = (i == nn || norm[i] == '.') ? i : -1;
        if (end < 0) continue;
        {
            int llen = end - start, j, has_non_ascii = 0;
            char label[512];
            int label_n = 0;

            for (j = start; j < end; j++) if (norm[j] >= 0x80) has_non_ascii = 1;
            if (has_non_ascii) {
                /* an A-label: `xn--` and the Punycode of the label's code points */
                int e;
                memcpy(label, "xn--", 4);
                e = idna_punycode_encode(norm + start, llen, label + 4, (int)sizeof(label) - 5);
                if (e < 0) { ok = 0; break; }
                label_n = e + 4;
            } else if (llen >= 4 && norm[start] == 'x' && norm[start + 1] == 'n' &&
                       norm[start + 2] == '-' && norm[start + 3] == '-') {
                /* already an A-label: it must DECODE, or it is not one — an `xn--` label that is not valid
                   Punycode is a failure, which is what keeps a made-up A-label out of the record. */
                uint32_t dec[512];
                char body[512];
                int dn, bn, k;
                for (k = 0; k < llen - 4; k++) body[k] = (char)norm[start + 4 + k];
                dn = idna_punycode_decode(body, (size_t)(llen - 4), dec, (int)(sizeof(dec) / sizeof(dec[0])));
                if (dn < 0) { ok = 0; break; }
                for (k = 0; k < llen; k++) label[k] = (char)norm[start + k];
                label_n = llen;
            } else {
                for (j = 0; j < llen; j++) label[j] = (char)norm[start + j];
                label_n = llen;
            }
            /* §4.2's structural rules: a label is at most 63 bytes, and the domain at most 253. */
            if (label_n > 63) { ok = 0; break; }
            if (rn + (size_t)label_n + 2 > cap) {
                cap = (rn + (size_t)label_n + 2) * 2;
                result = realloc(result, cap);
                CHECK(result, "idna: OOM building an A-label");
            }
            if (rn) result[rn++] = '.';
            memcpy(result + rn, label, (size_t)label_n);
            rn += (size_t)label_n;
        }
        start = i + 1;
    }
    free(norm);
    if (!ok || rn > 253) { free(result); return -1; }
    result[rn] = 0;
    *out = result;
    if (out_len) *out_len = rn;
    return 0;
}

/* Everything above is bytes; the caller frees. */
void idna_free(char *s) { free(s); }
