/* HTML §8.5.8 — the `XMLSerializer` interface. See xml_serializer.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_XML_SERIALIZER_H
#define ENGINE_HOST_BROWSER_CORE_HTML_XML_SERIALIZER_H
#include "quickjs.h"

void xml_serializer_init(JSContext *ctx);
/* §8.5.8's prototype for ONE realm — declared into core/realm.h's list, never installed by a host by hand. */
void xml_serializer_install_proto(JSContext *ctx);
void xml_serializer_install(JSContext *ctx, JSValueConst global);
void xml_serializer_free(void);

#endif
