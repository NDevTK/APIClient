/* The routed-post index (post_index.h). Each record is rendered once, at the post, and kept as its JSON text
   beside its identity; the array is the order the posts were made in. */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "core/json_buf.h"
#include "solver/post_index.h"
#include "solver/reclaim.h"

typedef struct { char *id; char *json; } PostIndexRecord;

static PostIndexRecord *g_posts;
static size_t g_posts_n, g_posts_cap;

void post_index_record(const PostIndexHead *h, const PostIndexSymbol *syms, size_t n)
{
    JsonBuf b = { 0 };
    size_t i;

    DCHECK(h != NULL && h->id != NULL && *h->id && h->target_doc != NULL && h->target_origin != NULL &&
           h->relation != NULL, "a routed post was indexed with a field of its head missing");
    DCHECK(n > 0 && syms != NULL, "a routed post was indexed with no unknown in it — only a message carrying one "
                                  "has a sender-side fact to state");
    for (i = 0; i < g_posts_n; i++)
        if (!strcmp(g_posts[i].id, h->id)) return;
    json_buf_raw(&b, "{");
    json_buf_key(&b, "id");
    json_buf_str(&b, h->id);
    json_buf_raw(&b, ",");
    json_buf_key(&b, "target");
    json_buf_str(&b, h->target_doc);
    json_buf_raw(&b, ",");
    json_buf_key(&b, "targetOrigin");
    json_buf_str(&b, h->target_origin);
    json_buf_raw(&b, ",");
    json_buf_key(&b, "relation");
    json_buf_str(&b, h->relation);
    json_buf_raw(&b, ",");
    json_buf_key(&b, "symbols");
    json_buf_raw(&b, "[");
    for (i = 0; i < n; i++) {
        DCHECK(syms[i].src != NULL && syms[i].shape != NULL && syms[i].path != NULL,
               "a routed post's unknown was indexed without its source, its shape or its path");
        if (i) json_buf_raw(&b, ",");
        json_buf_raw(&b, "{");
        json_buf_key(&b, "source");
        json_buf_str(&b, syms[i].src);
        json_buf_raw(&b, ",");
        json_buf_key(&b, "shape");
        json_buf_str(&b, syms[i].shape);
        json_buf_raw(&b, ",");
        json_buf_key(&b, "path");
        json_buf_str(&b, syms[i].path);
        json_buf_raw(&b, "}");
    }
    json_buf_raw(&b, "]}");
    if (g_posts_n == g_posts_cap) {
        size_t c = g_posts_cap ? g_posts_cap * 2 : 8;
        PostIndexRecord *a = realloc(g_posts, c * sizeof *a);

        CHECK(a != NULL, "post index: OOM growing the routed-post index");
        g_posts = a;
        g_posts_cap = c;
    }
    g_posts[g_posts_n].id = reclaim_strdup(h->id);
    g_posts[g_posts_n].json = json_buf_take(&b);
    CHECK(g_posts[g_posts_n].id != NULL && g_posts[g_posts_n].json != NULL,
          "post index: OOM keeping a routed post's record");
    g_posts_n++;
}

char *post_index_json(void)
{
    JsonBuf b = { 0 };
    size_t i;

    json_buf_raw(&b, "[");
    for (i = 0; i < g_posts_n; i++) {
        if (i) json_buf_raw(&b, ",");
        json_buf_raw(&b, g_posts[i].json);
    }
    json_buf_raw(&b, "]");
    return json_buf_take(&b);
}

void post_index_free(void)
{
    size_t i;

    for (i = 0; i < g_posts_n; i++) {
        free(g_posts[i].id);
        free(g_posts[i].json);
    }
    free(g_posts);
    g_posts = NULL;
    g_posts_n = g_posts_cap = 0;
}
