/* See json_buf.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/core/base.h>   /* lexbor_realloc: the allocator solver/reclaim.c gives the refusal edge */

#include "check.h"
#include "core/json_buf.h"

/* Room for `extra` more bytes and the NUL. The growth asks lexbor_realloc, which solver/reclaim.c points at the
   refusal edge, so a refusal inside a flow step sells the frontier's tail and retries; the capacity is published
   only after the allocation returns, so a sale run inside it sees the buffer that exists. A refusal that stands
   is fatal for an ordinary buffer and, for a JSON_BUF_AT_REST one, releases the bytes and marks the buffer lost.
   Returns whether the room is there. */
static int json_buf_ensure(JsonBuf *b, size_t extra)
{
    size_t cap = b->cap;
    char *grown;

    if (b->lost) return 0;
    if (b->n + extra + 1 <= cap) return 1;
    while (b->n + extra + 1 > cap) cap = cap ? cap * 2 : 256;
    grown = lexbor_realloc(b->b, cap);
    if (!grown) {
        CHECK(b->at_rest, "json_buf: OOM growing a JSON buffer — a dropped record is a report that lies by "
                          "omission");
        free(b->b);   /* realloc leaves the old block in place when it refuses */
        b->b = NULL;
        b->n = b->cap = 0;
        b->lost = 1;
        return 0;
    }
    b->b = grown;
    b->cap = cap;
    return 1;
}

char *json_buf_block(size_t n)
{
    DCHECK(n > 0, "json_buf: an output block of no bytes was asked — every composer's block holds at least its "
                  "closing NUL");
    return lexbor_malloc(n);
}

void json_buf_raw(JsonBuf *b, const char *s)
{
    size_t l = strlen(s);
    if (!json_buf_ensure(b, l)) return;
    memcpy(b->b + b->n, s, l);
    b->n += l;
}

/* THE HALF THE PREPROCESSOR CANNOT SEE. json_buf_key's macro proves its argument is a LITERAL and says nothing
   about what is in it, and `json_buf_key(&b, "a\":1,\"b")` compiles: a literal that closes its own key and
   opens a second field. So the shape is asserted here — the text this function receives is always the macro's
   own `"` + name + `":`, so anything else means the name carried a quote or a backslash of its own.
   THE OFFENDING LITERAL IS THE ADDRESS. A DCHECK stamps the line it is written at, which for a helper is this
   line for every caller; naming the text in the message is what makes the crash actionable, because the text
   is the source of the one call site that produced it and a grep finds it exactly. */
void json_buf_key_(JsonBuf *b, const char *quoted_name_colon)
{
    size_t l = strlen(quoted_name_colon);

    DCHECKF(l >= 4 && quoted_name_colon[0] == '"' && quoted_name_colon[l - 2] == '"' &&
                quoted_name_colon[l - 1] == ':' && !memchr(quoted_name_colon + 1, '"', l - 3) &&
                !memchr(quoted_name_colon + 1, '\\', l - 3),
            "json_buf: `%s` is not ONE JSON member name — the name carries a quote or a backslash of its own, "
            "so these bytes close the key and open something else. Grep that text for the json_buf_key( that "
            "wrote it: the literal IS the call site, which is why it is in this message and not just this line",
            quoted_name_colon);
    json_buf_raw(b, quoted_name_colon);
}

void json_buf_str(JsonBuf *b, const char *s)
{
    if (!json_buf_ensure(b, 1)) return;
    b->b[b->n++] = '"';
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            if (!json_buf_ensure(b, 2)) return;
            b->b[b->n++] = '\\';
            b->b[b->n++] = (char)c;
        }
        else if (c == '\n') json_buf_raw(b, "\\n");
        else if (c == '\r') json_buf_raw(b, "\\r");
        else if (c == '\t') json_buf_raw(b, "\\t");
        else if (c < 0x20) { char t[8]; snprintf(t, sizeof t, "\\u%04x", c); json_buf_raw(b, t); }
        else {
            if (!json_buf_ensure(b, 1)) return;
            b->b[b->n++] = (char)c;
        }
    }
    if (!json_buf_ensure(b, 1)) return;
    b->b[b->n++] = '"';
}

char *json_buf_take(JsonBuf *b)
{
    char *out = b->b;
    if (b->lost) return NULL;   /* a JSON_BUF_AT_REST buffer the allocator refused: its composition is absent */
    if (!out) {
        out = lexbor_malloc(1);
        if (!out) {
            CHECK(b->at_rest, "json_buf: OOM taking an empty JSON buffer");
            return NULL;
        }
        out[0] = 0;
    } else {
        out[b->n] = 0;
    }
    b->b = NULL;
    b->n = b->cap = 0;
    return out;
}

void json_buf_free(JsonBuf *b)
{
    free(b->b);
    b->b = NULL;
    b->n = b->cap = 0;
}
