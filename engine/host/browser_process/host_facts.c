/* host_facts.c — the boundary's checks. See host_facts.h for what the boundary IS.
 *
 * THIS FILE LINKS INTO THE BROWSER PROCESS ALONE, which is not a filing decision: `build.mjs`'s
 * BROWSER_PROCESS_SOURCES walks this directory and the objects enter the browser-process link only, so a
 * RENDERER source that tries to adopt a principal or reach a policy entry does not LINK — in dev and in
 * release alike. That is the same enforcement the same file already gives MIME Sniffing §7, and it is why the
 * unconditional DFAIL that used to stand inside `mime_sniff_compute` could be deleted. */

#include <string.h>

#include "check.h"
#include "browser_process/host_facts.h"

/* THE CLASSIFICATION IS THIS SWITCH, and it has no `default:` on purpose: clang's -Wswitch is on by default,
   so an enumerator nobody classified is a diagnostic at the line that added it, and the CHECK_FAIL below is
   what catches it in a build where the warning was quieted. A prose table would go stale the way CLAUDE.md
   §DFAIL describes — accurate about the spec, wrong about this tree — and this one cannot, because a new fact
   does not compile cleanly until it has been given a class. */
HostFactClass host_fact_class(HostFact f)
{
    switch (f) {
    case HOST_FACT_INITIATOR_PRINCIPAL:
    case HOST_FACT_INITIATOR_ADDRESS:
    case HOST_FACT_TOP_LEVEL_ADDRESS:
    case HOST_FACT_BROWSING_CONTEXT_GROUP:
    case HOST_FACT_FRAME_ID:
    case HOST_FACT_HOST_PERMISSION:
    case HOST_FACT_CREDENTIALED:
    case HOST_FACT_EXTENSION_ORIGIN:
    case HOST_FACT_OFFSCREEN_DOCUMENT_URL:
    case HOST_FACT_RESPONSE_HEADERS:
    case HOST_FACT_RESPONSE_URL_LIST:
    case HOST_FACT_COOKIE_JAR:
        return HOST_FACT_CLASS_BROWSER_STATED;
    case HOST_FACT_SCHEME_FILTER:
    case HOST_FACT_PRIVATE_NETWORK_CLASS:
    case HOST_FACT_CORB_BY_LOAD_TYPE:
    case HOST_FACT_CORS_CREDENTIALED:
    case HOST_FACT_METHOD_GET_ONLY:
    case HOST_FACT_MIME_SNIFF:
        return HOST_FACT_CLASS_ALGORITHM;
    case HOST_FACT_URL_DERIVED_ORIGIN:
    case HOST_FACT_CONTENT_SCRIPT_STATED_URL:
    case HOST_FACT_SENDER_ID:
    case HOST_FACT_SELF_REPORTED_TOP:
    case HOST_FACT_ENGINE_MINTED_DOCUMENT_NAME:
    case HOST_FACT_ENGINE_STATED_ORIGIN:
    case HOST_FACT_ENGINE_STATED_METHOD:
        return HOST_FACT_CLASS_REPORTED_NEVER_DECIDED;
    }
    CHECK_FAIL("a HostFact reached the classifier with no class — the boundary between what this process may "
               "be TOLD and what it may DECIDE is this switch, so an unclassified fact is a security-relevant "
               "value whose zone nobody decided");
}

bool host_origin_is_tuple(const char *serialized)
{
    const char *sep;
    if (serialized == NULL)
        return false;
    sep = strstr(serialized, "://");
    /* `sep != serialized` is `_isRealOrigin`'s `indexOf("://") > 0`: a form with no SCHEME before the authority
       is not a tuple origin, and "null" / "" / "null:<uuid>" have no "://" at all. */
    return sep != NULL && sep != serialized;
}

HostPrincipal host_principal_adopt(const char *browser_stated_origin, const char *document_id)
{
    HostPrincipal p;
    CHECK(browser_stated_origin != NULL && browser_stated_origin[0] != '\0',
          "a request reached the browser process with an empty principal — SECURITY.md fixes the "
          "credentialed-read principal at the requesting frame's MessageSender.origin, and an empty one is the "
          "trusted zone having failed to carry it rather than a document that has no origin: an opaque origin "
          "is a VALUE, minted per document, and is same-origin with nothing");
    CHECK(document_id != NULL && document_id[0] != '\0',
          "a principal arrived naming no documentId — the browser answers an origin per DOCUMENT and a "
          "(tab, frame) pair is reused across navigations at a DIFFERENT origin, so a principal that cannot "
          "name its document cannot be audited back to the MessageSender it came from");
    p.serialized = browser_stated_origin;
    p.document_id = document_id;
    p.stated = HOST_PRINCIPAL_STATED;
    return p;
}

bool host_principal_is_opaque(HostPrincipal p)
{
    CHECK(p.stated == HOST_PRINCIPAL_STATED,
          "a value that was never adopted from a MessageSender was read as a principal — this process cannot "
          "mint one and must not act on one it was not handed");
    return !host_origin_is_tuple(p.serialized);
}

bool host_principal_same_origin(HostPrincipal p, const char *resource_origin_from_url)
{
    if (host_principal_is_opaque(p))
        return false;
    if (!host_origin_is_tuple(resource_origin_from_url))
        return false;
    return strcmp(p.serialized, resource_origin_from_url) == 0;
}

HostZoneFacts host_zone_facts_adopt(const char *extension_origin, const char *offscreen_document_url)
{
    HostZoneFacts z;
    CHECK(host_origin_is_tuple(extension_origin),
          "the browser process was constructed without its own extension origin — every document-to-document "
          "hop in this extension is authenticated by sender.origin == chrome-extension://<id>, and a process "
          "that does not know its own origin cannot make that comparison");
    CHECK(offscreen_document_url != NULL && offscreen_document_url[0] != '\0',
          "the browser process was constructed without the offscreen document's URL — background.js pins the "
          "privileged chrome.* relay to that exact document, so a process missing it cannot tell the one "
          "trusted document from another extension page of the same origin");
    z.extension_origin = extension_origin;
    z.offscreen_document_url = offscreen_document_url;
    z.stated = HOST_ZONE_STATED;
    return z;
}

const char *host_request_fact_value(const HostRequestFacts *req, HostFact f)
{
    CHECK(req != NULL, "a request fact was asked for out of no request record at all");
    switch (f) {
    case HOST_FACT_INITIATOR_PRINCIPAL:    return req->principal.serialized;
    case HOST_FACT_INITIATOR_ADDRESS:      return req->initiator_address;
    case HOST_FACT_TOP_LEVEL_ADDRESS:      return req->top_level_address;
    case HOST_FACT_BROWSING_CONTEXT_GROUP: return req->browsing_context_group;
    /* Browser-stated, and NOT this record's strings: the three flags beside them are read as flags, and the
       last five belong to the process record or to a reply. NULL is the positive statement "not here". */
    case HOST_FACT_FRAME_ID:
    case HOST_FACT_HOST_PERMISSION:
    case HOST_FACT_CREDENTIALED:
    case HOST_FACT_EXTENSION_ORIGIN:
    case HOST_FACT_OFFSCREEN_DOCUMENT_URL:
    case HOST_FACT_RESPONSE_HEADERS:
    case HOST_FACT_RESPONSE_URL_LIST:
    case HOST_FACT_COOKIE_JAR:
        return NULL;
    case HOST_FACT_SCHEME_FILTER:
    case HOST_FACT_PRIVATE_NETWORK_CLASS:
    case HOST_FACT_CORB_BY_LOAD_TYPE:
    case HOST_FACT_CORS_CREDENTIALED:
    case HOST_FACT_METHOD_GET_ONLY:
    case HOST_FACT_MIME_SNIFF:
        CHECK_FAIL("an ALGORITHM was asked for as if it were a fact a request carries — this process COMPUTES "
                   "these, and a request that carried one would be the asking side handing in its own verdict");
    case HOST_FACT_URL_DERIVED_ORIGIN:
    case HOST_FACT_CONTENT_SCRIPT_STATED_URL:
    case HOST_FACT_SENDER_ID:
    case HOST_FACT_SELF_REPORTED_TOP:
    case HOST_FACT_ENGINE_MINTED_DOCUMENT_NAME:
    case HOST_FACT_ENGINE_STATED_ORIGIN:
    case HOST_FACT_ENGINE_STATED_METHOD:
        CHECK_FAIL("a REPORTED_NEVER_DECIDED value was read off the request record as authority — these cross "
                   "as data the trusted zone may relay and may never act on, so there is no field to read");
    }
    CHECK_FAIL("a HostFact was asked of a request record with no case — see host_fact_class");
}

void host_request_facts_check(const HostRequestFacts *req)
{
    int f;
    CHECK(req != NULL,
          "a policy decision was reached with no request facts at all — every check in this process is about "
          "ONE initiator, and a decision made without knowing which one is a confused deputy by construction");
    CHECK(req->principal.stated == HOST_PRINCIPAL_STATED,
          "a request carried a principal that was never adopted from a MessageSender — this process cannot "
          "mint one, so it is a value some other zone invented and handed over as authority");
    for (f = HOST_FACT_INITIATOR_PRINCIPAL; f <= HOST_FACT_BROWSING_CONTEXT_GROUP; f++) {
        const char *v;
        CHECK(host_fact_class((HostFact)f) == HOST_FACT_CLASS_BROWSER_STATED,
              "the browser-stated run at the head of HostFact stopped being contiguous — this loop's bound is "
              "written against that order, so a fact inserted into the middle of it would go unchecked while "
              "the loop still looked like it covered everything");
        v = host_request_fact_value(req, (HostFact)f);
        CHECK(v != NULL && v[0] != '\0',
              "a browser-stated fact reached the browser process empty — it is a chrome.* answer no WASM "
              "instance can ask for, so an empty one is the trusted zone not having carried it, and every "
              "decision made from here would be made about an initiator this process cannot name");
    }
    DCHECK(req->frame_id >= 0,
           "a request named a negative frame id — sender.frameId is browser-set and 0 IS the top-level "
           "traversable, so a negative one is a value that did not come off a MessageSender");
    DCHECK(req->host_permission,
           "a request states that the BROWSER applied its own same-origin policy to this fetch — then the "
           "re-implementation in this process is not what stands between the bundle and another origin's "
           "bytes, and these checks are being asked a question they were not written for. manifest.json "
           "declares <all_urls>, so every analyzer fetch bypasses the browser's SOP and this is always true");
}
