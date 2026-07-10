/* Blob / File — Blink core/fileapi (the Blob spec). A Blob's content is whatever the page put in it
 * (`new Blob([data], {type})`), so `.text()`/`.arrayBuffer()` must carry that data's taint — the
 * `URL.createObjectURL(new Blob([html],{type:'text/html'}))` navigation-XSS and `new Blob([location.hash])`
 * -> FileReader chains depend on it. File extends Blob with name/lastModified. See blob.c. */
#ifndef ENGINE_HOST_BROWSER_BLOB_H
#define ENGINE_HOST_BROWSER_BLOB_H
#include "quickjs.h"
void blob_init(JSContext *ctx);                                                       /* register the Blob class (qjs_init) */
JSValue js_blob_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv);   /* new Blob(parts, opts) */
JSValue js_file_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv);   /* new File(bits, name, opts) */
JSValue js_blob_make(JSContext *ctx, JSValue content, JSValueConst type);             /* shared: a Blob over given content */
#endif
