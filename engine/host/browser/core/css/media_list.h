/* CSSOM §4.4 — MediaList, the object behind every `media` attribute in the platform.
 *
 * THE COLLECTION IS A JS ARRAY OF SERIALIZED QUERIES, and that is the same decision css_rule.h records for a
 * sheet's rules, made for the same two reasons. (1) A parsed `MediaQuerySet` is malloc'd C: it could not park to
 * the IDB cold tier and could not fork per flow, and §4.4's collection is MUTABLE — `mediaText =`,
 * `appendMedium` and `deleteMedium` are exactly the mutations two flows must be able to disagree about. An
 * Array's mutations are property writes the COW delta already captures, and the snapshot machinery already
 * carries it. (2) Every member §4.4 declares is stated over the collection's MEMBERS — `length` counts them,
 * `item(index)` serializes one, both methods COMPARE one against each of them — so holding the per-query
 * SERIALIZATIONS is holding the thing the interface is about, and `mediaText` is CSSOM §4.2's list
 * serialization over them: "serialize each media query in the list ... and then serialize the list".
 *
 * WHAT IS NOT HERE. §4.4 has no constructor, so nothing but a component that OWNS a media attribute creates
 * one; each such attribute is `[SameObject]`, so its owner remembers the object it minted rather than minting a
 * fresh one per read. */
#ifndef ENGINE_HOST_BROWSER_CORE_CSS_MEDIA_LIST_H
#define ENGINE_HOST_BROWSER_CORE_CSS_MEDIA_LIST_H

#include <stdbool.h>
#include <stddef.h>

#include "quickjs.h"
#include "core/css/media_query.h"

void media_list_init(JSContext *ctx);
/* §4.4's prototype for ONE realm — declared into core/realm.h's list. */
void media_list_install_proto(JSContext *ctx);
/* `MediaList` as a global. */
void media_list_install(JSContext *ctx, JSValueConst global);
void media_list_free(JSRuntime *rt);

/* §4.4's "CREATE A MEDIALIST OBJECT WITH A STRING text" — a new object whose `mediaText` is then set to `text`,
   which is the parse and the canonicalisation, not a copy of the bytes. `text` may be NULL, which is the empty
   string and therefore the empty collection. OWNED: the caller frees. */
JSValue media_list_new(JSContext *ctx, const char *text);

/* Is `v` a MediaList? The brand, for a caller holding something it took off an attribute. */
bool media_list_is(JSContext *ctx, JSValueConst v);

/* §4.2's SERIALIZE A MEDIA QUERY LIST over this object's collection — the same string `mediaText` answers, for
   the C callers that need it as text: §7.3's `conditionText` ("must return the value of media.mediaText") and
   §6.4's serialize-a-CSS-rule. OWNED: the caller frees. Never NULL — an empty collection is "". */
char *media_list_text(JSContext *ctx, JSValueConst list);

/* The collection as the LANGUAGE sees it, parsed from the same text — for the EVALUATION a conditional group
   rule needs (does this `@media` apply?). OWNED: media_query_free. */
MediaQuerySet *media_list_query_set(JSContext *ctx, JSValueConst list);

/* THE SETTER ID FOR THIS COMPONENT'S `[PutForwards=mediaText]` PAIR — Web IDL §3.3.10 [PutForwards], whose
   five steps are §3.7.6 Attributes' and belong to the binding layer (idl_setter_id_put_forwards). What this
   component owns is the PAIR — (`media`, `mediaText`) — and EVERY carrier in the platform names the attribute
   `media`: CSSMediaRule's, CSSStyleSheet's and CSSImportRule's. That is what lets one declaration serve them
   all, exactly as the `style`/`cssText` pair does, because the forwarding is a real [[Get]] of `media` by NAME
   off the receiver followed by a [[Set]] of `mediaText` on the result. */
int media_list_put_forwards_setter(void);

#endif
