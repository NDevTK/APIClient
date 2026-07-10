/* Permissions API — Blink modules/permissions. navigator.permissions.query(desc) over a VIRTUAL permission
 * system (no real prompt headless): a real PermissionStatus whose state defaults to 'prompt' but forks. See
 * permissions.c. */
#ifndef ENGINE_HOST_BROWSER_PERMISSIONS_H
#define ENGINE_HOST_BROWSER_PERMISSIONS_H
#include "quickjs.h"
JSValue js_permissions_make(JSContext *ctx);   /* the navigator.permissions object */
#endif
