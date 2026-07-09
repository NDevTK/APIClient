/* The browser `location` object + external-input SOURCE getters + the page-principal split.  See location.c.
 *
 * location.hash / location.search / a postMessage `e.data` are the ATTACKER-INPUT sources: normal flow reads
 * a source-tagged opaque (control-flow forks, @H/@S record the shape); an @S replay flow reads the concrete
 * candidate (with the source's real leading char so slice(1)/substring behave). location.href=/.assign/
 * .replace and `window.location = url` are @S navigation sinks. set_origin splits the real principal into the
 * concrete location.protocol/host/hostname/port a bundle builds URLs from. */
#ifndef ENGINE_HOST_LOCATION_H
#define ENGINE_HOST_LOCATION_H
#include "quickjs.h"

void set_origin(const char *origin);   /* split the real principal into protocol/host/hostname/port + g_origin */
const char *location_host(void);       /* location.host (hostname[:port]) — the concrete page identity (document.domain) */

/* Define an external-input source getter on `obj`: magic 0=location.hash, 1=location.search, 2=postMessage
   e.data. Normal flow -> source-tagged opaque; @S replay -> the concrete candidate (with the source prefix). */
void def_source(JSContext *ctx, JSValueConst obj, const char *name, int magic);

JSValue make_location(JSContext *ctx);   /* build the location object + store the window.location singleton */
JSValue js_window_location_get(JSContext *ctx, JSValueConst t);
JSValue js_window_location_set(JSContext *ctx, JSValueConst t, JSValueConst val);
void location_free(JSContext *ctx);      /* teardown: free the location singleton */

#endif
