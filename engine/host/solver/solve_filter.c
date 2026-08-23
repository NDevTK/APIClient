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
            /* A RUN OF ONE IS NOT A RUN, AND THIS IS THE COMPONENT'S OWN CONTRACT APPLIED AT ITS BOUNDARY
               RATHER THAN A THRESHOLD ADDED TO IT. The header states why this field is a run and not a tally:
               "the escape a breakout is made of is a SEQUENCE, so twelve of its bytes scattered through the
               output are worth nothing and four of them adjacent are worth something, and a per-character
               tally cannot tell those apart." A single byte IS that per-character tally — one character of a
               payload carries no positional information at all, and every byte a constructed escape is made of
               (`<`, `s`, `v`, `g`, `=`, `(`, `)`) occurs in ordinary markup on its own.
               TWO IS THE ONLY NON-ARBITRARY VALUE, which is what makes this not the magic number the ratchet
               was designed to avoid: it is the shortest length at which "contiguous" is a CONSTRAINT rather
               than a restatement of "this character appears". Three, four, or half the payload would each be a
               tuning knob; two is where the word in the field's own name starts to mean something.
               MEASURED: a `<svg onload=X9()>` breakout reported `survivedBy: 1` beside a probe's 14, in a
               search whose breakout had demonstrably never travelled — a one-byte accident rendered
               indistinguishable from travel, in the field built to tell those apart. */
            if (k < 2 && clen >= 2) k = 0;
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
    DCHECK(o->run != 1 || o->len == 1,
           "the @S filter-survival observation reported a surviving run of ONE byte for a multi-byte candidate "
           "— a single character carries no positional information and is the per-character tally this field "
           "exists not to be, so a 1 here means the run floor above was bypassed and a coincidence is about to "
           "be reported as travel");
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

/* THE DELIVERABILITY TABLE — see the header for why it is observed rather than read off the declaration. */

void solve_delivered_all(SolveDelivered *d)
{
    DCHECK(d != NULL, "a search's byte-deliverability table was initialised through nothing — the table is the "
                      "constraint every derived escape is constructed under, and an uninitialised one is read "
                      "out of whatever the pending array's realloc last held");
    memset(d->ok, 1, sizeof d->ok);
}

int solve_delivered_byte(const SolveDelivered *d, char c)
{
    DCHECK(d != NULL, "the byte-deliverability table was asked about a byte with no table — a derivation "
                      "choosing between two spellings of one exit transition would then choose from nothing");
    return d->ok[(unsigned char)c] != 0;
}

/* EVERY BYTE, INCLUDING THE ONES A DERIVATION DID NOT CHOOSE. An escape is a SEQUENCE and it fires or it does
   not: one byte the source cannot carry defeats the whole of it, so the question is asked about the finished
   string rather than about the exit transition alone. */
int solve_delivered_ok(const SolveDelivered *d, const char *s)
{
    const unsigned char *p;

    DCHECK(d != NULL && s != NULL,
           "a constructed escape was checked for deliverability with no table or no escape — the check is what "
           "stops a breakout being seeded for a whole document re-run it cannot survive");
    for (p = (const unsigned char *)s; *p; p++) if (!d->ok[*p]) return 0;
    return 1;
}
