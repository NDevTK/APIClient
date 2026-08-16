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
 * says "set directive name to the result of running ASCII lowercase on directive name"; here the slice is kept
 * as written and every comparison against it is ASCII case-insensitive, which is the same relation. It is done
 * this way because the bytes are the caller's and are also what a violation report has to quote back — a
 * report that renames the page's own `script-SRC` is a report about a policy the server did not send. The
 * equivalence holds only while every lookup key is already lowercase, so csp_policy_directive ASSERTS that. */
#ifndef ENGINE_HOST_BROWSER_CORE_FRAME_CSP_DIRECTIVE_LIST_H
#define ENGINE_HOST_BROWSER_CORE_FRAME_CSP_DIRECTIVE_LIST_H
#include <stdbool.h>
#include <stddef.h>

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
   types are required as soon as ANY policy requires them).
   §2.2 also gives a CSP list a SELF-ORIGIN, for matching the 'self' keyword. It is absent here for the same
   reason the disposition is: the only questions asked of this model so far are about INLINE content and about
   `eval`, and neither ever compares a URL. It arrives with §6.7.2's URL matching, which is what needs it. */
typedef struct {
    CspPolicy *policies;
    size_t     n_policies;
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

/* §2.2.1 "parse a serialized CSP", run over each comma-delimited policy of a serialized CSP LIST.
   `out` must be zeroed; `serialized` is BORROWED and must outlive `out`. A policy whose directive set comes
   out EMPTY is not appended (§2.2.2), so an empty string, a stray comma and a policy of nothing but
   whitespace all produce a list of zero policies rather than a policy that says nothing. */
void csp_list_parse(CspList *out, const char *serialized, size_t len);

/* Frees exactly what csp_list_parse allocated — the directive and value arrays — and never the text. */
void csp_list_free(CspList *list);

/* An ASCII case-insensitive compare of a token against a literal that MUST already be ASCII lowercase, which
   is asserted: CSP compares keywords, directive names and scheme parts this way throughout, and a literal with
   a capital in it would match nothing while looking like it should. */
bool csp_token_is(CspToken token, const char *ascii_lowercase);

/* "policy contains a directive whose name is `name`", the containment test §2.2.1, §4.4.1 and §6.8.4 all use.
   Returns the directive or NULL. `name` must be ASCII lowercase. */
const CspDirective *csp_policy_directive(const CspPolicy *policy, const char *name);

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
