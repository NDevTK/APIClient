/* css-transforms-1 §6.2 and §6.3. See css_transform_attribute.h. */
#include <ctype.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "core/css/css_length.h"
#include "core/css/css_transform_attribute.h"
#include "core/json_buf.h"

typedef struct {
    const char *p, *e;
} TaCursor;

/* §6.2's `wsp`: "Either a U+000A LINE FEED, U+000D CARRIAGE RETURN, U+0009 CHARACTER TABULATION, or U+0020
   SPACE." */
static bool ta_is_wsp(char c)
{
    return c == '\n' || c == '\r' || c == '\t' || c == ' ';
}

static void ta_wsp(TaCursor *c)
{
    while (c->p < c->e && ta_is_wsp(*c->p)) c->p++;
}

/* `comma-wsp`, "(wsp+ comma? wsp*) | (comma wsp*)", where the caller's grammar makes it optional. */
static void ta_comma_wsp(TaCursor *c)
{
    ta_wsp(c);
    if (c->p < c->e && *c->p == ',') {
        c->p++;
        ta_wsp(c);
    }
}

static bool ta_digits(TaCursor *c)
{
    const char *s = c->p;

    while (c->p < c->e && isdigit((unsigned char)*c->p)) c->p++;
    return c->p > s;
}

/* A CSS Syntax §4.3.12 "Consume a number" `<number-token>`: an optional sign, digits with an optional fraction
   or a fraction alone, and an optional exponent. False moves nothing. */
static bool ta_number(TaCursor *c, double *out)
{
    TaCursor t = *c;
    char buf[64];
    size_t n;
    bool whole, frac = false;

    if (t.p < t.e && (*t.p == '+' || *t.p == '-')) t.p++;
    whole = ta_digits(&t);
    if (t.p + 1 < t.e && *t.p == '.' && isdigit((unsigned char)t.p[1])) {
        t.p++;
        frac = ta_digits(&t);
    }
    if (!whole && !frac) return false;
    if (t.p < t.e && (*t.p == 'e' || *t.p == 'E')) {
        TaCursor x = t;

        x.p++;
        if (x.p < x.e && (*x.p == '+' || *x.p == '-')) x.p++;
        if (ta_digits(&x)) t = x;
    }
    n = (size_t)(t.p - c->p);
    if (n >= sizeof buf) return false;
    memcpy(buf, c->p, n);
    buf[n] = '\0';
    *out = strtod(buf, NULL);
    c->p = t.p;
    return true;
}

/* One function's parenthesized numbers, at most `max`, each after the first preceded by an optional
   `comma-wsp`; `*count` is how many were read. */
static bool ta_numbers(TaCursor *c, double *v, unsigned max, unsigned *count)
{
    unsigned k = 0;

    ta_wsp(c);
    if (c->p >= c->e || *c->p != '(') return false;
    c->p++;
    ta_wsp(c);
    while (k < max) {
        TaCursor save = *c;

        if (k > 0) ta_comma_wsp(c);
        if (!ta_number(c, &v[k])) {
            *c = save;
            break;
        }
        k++;
    }
    ta_wsp(c);
    if (c->p >= c->e || *c->p != ')') return false;
    c->p++;
    *count = k;
    return true;
}

static void ta_num(JsonBuf *b, double v, const char *unit)
{
    char *t = css_length_serialize_number(v, unit);

    json_buf_raw(b, t);
    free(t);
}

/* §6.2's `transform` production, appended to `b` as §6.3's CSS functions. */
static bool ta_function(TaCursor *c, JsonBuf *b)
{
    static const struct { const char *name; unsigned min, max; } FNS[] = {
        { "matrix", 6, 6 }, { "translate", 1, 2 }, { "scale", 1, 2 }, { "rotate", 1, 3 }, { "skewX", 1, 1 },
        { "skewY", 1, 1 },
    };
    double v[6];
    unsigned i, k, n = 0;
    size_t len;

    for (i = 0; i < COUNTOF(FNS); i++) {
        len = strlen(FNS[i].name);
        if ((size_t)(c->e - c->p) >= len && memcmp(c->p, FNS[i].name, len) == 0) break;
    }
    if (i == COUNTOF(FNS)) return false;
    c->p += len;
    if (!ta_numbers(c, v, FNS[i].max, &n) || n < FNS[i].min) return false;
    /* rotate's grammar is "number ( comma-wsp? number comma-wsp? number )?", one or three. */
    if (i == 3 && n == 2) return false;
    switch (i) {
    case 0:
        json_buf_raw(b, "matrix(");
        for (k = 0; k < 6; k++) {
            if (k > 0) json_buf_raw(b, ", ");
            ta_num(b, v[k], "");
        }
        json_buf_raw(b, ")");
        break;
    /* "Number values interpreted as CSS <length> types with px units." */
    case 1:
        json_buf_raw(b, "translate(");
        ta_num(b, v[0], "px");
        if (n > 1) {
            json_buf_raw(b, ", ");
            ta_num(b, v[1], "px");
        }
        json_buf_raw(b, ")");
        break;
    case 2:
        json_buf_raw(b, "scale(");
        ta_num(b, v[0], "");
        if (n > 1) {
            json_buf_raw(b, ", ");
            ta_num(b, v[1], "");
        }
        json_buf_raw(b, ")");
        break;
    /* "Number value interpreted as CSS <angle> type with deg unit." With three values, "The behavior is
       equivalent to an initial translation by cx, cy, a rotation defined by the first number value interpreted
       as <angle> type with deg unit followed by a translation by -cx, -cy." */
    case 3:
        if (n == 3) {
            json_buf_raw(b, "translate(");
            ta_num(b, v[1], "px");
            json_buf_raw(b, ", ");
            ta_num(b, v[2], "px");
            json_buf_raw(b, ") ");
        }
        json_buf_raw(b, "rotate(");
        ta_num(b, v[0], "deg");
        json_buf_raw(b, ")");
        if (n == 3) {
            json_buf_raw(b, " translate(");
            ta_num(b, -v[1], "px");
            json_buf_raw(b, ", ");
            ta_num(b, -v[2], "px");
            json_buf_raw(b, ")");
        }
        break;
    case 4:
    case 5:
        json_buf_raw(b, i == 4 ? "skewX(" : "skewY(");
        ta_num(b, v[0], "deg");
        json_buf_raw(b, ")");
        break;
    default:
        DFAIL("css-transforms-1 §6.2's function table and its mapping switch have come apart");
        return false;
    }
    return true;
}

char *css_transform_attribute_to_css(const char *value, size_t len)
{
    TaCursor c;
    JsonBuf b = { 0 };
    bool any = false;
    char *out;

    DCHECK(value != NULL || len == 0, "an SVG transform attribute was parsed from no bytes");
    c.p = value;
    c.e = value + len;
    /* "transform-list: wsp* transforms? wsp*", with "transforms: transform | transform comma-wsp? transforms". */
    ta_wsp(&c);
    while (c.p < c.e) {
        if (any) {
            ta_comma_wsp(&c);
            json_buf_raw(&b, " ");
        }
        if (!ta_function(&c, &b)) {
            json_buf_free(&b);
            return NULL;
        }
        any = true;
        ta_wsp(&c);
    }
    if (!any) json_buf_raw(&b, "none");
    out = json_buf_take(&b);
    CHECK(out != NULL, "css-transforms-1 §6.2: OOM composing a transform attribute's CSS value");
    return out;
}
