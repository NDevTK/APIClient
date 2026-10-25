/*
 * Copyright (C) 2018-2026 Alexander Borisov
 *
 * Author: Alexander Borisov <borisov@lexbor.com>
 */

#include "lexbor/dom/interfaces/text.h"
#include "lexbor/dom/interfaces/comment.h"
#include "lexbor/dom/interfaces/processing_instruction.h"
#include "lexbor/dom/interfaces/document_type.h"

#include "lexbor/html/serialize.h"
#include "lexbor/html/tree.h"
#include "lexbor/ns/ns.h"

#ifndef LEXBOR_DISABLE_INTERNAL_EXTERN
    LXB_EXTERN const unsigned char lexbor_tokenizer_chars_map[256];
#endif

#define lxb_html_serialize_send(data, len, ctx)                                \
    do {                                                                       \
        status = cb((const lxb_char_t *) data, len, ctx);                      \
        if (status != LXB_STATUS_OK) {                                         \
            return status;                                                     \
        }                                                                      \
    }                                                                          \
    while (0)


typedef struct {
    lexbor_str_t  *str;
    lexbor_mraw_t *mraw;
}
lxb_html_serialize_ctx_t;


static lxb_status_t
lxb_html_serialize_str_callback(const lxb_char_t *data, size_t len, void *ctx);

static lxb_status_t
lxb_html_serialize_element_cb(lxb_dom_element_t *element,
                              lxb_html_serialize_cb_f cb, void *ctx);

static lxb_status_t
lxb_html_serialize_text_cb(lxb_dom_text_t *text,
                           lxb_html_serialize_cb_f cb, void *ctx);

static lxb_status_t
lxb_html_serialize_comment_cb(lxb_dom_comment_t *comment,
                              lxb_html_serialize_cb_f cb, void *ctx);

static lxb_status_t
lxb_html_serialize_processing_instruction_cb(lxb_dom_processing_instruction_t *pi,
                                             lxb_html_serialize_cb_f cb,
                                             void *ctx);

static lxb_status_t
lxb_html_serialize_document_type_cb(lxb_dom_document_type_t *doctype,
                                    lxb_html_serialize_cb_f cb, void *ctx);

static lxb_status_t
lxb_html_serialize_document_cb(lxb_dom_document_t *document,
                               lxb_html_serialize_cb_f cb, void *ctx);

static lxb_status_t
lxb_html_serialize_send_escaping_attribute_string(const lxb_char_t *data,
                                                  size_t len,
                                                  lxb_html_serialize_cb_f cb,
                                                  void *ctx);

static lxb_status_t
lxb_html_serialize_send_escaping_string(const lxb_char_t *data, size_t len,
                                        lxb_html_serialize_cb_f cb, void *ctx);

static lxb_status_t
lxb_html_serialize_attribute_cb(lxb_dom_attr_t *attr, bool has_raw,
                                lxb_html_serialize_cb_f cb, void *ctx);


lxb_status_t
lxb_html_serialize_cb(lxb_dom_node_t *node,
                      lxb_html_serialize_cb_f cb, void *ctx)
{
    switch (node->type) {
        case LXB_DOM_NODE_TYPE_ELEMENT:
            return lxb_html_serialize_element_cb(lxb_dom_interface_element(node),
                                                 cb, ctx);

        case LXB_DOM_NODE_TYPE_TEXT:
            return lxb_html_serialize_text_cb(lxb_dom_interface_text(node),
                                              cb, ctx);

        case LXB_DOM_NODE_TYPE_COMMENT:
            return lxb_html_serialize_comment_cb(lxb_dom_interface_comment(node),
                                                 cb, ctx);

        case LXB_DOM_NODE_TYPE_PROCESSING_INSTRUCTION:
            return lxb_html_serialize_processing_instruction_cb(lxb_dom_interface_processing_instruction(node),
                                                                cb, ctx);

        case LXB_DOM_NODE_TYPE_DOCUMENT_TYPE:
            return lxb_html_serialize_document_type_cb(lxb_dom_interface_document_type(node),
                                                       cb, ctx);

        case LXB_DOM_NODE_TYPE_DOCUMENT:
            return lxb_html_serialize_document_cb(lxb_dom_interface_document(node),
                                                  cb, ctx);

        default:
            break;
    }

    return LXB_STATUS_ERROR;
}

lxb_status_t
lxb_html_serialize_str(lxb_dom_node_t *node, lexbor_str_t *str)
{
    lxb_html_serialize_ctx_t ctx;

    if (str->data == NULL) {
        lexbor_str_init(str, node->owner_document->text, 1024);

        if (str->data == NULL) {
            return LXB_STATUS_ERROR_MEMORY_ALLOCATION;
        }
    }

    ctx.str = str;
    ctx.mraw = node->owner_document->text;

    return lxb_html_serialize_cb(node, lxb_html_serialize_str_callback, &ctx);
}

static lxb_status_t
lxb_html_serialize_str_callback(const lxb_char_t *data, size_t len, void *ctx)
{
    lxb_char_t *ret;
    lxb_html_serialize_ctx_t *s_ctx = ctx;

    ret = lexbor_str_append(s_ctx->str, s_ctx->mraw, data, len);
    if (ret == NULL) {
        return LXB_STATUS_ERROR_MEMORY_ALLOCATION;
    }

    return LXB_STATUS_OK;
}

static lxb_status_t
lxb_html_serialize_element_cb(lxb_dom_element_t *element,
                              lxb_html_serialize_cb_f cb, void *ctx)
{
    lxb_status_t status;
    const lxb_char_t *tag_name;
    size_t len = 0;

    lxb_dom_attr_t *attr;

    tag_name = lxb_dom_element_qualified_name(element, &len);
    if (tag_name == NULL) {
        return LXB_STATUS_ERROR;
    }

    lxb_html_serialize_send("<", 1, ctx);
    lxb_html_serialize_send(tag_name, len, ctx);

    if (element->is_value != NULL && element->is_value->data != NULL) {
        attr = lxb_dom_element_attr_is_exist(element,
                                             (const lxb_char_t *) "is", 2);
        if (attr == NULL) {
            lxb_html_serialize_send(" is=\"", 5, ctx);

            status = lxb_html_serialize_send_escaping_attribute_string(element->is_value->data,
                                                                       element->is_value->length,
                                                                       cb, ctx);
            if (status != LXB_STATUS_OK) {
                return status;
            }

            lxb_html_serialize_send("\"", 1, ctx);
        }
    }

    attr = element->first_attr;

    while (attr != NULL) {
        lxb_html_serialize_send(" ", 1, ctx);

        status = lxb_html_serialize_attribute_cb(attr, false, cb, ctx);
        if (status != LXB_STATUS_OK) {
            return status;
        }

        attr = attr->next;
    }

    lxb_html_serialize_send(">", 1, ctx);

    return LXB_STATUS_OK;
}

static lxb_status_t
lxb_html_serialize_text_cb(lxb_dom_text_t *text,
                           lxb_html_serialize_cb_f cb, void *ctx)
{
    lxb_status_t status;

    lxb_dom_node_t *node = lxb_dom_interface_node(text);
    lxb_dom_document_t *doc = node->owner_document;
    lexbor_str_t *data = &text->char_data.data;

    if (node->parent != NULL) {
        switch (node->parent->local_name) {
            case LXB_TAG_STYLE:
            case LXB_TAG_SCRIPT:
            case LXB_TAG_XMP:
            case LXB_TAG_IFRAME:
            case LXB_TAG_NOEMBED:
            case LXB_TAG_NOFRAMES:
            case LXB_TAG_PLAINTEXT:
                lxb_html_serialize_send(data->data, data->length, ctx);

                return LXB_STATUS_OK;

            case LXB_TAG_NOSCRIPT:
                if (doc->scripting) {
                    lxb_html_serialize_send(data->data, data->length, ctx);

                    return LXB_STATUS_OK;
                }

                break;

            default:
                break;
        }
    }

    return lxb_html_serialize_send_escaping_string(data->data, data->length,
                                                   cb, ctx);
}

static lxb_status_t
lxb_html_serialize_comment_cb(lxb_dom_comment_t *comment,
                              lxb_html_serialize_cb_f cb, void *ctx)
{
    lxb_status_t status;
    lexbor_str_t *data = &comment->char_data.data;

    lxb_html_serialize_send("<!--", 4, ctx);
    lxb_html_serialize_send(data->data, data->length, ctx);
    lxb_html_serialize_send("-->", 3, ctx);

    return LXB_STATUS_OK;
}

static lxb_status_t
lxb_html_serialize_processing_instruction_cb(lxb_dom_processing_instruction_t *pi,
                                             lxb_html_serialize_cb_f cb,
                                             void *ctx)
{
    lxb_status_t status;
    lexbor_str_t *data = &pi->char_data.data;

    lxb_html_serialize_send("<?", 2, ctx);
    lxb_html_serialize_send(pi->target.data, pi->target.length, ctx);
    lxb_html_serialize_send(" ", 1, ctx);
    lxb_html_serialize_send(data->data, data->length, ctx);
    lxb_html_serialize_send(">", 1, ctx);

    return LXB_STATUS_OK;
}

static lxb_status_t
lxb_html_serialize_document_type_cb(lxb_dom_document_type_t *doctype,
                                    lxb_html_serialize_cb_f cb, void *ctx)
{
    size_t length;
    const lxb_char_t *name;
    lxb_status_t status;

    lxb_html_serialize_send("<!DOCTYPE", 9, ctx);
    lxb_html_serialize_send(" ", 1, ctx);

    name = lxb_dom_document_type_name(doctype, &length);

    if (length != 0) {
        lxb_html_serialize_send(name, length, ctx);
    }

    lxb_html_serialize_send(">", 1, ctx);

    return LXB_STATUS_OK;
}

static lxb_status_t
lxb_html_serialize_document_cb(lxb_dom_document_t *document,
                               lxb_html_serialize_cb_f cb, void *ctx)
{
    lxb_status_t status;

    lxb_html_serialize_send("<#document>", 11, ctx);

    return LXB_STATUS_OK;
}

static lxb_status_t
lxb_html_serialize_send_escaping_attribute_string(const lxb_char_t *data,
                                                  size_t len,
                                                  lxb_html_serialize_cb_f cb,
                                                  void *ctx)
{
    lxb_status_t status;
    const lxb_char_t *pos = data;
    const lxb_char_t *end = data + len;

    while (data != end) {
        switch (*data) {
            /* U+0026 AMPERSAND (&) */
            case 0x26:
                if (pos != data) {
                    lxb_html_serialize_send(pos, (data - pos), ctx);
                }

                lxb_html_serialize_send("&amp;", 5, ctx);

                data++;
                pos = data;

                break;

            /* {0xC2, 0xA0} NO-BREAK SPACE */
            case 0xC2:
                data += 1;
                if (data == end) {
                    break;
                }

                if (*data != 0xA0) {
                    continue;
                }

                data -= 1;

                if (pos != data) {
                    lxb_html_serialize_send(pos, (data - pos), ctx);
                }

                lxb_html_serialize_send("&nbsp;", 6, ctx);

                data += 2;
                pos = data;

                break;

            /* U+003C LESS-THAN SIGN (<) */
            case 0x3C:
                if (pos != data) {
                    lxb_html_serialize_send(pos, (data - pos), ctx);
                }

                lxb_html_serialize_send("&lt;", 4, ctx);

                data++;
                pos = data;

                break;

            /* U+003E GREATER-THAN SIGN (>) */
            case 0x3E:
                if (pos != data) {
                    lxb_html_serialize_send(pos, (data - pos), ctx);
                }

                lxb_html_serialize_send("&gt;", 4, ctx);

                data++;
                pos = data;

                break;

            /* U+0022 QUOTATION MARK (") */
            case 0x22:
                if (pos != data) {
                    lxb_html_serialize_send(pos, (data - pos), ctx);
                }

                lxb_html_serialize_send("&quot;", 6, ctx);

                data++;
                pos = data;

                break;

            default:
                data++;

                break;
        }
    }

    if (pos != data) {
        lxb_html_serialize_send(pos, (data - pos), ctx);
    }

    return LXB_STATUS_OK;
}

static lxb_status_t
lxb_html_serialize_send_escaping_string(const lxb_char_t *data, size_t len,
                                        lxb_html_serialize_cb_f cb, void *ctx)
{
    lxb_status_t status;
    const lxb_char_t *pos = data;
    const lxb_char_t *end = data + len;

    while (data != end) {
        switch (*data) {
            /* U+0026 AMPERSAND (&) */
            case 0x26:
                if (pos != data) {
                    lxb_html_serialize_send(pos, (data - pos), ctx);
                }

                lxb_html_serialize_send("&amp;", 5, ctx);

                data++;
                pos = data;

                break;

            /* {0xC2, 0xA0} NO-BREAK SPACE */
            case 0xC2:
                data += 1;
                if (data == end) {
                    break;
                }

                if (*data != 0xA0) {
                    continue;
                }

                data -= 1;

                if (pos != data) {
                    lxb_html_serialize_send(pos, (data - pos), ctx);
                }

                lxb_html_serialize_send("&nbsp;", 6, ctx);

                data += 2;
                pos = data;

                break;

            /* U+003C LESS-THAN SIGN (<) */
            case 0x3C:
                if (pos != data) {
                    lxb_html_serialize_send(pos, (data - pos), ctx);
                }

                lxb_html_serialize_send("&lt;", 4, ctx);

                data++;
                pos = data;

                break;

            /* U+003E GREATER-THAN SIGN (>) */
            case 0x3E:
                if (pos != data) {
                    lxb_html_serialize_send(pos, (data - pos), ctx);
                }

                lxb_html_serialize_send("&gt;", 4, ctx);

                data++;
                pos = data;

                break;

            default:
                data++;

                break;
        }
    }

    if (pos != data) {
        lxb_html_serialize_send(pos, (data - pos), ctx);
    }

    return LXB_STATUS_OK;
}

static lxb_status_t
lxb_html_serialize_attribute_cb(lxb_dom_attr_t *attr, bool has_raw,
                                lxb_html_serialize_cb_f cb, void *ctx)
{
    size_t length;
    lxb_status_t status;
    const lxb_char_t *str;
    const lxb_dom_attr_data_t *data;

    data = lxb_dom_attr_data_by_id(attr->node.owner_document->attrs,
                                   attr->node.local_name);
    if (data == NULL) {
        return LXB_STATUS_ERROR;
    }

    if (attr->node.ns == LXB_NS__UNDEF) {
        lxb_html_serialize_send(lexbor_hash_entry_str(&data->entry),
                                data->entry.length, ctx);
        goto value;
    }

    if (attr->node.ns == LXB_NS_XML) {
        lxb_html_serialize_send((const lxb_char_t *) "xml:", 4, ctx);
        lxb_html_serialize_send(lexbor_hash_entry_str(&data->entry),
                                data->entry.length, ctx);

        goto value;
    }

    if (attr->node.ns == LXB_NS_XMLNS)
    {
        if (data->entry.length == 5
            && lexbor_str_data_cmp(lexbor_hash_entry_str(&data->entry),
                                   (const lxb_char_t *) "xmlns"))
        {
            lxb_html_serialize_send((const lxb_char_t *) "xmlns", 5, ctx);
        }
        else {
            lxb_html_serialize_send((const lxb_char_t *) "xmlns:", 6, ctx);
            lxb_html_serialize_send(lexbor_hash_entry_str(&data->entry),
                                    data->entry.length, ctx);
        }

        goto value;
    }

    if (attr->node.ns == LXB_NS_XLINK) {
        lxb_html_serialize_send((const lxb_char_t *) "xlink:", 6, ctx);
        lxb_html_serialize_send(lexbor_hash_entry_str(&data->entry),
                                data->entry.length, ctx);

        goto value;
    }

    str = lxb_dom_attr_qualified_name(attr, &length);
    if (str == NULL) {
        return LXB_STATUS_ERROR;
    }

    lxb_html_serialize_send(str, length, ctx);

value:

    if (attr->value == NULL) {
        lxb_html_serialize_send("=\"\"", 3, ctx);
        return LXB_STATUS_OK;
    }

    lxb_html_serialize_send("=\"", 2, ctx);

    if (has_raw) {
        lxb_html_serialize_send(attr->value->data, attr->value->length, ctx);
    }
    else {
        status = lxb_html_serialize_send_escaping_attribute_string(attr->value->data,
                                                                   attr->value->length,
                                                                   cb, ctx);
        if (status != LXB_STATUS_OK) {
            return status;
        }
    }

    lxb_html_serialize_send("\"", 1, ctx);

    return LXB_STATUS_OK;
}

