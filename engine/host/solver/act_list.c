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

static void acts_append(JsonBuf *b, int *first, const char *act)
{
    if (!*first) json_buf_raw(b, ",");
    *first = 0;
    json_buf_raw(b, act);
}

/* `act` with the candidate `data` composed for its message (borrowed), malloc'd. */
static char *act_with_data(const char *act, const char *data)
{
    JsonBuf b = { 0 };
    size_t l = strlen(act);
    char *body = malloc(l);

    DCHECK(l > 2 && act[l - 1] == '}', "a message act is not an object");
    CHECK(body, "solve: OOM composing a message act");
    memcpy(body, act, l - 1);
    body[l - 1] = 0;
    json_buf_raw(&b, body);
    free(body);
    json_buf_raw(&b, ",");
    json_buf_key(&b, "data");
    json_buf_raw(&b, data);
    json_buf_raw(&b, "}");
    return json_buf_take(&b);
}

/* A message act of the chain. Where the flow delivers this message's data (a verification), the act states the
   delivered value and is solved. Otherwise it states the data its message's domain composes (message_compose)
   where it can, `*unsolved` is raised, and the composition is appended to `*dl` (flow_deliveries_text's format)
   for the verification that delivers it; `*refused` takes the first composition refusal, malloc'd. */
static char *message_act(const Flow *f, int k, char **refused, int *unsolved, char **dl)
{
    char root[96], *data, *why = NULL, *act;
    const char *inject = f->cand_src, *delivered;

    engine_orphan_message_data_src(f, k, root, sizeof root);
    delivered = flow_delivery_of(f, root);
    if (delivered) return act_with_data(f->orphan_acts[k], delivered);
    *unsolved = 1;
    /* The payload rides this message only where the search's injection is this message's data or one of its
       members; every other message of the chain carries what its own domain admits. */
    if (strcmp(inject, root) && !concolic_member_of_root(inject, root)) inject = "";
    data = message_compose(root, inject, f->cand_payload, &why);
    if (!data) {
        if (!*refused) *refused = why;
        else free(why);
        act = strdup(f->orphan_acts[k]);
        CHECK(act, "solve: OOM copying a message act");
        return act;
    }
    act = act_with_data(f->orphan_acts[k], data);
    {
        size_t o = *dl ? strlen(*dl) : 0, n = o + strlen(root) + strlen(data) + 44;
        char *grown = realloc(*dl, n);

        CHECK(grown, "solve: OOM recording a message's composition");
        snprintf(grown + o, n - o, "%zu:%s%zu:%s", strlen(root), root, strlen(data), data);
        *dl = grown;
    }
    free(data);
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
           "a fire ran in a drive whose chain has no called member and which is not a resumed drive waiting "
           "for its first call — the mark and the chain are written together at the seed and at the claim");
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

