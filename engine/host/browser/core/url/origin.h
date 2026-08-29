/* ORIGIN — HTML §7.1.1 ("§7.5" in the older numbering this tree's comments use) and URL §4.7.
 *
 * AN ORIGIN IS A RECORD WITH AN IDENTITY, AND THIS ENGINE HELD A STRING. That is not a naming quibble: HTML
 * says an origin is "one of the following — an opaque origin: an internal value, with no serialization it can
 * be recreated from (it is serialized as `null` per serialization of an origin), FOR WHICH THE ONLY MEANINGFUL
 * OPERATION IS TESTING FOR EQUALITY; a tuple origin: a tuple consisting of a scheme, a host, a port and a
 * domain". Everything downstream — §7.2.1's WindowProxy filter, §7.3.1's `contentDocument`, §7.2.6.3's
 * disabled clause, Storage's storage key, Permissions §5.1 — is defined over that record, and a component
 * holding the SERIALIZATION cannot run the first step of the first algorithm:
 *
 *     "Two origins, A and B, are said to be same origin if the following algorithm returns true:
 *        1. If A and B are THE SAME OPAQUE ORIGIN, then return true.
 *        2. If A and B are both TUPLE ORIGINS and their schemes, hosts, and port are identical, then return
 *           true.
 *        3. Return false."
 *
 * Step 1 compares IDENTITY, and identity is exactly what the serialization drops: every opaque origin
 * serializes to "null". A string-holding engine therefore cannot tell ONE opaque origin looked at twice from
 * TWO distinct ones, and this one guessed "two" — right for two sandboxed frames, wrong for the case step 1
 * exists for. §7.3.2.1's determine-the-origin hands ONE opaque origin to several Documents on purpose ("if url is
 * about:srcdoc … return sourceOrigin", "if url matches about:blank and sourceOrigin is non-null, then return
 * sourceOrigin", with the standard's own note that these "result in two Documents that end up with the same
 * underlying origin"), so a `data:` document's `about:blank` child is a pair that MUST be same origin and was
 * not: its `contentDocument` was null, every §7.2.1 member outside the fixed cross-origin list was a
 * SecurityError, and — because an instance is an origin-keyed agent cluster — the child was routed to a PEER
 * INSTANCE that no host provisions.
 *
 * SO THE IDENTITY IS A NONCE (Blink's SecurityOrigin carries one for the same reason). It is MINTED by every
 * site where the standard says "return a NEW opaque origin" (URL §4.7's `data:`/`file:`/unknown-scheme case,
 * HTML §7.3.2.1 Creating browsing contexts' sandboxed-origin case and its null-url case) and COPIED — the
 * same record, so the same nonce — by §7.3.2.1's two inheritance cases. After that, `origin_same` decides
 * step 1 by comparing nonces and step 2 by comparing tuples, and nothing anywhere decides anything with a
 * serialization.
 *
 * THE SERIALIZATION STAYS, AS AN OPERATION AND NOT AS THE TYPE. §7.1.1's serializer is what `event.origin`,
 * `location.origin`, a create notice and a storage key are made of — all of them BYTES leaving this engine —
 * and its lossiness is harmless exactly because no decision reads it. The one comparison a serialization can
 * still decide is spelled out as its own function below (origin_is_serialized_tuple), because §9.4.4's
 * targetOrigin arrives as a page-supplied string and can only ever name a tuple.
 *
 * WHY IT LIVES UNDER core/url. The record is URL §4.7's answer ("return the tuple origin (url's scheme, url's
 * host, url's port, null)"), it is built out of a UrlRecord, and it is the type url.c's `url_serialize_origin`
 * has always been serializing without ever holding. HTML defines the ALGORITHMS over it, which is why their
 * spec sentences are quoted at each one in origin.c.
 *
 * LIFETIME: an origin lives for the AGENT and is immutable in every component but one. §7.1.1: origins "are
 * generally immutable. Only the domain of a tuple origin can be changed, and only through the document.domain
 * API" — so origin_set_domain below is the single writer, it captures into the running flow's COW delta before
 * it writes, and every domain it has ever parsed stays alive with the records. Nothing here is freed while the
 * agent runs: a parked flow's COW delta holds POD pointers into these records (window_proxy.c captures its
 * ProxyData by bytes, and the domain slot is captured by bytes too), so a freed origin — or a freed domain —
 * would resume a flow onto freed memory. The whole list goes at platform_agent_free. */
#ifndef ENGINE_HOST_BROWSER_CORE_URL_ORIGIN_H
#define ENGINE_HOST_BROWSER_CORE_URL_ORIGIN_H
#include <stdbool.h>
#include <stdint.h>

#include "core/url/url.h"

typedef struct Origin Origin;

/* §7.1.1's TWO KINDS, asked as one predicate. An opaque origin has no scheme, host or port to read — the only
   meaningful operation on it is equality — so a caller that needs those must ask this first. */
bool origin_is_opaque(const Origin *o);

/* §7.1.1's SAME ORIGIN, both steps. */
bool origin_same(const Origin *a, const Origin *b);

/* §7.1.1's SAME ORIGIN-DOMAIN, which is a DIFFERENT algorithm and not a laxer spelling of the one above: it is
   what §7.3.1's `content document` filters `iframe.contentDocument` by, while §7.2.1's cross-origin filter
   uses same ORIGIN. The standard prints a five-row table for the pair precisely because they disagree BOTH
   WAYS — equal domains with different ports are same origin-domain and NOT same origin; one side having set a
   domain makes a pair same origin and NOT same origin-domain — so a filter that agreed with same origin for
   every input would pass a test that only checked the agreeing rows. */
bool origin_same_origin_domain(const Origin *a, const Origin *b);

/* §7.1.1's EFFECTIVE DOMAIN: "1. If origin is an opaque origin, then return null. 2. If origin's domain is
   non-null, then return origin's domain. 3. Return origin's host." NULL is step 1 and it is what makes
   §7.1.1.2's `document.domain` getter answer the empty string and its setter throw. BORROWED — it is the
   record's own host or the agent's parsed domain, both of which live for the agent. */
const UrlHost *origin_effective_domain(const Origin *o);

/* §7.1.1's THREE TUPLE COMPONENTS, for the algorithms that READ an origin rather than compare two of them.
   CSP §6.7.2.8 is the caller and it reads all three: its schemeless-`host-source` arm asks whether the CSP
   list's SELF-ORIGIN's scheme §6.7.2.9 scheme-part matches the URL's, and the second bullet of its `'self'`
   arm compares the origin's host and port against the URL's for the secure-upgrade case where the two origins
   are not same origin.
   THESE ARE NOT origin_effective_domain, and the difference is exactly the one §7.1.1 draws: the effective
   domain answers the DOMAIN a `document.domain` write relaxed an origin to, while §6.7.2.8 says "origin's
   host" — a relaxed origin still has its own host, and matching a policy against the relaxed value would let a
   page widen its own `'self'` by assigning to `document.domain`.
   AN OPAQUE ORIGIN HAS NONE OF THEM. §7.1.1 gives it no scheme, no host and no port — "the only meaningful
   operation is testing for equality" — so each of these ASSERTS that the caller asked origin_is_opaque first
   and answered that case itself, rather than handing back a hole the caller would fill with a default. */
const char    *origin_scheme(const Origin *o);
const UrlHost *origin_host(const Origin *o);
int            origin_port(const Origin *o);   /* §4.1's null port is -1, exactly as in a UrlRecord */

/* §7.1.1's SAME ORIGIN WITH A URL ON ONE SIDE — "origin and url's origin are same origin", which is the first
   bullet of CSP §6.7.2.8's `'self'` arm and the only shape in which that comparison is ever made there.
   IT IS THE ONE READING OF §7.1.1 (origin_same decides it) AND IT MINTS NOTHING. URL §4.7 gives a URL with no
   tuple origin — `data:`, `file:`, an unknown scheme — a NEW OPAQUE ORIGIN, which is same origin with nothing
   including itself, so §7.1.1 step 1 answers false against every record and the mint is skipped rather than
   performed and thrown away. origin_of_url would keep an agent-lifetime record per question asked, and this
   question is asked of every source expression of every policy of every fetch. */
bool origin_same_as_url(const Origin *o, const UrlRecord *u);

/* §7.1.1.2's SETTER STEP 6, "set this's origin's domain to the result of parsing the given value" — and the ONE
   place an origin is ever mutated. §7.1.1: origins "are generally immutable. Only the domain of a tuple origin
   can be changed, and only through the document.domain API", which is why this takes a `const Origin *` like
   every other reader: the constness is the standard's sentence, and this function is its one exception.
   IT IS A PER-FLOW WRITE ON SHARED STATE, so it captures into the running flow's COW delta before it writes —
   an arm that relaxed its domain must not leave a sibling arm's `contentDocument` null. `owner` is the JS
   object whose algorithm is doing the writing (the Document), which is what anchors the delta entry.
   THE DOMAIN IS COPIED into agent-lifetime storage and never freed while the agent runs, for the same reason
   the records are: a parked flow's delta holds this slot as a POD pointer, so a freed domain would resume that
   flow onto freed memory. The caller keeps ownership of what it passed. */
void origin_set_domain(JSContext *ctx, JSValueConst owner, const Origin *o, const UrlHost *domain);

/* §7.1.1's SERIALIZATION — "null" for an opaque origin, `scheme://host[:port]` otherwise. BORROWED and
   computed once: it is a pure function of an immutable record, and every caller of it is producing BYTES that
   leave this engine (`event.origin`, a create notice, a storage key). Nothing decides with it. */
const char *origin_serialized(const Origin *o);

/* THE ONE COMPARISON A SERIALIZATION CAN DECIDE — is `o` the TUPLE origin that `serialized` names? §9.4.4's
   `postMessage` targetOrigin arrives as a page-supplied string, and a string can only ever name a tuple
   origin, so this is §7.1.1 step 2 with one side still in bytes. An OPAQUE `o` answers FALSE for every input
   INCLUDING "null" — that is step 1 refusing to be faked, and it is the same rule SECURITY.md states for the
   credentialed-read principal. */
bool origin_is_serialized_tuple(const Origin *o, const char *serialized);

/* URL §4.7's ORIGIN OF A URL, with identity: the tuple origin for a scheme that has one, and a NEW OPAQUE
   ORIGIN — a fresh nonce, so it is same origin with nothing but itself — for every other, `data:` and `file:`
   included ("this does indeed mean that these URLs cannot be same origin with themselves", which is true of
   two SEPARATE parses and false of one record read twice: that difference IS the nonce). */
const Origin *origin_of_url(const UrlRecord *u);

/* URL §4.7 + §7.1.1's serializer, as BYTES, minting no identity — for a caller that wants the string and never
   the record (`location.origin`, `URL.origin`, a blob's origin). It is the same one implementation of §4.7 as
   origin_of_url; that is why it is here rather than in url.c, which used to hold a second copy of the rule. */
char *origin_serialize_of_url(const UrlRecord *u);   /* OWNED */

/* URL §4.7's TUPLE, WITHOUT MINTING AN IDENTITY — the record whose scheme, host and port ARE this URL's tuple
   origin, or NULL when §4.7 gives it an opaque one. It answers `u` itself for a URL that carries its own
   origin and `scratch` for a `blob:`, whose origin is the URL its path spells; `scratch` is INITIALISED on
   every path and is the caller's to url_record_free whatever the answer was.
   IT EXISTS FOR THE ALGORITHM THAT READS COMPONENTS AND DECIDES NOTHING ABOUT IDENTITY (Secure Contexts §3.1
   over a URL), which would otherwise mint one origin per [SecureContext] member check — a record per call,
   kept for the agent, for a question that never compares two origins. */
const UrlRecord *origin_tuple_url(const UrlRecord *u, UrlRecord *scratch);

/* HTML §7.3.2.1 "Creating browsing contexts"' DETERMINE THE ORIGIN, verbatim, and the reason this file exists:
     1. If sandboxFlags has its sandboxed origin browsing context flag set, then return a new opaque origin.
     2. If url is null, then return a new opaque origin.
     3. If url is about:srcdoc: assert sourceOrigin is non-null; return sourceOrigin.
     4. If url matches about:blank and sourceOrigin is non-null, then return sourceOrigin.
     5. Return url's origin.
   Steps 3 and 4 return the SAME RECORD — that is the copy, and it is what makes a `data:` document's
   `about:blank` child same origin with it. `url` is NULL for the standard's null URL. */
const Origin *origin_determine(const UrlRecord *url, bool sandboxed_origin, const Origin *source);

/* THE AGENT'S ORIGIN — one record for the whole instance, because an instance IS an origin-keyed agent cluster
   (SECURITY.md), so "this document's origin" and "this agent's origin" are one fact. It is the ACCESSOR side of
   every same-origin check: §7.2.1 compares a navigable's origin against it.
   ADOPTED ONCE, from the serialization the trusted zone states (platform.c), because the host knows the
   principal and the engine may not invent one. A host that says "null" is stating an OPAQUE origin, and this is
   where that origin's identity is minted — every later reader of it gets THE SAME record, which is the whole
   point: a serialization round trip would mint a second one. */
void origin_agent_adopt(const char *serialized);
const Origin *origin_agent(void);

/* A PEER'S ORIGIN, FROM THE SERIALIZATION THE TRUSTED ZONE STAMPED — the only way an origin crosses an instance
   boundary, since a record does not cross a process any more than a JSValue does. "null" MINTS a fresh opaque
   origin, which is spec-exact ("an internal value, with no serialization it can be recreated from"): a peer's
   opaque origin is same origin with nothing here, and it never needs to be, because two documents sharing one
   opaque origin share an agent cluster and therefore an instance. */
const Origin *origin_parse(const char *serialized);

/* AN ORIGIN'S HANDLE, for the one place an origin must ride a JSValue: a scheduled job's arguments and a
   §7.4.1.2 document state. It is a HANDLE and never a serialization — that is the whole distinction this file
   is about — so it is meaningful only inside the agent that minted it. */
uint32_t origin_id(const Origin *o);
const Origin *origin_by_id(uint32_t id);

/* Released with the AGENT (platform_agent_free), never before: every parked flow's delta names these records. */
void origin_release(void);

#endif
