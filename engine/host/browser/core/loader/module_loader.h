/* Dynamic import — Blink core/loader. Parks every load forced execution reaches on the flow that asked. */
#ifndef ENGINE_HOST_BROWSER_CORE_LOADER_MODULE_LOADER_H
#define ENGINE_HOST_BROWSER_CORE_LOADER_MODULE_LOADER_H
#include "quickjs.h"

void        module_loader_install(JSRuntime *rt);
/* The AGENT's half, undone — core/platform.h's release column: ECMAScript §16.2.1.10 HostLoadImportedModule
   comes off the runtime. There is nothing else to give back: the register of specifiers this component used to
   keep is deleted, because what it existed to state — that these addresses are CODE — is Fetch §2.2.5
   "Requests"' DESTINATION, which belongs on the request rather than in a list one producer fills. */
void        module_loader_free(JSRuntime *rt);

#endif
