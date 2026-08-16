/* HTML §7.1.4's cross-origin embedder policy. See embedder_policy.h. */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "core/fetch/structured_fields.h"
#include "core/frame/embedder_policy.h"

static char *ep_dup(const char *s)
{
    char *r = strdup(s ? s : "");
    CHECK(r != NULL, "embedder policy: OOM copying a reporting endpoint");
    return r;
}

void embedder_policy_init(EmbedderPolicy *p)
{
    DCHECK(p != NULL, "an embedder policy was initialized into nothing");
    p->value = EMBEDDER_POLICY_UNSAFE_NONE;
    p->report_only_value = EMBEDDER_POLICY_UNSAFE_NONE;
    /* §7.1.4: "a reporting endpoint string, INITIALLY THE EMPTY STRING" — not null. The difference is real:
       the opener policy's endpoint is initially NULL, so the two absences are spelled differently and a
       consumer that tested one for the other would read this one as present. */
    p->endpoint = ep_dup("");
    p->report_only_endpoint = ep_dup("");
}

void embedder_policy_free(EmbedderPolicy *p)
{
    if (!p) return;
    free(p->endpoint);
    free(p->report_only_endpoint);
    p->endpoint = NULL;
    p->report_only_endpoint = NULL;
}

bool embedder_policy_compatible_with_cross_origin_isolation(EmbedderPolicyValue v)
{
    /* §7.1.4, verbatim: "An embedder policy value is compatible with cross-origin isolation if it is
       `credentialless` or `require-corp`." */
    return v == EMBEDDER_POLICY_CREDENTIALLESS || v == EMBEDDER_POLICY_REQUIRE_CORP;
}

/* §7.1.4.1: "The valid token values are the embedder policy values." A token outside them is not a value, and
   the caller's fail-open then leaves the policy at its default. `unsafe-none` is a valid token and IS the
   default, which is why this can answer it without the caller needing a third outcome. */
static bool ep_value_of_token(const char *token, EmbedderPolicyValue *out)
{
    if (!strcmp(token, "unsafe-none"))    { *out = EMBEDDER_POLICY_UNSAFE_NONE;    return true; }
    if (!strcmp(token, "require-corp"))   { *out = EMBEDDER_POLICY_REQUIRE_CORP;   return true; }
    if (!strcmp(token, "credentialless")) { *out = EMBEDDER_POLICY_CREDENTIALLESS; return true; }
    return false;
}

/* One header's contribution: parse it as an ITEM, and answer only when the item is a TOKEN naming a value that
   is compatible with cross-origin isolation — which is exactly the condition BOTH branches of §7.1.4's obtain
   are written with ("if parsedItem is non-null and parsedItem[0] is compatible with cross-origin isolation").
   `*endpoint` is replaced when the item carries a `report-to` parameter that is a STRING. */
static void ep_read(const HeaderList *headers, const char *name, EmbedderPolicyValue *value, char **endpoint)
{
    SfItem it;
    EmbedderPolicyValue v;
    const SfBareItem *report_to;

    if (!sf_header_item(headers, name, &it))
        return;   /* absent, or not a structured field at all — §7.1.4.1's fail-open */
    if (it.item.kind != SF_TOKEN || !ep_value_of_token(it.item.text, &v) ||
        !embedder_policy_compatible_with_cross_origin_isolation(v)) {
        sf_item_free(&it);
        return;
    }
    *value = v;
    /* §7.1.4.1: "of these, the `report-to` parameter can have a valid URL string identifying an appropriate
       reporting endpoint". A `report-to` that is not a string is not one, and the parameter is then ignored
       exactly as an absent one is. */
    report_to = sf_item_param(&it, "report-to");
    if (report_to && report_to->kind == SF_STRING) {
        free(*endpoint);
        *endpoint = ep_dup(report_to->text);
    }
    sf_item_free(&it);
}

void embedder_policy_obtain(EmbedderPolicy *out, const HeaderList *headers, bool secure_context)
{
    DCHECK(out != NULL && headers != NULL, "an embedder policy was obtained from nothing");
    embedder_policy_init(out);   /* step 1: "let policy be a new embedder policy" */
    /* Step 2: "if environment is a NON-SECURE CONTEXT, then return policy." A page served over http gets the
       default whatever it sends — which is why cross-origin isolation is unreachable off https and why the
       caller has to state the environment rather than this reading the header list alone. */
    if (!secure_context)
        return;
    ep_read(headers, "cross-origin-embedder-policy", &out->value, &out->endpoint);
    /* §7.1.4's report-only branch sets `policy's ENDPOINT` — not the report only one. That is the standard's
       own sentence; see embedder_policy.h for why it is reproduced rather than corrected here. */
    ep_read(headers, "cross-origin-embedder-policy-report-only", &out->report_only_value, &out->endpoint);
}
