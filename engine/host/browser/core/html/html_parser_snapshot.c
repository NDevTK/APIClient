/* The HTML parser's between-writes state as a value — see html_parser_snapshot.h. */
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/html/html.h>

#include "check.h"
#include "quickjs.h"
#include "core/html/html_parser_snapshot.h"

/* The fixed part of a snapshot. The open-elements and active-formatting lists (node addresses) and the template
   insertion modes (their records) follow it, in that order. */
typedef struct {
    /* Whether the tokenizer stood between tokens: an empty temp buffer, no attribute under construction, no
       UTF-8 sequence split across writes, and no pending table text. Only that state is restorable. */
    bool                            between_tokens;
    lxb_html_tokenizer_state_f      state, state_return;
    lxb_tag_id_t                    tmp_tag_id;
    lxb_html_tokenizer_opt_t        opt;
    lxb_html_form_element_t        *form;
    bool                            foster_parenting, frameset_ok;
    lxb_html_tree_insertion_mode_f  mode, original_mode;
    size_t                          n_open, n_active, n_template, template_size;
} HtmlParserSnapshot;

JSValue html_parser_snapshot_take(JSContext *ctx, const lxb_html_parser_t *parser)
{
    const lxb_html_tokenizer_t *tkz = parser->tkz;
    const lxb_html_tree_t *tree = parser->tree;
    HtmlParserSnapshot h;
    size_t open_n, active_n, tmpl_n, total;
    uint8_t *buf;
    JSValue ab;

    DCHECK(tkz != NULL && tree != NULL && tree->tkz_ref == tkz,
           "a parser snapshot was taken of a parser without its tokenizer and tree builder paired");
    memset(&h, 0, sizeof h);
    h.between_tokens = tkz->pos == tkz->start && tkz->token->attr_first == NULL && tkz->utf8_buf_len == 0 &&
                       tree->pending_table.text_list->length == 0;
    h.state = tkz->state;
    h.state_return = tkz->state_return;
    h.tmp_tag_id = tkz->tmp_tag_id;
    h.opt = tkz->opt;
    h.form = tree->form;
    h.foster_parenting = tree->foster_parenting;
    h.frameset_ok = tree->frameset_ok;
    h.mode = tree->mode;
    h.original_mode = tree->original_mode;
    h.n_open = open_n = tree->open_elements->length;
    h.n_active = active_n = tree->active_formatting->length;
    h.n_template = tmpl_n = tree->template_insertion_modes->length;
    h.template_size = tree->template_insertion_modes->struct_size;
    total = sizeof h + (open_n + active_n) * sizeof(void *) + tmpl_n * h.template_size;
    buf = malloc(total);
    CHECK(buf != NULL, "html-parse-oom: a parser snapshot could not be allocated");
    memcpy(buf, &h, sizeof h);
    memcpy(buf + sizeof h, tree->open_elements->list, open_n * sizeof(void *));
    memcpy(buf + sizeof h + open_n * sizeof(void *), tree->active_formatting->list, active_n * sizeof(void *));
    memcpy(buf + sizeof h + (open_n + active_n) * sizeof(void *), tree->template_insertion_modes->list,
           tmpl_n * h.template_size);
    ab = JS_NewArrayBufferCopy(ctx, buf, total);
    free(buf);
    CHECK(!JS_IsException(ab), "html-parse-oom: a parser snapshot's buffer could not be allocated");
    return ab;
}

void html_parser_snapshot_restore(JSContext *ctx, lxb_html_parser_t *parser, JSValueConst snap)
{
    lxb_html_tokenizer_t *tkz = parser->tkz;
    lxb_html_tree_t *tree = parser->tree;
    HtmlParserSnapshot h;
    const uint8_t *buf;
    size_t len = 0;

    buf = JS_GetArrayBuffer(ctx, &len, snap);
    DCHECK(buf != NULL && len >= sizeof h, "a parser snapshot is not the buffer html_parser_snapshot_take made");
    memcpy(&h, buf, sizeof h);
    DCHECK(len == sizeof h + (h.n_open + h.n_active) * sizeof(void *) + h.n_template * h.template_size &&
               h.template_size == tree->template_insertion_modes->struct_size,
           "a parser snapshot's length disagrees with the lists its header counts");
    /* NAMED RESIDUAL: a snapshot taken mid-token (pending characters or a tag in the temp buffer, an attribute
       under construction, a split UTF-8 sequence, pending table text) is not restorable; the token and those
       buffers are not copied. The next diff copies the temp buffer and the token with its attributes. Its
       absence shows as this crash: a fork between two writes that split a token. */
    if (!h.between_tokens)
        DFAIL("§8.4.3 document.write(): a fork's arm resumed a script-created parser whose last write ended "
              "mid-token, and only a tokenizer standing between tokens is restorable. Copy the temp buffer, "
              "the token and its attributes into the snapshot");
    tkz->state = h.state;
    tkz->state_return = h.state_return;
    tkz->tmp_tag_id = h.tmp_tag_id;
    tkz->opt = h.opt;
    tkz->pos = tkz->start;
    tree->form = h.form;
    tree->foster_parenting = h.foster_parenting;
    tree->frameset_ok = h.frameset_ok;
    tree->mode = h.mode;
    tree->original_mode = h.original_mode;
    if (h.n_open > tree->open_elements->size)
        CHECK(lexbor_array_expand(tree->open_elements, h.n_open) != NULL,
              "html-parse-oom: the stack of open elements could not hold a restored parser's");
    memcpy(tree->open_elements->list, buf + sizeof h, h.n_open * sizeof(void *));
    tree->open_elements->length = h.n_open;
    if (h.n_active > tree->active_formatting->size)
        CHECK(lexbor_array_expand(tree->active_formatting, h.n_active) != NULL,
              "html-parse-oom: the active formatting list could not hold a restored parser's");
    memcpy(tree->active_formatting->list, buf + sizeof h + h.n_open * sizeof(void *), h.n_active * sizeof(void *));
    tree->active_formatting->length = h.n_active;
    if (h.n_template > tree->template_insertion_modes->size)
        CHECK(lexbor_array_obj_expand(tree->template_insertion_modes, h.n_template) != NULL,
              "html-parse-oom: the template insertion modes could not hold a restored parser's");
    memcpy(tree->template_insertion_modes->list, buf + sizeof h + (h.n_open + h.n_active) * sizeof(void *),
           h.n_template * h.template_size);
    tree->template_insertion_modes->length = h.n_template;
}
