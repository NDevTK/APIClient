/* THE DOMImplementation INTERFACE — DOM §4.5.1. */
#ifndef ENGINE_HOST_BROWSER_CORE_DOM_DOM_IMPLEMENTATION_H
#define ENGINE_HOST_BROWSER_CORE_DOM_DOM_IMPLEMENTATION_H
#include "quickjs.h"

/* THE AGENT'S HALF: the class and the per-realm prototype declaration. */
void dom_implementation_init(JSContext *ctx);
/* §4.5.1's interface prototype object for ONE realm — declared into core/realm.h's list. */
void dom_implementation_install_proto(JSContext *ctx);
/* The interface OBJECT on a realm's global, so `document.implementation instanceof DOMImplementation` holds. */
void dom_implementation_install(JSContext *ctx, JSValueConst global);
/* Reached from document_agent_free — §4.5.1 is declared by document_init, so it is released by its declarer. */
void dom_implementation_free(void);

/* THE ONE DOMImplementation OBJECT FOR ONE DOCUMENT — §4.5's `[SameObject]`. `doc_obj` is that document's
   wrapper, which is what the object's "associated document" is; document.c holds the result on the document's
   own record and hands it back on every read. OWNED by the caller. */
JSValue dom_implementation_new(JSContext *ctx, JSValueConst doc_obj);

/* THE ASSOCIATED DOCUMENT IS GONE. A page can hold `document.implementation` past the document's teardown, so
   the object's one piece of state is cleared rather than left naming freed memory — a member reached through
   it then CRASHES at the read, which is the difference between a bug that is reported and one that is a
   corrupted heap three algorithms later. */
void dom_implementation_detach(JSContext *ctx, JSValueConst impl);

#endif
