/* The cold tier's first contract — the snapshot census. See cold.h. */
#include "solver/cold.h"
#include "solver/flow.h"
#include "solver/cow.h"
#include "solver/dom_cow.h"
#include "solver/decide.h"
#include "solver/concolic.h"
#include "check.h"

#include <string.h>

/* THE WALK IS OVER THE FRONTIER, not over an allocator. Every row below is asked of the component that owns
   that part of a snapshot, because that is the only place the size is known without restating a private
   `sizeof` — and a restated one drifts silently the next time an entry kind is added, which is exactly the
   class of wrong-answer-that-looks-like-a-measurement this exists to avoid. */
void cold_census(ColdCensus *out)
{
    const Flow *f;
    int i;

    DCHECK(out != NULL, "the snapshot census was asked to fill nothing");
    memset(out, 0, sizeof *out);

    for (i = 0; (f = flow_at(i)) != NULL; i++) {
        long e = 0, b = 0;

        out->flows++;
        if (f->frame) out->framed++;
        if (flow_blocked(f)) out->blocked++;

        /* THE DECISION VECTOR, in BOTH of the two places one flow's can be. A parked flow's lives in its
           suspend blob; the RUNNING flow's is live in decide.c and its `dec` field is only the vector it was
           born with. Counting one and not the other would report the running flow as free. */
        e = b = 0;
        if (f->dec_blob) decide_blob_stats(f->dec_blob, &e, &b);
        else if (f == flow_running()) decide_live_stats(&e, &b);
        out->dec_entries += e;
        out->dec_bytes += b;
        /* …plus the vector the flow was CREATED with, which flow.c owns for the flow's whole life. A flow that
           has never been switched in has only that one; a suspended one holds a second copy of the same prefix
           in its blob, and both are real bytes. */
        if (f->dec) { out->dec_entries += f->dec_n; out->dec_bytes += f->dec_n; }

        cow_delta_head_stats((const CowDelta *)f->delta, &e, &b);
        out->head_entries += e;
        out->head_bytes += b;

        out->dom_head_entries += f->dom_n;
        out->dom_head_bytes += dom_cow_head_bytes(f->dom_cap);

        out->job_count += f->njob;
        /* THE REPLIES THE HOST STILL OWES, asked of the register itself rather than measured here — the same
           rule the rest of this walk keeps. It is a JS Array of records now, so its bytes are quickjs's and its
           strings are shared with everything else that names them; a `sizeof` restated here would have to know
           the record's shape, which is exactly what drifts the next time a field is added. */
        out->pend_count += pending_count(f->pending);
        out->pend_bytes += pending_bytes(f->pending);
        {
            int k;
            for (k = 0; k < f->dyn_n; k++) out->dyn_bytes += (long)strlen(f->dyn[k]) + 1;
        }

        out->misc_bytes += (long)sizeof(Flow);
        if (f->cand_src) out->misc_bytes += (long)strlen(f->cand_src) + 1;
        if (f->cand_payload) out->misc_bytes += (long)strlen(f->cand_payload) + 1;
        if (f->deliver) out->misc_bytes += (long)strlen(f->deliver) + 1;
        if (f->deliver_origin) out->misc_bytes += (long)strlen(f->deliver_origin) + 1;
        if (f->jobs) out->misc_bytes += (long)f->jobcap * (long)sizeof(FlowJob);
    }

    /* THE SHARED ROWS, ONCE. A frozen segment is referenced by every flow forked below it, so adding it to each
       flow's own total would report the structural sharing as if it did not exist — which is precisely the
       mistake a pager makes when it writes one flow's snapshot without knowing what its siblings already
       wrote. */
    cow_chain_stats(&out->seg_count, &out->seg_entries);
    out->seg_bytes = cow_chain_bytes();
    dom_cow_chain_stats(&out->dom_seg_count, &out->dom_seg_entries);
    out->dom_seg_bytes = dom_cow_chain_bytes();
    concolic_chain_stats(&out->pin_seg_count, &out->pin_seg_entries, &out->pin_seg_bytes);
    decide_chain_stats(&out->dec_seg_count, &out->dec_seg_entries, &out->dec_seg_bytes);
}
