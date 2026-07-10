/* Document query methods — Blink core/dom/Document. Real DOM lookups over the live Lexbor tree via the CSS
 * selector engine (dom_select.c); a found node is wrapped as a JS Element (el_wrap, dom_element.c). No
 * scheduler coupling — a pure read the orphan driver / boot flow reaches. See document.c. */
#ifndef ENGINE_HOST_BROWSER_DOCUMENT_H
#define ENGINE_HOST_BROWSER_DOCUMENT_H
#include "quickjs.h"
JSValue js_doc_getElementById(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
JSValue js_doc_querySelector(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
JSValue js_doc_querySelectorAll(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
JSValue js_doc_getByClass(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);   /* getElementsByClassName */
JSValue js_doc_createElement(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);   /* real Lexbor element + custom-element upgrade */
JSValue js_doc_currentscript(JSContext *ctx, JSValueConst t);            /* getter: the executing script this inline block */
void doc_set_current_script(JSContext *ctx, JSValue v);                  /* scheduler boot loop feeds currentScript (consumes v) */
void document_init(JSContext *ctx, JSValue global);   /* register the Document class + prototype + window.Document (call before js_document_make) */
JSValue js_document_make(JSContext *ctx);   /* the window.document instance (shares Document.prototype) */
#endif
