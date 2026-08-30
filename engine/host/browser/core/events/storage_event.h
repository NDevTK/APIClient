/* THE StorageEvent INTERFACE — HTML §12.2.4 "The StorageEvent interface". See storage_event.c.
 *
 *     [Exposed=Window]
 *     interface StorageEvent : Event {
 *       constructor(DOMString type, optional StorageEventInit eventInitDict = {});
 *       readonly attribute DOMString? key;
 *       readonly attribute DOMString? oldValue;
 *       readonly attribute DOMString? newValue;
 *       readonly attribute USVString url;
 *       readonly attribute Storage? storageArea;
 *       undefined initStorageEvent(DOMString type, optional boolean bubbles = false,
 *                                  optional boolean cancelable = false, optional DOMString? key = null,
 *                                  optional DOMString? oldValue = null, optional DOMString? newValue = null,
 *                                  optional USVString url = "", optional Storage? storageArea = null);
 *     };
 *     dictionary StorageEventInit : EventInit {
 *       DOMString? key = null;
 *       DOMString? oldValue = null;
 *       DOMString? newValue = null;
 *       USVString url = "";
 *       Storage? storageArea = null;
 *     };
 *
 * WHAT WAS BLOCKED ON IT. HTML §12.2.1 "The Storage interface"'s BROADCAST is the last step of setItem,
 * removeItem and clear, and its step 4 is "queue a global task on the DOM manipulation task source given
 * remoteStorage's relevant global object to fire an event named storage at remoteStorage's relevant global
 * object, using StorageEvent" — so a second same-origin document that had read `localStorage` gave
 * core/storage/storage.c a set of targets and NOTHING TO FIRE AT THEM. That is the whole of cross-document Web
 * Storage: one window writes and the others learn, which is how a shipped app keeps two tabs' auth state in
 * step and how every one-window-tells-another test in a corpus is written.
 *
 * THE FIVE ATTRIBUTES ARE THE EVENT'S ENTIRE CONTENT and a listener reads all of them: §12.2.4 says each "must
 * return the value it was initialized to", and a handler's first line is `if (e.key.indexOf(prefix) === 0)`
 * followed by `e.newValue`. Firing a plain Event here would not lose five properties quietly — it would make
 * every one of them `undefined` and send that `indexOf` to a TypeError.
 *
 * IT IS A REAL SUBCLASS: `StorageEvent.prototype.__proto__ === Event.prototype`, so `e instanceof Event` holds
 * and `initEvent` works on one. The base half is event_new_derived's.
 *
 * IT HAS A createEvent ROW — DOM §4.5's table names StorageEvent — and core/events/create_event.c asserts the
 * pairing from both sides, so the row and this interface arrive together or the first
 * `createEvent('StorageEvent')` crashes naming the one that did not. That is also why `initStorageEvent` is
 * built rather than skipped: §4.5 leaves the initialized flag UNSET, and only an initializer sets it, so
 * without this member the factory row would answer an event that can never be dispatched.
 *
 * `storageArea` NAMES ANOTHER INTERFACE'S CLASS, which is the one thing this file cannot decide for itself:
 * §3.2.15's brand for `Storage? storageArea` is core/storage/storage.c's class id, read through
 * storage_class_id() at declaration because core/platform.c declares the `storage` row BEFORE the `event` row
 * this interface is declared from. The declaration asserts that rather than assuming it.
 *
 * THE SLOTS ARE OWN PROPERTIES UNDER A PRIVATE SYMBOL, for the reason event.c gives: a slot written as a
 * property write is captured by the COW delta, so the event's state time-travels with the flow that fired it,
 * and the symbol is a brand a page cannot forge.
 *
 * THE THREE VALUE SLOTS HOLD VALUES AND NEVER C STRINGS, and that is load-bearing rather than tidy. Web Storage
 * is a STORED-XSS carrier: core/storage/storage.c keeps unknown external input in the map as itself, so the
 * `newValue` a broadcast carries into ANOTHER document's handler may be attacker input with its provenance and
 * its example intact. A ToString anywhere on this path would de-taint it and the whole stored-source route
 * through §12.2.1 would go quiet. `key` and `oldValue` cross for the same reason, and `null` is a real value of
 * all three (`clear()` broadcasts with null, null and null).
 *
 * THE INTERFACE OBJECT IS A PER-REALM INTRINSIC, declared into realm.h's one list beside the prototype — §3.7
 * gives each realm its own interface OBJECT for the same reason it gives each its own prototype. */
#ifndef ENGINE_HOST_BROWSER_CORE_EVENTS_STORAGE_EVENT_H
#define ENGINE_HOST_BROWSER_CORE_EVENTS_STORAGE_EVENT_H

#include "quickjs.h"

/* Declared ONCE PER AGENT, from core/events/event.c's subclass list — its prototype chains to that realm's
   Event.prototype, and core/realm.h runs the per-realm installs in declaration order. */
void storage_event_init(JSContext *ctx);
void storage_event_install_protos(JSContext *ctx);
/* Undone ONCE PER AGENT. The RUNTIME, not a realm: what it gives back is the agent's — a private
   Symbol, a class id and this interface's member declarations — and every prototype it built is in
   some realm's class-proto slot and goes with that realm. Reached from core/events/event.c's
   event_free_subclasses, which is core/platform.c's `event` row. */
void storage_event_free(JSRuntime *rt);

/* DOM §4.5 createEvent's maker: a default instance — key, oldValue and newValue null, url the empty string,
   storageArea null. Steps 6-8 then overwrite type and isTrusted and unset the initialized flag. */
JSValue storage_event_new(JSContext *ctx);

/* HTML §12.2.1's broadcast step 4, as the event it names: `storage`, TRUSTED, non-bubbling and non-cancelable,
 * with the five attributes initialized to what that step passes.
 *
 * `ctx` IS THE TARGET'S REALM and not the broadcaster's — the event is handed to the RECEIVING document's
 * listeners, so it is built with that realm's StorageEvent.prototype, exactly as core/events/broadcast_channel.c
 * builds each destination's MessageEvent in the destination's own realm.
 *
 * `key`, `old_value` and `new_value` are VALUES — a string, null, or unknown external input crossing as itself
 * (see this file's header). `url` is the serialization of the broadcasting Document's URL, which §12.2.1 step 2
 * computes. `storage_area` is the remote Storage the event is about. */
JSValue storage_event_new_to_fire(JSContext *ctx, JSValueConst key, JSValueConst old_value,
                                  JSValueConst new_value, const char *url, JSValueConst storage_area);

#endif
