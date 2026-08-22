/* HTML §7.1.3's cross-origin opener policy. See opener_policy.h. */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "core/fetch/structured_fields.h"
#include "core/frame/embedder_policy.h"
#include "core/frame/opener_policy.h"

void opener_policy_init(OpenerPolicy *p)
{
    DCHECK(p != NULL, "an opener policy was initialized into nothing");
    p->value = OPENER_POLICY_UNSAFE_NONE;
    p->report_only_value = OPENER_POLICY_UNSAFE_NONE;
    /* §7.1.3: "a reporting endpoint, which is string or NULL, initially null." Not the empty string — the
       embedder policy beside it is the one whose endpoints start empty, and the two must not be confused. */
    p->reporting_endpoint = NULL;
    p->report_only_reporting_endpoint = NULL;
}

void opener_policy_free(OpenerPolicy *p)
{
    if (!p) return;
    free(p->reporting_endpoint);
    free(p->report_only_reporting_endpoint);
    p->reporting_endpoint = NULL;
    p->report_only_reporting_endpoint = NULL;
}

/* §7.1.3.1: "if parsedItem[1]["report-to"] exists AND IT IS A STRING". A `report-to` of any other kind is not
   one, and is ignored exactly as an absent one is. */
static void op_take_report_to(const SfItem *it, char **endpoint)
{
    const SfBareItem *report_to = sf_item_param(it, "report-to");

    if (!report_to || report_to->kind != SF_STRING) return;
    free(*endpoint);
    *endpoint = strdup(report_to->text);
    CHECK(*endpoint != NULL, "opener policy: OOM copying a reporting endpoint");
}

void opener_policy_obtain(OpenerPolicy *out, const HeaderList *headers, bool secure_context)
{
    SfItem it;

    DCHECK(out != NULL && headers != NULL, "an opener policy was obtained from nothing");
    opener_policy_init(out);   /* step 1: "let policy be a new opener policy" */
    /* Step 2: "if reservedEnvironment is a non-secure context, then return policy." */
    if (!secure_context)
        return;

    /* Step 3-4: the ENFORCED header. */
    if (sf_header_item(headers, "cross-origin-opener-policy", &it)) {
        if (it.item.kind == SF_TOKEN) {
            if (!strcmp(it.item.text, "same-origin")) {
                /* §7.1.3 step 4.1: "let coep be the result of obtaining a cross-origin embedder policy from
                   response and reservedEnvironment; if coep's value is COMPATIBLE WITH CROSS-ORIGIN
                   ISOLATION, then set policy's value to `same-origin-plus-COEP`; otherwise set it to
                   `same-origin`." This is the whole reason the two headers cannot be read separately: a page
                   sending only COOP is `same-origin`, and the SAME page with a `require-corp` beside it is the
                   value that makes §7.1.3.2 mark the new group cross-origin isolated. */
                EmbedderPolicy coep;
                embedder_policy_obtain(&coep, headers, secure_context);
                out->value = embedder_policy_compatible_with_cross_origin_isolation(coep.value)
                                 ? OPENER_POLICY_SAME_ORIGIN_PLUS_COEP
                                 : OPENER_POLICY_SAME_ORIGIN;
                embedder_policy_free(&coep);
            } else if (!strcmp(it.item.text, "same-origin-allow-popups")) {
                out->value = OPENER_POLICY_SAME_ORIGIN_ALLOW_POPUPS;
            } else if (!strcmp(it.item.text, "noopener-allow-popups")) {
                out->value = OPENER_POLICY_NOOPENER_ALLOW_POPUPS;
            }
            /* NO `else` — §7.1.3.1: "user agents will IGNORE this header if it contains an invalid value",
               and an explicit `same-origin-plus-COEP` token is one of those: the standard says that value
               "cannot be directly set via the header", so it has no branch here and leaves the default. */
        }
        op_take_report_to(&it, &out->reporting_endpoint);
        sf_item_free(&it);
    }

    /* Step 5-6: the REPORT-ONLY header, whose branch list is DELIBERATELY SHORTER — §7.1.3 gives it
       `same-origin` and `same-origin-allow-popups` and NOT `noopener-allow-popups`. Reproduced as written. */
    if (sf_header_item(headers, "cross-origin-opener-policy-report-only", &it)) {
        if (it.item.kind == SF_TOKEN) {
            if (!strcmp(it.item.text, "same-origin")) {
                /* §7.1.3 step 6.1, and it is NOT the same test as step 4.1's: report-only COOP takes
                   `same-origin-plus-COEP` when the COEP's value OR ITS REPORT-ONLY VALUE is compatible with
                   cross-origin isolation. The standard's own note says why — "this allows developers more
                   freedom in the order of deployment of COOP and COEP" — so the asymmetry is the feature. */
                EmbedderPolicy coep;
                embedder_policy_obtain(&coep, headers, secure_context);
                out->report_only_value =
                    (embedder_policy_compatible_with_cross_origin_isolation(coep.value) ||
                     embedder_policy_compatible_with_cross_origin_isolation(coep.report_only_value))
                        ? OPENER_POLICY_SAME_ORIGIN_PLUS_COEP
                        : OPENER_POLICY_SAME_ORIGIN;
                embedder_policy_free(&coep);
            } else if (!strcmp(it.item.text, "same-origin-allow-popups")) {
                out->report_only_value = OPENER_POLICY_SAME_ORIGIN_ALLOW_POPUPS;
            }
        }
        /* AND HERE THE ENDPOINT IS THE REPORT-ONLY ONE, where §7.1.4's report-only branch writes the ENFORCED
           endpoint. The two standards' sections differ on this line and both are implemented as written. */
        op_take_report_to(&it, &out->report_only_reporting_endpoint);
        sf_item_free(&it);
    }
}
