#ifndef ENGINE_HOST_HTML_SCRIPT_ELEMENT_H
#define ENGINE_HOST_HTML_SCRIPT_ELEMENT_H
#include "quickjs.h"
#include <lexbor/dom/dom.h>
/* HTMLScriptElement / ScriptLoader (Blink core/html): when a <script src> is inserted into the live DOM, Blink
 * fetches + runs it. Here that is LAZY-CHUNK discovery — the src (possibly a JS-computed URL held in the
 * attribute taint shadow) is queued for fetch so the chunk's endpoints + code are learned. A pure browser
 * component: it FEEDS the scheduler (chunk queue) and holds no control flow of its own. */
void script_maybe_load(JSContext *ctx, lxb_dom_element_t *el);

/* Script-element load-event eligibility (a <script>'s 'load' handler is a load-gated continuation — fires on
   chunk-provide, not boot seed). js_add_listener records the gate; the scheduler skips a gated handler; chunk
   provide releases it. See the ScriptLoad registry in html_script_element.c. */
int  script_load_bind_if(JSContext *ctx, JSValueConst target, const char *type, JSValueConst fn);   /* 1 iff a <script> 'load' listener */
void script_load_release(const char *url);   /* chunk provided -> its load handlers become eligible */
int  script_load_gated(void *fnptr);         /* 1 iff fn is a load handler whose chunk has not provided (scheduler skip) */
void script_load_free(void);                 /* teardown */
#endif
