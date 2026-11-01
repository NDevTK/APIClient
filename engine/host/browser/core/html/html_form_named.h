/* HTML §4.10.3 The form element — HTMLFormElement's named properties, under its [LegacyOverrideBuiltIns] and
 * [LegacyUnenumerableNamedProperties], and the Web IDL §3.9 legacy platform object internal methods they give a
 * form element's wrapper. Blink's HTMLFormElement::AnonymousNamedGetter and its past names map. See
 * html_form_named.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_HTML_FORM_NAMED_H
#define ENGINE_HOST_BROWSER_CORE_HTML_HTML_FORM_NAMED_H

#include "quickjs.h"

/* Mints the class every HTML `form` element's wrapper is built with (node_wrap), its named-access step and its
   past names map's slot key; the agent's, from html_form_declare. */
void html_form_named_declare(JSContext *ctx);
void html_form_named_free(JSRuntime *rt);
/* The form wrapper class; 0 before html_form_named_declare. */
JSClassID html_form_named_class(void);

#endif
