/* THE MOCK FILE DEVICE — the storage a file picker chooses from. See file_device.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_FILE_FILE_DEVICE_H
#define ENGINE_HOST_BROWSER_CORE_FILE_FILE_DEVICE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "quickjs.h"

/* PUT A FILE ON THE DEVICE — the mock-backed IO edge, the one way bytes enter this engine as a file the user
   could choose. `name` has its PATH COMPONENTS stripped here (HTML §4.10.5.1.17: "Filenames must not contain
   path components ... those parts of filenames that are separated by U+005C"), so nothing downstream has to
   remember that rule and `C:\fakepath\` can never be mistaken for one.
   `type` is the MIME type the storage records for it, "" for none. Everything is COPIED.
   DEVICE STATE IS BASELINE STATE, not a flow's: it is what the user's disk holds, the same for every flow, and
   no flow writes it — a selection COPIES bytes into per-flow File objects, which is what time-travels. */
void file_device_add(const char *name, const char *type, const char *bytes, size_t len, int64_t last_modified);
/* How many files the device holds. Zero is the state of a device nothing has put a file on, which is a
   different fact from a control with nothing selected. */
uint32_t file_device_count(void);

/* HTML §4.10.5.1.17's `accept` FILTER — "user agents should prevent the user from selecting files that are not
   accepted by one (or more) of these tokens". `accept` is the attribute's value (NULL when it is absent), and
   the tokens are exactly the four forms the section lists: `audio/*`, `video/*`, `image/*`, a valid MIME type
   with no parameters, and a string beginning with U+002E. */
bool file_device_accepts(const char *accept, size_t accept_len, const char *name, const char *type);

/* THE DEVICE'S ANSWER TO A PICKER — a FileList (File API §5) of the device files this control accepts, at most
   ONE unless `multiple` (§4.10.5.1.17: "Unless the multiple attribute is set, there must be no more than one
   file in the list of selected files"). The Files are minted in `ctx`'s realm, because a File carries the
   realm's File.prototype and a device shared between realms must not hand one realm's object to another.
   EMPTY when the device holds nothing this control accepts — the same answer a real prompt gives a user who
   picks nothing. */
JSValue file_device_select(JSContext *ctx, const char *accept, size_t accept_len, bool multiple);

void file_device_free(void);

#endif
