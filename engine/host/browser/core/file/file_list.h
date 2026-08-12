/* THE FileList INTERFACE — W3C File API §5. See file_list.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_FILE_FILE_LIST_H
#define ENGINE_HOST_BROWSER_CORE_FILE_FILE_LIST_H
#include <stdbool.h>
#include <stdint.h>

#include "quickjs.h"

/* Declared once per AGENT — the class, the slot key its list hangs off, the two members, and the per-realm
   intrinsic that builds §5's prototype and interface object. Run from blob_init, which is this directory's
   declaration point: §3, §4 and §5 are one component and a File API with a File and no FileList is the half
   that cannot answer `input.files`. */
void file_list_init(JSContext *ctx);
/* §5's PROTOTYPE AND INTERFACE OBJECT FOR ONE REALM — declared into core/realm.h's one list, run once per
   realm. Both, because §3.7 gives each realm its own interface object for the same reason it gives each its
   own prototype. */
void file_list_install_protos(JSContext *ctx);
/* PER REALM. OWNED: the caller frees. */
JSValue file_list_proto(JSContext *ctx);
void file_list_free(JSContext *ctx);

/* §5's "list of File objects", as the FileList that IS one. `files` is a JS Array of File objects and is
   CONSUMED — the list lives as a JS value so it forks per flow and parks with the flow holding it, which a
   malloc'd C list could do neither of. Every element must be a File (§5 says so and this asserts it): a plain
   Blob in the list is a `files[0].name` that throws. */
JSValue file_list_new(JSContext *ctx, JSValue files);
/* §5 WITH NO FILES — the state a file control is in before anything selects. It is a real FileList, which is
   the whole difference between "nothing is selected" and "there is no such thing here". */
JSValue file_list_new_empty(JSContext *ctx);

/* THE BRAND TEST — is this value a FileList. The list hangs off a Symbol this component minted and never
   published, so a page cannot forge one. */
bool file_list_is(JSContext *ctx, JSValueConst v);
/* §5.1's `length`, and §5.2's `item(index)` read from C. `item` is OWNED and is JS_UNDEFINED past the end,
   which is the INDEXED GETTER's answer; the `item()` OPERATION turns that into §5.2's null. */
uint32_t file_list_length(JSContext *ctx, JSValueConst v);
JSValue  file_list_item(JSContext *ctx, JSValueConst v, uint32_t i);

#endif
