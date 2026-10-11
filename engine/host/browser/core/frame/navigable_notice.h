/* The provisioning notices' field tables: what `navigable.create`, `navigable.swap` and `navigable.navigate`
 * carry, in order, as one declaration every composer and reader derives its indices from.
 *
 * A notice is the operation name and then one TAB before each field (solver/engine.h's engine_notice_build).
 * The enumerators below are a field's position among the fields, which is the composer's array index; a reader
 * that splits the whole record on TAB finds field `i` at `1 + i`, after the operation name.
 * Each entry states its encoding. A TOKEN is written raw and its grammar holds no TAB. A REMAINDER is written
 * raw and may hold TABs, so it can only be the last field and a reader keeps everything after its index
 * (engine_notice_build asserts that only the last field holds a TAB; the checks below keep it last).
 * The trusted zone's readers take their indices from extension/lib/navigable-notice.js, which
 * engine/noticegen.mjs writes from this file; the build refuses a copy that this file does not re-render. */
#ifndef ENGINE_HOST_BROWSER_CORE_FRAME_NAVIGABLE_NOTICE_H
#define ENGINE_HOST_BROWSER_CORE_FRAME_NAVIGABLE_NOTICE_H

/* `navigable.create`: a navigable whose document another agent hosts (core/frame/navigable.c). Items of HTML
   §7.1.7's clone of the creator's container cross because the hosting instance cannot derive them, and the
   policy text is the remainder because a raw CSP header may contain a TAB. Each field's grammar is stated at the
   composer. */
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
    X(PARENT, parent, TOKEN)                   /* HTML §7.3.1.3's parent navigable (remote_object.h), or `u` */  \
    X(CONTAINER_POLICY, containerPolicy, TOKEN) /* Permissions Policy §9.5's result for the container */         \
    X(ANCESTOR_ORIGINS, ancestorOrigins, TOKEN) /* HTML §3.1.3's internal ancestor origin objects list */        \
    X(SANDBOX_FLAGS, sandboxFlags, TOKEN)      /* HTML §7.1.5's creation sandboxing flag set */                  \
    X(PROVENANCE, provenance, TOKEN)           /* what the navigation is evidence of (PENDING_PROVENANCE_*) */   \
    X(GROUP, group, TOKEN)                     /* `creator` or `new` browsing context group */                   \
    X(DRIVEN, driven, TOKEN)                   /* the drive mark (PENDING_DRIVEN_*) */                           \
    X(PINNED, pinned, TOKEN)                   /* the witness mark (PENDING_PINNED_*) */                         \
    X(OPENER, opener, TOKEN)                   /* HTML §7.2.2.4's opener (remote_object.h), or `N` */            \
    X(POLICY, policy, REMAINDER)                   /* the creator's CSP list, raw; the remainder */

/* `navigable.swap` (HTML §7.1.3.2's browsing context group swap, core/frame/browsing_context_group.c) and
   `navigable.navigate` (a top-level traversable navigated into a peer, core/frame/navigable.c): one record,
   since both provision a top-level document with no creator-side container. */
#define NAVIGABLE_PROVISION_FIELDS(X)                                                                      \
    X(DOCUMENT, document, TOKEN)               /* the new document's world name */                               \
    X(ADDRESS, address, TOKEN)                 /* the address loaded */                                          \
    X(ORIGIN, origin, TOKEN)                   /* that address's origin, serialized */                           \
    X(PROVENANCE, provenance, TOKEN)           /* what the load is evidence of (PENDING_PROVENANCE_*) */         \
    X(DRIVEN, driven, TOKEN)                   /* the drive mark (PENDING_DRIVEN_*) */                           \
    X(PINNED, pinned, TOKEN)                   /* the witness mark (PENDING_PINNED_*) */

enum { NAVIGABLE_NOTICE_TOKEN, NAVIGABLE_NOTICE_REMAINDER };

#define NAVIGABLE_NOTICE_ENUM_CREATE(id, js, enc) NAVIGABLE_CREATE_##id,
#define NAVIGABLE_NOTICE_ENUM_PROVISION(id, js, enc) NAVIGABLE_PROVISION_##id,
enum { NAVIGABLE_CREATE_FIELDS(NAVIGABLE_NOTICE_ENUM_CREATE) NAVIGABLE_CREATE_N };
enum { NAVIGABLE_PROVISION_FIELDS(NAVIGABLE_NOTICE_ENUM_PROVISION) NAVIGABLE_PROVISION_N };
#undef NAVIGABLE_NOTICE_ENUM_CREATE
#undef NAVIGABLE_NOTICE_ENUM_PROVISION

/* A record has at most one REMAINDER and it is the last field: the count of remainders, and the sum of their
   positions, are compile-time sums over the table. */
#define NAVIGABLE_NOTICE_IS_REMAINDER(enc) (NAVIGABLE_NOTICE_##enc == NAVIGABLE_NOTICE_REMAINDER)
#define NAVIGABLE_NOTICE_COUNT_CREATE(id, js, enc) + NAVIGABLE_NOTICE_IS_REMAINDER(enc)
#define NAVIGABLE_NOTICE_AT_CREATE(id, js, enc) + (NAVIGABLE_NOTICE_IS_REMAINDER(enc) ? NAVIGABLE_CREATE_##id : 0)
#define NAVIGABLE_NOTICE_COUNT_PROVISION(id, js, enc) + NAVIGABLE_NOTICE_IS_REMAINDER(enc)
#define NAVIGABLE_NOTICE_AT_PROVISION(id, js, enc) \
    + (NAVIGABLE_NOTICE_IS_REMAINDER(enc) ? NAVIGABLE_PROVISION_##id : 0)
_Static_assert((0 NAVIGABLE_CREATE_FIELDS(NAVIGABLE_NOTICE_COUNT_CREATE)) == 0 ||
               ((0 NAVIGABLE_CREATE_FIELDS(NAVIGABLE_NOTICE_COUNT_CREATE)) == 1 &&
                (0 NAVIGABLE_CREATE_FIELDS(NAVIGABLE_NOTICE_AT_CREATE)) == NAVIGABLE_CREATE_N - 1),
               "the create notice has a REMAINDER that is not its one last field, and only the last may hold a TAB");
_Static_assert((0 NAVIGABLE_PROVISION_FIELDS(NAVIGABLE_NOTICE_COUNT_PROVISION)) == 0 ||
               ((0 NAVIGABLE_PROVISION_FIELDS(NAVIGABLE_NOTICE_COUNT_PROVISION)) == 1 &&
                (0 NAVIGABLE_PROVISION_FIELDS(NAVIGABLE_NOTICE_AT_PROVISION)) == NAVIGABLE_PROVISION_N - 1),
               "the provision notice has a REMAINDER that is not its one last field");
#undef NAVIGABLE_NOTICE_COUNT_CREATE
#undef NAVIGABLE_NOTICE_AT_CREATE
#undef NAVIGABLE_NOTICE_COUNT_PROVISION
#undef NAVIGABLE_NOTICE_AT_PROVISION
#undef NAVIGABLE_NOTICE_IS_REMAINDER

#endif
