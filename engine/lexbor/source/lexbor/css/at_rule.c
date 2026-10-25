/*
 * Copyright (C) 2021-2026 Alexander Borisov
 *
 * Author: Alexander Borisov <borisov@lexbor.com>
 */

#include "lexbor/css/at_rule.h"
#include "lexbor/css/css.h"
#include "lexbor/css/parser.h"
#include "lexbor/css/stylesheet.h"
#include "lexbor/css/at_rule/state.h"
#include "lexbor/css/at_rule/types.h"
#include "lexbor/css/at_rule/res.h"
#include "lexbor/core/serialize.h"


const lxb_css_entry_at_rule_data_t *
lxb_css_at_rule_by_name(const lxb_char_t *name, size_t length)
{
    const lexbor_shs_entry_t *entry;

    entry = lexbor_shs_entry_get_lower_static(lxb_css_at_rule_shs,
                                              name, length);
    if (entry == NULL) {
        return NULL;
    }

    return entry->value;
}

const lxb_css_entry_at_rule_data_t *
lxb_css_at_rule_by_id(uintptr_t id)
{
    return &lxb_css_at_rule_data[id];
}

lxb_css_rule_at_t *
lxb_css_at_rule_create(lxb_css_parser_t *parser,
                       const lxb_char_t *name, size_t length,
                       const lxb_css_entry_at_rule_data_t **out_entry)
{
    void *prop;
    const lxb_css_entry_at_rule_data_t *entry;
    lxb_css_at_rule__custom_t *custom;
    lxb_css_rule_at_t *at;

    at = lxb_css_rule_at_create(parser->memory);
    if (at == NULL) {
        return NULL;
    }

    entry = lxb_css_at_rule_by_name(name, length);
    if (entry == NULL) {
        entry = lxb_css_at_rule_by_id(LXB_CSS_AT_RULE__CUSTOM);

        prop = entry->create(parser->memory);
        if (prop == NULL) {
            goto failed;
        }

        custom = prop;

        (void) lexbor_str_init(&custom->name, parser->memory->mraw, length);
        if (custom->name.data == NULL) {
            goto failed;
        }

        memcpy(custom->name.data, name, length);

        custom->name.length = length;
        custom->name.data[custom->name.length] = 0x00;
    }
    else {
        prop = entry->create(parser->memory);
        if (prop == NULL) {
            goto failed;
        }
    }

    at->type = entry->unique;
    at->u.user = prop;

    if (out_entry != NULL) {
        *out_entry = entry;
    }

    return at;

failed:

    if (prop != NULL) {
        (void) entry->destroy(parser->memory, prop, true);
    }

    if (at != NULL) {
        (void) lxb_css_rule_at_destroy(at, true);
    }

    if (out_entry != NULL) {
        *out_entry = NULL;
    }

    return NULL;
}

void *
lxb_css_at_rule_destroy(lxb_css_memory_t *memory, void *value,
                        lxb_css_at_rule_type_t type, bool self_destroy)
{
    const lxb_css_entry_at_rule_data_t *data;

    data = lxb_css_at_rule_by_id(type);
    if (data == NULL) {
        return value;
    }

    return data->destroy(memory, value, self_destroy);
}

lxb_status_t
lxb_css_at_rule_convert_to_undef(lxb_css_parser_t *parser,
                                 lxb_css_rule_at_t *at)
{
    lxb_css_at_rule__undef_t *undef;

    undef = lxb_css_at_rule__undef_create(parser->memory);
    if (undef == NULL) {
        return LXB_STATUS_ERROR_MEMORY_ALLOCATION;
    }

    undef->type = at->type;

    (void) lxb_css_at_rule_destroy(parser->memory, at, undef->type, false);

    at->type = LXB_CSS_AT_RULE__UNDEF;
    at->u.undef = undef;

    return lxb_css_make_data(parser, &at->u.undef->prelude,
                             at->prelude_begin, at->prelude_end);
}

lxb_status_t
lxb_css_at_rule_serialize_name(const void *style, lxb_css_at_rule_type_t type,
                               lexbor_serialize_cb_f cb, void *ctx)
{
    const lxb_css_entry_at_rule_data_t *data;

    if (type == LXB_CSS_AT_RULE__UNDEF) {
        return lxb_css_at_rule__undef_serialize_name(style, cb, ctx);
    }
    else if (type == LXB_CSS_AT_RULE__CUSTOM) {
        return lxb_css_at_rule__custom_serialize_name(style, cb, ctx);
    }

    data = lxb_css_at_rule_by_id(type);
    if (data == NULL) {
        return LXB_STATUS_ERROR_UNEXPECTED_DATA;
    }

    return cb(data->name, data->length, ctx);
}

lxb_status_t
lxb_css_at_rule_serialize_name_str(const void *style, lxb_css_at_rule_type_t type,
                                   lexbor_mraw_t *mraw, lexbor_str_t *str)
{
    const lxb_css_entry_at_rule_data_t *data;

    if (type == LXB_CSS_AT_RULE__UNDEF) {
        return lxb_css_serialize_str_handler(style, str, mraw,
                                        lxb_css_at_rule__undef_serialize_name);
    }
    else if (type == LXB_CSS_AT_RULE__CUSTOM) {
        return lxb_css_serialize_str_handler(style, str, mraw,
                                       lxb_css_at_rule__custom_serialize_name);
    }

    data = lxb_css_at_rule_by_id(type);
    if (data == NULL) {
        return LXB_STATUS_ERROR_UNEXPECTED_DATA;
    }

    if (str->data == NULL) {
        lexbor_str_init(str, mraw, data->length);
        if (str->data == NULL) {
            return LXB_STATUS_ERROR_MEMORY_ALLOCATION;
        }
    }

    (void) lexbor_str_append(str, mraw, data->name, data->length);

    return LXB_STATUS_OK;
}

/* _undef. */

void *
lxb_css_at_rule__undef_create(lxb_css_memory_t *memory)
{
    return lexbor_mraw_calloc(memory->mraw, sizeof(lxb_css_at_rule__undef_t));
}

void *
lxb_css_at_rule__undef_destroy(lxb_css_memory_t *memory,
                               void *style, bool self_destroy)
{
    if (style == NULL) {
        return NULL;
    }

    if (self_destroy) {
        return lexbor_mraw_free(memory->mraw, style);
    }

    return style;
}

lxb_status_t
lxb_css_at_rule__undef_serialize_name(const void *at, lexbor_serialize_cb_f cb,
                                      void *ctx)
{
    const lxb_css_entry_at_rule_data_t *data;
    const lxb_css_at_rule__undef_t *undef = at;

    data = lxb_css_at_rule_by_id(undef->type);
    if (data == NULL) {
        return LXB_STATUS_ERROR_UNEXPECTED_DATA;
    }

    return cb(data->name, data->length, ctx);
}

/* _custom. */

void *
lxb_css_at_rule__custom_create(lxb_css_memory_t *memory)
{
    return lexbor_mraw_calloc(memory->mraw, sizeof(lxb_css_at_rule__custom_t));
}

void *
lxb_css_at_rule__custom_destroy(lxb_css_memory_t *memory,
                                void *style, bool self_destroy)
{
    if (style == NULL) {
        return NULL;
    }

    if (self_destroy) {
        return lexbor_mraw_free(memory->mraw, style);
    }

    return style;
}

lxb_status_t
lxb_css_at_rule__custom_serialize_name(const void *at, lexbor_serialize_cb_f cb,
                                       void *ctx)
{
    const lxb_css_at_rule__custom_t *custom = at;

    return cb(custom->name.data, custom->name.length, ctx);
}

/* Media. */

void *
lxb_css_at_rule_media_create(lxb_css_memory_t *memory)
{
    return lexbor_mraw_calloc(memory->mraw, sizeof(lxb_css_at_rule_media_t));
}

void *
lxb_css_at_rule_media_destroy(lxb_css_memory_t *memory,
                              void *style, bool self_destroy)
{
    return lxb_css_at_rule__undef_destroy(memory, style, self_destroy);
}

/* Namespace. */

void *
lxb_css_at_rule_namespace_create(lxb_css_memory_t *memory)
{
    return lexbor_mraw_calloc(memory->mraw,
                              sizeof(lxb_css_at_rule_namespace_t));
}

void *
lxb_css_at_rule_namespace_destroy(lxb_css_memory_t *memory,
                                  void *style, bool self_destroy)
{
    return lxb_css_at_rule__undef_destroy(memory, style, self_destroy);
}

/* Font-face. */

void *
lxb_css_at_rule_font_face_create(lxb_css_memory_t *memory)
{
    return lexbor_mraw_calloc(memory->mraw,
                              sizeof(lxb_css_at_rule_font_face_t));
}

void *
lxb_css_at_rule_font_face_destroy(lxb_css_memory_t *memory,
                                  void *style, bool self_destroy)
{
    return lxb_css_at_rule__undef_destroy(memory, style, self_destroy);
}

