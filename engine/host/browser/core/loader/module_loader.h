/* Dynamic import — Blink core/loader. Records every specifier forced execution reaches. */
#ifndef ENGINE_HOST_BROWSER_CORE_LOADER_MODULE_LOADER_H
#define ENGINE_HOST_BROWSER_CORE_LOADER_MODULE_LOADER_H
#include "quickjs.h"

void        module_loader_install(JSRuntime *rt);
const char *module_loader_chunks(void);   /* newline-joined, or "" — BORROWED, valid until the release */
/* The AGENT's half, undone — core/platform.h's release column: ECMAScript §16.2.1.10 HostLoadImportedModule
   comes off the runtime and the register's strdup'd specifiers are freed. The pointer `module_loader_chunks`
   answers with dies here. */
void        module_loader_free(JSRuntime *rt);

#endif
