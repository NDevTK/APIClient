/* @S filter-survival — see solve_filter.h for why this is a DISTANCE and not the boolean the sinks had. */
#include "solver/solve_filter.h"
#include "check.h"
#include <string.h>

void solve_filter_survival(const char *out, const char *cand, FilterObs *o) {
    size_t olen, clen, i;
    int best = 0, best_at = -1, best_out = -1;

    DCHECK(out != NULL && cand != NULL && o != NULL,
           "the @S filter-survival observation was asked to measure nothing, or with nowhere to put what it "
           "measures — it is called from a sink detector that has just converted a real string and holds the "
           "running candidate's own payload, so a NULL here is a caller that has neither");
    olen = strlen(out);
    clen = strlen(cand);
    DCHECK(clen > 0,
           "a candidate flow carries an EMPTY payload — the payload IS the substitution, so a flow with none is "
           "an exploration flow wearing a candidate's identity and every rung measured against it is measured "
           "against nothing");

    /* THE LONGEST COMMON RUN, from each start of the CANDIDATE. Addressed by the candidate's offsets rather
       than the output's because that is the answer a mutation needs — WHICH segment of the escape the page's
       filter ate — and because the candidate is the short side, which is what makes the prune below work. */
    for (i = 0; i < clen; i++) {
        const char *p = out;
        size_t left = olen;

        /* NO LONGER RUN CAN BEGIN HERE, so the remaining starts cannot improve on what is already recorded.
           This is not a bound on the work: it discards starts whose maximum possible answer is already known
           to be worse, which is arithmetic about this measurement and not a limit on how much of it runs. */
        if (clen - i <= (size_t)best) break;

        while (left > 0) {
            const char *q = memchr(p, cand[i], left);
            size_t k = 0;

            if (!q) break;
            while (i + k < clen && (size_t)(q - out) + k < olen && q[k] == cand[i + k]) k++;
            if ((int)k > best) { best = (int)k; best_at = (int)i; best_out = (int)(q - out); }
            if ((size_t)best == clen - i) break;   /* maximal for this start — nothing after it can beat it */
            left = olen - (size_t)(q - out) - 1;
            p = q + 1;
        }
    }

    o->len = (int)clen;
    o->run = best;
    o->at = best_at;
    o->out_at = best_out;

    /* TWO-SIDED, BECAUSE A WRONG SURVIVAL NUMBER IS A WRONG FITNESS AND A FITNESS IS AN ORDER. The first two
       state the arithmetic; the third RE-READS the bytes at the offsets just reported, which is the only thing
       that can say the pair of offsets names the run rather than merely being in range. */
    DCHECK(o->run >= 0 && o->run <= o->len,
           "the @S filter-survival observation reported a surviving run longer than the candidate it measured — "
           "the run is a substring of the candidate by construction, so this is the scan reading past its own "
           "operand");
    DCHECK((o->run == 0) == (o->at < 0 && o->out_at < 0),
           "the @S filter-survival observation reported a run without saying where it is, or a position without "
           "a run — the mutation step reads both together to decide which segment of the escape the page's "
           "filter ate, so half an answer is one it would act on");
    DCHECK(o->run == 0 ||
           ((size_t)o->at + (size_t)o->run <= clen && (size_t)o->out_at + (size_t)o->run <= olen &&
            !memcmp(out + o->out_at, cand + o->at, (size_t)o->run)),
           "the @S filter-survival observation reported a run that is not at the offsets it reported — the "
           "fitness the WFQ reads is computed from this number and the mutation reads these offsets, so a run "
           "that is not there orders the search by a measurement of nothing");
}
