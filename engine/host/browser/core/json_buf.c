/* See json_buf.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "core/json_buf.h"

static void json_buf_ensure(JsonBuf *b, size_t extra)
{
    if (b->n + extra + 1 <= b->cap) return;
    while (b->n + extra + 1 > b->cap) b->cap = b->cap ? b->cap * 2 : 256;
    b->b = realloc(b->b, b->cap);
    CHECK(b->b != NULL, "json_buf: OOM growing a JSON buffer — a dropped record is a report that lies by "
                        "omission");
}

void json_buf_puts(JsonBuf *b, const char *s)
{
    size_t l = strlen(s);
    json_buf_ensure(b, l);
    memcpy(b->b + b->n, s, l);
    b->n += l;
}

void json_buf_str(JsonBuf *b, const char *s)
{
    json_buf_ensure(b, 1);
    b->b[b->n++] = '"';
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { json_buf_ensure(b, 2); b->b[b->n++] = '\\'; b->b[b->n++] = (char)c; }
        else if (c == '\n') json_buf_puts(b, "\\n");
        else if (c == '\r') json_buf_puts(b, "\\r");
        else if (c == '\t') json_buf_puts(b, "\\t");
        else if (c < 0x20) { char t[8]; snprintf(t, sizeof t, "\\u%04x", c); json_buf_puts(b, t); }
        else { json_buf_ensure(b, 1); b->b[b->n++] = (char)c; }
    }
    json_buf_ensure(b, 1);
    b->b[b->n++] = '"';
}

char *json_buf_take(JsonBuf *b)
{
    char *out = b->b;
    if (!out) {
        out = malloc(1);
        CHECK(out != NULL, "json_buf: OOM taking an empty JSON buffer");
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
