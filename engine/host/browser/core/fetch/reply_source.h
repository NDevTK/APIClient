/* WHAT A REPLY'S BYTES ARE CALLED — ONE spelling, for every interface that hands a page a byte sequence a
   SERVER filled. See reply_source.c for why it is one and why it is percent-encoded. */
#ifndef ENGINE_HOST_BROWSER_CORE_FETCH_REPLY_SOURCE_H
#define ENGINE_HOST_BROWSER_CORE_FETCH_REPLY_SOURCE_H
#include <stddef.h>

/* NAME A REPLY AFTER THE ADDRESS ITS BYTES CAME FROM. `url` is the reply's own URL, serialized — Fetch's
   `response.url` and XHR §3.6.1 The responseURL getter are the same fact through two doors, and both are
   already in hand at the two call sites, so nothing is plumbed to reach this.
   Returns a MALLOC'D name the caller frees, or NULL when there is no address — which is the POSITIVE
   statement that these bytes are not a server's (a `new Response("x")` the page built has no URL), never a
   hole a default fills. */
char *reply_source_name(const char *url, size_t len);

#endif
