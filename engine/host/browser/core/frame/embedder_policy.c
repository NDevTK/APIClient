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
bool embedder_policy_value_of_token(const char *token, EmbedderPolicyValue *out)
{
    if (!token) return false;
    if (!strcmp(token, "unsafe-none"))    { *out = EMBEDDER_POLICY_UNSAFE_NONE;    return true; }
    if (!strcmp(token, "require-corp"))   { *out = EMBEDDER_POLICY_REQUIRE_CORP;   return true; }
    if (!strcmp(token, "credentialless")) { *out = EMBEDDER_POLICY_CREDENTIALLESS; return true; }
    return false;
}

const char *embedder_policy_value_token(EmbedderPolicyValue v)
{
    switch (v) {
    case EMBEDDER_POLICY_UNSAFE_NONE:    return "unsafe-none";
    case EMBEDDER_POLICY_REQUIRE_CORP:   return "require-corp";
    case EMBEDDER_POLICY_CREDENTIALLESS: return "credentialless";
    }
    /* NOT A `default:` ARM, so the day §7.1.4 gains a fourth value the COMPILER names every switch that has
       to grow. This is reached only by a value that is not one of the three, which is a cast from an integer
       somebody read off a wire without asking whether it names a policy. */
    DFAIL("an embedder policy value that HTML §7.1.4 does not define was serialized — the section names three "
          "strings and embedder_policy_value_of_token is what turns bytes back into one, so a value outside "
          "them is an integer that crossed a seam pretending to be a policy");
    return "unsafe-none";
}

SerializedEmbedderPolicy serialized_embedder_policy(EmbedderPolicyValue value, const char *endpoint,
                                                    EmbedderPolicyValue report_only_value,
                                                    const char *report_only_endpoint)
{
    SerializedEmbedderPolicy out;

    /* §7.1.4 MAKES BOTH ENDPOINTS STRINGS WHOSE ABSENCE IS THE EMPTY ONE, so there is no NULL to spell here —
       and refusing it at the one constructor is what keeps every seam from having to decide what a NULL meant.
       An opener policy's endpoint IS null initially (core/frame/opener_policy.h), which is why the two are
       spelled differently and why a caller that copied the other one's shape crashes here rather than handing
       a reader an absence that is not this standard's. */
    DCHECK(endpoint != NULL && report_only_endpoint != NULL,
           "§7.1.4's embedder policy was built with a NULL reporting endpoint — the section says \"a reporting "
           "endpoint STRING, initially the empty string\", so the absence of one IS the empty string and a "
           "NULL is a producer that stopped writing the item rather than a policy that names no endpoint");
    out.value = value;
    out.endpoint = endpoint;
    out.report_only_value = report_only_value;
    out.report_only_endpoint = report_only_endpoint;
    return out;
}

SerializedEmbedderPolicy serialized_embedder_policy_new(void)
{
    return serialized_embedder_policy(EMBEDDER_POLICY_UNSAFE_NONE, "", EMBEDDER_POLICY_UNSAFE_NONE, "");
}

SerializedEmbedderPolicy serialized_embedder_policy_of(const EmbedderPolicy *p)
{
    DCHECK(p != NULL, "§7.1.4's embedder policy was serialized from nothing — every §7.1.7 policy container "
                      "holds one, initially a new one, so a NULL here is a caller holding half a container");
    return serialized_embedder_policy(p->value, p->endpoint, p->report_only_value, p->report_only_endpoint);
}

void embedder_policy_adopt(EmbedderPolicy *out, SerializedEmbedderPolicy s)
{
    DCHECK(out != NULL, "§7.1.7's clone was asked to copy an embedder policy into nothing");
    /* EVERY OWNED FIELD IS DUPLICATED, WHICH IS THE WHOLE OF THIS FUNCTION AND THE WHOLE OF THE OBLIGATION AN
       ITEM CREATES. A struct copied field-by-field must dup every owned field, so the day §7.1.4 gains a fifth
       item this is one of the two places that must grow — the other is embedder_policy_free, and they are read
       together for exactly that reason. `serialized_embedder_policy` has already refused a NULL endpoint, so
       ep_dup here is copying a string rather than deciding what an absence meant. */
    out->value = s.value;
    out->report_only_value = s.report_only_value;
    out->endpoint = ep_dup(s.endpoint);
    out->report_only_endpoint = ep_dup(s.report_only_endpoint);
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
    if (it.item.kind != SF_TOKEN || !embedder_policy_value_of_token(it.item.text, &v) ||
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
