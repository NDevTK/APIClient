/* Dynamic import — Blink core/loader. Records every specifier forced execution reaches. */
#ifndef ENGINE_HOST_BROWSER_CORE_LOADER_MODULE_LOADER_H
#define ENGINE_HOST_BROWSER_CORE_LOADER_MODULE_LOADER_H
#include "quickjs.h"

void        module_loader_install(JSRuntime *rt);
const char *module_loader_chunks(void);   /* newline-joined, or "" */

#endif
