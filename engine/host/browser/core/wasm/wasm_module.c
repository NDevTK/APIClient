/* A decoded WebAssembly module's pools. See wasm_module.h. */
#include "core/wasm/wasm_module.h"

#include <stdlib.h>
#include <string.h>

#include "check.h"

void wasm_module_free(WasmModule *m)
{
    DCHECK(m != NULL, "a WebAssembly module was freed with no module");
    free(m->types);
    free(m->val_types);
    free(m->fields);
    free(m->type_indices);
    memset(m, 0, sizeof *m);
}
