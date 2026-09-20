/*
 * Copyright (C) 2020 Alexander Borisov
 *
 * Author: Alexander Borisov <borisov@lexbor.com>
 */

#include "lexbor/core/serialize.h"
#include "lexbor/css/css.h"
#include "lexbor/css/selectors/selectors.h"
#include "lexbor/css/selectors/selector.h"
#include "lexbor/css/selectors/pseudo.h"
#include "lexbor/css/selectors/pseudo_const.h"
#include "lexbor/css/selectors/pseudo_state.h"
#include "lexbor/css/selectors/state.h"
#include "lexbor/css/selectors/pseudo_res.h"


typedef void
(*lxb_css_selector_destroy_f)(lxb_css_selector_t *selector,
                              lxb_css_memory_t *mem);
typedef lxb_status_t
(*lxb_css_selector_serialize_f)(lxb_css_selector_t *selector,
                                lexbor_serialize_cb_f cb, void *ctx);


static void
lxb_css_selector_destroy_undef(lxb_css_selector_t *selector,
                               lxb_css_memory_t *mem);
static void
lxb_css_selector_destroy_any(lxb_css_selector_t *selector,
                             lxb_css_memory_t *mem);
static void
lxb_css_selector_destroy_id(lxb_css_selector_t *selector,
                            lxb_css_memory_t *mem);
static void
lxb_css_selector_destroy_attribute(lxb_css_selector_t *selector,
                                   lxb_css_memory_t *mem);
static void
lxb_css_selector_destroy_pseudo_class_function(lxb_css_selector_t *selector,
                                               lxb_css_memory_t *mem);
static void
lxb_css_selector_destroy_pseudo_element_function(lxb_css_selector_t *selector,
                                                 lxb_css_memory_t *mem);

static lxb_status_t
lxb_css_selector_serialize_undef(lxb_css_selector_t *selector,
                                 lexbor_serialize_cb_f cb, void *ctx);
static lxb_status_t
lxb_css_selector_serialize_any(lxb_css_selector_t *selector,
                               lexbor_serialize_cb_f cb, void *ctx);
static lxb_status_t
lxb_css_selector_serialize_id(lxb_css_selector_t *selector,
                              lexbor_serialize_cb_f cb, void *ctx);
static lxb_status_t
lxb_css_selector_serialize_class(lxb_css_selector_t *selector,
                                 lexbor_serialize_cb_f cb, void *ctx);
static lxb_status_t
lxb_css_selector_serialize_attribute(lxb_css_selector_t *selector,
                                     lexbor_serialize_cb_f cb, void *ctx);
static lxb_status_t
lxb_css_selector_serialize_pseudo_class(lxb_css_selector_t *selector,
                                        lexbor_serialize_cb_f cb, void *ctx);
static lxb_status_t
lxb_css_selector_serialize_pseudo_class_function(lxb_css_selector_t *selector,
                                                 lexbor_serialize_cb_f cb, void *ctx);
static lxb_status_t
lxb_css_selector_serialize_pseudo_element(lxb_css_selector_t *selector,
                                          lexbor_serialize_cb_f cb, void *ctx);
static lxb_status_t
lxb_css_selector_serialize_pseudo_element_function(lxb_css_selector_t *selector,
                                                   lexbor_serialize_cb_f cb, void *ctx);

static lxb_status_t
lxb_css_selector_serialize_pseudo_single(lxb_css_selector_t *selector,
                                         lexbor_serialize_cb_f cb, void *ctx,
                                         bool is_class);


static const lxb_css_selector_destroy_f
                  lxb_selector_destroy_map[LXB_CSS_SELECTOR_TYPE__LAST_ENTRY] =
{
    lxb_css_selector_destroy_undef,
    lxb_css_selector_destroy_any,
    lxb_css_selector_destroy_any,
    lxb_css_selector_destroy_id,
    lxb_css_selector_destroy_id,
    lxb_css_selector_destroy_attribute,
    lxb_css_selector_destroy_undef,
    lxb_css_selector_destroy_pseudo_class_function,
    lxb_css_selector_destroy_undef,
    lxb_css_selector_destroy_pseudo_element_function
};

static const lxb_css_selector_serialize_f
                lxb_selector_serialize_map[LXB_CSS_SELECTOR_TYPE__LAST_ENTRY] =
{
    lxb_css_selector_serialize_undef,
    lxb_css_selector_serialize_any,
    lxb_css_selector_serialize_any,
    lxb_css_selector_serialize_id,
    lxb_css_selector_serialize_class,
    lxb_css_selector_serialize_attribute,
    lxb_css_selector_serialize_pseudo_class,
    lxb_css_selector_serialize_pseudo_class_function,
    lxb_css_selector_serialize_pseudo_element,
    lxb_css_selector_serialize_pseudo_element_function
};


lxb_css_selector_t *
lxb_css_selector_create(lxb_css_selector_list_t *list)
{
    lxb_css_selector_t *selector = lexbor_dobject_calloc(list->memory->objs);
    if (selector == NULL) {
        return NULL;
    }

    selector->list = list;

    return selector;
}

void
lxb_css_selector_destroy(lxb_css_selector_t *selector)
{
    lxb_css_memory_t *memory;

    if (selector != NULL) {
        memory = selector->list->memory;

        lxb_selector_destroy_map[selector->type](selector, memory);
        lexbor_dobject_free(memory->objs, selector);
    }
}

void
lxb_css_selector_destroy_chain(lxb_css_selector_t *selector)
{
    lxb_css_selector_t *next;

    while (selector != NULL) {
        next = selector->next;
        lxb_css_selector_destroy(selector);
        selector = next;
    }
}

void
lxb_css_selector_remove(lxb_css_selector_t *selector)
{
    if (selector->next != NULL) {
        selector->next->prev = selector->prev;
    }

    if (selector->prev != NULL) {
        selector->prev->next = selector->next;
    }

    if (selector->list->first == selector) {
        selector->list->first = selector->next;
    }

    if (selector->list->last == selector) {
        selector->list->last = selector->prev;
    }
}

lxb_css_selector_list_t *
lxb_css_selector_list_create(lxb_css_memory_t *mem)
{
    lxb_css_selector_list_t *list;

    list = lexbor_dobject_calloc(mem->objs);
    if (list == NULL) {
        return NULL;
    }

    list->memory = mem;

    return list;
}

void
lxb_css_selector_list_remove(lxb_css_selector_list_t *list)
{
    if (list->next != NULL) {
        list->next->prev = list->prev;
    }

    if (list->prev != NULL) {
        list->prev->next = list->next;
    }
}

void
lxb_css_selector_list_selectors_remove(lxb_css_selectors_t *selectors,
                                       lxb_css_selector_list_t *list)
{
    lxb_css_selector_list_remove(list);

    if (selectors->list == list) {
        selectors->list = list->next;
    }

    if (selectors->list_last == list) {
        selectors->list_last = list->prev;
    }
}

void
lxb_css_selector_list_destroy(lxb_css_selector_list_t *list)
{
    if (list != NULL) {
        lxb_css_selector_destroy_chain(list->first);
        lexbor_dobject_free(list->memory->objs, list);
    }
}

void
lxb_css_selector_list_destroy_chain(lxb_css_selector_list_t *list)
{
    lxb_css_selector_list_t *next;

    while (list != NULL) {
        next = list->next;
        lxb_css_selector_list_destroy(list);
        list = next;
    }
}

void
lxb_css_selector_list_destroy_memory(lxb_css_selector_list_t *list)
{
    if (list != NULL) {
        (void) lxb_css_memory_destroy(list->memory, true);
    }
}

static void
lxb_css_selector_destroy_undef(lxb_css_selector_t *selector,
                               lxb_css_memory_t *mem)
{
    /* Do nothing. */
}

static void
lxb_css_selector_destroy_any(lxb_css_selector_t *selector,
                             lxb_css_memory_t *mem)
{
    if (selector->ns.data != NULL) {
        lexbor_mraw_free(mem->mraw, selector->ns.data);
    }

    if (selector->name.data != NULL) {
        lexbor_mraw_free(mem->mraw, selector->name.data);
    }
}

static void
lxb_css_selector_destroy_id(lxb_css_selector_t *selector,
                            lxb_css_memory_t *mem)
{
    if (selector->name.data != NULL) {
        (void) lexbor_mraw_free(mem->mraw, selector->name.data);
    }
}

static void
lxb_css_selector_destroy_attribute(lxb_css_selector_t *selector,
                                   lxb_css_memory_t *mem)
{
    if (selector->ns.data != NULL) {
        lexbor_mraw_free(mem->mraw, selector->ns.data);
    }

    if (selector->name.data != NULL) {
        lexbor_mraw_free(mem->mraw, selector->name.data);
    }

    if (selector->u.attribute.value.data != NULL) {
        lexbor_mraw_free(mem->mraw, selector->u.attribute.value.data);
    }
}

static void
lxb_css_selector_destroy_pseudo_class_function(lxb_css_selector_t *selector,
                                               lxb_css_memory_t *mem)
{
    lxb_css_selector_anb_of_t *anbof;
    lxb_css_selector_pseudo_t *pseudo;

    pseudo = &selector->u.pseudo;

    switch (pseudo->type) {
        case LXB_CSS_SELECTOR_PSEUDO_CLASS_FUNCTION_CURRENT:
            break;
        case LXB_CSS_SELECTOR_PSEUDO_CLASS_FUNCTION_DIR:
            break;

        case LXB_CSS_SELECTOR_PSEUDO_CLASS_FUNCTION_HAS:
        case LXB_CSS_SELECTOR_PSEUDO_CLASS_FUNCTION_IS:
            lxb_css_selector_list_destroy_chain(pseudo->data);
            break;

        case LXB_CSS_SELECTOR_PSEUDO_CLASS_FUNCTION_LANG:
            break;

        case LXB_CSS_SELECTOR_PSEUDO_CLASS_FUNCTION_NOT:
            lxb_css_selector_list_destroy_chain(pseudo->data);
            break;

        case LXB_CSS_SELECTOR_PSEUDO_CLASS_FUNCTION_NTH_CHILD:
        case LXB_CSS_SELECTOR_PSEUDO_CLASS_FUNCTION_NTH_COL:
        case LXB_CSS_SELECTOR_PSEUDO_CLASS_FUNCTION_NTH_LAST_CHILD:
        case LXB_CSS_SELECTOR_PSEUDO_CLASS_FUNCTION_NTH_LAST_COL:
        case LXB_CSS_SELECTOR_PSEUDO_CLASS_FUNCTION_NTH_LAST_OF_TYPE:
        case LXB_CSS_SELECTOR_PSEUDO_CLASS_FUNCTION_NTH_OF_TYPE:
            anbof = pseudo->data;

            if (anbof != NULL) {
                lxb_css_selector_list_destroy_chain(anbof->of);
                lexbor_mraw_free(mem->mraw, anbof);
            }
            break;

        case LXB_CSS_SELECTOR_PSEUDO_CLASS_FUNCTION_WHERE:
            lxb_css_selector_list_destroy_chain(pseudo->data);
            break;

        default:
            break;
    }
}

static void
lxb_css_selector_destroy_pseudo_element_function(lxb_css_selector_t *selector,
                                                 lxb_css_memory_t *mem)
{

}

lxb_status_t
lxb_css_selector_serialize(lxb_css_selector_t *selector,
                           lexbor_serialize_cb_f cb, void *ctx)
{
    return lxb_selector_serialize_map[selector->type](selector, cb, ctx);
}

lxb_status_t
lxb_css_selector_serialize_chain(lxb_css_selector_t *selector,
                                 lexbor_serialize_cb_f cb, void *ctx)
{
    size_t length;
    lxb_char_t *data;
    lxb_status_t status;

    if (selector == NULL) {
        return LXB_STATUS_OK;
    }

    if (selector->combinator > LXB_CSS_SELECTOR_COMBINATOR_CLOSE) {
        data = lxb_css_selector_combinator(selector, &length);
        if (data == NULL) {
            return LXB_STATUS_ERROR_UNEXPECTED_DATA;
        }

        lxb_css_selector_serialize_write(data, length);
        lxb_css_selector_serialize_write(" ", 1);
    }

    status = lxb_css_selector_serialize(selector, cb, ctx);
    if (status != LXB_STATUS_OK) {
        return status;
    }

    selector = selector->next;

    while (selector != NULL) {
        data = lxb_css_selector_combinator(selector, &length);
        if (data == NULL) {
            return LXB_STATUS_ERROR_UNEXPECTED_DATA;
        }

        if (length != 0) {
            lxb_css_selector_serialize_write(" ", 1);

            if (*data != ' ') {
                lxb_css_selector_serialize_write(data, length);
                lxb_css_selector_serialize_write(" ", 1);
            }
        }

        status = lxb_css_selector_serialize(selector, cb, ctx);
        if (status != LXB_STATUS_OK) {
            return status;
        }

        selector = selector->next;
    }

    return LXB_STATUS_OK;
}

lxb_char_t *
lxb_css_selector_serialize_chain_char(lxb_css_selector_t *selector,
                                      size_t *out_length)
{
    size_t length = 0;
    lxb_status_t status;
    lexbor_str_t str;

    status = lxb_css_selector_serialize_chain(selector, lexbor_serialize_length_cb,
                                              &length);
    if (status != LXB_STATUS_OK) {
        goto failed;
    }

    /* + 1 == '\0' */
    str.data = lexbor_malloc(length + 1);
    if (str.data == NULL) {
        goto failed;
    }

    str.length = 0;

    status = lxb_css_selector_serialize_chain(selector, lexbor_serialize_copy_cb,
                                              &str);
    if (status != LXB_STATUS_OK) {
        lexbor_free(str.data);
        goto failed;
    }

    str.data[str.length] = '\0';

    if (out_length != NULL) {
        *out_length = str.length;
    }

    return str.data;

failed:

    if (out_length != NULL) {
        *out_length = 0;
    }

    return NULL;
}

lxb_status_t
lxb_css_selector_serialize_list(lxb_css_selector_list_t *list,
                                 lexbor_serialize_cb_f cb, void *ctx)
{
    if (list != NULL) {
        return lxb_css_selector_serialize_chain(list->first, cb, ctx);
    }

    return LXB_STATUS_OK;
}

lxb_char_t *
lxb_css_selector_serialize_list_char(lxb_css_selector_list_t *list,
                                      size_t *out_length)
{
    size_t length = 0;
    lxb_status_t status;
    lexbor_str_t str;

    status = lxb_css_selector_serialize_list_chain(list, lexbor_serialize_length_cb,
                                                   &length);
    if (status != LXB_STATUS_OK) {
        goto failed;
    }

    /* + 1 == '\0' */
    str.data = lexbor_malloc(length + 1);
    if (str.data == NULL) {
        goto failed;
    }

    str.length = 0;

    status = lxb_css_selector_serialize_list_chain(list, lexbor_serialize_copy_cb,
                                                   &str);
    if (status != LXB_STATUS_OK) {
        lexbor_free(str.data);
        goto failed;
    }

    str.data[str.length] = '\0';

    if (out_length != NULL) {
        *out_length = str.length;
    }

    return str.data;

failed:

    if (out_length != NULL) {
        *out_length = 0;
    }

    return NULL;
}

lxb_status_t
lxb_css_selector_serialize_list_chain(lxb_css_selector_list_t *list,
                                      lexbor_serialize_cb_f cb, void *ctx)
{
    lxb_status_t status;

    if (list == NULL) {
        return LXB_STATUS_OK;
    }

    status = lxb_css_selector_serialize_chain(list->first, cb, ctx);
    if (status != LXB_STATUS_OK) {
        return status;
    }

    list = list->next;

    while (list != NULL) {
        lxb_css_selector_serialize_write(", ", 2);

        status = lxb_css_selector_serialize_chain(list->first, cb, ctx);
        if (status != LXB_STATUS_OK) {
            return status;
        }

        list = list->next;
    }

    return LXB_STATUS_OK;
}

lxb_char_t *
lxb_css_selector_serialize_list_chain_char(lxb_css_selector_list_t *list,
                                           size_t *out_length)
{
    size_t length = 0;
    lxb_status_t status;
    lexbor_str_t str;

    status = lxb_css_selector_serialize_list_chain(list, lexbor_serialize_length_cb,
                                                   &length);
    if (status != LXB_STATUS_OK) {
        goto failed;
    }

    /* + 1 == '\0' */
    str.data = lexbor_malloc(length + 1);
    if (str.data == NULL) {
        goto failed;
    }

    str.length = 0;

    status = lxb_css_selector_serialize_list_chain(list, lexbor_serialize_copy_cb,
                                                   &str);
    if (status != LXB_STATUS_OK) {
        lexbor_free(str.data);
        goto failed;
    }

    str.data[str.length] = '\0';

    if (out_length != NULL) {
        *out_length = str.length;
    }

    return str.data;

failed:

    if (out_length != NULL) {
        *out_length = 0;
    }

    return NULL;
}


static lxb_status_t
lxb_css_selector_serialize_undef(lxb_css_selector_t *selector,
                                 lexbor_serialize_cb_f cb, void *ctx)
{
    return LXB_STATUS_ERROR_UNEXPECTED_DATA;
}

/* CSSOM §2.1 "Common Serializing Idioms"' SERIALIZE AN IDENTIFIER. It was not performed anywhere in this
   file, and it is what makes a selector ROUND-TRIP: CSS Syntax 3 §4.3.7 "Consume an escaped code point" runs
   in the TOKENIZER, so the ident this selector holds is the UNESCAPED text — `.\!p-0` arrives as the class
   name `!p-0` and `.bottom-100\%` as `bottom-100%` — and writing those bytes back produces `.!p-0`, which
   is not a selector at all. A re-parse of such a serialization fails its prelude and lexbor reports the
   rule as LXB_CSS_RULE_BAD_STYLE, so the rule silently changes KIND while keeping its position.
   EVERY IDENTIFIER-BEARING COMPONENT OF CSSOM §5.2 "Serializing Selectors"' SERIALIZE A SIMPLE SELECTOR GOES
   THROUGH HERE, in that algorithm's own words: "Append a "." (U+002E), followed by the serialization of
   the class name as an identifier to s", "Append a "#" (U+0023), followed by the serialization of the ID
   as an identifier to s", "If this is a type selector append the serialization of the element name as an
   identifier to s",
   "Append the serialization of the attribute name as an identifier to s", and the namespace prefix "as an
   identifier".
   THE RULES BELOW ARE THE STANDARD'S OWN AND IN ITS ORDER, and CSSOM §2.1's two escape idioms are
   DIFFERENT THINGS. Both open with U+005C; where they differ is what follows, and the standard's words for
   the first are "followed by the character" while the second writes the code point — "followed by the
   Unicode code point as the smallest possible number of hexadecimal digits in the range 0-9 a-f (U+0030 to
   U+0039 and U+0061 to U+0066) to represent the code point in base 16, followed by a single SPACE
   (U+0020)". (Neither sentence is quoted whole: each opens by naming the U+005C inside its own quotation
   marks, so a verbatim run would close here at that mark and reach the audit as a fragment.)
   THE WALK STEPS BY UTF-8 SEQUENCE AND NOT BY BYTE, because "the first character" and "the second
   character" are the standard's words: a name whose first character is multi-byte would otherwise have its
   second BYTE judged by a rule written about its second CHARACTER. A code point at or above U+0080 is
   copied raw by the standard's own next-to-last rule, so the sequence never has to be decoded. */
static lxb_status_t
lxb_css_selector_serialize_ident(const lxb_char_t *data, size_t length,
                                 lexbor_serialize_cb_f cb, void *ctx)
{
    static const lxb_char_t hex[] = "0123456789abcdef";

    lxb_char_t buf[4];
    lxb_status_t status;
    size_t i, n, k, nth;
    lxb_char_t cp;

    i = 0;
    nth = 0;

    while (i < length) {
        cp = data[i];

        if (cp >= 0x80) {
            n = (cp >= 0xF0) ? 4 : (cp >= 0xE0) ? 3 : (cp >= 0xC0) ? 2 : 1;

            if (i + n > length) {
                n = 1;
            }

            lxb_css_selector_serialize_write(data + i, n);

            i += n;
            nth += 1;
            continue;
        }

        if (cp == 0x00) {
            /* "If the character is NULL (U+0000), then the REPLACEMENT CHARACTER (U+FFFD)." */
            lxb_css_selector_serialize_write("\xEF\xBF\xBD", 3);
        }
        else if (cp <= 0x1F || cp == 0x7F
                 || (nth == 0 && cp >= '0' && cp <= '9')
                 || (nth == 1 && cp >= '0' && cp <= '9' && data[0] == '-'))
        {
            k = 0;
            buf[k++] = '\\';

            if (cp >= 0x10) {
                buf[k++] = hex[(cp >> 4) & 0x0F];
            }

            buf[k++] = hex[cp & 0x0F];
            buf[k++] = ' ';

            lxb_css_selector_serialize_write(buf, k);
        }
        /* The lone "-" of the fifth rule ("is the first character and is a "-" (U+002D), and there is no
           second character"), and the standard's final `Otherwise` — one branch because the rule between
           them, which keeps "-" itself, is exactly what the second disjunct negates. */
        else if ((nth == 0 && cp == '-' && length == 1)
                 || !(cp == '-' || cp == '_'
                      || (cp >= '0' && cp <= '9')
                      || (cp >= 'A' && cp <= 'Z')
                      || (cp >= 'a' && cp <= 'z')))
        {
            buf[0] = '\\';
            buf[1] = cp;

            lxb_css_selector_serialize_write(buf, 2);
        }
        else {
            lxb_css_selector_serialize_write(data + i, 1);
        }

        i += 1;
        nth += 1;
    }

    return LXB_STATUS_OK;
}

static lxb_status_t
lxb_css_selector_serialize_any(lxb_css_selector_t *selector,
                               lexbor_serialize_cb_f cb, void *ctx)
{
    lxb_status_t status;

    if (selector->ns.data != NULL) {
        /* A "*" PREFIX IS THIS PARSER'S OWN LITERAL AND NOT AN IDENTIFIER, and CSSOM §5.2 asks for an
           identifier only for a prefix that NAMES a namespace: the two namespace states build a
           one-byte "*" by hand for `*|E` and for the universal selector, so escaping it would write `\*|E`
           and change which elements the selector matches.
           NAMED RESIDUAL — A PREFIX A PAGE WROTE AS `\*` IS INDISTINGUISHABLE FROM IT. WHAT IS NOT COVERED:
           an escaped `\*` ident reaches `ns` as the same one byte this parser writes for the any-namespace
           prefix, so it is emitted unescaped. WHAT THE NEXT DIFF BUILDS: a bit on lxb_css_selector_t set by
           the two states that synthesize the literal, so the test is on how the prefix was PRODUCED rather
           than on what it spells. HOW ITS ABSENCE WOULD SHOW, as an observation: a rule whose prefix was
           written `\*|E` serializes as `*|E`, so it reads back through CSSOM naming any namespace and
           matches elements the page's own selector excluded. */
        if (selector->ns.length == 1 && selector->ns.data[0] == '*') {
            lxb_css_selector_serialize_write(selector->ns.data,
                                             selector->ns.length);
        }
        else {
            status = lxb_css_selector_serialize_ident(selector->ns.data,
                                                      selector->ns.length,
                                                      cb, ctx);
            if (status != LXB_STATUS_OK) {
                return status;
            }
        }

        lxb_css_selector_serialize_write("|", 1);
    }

    if (selector->name.data != NULL) {
        /* CSSOM §5.2: "If this is a universal selector append "*" (U+002A) to s" — the standard reaches for
           the literal rather than for an identifier, and so does this parser: a LXB_CSS_SELECTOR_TYPE_ANY
           carries a hand-built one-byte "*" as its name. An element name, and the attribute name this entry
           also
           serves, are identifiers. */
        if (selector->type == LXB_CSS_SELECTOR_TYPE_ANY) {
            return cb(selector->name.data, selector->name.length, ctx);
        }

        return lxb_css_selector_serialize_ident(selector->name.data,
                                                selector->name.length,
                                                cb, ctx);
    }

    return LXB_STATUS_OK;
}

static lxb_status_t
lxb_css_selector_serialize_id(lxb_css_selector_t *selector,
                              lexbor_serialize_cb_f cb, void *ctx)
{
    lxb_status_t status;

    lxb_css_selector_serialize_write("#", 1);

    if (selector->name.data != NULL) {
        return lxb_css_selector_serialize_ident(selector->name.data,
                                                selector->name.length,
                                                cb, ctx);
    }

    return LXB_STATUS_OK;
}

static lxb_status_t
lxb_css_selector_serialize_class(lxb_css_selector_t *selector,
                                 lexbor_serialize_cb_f cb, void *ctx)
{
    lxb_status_t status;

    lxb_css_selector_serialize_write(".", 1);

    if (selector->name.data != NULL) {
        return lxb_css_selector_serialize_ident(selector->name.data,
                                                selector->name.length,
                                                cb, ctx);
    }

    return LXB_STATUS_OK;
}

/* CSSOM §2.1 "Common Serializing Idioms"' SERIALIZE A STRING, which CSSOM §5.2 "Serializing Selectors"'
   serialize a simple selector asks for by name: "followed by the serialization of the attribute value as a
   string". THE REVERSE SOLIDUS WAS NOT ESCAPED, and that is the same round-trip break the identifier
   serializer above fixes, at the sibling site: the tokenizer consumes the escape, so `[x="a\\b"]` arrives
   holding `a\b` and writing those bytes back
   produces `[x="a\b"]`, whose re-parse consumes `\b` as an escape and yields `ab`. A NEWLINE WAS NOT ESCAPED
   EITHER, and CSS Syntax 3 §4.3.5 "Consume a string token" ends the string on one: "newline: This is a parse
   error. Reconsume the current input code point, create a <bad-string-token>, and return it." The rest of C0
   is escaped because CSSOM asks for it and not to keep a parse alive.
   THE RULES ARE CSSOM §2.1's OWN: "If the character is NULL (U+0000), then the REPLACEMENT CHARACTER
   (U+FFFD). If the character is in the range [\1-\1f] (U+0001 to U+001F) or is U+007F, the character
   escaped as code point. If the character is '"' (U+0022) or "\" (U+005C), the escaped character.
   Otherwise, the character itself." The quotation mark used to go out as `\000022`, which re-parses to the
   same string and is what no browser writes; the standard's own escaped-character form is `\"`.
   NO UTF-8 STEPPING IS NEEDED HERE, unlike the identifier serializer: none of these rules asks which
   CHARACTER position this is, and every byte at or above 0x80 falls to the last rule and is copied. */
static lxb_status_t
lxb_css_selector_serialize_escape_write(lxb_char_t *p, lxb_char_t *end,
                                        lexbor_serialize_cb_f cb, void *ctx)
{
    static const lxb_char_t hex[] = "0123456789abcdef";

    lxb_char_t buf[4];
    lxb_char_t *begin;
    lxb_status_t status;
    size_t k;

    begin = p;

    lxb_css_selector_serialize_write("\"", 1);

    while (p < end) {
        if (*p == 0x00 || *p <= 0x1F || *p == 0x7F || *p == '"' || *p == '\\') {
            if (begin < p) {
                lxb_css_selector_serialize_write(begin, p - begin);
            }

            if (*p == 0x00) {
                lxb_css_selector_serialize_write("\xEF\xBF\xBD", 3);
            }
            else if (*p == '"' || *p == '\\') {
                buf[0] = '\\';
                buf[1] = *p;

                lxb_css_selector_serialize_write(buf, 2);
            }
            else {
                k = 0;
                buf[k++] = '\\';

                if (*p >= 0x10) {
                    buf[k++] = hex[(*p >> 4) & 0x0F];
                }

                buf[k++] = hex[*p & 0x0F];
                buf[k++] = ' ';

                lxb_css_selector_serialize_write(buf, k);
            }

            begin = p + 1;
        }

        p++;
    }

    if (begin < p) {
        lxb_css_selector_serialize_write(begin, p - begin);
    }

    lxb_css_selector_serialize_write("\"", 1);

    return LXB_STATUS_OK;
}

static lxb_status_t
lxb_css_selector_serialize_attribute(lxb_css_selector_t *selector,
                                     lexbor_serialize_cb_f cb, void *ctx)
{
    lxb_char_t *p, *end;
    lxb_status_t status;
    lxb_css_selector_attribute_t *attr;

    lxb_css_selector_serialize_write("[", 1);

    status = lxb_css_selector_serialize_any(selector, cb, ctx);
    if (status != LXB_STATUS_OK) {
        return status;
    }

    attr = &selector->u.attribute;

    if (attr->value.data == NULL) {
        return cb((lxb_char_t *) "]", 1, ctx);
    }

    switch (attr->match) {
        case LXB_CSS_SELECTOR_MATCH_EQUAL:
            lxb_css_selector_serialize_write("=", 1);
            break;
        case LXB_CSS_SELECTOR_MATCH_INCLUDE:
            lxb_css_selector_serialize_write("~=", 2);
            break;
        case LXB_CSS_SELECTOR_MATCH_DASH:
            lxb_css_selector_serialize_write("|=", 2);
            break;
        case LXB_CSS_SELECTOR_MATCH_PREFIX:
            lxb_css_selector_serialize_write("^=", 2);
            break;
        case LXB_CSS_SELECTOR_MATCH_SUFFIX:
            lxb_css_selector_serialize_write("$=", 2);
            break;
        case LXB_CSS_SELECTOR_MATCH_SUBSTRING:
            lxb_css_selector_serialize_write("*=", 2);
            break;

        default:
            return LXB_STATUS_ERROR_UNEXPECTED_DATA;
    }

    p = attr->value.data;
    end = attr->value.data + attr->value.length;

    status = lxb_css_selector_serialize_escape_write(p, end, cb, ctx);
    if (status != LXB_STATUS_OK) {
        return status;
    }

    if (attr->modifier != LXB_CSS_SELECTOR_MODIFIER_UNSET) {
        switch (attr->modifier) {
            case LXB_CSS_SELECTOR_MODIFIER_I:
                lxb_css_selector_serialize_write("i", 1);
                break;

            case LXB_CSS_SELECTOR_MODIFIER_S:
                lxb_css_selector_serialize_write("s", 1);
                break;

            default:
                return LXB_STATUS_ERROR_UNEXPECTED_DATA;
        }
    }

    return cb((lxb_char_t *) "]", 1, ctx);
}

static lxb_status_t
lxb_css_selector_serialize_pseudo_class(lxb_css_selector_t *selector,
                                        lexbor_serialize_cb_f cb, void *ctx)
{
    return lxb_css_selector_serialize_pseudo_single(selector, cb, ctx, true);
}

static lxb_status_t
lxb_css_selector_serialize_pseudo_class_function(lxb_css_selector_t *selector,
                                                 lexbor_serialize_cb_f cb, void *ctx)
{
    lxb_status_t status;
    lxb_char_t *p, *end;
    lxb_css_selector_pseudo_t *pseudo;
    lxb_css_selector_contains_t *contains;
    const lxb_css_selectors_pseudo_data_func_t *pfunc;

    pseudo = &selector->u.pseudo;

    pfunc = &lxb_css_selectors_pseudo_data_pseudo_class_function[pseudo->type];

    lxb_css_selector_serialize_write(":", 1);
    lxb_css_selector_serialize_write(pfunc->name, pfunc->length);
    lxb_css_selector_serialize_write("(", 1);

    switch (pseudo->type) {
        case LXB_CSS_SELECTOR_PSEUDO_CLASS_FUNCTION_CURRENT:
            break;
        case LXB_CSS_SELECTOR_PSEUDO_CLASS_FUNCTION_DIR:
            break;

        case LXB_CSS_SELECTOR_PSEUDO_CLASS_FUNCTION_HAS:
        case LXB_CSS_SELECTOR_PSEUDO_CLASS_FUNCTION_IS:
            status = lxb_css_selector_serialize_list_chain(pseudo->data,
                                                           cb, ctx);
            break;
        case LXB_CSS_SELECTOR_PSEUDO_CLASS_FUNCTION_LANG:
            break;

        case LXB_CSS_SELECTOR_PSEUDO_CLASS_FUNCTION_NOT:
            status = lxb_css_selector_serialize_list_chain(pseudo->data,
                                                           cb, ctx);
            break;

        case LXB_CSS_SELECTOR_PSEUDO_CLASS_FUNCTION_NTH_CHILD:
        case LXB_CSS_SELECTOR_PSEUDO_CLASS_FUNCTION_NTH_COL:
        case LXB_CSS_SELECTOR_PSEUDO_CLASS_FUNCTION_NTH_LAST_CHILD:
        case LXB_CSS_SELECTOR_PSEUDO_CLASS_FUNCTION_NTH_LAST_COL:
        case LXB_CSS_SELECTOR_PSEUDO_CLASS_FUNCTION_NTH_LAST_OF_TYPE:
        case LXB_CSS_SELECTOR_PSEUDO_CLASS_FUNCTION_NTH_OF_TYPE:
            status = LXB_STATUS_OK;

            if (pseudo->data != NULL) {
                status = lxb_css_selector_serialize_anb_of(pseudo->data,
                                                           cb, ctx);
            }
            break;

        case LXB_CSS_SELECTOR_PSEUDO_CLASS_FUNCTION_WHERE:
            status = lxb_css_selector_serialize_list_chain(pseudo->data,
                                                           cb, ctx);
            break;

        case LXB_CSS_SELECTOR_PSEUDO_CLASS_FUNCTION_LEXBOR_CONTAINS:
            contains = pseudo->data;
            p = contains->str.data;
            end = p + contains->str.length;

            status = lxb_css_selector_serialize_escape_write(p, end, cb, ctx);
            if (status != LXB_STATUS_OK) {
                return status;
            }

            if (contains->insensitive) {
                lxb_css_selector_serialize_write(" i", 2);
            }

            break;

        default:
            status = LXB_STATUS_OK;
            break;
    }

    if (status != LXB_STATUS_OK) {
        return status;
    }

    lxb_css_selector_serialize_write(")", 1);

    return LXB_STATUS_OK;
}

static lxb_status_t
lxb_css_selector_serialize_pseudo_element(lxb_css_selector_t *selector,
                                          lexbor_serialize_cb_f cb, void *ctx)
{
    return lxb_css_selector_serialize_pseudo_single(selector, cb, ctx, false);
}

static lxb_status_t
lxb_css_selector_serialize_pseudo_element_function(lxb_css_selector_t *selector,
                                                   lexbor_serialize_cb_f cb, void *ctx)
{
    return LXB_STATUS_OK;
}

static lxb_status_t
lxb_css_selector_serialize_pseudo_single(lxb_css_selector_t *selector,
                                         lexbor_serialize_cb_f cb, void *ctx,
                                         bool is_class)
{
    lxb_status_t status;
    lxb_css_selector_pseudo_t *pseudo;
    const lxb_css_selectors_pseudo_data_t *pclass;

    pseudo = &selector->u.pseudo;

    if (is_class) {
        pclass = &lxb_css_selectors_pseudo_data_pseudo_class[pseudo->type];
        lxb_css_selector_serialize_write(":", 1);
    }
    else {
        pclass = &lxb_css_selectors_pseudo_data_pseudo_element[pseudo->type];
        lxb_css_selector_serialize_write("::", 2);
    }

    lxb_css_selector_serialize_write(pclass->name, pclass->length);

    return LXB_STATUS_OK;
}

lxb_status_t
lxb_css_selector_serialize_anb_of(lxb_css_selector_anb_of_t *anbof,
                                  lexbor_serialize_cb_f cb, void *ctx)
{
    lxb_status_t status;

    static const lxb_char_t of[] = " of ";

    status = lxb_css_syntax_anb_serialize(&anbof->anb, cb, ctx);
    if (status != LXB_STATUS_OK) {
        return status;
    }

    if (anbof->of != NULL) {
        lxb_css_selector_serialize_write(of, sizeof(of) - 1);

        return lxb_css_selector_serialize_list_chain(anbof->of, cb, ctx);
    }

    return LXB_STATUS_OK;
}

lxb_char_t *
lxb_css_selector_combinator(lxb_css_selector_t *selector, size_t *out_length)
{
    switch (selector->combinator) {
        case LXB_CSS_SELECTOR_COMBINATOR_DESCENDANT:
            if (out_length != NULL) {*out_length = 1;}
            return (lxb_char_t *) " ";

        case LXB_CSS_SELECTOR_COMBINATOR_CLOSE:
            if (out_length != NULL) {*out_length = 0;}
            return (lxb_char_t *) "";

        case LXB_CSS_SELECTOR_COMBINATOR_CHILD:
            if (out_length != NULL) {*out_length = 1;}
            return (lxb_char_t *) ">";

        case LXB_CSS_SELECTOR_COMBINATOR_SIBLING:
            if (out_length != NULL) {*out_length = 1;}
            return (lxb_char_t *) "+";

        case LXB_CSS_SELECTOR_COMBINATOR_FOLLOWING:
            if (out_length != NULL) {*out_length = 1;}
            return (lxb_char_t *) "~";

        case LXB_CSS_SELECTOR_COMBINATOR_CELL:
            if (out_length != NULL) {*out_length = 2;}
            return (lxb_char_t *) "||";

        default:
            if (out_length != NULL) {*out_length = 0;}
            return NULL;
    }
}

void
lxb_css_selector_list_append(lxb_css_selector_list_t *list,
                             lxb_css_selector_t *selector)
{
    selector->prev = list->last;

    if (list->last != NULL) {
        list->last->next = selector;
    }
    else {
        list->first = selector;
    }

    list->last = selector;
}

void
lxb_css_selector_append_next(lxb_css_selector_t *dist, lxb_css_selector_t *src)
{
    if (dist->next != NULL) {
        dist->next->prev = src;
    }

    src->prev = dist;
    src->next = dist->next;

    dist->next = src;
}

void
lxb_css_selector_list_append_next(lxb_css_selector_list_t *dist,
                                  lxb_css_selector_list_t *src)
{
    if (dist->next != NULL) {
        dist->next->prev = src;
    }

    src->prev = dist;
    src->next = dist->next;

    dist->next = src;
}
