/*
 * Copyright (C) 2021-2026 Alexander Borisov
 *
 * Author: Alexander Borisov <borisov@lexbor.com>
 */

#include "lexbor/css/rule.h"
#include "lexbor/css/parser.h"
#include "lexbor/core/serialize.h"
#include "lexbor/css/stylesheet.h"
#include "lexbor/css/selectors/selectors.h"


void *
lxb_css_rule_destroy(lxb_css_rule_t *rule, bool self_destroy)
{
    switch (rule->type) {
        case LXB_CSS_RULE_LIST:
            return lxb_css_rule_list_destroy(lxb_css_rule_list(rule),
                                             self_destroy);
        case LXB_CSS_RULE_AT_RULE:
            return lxb_css_rule_at_destroy(lxb_css_rule_at(rule),
                                           self_destroy);
        case LXB_CSS_RULE_STYLE:
            return lxb_css_rule_style_destroy(lxb_css_rule_style(rule),
                                              self_destroy);
        case LXB_CSS_RULE_BAD_STYLE:
            return lxb_css_rule_bad_style_destroy(lxb_css_rule_bad_style(rule),
                                                  self_destroy);
        case LXB_CSS_RULE_DECLARATION:
            return lxb_css_rule_declaration_destroy(lxb_css_rule_declaration(rule),
                                                    self_destroy);
        case LXB_CSS_RULE_DECLARATION_LIST:
            return lxb_css_rule_declaration_list_destroy(lxb_css_rule_declaration_list(rule),
                                                         self_destroy);
        case LXB_CSS_RULE_STYLESHEET:
        case LXB_CSS_RULE_UNDEF:
            return NULL;
    }

    return NULL;
}

lxb_css_rule_list_t *
lxb_css_rule_list_destroy(lxb_css_rule_list_t *list, bool self_destroy)
{
    lxb_css_rule_t *rule, *next, *parent;
    lxb_css_rule_t *child_rule;
    lxb_css_memory_t *memory;
    lxb_css_rule_at_t *at;
    lxb_css_rule_style_t *style;
    lxb_css_rule_bad_style_t *bad;
    lxb_css_rule_list_t *sub_list;
    lxb_css_rule_declaration_list_t *declr_list;

    if (list == NULL) {
        return NULL;
    }

    rule = list->first;
    memory = lxb_css_rule(list)->memory;

    while (rule != NULL) {
        child_rule = NULL;

        switch (rule->type) {
            case LXB_CSS_RULE_LIST:
                sub_list = lxb_css_rule_list(rule);

                if (sub_list->first != NULL) {
                    child_rule = sub_list->first;

                    sub_list->first = NULL;
                    sub_list->last = NULL;
                }
                break;

            case LXB_CSS_RULE_STYLE:
                style = lxb_css_rule_style(rule);

                if (style->child != NULL) {
                    child_rule = lxb_css_rule(style->child);
                    style->child = NULL;
                }
                break;

            case LXB_CSS_RULE_BAD_STYLE:
                bad = lxb_css_rule_bad_style(rule);

                if (bad->child != NULL) {
                    child_rule = lxb_css_rule(bad->child);
                    bad->child = NULL;
                }
                break;

            case LXB_CSS_RULE_AT_RULE:
                at = lxb_css_rule_at(rule);

                if (at->type == LXB_CSS_AT_RULE_MEDIA) {
                    if (at->u.media != NULL && at->u.media->block != NULL) {
                        child_rule = lxb_css_rule(at->u.media->block);
                        at->u.media->block = NULL;
                    }
                }
                else if (at->type == LXB_CSS_AT_RULE__UNDEF) {
                    if (at->u.undef != NULL && at->u.undef->block != NULL) {
                        child_rule = lxb_css_rule(at->u.undef->block);
                        at->u.undef->block = NULL;
                    }
                }
                else if (at->type == LXB_CSS_AT_RULE__CUSTOM) {
                    if (at->u.custom != NULL && at->u.custom->block != NULL) {
                        child_rule = lxb_css_rule(at->u.custom->block);
                        at->u.custom->block = NULL;
                    }
                }
                else if (at->type == LXB_CSS_AT_RULE_FONT_FACE) {
                    if (at->u.font_face != NULL && at->u.font_face->block != NULL) {
                        child_rule = lxb_css_rule(at->u.font_face->block);
                        at->u.font_face->block = NULL;
                    }
                }
                break;

            case LXB_CSS_RULE_DECLARATION_LIST:
                declr_list = lxb_css_rule_declaration_list(rule);

                if (declr_list->first != NULL) {
                    child_rule = declr_list->first;

                    declr_list->first = NULL;
                    declr_list->last = NULL;
                }
                break;

            default:
                break;
        }

        if (child_rule != NULL) {
            rule = child_rule;
            continue;
        }

        next = rule->next;
        parent = rule->parent;

        (void) lxb_css_rule_destroy(rule, true);

        if (next != NULL) {
            rule = next;
        }
        else {
            if (parent == lxb_css_rule(list)) {
                break;
            }

            rule = parent;
        }
    }

    if (self_destroy) {
        return lexbor_mraw_free(memory->tree, list);
    }

    list->first = NULL;
    list->last = NULL;

    return list;
}

lxb_css_rule_at_t *
lxb_css_rule_at_destroy(lxb_css_rule_at_t *at, bool self_destroy)
{
    lxb_css_memory_t *memory = lxb_css_rule(at)->memory;

    switch (at->type) {
        case LXB_CSS_AT_RULE__UNDEF:
            (void) lxb_css_at_rule__undef_destroy(memory, at->u.undef, true);
            break;

        case LXB_CSS_AT_RULE_MEDIA:
            (void) lxb_css_at_rule_media_destroy(memory, at->u.media, true);
            break;

        case LXB_CSS_AT_RULE_NAMESPACE:
            (void) lxb_css_at_rule_namespace_destroy(memory, at->u.ns, true);
            break;

        case LXB_CSS_AT_RULE__LAST_ENTRY:
            break;
    }

    if (self_destroy) {
        return lexbor_mraw_free(memory->tree, at);
    }

    return at;
}

lxb_css_rule_style_t *
lxb_css_rule_style_destroy(lxb_css_rule_style_t *style, bool self_destroy)
{
    lxb_css_memory_t *memory = lxb_css_rule(style)->memory;

    lxb_css_selector_list_destroy(style->selector);
    (void) lxb_css_rule_declaration_list_destroy(style->declarations, true);
    (void) lxb_css_rule_list_destroy(style->child, true);

    style->selector = NULL;
    style->declarations = NULL;
    style->child = NULL;

    if (self_destroy) {
        return lexbor_mraw_free(memory->tree, style);
    }

    return style;
}

lxb_css_rule_bad_style_t *
lxb_css_rule_bad_style_destroy(lxb_css_rule_bad_style_t *bad, bool self_destroy)
{
    lxb_css_memory_t *memory = lxb_css_rule(bad)->memory;

    (void) lexbor_str_destroy(&bad->selectors, memory->mraw, false);
    bad->declarations = lxb_css_rule_declaration_list_destroy(bad->declarations,
                                                              true);
    bad->child = lxb_css_rule_list_destroy(bad->child, true);

    if (self_destroy) {
        return lexbor_mraw_free(memory->tree, bad);
    }

    return bad;
}

lxb_css_rule_declaration_list_t *
lxb_css_rule_declaration_list_destroy(lxb_css_rule_declaration_list_t *list,
                                      bool self_destroy)
{
    lxb_css_rule_t *declr, *next;

    if (list == NULL) {
        return NULL;
    }

    declr = list->first;

    while (declr != NULL) {
        next = declr->next;
        (void) lxb_css_rule_destroy(declr, true);
        declr = next;
    }

    if (self_destroy) {
        return lexbor_mraw_free(lxb_css_rule(list)->memory->tree, list);
    }

    list->first = NULL;
    list->last = NULL;

    return list;
}

lxb_css_rule_declaration_t *
lxb_css_rule_declaration_destroy(lxb_css_rule_declaration_t *declr,
                                 bool self_destroy)
{
    lxb_css_memory_t *memory = lxb_css_rule(declr)->memory;

    if (declr->u.user != NULL) {
        declr->u.user = lxb_css_property_destroy(memory, declr->u.user,
                                                 declr->type, true);
    }

    if (self_destroy) {
        return lexbor_mraw_free(memory->tree, declr);
    }

    return declr;
}

lxb_status_t
lxb_css_rule_declaration_serialize(const lxb_css_rule_declaration_t *declaration,
                                   lexbor_serialize_cb_f cb, void *ctx)
{
    lxb_status_t status;
    const lxb_css_entry_data_t *data, *undata;
    const lxb_css_property__undef_t *undef;
    const lxb_css_property__custom_t *custom;

    static const lxb_char_t cl_str[] = ": ";
    static const lxb_char_t imp_str[] = " !important";

    data = lxb_css_property_by_id(declaration->type);
    if (data == NULL) {
        return LXB_STATUS_ERROR_NOT_EXISTS;
    }

    if (declaration->type == LXB_CSS_PROPERTY__UNDEF) {
        undef = declaration->u.undef;

        if (undef->type > LXB_CSS_PROPERTY__CUSTOM) {
            undata = lxb_css_property_by_id(undef->type);
            if (undata == NULL) {
                return LXB_STATUS_ERROR_NOT_EXISTS;
            }

            lexbor_serialize_write(cb, undata->name, undata->length, ctx, status);
            lexbor_serialize_write(cb, cl_str, (sizeof(cl_str) - 1), ctx, status);
        }
    }
    else if (declaration->type == LXB_CSS_PROPERTY__CUSTOM) {
        custom = declaration->u.custom;

        lexbor_serialize_write(cb, custom->name.data, custom->name.length,
                               ctx, status);
        lexbor_serialize_write(cb, cl_str, (sizeof(cl_str) - 1), ctx, status);
    }
    else {
        lexbor_serialize_write(cb, data->name, data->length, ctx, status);
        lexbor_serialize_write(cb, cl_str, (sizeof(cl_str) - 1), ctx, status);
    }

    status = data->serialize(declaration->u.user, cb, ctx);
    if (status != LXB_STATUS_OK) {
        return status;
    }

    if (declaration->important && declaration->type != LXB_CSS_PROPERTY__UNDEF) {
        lexbor_serialize_write(cb, imp_str, (sizeof(imp_str) - 1), ctx, status);
    }

    return LXB_STATUS_OK;
}

lxb_status_t
lxb_css_rule_declaration_serialize_name(const lxb_css_rule_declaration_t *declr,
                                        lexbor_serialize_cb_f cb, void *ctx)
{
    return lxb_css_property_serialize_name(declr->u.user, declr->type, cb, ctx);
}
