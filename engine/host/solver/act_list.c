/* solver/act_list.c — the acts a run performed, as a PoC's or a recipe's `acts` (see act_list.h). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "core/json_buf.h"
#include "solver/act_list.h"
#include "solver/concolic.h"
#include "solver/engine.h"
#include "solver/flow.h"
#include "solver/message_compose.h"   /* a message act's candidate data, from its domain */
#include "solver/reclaim.h"

static void acts_append(JsonBuf *b, int *first, const char *act)
{
    if (!*first) json_buf_raw(b, ",");
    *first = 0;
    json_buf_raw(b, act);
}

/* Who holds the origin a message act is posted from (a JSON String this file composed): `attacker-site` for
   the attacker's own site or a name under it, which the person verifying serves, `registered-domain` for a
   registrable domain an attacker would have to register, which no live verify reproduces without one. */
static const char *origin_owned(const char *origin)
{
    const char *site = concolic_attacker_site(), *host = strstr(origin, "://");
    size_t hn, sn = strlen(site);

    DCHECK(host && origin[0] == '"', "a message act's origin is not a JSON String holding an origin");
    host += 3;
    hn = strcspn(host, "\":/");
    if ((hn == sn && !memcmp(host, site, sn)) || (hn > sn + 1 && host[hn - sn - 1] == '.' &&
                                                   !memcmp(host + hn - sn, site, sn)))
        return "attacker-site";
    return "registered-domain";
}

/* `act` with the `data` (JSON) and the `origin` (a JSON String) its message is posted with (both borrowed), and
   who holds that origin (origin_owned), malloc'd. */
static char *act_with_message(const char *act, const char *data, const char *origin)
{
    JsonBuf b = { 0 };
    size_t l = strlen(act);
    char *body = reclaim_malloc(l);

    DCHECK(l > 2 && act[l - 1] == '}', "a message act is not an object");
    CHECK(body, "act list: OOM composing a message act");
    memcpy(body, act, l - 1);
    body[l - 1] = 0;
    json_buf_raw(&b, body);
    free(body);
    json_buf_raw(&b, ",");
    json_buf_key(&b, "origin");
    json_buf_raw(&b, origin);
    json_buf_raw(&b, ",");
    json_buf_key(&b, "originOwned");
    json_buf_str(&b, origin_owned(origin));
    json_buf_raw(&b, ",");
    json_buf_key(&b, "data");
    json_buf_raw(&b, data);
    json_buf_raw(&b, "}");
    return json_buf_take(&b);
}

/* One delivery appended to `*dl` in flow_deliveries_text's format. */
static void dl_append(char **dl, const char *src, const char *val)
{
    size_t o = *dl ? strlen(*dl) : 0, n = o + strlen(src) + strlen(val) + 44;
    char *grown = realloc(*dl, n);

    CHECK(grown, "act list: OOM recording a message's composition");
    snprintf(grown + o, n - o, "%zu:%s%zu:%s", strlen(src), src, strlen(val), val);
    *dl = grown;
}

/* Candidate `v` of the origins message_compose_origins composes for `osrc`, which composes at least one, as a
   JSON String, malloc'd; `*n` takes how many there are, and `v` must be below it. */
static char *origin_json(const char *osrc, int v, int *n)
{
    char **origins, *out;
    JsonBuf b = { 0 };

    *n = message_compose_origins(osrc, &origins);
    DCHECK(v >= 0 && v < *n, "an origin candidate was asked for past the candidates composed for its source");
    json_buf_str(&b, origins[v]);
    for (int i = 0; i < *n; i++) free(origins[i]);
    free(origins);
    out = json_buf_take(&b);
    CHECK(out, "act list: OOM spelling a composed origin");
    return out;
}

/* A message act of the chain, stating the origin it is posted from and its data. Where the flow delivers both
   (a candidate's composed messages), the act states the delivered values and is solved. Otherwise each value
   not delivered is composed (message_compose, message_compose_origin), `*unsolved` is raised, and the
   compositions are appended to `*dl` (flow_deliveries_text's format) for the verification that delivers them;
   `*refused` takes the first data composition's refusal, malloc'd, and the act then states neither value. */
static char *message_act(const Flow *f, int k, char **refused, int *unsolved, char **dl)
{
    char root[96], osrc[96], *data, *origin, *why = NULL, *act;
    /* An exploring flow (a sighting's witness) injects nothing: every message holds what its own domain admits. */
    const char *inject = f->cand_src ? f->cand_src : "", *payload = f->cand_payload ? f->cand_payload : "";
    const char *ddel, *odel;

    engine_orphan_message_sources(f, k, root, osrc, sizeof root);
    ddel = flow_delivery_of(f, root);
    odel = flow_delivery_of(f, osrc);
    if (ddel && odel) return act_with_message(f->orphan_acts[k], ddel, odel);
    *unsolved = 1;
    /* The payload rides this message only where the search's injection is this message's data or one of its
       members; every other message of the chain carries what its own domain admits. */
    if (*inject && strcmp(inject, root) && !concolic_member_of_root(inject, root)) inject = "";
    data = ddel ? reclaim_strdup(ddel) : message_compose(root, inject, payload, &why);
    if (!data) {
        if (!*refused) *refused = why;
        else free(why);
        act = reclaim_strdup(f->orphan_acts[k]);
        CHECK(act, "act list: OOM copying a message act");
        return act;
    }
    {
        int no;

        origin = odel ? reclaim_strdup(odel) : origin_json(osrc, 0, &no);
        CHECK(origin, "act list: OOM copying a delivered origin");
    }
    act = act_with_message(f->orphan_acts[k], data, origin);
    if (!ddel) dl_append(dl, root, data);
    if (!odel) dl_append(dl, osrc, origin);
    free(data);
    free(origin);
    return act;
}

char *act_list_json(const Flow *f, int *no_act, int *unsolved, char **refused, char **dl)
{
    int called = !f->orphan ? 0 : f->orphan_want ? f->orphan_claim_at : f->orphan_chain_n, k, s = 0, first = 1;
    JsonBuf b = { 0 };

    *no_act = 0;
    *unsolved = 0;
    *refused = NULL;
    *dl = NULL;
    /* A seeded drive writes its chain with its mark, so only the cold tier's rebuild (cold.c's 'o' record:
       `orphan_want` with nothing claimed) leaves a drive that has called nothing. Its replay before the first
       claim is the page's own reach, as engine.c's fork treats it ("a drive that has called nothing"), so it
       contributes no drive act; the modelled user acts still name themselves. */
    DCHECK(!f->orphan || called > 0 || (f->orphan_want && f->orphan_claim_at == 0),
           "an act list was asked of a drive whose chain has no called member and which is not a resumed drive "
           "waiting for its first call — the mark and the chain are written together at the seed and at the claim");
    if (called == 0 && f->user_acts_n == 0) return NULL;
    for (k = 0; k < called; k++)
        if (!f->orphan_acts[k]) { *no_act = 1; return NULL; }
    json_buf_raw(&b, "[");
    /* In performance order: a user act performed after `user_act_at` called members stands before that member. */
    for (k = 0; k <= called; k++) {
        for (; s < f->user_acts_n && f->user_act_at[s] <= k; s++) acts_append(&b, &first, f->user_acts[s]);
        if (k == called) break;
        if (strstr(f->orphan_acts[k], "\"kind\":\"message\"")) {
            char *act = message_act(f, k, refused, unsolved, dl);

            acts_append(&b, &first, act);
            free(act);
        } else {
            acts_append(&b, &first, f->orphan_acts[k]);
        }
    }
    json_buf_raw(&b, "]");
    return json_buf_take(&b);
}


char *act_list_message_deliveries(const Flow *f, const char *inject, const char *payload)
{
    int called = !f->orphan ? 0 : f->orphan_want ? f->orphan_claim_at : f->orphan_chain_n;
    char *out = NULL;

    DCHECK(inject && *inject && payload && *payload, "message deliveries were composed with no injection or payload");
    for (int k = 0; k < called; k++) {
        char root[96], osrc[96], *data, *origin, *why = NULL;
        const char *inj = inject;

        if (!f->orphan_acts[k] || !strstr(f->orphan_acts[k], "\"kind\":\"message\"")) continue;
        engine_orphan_message_sources(f, k, root, osrc, sizeof root);
        if (strcmp(inj, root) && !concolic_member_of_root(inj, root)) inj = "";
        data = message_compose(root, inj, payload, &why);
        if (!data) { free(why); free(out); return NULL; }
        dl_append(&out, root, data);
        free(data);
        /* Each origin candidate under `<origin source>#<v>`, once per source: the messages one window posts share
           its origin source, and a candidate run delivers variant v of it to all of them. */
        if (out) {
            char first[112];

            snprintf(first, sizeof first, "%zu:%s#0", strlen(osrc) + 2, osrc);
            if (strstr(out, first)) continue;
        }
        for (int v = 0, no = 1; v < no; v++) {
            char key[112];

            origin = origin_json(osrc, v, &no);
            snprintf(key, sizeof key, "%s#%d", osrc, v);
            dl_append(&out, key, origin);
            free(origin);
        }
    }
    return out;
}
