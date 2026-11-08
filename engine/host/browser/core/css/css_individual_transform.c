/* css-transforms-2 §5's translate, rotate and scale. See css_individual_transform.h. */
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "core/css/css_dimension.h"
#include "core/css/css_individual_transform.h"
#include "core/css/css_shorthand.h"
#include "core/json_buf.h"

static bool it_word_is(const char *w, size_t n, const char *kw)
{
    size_t i;

    for (i = 0; i < n; i++)
        if (kw[i] == '\0' || (char)tolower((unsigned char)w[i]) != kw[i]) return false;
    return kw[n] == '\0';
}

/* Parses `text` as exactly one function of `kind`. */
static bool it_one(JSContext *realm, const CssFontMetrics *font, const char *text, CssTransformFnKind kind,
                   CssTransformFunction *out)
{
    CssTransformList list;
    bool ok;

    if (!css_transform_list_parse(realm, font, text, &list)) return false;
    ok = list.n == 1 && list.fn[0].kind == kind;
    if (ok) *out = list.fn[0];
    css_transform_list_free(&list);
    return ok;
}

bool css_individual_transform_parse(JSContext *realm, const CssFontMetrics *font, const char *name,
                                    const char *value, CssTransformFunction *out)
{
    const char *w[5];
    size_t wl[5];
    char buf[512];
    int n;

    DCHECK(name != NULL && value != NULL && out != NULL, "css-transforms-2 §5 was asked with no name or value");
    n = css_shorthand_components(value, w, wl, 4);
    if (n < 1 || (n == 1 && it_word_is(w[0], wl[0], "none"))) return false;
    for (int i = 0; i < n; i++)
        if (wl[i] > 100) return false;
    if (strcmp(name, "translate") == 0) {
        /* "accepts 1-3 values, each specifying a translation against one axis, in the order X, Y, then Z. When
           the second or third values are missing, they default to 0px." */
        if (n > 3) return false;
        snprintf(buf, sizeof buf, "translate3d(%.*s, %.*s, %.*s)", (int)wl[0], w[0], n > 1 ? (int)wl[1] : 3,
                 n > 1 ? w[1] : "0px", n > 2 ? (int)wl[2] : 3, n > 2 ? w[2] : "0px");
        return it_one(realm, font, buf, CSS_TF_TRANSLATE3D, out);
    }
    if (strcmp(name, "scale") == 0) {
        /* `[ <number> | <percentage> ]{1,3}`: a missing Y is the X, and a missing Z is 1. */
        if (n > 3) return false;
        snprintf(buf, sizeof buf, "scale3d(%.*s, %.*s, %.*s)", (int)wl[0], w[0], (int)wl[n > 1 ? 1 : 0],
                 w[n > 1 ? 1 : 0], n > 2 ? (int)wl[2] : 1, n > 2 ? w[2] : "1");
        return it_one(realm, font, buf, CSS_TF_SCALE3D, out);
    }
    DCHECK(strcmp(name, "rotate") == 0, "css-transforms-2 §5 has three individual transforms");
    /* `none | <angle> | [ x | y | z | <number>{3} ] && <angle>`: the x, y and z keywords are rotateX(),
       rotateY() and rotateZ(), and three numbers are rotate3d()'s axis, with the angle on either side. */
    if (n == 1) {
        snprintf(buf, sizeof buf, "rotate(%.*s)", (int)wl[0], w[0]);
        return it_one(realm, font, buf, CSS_TF_ROTATE, out);
    }
    if (n == 2) {
        static const struct { const char *kw, *fn; CssTransformFnKind kind; } AX[] = {
            { "x", "rotateX", CSS_TF_ROTATE_X }, { "y", "rotateY", CSS_TF_ROTATE_Y }, { "z", "rotateZ", CSS_TF_ROTATE_Z },
        };
        for (size_t k = 0; k < COUNTOF(AX); k++)
            for (int a = 0; a < 2; a++)
                if (it_word_is(w[a], wl[a], AX[k].kw)) {
                    snprintf(buf, sizeof buf, "%s(%.*s)", AX[k].fn, (int)wl[1 - a], w[1 - a]);
                    return it_one(realm, font, buf, AX[k].kind, out);
                }
        return false;
    }
    if (n == 4) {
        snprintf(buf, sizeof buf, "rotate3d(%.*s, %.*s, %.*s, %.*s)", (int)wl[0], w[0], (int)wl[1], w[1],
                 (int)wl[2], w[2], (int)wl[3], w[3]);
        if (it_one(realm, font, buf, CSS_TF_ROTATE3D, out)) return true;
        snprintf(buf, sizeof buf, "rotate3d(%.*s, %.*s, %.*s, %.*s)", (int)wl[1], w[1], (int)wl[2], w[2],
                 (int)wl[3], w[3], (int)wl[0], w[0]);
        return it_one(realm, font, buf, CSS_TF_ROTATE3D, out);
    }
    return false;
}

static void it_raw_owned(JsonBuf *b, char *t)
{
    json_buf_raw(b, t);
    free(t);
}

static bool it_zero(const CssLength *len)
{
    return len->kind == CSS_LENGTH_ABSOLUTE && len->px.env == CSS_ENV_NONE && len->px.px == 0.0;
}

/* An angle argument in css-values-4 §7.1's canonical unit, deg. */
static char *it_angle(const CssTransformFunction *fn)
{
    double deg = 0.0;
    bool ok = fn->angle_unit[0][0] == '\0' ||
              css_angle_deg(fn->angle_unit[0], strlen(fn->angle_unit[0]), fn->angle[0], &deg);

    DCHECK(ok, "a rotate angle carries a unit core/css/css_transform_function.c admitted and css_angle_deg "
               "does not convert");
    return css_length_serialize_number(deg + 0.0, "deg");
}

char *css_individual_transform_serialize(const char *name, const CssTransformFunction *fn)
{
    JsonBuf b = { 0 };
    char *out;
    int k, last;

    DCHECK(name != NULL && fn != NULL, "css-transforms-2 §5's computed value was serialized with nothing");
    if (strcmp(name, "translate") == 0) {
        /* "the keyword none or a pair of computed <length-percentage> values and an absolute length", with the
           trailing zero components omitted as the shorter equivalent value. */
        DCHECK(fn->kind == CSS_TF_TRANSLATE3D, "a computed translate is not its translate3d()");
        last = !it_zero(&fn->length[2]) ? 2 : !it_zero(&fn->length[1]) ? 1 : 0;
        for (k = 0; k <= last; k++) {
            if (k > 0) json_buf_raw(&b, " ");
            it_raw_owned(&b, css_transform_length_serialize(&fn->length[k]));
        }
    } else if (strcmp(name, "scale") == 0) {
        DCHECK(fn->kind == CSS_TF_SCALE3D, "a computed scale is not its scale3d()");
        last = fn->number[2] != 1.0 ? 2 : fn->number[1] != fn->number[0] ? 1 : 0;
        for (k = 0; k <= last; k++) {
            if (k > 0) json_buf_raw(&b, " ");
            it_raw_owned(&b, css_length_serialize_number(fn->number[k] + 0.0, ""));
        }
    } else {
        /* "the keyword none, or an <angle> with an axis consisting of a list of three <number>s". */
        if (fn->kind == CSS_TF_ROTATE_X) json_buf_raw(&b, "x ");
        else if (fn->kind == CSS_TF_ROTATE_Y) json_buf_raw(&b, "y ");
        else if (fn->kind == CSS_TF_ROTATE_Z) json_buf_raw(&b, "z ");
        else if (fn->kind == CSS_TF_ROTATE3D)
            for (k = 0; k < 3; k++) {
                it_raw_owned(&b, css_length_serialize_number(fn->number[k] + 0.0, ""));
                json_buf_raw(&b, " ");
            }
        else DCHECK(fn->kind == CSS_TF_ROTATE, "a computed rotate is none of its four functions");
        it_raw_owned(&b, it_angle(fn));
    }
    out = json_buf_take(&b);
    CHECK(out != NULL, "css-transforms-2 §5: OOM serializing a computed individual transform");
    return out;
}
