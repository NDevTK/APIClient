/*
 * Copyright (C) 2026 Alexander Borisov
 *
 * Author: Alexander Borisov <borisov@lexbor.com>
 */

#include "lexbor/html/tag.h"
#include "lexbor/html/tag_res.h"

#include <string.h>


lxb_tag_id_t
lxb_html_tag_svg_static(lexbor_hash_t *hash, lxb_tag_id_t tag_id)
{
    const lxb_tag_data_t *data;
    const lxb_html_tag_fixname_t *fix;
    lxb_tag_id_t low;

    if (tag_id < LXB_TAG__LAST_ENTRY) {
        return lxb_html_tag_res_fixname_svg[tag_id].name != NULL
               ? LXB_TAG__UNDEF : tag_id;
    }

    low = lxb_tag_id_lower(hash, tag_id);
    if (low == LXB_TAG__UNDEF || low >= LXB_TAG__LAST_ENTRY) {
        return tag_id;
    }

    data = lxb_tag_data_by_id(tag_id);
    fix = &lxb_html_tag_res_fixname_svg[low];

    if (data != NULL && fix->name != NULL && fix->len == data->entry.length
        && memcmp(fix->name, lexbor_hash_entry_str(&data->entry), fix->len) == 0)
    {
        return low;
    }

    return tag_id;
}

bool
lxb_html_tag_is_category(lxb_tag_id_t tag_id, lxb_ns_id_t ns,
                         lxb_html_tag_category_t cat)
{
    if (ns == LXB_NS_SVG) {
        tag_id = lxb_html_tag_svg_static(NULL, tag_id);
    }

    if (tag_id < LXB_TAG__LAST_ENTRY && ns < LXB_NS__LAST_ENTRY) {
        return lxb_html_tag_res_cats[tag_id][ns] & cat;
    }

    return (LXB_HTML_TAG_CATEGORY_ORDINARY) & cat;
}

const lxb_html_tag_fixname_t *
lxb_html_tag_fixname_svg(lxb_tag_id_t tag_id)
{
    if (tag_id >= LXB_TAG__LAST_ENTRY) {
        return NULL;
    }

    return &lxb_html_tag_res_fixname_svg[tag_id];
}
