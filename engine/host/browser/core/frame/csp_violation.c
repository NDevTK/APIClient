/* CSP §2.4 "Violations" AND ITS TWO OBSERVABLES — §5.2 "Obtain the blockedURI of a violation's resource",
 * §5.4 "Strip URL for use in reports" and §5.5 "Report a violation".
 *
 * WHAT WAS BLOCKED ON THIS. Every refusal this engine makes on a page's behalf — §4.1.2 blocking a request,
 * §4.2.3 refusing an inline script or style, §4.4.1 stopping an `eval`, §6.3.1.1 rejecting a `<base href>` —
 * ends in §5.5, and §5.5 was not built. The BLOCKS were right and whole; what a page could observe of them
 * was nothing at all. core/events/event_target.c has installed `onsecuritypolicyviolation` for as long as it
 * has had a handler table and nothing in this tree ever wrote it, so a document that counted the event across
 * every block this engine made read zero where a browser fires one per violating policy.
 *
 * ONLY THE EVENT HALF IS HERE, AND THAT IS A BOUNDARY RATHER THAN A STAGE OF THE WORK. §5.5 has two
 * observables and they are not this component's to make alike. The EVENT is a dispatch inside one realm and
 * needs no network at all. The REPORT is a `fetch` — "let request be a new request initialized as follows:
 * method POST, url endpoint …" — and whether such a request may be SENT is a firing decision for the trusted
 * zone's one chokepoint to make out of its method, its credential state and the provenance of the path that
 * reached it: a violation this engine FORCED is a report no real client would have sent, addressed to an
 * endpoint a stranger's header named. SECURITY.md keeps every network decision at that chokepoint and the
 * engine holds none, so this component composes a violation and decides no egress.
 *   NAMED RESIDUAL — WHAT IS NOT COVERED: §5.5's `report-uri` and `report-to` arms, which are everything
 *   after its event step. WHAT THE NEXT DIFF BUILDS: §5.3 "Obtain the deprecated serialization of violation"
 *   and Reporting's "generate and queue a report", composed HERE and handed to `safeFetch` as a request
 *   CARRYING ITS PROVENANCE, so the trusted zone decides whether it goes rather than the engine.
 *   HOW ITS ABSENCE WOULD SHOW: a serving host's access log carries no `application/csp-report` POST for a
 *   document whose policy names a `report-uri`, where a browser sends one per violation.
 *
 * FOUR OF §2.4's FIELDS ARE NOT POPULATED AND EACH IS ITS OWN RESIDUAL, recorded at the field rather than
 * here so a reader meets each one where they would read it.
 *
 * THE VIOLATION IS CREATED AND REPORTED IN ONE ACT, which is why no violation struct leaves this file. §4.1.2
 * step 3.3.1 is one step — "execute §5.5 Report a violation on the RESULT OF executing §2.4.2 …" — and the
 * only reader of a violation object anywhere in CSP is §5.5 and the two serializations it feeds. A struct in
 * the header would be a shape every caller could half-fill; a call cannot be half-made. */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/dom/document.h"
#include "core/dom/node.h"
#include "core/dom/shadow_root.h"
#include "core/events/event_target.h"
#include "core/events/security_policy_violation_event.h"
#include "core/frame/csp_directive_list.h"
#include "core/frame/csp_source_list.h"
#include "core/frame/csp_violation.h"
#include "core/url/url.h"

CspReporter csp_reporter(JSContext *ctx)
{
    CspReporter r;

    DCHECK(ctx != NULL,
           "a CSP reporter was built with no realm. CSP §2.4.1 reads the GLOBAL OBJECT's url and §5.5 fires "
           "at that global's associated Document, so there is no reporting to be done without one — a caller "
           "running the decision outside any global object says so with csp_reporter_none, which is a "
           "different claim about a named algorithm and reads as one");
    r.ctx = ctx;
    r.stated = true;
    return r;
}

CspReporter csp_reporter_none(void)
{
    CspReporter r;

    r.ctx = NULL;
    r.stated = true;
    return r;
}

/* ---- CSP §5.4 "Strip URL for use in reports" -------------------------------------------------------------- */

/* "Given a URL url, this algorithm returns a string representing the URL for use in violation reports."
   Every step, in its own order. Returns an OWNED string, or NULL on an allocation failure.
   STEP 1 RETURNS A BARE SCHEME AND NOT A URL, which is the whole of this algorithm's privacy purpose: a
   `data:` or `blob:` or `filesystem:` address carries the content itself, so what a report may say about one
   is its scheme and nothing more.
   STEP 2 IS SPELLED `null` HERE AND §5.4 SAYS "the empty string", AND THAT DEVIATION IS DELIBERATE — IT IS
   THE ONE WORD OF §5.4 THIS COMPONENT READS AGAINST ITS LETTER, SO IT CARRIES ITS EVIDENCE.
   This file used to write the empty string literally, and the observable of that was large: URL §4.5 "URL
   serializing" says "If exclude fragment is false and url's fragment is NON-NULL, then append U+0023 (#),
   followed by url's fragment, to output", and core/url/url.c's own arm tests the POINTER — so every
   `documentURI` and every `blockedURI` ended in a bare `#`, for an address that carried no fragment as much
   as for one that did. Step 5 invokes "the URL serializer" with no exclude-fragment argument, and §4.5 gives
   that argument a default of false, so the literal composition of the two steps really does emit the `#`.
   WHY THE WORD RATHER THAN THE FLAG. One of the two steps has to give, and only one of them has a spec
   sentence behind it. Referrer Policy §8.4 "Strip url for use as a referrer" is the same working group's
   algorithm for the same job — its prose says "a URLs fragment, username, and password components must be
   stripped from the URL before it's sent out" — and it spells the three steps APART: "Set url's username to
   the empty string", "Set url's password to the empty string", "Set url's fragment to NULL". That is the
   distinction §5.4 loses: an empty username or password serializes to nothing, because §4.5 emits credentials
   only when "url includes credentials", while an empty FRAGMENT is the one of the three whose empty value is
   still emitted, and null is the only value that strips it. §5.4 carries the credential idiom onto a field
   where it has a different observable, which is why the section titled "Strip URL for use in reports" would
   not strip. So the deviation is confined to step 2 and step 5 stays exactly as §5.4 writes it — the plain
   serializer, no flag — rather than the other way round, for which no standard anywhere gives a sentence.
   MEASURED, AND THE SPEC IS NOT ALONE IN SAYING SO. web-platform-tests' own
   `content-security-policy/support/testharness-helper.js` resolves on `e.blockedURI == url` against a
   FRAGMENTLESS url, so the literal reading does not fail those tests, it hangs them; and a document served
   `connect-src 'none'` whose handler beacons its fields to the serving host's access log had real Chrome
   write `https://example.invalid/x` where this engine wrote `https://example.invalid/x#`, in one log, from
   one fixture, the two runs separated by `sec-fetch-dest`. A no-policy control on the same host produced no
   beacon from either.
   THIS TREE ALREADY HELD THE CORRECT SPELLING ONE ZONE OVER, WHICH IS THE CHEAPEST CHECK OF ALL: the
   trusted zone strips a page address the same way in `extension/offscreen-brain.js`, as `base.hash = ""`,
   under a comment reading "empty string ⇒ fragment null ⇒ no trailing `#` in href" — and it is right,
   because URL §6.1 "URL class" gives that setter a first step of "If the given value is the empty string,
   then set this's URL's fragment to null and return". The JS half asked the URL API and got null; the C
   half wrote the field directly and got the empty string. One project, one question, two answers.
   THE TELL FOR THE NEXT READER, WHO WILL REACH FOR THE OTHER REPAIR: passing the exclude-fragment flag at
   step 5 gets the same bytes and cites nothing. This spelling is the one Referrer Policy §8.4 writes down. */
static char *csp_strip_url_for_reports(const UrlRecord *url)
{
    UrlRecord stripped;
    char *out;

    DCHECK(url != NULL && url->scheme != NULL,
           "CSP §5.4 was run over a URL with no scheme — §4.1's URL record gives every URL one, so a record "
           "without it is zero-filled rather than parsed");
    /* STEP 1 — "if url's scheme is not an HTTP(S) scheme, then return url's scheme." */
    if (!url_scheme_is_http_s(url->scheme)) {
        out = malloc(strlen(url->scheme) + 1);
        if (out) strcpy(out, url->scheme);
        return out;
    }
    /* STEPS 2-4 mutate the URL, so they run over a COPY: the record belongs to the caller's request or
       document and a report is not allowed to edit either. */
    url_record_init(&stripped);
    if (!url_record_copy(&stripped, url)) {
        url_record_free(&stripped);
        return NULL;
    }
    free(stripped.fragment);                    /* STEP 2 — §5.4 says "the empty string"; see the banner */
    stripped.fragment = NULL;
    free(stripped.username);                    /* STEP 3 — "set url's username to the empty string" */
    stripped.username = malloc(1);
    if (stripped.username) stripped.username[0] = 0;
    free(stripped.password);                    /* STEP 4 — "set url's password to the empty string" */
    stripped.password = malloc(1);
    if (stripped.password) stripped.password[0] = 0;
    /* STEP 5 — "return the result of executing the URL serializer on url", with no exclude-fragment flag.
       THE GUARD NO LONGER NAMES THE FRAGMENT, AND MUST NOT: it is an allocation-failure test, and step 2 now
       stores a NULL there ON PURPOSE. Leaving `stripped.fragment` in this conjunction would read the step's
       own correct answer as a failed `malloc` and return NULL, which the caller turns into a fatal `CHECK` —
       so the two lines are one change and splitting them aborts every violation this component reports. */
    out = (stripped.username && stripped.password)
              ? url_serialize(&stripped, /*exclude_fragment*/ false)
              : NULL;
    url_record_free(&stripped);
    return out;
}

/* §5.4 over a SERIALIZED address, which is what every caller here holds — a document's url and a request's
   url are both stored serialized in this engine, so the parse is here rather than at each of them.
   An address this engine NAVIGATED to or BUILT a request from is one its own parser already accepted, so a
   parse that fails here is not a page's odd input; it is a caller handing over something that was never a
   URL. It answers the EMPTY STRING rather than aborting because the operand reaching the failing arm would
   be a string no algorithm in this file can name, and §5.5's attributes are USVStrings with no absent value.
   Returns an OWNED string, never NULL. */
static char *csp_strip_serialized_url(const char *url)
{
    UrlRecord rec;
    char *out = NULL;

    url_record_init(&rec);
    if (url && url_parse(&rec, url, strlen(url), NULL))
        out = csp_strip_url_for_reports(&rec);
    url_record_free(&rec);
    if (!out) {
        out = malloc(1);
        CHECK(out != NULL, "CSP §5.4's result could not be allocated");
        out[0] = 0;
    }
    return out;
}

/* ---- CSP §5.2 "Obtain the blockedURI of a violation's resource" ------------------------------------------- */

/* §2.4's RESOURCE STRINGS, in the enum's own order. A C initializer list SHORTER than its enum is legal and
   zero-fills what it does not name, so the pairing is ASSERTED below rather than claimed here: the two are
   one list written twice and the assert is what makes that true by construction instead of by convention. */
static const char *const CSP_RESOURCE_STRING[] = {
    NULL,                     /* CSP_RESOURCE_URL — the URL arm names no string; §5.2 step 2 runs §5.4 */
    "inline",
    "eval",
    "wasm-eval",
    "trusted-types-policy",
    "trusted-types-sink",
};

/* "Given a violation's resource resource, this algorithm returns a string, to be used as the blocked URI
   field for violation reports." Returns an OWNED string, never NULL.
   STEP 1 IS THE SPEC'S OWN ASSERT — "Assert: resource is a URL or a string" — and it is a DCHECK here rather
   than a refusal because the operand is an enum THIS CODEBASE computes at the site that decided the
   violation. No byte of a policy and no byte of a page reaches it. */
static char *csp_violation_blocked_uri(CspResourceKind kind, const char *resource_url)
{
    char *out;

    DCHECK((int)kind >= (int)CSP_RESOURCE_URL && (int)kind <= (int)CSP_RESOURCE_TRUSTED_TYPES_SINK,
           "CSP §5.2's step 1 assert: a violation's resource was outside §2.4's domain. §2.4 lists the URL "
           "arm and five strings and gives no sixth, and the null it also lists is only allowed while the "
           "violation is being populated — which is a state no caller of this file can hold, since a "
           "violation here is created and reported in one act");
    DCHECK((int)(sizeof(CSP_RESOURCE_STRING) / sizeof(CSP_RESOURCE_STRING[0])) ==
               (int)CSP_RESOURCE_TRUSTED_TYPES_SINK + 1,
           "CSP §2.4's resource enumeration and the string table beside it disagree about how many arms §2.4 "
           "lists — a short initializer list zero-fills, so an arm added to the enum alone would name a NULL "
           "string and §5.2 step 3 would return it as a blockedURI");
    /* STEP 2 — "if resource is a URL, return the result of executing §5.4 … on resource." */
    if (kind == CSP_RESOURCE_URL) {
        DCHECK(resource_url != NULL,
               "CSP §5.2 was handed the URL arm of §2.4's resource with no URL. The two are one statement — "
               "a violation whose resource is a URL HAS that URL — so a NULL here is a caller that named the "
               "arm and never placed its operand");
        return csp_strip_serialized_url(resource_url);
    }
    /* STEP 3 — "return resource", which for every other arm is §2.4's own spelling of it. */
    DCHECK(CSP_RESOURCE_STRING[kind] != NULL,
           "§2.4's resource table names no string for an arm that is not the URL one — only CSP_RESOURCE_URL "
           "carries a NULL there, and §5.2 step 2 has already answered for it");
    out = malloc(strlen(CSP_RESOURCE_STRING[kind]) + 1);
    CHECK(out != NULL, "CSP §5.2's result could not be allocated");
    strcpy(out, CSP_RESOURCE_STRING[kind]);
    return out;
}

/* ---- CSP §2.4's SAMPLE ------------------------------------------------------------------------------------ */

/* §4.2.3's and §4.4.1's identical sample step — "if directive's value contains the expression 'report-sample'
   then set violation's sample to the substring of source containing its FIRST 40 CHARACTERS".
   FORTY CHARACTERS AND NOT FORTY BYTES, which is why this walks rather than calling memcpy: `source` is the
   page's own content, so forty bytes of it cuts a multi-byte code point in half and puts an ill-formed
   sequence into a `DOMString` attribute. The walk counts UTF-8 lead bytes — a byte outside the 0x80..0xBF
   continuation range starts a code point — which is the same reading Encoding's decoder makes and needs no
   decode here, because the answer wanted is a BYTE LENGTH and not a string.
   `out` receives the byte length; the returned pointer is INTO `source` and is not NUL-terminated. */
#define CSP_SAMPLE_CHARS 40

static size_t csp_sample_bytes(const char *source, size_t source_len)
{
    size_t i = 0, chars = 0;

    while (i < source_len && chars < CSP_SAMPLE_CHARS) {
        i++;
        while (i < source_len && ((unsigned char)source[i] & 0xC0) == 0x80) i++;
        chars++;
    }
    return i;
}

/* ---- CSP §5.5 "Report a violation" ------------------------------------------------------------------------ */

/* §5.5 STEPS 3.1 THROUGH 3.3 — which object the event is dispatched at. Returns an OWNED value.
   STEP 3.1 is "if target is not null, and global is a Window, and target's SHADOW-INCLUDING ROOT is not
   global's associated Document, set target to null", and its own note says what that buys: "This ensures that
   we fire events only at elements connected to violation's policy's Document. If a violation is caused by an
   element which isn't connected to that document, we'll fire the event at the document rather than the
   element in order to ensure that the violation is visible to the document's listeners."
   IT IS THE SHADOW-INCLUDING ROOT AND NOT THE NODE DOCUMENT, and the two differ for exactly the population
   the step is about: an element inside a shadow tree has this document as its NODE DOCUMENT and its shadow
   root as its ROOT, so a node-document test would target an element the step says to skip. A detached
   element's root is itself, which is neither.
   STEPS 3.2 AND 3.3 collapse into one line here because this engine's global IS a Window and §5.5's own two
   steps are "if target is null, set target to violation's global object" and "if target is a Window, set
   target to target's associated Document". */
static JSValue csp_violation_target(JSContext *ctx, const lxb_dom_element_t *element)
{
    JSValueConst doc = document_object(ctx);

    if (element) {
        lxb_dom_node_t *n = lxb_dom_interface_node((lxb_dom_element_t *)element);

        if (shadow_root_shadow_including_root(n) == node_of(doc))
            return node_wrap(ctx, n);   /* OWNED */
    }
    return JS_DupValue(ctx, doc);
}

/* §5.5's own body, for a violation whose every field is already in hand. `resource_url` is the URL arm's
   operand and `sample`/`sample_len` are §2.4's sample, already cut.
   THE DISPOSITION IS "enforce" AND IS NOT READ OFF THE POLICY, because a policy in this build has no other
   value to be read: core/frame/csp_directive_list.h records that a container is built from
   `Content-Security-Policy` and from CSP §3.3's `<meta>` and never from `Content-Security-Policy-Report-Only`,
   which this build is never handed. §2.2 gives a policy a disposition of "enforce" or "report" and this one
   parses only the first, which is also why §4.1.2's "if policy's disposition is report, skip to the next
   policy" is vacuous here rather than skipped.
     NAMED RESIDUAL — WHAT IS NOT COVERED: a report-only policy, whose violations a browser reports without
     blocking. WHAT THE NEXT DIFF BUILDS: §2.2.2's second extraction — "for each token returned by extracting
     header list values given `Content-Security-Policy-Report-Only` … let policy be the result of parsing
     token with a source of header and a disposition of report" — a disposition field on CspPolicy, and the
     two `if policy's disposition is …` arms §4.1.2 and §4.2.3 already spell.
     HOW ITS ABSENCE WOULD SHOW: a document served BOTH headers fires one event where a browser fires two,
     and the one it fires reads `disposition: "enforce"` with no `"report"` event beside it. */
static void csp_fire_violation(JSContext *ctx, const CspPolicy *policy, const char *effective_directive,
                               CspResourceKind kind, const char *resource_url,
                               const char *sample, size_t sample_len, const lxb_dom_element_t *element)
{
    SecurityPolicyViolationEventFields f;
    JSValue target, ev;
    char *document_uri, *blocked_uri, *original_policy, *sample_z;

    /* §5.5 STEP 4's OWN CONDITION, ASKED BEFORE ANYTHING IS COMPOSED — "if target implements EventTarget,
       fire an event named securitypolicyviolation …". Steps 3.2 and 3.3 resolve a null target to the global's
       associated Document, and a realm that has no active document resolves to nothing that implements
       EventTarget. Asking here rather than after the mint is what stops a composed event having no owner.
       IT IS NOT A DEFENSIVE `if`: it is the step, and deleting the thing it selects against would leave the
       question still owed — §5.5 asks it of every violation, including the ones whose target IS a Document. */
    target = csp_violation_target(ctx, element);
    if (!JS_IsObject(target)) {
        JS_FreeValue(ctx, target);
        return;
    }
    /* §2.4: "each violation has a url, which is its global object's url". */
    document_uri = csp_strip_serialized_url(document_url(ctx));
    blocked_uri = csp_violation_blocked_uri(kind, resource_url);
    original_policy = csp_policy_serialize(policy);
    CHECK(original_policy != NULL,
          "CSP §5.5's originalPolicy could not be allocated — an allocation failure while composing a "
          "violation, and a flow that continued would report a refusal the page cannot attribute to a rule");
    sample_z = malloc(sample_len + 1);
    CHECK(sample_z != NULL, "CSP §2.4's sample could not be allocated");
    if (sample_len) memcpy(sample_z, sample, sample_len);
    sample_z[sample_len] = 0;

    f.document_uri = document_uri;
    /* §2.4.1: "if global is a Window object, set violation's referrer to global's document's referrer".
         NAMED RESIDUAL — WHAT IS NOT COVERED: the referrer, which reads as the empty string here.
         `document.referrer` IS NOT THE THING TO REACH FOR and that is why this is a residual rather than a
         line: core/dom/document_metadata.c answers it with a CONCOLIC value rather than a concrete string,
         deliberately, so that a referrer-gated path stays reachable — and a concolic has no concrete bytes
         to put in a `USVString` attribute. WHAT THE NEXT DIFF BUILDS: a CONCRETE referrer stored on the
         Document by the navigation that created it, beside its url, so that this line and the concolic
         member are two readings of one stored fact rather than one of them inventing a value. HOW ITS
         ABSENCE WOULD SHOW: a handler reading `e.referrer` on a document reached by a link gets "" where a
         browser gives the referring address. */
    f.referrer = "";
    f.blocked_uri = blocked_uri;
    f.effective_directive = effective_directive;
    f.original_policy = original_policy;
    /* §2.4.1: "if the user agent is currently executing script and CAN EXTRACT a source file's URL, line
       number and column number from the global, set violation's source file, line number and column number
       accordingly" — a conditional whose false arm leaves §2.4's initial values, which are null and zero.
         NAMED RESIDUAL — WHAT IS NOT COVERED: the source position. WHAT THE NEXT DIFF BUILDS: an accessor
         answering WHERE THE RUNNING FLOW CURRENTLY IS — a script url, a line and a column for the frame on
         top — and §2.4.1's own condition ("if the user agent is currently executing script AND CAN EXTRACT"
         …) asked of it, so a violation raised by a parse rather than by script keeps §2.4's initial values.
         core/events/report_exception.h's `report_exception_position` is NOT that accessor and naming it
         would send a reader to the wrong question: it takes an EXCEPTION and answers where that exception
         was thrown, which is a different fact from where execution stands. HOW ITS ABSENCE WOULD SHOW: a
         handler reading `e.sourceFile` for a violation an inline script caused gets "" and `e.lineNumber` 0,
         where a browser names the document and the line. */
    f.source_file = NULL;
    f.sample = sample_z;
    f.disposition = "enforce";
    /* §2.4: "each violation has a status, which is a non-negative integer representing the HTTP status code
       of the resource for which the global object was instantiated" — which §2.4.1 itself flags as
       unspecified: "how exactly do we get the status code? we don't actually store it anywhere."
         NAMED RESIDUAL — WHAT IS NOT COVERED: the status. WHAT THE NEXT DIFF BUILDS: the status of the
         response §7.4.5 created this Document from, carried on the document record beside its url. HOW ITS
         ABSENCE WOULD SHOW: a handler reading `e.statusCode` gets 0 for a document a server answered 200
         for. §5.1's dictionary gives the member `= 0`, so this is the IDL's own default and not an invented
         number. */
    f.status_code = 0;
    f.line_number = 0;
    f.column_number = 0;

    ev = security_policy_violation_event_new_to_fire(ctx, &f);
    free(document_uri);
    free(blocked_uri);
    free(original_policy);
    free(sample_z);
    if (JS_IsException(ev)) {
        /* An allocation failure inside the mint. The BLOCK has already happened and is the answer the page
           gets; what is lost is its observable, and there is no arm of §5.5 that reports a violation about
           failing to report a violation. */
        JS_FreeValue(ctx, ev);
        JS_FreeValue(ctx, target);
        return;
    }
    /* §5.5's "QUEUE A TASK to run the following steps", whose own note says why it is queued: "to ensure that
       the event targeting and dispatch happens after JavaScript completes execution of the task responsible
       for a given violation (which might manipulate the DOM)". core/events/event_target.h names
       `event_target_fire` as exactly that reach and `event_target_fire_run` as the synchronous one, and this
       is the queued caller its contract is written for. `ev` is CONSUMED. */
    event_target_fire(ctx, target, ev, JS_UNDEFINED);
    JS_FreeValue(ctx, target);
}

void csp_report_violation_for_request(CspReporter r, const CspPolicy *policy, const char *destination,
                                      const char *request_url)
{
    const char *directive;

    DCHECK(r.stated,
           "CSP §4.1.2 step 3.3.1 was reached with a reporter nobody built — a zero-filled struct is neither "
           "of csp_violation.h's two claims, and a walk that took it would decide a request and report "
           "nothing with no site anywhere saying so. Build one with csp_reporter or csp_reporter_none");
    DCHECK(policy != NULL && request_url != NULL,
           "CSP §2.4.2 was reached with no policy or no request URL — it creates a violation FOR a request "
           "and a policy, so neither operand has an absent form");
    if (!r.ctx) return;   /* csp_reporter_none: the decision was asked for outside any global object */
    /* §2.4.2 STEP 1 — "let directive be the result of executing §6.8.1 get the effective directive for
       request on request". ASKED HERE rather than taken from the walk that just blocked, because §2.4.2 asks
       it itself and two answers to one question are free to disagree; §6.8.1 is a pure function of the
       destination, so asking twice costs a table lookup and cannot drift. */
    directive = csp_effective_directive_for_request(destination);
    DCHECK(directive != NULL,
           "CSP §2.4.2 was reached for a request §6.8.1 governs with no fetch directive. §4.1.2 runs §6.7.2.1 "
           "over every policy and can only reach step 3.3.1 for a request some directive governs, so a null "
           "here is a violation reported for a request no policy could have blocked — §6.8.1's `report` row "
           "is the one destination that answers null and it is refused before the walk starts");
    /* §2.4.2 STEP 2 runs §2.4.1 over the request's client's global object, and STEP 3 sets the violation's
       resource to the request's url — the URL arm of §2.4's resource. */
    csp_fire_violation(r.ctx, policy, directive, CSP_RESOURCE_URL, request_url, "", 0, NULL);
}

void csp_report_violation_for_global(CspReporter r, const CspPolicy *policy, const char *effective_directive,
                                     CspResourceKind resource, const char *source, size_t source_len,
                                     const lxb_dom_element_t *element)
{
    size_t sample_len = 0;

    DCHECK(r.stated,
           "CSP §5.5 was reached with a reporter nobody built — a zero-filled struct is neither of "
           "csp_violation.h's two claims. Build one with csp_reporter or csp_reporter_none");
    DCHECK(policy != NULL && effective_directive != NULL,
           "CSP §2.4.1 was reached with no policy or no directive — it creates a violation FOR a global, a "
           "policy and a directive, so neither operand has an absent form");
    DCHECK(resource != CSP_RESOURCE_URL,
           "CSP §2.4.1 was reached with the URL arm of §2.4's resource. §2.4.1 sets the resource to NULL and "
           "leaves it to its caller; the only caller that makes it a URL is §2.4.2, which has a request to "
           "take one from and is csp_report_violation_for_request");
    if (!r.ctx) return;   /* csp_reporter_none: the decision was asked for outside any global object */
    /* §4.2.3's and §4.4.1's SAMPLE STEP — "if directive's value contains the expression 'report-sample'".
       THE DIRECTIVE IS RESOLVED HERE AND NOT PASSED, because it is the one this violation's own effective
       directive names and §6.8.4 answers for at most one directive of a policy; a caller passing a directive
       beside a directive NAME would be stating one fact twice.
       THE LOOKUP IS INSIDE THE CONTENT GUARD AND THAT IS LOAD-BEARING RATHER THAN AN OPTIMISATION, because
       §6.8.3's fallback list is a FETCH DIRECTIVE list and a DOCUMENT directive has none: §6.8.3's own
       trailing "return «»" is what `base-uri` gets, so csp_policy_governing_directive answers NULL for it —
       CORRECTLY, and for a name §6.8.4 is not about. §6.3.1.1 is the caller that hands over a `base-uri`
       violation, it carries NO content, and §2.4 gives a sample-less violation the empty string, so there was
       never a directive to look up for it. Hoisting this line out of the guard aborts every `<base href>`
       refusal.
       WHERE THERE IS CONTENT THE DIRECTIVE MUST EXIST, and that is what the assert says: the caller found it
       by running §6.8.4 over this same policy and this same effective directive one step earlier, which is
       how it knows the content was refused at all. */
    if (source && source_len) {
        const CspDirective *d = csp_policy_governing_directive(policy, effective_directive);

        DCHECK(d != NULL,
               "CSP §5.5 was handed the CONTENT of a violation by a policy that carries no directive "
               "governing that violation's own effective directive. A caller with content found the "
               "refusing directive by running §6.8.4 over this policy and this name one step earlier, so a "
               "NULL here means the two walks were given different names");
        if (csp_source_list_contains(d, "'report-sample'"))
            sample_len = csp_sample_bytes(source, source_len);
    }
    csp_fire_violation(r.ctx, policy, effective_directive, resource, NULL,
                       sample_len ? source : "", sample_len, element);
}
