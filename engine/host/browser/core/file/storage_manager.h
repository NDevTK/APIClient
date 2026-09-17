/* StorageManager — Storage §8 "API"'s interface, and File System §3's partial that puts getDirectory() on it.
   See storage_manager.c. (THE SECTION IS §8 AND NOT §2: §2 is "Terminology" and declares nothing. The .c
   banner records the same correction; this was the third copy of the wrong number and the last one left.) */
#ifndef ENGINE_HOST_BROWSER_CORE_FILE_STORAGE_MANAGER_H
#define ENGINE_HOST_BROWSER_CORE_FILE_STORAGE_MANAGER_H

#include "quickjs.h"

/* Declared once per AGENT, and AFTER navigator_init: this component's own per-realm intrinsic builds the
   StorageManager the member ANSWERS with, and core/realm.h runs the intrinsics in declaration order. */
void storage_manager_init(JSContext *ctx);

/* STORAGE §8's `NavigatorStorage` MIXIN MEMBER, INSTALLED ON ONE INCLUDER'S INTERFACE PROTOTYPE OBJECT.
 *
 * WEB IDL §3.7.3 "Interface prototype object" gives an `interface mixin` no prototype of its own, so
 * `[SameObject] readonly attribute StorageManager storage` has no object anywhere until an INCLUDER supplies
 * one — and §8 writes two: `Navigator includes NavigatorStorage;` and `WorkerNavigator includes
 * NavigatorStorage;`. So the includer hands its prototype here; this component owns the member's steps and
 * never decides which includers a realm has.
 *
 * IT TAKES THE PROTOTYPE AND NOT THE INSTANCE, which is Web IDL §3.7.6 "Attributes" in one sentence:
 * "Regular attributes are exposed on the interface prototype object, unless the attribute is unforgeable or
 * if the interface was declared with the [Global] extended attribute". `storage` carries no
 * [LegacyUnforgeable] and `Navigator` is `[Exposed=Window]` and not `[Global]`, so neither arm of that
 * "unless" applies. This was installed on the NAVIGATOR ITSELF and that is a wrong answer rather than a
 * narrow one — an own property of `navigator`, missing from `Navigator.prototype`, and `delete`-able, which
 * is the same four-way divergence core/frame/navigator.h's own header names as what makes an interface an
 * interface.
 *
 * IT IS THE SAME SHAPE core/frame/navigator_beacon.h STATES FOR BEACON §2.1's OPERATION, and deliberately so:
 * a member another standard declares on Navigator is installed FROM navigator's own per-realm intrinsic, with
 * the prototype passed in, because that is the one place per realm that has a prototype to give. A second
 * mechanism for the same question — an accessor handing this component the prototype to reach back for —
 * would be two right answers to one question, which is the shape that drifts. */
void storage_manager_install_navigator_storage(JSContext *ctx, JSValueConst proto);

void storage_manager_free(void);

#endif
