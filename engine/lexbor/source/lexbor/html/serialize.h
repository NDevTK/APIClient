/*
 * Copyright (C) 2018 Alexander Borisov
 *
 * Author: Alexander Borisov <borisov@lexbor.com>
 */

#ifndef LEXBOR_HTML_SERIALIZE_H
#define LEXBOR_HTML_SERIALIZE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lexbor/core/str.h"
#include "lexbor/dom/interfaces/element.h"

#include "lexbor/html/base.h"


typedef lxb_status_t
(*lxb_html_serialize_cb_f)(const lxb_char_t *data, size_t len, void *ctx);


LXB_API lxb_status_t
lxb_html_serialize_cb(lxb_dom_node_t *node,
                      lxb_html_serialize_cb_f cb, void *ctx);

LXB_API lxb_status_t
lxb_html_serialize_str(lxb_dom_node_t *node, lexbor_str_t *str);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LEXBOR_HTML_SERIALIZE_H */
