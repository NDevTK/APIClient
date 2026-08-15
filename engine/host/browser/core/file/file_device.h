/* HTML §4.10.5.1.17's FILE CONTROL PICKER — the `accept` filter over the ONE virtual filesystem, and the
   selection it answers a prompt with. See file_device.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_FILE_FILE_DEVICE_H
#define ENGINE_HOST_BROWSER_CORE_FILE_FILE_DEVICE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "quickjs.h"

/* HOW MANY FILES THE DEVICE HOLDS — the LOCAL FILE SYSTEM root's file entries (core/file/file_system.h). Zero
   is the state of a device nothing has put a file on, which is a different fact from a control with nothing
   selected. Files are put on it through file_system_local_add, which is the ONE edge bytes enter this engine
   as a file the user could choose; there is no store here.
   IT TAKES A CONTEXT because the device is built out of JS values — that is what makes a page's write to it
   ride the per-flow COW delta, so a flow that wrote a file sees it and its sibling does not. */
uint32_t file_device_count(JSContext *ctx);

/* HTML §4.10.5.1.17's `accept` FILTER — "user agents should prevent the user from selecting files that are not
   accepted by one (or more) of these tokens". `accept` is the attribute's value (NULL when it is absent), and
   the tokens are exactly the four forms the section lists: the three wildcard strings `audio`, `video` and
   `image` each followed by U+002F and U+002A, a valid MIME type with no parameters, and a string beginning
   with U+002E. (Spelled out rather than written literally: a solidus-asterisk inside a block comment opens a
   nested comment as far as -Wcomment is concerned, and this header is included widely enough that the warning
   fired on most of the build.) */
bool file_device_accepts(const char *accept, size_t accept_len, const char *name, const char *type);

/* THE DEVICE'S ANSWER TO A PICKER — a FileList (File API §5) of the device files this control accepts, at most
   ONE unless `multiple` (§4.10.5.1.17: "Unless the multiple attribute is set, there must be no more than one
   file in the list of selected files"). The Files are minted in `ctx`'s realm, because a File carries the
   realm's File.prototype and a device shared between realms must not hand one realm's object to another.
   EMPTY when the device holds nothing this control accepts — the same answer a real prompt gives a user who
   picks nothing. */
JSValue file_device_select(JSContext *ctx, const char *accept, size_t accept_len, bool multiple);

#endif
