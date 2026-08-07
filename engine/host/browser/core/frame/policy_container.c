/* THE POLICY CONTAINER — HTML §7.2.6, and CSP's source list to the depth this engine acts on.
 *
 * WHY IT EXISTS, AND WHY IT IS THE ANSWER TO "how does an about:blank child get a policy". A navigable created
 * with no URL — `window.open()` with no argument, `<iframe>` with no src — gets its initial `about:blank`
 * Document SYNCHRONOUSLY, and that Document has no response to take a policy from. HTML's answer is not a
 * special case: every Document has a POLICY CONTAINER (a CSP list, a referrer policy, embedder policy and
 * sandboxing flags), and §7.4's create-a-new-browsing-context says that when there is a creator, the new
 * document's policy container is a CLONE OF THE CREATOR'S. So the child inherits the parent's CSP by the
 * ordinary rule, not by an inheritance rule written for CSP.
 *
 * THAT IS ALSO WHY IT IS NOT IPC. A same-origin about:blank child is created in the creator's own agent: same
 * heap, same event loop, scripted synchronously — `iframe.contentWindow.document.body` has to answer on the
 * line after the append, and no message-passing scheme can do that. IPC enters only where the navigable is
 * hosted by ANOTHER agent (a cross-origin navigation, site isolation), and there HTML never requires
 * synchronous access: the cross-origin surface is postMessage and a fixed whitelist. So the rule this engine
 * needs is one instance per AGENT, not per document — see policy_container.h.
 *
 * IT IS PER-FLOW, for the reason the WindowProxy binding is. A flow that navigates the frame replaces its
 * policy container; a sibling that did not still has the old one, and a check against a container the engine
 * "last" installed answers for a world the flow is not in. So the record rides the COW delta.
 *
 * WHAT IS PARSED IS WHAT IS ACTED ON. CSP is a large specification and this parses the part the solver asks
 * about — whether an inline handler, an `eval`, or a `javascript:` URL would RUN — because §S says a firing
 * breakout in the model is not an exploit unless it survives the page's actual policy. A directive that is
 * parsed but not understood is not silently permitted: policy_allows crashes on a question this file cannot
 * answer, so the gap is a name rather than a wrong verdict. */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/frame/policy_container.h"

/* CSP §2.2's SOURCE LIST, reduced to the three answers a script-execution question turns on. The raw text is
   kept because the parse is partial and the text is what a later, fuller parse reads — and because a report is
   worth nothing without the policy it names. */
struct PolicyContainer {
    char *csp_text;        /* the serialized policy list, ';'-joined, or NULL for none (owned) */
    uint8_t has_script_src;   /* a script-src or default-src was present at all */
    uint8_t unsafe_inline;
    uint8_t unsafe_eval;
    uint8_t strict_dynamic;   /* CSP §6.1: with 'strict-dynamic', host sources are IGNORED for scripts */
    uint8_t nonce_required;   /* a nonce-source was present, so an inline script needs the right nonce */
    uint8_t unknown_source;   /* a source expression this parser does not model — see policy_allows */
    char *referrer_policy;    /* §7.2.6's referrer policy (owned) */
};

/* Is `tok` the CSP keyword `name`? Keywords are quoted in the grammar and matched ASCII-case-insensitively. */
static bool kw(const char *tok, size_t n, const char *name)
{
    size_t k = strlen(name);
    size_t i;
    if (n != k) return false;
    for (i = 0; i < n; i++) {
        char a = tok[i], b = name[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

/* CSP §2's serialized policy: directives separated by ';', each a name and space-separated values. Only the
   script-governing ones are read; everything else is kept in the text and answered for by nobody. */
static void parse_policy(PolicyContainer *p, const char *text, size_t len)
{
    size_t i = 0;

    while (i < len) {
        size_t start, end, ni;
        const char *name;
        size_t name_len;
        bool is_script;

        while (i < len && (text[i] == ';' || text[i] == ' ' || text[i] == '\t' || text[i] == '\n')) i++;
        start = i;
        while (i < len && text[i] != ';') i++;
        end = i;
        while (end > start && (text[end - 1] == ' ' || text[end - 1] == '\t')) end--;
        if (end == start) continue;

        ni = start;
        while (ni < end && text[ni] != ' ' && text[ni] != '\t') ni++;
        name = text + start;
        name_len = ni - start;
        /* CSP §6.1: `script-src` governs scripts, and `default-src` is the fallback when it is absent. A
           policy with neither says nothing about scripts at all, which is not the same as forbidding them. */
        is_script = kw(name, name_len, "script-src") || kw(name, name_len, "script-src-elem") ||
                    kw(name, name_len, "script-src-attr") ||
                    (kw(name, name_len, "default-src") && !p->has_script_src);
        if (!is_script) continue;
        p->has_script_src = 1;

        while (ni < end) {
            size_t vs, ve;
            while (ni < end && (text[ni] == ' ' || text[ni] == '\t')) ni++;
            vs = ni;
            while (ni < end && text[ni] != ' ' && text[ni] != '\t') ni++;
            ve = ni;
            if (ve == vs) break;
            if (kw(text + vs, ve - vs, "'unsafe-inline'")) p->unsafe_inline = 1;
            else if (kw(text + vs, ve - vs, "'unsafe-eval'")) p->unsafe_eval = 1;
            else if (kw(text + vs, ve - vs, "'strict-dynamic'")) p->strict_dynamic = 1;
            else if (kw(text + vs, ve - vs, "'none'")) { /* nothing may run; no flag needed */ }
            else if (kw(text + vs, ve - vs, "'self'")) { /* a host source; irrelevant to INLINE execution */ }
            else if (ve - vs > 7 && !strncmp(text + vs, "'nonce-", 7)) p->nonce_required = 1;
            else if (ve - vs > 8 && (!strncmp(text + vs, "'sha256-", 8) || !strncmp(text + vs, "'sha384-", 8) ||
                                     !strncmp(text + vs, "'sha512-", 8))) {
                /* A HASH SOURCE permits an inline script whose body hashes to it. Computing that is a real
                   capability this file does not have, so it is recorded as unknown rather than assumed to
                   permit or forbid — see policy_allows. */
                p->unknown_source = 1;
            } else if (text[vs] == '\'') {
                p->unknown_source = 1;   /* a keyword source this parser does not model */
            }
            /* A HOST SOURCE (https://x, *.example, data:) does not permit INLINE execution under any
               circumstances, so it needs no flag: CSP §6.1 requires 'unsafe-inline', a nonce or a hash. */
        }
    }
}

PolicyContainer *policy_container_new(const char *csp_text, const char *referrer_policy)
{
    PolicyContainer *p = calloc(1, sizeof *p);
    CHECK(p != NULL, "policy container: OOM");
    if (csp_text && *csp_text) {
        p->csp_text = strdup(csp_text);
        CHECK(p->csp_text != NULL, "policy container: OOM copying a policy");
        parse_policy(p, p->csp_text, strlen(p->csp_text));
    }
    if (referrer_policy) {
        p->referrer_policy = strdup(referrer_policy);
        CHECK(p->referrer_policy != NULL, "policy container: OOM copying a referrer policy");
    }
    return p;
}

PolicyContainer *policy_container_clone(const PolicyContainer *src)
{
    /* §7.2.6's "clone a policy container", and §7.4's answer to where an about:blank child's policy comes
       from. It is a DEEP copy: the child's policy is its own from the moment it exists, so a later navigation
       of the parent does not reach back and change what the child is allowed to do. */
    DCHECK(src != NULL, "a policy container was cloned from nothing — every document has one, including the "
                        "initial about:blank, which is the whole reason this operation exists");
    return policy_container_new(src->csp_text, src->referrer_policy);
}

void policy_container_free(PolicyContainer *p)
{
    if (!p) return;
    free(p->csp_text);
    free(p->referrer_policy);
    free(p);
}

const char *policy_container_csp(const PolicyContainer *p) { return p ? p->csp_text : NULL; }

bool policy_allows(const PolicyContainer *p, PolicyScriptKind kind)
{
    /* A document with no policy allows everything, which is the overwhelmingly common case and is what "no
       Content-Security-Policy header" means. */
    if (!p || !p->has_script_src)
        return true;
    /* AND THE ONE THIS FILE CANNOT ANSWER CRASHES. A hash source permits an inline script whose body hashes to
       it, and computing that hash is a capability this parser does not have; a keyword it does not model could
       permit or forbid. Answering "allowed" would report an exploit that a real browser blocks, and answering
       "blocked" would suppress a real one — both are worse than naming the missing piece here. */
    DCHECK(!p->unknown_source,
           "this policy carries a source expression the parser does not model (a hash source, or a keyword "
           "outside 'unsafe-inline'/'unsafe-eval'/'strict-dynamic'/'self'/'none'/'nonce-'), and the verdict "
           "for an inline script under it cannot be decided without it — build that source expression");
    switch (kind) {
    case POLICY_INLINE_SCRIPT:
    case POLICY_INLINE_HANDLER:
    case POLICY_JAVASCRIPT_URL:
        /* CSP §6.1: a nonce or hash source makes 'unsafe-inline' be IGNORED — for the WHOLE DIRECTIVE, not
           only for script elements, which is the rule that makes adding a nonce to a legacy policy actually
           tighten it rather than widen it. I wrote the narrower version first and the C test caught it: a
           handler under  came out ALLOWED, which would have reported
           an exploit that a real browser blocks.
           A nonce does not rescue a handler or a javascript: URL either — neither can carry one — so once
           'unsafe-inline' is ignored there is nothing left to permit them. 'strict-dynamic' does not permit
           inline at all; it only stops host sources from mattering. */
        return p->unsafe_inline && !p->nonce_required;
    default:
        DCHECK(kind == POLICY_EVAL, "a policy was asked about a script kind this component does not have");
        return p->unsafe_eval != 0;
    }
}
