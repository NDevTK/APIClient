/* The navigable notices' field tables: what `navigable.create`, `navigable.swap`, `navigable.navigate` and
 * `navigable.request` carry, in order, as one declaration every composer and reader derives its indices from.
 *
 * A notice is the operation name and then one TAB before each field (solver/engine.h's engine_notice_build).
 * The enumerators below are a field's position among the fields, which is the composer's array index; a reader
 * that splits the whole record on TAB finds field `i` at `1 + i`, after the operation name.
 * Each entry states its encoding. A TOKEN is written raw and its grammar holds no TAB. A BASE64 field is a
 * byte string written as RFC 4648 §4 base64 by the engine's codec (JS_Base64Encode), whose alphabet holds no
 * TAB, and a reader decodes it. So no field holds a TAB and every record has exactly its table's count.
 * The trusted zone's readers take their indices from extension/lib/navigable-notice.js, which
 * engine/noticegen.mjs writes from this file; the build refuses a copy that this file does not re-render. */
#ifndef ENGINE_HOST_BROWSER_CORE_FRAME_NAVIGABLE_NOTICE_H
#define ENGINE_HOST_BROWSER_CORE_FRAME_NAVIGABLE_NOTICE_H

/* `navigable.create`: a navigable whose document another agent hosts (core/frame/navigable.c). Items of HTML
   §7.1.7's clone of the creator's container cross because the hosting instance cannot derive them. The two CSP
   texts are BASE64 because CSP's grammar admits a TAB (optional-ascii-whitespace includes %x09) and a
   violation report's original-policy is the policy as delivered, so the text is carried rather than
   re-serialized. Each field's grammar is stated at the composer. */
#define NAVIGABLE_CREATE_FIELDS(X)                                                                         \
    X(CHILD, child, TOKEN)                     /* the new document's world name */                               \
    X(CREATOR, creator, TOKEN)                 /* the creating document's world name */                          \
    X(ADDRESS, address, TOKEN)                 /* the address the navigable is navigated to */                   \
    X(ORIGIN, origin, TOKEN)                   /* that address's origin, serialized */                           \
    X(TOP_LEVEL_URL, topLevelUrl, TOKEN)       /* HTML §8.1.3.1's top-level creation URL */                      \
    X(CSP_SELF_ORIGIN, cspSelfOrigin, TOKEN)   /* CSP §2.2's self-origin of the creator's list */                \
    X(COEP, coep, TOKEN)                       /* HTML §7.1.4's embedder policy value */                         \
    X(COEP_ENDPOINT, coepEndpoint, TOKEN)      /* its reporting endpoint */                                      \
    X(COEP_REPORT_ONLY, coepReportOnly, TOKEN) /* its report-only value */                                       \
    X(COEP_REPORT_ONLY_ENDPOINT, coepReportOnlyEndpoint, TOKEN)                                                  \
    X(REFERRER_POLICY, referrerPolicy, TOKEN)  /* HTML §7.1.7's referrer policy item, Referrer Policy §3 */    \
    X(PARENT, parent, TOKEN)                   /* HTML §7.3.1.3's parent navigable (remote_object.h), or `u` */  \
    X(CONTAINER_POLICY, containerPolicy, TOKEN) /* Permissions Policy §9.5's result for the container */         \
    X(ANCESTOR_ORIGINS, ancestorOrigins, TOKEN) /* HTML §3.1.3's internal ancestor origin objects list */        \
    X(SANDBOX_FLAGS, sandboxFlags, TOKEN)      /* HTML §7.1.5's creation sandboxing flag set */                  \
    X(PROVENANCE, provenance, TOKEN)           /* what the navigation is evidence of (PENDING_PROVENANCE_*) */   \
    X(GROUP, group, TOKEN)                     /* `creator` or `new` browsing context group */                   \
    X(DRIVEN, driven, TOKEN)                   /* the drive mark (PENDING_DRIVEN_*) */                           \
    X(PINNED, pinned, TOKEN)                   /* the witness mark (PENDING_PINNED_*) */                         \
    X(OPENER, opener, TOKEN)                   /* HTML §7.2.2.4's opener (remote_object.h), or `N` */            \
    X(POLICY, policy, BASE64)                  /* the creator's CSP list's enforce policies (CSP §2.2) */        \
    X(POLICY_REPORT_ONLY, policyReportOnly, BASE64) /* …and its report policies */

/* `navigable.swap` (HTML §7.1.3.2's browsing context group swap, core/frame/browsing_context_group.c) and
   `navigable.navigate` (a top-level traversable navigated into a peer, core/frame/navigable.c): one record,
   since both provision a top-level document with no creator-side container. */
#define NAVIGABLE_PROVISION_FIELDS(X)                                                                      \
    X(DOCUMENT, document, TOKEN)               /* the new document's world name */                               \
    X(ADDRESS, address, TOKEN)                 /* the address loaded */                                          \
    X(ORIGIN, origin, TOKEN)                   /* that address's origin, serialized */                           \
    X(PROVENANCE, provenance, TOKEN)           /* what the load is evidence of (PENDING_PROVENANCE_*) */         \
    X(DRIVEN, driven, TOKEN)                   /* the drive mark (PENDING_DRIVEN_*) */                           \
    X(PINNED, pinned, TOKEN)                   /* the witness mark (PENDING_PINNED_*) */                         \
    X(ISSUER, issuer, TOKEN)                   /* HTML §7.4.2.2's sourceDocument, by its world name */         \
    X(OPENER, opener, TOKEN)                   /* the engine's claim of §7.2.2.4's opener (remote_object.h), or `N` */

/* `navigable.open`: an auxiliary navigable this agent created and holds (HTML §7.3.1.7 step 8), so the zone has
   its own record of who created it before any navigation of it reaches a peer. The opener is the engine's
   claim, which the zone accepts only as `N` or as the creator. */
#define NAVIGABLE_OPEN_FIELDS(X)                                                                           \
    X(CHILD, child, TOKEN)                     /* the new navigable's document's world name */                   \
    X(CREATOR, creator, TOKEN)                 /* the creating document's world name */                          \
    X(OPENER, opener, TOKEN)                   /* the engine's claim of §7.2.2.4's opener (remote_object.h), or `N` */

enum { NAVIGABLE_NOTICE_TOKEN, NAVIGABLE_NOTICE_BASE64 };

/* `navigable.request`: the HTML §7.4.5 request facts of a child navigable `creator` created at `address`
   (core/frame/navigable.c's nav_announce_child_request), for the zone's own load of the same frame. */
#define NAVIGABLE_REQUEST_FIELDS(X)                                                                        \
    X(CREATOR, creator, TOKEN)          /* the creating document's world name */                          \
    X(CHILD, child, TOKEN)              /* the child navigable's document's world name */                 \
    X(DESTINATION, destination, TOKEN)  /* Fetch §2.2.5's destination of the container */                 \
    X(REFERRER_POLICY, referrerPolicy, TOKEN) /* after Fetch §4.1 Main fetch, possibly empty */           \
    X(ADDRESS, address, TOKEN)          /* the address the child is navigated to */

#define NAVIGABLE_NOTICE_ENUM_CREATE(id, js, enc) NAVIGABLE_CREATE_##id,
#define NAVIGABLE_NOTICE_ENUM_PROVISION(id, js, enc) NAVIGABLE_PROVISION_##id,
#define NAVIGABLE_NOTICE_ENUM_REQUEST(id, js, enc) NAVIGABLE_REQUEST_##id,
#define NAVIGABLE_NOTICE_ENUM_OPEN(id, js, enc) NAVIGABLE_OPEN_##id,
enum { NAVIGABLE_CREATE_FIELDS(NAVIGABLE_NOTICE_ENUM_CREATE) NAVIGABLE_CREATE_N };
enum { NAVIGABLE_PROVISION_FIELDS(NAVIGABLE_NOTICE_ENUM_PROVISION) NAVIGABLE_PROVISION_N };
enum { NAVIGABLE_REQUEST_FIELDS(NAVIGABLE_NOTICE_ENUM_REQUEST) NAVIGABLE_REQUEST_N };
enum { NAVIGABLE_OPEN_FIELDS(NAVIGABLE_NOTICE_ENUM_OPEN) NAVIGABLE_OPEN_N };
#undef NAVIGABLE_NOTICE_ENUM_CREATE
#undef NAVIGABLE_NOTICE_ENUM_PROVISION
#undef NAVIGABLE_NOTICE_ENUM_REQUEST
#undef NAVIGABLE_NOTICE_ENUM_OPEN

/* A create field's encoding, NAVIGABLE_NOTICE_TOKEN or NAVIGABLE_NOTICE_BASE64; -1 for a position the table
   does not declare, which a caller asserts against. */
#define NAVIGABLE_NOTICE_ENCODING_CASE(id, js, enc) case NAVIGABLE_CREATE_##id: return NAVIGABLE_NOTICE_##enc;
static inline int navigable_create_encoding(int field)
{
    switch (field) {
        NAVIGABLE_CREATE_FIELDS(NAVIGABLE_NOTICE_ENCODING_CASE)
    default:
        return -1;
    }
}
#undef NAVIGABLE_NOTICE_ENCODING_CASE

#endif
