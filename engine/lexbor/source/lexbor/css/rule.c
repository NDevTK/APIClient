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


/*
 * One rule tree is destroyed by one walk that holds no stack: a rule's owned
 * children are detached one at a time, so the walk descends into the detached
 * child, frees each rule once it owns none, and climbs back through the parent
 * link the detach wrote. Nesting depth (style rules inside style rules, blocks
 * inside at-rules) therefore costs neither C stack nor heap.
 */
static lxb_css_rule_t *
lxb_css_rule_child_detach(lxb_css_rule_t *rule)
{
    lxb_css_rule_t *child = NULL;
    lxb_css_rule_list_t *list;
    lxb_css_rule_declaration_list_t *declrs;
    lxb_css_rule_style_t *style;
    lxb_css_rule_bad_style_t *bad;
    lxb_css_rule_at_t *at;

    switch (rule->type) {
        case LXB_CSS_RULE_LIST:
            list = lxb_css_rule_list(rule);
            child = list->first;

            if (child != NULL) {
                list->first = child->next;

                if (list->first == NULL) {
                    list->last = NULL;
                }
            }
            break;

        case LXB_CSS_RULE_DECLARATION_LIST:
            declrs = lxb_css_rule_declaration_list(rule);
            child = declrs->first;

            if (child != NULL) {
                declrs->first = child->next;

                if (declrs->first == NULL) {
                    declrs->last = NULL;
                }
            }
            break;

        case LXB_CSS_RULE_STYLE:
            style = lxb_css_rule_style(rule);

            if (style->declarations != NULL) {
                child = lxb_css_rule(style->declarations);
                style->declarations = NULL;
            }
            else if (style->child != NULL) {
                child = lxb_css_rule(style->child);
                style->child = NULL;
            }
            break;

        case LXB_CSS_RULE_BAD_STYLE:
            bad = lxb_css_rule_bad_style(rule);

            if (bad->declarations != NULL) {
                child = lxb_css_rule(bad->declarations);
                bad->declarations = NULL;
            }
            else if (bad->child != NULL) {
                child = lxb_css_rule(bad->child);
                bad->child = NULL;
            }
            break;

        case LXB_CSS_RULE_AT_RULE:
            at = lxb_css_rule_at(rule);

            if (at->u.user == NULL) {
                break;
            }

            switch (at->type) {
                case LXB_CSS_AT_RULE__UNDEF:
                    child = lxb_css_rule(at->u.undef->block);
                    at->u.undef->block = NULL;
                    break;

                case LXB_CSS_AT_RULE__CUSTOM:
                    child = lxb_css_rule(at->u.custom->block);
                    at->u.custom->block = NULL;
                    break;

                case LXB_CSS_AT_RULE_MEDIA:
                    child = lxb_css_rule(at->u.media->block);
                    at->u.media->block = NULL;
                    break;

                case LXB_CSS_AT_RULE_FONT_FACE:
                    child = lxb_css_rule(at->u.font_face->block);
                    at->u.font_face->block = NULL;
                    break;

                case LXB_CSS_AT_RULE_NAMESPACE:
                case LXB_CSS_AT_RULE__LAST_ENTRY:
                    break;
            }
            break;

        case LXB_CSS_RULE_DECLARATION:
        case LXB_CSS_RULE_STYLESHEET:
        case LXB_CSS_RULE_UNDEF:
            break;
    }

    if (child != NULL) {
        child->next = NULL;
        child->prev = NULL;
        child->parent = rule;
    }

    return child;
}

/* Frees one rule's own record; every rule it owned is already detached. */
static void *
lxb_css_rule_record_destroy(lxb_css_rule_t *rule, bool self_destroy)
{
    lxb_css_memory_t *memory = rule->memory;
    lxb_css_rule_style_t *style;
    lxb_css_rule_bad_style_t *bad;

    switch (rule->type) {
        case LXB_CSS_RULE_LIST:
        case LXB_CSS_RULE_DECLARATION_LIST:
            break;

        case LXB_CSS_RULE_AT_RULE:
            return lxb_css_rule_at_destroy(lxb_css_rule_at(rule), self_destroy);

        case LXB_CSS_RULE_STYLE:
            style = lxb_css_rule_style(rule);

            lxb_css_selector_list_destroy(style->selector);
            style->selector = NULL;
            break;

        case LXB_CSS_RULE_BAD_STYLE:
            bad = lxb_css_rule_bad_style(rule);

            (void) lexbor_str_destroy(&bad->selectors, memory->mraw, false);
            break;

        case LXB_CSS_RULE_DECLARATION:
            return lxb_css_rule_declaration_destroy(lxb_css_rule_declaration(rule),
                                                    self_destroy);
        case LXB_CSS_RULE_STYLESHEET:
        case LXB_CSS_RULE_UNDEF:
            return NULL;
    }

    if (self_destroy) {
        return lexbor_mraw_free(memory->tree, rule);
    }

    return rule;
}

void *
lxb_css_rule_destroy(lxb_css_rule_t *rule, bool self_destroy)
{
    lxb_css_rule_t *node, *child, *parent;

    node = rule;

    for (;;) {
        child = lxb_css_rule_child_detach(node);

        if (child != NULL) {
            node = child;
            continue;
        }

        if (node == rule) {
            return lxb_css_rule_record_destroy(rule, self_destroy);
        }

        parent = node->parent;
        (void) lxb_css_rule_record_destroy(node, true);
        node = parent;
    }
}

lxb_css_rule_list_t *
lxb_css_rule_list_destroy(lxb_css_rule_list_t *list, bool self_destroy)
{
    if (list == NULL) {
        return NULL;
    }

    return lxb_css_rule_destroy(lxb_css_rule(list), self_destroy);
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
    return lxb_css_rule_destroy(lxb_css_rule(style), self_destroy);
}

lxb_css_rule_bad_style_t *
lxb_css_rule_bad_style_destroy(lxb_css_rule_bad_style_t *bad, bool self_destroy)
{
    return lxb_css_rule_destroy(lxb_css_rule(bad), self_destroy);
}

lxb_css_rule_declaration_list_t *
lxb_css_rule_declaration_list_destroy(lxb_css_rule_declaration_list_t *list,
                                      bool self_destroy)
{
    if (list == NULL) {
        return NULL;
    }

    return lxb_css_rule_destroy(lxb_css_rule(list), self_destroy);
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
