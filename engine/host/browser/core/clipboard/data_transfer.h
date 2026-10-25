/* HTML §6.11.3 The DataTransfer interface, with §6.11.3.1 The DataTransferItemList interface and §6.11.3.2 The
   DataTransferItem interface: the page's view of §6.11.2's drag data store. See data_transfer.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_CLIPBOARD_DATA_TRANSFER_H
#define ENGINE_HOST_BROWSER_CORE_CLIPBOARD_DATA_TRANSFER_H
#include <stdbool.h>

#include "quickjs.h"

/* §6.11.2's drag data store mode. */
typedef enum { DND_MODE_READ_WRITE = 0, DND_MODE_READ_ONLY, DND_MODE_PROTECTED } DragDataStoreMode;

/* The three classes, their slot key and member declarations, once per agent; core/platform.c's row. */
void data_transfer_init(JSContext *ctx);
/* The three prototypes and interface objects, once per realm (core/realm.h). */
void data_transfer_install_protos(JSContext *ctx);
void data_transfer_free(JSRuntime *rt);

/* Web IDL §3.2.15's "V implements DataTransfer". */
bool data_transfer_is(JSContext *ctx, JSValueConst v);

/* A DataTransfer associated with a new drag data store whose item list is one File item per element of
   `files` (a JS Array of File objects, borrowed), in `mode`, with §6.11.4's dropEffect and the store's allowed
   effects state as effectAllowed: the user agent's half of fire a DND event, steps 4-6. Owned. */
JSValue data_transfer_new_for_drag(JSContext *ctx, JSValueConst files, DragDataStoreMode mode,
                                   const char *drop_effect, const char *effect_allowed);

#endif
