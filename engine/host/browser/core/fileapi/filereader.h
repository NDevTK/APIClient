/* FileReader — Blink fileapi/FileReader. The classic file-upload read: reader.readAsText(file) sets
 * reader.result to the file's (attacker-controlled) content and fires onload; a handler that sinks
 * reader.result / e.target.result is the file-upload XSS surface. See filereader.c. */
#ifndef ENGINE_HOST_BROWSER_FILEREADER_H
#define ENGINE_HOST_BROWSER_FILEREADER_H
#include "quickjs.h"
JSValue js_filereader_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv);   /* new FileReader() */
#endif
