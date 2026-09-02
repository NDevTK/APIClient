/* CSP §2.2, §2.2.1, §2.3 AND §6.8 — THE POLICY / DIRECTIVE-SET MODEL. See csp_directive_list.c.
 *
 * WHY THIS FILE EXISTS. A policy container used to hold its CSP as `csp_text`, a SERIALIZED CSP list, and
 * every question anyone had of it was answered by a fresh ad-hoc scan of that string: policy_container.c
 * split it one way to find four script directives, and core/html/trusted_types.c split it a second way, with
 * its own whitespace predicate and its own token compare, to find a fifth. Two readings of one grammar is one
 * reading too many — §2.2.1 has a DUPLICATE rule and an ASCII rule and a strip rule, and a scanner written per
 * question implements whichever of them its author remembered. There is now ONE parse, and a question is a
 * LOOKUP in its result.
 *
 * WHAT IS HERE AND WHAT IS NEXT DOOR. This file knows what a directive IS — a name and a value that is a set
 * of strings (§2.3) — and nothing whatever about what those strings MEAN. The meaning of a value is the source
 * list, and that is csp_source_list.h: the two are separate because they are separate in the standard and
 * because most directives' values are not source lists at all (`sandbox` is a sandboxing directive, `report-to`
 * is a token, `require-trusted-types-for` is a list of sink groups). §6.8's algorithms are here rather than
 * there for the same test: every one of them reads directive NAMES and a policy's directive set, and not one
 * of them looks inside a value.
 *
 * THE PARSE BORROWS ITS TEXT. Every name and every value token is a SLICE of the serialized bytes the caller
 * handed in — nothing is copied — which is what keeps a policy container "a flat parse over one owned string"
 * that serializes to that string and reconstitutes by parsing it again (policy_container.h). The caller must
 * outlive the list; csp_list_free frees the ARRAYS this parse allocated and never the text.
 *
 * THE NAME IS NOT LOWERCASED IN PLACE, AND THAT IS A DELIBERATE DEPARTURE FROM §2.2.1's LETTER. The standard
 * says "Set directive name to be the result of running ASCII lowercase on directive name"; here the slice is kept
 * as written and every comparison against it is ASCII case-insensitive, which is the same relation. It is done
 * this way because the bytes are the caller's and are also what a violation report has to quote back — a
 * report that renames the page's own `script-SRC` is a report about a policy the server did not send. The
 * equivalence holds only while every lookup key is already lowercase, so csp_policy_directive ASSERTS that. */
#ifndef ENGINE_HOST_BROWSER_CORE_FRAME_CSP_DIRECTIVE_LIST_H
#define ENGINE_HOST_BROWSER_CORE_FRAME_CSP_DIRECTIVE_LIST_H
#include <stdbool.h>
#include <stddef.h>

#include "core/url/origin.h"

/* A SLICE of the serialized policy: a directive name, or one of §2.3's value strings. Never NUL-terminated —
   the bytes belong to the caller's buffer and a token ends where the next one begins. */
typedef struct {
    const char *p;
    size_t      n;
} CspToken;

/* §2.3's DIRECTIVE: "a name / value pair. The name is a non-empty string, and the value is a set of non-empty
   strings. The value MAY be empty." */
typedef struct {
    CspToken  name;
    /* THE RAW REMAINDER, leading and trailing ASCII whitespace stripped. §2.3's value is the SPLIT of it, and
       that is what a source list reads; this is kept beside it because two directives in this engine are
       defined by another standard's parse over the whole value rather than over its tokens — HTML §7.1.5's
       `sandbox` is one, and it distinguishes an ABSENT directive from one with an empty value. */
    CspToken  value_text;
    CspToken *value;      /* the ASCII-whitespace split of value_text, no empty strings (owned) */
    size_t    n_value;
} CspDirective;

/* §2.2's POLICY, reduced to its DIRECTIVE SET. A policy also has a disposition and a source; neither is here,
   because neither is yet DELIVERED to this engine: only `Content-Security-Policy` and CSP §3.3's `<meta>` are
   parsed, so every policy has disposition "enforce", and the source that would distinguish the two is a field
   that cannot survive §7.4's clone (which travels as TEXT), so it arrives with that transport rather than
   before it. core/dom/document.c asserts the one place the missing source would change an answer. */
typedef struct {
    CspDirective *directives;    /* in delivery order (owned) */
    size_t        n_directives;
} CspPolicy;

/* §2.2's CSP LIST. A list, not a policy: several policies arrive comma-delimited in one header or as several
   `<meta>` elements, and they are enforced INDEPENDENTLY — which is why the quantifier over this list differs
   per question and every caller has to state its own (content runs only if EVERY policy permits it; trusted
   types are required as soon as ANY policy requires them). */
typedef struct {
    CspPolicy *policies;
    size_t     n_policies;
    /* §2.2's SELF-ORIGIN: "an origin which is used when matching the 'self' keyword". It is a member of the
       LIST and not of a policy, and it CANNOT come from the serialized text — §2.2.2 states it from OUTSIDE
       the bytes ("return a CSP list whose policies is policies and self-origin is response's URL's origin"),
       which is why csp_list_parse takes it as an argument rather than deriving one.
       IT IS A RECORD AND NOT A URL, and §2.2's own note says why: it exists so that a document with an OPAQUE
       origin which INHERITED its policy still resolves `'self'` against the origin the policy came from. A
       document whose address is `about:blank` or `data:` has no such origin anywhere in its URL, so a
       self-origin carried as a URL would answer `'self'` with a fresh opaque origin in exactly the case the
       field was added for. §6.7.2.8 also READS its scheme, host and port, which only the record has.
       BORROWED — an origin lives for the agent (core/url/origin.h). NULL is a positive statement and not a
       hole: it says this list will never be asked to match a URL, which §7.1.5's CSP-derived sandboxing flags
       parse is (it reads one directive's raw value and no source list at all). §6.7.2.7 asserts it. */
    const Origin *self_origin;
} CspList;

/* §4.2.3's `type` and §4.2.4's "navigation" — the five strings the standard's inline checks are written in
   terms of. An enum rather than a string because §6.8.2 switches on it and a typo in a string would silently
   become §6.8.2's "return null", which is a check that governs nothing. */
typedef enum {
    CSP_INLINE_SCRIPT = 0,        /* "script" — an inline script block */
    CSP_INLINE_SCRIPT_ATTRIBUTE,  /* "script attribute" — an event handler content attribute */
    CSP_INLINE_NAVIGATION,        /* "navigation" — §4.2.4's javascript: URL check */
    CSP_INLINE_STYLE,             /* "style" */
    CSP_INLINE_STYLE_ATTRIBUTE,   /* "style attribute" */
} CspInlineType;

/* §2.2.1 "parse a serialized CSP", run over each comma-delimited policy of a serialized CSP LIST, and §2.2.2's
   final step, which is the only place the list's SELF-ORIGIN can come from.
   `out` must be zeroed; `serialized` is BORROWED and must outlive `out`. A policy whose directive set comes
   out EMPTY is not appended (§2.2.2), so an empty string, a stray comma and a policy of nothing but
   whitespace all produce a list of zero policies rather than a policy that says nothing.
   `self_origin` is §2.2.2's "response's URL's origin", stated by the caller because the bytes do not contain
   it and because WHOSE origin it is belongs to the operation: a Document created from a response takes its
   own, and one created by §7.4's clone takes the CREATOR's along with the text. NULL only for a list that
   will never match a URL — see the field. */
void csp_list_parse(CspList *out, const char *serialized, size_t len, const Origin *self_origin);

/* Frees exactly what csp_list_parse allocated — the directive and value arrays — and never the text. */
void csp_list_free(CspList *list);

/* §2.2.1 "parse a serialized CSP" OVER ONE SERIALIZED POLICY — the algorithm §2.2.2 runs per comma-delimited
   member above, and the one HTML §4.2.5.3 "Pragma directives" invokes BY NAME on a `<meta>` element's
   `content` attribute ("let policy be the result of executing Content Security Policy's parse a serialized
   Content Security Policy algorithm on the meta element's content attribute's value").
   IT IS ONE POLICY AND NOT A LIST, and the difference is not pedantic: §2.2 makes U+002C the LIST delimiter
   and §2.2.1 gives it no meaning at all, so a comma inside a `<meta>` policy is part of a directive value.
   A caller that reached for csp_list_parse would be reading a policy the standard does not split as two.
   `out` must be zeroed; `text` is BORROWED and must outlive `out` (this parse copies nothing — see the file
   header). A policy whose directive set comes out empty is a policy that says nothing, which §2.2.2's own
   step drops rather than appends; that judgement is the caller's here, because this entry parses ONE policy
   and has no list to append it to. */
void csp_policy_parse(CspPolicy *out, const char *text, size_t len);

/* Frees exactly what ONE csp_policy_parse allocated — the directive array and each directive's value array —
   and never the text. csp_list_free does this for every member of a list; a policy parsed on its own has no
   list to be freed through. */
void csp_policy_free(CspPolicy *policy);

/* HTML §4.2.5.3 "Pragma directives"' step 4 — "remove all occurrences of the report-uri, frame-ancestors, and
   sandbox directives from policy" — as the one operation it is: a directive is taken OUT of a policy's
   directive set. `name` must be ASCII lowercase (csp_policy_directive's rule, asserted the same way) and the
   match is ASCII case-insensitive, so the page's own spelling is what is compared and never rewritten.
   ALL OCCURRENCES, not the first: §2.2.1 step 3.4 already makes a name unique within a policy IT parsed, so
   this loops for a policy assembled any other way rather than resting on that parse's property. */
void csp_policy_remove_directive(CspPolicy *policy, const char *name);

/* §2.2's SERIALIZATION OF ONE POLICY — the inverse of csp_policy_parse, and the form a policy travels in
   through this build (policy_container.h: "a flat parse over one owned string", so a container serializes to
   its text and reconstitutes by parsing it again). Directives in delivery order, joined by "; "; a directive
   whose value is empty serializes as its bare name, which is what `upgrade-insecure-requests` is.
   NULL FOR A POLICY WITH NO DIRECTIVES, which is a positive statement and the same one §2.2.2 makes when it
   refuses to append such a policy: a policy that says nothing is not a policy that permits everything.
   The result is a NUL-terminated string the CALLER FREES. */
char *csp_policy_serialize(const CspPolicy *policy);

/* An ASCII case-insensitive compare of a token against a literal that MUST already be ASCII lowercase, which
   is asserted: CSP compares keywords, directive names and scheme parts this way throughout, and a literal with
   a capital in it would match nothing while looking like it should. */
bool csp_token_is(CspToken token, const char *ascii_lowercase);

/* THE PREDICATE THAT ASSERTION IS MADE OF, exported because §6.7.3.1's substring test in csp_source_list.c
   folds a haystack against a literal the same way and must assert the same thing about it. One reading of
   "this literal is already lowercase", for the same reason there is one reading of the grammar. */
bool csp_is_ascii_lowercase(const char *s);

/* "policy contains a directive whose name is `name`", the containment test §2.2.1, §4.4.1 and §6.8.4 all use.
   Returns the directive or NULL. `name` must be ASCII lowercase. */
const CspDirective *csp_policy_directive(const CspPolicy *policy, const char *name);

/* §6.8.1 "get the effective directive for request" — the fetch-directive name that controls a request, chosen
   by that request's DESTINATION.
   THE DESTINATION IS FETCH'S OWN STRING, not an enum of this file's invention. Fetch §2.2.5 defines a
   request's destination as the empty string or one of a fixed list of literals, §6.8.1 switches on exactly
   those literals, and an enum would be a second spelling of one vocabulary — with one live member today
   (`fetch()` and XMLHttpRequest both make a request whose destination is the EMPTY STRING, which is §6.8.1's
   first row and answers `connect-src`).
   §6.8.1 CAN RETURN NULL, for the "report" destination alone, and that null means the request is governed by
   no fetch directive at all. It is spelled as NULL here rather than folded into `connect-src`, because
   §6.8.1's own trailing default IS `connect-src` and collapsing the two would make a report upload subject to
   a directive the standard exempts it from. */
const char *csp_effective_directive_for_request(const char *destination);

/* §6.8.2 "get the effective directive for inline checks". Never returns NULL for the five types above — the
   standard's trailing "return null" covers types it does not define, which this enum does not have. */
const char *csp_effective_directive_for_inline_checks(CspInlineType type);

/* §6.8.3 "get fetch directive fallback list" — the effective directive itself followed by the directives it
   falls back to, most relevant first, NULL-terminated. A name the standard gives no list returns an EMPTY
   list (just the terminator), which is what §6.8.3's trailing "return «»" says; `script-src` is such a name,
   because it is only ever a FALLBACK and is never what §6.8.1 or §6.8.2 returns. */
const char *const *csp_fetch_directive_fallback_list(const char *directive_name);

/* §6.8.4 "should fetch directive execute", verbatim: walk the effective directive's fallback list and answer
   Yes only for the first entry that names THIS directive with no earlier entry present in the policy. This is
   the algorithm that makes `script-src` REPLACE `default-src` for scripts rather than add to it. */
bool csp_should_fetch_directive_execute(const char *effective_name, const char *directive_name,
                                        const CspPolicy *policy);

/* THE ONE DIRECTIVE OF `policy` THAT §6.8.4 SAYS EXECUTES for `effective_name`, or NULL when the policy
   carries none of them — which is a policy that says NOTHING about this check, and is not the same as one that
   forbids it (`img-src 'none'` alone blocks no handler).
   It exists because §4.2.3 asks every directive in turn and §6.8.4 answers Yes for at most one of them, so a
   caller that looped would be writing the same walk again to throw all but one answer away. The equivalence is
   ASSERTED against §6.8.4 itself rather than argued in a comment. */
const CspDirective *csp_policy_governing_directive(const CspPolicy *policy, const char *effective_name);

#endif
