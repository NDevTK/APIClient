/* File System Access — see fsa.h. A MOCK filesystem (headless has no disk, but the spec defines the behavior):
 * the pickers resolve to file/directory handles; a handle's getFile() yields a File whose content is
 * ATTACKER-controlled (the victim opens the attacker's file) -> a concolic {fileContent} source that a replay
 * flow fills with the @S candidate, exactly like location.hash. So a `showOpenFilePicker().then(([h])=>h.getFile())
 * .then(f=>f.text()).then(t=>el.innerHTML=t)` file-upload XSS is a solvable @S chain. createWritable() records
 * writes (a mock file the solver could round-trip). */
#include "modules/fsa.h"
#include "solver/concolic.h"   /* g_concolic, js_concolic, js_noop */

#include "platform/promise.h"
extern char *g_candidate;                                  /* @S replay: file content is attacker-controlled -> deliver the candidate */

/* A file's text/bytes: attacker-controlled -> the @S candidate on a replay flow, else a concolic source. */
static JSValue fsa_content(JSContext *ctx) {
    if (g_candidate) return JS_NewString(ctx, g_candidate);
    return js_concolic(ctx, "{fileContent}", JS_UNDEFINED);
}
static JSValue fsa_text(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) { (void)t; (void)c; (void)v; return js_resolved(ctx, fsa_content(ctx)); }

/* File (a Blob): name/size/type/lastModified are attacker-chosen; text()/arrayBuffer() resolve the content. */
static JSValue fsa_make_file(JSContext *ctx) {
    JSValue f = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, f, "name", js_concolic(ctx, "{fileName}", JS_UNDEFINED));
    JS_SetPropertyStr(ctx, f, "size", js_concolic(ctx, "{fileSize}", JS_UNDEFINED));
    JS_SetPropertyStr(ctx, f, "type", js_concolic(ctx, "{fileType}", JS_UNDEFINED));
    JS_SetPropertyStr(ctx, f, "lastModified", js_concolic(ctx, "{fileMtime}", JS_UNDEFINED));
    JS_SetPropertyStr(ctx, f, "text", JS_NewCFunction(ctx, fsa_text, "text", 0));
    JS_SetPropertyStr(ctx, f, "arrayBuffer", JS_NewCFunction(ctx, fsa_text, "arrayBuffer", 0));
    JS_SetPropertyStr(ctx, f, "slice", JS_NewCFunction(ctx, fsa_text, "slice", 3));
    return f;
}
static JSValue fsa_getfile(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) { (void)t; (void)c; (void)v; return js_resolved(ctx, fsa_make_file(ctx)); }
static JSValue fsa_writable(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) {   /* createWritable() -> a writable stream */
    (void)t; (void)c; (void)v;
    JSValue w = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, w, "write", JS_NewCFunction(ctx, js_noop, "write", 1));      /* records a write to the mock file */
    JS_SetPropertyStr(ctx, w, "seek", JS_NewCFunction(ctx, js_noop, "seek", 1));
    JS_SetPropertyStr(ctx, w, "truncate", JS_NewCFunction(ctx, js_noop, "truncate", 1));
    JS_SetPropertyStr(ctx, w, "close", JS_NewCFunction(ctx, js_noop, "close", 0));
    return js_resolved(ctx, w);
}

/* A FileSystemFileHandle: getFile()/createWritable(); a directory handle: getFileHandle()/getDirectoryHandle(). */
static JSValue fsa_make_file_handle(JSContext *ctx) {
    JSValue h = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, h, "kind", JS_NewString(ctx, "file"));
    JS_SetPropertyStr(ctx, h, "name", js_concolic(ctx, "{fileName}", JS_UNDEFINED));
    JS_SetPropertyStr(ctx, h, "getFile", JS_NewCFunction(ctx, fsa_getfile, "getFile", 0));
    JS_SetPropertyStr(ctx, h, "createWritable", JS_NewCFunction(ctx, fsa_writable, "createWritable", 0));
    return h;
}
static JSValue fsa_get_file_handle(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) { (void)t; (void)c; (void)v; return js_resolved(ctx, fsa_make_file_handle(ctx)); }
static JSValue fsa_make_dir_handle(JSContext *ctx) {
    JSValue d = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, d, "kind", JS_NewString(ctx, "directory"));
    JS_SetPropertyStr(ctx, d, "name", js_concolic(ctx, "{dirName}", JS_UNDEFINED));
    JS_SetPropertyStr(ctx, d, "getFileHandle", JS_NewCFunction(ctx, fsa_get_file_handle, "getFileHandle", 1));
    JS_SetPropertyStr(ctx, d, "getDirectoryHandle", JS_NewCFunction(ctx, js_fsa_dir_picker, "getDirectoryHandle", 1));
    JS_SetPropertyStr(ctx, d, "removeEntry", JS_NewCFunction(ctx, js_noop, "removeEntry", 1));
    return d;
}

JSValue js_fsa_open_picker(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) {   /* -> Promise<[FileSystemFileHandle]> */
    (void)t; (void)c; (void)v;
    JSValue arr = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, arr, 0, fsa_make_file_handle(ctx));
    return js_resolved(ctx, arr);
}
JSValue js_fsa_save_picker(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) { (void)t; (void)c; (void)v; return js_resolved(ctx, fsa_make_file_handle(ctx)); }
JSValue js_fsa_dir_picker(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) { (void)t; (void)c; (void)v; return js_resolved(ctx, fsa_make_dir_handle(ctx)); }
