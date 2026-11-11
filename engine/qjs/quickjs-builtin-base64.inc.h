/* base64 */

static const unsigned char b64_enc[64] = {
    'A','B','C','D','E','F','G','H','I','J','K','L','M','N','O','P',
    'Q','R','S','T','U','V','W','X','Y','Z',
    'a','b','c','d','e','f','g','h','i','j','k','l','m','n','o','p',
    'q','r','s','t','u','v','w','x','y','z',
    '0','1','2','3','4','5','6','7','8','9',
    '+','/'
};

static const unsigned char b64url_enc[64] = {
    'A','B','C','D','E','F','G','H','I','J','K','L','M','N','O','P',
    'Q','R','S','T','U','V','W','X','Y','Z',
    'a','b','c','d','e','f','g','h','i','j','k','l','m','n','o','p',
    'q','r','s','t','u','v','w','x','y','z',
    '0','1','2','3','4','5','6','7','8','9',
    '-','_'
};

enum { K_VAL = 1u, K_WS = 2u };

static const uint8_t b64_val[256] = {
    ['A']=0, ['B']=1, ['C']=2, ['D']=3, ['E']=4, ['F']=5, ['G']=6, ['H']=7,
    ['I']=8, ['J']=9, ['K']=10,['L']=11,['M']=12,['N']=13,['O']=14,['P']=15,
    ['Q']=16,['R']=17,['S']=18,['T']=19,['U']=20,['V']=21,['W']=22,['X']=23,['Y']=24,['Z']=25,
    ['a']=26,['b']=27,['c']=28,['d']=29,['e']=30,['f']=31,['g']=32,['h']=33,
    ['i']=34,['j']=35,['k']=36,['l']=37,['m']=38,['n']=39,['o']=40,['p']=41,
    ['q']=42,['r']=43,['s']=44,['t']=45,['u']=46,['v']=47,['w']=48,['x']=49,['y']=50,['z']=51,
    ['0']=52,['1']=53,['2']=54,['3']=55,['4']=56,['5']=57,['6']=58,['7']=59,['8']=60,['9']=61,
    ['+']=62, ['/']=63,
    ['-']=62, ['_']=63,
};

static const char b64_flags[256] = {
    [' ']=K_WS, ['\t']=K_WS, ['\n']=K_WS, ['\f']=K_WS, ['\r']=K_WS,
    ['A']=K_VAL,['B']=K_VAL,['C']=K_VAL,['D']=K_VAL,['E']=K_VAL,['F']=K_VAL,['G']=K_VAL,['H']=K_VAL,
    ['I']=K_VAL,['J']=K_VAL,['K']=K_VAL,['L']=K_VAL,['M']=K_VAL,['N']=K_VAL,['O']=K_VAL,['P']=K_VAL,
    ['Q']=K_VAL,['R']=K_VAL,['S']=K_VAL,['T']=K_VAL,['U']=K_VAL,['V']=K_VAL,['W']=K_VAL,['X']=K_VAL,
    ['Y']=K_VAL,['Z']=K_VAL,
    ['a']=K_VAL,['b']=K_VAL,['c']=K_VAL,['d']=K_VAL,['e']=K_VAL,['f']=K_VAL,['g']=K_VAL,['h']=K_VAL,
    ['i']=K_VAL,['j']=K_VAL,['k']=K_VAL,['l']=K_VAL,['m']=K_VAL,['n']=K_VAL,['o']=K_VAL,['p']=K_VAL,
    ['q']=K_VAL,['r']=K_VAL,['s']=K_VAL,['t']=K_VAL,['u']=K_VAL,['v']=K_VAL,['w']=K_VAL,['x']=K_VAL,
    ['y']=K_VAL,['z']=K_VAL,
    ['0']=K_VAL,['1']=K_VAL,['2']=K_VAL,['3']=K_VAL,['4']=K_VAL,['5']=K_VAL,['6']=K_VAL,['7']=K_VAL,
    ['8']=K_VAL,['9']=K_VAL,
    ['+']=K_VAL,['/']=K_VAL,
};

static const char b64_flags_url[256] = {
    [' ']=K_WS, ['\t']=K_WS, ['\n']=K_WS, ['\f']=K_WS, ['\r']=K_WS,
    ['A']=K_VAL,['B']=K_VAL,['C']=K_VAL,['D']=K_VAL,['E']=K_VAL,['F']=K_VAL,['G']=K_VAL,['H']=K_VAL,
    ['I']=K_VAL,['J']=K_VAL,['K']=K_VAL,['L']=K_VAL,['M']=K_VAL,['N']=K_VAL,['O']=K_VAL,['P']=K_VAL,
    ['Q']=K_VAL,['R']=K_VAL,['S']=K_VAL,['T']=K_VAL,['U']=K_VAL,['V']=K_VAL,['W']=K_VAL,['X']=K_VAL,
    ['Y']=K_VAL,['Z']=K_VAL,
    ['a']=K_VAL,['b']=K_VAL,['c']=K_VAL,['d']=K_VAL,['e']=K_VAL,['f']=K_VAL,['g']=K_VAL,['h']=K_VAL,
    ['i']=K_VAL,['j']=K_VAL,['k']=K_VAL,['l']=K_VAL,['m']=K_VAL,['n']=K_VAL,['o']=K_VAL,['p']=K_VAL,
    ['q']=K_VAL,['r']=K_VAL,['s']=K_VAL,['t']=K_VAL,['u']=K_VAL,['v']=K_VAL,['w']=K_VAL,['x']=K_VAL,
    ['y']=K_VAL,['z']=K_VAL,
    ['0']=K_VAL,['1']=K_VAL,['2']=K_VAL,['3']=K_VAL,['4']=K_VAL,['5']=K_VAL,['6']=K_VAL,['7']=K_VAL,
    ['8']=K_VAL,['9']=K_VAL,
    ['-']=K_VAL,['_']=K_VAL,
};

static size_t b64_encode(const uint8_t *src, size_t len, char *dst,
                         const unsigned char *alpha)
{
    size_t i = 0, j = 0;
    size_t main_len = (len / 3) * 3;

    for (; i < main_len; i += 3, j += 4) {
        uint32_t v = 65536*src[i] + 256*src[i + 1] + src[i + 2];
        dst[j + 0] = alpha[(v >> 18) & 63];
        dst[j + 1] = alpha[(v >> 12) & 63];
        dst[j + 2] = alpha[(v >> 6) & 63];
        dst[j + 3] = alpha[v & 63];
    }

    size_t rem = len - i;
    if (rem == 1) {
        uint32_t v = 65536*src[i];
        dst[j++] = alpha[(v >> 18) & 63];
        dst[j++] = alpha[(v >> 12) & 63];
        dst[j++] = '=';
        dst[j++] = '=';
    } else if (rem == 2) {
        uint32_t v = 65536*src[i] + 256*src[i + 1];
        dst[j++] = alpha[(v >> 18) & 63];
        dst[j++] = alpha[(v >> 12) & 63];
        dst[j++] = alpha[(v >> 6) & 63];
        dst[j++] = '=';
    }
    return j;
}

/* THE STANDARD BASE64 ENCODER, EXPORTED — because the HOST NEEDS ONE and this is the engine that has it.
   A cross-instance message is bytes, the notice channel that carries it to the trusted zone is TEXT, and the
   choice was between binding the codec that already exists here and hand-rolling a second one in the host.
   `dst_size` is checked rather than trusted: the caller sizes with JS_Base64EncodedSize and a mismatch between
   the two is the one way this can corrupt a message, so it fails loudly instead of writing past the buffer.
   Returns the number of characters written, or 0 when `dst` is too small. */
size_t JS_Base64EncodedSize(size_t len)
{
    return ((len + 2) / 3) * 4;
}

size_t JS_Base64Encode(char *dst, size_t dst_size, const uint8_t *src, size_t len)
{
    if (dst_size < JS_Base64EncodedSize(len))
        return 0;
    return b64_encode(src, len, dst, b64_enc);
}

/* Implements https://infra.spec.whatwg.org/#forgiving-base64-decode */
static size_t
b64_decode(const char *src, size_t len, uint8_t *dst, int *err)
{
    size_t i, j;
    uint32_t acc;
    int seen, pad;
    unsigned ch;

    acc = 0;
    seen = 0;
    for (i = 0, j = 0; i < len; i++) {
        ch = (unsigned char)src[i];
        if ((b64_flags[ch] & K_WS))
            continue;
        if (!(b64_flags[ch] & K_VAL))
            break;
        acc = (acc << 6) | b64_val[ch];
        seen++;
        if (seen == 4) {
            dst[j++] = (acc >> 16) & 0xFF;
            dst[j++] = (acc >> 8) & 0xFF;
            dst[j++] = acc & 0xFF;
            seen = 0;
            acc = 0;
        }
    }

    if (seen != 0) {
        if (seen == 3) {
            dst[j++] = (acc >> 10) & 0xFF;
            dst[j++] = (acc >> 2) & 0xFF;
        } else if (seen == 2) {
            dst[j++] = (acc >> 4) & 0xFF;
        } else {
            *err = 1;
            return 0;
        }
        for (pad = 0; i < len; i++) {
            ch = (unsigned char)src[i];
            if (pad < 2 && ch == '=')
                pad++;
            else if (!(b64_flags[ch] & K_WS))
                break;
        }
        if (pad != 0 && seen + pad != 4) {
            *err = 1;
            return 0;
        }
    }

    *err = i < len;
    return j;
}

/* THE DECODER HALF, EXPORTED for the same reason the encoder is: a message that crossed a text channel has to
   come back to bytes on the other side, and the alternative was a second implementation of a codec this engine
   already has. `err` is the forgiving-base64 error the WHATWG algorithm reports; a caller that gets one has
   been handed something that is not what this engine encoded, which is a corrupted transport rather than a
   page's mistake. Returns the number of BYTES written; `dst` must hold at least JS_Base64DecodedMax(len). */
size_t JS_Base64DecodedMax(size_t len)
{
    return (len / 4 + 1) * 3;
}

size_t JS_Base64Decode(uint8_t *dst, size_t dst_size, const char *src, size_t len, int *err)
{
    if (dst_size < JS_Base64DecodedMax(len)) {
        if (err) *err = -1;
        return 0;
    }
    return b64_decode(src, len, dst, err);
}

static JSValue js_btoa(JSContext *ctx, JSValueConst this_val,
                       int argc, JSValueConst *argv)
{
    const uint8_t *in8;
    uint8_t *tmp = NULL;
    uint8_t *outp;
    JSValue val, ret = JS_EXCEPTION;
    JSString *s, *ostr;
    size_t len, out_len, written;

    {   /* THE CODEC RUNS, on the operand's own example — the twin of atob's rule and the same re-entry. */
        JSValue ex = js_concolic_operand(ctx, argv[0]);
        if (!JS_IsUninitialized(ex)) {
            JSValue real = JS_UNDEFINED;
            if (js_concolic_codec_operand(argv[0], ex)) { JSValueConst a2[1]; a2[0] = ex; real = js_btoa(ctx, this_val, 1, a2); }
            JS_FreeValue(ctx, ex);
            return js_concolic_derive(ctx, argv[0], "btoa", real);
        }
    }
    DCHECK(argc < 1 || JS_VALUE_GET_TAG(argv[0]) != JS_TAG_OBJECT,
           "btoa's body was handed an object — its machine coerces `data` to a primitive before the body runs");
    val = JS_ToString(ctx, argv[0]);
    if (unlikely(JS_IsException(val)))
        return JS_EXCEPTION;

    s = JS_VALUE_GET_STRING(val);
    len = (size_t)s->len;

    if (likely(!s->is_wide_char)) {
        in8 = (const uint8_t *)str8(s);
    } else {
        const uint16_t *src = str16(s);
        tmp = js_malloc(ctx, likely(len) ? len : 1);
        if (unlikely(!tmp))
            goto fail;
        for (size_t i = 0; i < len; i++) {
            uint32_t c = src[i];
            if (unlikely(c > 0xFF)) {
                JS_ThrowDOMException(ctx, "InvalidCharacterError",
                                     "String contains an invalid character");
                goto fail;
            }
            tmp[i] = (uint8_t)c;
        }
        in8 = tmp;
    }

    if (unlikely(len > (SIZE_MAX - 2) / 3)) {
        JS_ThrowRangeError(ctx, "input too large");
        goto fail;
    }
    out_len = 4 * ((len + 2) / 3);
    if (unlikely(out_len > JS_STRING_LEN_MAX)) {
        JS_ThrowRangeError(ctx, "output too large");
        goto fail;
    }

    ostr = js_alloc_string(ctx, out_len, 0);
    if (unlikely(!ostr))
        goto fail;

    outp = str8(ostr);
    written = b64_encode(in8, len, (char *)outp, b64_enc);
    outp[written] = '\0';
    ostr->len = out_len;
    ret = JS_MKPTR(JS_TAG_STRING, ostr);
fail:
    if (tmp)
        js_free(ctx, tmp);
    JS_FreeValue(ctx, val);
    return ret;
}

static JSValue js_atob(JSContext *ctx, JSValueConst this_val,
                       int argc, JSValueConst *argv)
{
    const uint8_t *in;
    uint8_t *tmp = NULL, *outp;
    JSValue val, ret = JS_EXCEPTION;
    JSString *s, *ostr;
    size_t slen, out_cap, out_len;
    int err;

    {   /* THE CODEC RUNS, on the operand's own example — re-entered with it in place of the operand, the
           same path a page with a known value takes, so the derived unknown carries a real decode. */
        JSValue ex = js_concolic_operand(ctx, argv[0]);
        if (!JS_IsUninitialized(ex)) {
            JSValue real = JS_UNDEFINED;
            if (js_concolic_codec_operand(argv[0], ex)) { JSValueConst a2[1]; a2[0] = ex; real = js_atob(ctx, this_val, 1, a2); }
            JS_FreeValue(ctx, ex);
            return js_concolic_derive(ctx, argv[0], "atob", real);
        }
    }
    DCHECK(argc < 1 || JS_VALUE_GET_TAG(argv[0]) != JS_TAG_OBJECT,
           "atob's body was handed an object — its machine coerces `data` to a primitive before the body runs");
    val = JS_ToString(ctx, argv[0]);
    if (unlikely(JS_IsException(val)))
        return JS_EXCEPTION;

    s = JS_VALUE_GET_STRING(val);
    slen = (size_t)s->len;

    if (likely(!s->is_wide_char)) {
        const uint8_t *p = (const uint8_t *)str8(s);
        for (size_t i = 0; i < slen; i++) {
            if (unlikely(p[i] & 0x80)) {
                JS_ThrowDOMException(ctx, "InvalidCharacterError",
                    "The string to be decoded is not correctly encoded");
                goto fail;
            }
        }
        in = p;
    } else {
        const uint16_t *src = str16(s);
        tmp = js_malloc(ctx, likely(slen) ? slen : 1);
        if (unlikely(!tmp))
            goto fail;
        for (size_t i = 0; i < slen; i++) {
            if (unlikely(src[i] > 0x7F)) {
                JS_ThrowDOMException(ctx, "InvalidCharacterError",
                    "The string to be decoded is not correctly encoded");
                goto fail;
            }
            tmp[i] = (uint8_t)src[i];
        }
        in = tmp;
    }

    if (unlikely(slen > (SIZE_MAX / 3) * 4)) {
        JS_ThrowRangeError(ctx, "input too large");
        goto fail;
    }
    out_cap = (slen / 4) * 3 + 3;
    if (unlikely(out_cap > JS_STRING_LEN_MAX)) {
        JS_ThrowRangeError(ctx, "output too large");
        goto fail;
    }

    ostr = js_alloc_string(ctx, out_cap, 0);
    if (unlikely(!ostr))
        goto fail;

    outp = str8(ostr);
    err = 0;
    out_len = b64_decode((const char *)in, slen, outp, &err);

    if (unlikely(err)) {
        js_free_string(ctx->rt, ostr);
        JS_ThrowDOMException(ctx, "InvalidCharacterError",
            "The string to be decoded is not correctly encoded");
        goto fail;
    }
    outp[out_len] = '\0';
    ostr->len = out_len;
    ret = JS_MKPTR(JS_TAG_STRING, ostr);
fail:
    if (tmp)
        js_free(ctx, tmp);
    JS_FreeValue(ctx, val);
    return ret;
}

static const JSCFunctionListEntry js_base64_funcs[] = {
    JS_CFUNC_STEP_DEF("btoa", 1, STEPDEF_BTOA ),
    JS_CFUNC_STEP_DEF("atob", 1, STEPDEF_ATOB ),
};


/* Uint8Array base64/hex (tc39 proposal-arraybuffer-base64) */

enum {
    B64_ALPHABET_BASE64 = 0,
    B64_ALPHABET_BASE64URL = 1,
};

enum {
    B64_LAST_LOOSE = 0,
    B64_LAST_STRICT = 1,
    B64_LAST_STOP_BEFORE_PARTIAL = 2,
};


static size_t b64_skip_ws(const char *src, size_t len, size_t index,
                          const char *flags)
{
    while (index < len && (flags[(unsigned char)src[index]] & K_WS))
        index++;
    return index;
}

/* Implements the FromBase64 abstract operation.
   src/src_len: the input string (must be ASCII/latin1)
   dst/max_len: output buffer
   flags: b64_flags or b64_flags_url (selects valid characters)
   last_chunk: B64_LAST_LOOSE, B64_LAST_STRICT, or B64_LAST_STOP_BEFORE_PARTIAL
   *p_read: set to number of input characters consumed
   *p_err: set to 1 on error, 0 on success
   Returns: number of bytes written to dst */
static size_t from_base64(const char *src, size_t src_len,
                          uint8_t *dst, size_t max_len,
                          const char *flags, int last_chunk,
                          size_t *p_read, int *p_err)
{
    size_t read = 0, written = 0;
    uint32_t acc = 0;
    int seen = 0;
    size_t index = 0;

    *p_err = 0;

    if (max_len == 0) {
        *p_read = 0;
        return 0;
    }

    /* Fast path: decode complete groups of 4 valid characters.
       Breaks out on whitespace, padding, invalid chars, or capacity. */
    while (index + 4 <= src_len && written + 3 <= max_len) {
        uint8_t f = flags[(unsigned char)src[index]]
                  & flags[(unsigned char)src[index + 1]]
                  & flags[(unsigned char)src[index + 2]]
                  & flags[(unsigned char)src[index + 3]];
        if (!(f & K_VAL))
            break;
        uint32_t v = ((uint32_t)b64_val[(unsigned char)src[index]] << 18)
                   | ((uint32_t)b64_val[(unsigned char)src[index + 1]] << 12)
                   | ((uint32_t)b64_val[(unsigned char)src[index + 2]] << 6)
                   | (uint32_t)b64_val[(unsigned char)src[index + 3]];
        dst[written]     = (uint8_t)(v >> 16);
        dst[written + 1] = (uint8_t)(v >> 8);
        dst[written + 2] = (uint8_t)(v);
        written += 3;
        index += 4;
    }
    read = index;

    if (written >= max_len) {
        *p_read = read;
        return written;
    }

    /* Slow path: handle whitespace, padding, partial groups, capacity. */
    for (;;) {
        index = b64_skip_ws(src, src_len, index, flags);

        if (index == src_len) {
            if (seen > 0) {
                if (last_chunk == B64_LAST_STOP_BEFORE_PARTIAL) {
                    *p_read = read;
                    return written;
                }
                if (last_chunk == B64_LAST_STRICT) {
                    *p_err = 1;
                    return 0;
                }
                /* loose */
                if (seen == 1) {
                    *p_err = 1;
                    return 0;
                }
                goto decode_partial;
            }
            *p_read = src_len;
            return written;
        }

        unsigned char ch = src[index++];

        if (ch == '=') {
            if (seen < 2) {
                *p_err = 1;
                return 0;
            }
            index = b64_skip_ws(src, src_len, index, flags);
            if (seen == 2) {
                if (index == src_len) {
                    if (last_chunk == B64_LAST_STOP_BEFORE_PARTIAL) {
                        *p_read = read;
                        return written;
                    }
                    *p_err = 1;
                    return 0;
                }
                if (src[index] == '=') {
                    index++;
                    index = b64_skip_ws(src, src_len, index, flags);
                } else {
                    *p_err = 1;
                    return 0;
                }
            }
            /* After padding, only whitespace is allowed */
            if (index != src_len) {
                *p_err = 1;
                return 0;
            }
            if (last_chunk == B64_LAST_STRICT) {
                uint32_t mask = (seen == 2) ? 0xF : 0x3;
                if (acc & mask) {
                    *p_err = 1;
                    return 0;
                }
            }
            goto decode_partial;
        }

        if (!(flags[ch] & K_VAL)) {
            *p_err = 1;
            return 0;
        }

        /* Check remaining capacity before committing to this group */
        {
            size_t remaining = max_len - written;
            if ((remaining == 1 && seen == 2) ||
                    (remaining == 2 && seen == 3)) {
                *p_read = read;
                return written;
            }
        }

        acc = (acc << 6) | b64_val[ch];
        seen++;

        if (seen == 4) {
            dst[written]     = (uint8_t)(acc >> 16);
            dst[written + 1] = (uint8_t)(acc >> 8);
            dst[written + 2] = (uint8_t)(acc);
            written += 3;
            acc = 0;
            seen = 0;
            read = index;
            if (written >= max_len) {
                *p_read = read;
                return written;
            }
        }
    }

decode_partial:
    if (seen == 2) {
        dst[written++] = (uint8_t)(acc >> 4);
    } else if (seen == 3) {
        dst[written]     = (uint8_t)(acc >> 10);
        dst[written + 1] = (uint8_t)(acc >> 2);
        written += 2;
    }
    *p_read = src_len;
    return written;
}

/* Hex helpers */
static const char u8a_hex_digits[] = "0123456789abcdef";

static int u8a_hex_nibble(unsigned char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static size_t u8a_hex_encode(const uint8_t *src, size_t len, char *dst)
{
    for (size_t i = 0; i < len; i++) {
        dst[i * 2]     = u8a_hex_digits[src[i] >> 4];
        dst[i * 2 + 1] = u8a_hex_digits[src[i] & 0xF];
    }
    return len * 2;
}

/* Decode hex string to bytes.
   Returns bytes written. Sets *p_read to chars consumed, *p_err on error. */
static size_t u8a_hex_decode(const char *src, size_t src_len,
                             uint8_t *dst, size_t max_len,
                             size_t *p_read, int *p_err)
{
    size_t written = 0, i = 0;
    *p_err = 0;

    if (src_len & 1) {
        *p_err = 1;
        return 0;
    }

    while (i < src_len && written < max_len) {
        int hi = u8a_hex_nibble(src[i]);
        int lo = u8a_hex_nibble(src[i + 1]);
        if (hi < 0 || lo < 0) {
            *p_err = 1;
            return 0;
        }
        dst[written++] = (uint8_t)((hi << 4) | lo);
        i += 2;
    }

    *p_read = i;
    return written;
}

/* Validate that this_val is a Uint8Array (type check only, no detach check).
   Returns the JSObject pointer or NULL on error (throws). */
static JSObject *check_uint8array(JSContext *ctx, JSValueConst this_val)
{
    JSObject *p;

    if (JS_VALUE_GET_TAG(this_val) != JS_TAG_OBJECT)
        goto fail;
    p = JS_VALUE_GET_OBJ(this_val);
    if (p->class_id != JS_CLASS_UINT8_ARRAY)
        goto fail;
    return p;
fail:
    JS_ThrowTypeError(ctx, "not a Uint8Array");
    return NULL;
}

/* Get the data pointer and length of a Uint8Array, checking for detached
   buffers. Must be called after options are read (per spec ordering).
   Returns 0 on success, -1 on error (throws). */
static int get_uint8array_bytes(JSContext *ctx, JSObject *p,
                                uint8_t **pdata, size_t *plen)
{
    if (typed_array_is_oob(p)) {
        JS_ThrowTypeErrorArrayBufferOOB(ctx);
        return -1;
    }
    *pdata = p->u.array.u.uint8_ptr;
    *plen = p->u.array.count;
    return 0;
}

/* DELETED: parse_alphabet_option and parse_last_chunk_option. Both performed 27.x's GetOption with
   JS_GetPropertyStr from C, and an options BAG is the page's object — `{ get alphabet() { … } }` or a Proxy makes
   each read the page's code with no flow base. They are step_strop_run calls on the machines below, which is one
   sub-sequence rather than two hand-rolled copies of the same read-and-match.

   THE ALLOWED VALUES ARE THE SPEC'S LISTS, and the index IS the mode, which is why the enums below are ordered to
   match rather than mapped. */
static const char *const b64_alphabet_names[] = { "base64", "base64url", NULL };
static const char *const b64_lastchunk_names[] = { "loose", "strict", "stop-before-partial", NULL };

/* The base64 family as STEP MACHINES. Each reads one or two options — the only page-visible steps it has — and
   everything after them (the buffer access, the codec, the result) invokes nothing. The spec ORDER is preserved:
   the options are read before the buffer is touched, which is what the deleted helpers' call sites spelled out
   and what the machines' stages now carry. */
typedef struct JSB64Op {
    JSStepHdr hdr;       /* MUST be first */
    JSValue result;      /* owned */
    JSValueConst options;/* borrowed from the invocation the header holds */
    int alphabet;
    int last_chunk;      /* also carries omitPadding for toBase64 */
} JSB64Op;
_Static_assert(offsetof(JSB64Op, hdr) == 0, "JSStepHdr must be first in JSB64Op");

enum { B64OP_TO = 0, B64OP_FROM, B64OP_SET_FROM };
static JSValue js_b64op_finish(JSContext *ctx, JSB64Op *s);
static JSValue js_make_read_written(JSContext *ctx, size_t read, size_t written);
/* The VALIDATE stage is the prologue, and it is a stage of its own so a resumption after a parked read cannot
   land on it. It did once — B64ST_ALPHABET was 0, so the re-entry re-ran the prologue, whose first act is to FREE
   cb_result, and the option's value was destroyed before the sub-sequence could be handed it. Every option
   silently read as undefined and took its fallback; the two-phase DCHECK cannot see this, because the read
   WAS answered at the site that parked it, just with a value that had been thrown away. */
/* THREE algorithms, one machine. The standard has no section number for them yet — the ArrayBuffer-base64
   proposal is not numbered in the published edition — so the algorithm is named and its steps are the
   proposal's own. A number invented here would be a claim about the standard rather than a reference to it.
   ONE stage list expanded once per algorithm; see AFIND_STAGES. */
#define B64OP_STAGES(X, VALIDATE, ALPHABET, SECOND, DONE) \
    X(B64ST_VALIDATE, VALIDATE) \
    X(B64ST_ALPHABET, ALPHABET) \
    X(B64ST_SECOND,   SECOND)   \
    X(B64ST_DONE,     DONE)
enum { B64OP_STAGES(JS_STEP_STAGE_ENUM, 0, 0, 0, 0) };
static const char *const js_u8_tobase64_steps[] = {
    B64OP_STAGES(JS_STEP_STAGE_LABEL,
        "Uint8Array.prototype.toBase64 steps 1-3 (ValidateUint8Array(O); opts is GetOptionsObject(options))",
        "Uint8Array.prototype.toBase64 step 4 (alphabet is GetOption(opts, \"alphabet\", string))",
        "Uint8Array.prototype.toBase64 step 5 (omitPadding is ToBoolean(Get(opts, \"omitPadding\")))",
        "Uint8Array.prototype.toBase64 steps 6-8 (toEncode is GetUint8ArrayBytes(O); the encoded String)")
    NULL };
static const char *const js_u8_frombase64_steps[] = {
    B64OP_STAGES(JS_STEP_STAGE_LABEL,
        "Uint8Array.fromBase64 steps 1-2 (string is a String; opts is GetOptionsObject(options))",
        "Uint8Array.fromBase64 step 3 (alphabet is GetOption(opts, \"alphabet\", string))",
        "Uint8Array.fromBase64 step 4 (lastChunkHandling is GetOption(opts, \"lastChunkHandling\", string))",
        "Uint8Array.fromBase64 steps 5-7 (FromBase64(string, alphabet, lastChunkHandling); the new Uint8Array)")
    NULL };
static const char *const js_u8_setfrombase64_steps[] = {
    B64OP_STAGES(JS_STEP_STAGE_LABEL,
        "Uint8Array.prototype.setFromBase64 steps 1-4 (ValidateUint8Array(into); string is a String; opts)",
        "Uint8Array.prototype.setFromBase64 step 5 (alphabet is GetOption(opts, \"alphabet\", string))",
        "Uint8Array.prototype.setFromBase64 step 6 (lastChunkHandling is GetOption(opts, \"lastChunkHandling\", string))",
        "Uint8Array.prototype.setFromBase64 steps 7-11 (the bytes are written into `into`; {read, written})")
    NULL };

static int js_b64op_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSB64Op *s = st;
    int which = s->hdr.arg;
    int r;

    if (s->hdr.stage == B64ST_VALIDATE) {
        JSValueConst opt_arg;
        int optidx = (which == B64OP_TO) ? 0 : 1;
        JS_FreeValue(ctx, cb_result); cb_result = JS_UNDEFINED;
        s->result = JS_UNDEFINED;
        /* the RECEIVER and ARGUMENT validations the spec performs before any option read. */
        if (which != B64OP_FROM) {
            JSObject *p = check_uint8array(ctx, s->hdr.this_val);
            if (!p) return -1;
            if (which == B64OP_SET_FROM && typed_array_is_immutable(p))
                return (JS_ThrowTypeErrorImmutableArrayBuffer(ctx), -1);
        }
        if (which != B64OP_TO && !JS_IsString(step_arg(&s->hdr, 0)))
            return (JS_ThrowTypeError(ctx, "expected string"), -1);
        opt_arg = (s->hdr.argc > optidx) ? step_arg(&s->hdr, optidx) : JS_UNDEFINED;
        if (!JS_IsUndefined(opt_arg) && !JS_IsObject(opt_arg))
            return (JS_ThrowTypeError(ctx, "options must be an object"), -1);
        s->options = opt_arg;           /* borrowed: the header owns the invocation for the machine's life */
        s->hdr.stage = B64ST_ALPHABET;
    }
    if (s->hdr.stage == B64ST_ALPHABET) {
        r = step_strop_run(ctx, &s->hdr, s->options, JS_ATOM_alphabet, b64_alphabet_names,
                           B64_ALPHABET_BASE64, cb_result, &s->alphabet, out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r) return r;
        s->hdr.stage = B64ST_SECOND;
    }
    if (s->hdr.stage == B64ST_SECOND) {
        if (which == B64OP_TO) {
            /* omitPadding is ToBoolean, not an enumerated string, so it is the plain read. */
            JSValue v;
            if (JS_IsUndefined(s->options)) { s->last_chunk = 0; JS_FreeValue(ctx, cb_result); }
            else {
                r = step_getprop_run(ctx, &s->hdr, s->options, JS_ATOM_omitPadding, cb_result, &v,
                                     out_cb, out_argc);
                cb_result = JS_UNDEFINED;
                if (r) return r;
                s->last_chunk = JS_ToBool(ctx, v);
                JS_FreeValue(ctx, v);
            }
        } else {
            r = step_strop_run(ctx, &s->hdr, s->options, JS_ATOM_lastChunkHandling, b64_lastchunk_names,
                               B64_LAST_LOOSE, cb_result, &s->last_chunk, out_cb, out_argc);
            cb_result = JS_UNDEFINED;
            if (r) return r;
        }
        s->hdr.stage = B64ST_DONE;
    }
    DCHECK(s->hdr.stage == B64ST_DONE, "a base64 operation resumed in an unknown stage");
    JS_FreeValue(ctx, cb_result);
    s->result = js_b64op_finish(ctx, s);
    return JS_IsException(s->result) ? (s->result = JS_UNDEFINED, -1) : 0;
}

/* WHAT THIS MACHINE OWNS (JSTrampStepDef.visit). Only the result: `options` is a borrowed view of the
   invocation the header holds, which is what its JSValueConst says. */
static void js_b64op_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSB64Op *s = st;
    v->val(ctx, &s->result);
}

static JSValue js_b64op_fini(JSContext *ctx, void *st, bool take_result)
{
    JSB64Op *s = st;
    JSValue r = take_result ? s->result : JS_UNDEFINED;
    if (take_result) s->result = JS_UNDEFINED;
    return r;
}

static const JSTrampStepDef js_u8_tobase64_def =
    { sizeof(JSB64Op), js_b64op_step, js_b64op_fini, B64OP_TO, .visit = js_b64op_visit,
        .algorithm = "Uint8Array.prototype.toBase64 ( [ options ] )",
        .steps = js_u8_tobase64_steps };
static const JSTrampStepDef js_u8_frombase64_def =
    { sizeof(JSB64Op), js_b64op_step, js_b64op_fini, B64OP_FROM, .visit = js_b64op_visit,
        .algorithm = "Uint8Array.fromBase64 ( string [ , options ] )",
        .steps = js_u8_frombase64_steps };
static const JSTrampStepDef js_u8_setfrombase64_def =
    { sizeof(JSB64Op), js_b64op_step, js_b64op_fini, B64OP_SET_FROM, .visit = js_b64op_visit,
        .algorithm = "Uint8Array.prototype.setFromBase64 ( string [ , options ] )",
        .steps = js_u8_setfrombase64_steps };

/* Everything AFTER the options: the buffer access, the codec and the result. Not one step of it is the page's
   code, which is why it is a plain C tail and only the reads above became requests. One function for all three
   operations because the receiver checks, the option order and the OOB re-check are shared and splitting them
   was how the C bodies came to disagree about when the buffer is read. */
static JSValue js_b64op_finish(JSContext *ctx, JSB64Op *s)
{
    int which = s->hdr.arg;
    uint8_t *data;
    size_t len, str_len, read_pos, decoded_len;
    const char *str;
    int err;

    if (which == B64OP_TO) {
        JSObject *p = check_uint8array(ctx, s->hdr.this_val);
        size_t out_len, written;
        JSString *ostr;
        char *dst;
        DCHECK(p != NULL, "toBase64's receiver was validated at stage 0 and cannot have changed");
        /* the OOB re-check belongs HERE: an option getter is the page's code and can detach the buffer. */
        if (get_uint8array_bytes(ctx, p, &data, &len))
            return JS_EXCEPTION;
        out_len = 4 * ((len + 2) / 3);
        if (unlikely(out_len > JS_STRING_LEN_MAX))
            return JS_ThrowRangeError(ctx, "output too large");
        ostr = js_alloc_string(ctx, out_len, 0);
        if (!ostr)
            return JS_EXCEPTION;
        dst = (char *)str8(ostr);
        written = b64_encode(data, len, dst,
                             s->alphabet == B64_ALPHABET_BASE64URL ? b64url_enc : b64_enc);
        if (s->last_chunk) {   /* omitPadding */
            while (written > 0 && dst[written - 1] == '=')
                written--;
        }
        dst[written] = '\0';
        ostr->len = written;
        return JS_MKPTR(JS_TAG_STRING, ostr);
    }

    str = JS_ToCStringLen(ctx, &str_len, step_arg(&s->hdr, 0));
    if (!str)
        return JS_EXCEPTION;

    if (which == B64OP_FROM) {
        size_t out_cap = (str_len / 4) * 3 + 3;
        uint8_t *buf = js_malloc(ctx, out_cap ? out_cap : 1);
        JSValue result;
        if (!buf) { JS_FreeCString(ctx, str); return JS_EXCEPTION; }
        decoded_len = from_base64(str, str_len, buf, out_cap,
                                  s->alphabet == B64_ALPHABET_BASE64URL ? b64_flags_url : b64_flags,
                                  s->last_chunk, &read_pos, &err);
        JS_FreeCString(ctx, str);
        if (err) { js_free(ctx, buf); return JS_ThrowSyntaxError(ctx, "invalid base64 string"); }
        result = JS_NewUint8ArrayCopy(ctx, buf, decoded_len);
        js_free(ctx, buf);
        return result;
    }

    {
        JSObject *p = check_uint8array(ctx, s->hdr.this_val);
        DCHECK(p != NULL, "setFromBase64's receiver was validated at stage 0 and cannot have changed");
        if (get_uint8array_bytes(ctx, p, &data, &len)) { JS_FreeCString(ctx, str); return JS_EXCEPTION; }
        decoded_len = from_base64(str, str_len, data, len,
                                  s->alphabet == B64_ALPHABET_BASE64URL ? b64_flags_url : b64_flags,
                                  s->last_chunk, &read_pos, &err);
        JS_FreeCString(ctx, str);
        if (err)
            return JS_ThrowSyntaxError(ctx, "invalid base64 string");
        return js_make_read_written(ctx, read_pos, decoded_len);
    }
}

/* Uint8Array.prototype.toHex() */
static JSValue js_uint8array_to_hex(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    uint8_t *data;
    size_t len, out_len;
    JSObject *p;
    JSString *ostr;

    p = check_uint8array(ctx, this_val);
    if (!p)
        return JS_EXCEPTION;
    if (get_uint8array_bytes(ctx, p, &data, &len))
        return JS_EXCEPTION;

    out_len = len * 2;
    if (unlikely(out_len > JS_STRING_LEN_MAX))
        return JS_ThrowRangeError(ctx, "output too large");

    ostr = js_alloc_string(ctx, out_len, 0);
    if (!ostr)
        return JS_EXCEPTION;

    u8a_hex_encode(data, len, (char *)str8(ostr));
    str8(ostr)[out_len] = '\0';
    return JS_MKPTR(JS_TAG_STRING, ostr);
}

/* Uint8Array.fromHex(string) */
static JSValue js_uint8array_from_hex(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    const char *str;
    size_t str_len, read_pos, decoded_len, out_cap;
    int err;
    uint8_t *buf;
    JSValue result;

    if (!JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "expected string");

    str = JS_ToCStringLen(ctx, &str_len, argv[0]);
    if (!str)
        return JS_EXCEPTION;

    out_cap = str_len / 2 + 1;
    buf = js_malloc(ctx, out_cap ? out_cap : 1);
    if (!buf) {
        JS_FreeCString(ctx, str);
        return JS_EXCEPTION;
    }

    decoded_len = u8a_hex_decode(str, str_len, buf, out_cap, &read_pos, &err);
    JS_FreeCString(ctx, str);

    if (err) {
        js_free(ctx, buf);
        return JS_ThrowSyntaxError(ctx, "invalid hex string");
    }

    result = JS_NewUint8ArrayCopy(ctx, buf, decoded_len);
    js_free(ctx, buf);
    return result;
}

/* Return a { read, written } result object */
static JSValue js_make_read_written(JSContext *ctx, size_t read, size_t written)
{
    JSValue obj = JS_NewObject(ctx);
    if (JS_IsException(obj))
        return JS_EXCEPTION;
    if (JS_DefinePropertyValueStr(ctx, obj, "read",
            js_uint32(read), JS_PROP_C_W_E) < 0)
        goto fail;
    if (JS_DefinePropertyValueStr(ctx, obj, "written",
            js_uint32(written), JS_PROP_C_W_E) < 0)
        goto fail;
    return obj;
fail:
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

/* Uint8Array.prototype.setFromBase64(string[, options]) */
/* Uint8Array.prototype.setFromHex(string) */
static JSValue js_uint8array_set_from_hex(JSContext *ctx,
                                          JSValueConst this_val,
                                          int argc, JSValueConst *argv)
{
    uint8_t *data;
    size_t len;
    const char *str;
    size_t str_len, read_pos, decoded_len;
    JSObject *p;
    int err;

    p = check_uint8array(ctx, this_val);
    if (!p)
        return JS_EXCEPTION;

    if (typed_array_is_immutable(p))
        return JS_ThrowTypeErrorImmutableArrayBuffer(ctx);

    if (!JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "expected string");

    str = JS_ToCStringLen(ctx, &str_len, argv[0]);
    if (!str)
        return JS_EXCEPTION;

    if (get_uint8array_bytes(ctx, p, &data, &len)) {
        JS_FreeCString(ctx, str);
        return JS_EXCEPTION;
    }

    decoded_len = u8a_hex_decode(str, str_len, data, len, &read_pos, &err);
    JS_FreeCString(ctx, str);

    if (err)
        return JS_ThrowSyntaxError(ctx, "invalid hex string");

    return js_make_read_written(ctx, read_pos, decoded_len);
}

static const JSCFunctionListEntry js_uint8array_proto_funcs[] = {
    JS_CFUNC_STEP_DEF("toBase64", 0, STEPDEF_U8_TOBASE64),
    JS_CFUNC_DEF("toHex", 0, js_uint8array_to_hex),
    JS_CFUNC_STEP_DEF("setFromBase64", 1, STEPDEF_U8_SETFROMBASE64),
    JS_CFUNC_DEF("setFromHex", 1, js_uint8array_set_from_hex),
};

static const JSCFunctionListEntry js_uint8array_funcs[] = {
    JS_CFUNC_STEP_DEF("fromBase64", 1, STEPDEF_U8_FROMBASE64),
    JS_CFUNC_DEF("fromHex", 1, js_uint8array_from_hex),
};

static int js_uint8array_funcs_init(JSContext *ctx)
{
    JSValue ctor, proto;

    ctor = JS_GetProperty(ctx, ctx->global_obj, JS_ATOM_Uint8Array);
    if (JS_IsException(ctor))
        return -1;
    proto = JS_GetProperty(ctx, ctor, JS_ATOM_prototype);
    if (JS_IsException(proto)) {
        JS_FreeValue(ctx, ctor);
        return -1;
    }
    JS_SetPropertyFunctionList(ctx, proto,
                               js_uint8array_proto_funcs,
                               countof(js_uint8array_proto_funcs));
    JS_FreeValue(ctx, proto);
    JS_SetPropertyFunctionList(ctx, ctor,
                               js_uint8array_funcs,
                               countof(js_uint8array_funcs));
    JS_FreeValue(ctx, ctor);
    return 0;
}

int JS_AddIntrinsicAToB(JSContext *ctx)
{
    /* atob throws an InvalidCharacterError, so it needs THIS context's DOMException prototype. The test used to
       be JS_IsRegisteredClass, which is runtime-wide: with more than one context the second got atob and no
       prototype at all, and its InvalidCharacterError had nothing to build on. The intrinsic is idempotent per
       context now, so asking it unconditionally is both correct and cheaper than asking the wrong question. */
    if (JS_AddIntrinsicDOMException(ctx))
        return -1;
    JS_SetPropertyFunctionList(ctx, ctx->global_obj,
                               js_base64_funcs, countof(js_base64_funcs));
    return 0;
}
