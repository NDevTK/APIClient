/* HashChangeEvent — HTML §7.2.7.3. See hash_change_event.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_EVENTS_HASH_CHANGE_EVENT_H
#define ENGINE_HOST_BROWSER_CORE_EVENTS_HASH_CHANGE_EVENT_H

#include "quickjs.h"

void hash_change_event_init(JSContext *ctx);             /* the slot key + the IDL declarations (agent init) */
/* §3.7: THIS REALM's prototype AND interface object. Declared into realm.h's one list by the init above. */
void hash_change_event_install_protos(JSContext *ctx);
void hash_change_event_free(JSContext *ctx);

/* DOM §2.5's CREATE AN EVENT using HashChangeEvent: every attribute at its un-initialized value, which for
   `USVString oldURL` and `USVString newURL` is the empty string. §4.5's createEvent is the caller — the table
   there names HashChangeEvent, so this row exists the moment the interface does. */
JSValue hash_change_event_new(JSContext *ctx);

/* HTML §7.4.6.2's "fire an event named hashchange at document's relevant global object, USING HashChangeEvent,
   with the oldURL attribute initialized to the serialization of oldURL and the newURL attribute initialized to
   the serialization of entry's URL" — the EVENT half of it. Both arguments are already SERIALIZED URLs, because
   that is what the standard's own step says and because the caller is the one holding the two URL records.
   The dispatch is QUEUED (§7.4.6.2 queues a global task on the DOM manipulation task source for it), which is
   why this hands back an event rather than firing: event_target_fire is the queued reach.
   `hashchange` neither bubbles nor is cancelable — §7.4.6.2 sets none of the flags. isTrusted is TRUE.
   Returns a new owned HashChangeEvent. */
JSValue hash_change_event_new_to_fire(JSContext *ctx, const char *old_url, const char *new_url);

#endif
