/* The provisioning notices' field tables: what `navigable.create`, `navigable.swap` and `navigable.navigate`
 * carry, in order, as one declaration every composer and reader derives its indices from.
 *
 * A notice is the operation name and then one TAB before each field (solver/engine.h's engine_notice_build).
 * The enumerators below are a field's position among the fields, which is the composer's array index; a reader
 * that splits the whole record on TAB finds field `i` at `1 + i`, after the operation name. Only the last field
 * may hold a TAB (engine_notice_build asserts it), so a reader caps its split there and keeps the remainder.
 * The trusted zone's readers take their indices from extension/lib/navigable-notice.js, which
 * engine/noticegen.mjs writes from this file; the build refuses a copy that this file does not re-render. */
#ifndef ENGINE_HOST_BROWSER_CORE_FRAME_NAVIGABLE_NOTICE_H
#define ENGINE_HOST_BROWSER_CORE_FRAME_NAVIGABLE_NOTICE_H

/* `navigable.create`: a navigable whose document another agent hosts (core/frame/navigable.c). Items of HTML
   §7.1.7's clone of the creator's container cross because the hosting instance cannot derive them, and the
   policy text is the remainder because a raw CSP header may contain a TAB. Each field's grammar is stated at the
   composer. */
#define NAVIGABLE_CREATE_FIELDS(X)                                                                         \
    X(CHILD, child)                     /* the new document's world name */                               \
    X(CREATOR, creator)                 /* the creating document's world name */                          \
    X(ADDRESS, address)                 /* the address the navigable is navigated to */                   \
    X(ORIGIN, origin)                   /* that address's origin, serialized */                           \
    X(TOP_LEVEL_URL, topLevelUrl)       /* HTML §8.1.3.1's top-level creation URL */                      \
    X(CSP_SELF_ORIGIN, cspSelfOrigin)   /* CSP §2.2's self-origin of the creator's list */                \
    X(COEP, coep)                       /* HTML §7.1.4's embedder policy value */                         \
    X(COEP_ENDPOINT, coepEndpoint)      /* its reporting endpoint */                                      \
    X(COEP_REPORT_ONLY, coepReportOnly) /* its report-only value */                                       \
    X(COEP_REPORT_ONLY_ENDPOINT, coepReportOnlyEndpoint)                                                  \
    X(PARENT, parent)                   /* HTML §7.3.1.3's parent navigable (remote_object.h), or `u` */  \
    X(CONTAINER_POLICY, containerPolicy) /* Permissions Policy §9.5's result for the container */         \
    X(ANCESTOR_ORIGINS, ancestorOrigins) /* HTML §3.1.3's internal ancestor origin objects list */        \
    X(SANDBOX_FLAGS, sandboxFlags)      /* HTML §7.1.5's creation sandboxing flag set */                  \
    X(PROVENANCE, provenance)           /* what the navigation is evidence of (PENDING_PROVENANCE_*) */   \
    X(GROUP, group)                     /* `creator` or `new` browsing context group */                   \
    X(DRIVEN, driven)                   /* the drive mark (PENDING_DRIVEN_*) */                           \
    X(PINNED, pinned)                   /* the witness mark (PENDING_PINNED_*) */                         \
    X(OPENER, opener)                   /* HTML §7.2.2.4's opener (remote_object.h), or `N` */            \
    X(POLICY, policy)                   /* the creator's CSP list, raw; the remainder */

/* `navigable.swap` (HTML §7.1.3.2's browsing context group swap, core/frame/browsing_context_group.c) and
   `navigable.navigate` (a top-level traversable navigated into a peer, core/frame/navigable.c): one record,
   since both provision a top-level document with no creator-side container. */
#define NAVIGABLE_PROVISION_FIELDS(X)                                                                      \
    X(DOCUMENT, document)               /* the new document's world name */                               \
    X(ADDRESS, address)                 /* the address loaded */                                          \
    X(ORIGIN, origin)                   /* that address's origin, serialized */                           \
    X(PROVENANCE, provenance)           /* what the load is evidence of (PENDING_PROVENANCE_*) */         \
    X(DRIVEN, driven)                   /* the drive mark (PENDING_DRIVEN_*) */                           \
    X(PINNED, pinned)                   /* the witness mark (PENDING_PINNED_*) */

#define NAVIGABLE_NOTICE_ENUM_CREATE(id, js) NAVIGABLE_CREATE_##id,
#define NAVIGABLE_NOTICE_ENUM_PROVISION(id, js) NAVIGABLE_PROVISION_##id,
enum { NAVIGABLE_CREATE_FIELDS(NAVIGABLE_NOTICE_ENUM_CREATE) NAVIGABLE_CREATE_N };
enum { NAVIGABLE_PROVISION_FIELDS(NAVIGABLE_NOTICE_ENUM_PROVISION) NAVIGABLE_PROVISION_N };
#undef NAVIGABLE_NOTICE_ENUM_CREATE
#undef NAVIGABLE_NOTICE_ENUM_PROVISION

/* The policy is the create record's remainder: the one field allowed a TAB, so it must be last. */
_Static_assert(NAVIGABLE_CREATE_POLICY == NAVIGABLE_CREATE_N - 1,
               "the create notice's policy is not its last field, and only the last field may hold a TAB");

#endif
