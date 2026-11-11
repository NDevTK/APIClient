/* URI handling */

static int string_get_hex(JSString *p, int k, int n) {
    int c = 0, h;
    while (n-- > 0) {
        if ((h = from_hex(string_get(p, k++))) < 0)
            return -1;
        c = (c << 4) | h;
    }
    return c;
}

static int isURIReserved(int c) {
    return c < 0x100 && memchr(";/?:@&=+$,#", c, sizeof(";/?:@&=+$,#") - 1) != NULL;
}

static int JS_PRINTF_FORMAT_ATTR(2, 3) js_throw_URIError(JSContext *ctx, JS_PRINTF_FORMAT const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    JS_ThrowError(ctx, JS_URI_ERROR, fmt, ap);
    va_end(ap);
    return -1;
}

static int hex_decode(JSContext *ctx, JSString *p, int k) {
    int c;

    if (k >= p->len || string_get(p, k) != '%')
        return js_throw_URIError(ctx, "expecting %%");
    if (k + 2 >= p->len || (c = string_get_hex(p, k + 1, 2)) < 0)
        return js_throw_URIError(ctx, "expecting hex digit");

    return c;
}

static JSValue js_global_decodeURI(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv, int isComponent)
{
    JSValue str;
    StringBuffer b_s, *b = &b_s;
    JSString *p;
    int k, c, c1, n, c_min;

    {   /* THE CODEC RUNS, on the operand's own example. §solver models a spec codec faithfully and
           never enumerates or predicts it, so the derived unknown carries what this function
           REALLY produced from the concrete it had — re-entered here with the example in place of
           the operand, which is the same code path a page with a known value takes. */
        JSValue ex = js_concolic_operand(ctx, argv[0]);
        if (!JS_IsUninitialized(ex)) {
            JSValue real = JS_UNDEFINED;
            if (js_concolic_codec_operand(argv[0], ex)) { JSValueConst a2[1]; a2[0] = ex; real = js_global_decodeURI(ctx, this_val, 1, a2, isComponent); }
            JS_FreeValue(ctx, ex);
            return js_concolic_derive(ctx, argv[0],
                                      isComponent ? "decodeURIComponent" : "decodeURI", real);
        }
    }
    str = JS_ToString(ctx, argv[0]);
    if (JS_IsException(str))
        return str;

    string_buffer_init(ctx, b, 0);

    p = JS_VALUE_GET_STRING(str);
    for (k = 0; k < p->len;) {
        c = string_get(p, k);
        if (c == '%') {
            c = hex_decode(ctx, p, k);
            if (c < 0)
                goto fail;
            k += 3;
            if (c < 0x80) {
                if (!isComponent && isURIReserved(c)) {
                    c = '%';
                    k -= 2;
                }
            } else {
                /* Decode URI-encoded UTF-8 sequence */
                if (c >= 0xc0 && c <= 0xdf) {
                    n = 1;
                    c_min = 0x80;
                    c &= 0x1f;
                } else if (c >= 0xe0 && c <= 0xef) {
                    n = 2;
                    c_min = 0x800;
                    c &= 0xf;
                } else if (c >= 0xf0 && c <= 0xf7) {
                    n = 3;
                    c_min = 0x10000;
                    c &= 0x7;
                } else {
                    n = 0;
                    c_min = 1;
                    c = 0;
                }
                while (n-- > 0) {
                    c1 = hex_decode(ctx, p, k);
                    if (c1 < 0)
                        goto fail;
                    k += 3;
                    if ((c1 & 0xc0) != 0x80) {
                        c = 0;
                        break;
                    }
                    c = (c << 6) | (c1 & 0x3f);
                }
                if (c < c_min || c > 0x10FFFF || is_surrogate(c)) {
                    js_throw_URIError(ctx, "malformed UTF-8");
                    goto fail;
                }
            }
        } else {
            k++;
        }
        string_buffer_putc(b, c);
    }
    JS_FreeValue(ctx, str);
    return string_buffer_end(b);

fail:
    JS_FreeValue(ctx, str);
    string_buffer_free(b);
    return JS_EXCEPTION;
}

static int isUnescaped(int c) {
    static char const unescaped_chars[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz"
        "0123456789"
        "@*_+-./";
    return c < 0x100 &&
        memchr(unescaped_chars, c, sizeof(unescaped_chars) - 1);
}

static int isURIUnescaped(int c, int isComponent) {
    return c < 0x100 &&
        ((c >= 0x61 && c <= 0x7a) ||
         (c >= 0x41 && c <= 0x5a) ||
         (c >= 0x30 && c <= 0x39) ||
         memchr("-_.!~*'()", c, sizeof("-_.!~*'()") - 1) != NULL ||
         (!isComponent && isURIReserved(c)));
}

static int encodeURI_hex(StringBuffer *b, int c) {
    uint8_t buf[6];
    int n = 0;
    const char *hex = "0123456789ABCDEF";

    buf[n++] = '%';
    if (c >= 256) {
        buf[n++] = 'u';
        buf[n++] = hex[(c >> 12) & 15];
        buf[n++] = hex[(c >>  8) & 15];
    }
    buf[n++] = hex[(c >> 4) & 15];
    buf[n++] = hex[(c >> 0) & 15];
    return string_buffer_write8(b, buf, n);
}

/* §19.2.6.5 Encode's result over any string: each code unit is one unescapedSet holds (isURIUnescaped, this
   file's reading of the ASCII word characters, "-.!~*'()" and encodeURI's extraUnescaped), or one of an escape
   "%" followed by StringPad(hex, 2, "0", start) of an uppercase hexadecimal octet, whose digits are word
   characters. Stated on the unknown `r` through JSConcolicHooks.string_class. */
static void js_uri_encode_class(JSContext *ctx, JSValueConst r, int isComponent)
{
    uint32_t pairs[2 * 64];
    int n = 0, c;

    for (c = 0; c < 0x80; c++) {
        if (!isURIUnescaped(c, isComponent) && c != '%') continue;
        if (n > 0 && pairs[2 * n - 1] == (uint32_t)c - 1) { pairs[2 * n - 1] = (uint32_t)c; continue; }
        DCHECK(n < 64, "Encode's unescaped set split into more ranges than its ASCII span can hold");
        pairs[2 * n] = pairs[2 * n + 1] = (uint32_t)c;
        n++;
    }
    g_concolic.string_class(ctx, r, pairs, n);
}

static JSValue js_global_encodeURI(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv,
                                   int isComponent)
{
    JSValue str;
    StringBuffer b_s, *b = &b_s;
    JSString *p;
    int k, c, c1;

    {   /* THE CODEC RUNS, on the operand's own example. §solver models a spec codec faithfully and
           never enumerates or predicts it, so the derived unknown carries what this function
           REALLY produced from the concrete it had — re-entered here with the example in place of
           the operand, which is the same code path a page with a known value takes. */
        JSValue ex = js_concolic_operand(ctx, argv[0]);
        if (!JS_IsUninitialized(ex)) {
            JSValue real = JS_UNDEFINED;
            JSValue r;

            if (js_concolic_codec_operand(argv[0], ex)) { JSValueConst a2[1]; a2[0] = ex; real = js_global_encodeURI(ctx, this_val, 1, a2, isComponent); }
            JS_FreeValue(ctx, ex);
            r = js_concolic_derive(ctx, argv[0], isComponent ? "encodeURIComponent" : "encodeURI", real);
            if (g_concolic.is(r)) js_uri_encode_class(ctx, r, isComponent);
            return r;
        }
    }
    str = JS_ToString(ctx, argv[0]);
    if (JS_IsException(str))
        return str;

    p = JS_VALUE_GET_STRING(str);
    string_buffer_init(ctx, b, p->len);
    for (k = 0; k < p->len;) {
        c = string_get(p, k);
        k++;
        if (isURIUnescaped(c, isComponent)) {
            string_buffer_putc16(b, c);
        } else {
            if (is_lo_surrogate(c)) {
                js_throw_URIError(ctx, "invalid character");
                goto fail;
            } else if (is_hi_surrogate(c)) {
                if (k >= p->len) {
                    js_throw_URIError(ctx, "expecting surrogate pair");
                    goto fail;
                }
                c1 = string_get(p, k);
                k++;
                if (!is_lo_surrogate(c1)) {
                    js_throw_URIError(ctx, "expecting surrogate pair");
                    goto fail;
                }
                c = from_surrogate(c, c1);
            }
            if (c < 0x80) {
                encodeURI_hex(b, c);
            } else {
                /* XXX: use C UTF-8 conversion ? */
                if (c < 0x800) {
                    encodeURI_hex(b, (c >> 6) | 0xc0);
                } else {
                    if (c < 0x10000) {
                        encodeURI_hex(b, (c >> 12) | 0xe0);
                    } else {
                        encodeURI_hex(b, (c >> 18) | 0xf0);
                        encodeURI_hex(b, ((c >> 12) & 0x3f) | 0x80);
                    }
                    encodeURI_hex(b, ((c >> 6) & 0x3f) | 0x80);
                }
                encodeURI_hex(b, (c & 0x3f) | 0x80);
            }
        }
    }
    JS_FreeValue(ctx, str);
    return string_buffer_end(b);

fail:
    JS_FreeValue(ctx, str);
    string_buffer_free(b);
    return JS_EXCEPTION;
}

static JSValue js_global_escape(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    JSValue str;
    StringBuffer b_s, *b = &b_s;
    JSString *p;
    int i, len, c;

    {   /* THE CODEC RUNS, on the operand's own example. §solver models a spec codec faithfully and
           never enumerates or predicts it, so the derived unknown carries what this function
           REALLY produced from the concrete it had — re-entered here with the example in place of
           the operand, which is the same code path a page with a known value takes. */
        JSValue ex = js_concolic_operand(ctx, argv[0]);
        if (!JS_IsUninitialized(ex)) {
            JSValue real = JS_UNDEFINED;
            if (js_concolic_codec_operand(argv[0], ex)) { JSValueConst a2[1]; a2[0] = ex; real = js_global_escape(ctx, this_val, 1, a2); }
            JS_FreeValue(ctx, ex);
            return js_concolic_derive(ctx, argv[0], "escape", real);
        }
    }
    str = JS_ToString(ctx, argv[0]);
    if (JS_IsException(str))
        return str;

    p = JS_VALUE_GET_STRING(str);
    string_buffer_init(ctx, b, p->len);
    for (i = 0, len = p->len; i < len; i++) {
        c = string_get(p, i);
        if (isUnescaped(c)) {
            string_buffer_putc16(b, c);
        } else {
            encodeURI_hex(b, c);
        }
    }
    JS_FreeValue(ctx, str);
    return string_buffer_end(b);
}

static JSValue js_global_unescape(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    JSValue str;
    StringBuffer b_s, *b = &b_s;
    JSString *p;
    int i, len, c, n;

    {   /* THE CODEC RUNS, on the operand's own example. §solver models a spec codec faithfully and
           never enumerates or predicts it, so the derived unknown carries what this function
           REALLY produced from the concrete it had — re-entered here with the example in place of
           the operand, which is the same code path a page with a known value takes. */
        JSValue ex = js_concolic_operand(ctx, argv[0]);
        if (!JS_IsUninitialized(ex)) {
            JSValue real = JS_UNDEFINED;
            if (js_concolic_codec_operand(argv[0], ex)) { JSValueConst a2[1]; a2[0] = ex; real = js_global_unescape(ctx, this_val, 1, a2); }
            JS_FreeValue(ctx, ex);
            return js_concolic_derive(ctx, argv[0], "unescape", real);
        }
    }
    str = JS_ToString(ctx, argv[0]);
    if (JS_IsException(str))
        return str;

    string_buffer_init(ctx, b, 0);
    p = JS_VALUE_GET_STRING(str);
    for (i = 0, len = p->len; i < len; i++) {
        c = string_get(p, i);
        if (c == '%') {
            if (i + 6 <= len
            &&  string_get(p, i + 1) == 'u'
            &&  (n = string_get_hex(p, i + 2, 4)) >= 0) {
                c = n;
                i += 6 - 1;
            } else
            if (i + 3 <= len
            &&  (n = string_get_hex(p, i + 1, 2)) >= 0) {
                c = n;
                i += 3 - 1;
            }
        }
        string_buffer_putc16(b, c);
    }
    JS_FreeValue(ctx, str);
    return string_buffer_end(b);
}

