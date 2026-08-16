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
 * AND THE CLONE CROSSES INSTANCES. One WASM instance is one DOCUMENT regardless of origin, so the creator and
 * the child it clones from are not in the same heap. `iframe.contentWindow.document.body` still has to answer
 * on the line after the append — the flow SUSPENDS at that boundary and resumes with the value, exactly as it
 * suspends at an await, which is what makes a synchronous-looking cross-document read possible without
 * co-locating the documents. Nothing here may be shaped to avoid that transport; see policy_container.h.
 *
 * IT IS NOT YET PER-FLOW, and the place that will make it so is asserted rather than described. A flow that
 * NAVIGATES a frame replaces its policy container while a sibling that did not still holds the old one, so the
 * binding is per-flow state for the same reason the WindowProxy binding is. Navigation is not built, so today
 * a document's container is installed once on the baseline before any flow runs — and document_install CRASHES
 * on a second install rather than carrying a comment promising to handle it.
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
#include "core/frame/sandboxing.h"

/* CSP §2.2's SOURCE LIST for ONE directive, reduced to the answers a script-execution question turns on. */
typedef struct {
    uint8_t present;          /* the directive appeared in this policy at all */
    uint8_t unsafe_inline;
    uint8_t unsafe_eval;
    uint8_t strict_dynamic;   /* CSP §6.1: with 'strict-dynamic', host sources are IGNORED for scripts */
    uint8_t nonce_required;   /* a nonce-source was present, so an inline script needs the right nonce */
    uint8_t unknown_source;   /* a source expression this parser does not model — see policy_allows */
} Directive;

/* ONE POLICY: the script-governing directives, kept SEPARATELY rather than merged into one flag set. Merging
   was the first version and it is wrong in a way the C test caught: `default-src 'unsafe-inline'; script-src
   'self'` would have come out permitting an inline handler, because the flags were OR'd. CSP §6.1 does not
   combine them — a present `script-src` REPLACES `default-src` for scripts entirely, so which directive is
   read has to be decided before any source expression is looked at. See `governing`. */
typedef struct {
    Directive default_src;
    Directive script_src;
    Directive script_src_elem;
    Directive script_src_attr;
    /* CSP's `sandbox` directive, which is NOT a source list and shares nothing with the four above: its value
       is a SANDBOXING DIRECTIVE (§7.1.5), parsed by that standard's own algorithm, and what it produces is a
       flag set rather than a permission. It is kept here rather than folded into a flag on the container
       because §7.1.5's CSP-derived sandboxing flags reads the LAST policy in the list that carries one — a
       per-policy question, which a merged bit could not answer. */
    uint8_t      sandbox_present;
    SandboxFlags sandbox_flags;
} Policy;

/* THE CONTAINER HOLDS A LIST, not a policy — CSP §2.2. Several policies arrive as a comma-delimited header, or
   as several `<meta>` elements, and they are enforced INDEPENDENTLY: content must be allowed by EVERY policy.
   Flattening them into one policy was the other half of the same bug — it makes a duplicate `script-src` in
   the flattened text a duplicate WITHIN one policy, which §2.2.1 says to IGNORE, so the narrowing second
   policy vanished and a blocked handler was reported as live.
   The raw text is kept because the parse is partial and the text is what a later, fuller parse reads — and
   because a report is worth nothing without the policy it names. */
struct PolicyContainer {
    char *csp_text;           /* the serialized CSP list, comma-delimited, or NULL for none (owned) */
    Policy *policies;         /* one per comma-delimited policy (owned) */
    size_t n_policies;
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

/* CSP §2.2's source list: space-separated source expressions, read for the three that decide whether inline
   script or eval may run. */
static void parse_source_list(Directive *d, const char *text, size_t i, size_t end)
{
    while (i < end) {
        size_t vs, ve;
        while (i < end && (text[i] == ' ' || text[i] == '\t')) i++;
        vs = i;
        while (i < end && text[i] != ' ' && text[i] != '\t') i++;
        ve = i;
        if (ve == vs) break;
        if (kw(text + vs, ve - vs, "'unsafe-inline'")) d->unsafe_inline = 1;
        else if (kw(text + vs, ve - vs, "'unsafe-eval'")) d->unsafe_eval = 1;
        else if (kw(text + vs, ve - vs, "'strict-dynamic'")) d->strict_dynamic = 1;
        else if (kw(text + vs, ve - vs, "'none'")) { /* nothing may run; no flag needed */ }
        else if (kw(text + vs, ve - vs, "'self'")) { /* a host source; irrelevant to INLINE execution */ }
        else if (ve - vs > 7 && !strncmp(text + vs, "'nonce-", 7)) d->nonce_required = 1;
        else if (ve - vs > 8 && (!strncmp(text + vs, "'sha256-", 8) || !strncmp(text + vs, "'sha384-", 8) ||
                                 !strncmp(text + vs, "'sha512-", 8))) {
            /* A HASH SOURCE permits an inline script whose body hashes to it. Computing that is a real
               capability this file does not have, so it is recorded as unknown rather than assumed to
               permit or forbid — see policy_allows. */
            d->unknown_source = 1;
        } else if (text[vs] == '\'') {
            d->unknown_source = 1;   /* a keyword source this parser does not model */
        }
        /* A HOST SOURCE (https://x, *.example, data:) does not permit INLINE execution under any
           circumstances, so it needs no flag: CSP §6.1 requires 'unsafe-inline', a nonce or a hash. */
    }
}

/* CSP §2.2.1's parse: directives separated by ';', each a name and a source list. Only the script-governing
   ones are read; everything else is kept in the text and answered for by nobody. */
static void parse_policy(Policy *p, const char *text, size_t len)
{
    size_t i = 0;

    while (i < len) {
        size_t start, end, ni, name_len;
        const char *name;
        Directive *d;

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

        /* THE `sandbox` DIRECTIVE IS READ FIRST AND SEPARATELY, because its value is not a source list. §7.1.5
           parses it, and §2.2.1's "a directive whose name is already in this policy is IGNORED" applies to it
           exactly as it applies to the four below — the first one in a policy wins. */
        if (kw(name, name_len, "sandbox")) {
            if (!p->sandbox_present) {
                p->sandbox_present = 1;
                p->sandbox_flags = sandbox_parse_directive(text + ni, end - ni);
            }
            continue;
        }
        d = kw(name, name_len, "script-src")      ? &p->script_src
          : kw(name, name_len, "script-src-elem") ? &p->script_src_elem
          : kw(name, name_len, "script-src-attr") ? &p->script_src_attr
          : kw(name, name_len, "default-src")     ? &p->default_src
                                                  : NULL;
        if (!d) continue;
        /* §2.2.1: a directive whose name is ALREADY in this policy is IGNORED — the first wins, and this is
           per-policy, which is why the container keeps the policies apart. */
        if (d->present) continue;
        d->present = 1;
        parse_source_list(d, text, ni, end);
    }
}

/* CSP §6.1's FALLBACK CHAIN, which is where the merged-flags version went wrong: `script-src` does not add to
   `default-src`, it REPLACES it for scripts, and the granular forms replace `script-src` in turn. So the
   directive to read is chosen first and only that one's sources are consulted. `script-src-attr` governs event
   handlers and javascript: URLs; `script-src-elem` governs script elements; eval has no granular form and
   falls straight to `script-src`. A policy with none of them says nothing about scripts — which is not the
   same as forbidding them. */
static const Directive *governing(const Policy *p, PolicyScriptKind kind)
{
    const Directive *d = kind == POLICY_INLINE_SCRIPT                                   ? &p->script_src_elem
                       : (kind == POLICY_INLINE_HANDLER || kind == POLICY_JAVASCRIPT_URL) ? &p->script_src_attr
                                                                                          : NULL;
    if (d && d->present) return d;
    if (p->script_src.present) return &p->script_src;
    if (p->default_src.present) return &p->default_src;
    return NULL;
}

/* A SERIALIZED CSP LIST is comma-delimited (CSP §2.2). Counting first and allocating once keeps the policies
   in one block, which is what the clone copies. */
static void parse_policy_list(PolicyContainer *c, const char *text, size_t len)
{
    size_t i, n = 1;
    size_t start;

    for (i = 0; i < len; i++)
        if (text[i] == ',') n++;
    c->policies = calloc(n, sizeof *c->policies);
    CHECK(c->policies != NULL, "policy container: OOM parsing a policy list");
    c->n_policies = n;

    n = 0;
    start = 0;
    for (i = 0; i <= len; i++) {
        if (i == len || text[i] == ',') {
            parse_policy(&c->policies[n++], text + start, i - start);
            start = i + 1;
        }
    }
    DCHECK(n == c->n_policies, "the policy-list split disagreed with its own count");
}

PolicyContainer *policy_container_new(const char *csp_text, const char *referrer_policy)
{
    PolicyContainer *p = calloc(1, sizeof *p);
    CHECK(p != NULL, "policy container: OOM");
    if (csp_text && *csp_text) {
        p->csp_text = strdup(csp_text);
        CHECK(p->csp_text != NULL, "policy container: OOM copying a policy");
        parse_policy_list(p, p->csp_text, strlen(p->csp_text));
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
    free(p->policies);
    free(p->referrer_policy);
    free(p);
}

const char *policy_container_csp(const PolicyContainer *p) { return p ? p->csp_text : NULL; }

SandboxFlags policy_csp_derived_sandboxing_flags(const char *serialized_csp_list, size_t len)
{
    /* §7.1.5's "Every CSP list cspList has CSP-derived sandboxing flags", verbatim: collect the `sandbox`
       directive of every ENFORCE policy, and if there are any, parse a sandboxing directive over the LAST
       one. Not the union of them — the last one WINS, which is the one place in CSP where a later policy does
       something other than narrow, and is why this cannot be answered from a per-container flag.
       NO OWNED TEXT: the parse copies nothing out of `serialized_csp_list` (a Policy holds bytes only), so
       the container below borrows the caller's bytes for the length of this call and owns only its array. */
    PolicyContainer c;
    SandboxFlags out = 0;
    size_t i;

    if (!serialized_csp_list || !len) return 0;
    memset(&c, 0, sizeof c);
    parse_policy_list(&c, serialized_csp_list, len);
    for (i = 0; i < c.n_policies; i++)
        if (c.policies[i].sandbox_present)
            out = c.policies[i].sandbox_flags;
    free(c.policies);
    return out;
}

/* Does ONE policy permit this? The directive is chosen by §6.1's chain first, then read. */
static bool one_allows(const Policy *pol, PolicyScriptKind kind)
{
    const Directive *d = governing(pol, kind);

    /* A policy that governs no script directive says nothing about scripts, which is not the same as
       forbidding them: `img-src 'none'` alone blocks no handler. */
    if (!d)
        return true;
    /* AND THE ONE THIS FILE CANNOT ANSWER CRASHES. A hash source permits an inline script whose body hashes to
       it, and computing that hash is a capability this parser does not have; a keyword it does not model could
       permit or forbid. Answering "allowed" would report an exploit that a real browser blocks, and answering
       "blocked" would suppress a real one — both are worse than naming the missing piece here. */
    DCHECK(!d->unknown_source,
           "this policy carries a source expression the parser does not model (a hash source, or a keyword "
           "outside 'unsafe-inline'/'unsafe-eval'/'strict-dynamic'/'self'/'none'/'nonce-'), and the verdict "
           "for an inline script under it cannot be decided without it — build that source expression");
    switch (kind) {
    case POLICY_INLINE_SCRIPT:
    case POLICY_INLINE_HANDLER:
    case POLICY_JAVASCRIPT_URL:
        /* CSP §6.1: a nonce or hash source makes 'unsafe-inline' be IGNORED — for the WHOLE DIRECTIVE, not
           only for script elements, which is the rule that makes adding a nonce to a legacy policy actually
           tighten it rather than widen it. A nonce does not rescue a handler or a javascript: URL either —
           neither can carry one — so once 'unsafe-inline' is ignored there is nothing left to permit them.
           'strict-dynamic' does not permit inline at all; it only stops host sources from mattering. */
        return d->unsafe_inline && !d->nonce_required;
    default:
        DCHECK(kind == POLICY_EVAL, "a policy was asked about a script kind this component does not have");
        return d->unsafe_eval != 0;
    }
}

bool policy_allows(const PolicyContainer *p, PolicyScriptKind kind)
{
    size_t i;

    /* A document with no policy allows everything, which is the overwhelmingly common case and is what "no
       Content-Security-Policy header" means. */
    if (!p)
        return true;
    /* CSP §2.2: the policies in a list are enforced INDEPENDENTLY, so content runs only if EVERY one permits
       it. That is why the container keeps them apart rather than flattening them — a second policy can only
       narrow, and a flattened one could widen. */
    for (i = 0; i < p->n_policies; i++)
        if (!one_allows(&p->policies[i], kind))
            return false;
    return true;
}
