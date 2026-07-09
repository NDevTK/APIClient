/* File System Access — Blink modules/file_system_access. showOpenFilePicker/showSaveFilePicker/
 * showDirectoryPicker + navigator.storage.getDirectory (OPFS). A picked file's content is ATTACKER-controlled
 * (the victim opens the attacker's file), so it is a concolic {fileContent} source (a replay flow injects the
 * @S candidate here). See fsa.c. */
#ifndef ENGINE_HOST_BROWSER_FSA_H
#define ENGINE_HOST_BROWSER_FSA_H
#include "quickjs.h"
JSValue js_fsa_open_picker(JSContext *ctx, JSValueConst t, int c, JSValueConst *v);   /* showOpenFilePicker() */
JSValue js_fsa_save_picker(JSContext *ctx, JSValueConst t, int c, JSValueConst *v);   /* showSaveFilePicker() */
JSValue js_fsa_dir_picker(JSContext *ctx, JSValueConst t, int c, JSValueConst *v);    /* showDirectoryPicker() / getDirectory() */
#endif
