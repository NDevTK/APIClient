/* The cold tier — the snapshot census, and the recipe that crosses the tier. See cold.h. */
#include "solver/cold.h"
#include "solver/flow.h"
#include "solver/cow.h"
#include "solver/dom_cow.h"
#include "solver/decide.h"
#include "solver/concolic.h"
#include "solver/world.h"
#include "solver/solve.h"     /* …and the @S sink table a parked candidate's class is re-bound through */
#include "check.h"

#include <stdio.h>
#include <stdlib.h>
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
           suspend blob — which is where a COLD-RESUMED flow's rebuilt chain lives too, so a resumed flow is
           counted exactly like a forked one; the RUNNING flow's is live in decide.c and in no blob at all.
           Counting one and not the other would report the running flow as free. */
        e = b = 0;
        if (f->dec_blob) decide_blob_stats(f->dec_blob, &e, &b);
        else if (f == flow_running()) decide_live_stats(&e, &b);
        out->dec_entries += e;
        out->dec_bytes += b;
        /* THERE IS NO SECOND ROW FOR A BIRTH VECTOR, and the row that used to stand here counted a field no
           caller ever set: `Flow.dec` was the flat from-baseline replay vector, always NULL, so this branch was
           unreachable and the two @COLD numbers it fed are unchanged by its deletion. It is worth saying what
           it WOULD have measured, because that is why the field is gone rather than renamed: it added
           `dec_n` to BOTH totals, i.e. one byte per entry, which is true of a private array and false of the
           shared chain that replaced it. `dec_entries` deliberately counts the whole chain once per flow that
           STANDS on it — the sharing multiplied back out, which is the honest answer to "how many decisions is
           this flow's path", and the pair with `dec_seg_*` below (counted once for the frontier) is exactly
           what says the sharing is real. `dec_bytes` is what the flow itself owns for it: one blob header. */

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
        if (f->disc_url) out->misc_bytes += (long)strlen(f->disc_url) + 1;
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

/* ─────────────────────────────────────────────────────────────────────────────────────────────────────────
   THE RECIPE — what crosses the tier. See cold.h for what it carries and what it deliberately does not.
 *
 * THE PARK DOCUMENT is ONE buffer per session, handed to the host as part of the result document. The host
 * stores it as ONE IndexedDB value keyed by (origin, bundle id), so there is exactly one of these however many
 * times the engine parks into it — and it parks more than once: a PARTIAL self-park writes the lowest-value
 * TAIL and releases it while the engine keeps running, and the tail it writes an hour later has to land in the
 * same document as the first, or whichever half the host does not store is a set of flows nothing will resume.
 * SO THE DOCUMENT IS APPENDED TO, RECORD BY RECORD, and nothing that terminates it is ever written into it.
 *
 * AND THE FORM IT IS ACCUMULATED IN IS THE FORM ITS OWN READER PARSES — ';'-joined records — which is the
 * correction this paragraph carries and the reason the read half had never run. The buffer used to hold JSON
 * (`["s0,-,01","f0,3"`, with the closing bracket rendered), while cold_resume parses a ';'-joined string; the
 * only producer of THAT language in the whole system was `extension/bridge.js`, which joins the stored array
 * on its way back to qjs_begin. So the engine WROTE one language and READ another, no process ever held both
 * ends, and every one of the resume arms below — the 's'/'c'/'d' rebuilds, park_unhex, solve_resume_candidate
 * — was reachable only from a browser with an IndexedDB in it. A component whose input nothing in the program
 * can produce is not a component that has not been exercised yet; it is one that CANNOT be, and the fix is not
 * a second host, it is to make the writer speak the reader's language.
 * THE JSON ARRAY IS A TRANSPORT, RENDERED AT THE BOUNDARY THAT NEEDS IT. `_park` rides the result document
 * because the trusted zone stores it in IndexedDB and IndexedDB stores JSON, so cold_park_json builds the
 * array out of the records on demand — which is also where the claim "nothing in this document needs JSON
 * escaping" is finally asserted rather than distributed across four writers. A host with a FILE (the fixture)
 * stores cold_park_recipes() directly and hands it straight back, which is the same document with one fewer
 * translation in it. */
static char *g_park;
static size_t g_park_len, g_park_cap;
static long  g_park_recs;
/* …AND THE RENDERED TRANSPORT, rebuilt from the records on every ask rather than accumulated beside them. Two
   accumulated copies would be two things to keep in step, and result.c asks twice (once to size its buffer,
   once to fill it) — so this is idempotent by construction: the same records render the same bytes. */
static char *g_park_json;

static void park_reserve(size_t n)
{
    if (g_park_len + n + 1 > g_park_cap) {
        size_t nc = g_park_cap ? g_park_cap : 256;
        while (g_park_len + n + 1 > nc) nc *= 2;
        g_park = realloc(g_park, nc);
        /* A CHECK RATHER THAN A DCHECK, and in the one place where that reads backwards: this runs BECAUSE the
           host is out of RAM. A park that cannot allocate its own document has lost the whole residue it was
           called to save — every parked flow, silently, in exactly the situation the cold tier exists for —
           and there is no version of proceeding past it. The document is a few hundred KiB against a frontier
           of hundreds of MiB, which is the whole reason paging pays for itself. */
        CHECK(g_park, "the cold tier could not allocate its own park document — the entire suspended frontier "
                      "would be dropped at the moment it was being saved");
        g_park_cap = nc;
    }
}

static void park_raw(const char *s, size_t n)
{
    park_reserve(n);
    memcpy(g_park + g_park_len, s, n);
    g_park_len += n;
    g_park[g_park_len] = 0;
}

static void park_str(const char *s) { park_raw(s, strlen(s)); }

/* ONE RECORD BEGINS. The separator is written BEFORE the record rather than after it, so the document never
   carries a trailing byte a later append has to reach back and unwrite — the property the rendered closing
   bracket used to buy, now true of the accumulation itself. */
static void park_open_rec(void) { if (g_park_recs++) park_str(";"); }

/* ONE RECORD. The charset is asserted rather than escaped: a record is built from digits, the two arm
   characters and the punctuation below, so nothing in it can need JSON escaping on the way to the transport
   and nothing in it can be the ';' THIS FILE joins records with. An escape path here would be a second encoder
   for text this file produces itself — and the failure it would hide (a record that splits into two on the way
   back) is exactly the one that must never be silent. */
static void park_rec(const char *s)
{
    const char *p;

    for (p = s; *p; p++)
        DCHECK((*p >= '0' && *p <= '9') || *p == ',' || *p == '-' || *p == '+' || *p == '.' ||
               *p == 'e' || *p == 'E' || *p == 'f',
               "a park record holds a character the recipe grammar does not have — it would need JSON escaping "
               "on the way out, or (a ';') would split into two records on the way back in");
    park_open_rec();
    park_str(s);
}

/* THE ONE RECORD WHOSE LAST FIELD IS TEXT THIS FILE DID NOT COMPOSE OUT OF DIGITS — a discovery flow's whole
   identity is the address it probes (solver/discovery.h), so the address has to cross. It gets its OWN charset
   rather than widening park_rec's for every record, because the reason that assert is narrow is that a record
   built from digits can never need JSON escaping and can never contain the ';' this file joins records with. A
   URL can contain neither either — every candidate is minted by discovery.c out of an origin, a well-known path
   and a query — and this states exactly that, so the day one carries a quote, a backslash, a ';' or a byte
   outside printable ASCII the park says so instead of storing a document that splits in two on the way back. */
static void park_rec_url(double val, const char *url)
{
    char head[64];
    const char *p;

    snprintf(head, sizeof head, "d%.17g,", val);
    for (p = url; *p; p++) {
        unsigned char c = (unsigned char)*p;
        DCHECK(c > 0x20 && c < 0x7f && c != '"' && c != '\\' && c != ';',
               "a parked discovery candidate holds a character the recipe grammar does not have — it would "
               "need JSON escaping on the way out, or (a ';') would split into two records on the way back in");
    }
    park_open_rec();
    park_str(head);
    park_str(url);
}

/* ATTACKER TEXT CROSSES AS HEX, and it is the one thing in this document whose charset the grammar cannot
   state. Every other field is composed of digits by this file and park_rec asserts exactly that; park_rec_url
   widens it once by naming what an address cannot contain. A BREAKOUT can contain anything: `';X9()//` holds
   the ';' this file joins records with, an HTML breakout holds the '"' the transport quotes each record with,
   and a payload that survives a filter may be any byte at all. So the choice is a charset PREDICATE that three
   separate consumers would have to be kept in step with — this grammar, the JSON render, and cold_resume's
   split — or an ENCODING with no predicate in it. Hex has none: two characters per byte, both inside every one of
   those alphabets, and park_unhex is the only thing that ever reads one back. It costs a doubling on two short
   fields, which is the price of never having to be right about a set of bytes again. */
static void park_hex(const char *s)
{
    static const char H[] = "0123456789abcdef";
    const unsigned char *p;

    for (p = (const unsigned char *)s; *p; p++) {
        char pair[2];
        pair[0] = H[*p >> 4];
        pair[1] = H[*p & 15];
        park_raw(pair, 2);
    }
}

static int park_hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;   /* including upper case: this file writes lower, so anything else is not our document */
}

/* …AND BACK, as a malloc'd NUL-terminated string the caller owns. It is the decoder's job to refuse a field
   the encoder cannot have produced, because the failure it would otherwise cause is silent: half a breakout is
   a string that injects nothing and reports as a candidate that did not fire. */
static char *park_unhex(const char *p, const char *end)
{
    size_t n = (size_t)(end - p), i;
    char *out;

    DCHECK(n > 0 && (n & 1) == 0,
           "a parked @S candidate's text is empty or has an odd number of hex digits — it was truncated on the "
           "way out or split on the way back, and half a breakout substitutes nothing while still reporting as "
           "a candidate that was tried");
    out = malloc(n / 2 + 1);
    CHECK(out, "the cold tier could not rebuild a parked candidate's text");
    for (i = 0; i < n; i += 2) {
        int hi = park_hexval(p[i]), lo = park_hexval(p[i + 1]);
        DCHECK(hi >= 0 && lo >= 0,
               "a parked @S candidate's text is not lower-case hex — this file wrote it and nothing else may, "
               "so the document did not come from this engine's park");
        out[i / 2] = (char)((hi << 4) | lo);
    }
    out[n / 2] = 0;
    return out;
}

/* AN @S CANDIDATE SESSION'S RECIPE. It is an 'f' record — a flow standing on a decision chain, carrying its
   reward — PLUS the three things that make it a candidate rather than an exploration flow: the SOURCE its
   payload replaces, the PAYLOAD, and the SINK CLASS its fire is recorded against. It needs both halves: a
   candidate is an ordinary member of the one frontier, so it can branch and be preempted like anything else,
   and a record holding only the substitution would resume it from the baseline having lost every arm it took.
   THE PAYLOAD CROSSES AS BYTES, NEVER AS AN INDEX. Today solve.c seeds from fixed literal tables, so a
   candidate's payload is one of a small closed vocabulary and an ordinal into that table would round-trip
   perfectly and be shorter. It is still the wrong record: §@S says the breakout is DERIVED from the real parse
   context, and the day that derivation lands the payload becomes a computed string and every index written by
   an older session names a row that no longer exists — silently, because an index is always a valid index.
   Text costs a hex doubling of a few dozen bytes and is correct in both worlds.
   THE SINK CROSSES AS ITS NAME, because `cand_sink` is a pointer into solve.c's static table and a pointer has
   no identity outside the session that minted it. solve_resume_candidate is what binds the name back to that
   table's own pointer, which is what keeps solve.c's identity compare exact on a resumed flow.
   AND TWO CANDIDATE FIELDS DELIBERATELY DO NOT CROSS, which is a decision and not an omission:
     - `cand_verifying` is not state at all. solve_flow_begin sets it from `cand_src` on EVERY switch-in, so it
       is a restatement of "this flow has a candidate" that the scheduler re-derives before the flow runs an
       opcode. Carrying it would be carrying a duplicate of a field already here.
     - `cand_fired` is dropped ON PURPOSE, and dropping it is what keeps a finding an OBSERVATION. A candidate
       can fire mid-run and be preempted before it finishes, so a parked one may well be carrying the bit. If
       it crossed, solve_flow_end would record a PoC on the strength of a fire THIS session never saw — and a
       replay does not have to reproduce it, because §Time-travel says a resumed flow re-derives its values
       from CURRENT sources and a fire that depended on a reply may simply not recur. §@S is explicit that only
       firing proves it; a carried bit is a claim. Re-run, and re-observe, or report nothing. */
static void park_rec_cand(const Flow *f, long id)
{
    char head[96];
    const char *p;

    DCHECK(f->cand_src && *f->cand_src && f->cand_payload && *f->cand_payload && f->cand_sink,
           "an @S candidate was parked missing part of its substitution — the source, the payload and the sink "
           "class are one identity, and a record holding two of the three resumes a flow that injects nothing "
           "or cannot say what it fired");
    DCHECK(f->disc_url == NULL,
           "a flow is both an @S candidate and a discovery probe — those are two different identities seeded "
           "by two different places, and whichever record kind is written would drop the other one");
    /* THE SINK NAME IS THE ONE VARIABLE FIELD LEFT IN THE GRAMMAR'S OWN CHARSET, so it is asserted rather than
       hexed: it comes from solve.c's closed table and stays readable at the one point a human ever looks at
       this document. A ',' in it would move every field after it; a ';' would split the record in two. */
    for (p = f->cand_sink; *p; p++)
        DCHECK((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z'),
               "a sink class name holds something other than letters — it is a field of the recipe grammar, so "
               "a ',' in it shifts every field after it and a ';' splits the record in two");
    if (id < 0) snprintf(head, sizeof head, "c-,%.17g,", f->val);
    else        snprintf(head, sizeof head, "c%ld,%.17g,", id, f->val);
    park_open_rec();
    park_str(head);
    park_str(f->cand_sink);
    park_str(",");
    park_hex(f->cand_src);
    park_str(",");
    park_hex(f->cand_payload);
}

/* THE SEGMENT ORDINALS — the cross-tier NAME of a frozen decision segment, valid only inside one park document
   (which is the whole of what has to agree about it). The name lives ON THE SEGMENT (decide.h's
   decide_seg_park_id), and the only thing kept here is which ordinal comes next.
   IT USED TO BE A POINTER-KEYED HASH INDEX HERE, and that is a table this file must not own once the park can
   run more than once in a session: a partial park RELEASES the tail it just wrote, so a segment nothing stands
   on is freed between two parks and the allocator may hand its address back for a segment on an entirely
   different path. The index would answer the new one with the dead one's ordinal — every flow standing on it
   resuming onto a path nothing ever took, with no crash anywhere to say so. An identity that dies with the
   thing it identifies cannot be re-used, so the fifty lines of open addressing are gone rather than guarded. */
static long g_park_segs;   /* the next ordinal — dense and ascending across the WHOLE document */

/* WHAT THIS SESSION'S PARK DOCUMENT HOLDS, PER KIND — see cold.h for why a total cannot answer the question a
   reader of a round trip is actually asking. Counted at the WRITE, so it cannot disagree with the records. */
static ColdParked g_parked_census;

void cold_parked(ColdParked *out)
{
    DCHECK(out != NULL, "the cold tier was asked to report a park into nothing");
    *out = g_parked_census;
}

/* EMIT `seg` AND EVERY UNEMITTED SEGMENT BELOW IT, base first, and answer its ordinal. Iterative and not
   recursive for the reason every other walk of these chains is: the chain's depth is the fork depth, an
   unknown-length walk makes that as deep as the walk is long, and the C stack cannot be parked. */
static const void **g_walk; static long g_walk_n, g_walk_cap;

static long park_emit_chain(const void *seg)
{
    const void *s;
    long id = decide_seg_park_id(seg);

    if (id >= 0) return id;                     /* a sibling already wrote this prefix — that IS the sharing */
    g_walk_n = 0;
    for (s = seg; s && decide_seg_park_id(s) < 0; s = decide_seg_base(s)) {
        if (g_walk_n >= g_walk_cap) {
            g_walk_cap = g_walk_cap ? g_walk_cap * 2 : 64;
            g_walk = realloc(g_walk, (size_t)g_walk_cap * sizeof *g_walk);
            CHECK(g_walk, "the cold tier could not grow its chain walk — the park would write a segment whose "
                          "base it never wrote");
        }
        g_walk[g_walk_n++] = s;
    }
    while (g_walk_n) {                          /* pop deepest-first: a base is always written before its user */
        const void *cur = g_walk[--g_walk_n];
        const void *base = decide_seg_base(cur);
        const signed char *arms;
        int n = decide_seg_arms(cur, &arms), k;
        long bid = base ? decide_seg_park_id(base) : -1;
        char head[64];

        DCHECK(!base || bid >= 0,
               "a decision segment is being written before the one it stands on — the rebuild is a single "
               "forward pass, so a base that has no ordinal yet is a chain this walk descended wrongly");
        snprintf(head, sizeof head, "s%ld,", g_park_segs);
        park_open_rec();
        park_str(head);
        if (bid < 0) park_str("-");
        else { char b[32]; snprintf(b, sizeof b, "%ld", bid); park_str(b); }
        park_str(",");
        for (k = 0; k < n; k++) {
            char c;
            DCHECK(arms[k] == 0 || arms[k] == 1,
                   "a decision slot holds something that is not an arm — the vector records 0 or 1 per branch, "
                   "and anything else resumes a flow onto a path it never took");
            c = (char)('0' + arms[k]);
            park_raw(&c, 1);
        }
        decide_seg_set_park_id(cur, g_park_segs++);
        g_parked_census.segs++;
    }
    id = decide_seg_park_id(seg);
    DCHECK(id >= 0, "a decision chain was walked and the segment it started from still has no ordinal");
    return id;
}

/* PARK ONE FLOW — the primitive, and the whole-frontier park below is the loop over it. See cold.h. */
void cold_park_flow(Flow *f)
{
    const void *seg;
    long id;
    char rec[64];

    DCHECK(f != NULL, "the cold tier was asked to write the recipe of no flow at all");
    /* THE RUNNING FLOW'S PATH IS NOT IN ITS BLOB. decide.c keeps the live flow's evolving vector in its own
       globals and only a suspend freezes it into the chain, so a park taken with this flow still switched in
       would write its recipe from the chain it stood on at its LAST suspend — every arm it has taken since, and
       every sibling those arms imply, silently absent from the path it resumes on. Asked PER FLOW rather than
       of the scheduler, because a partial park writes a tail while a different flow legitimately holds the
       thread; what may not be written is the one flow whose state is not in its own blob. */
    DCHECK(f != flow_running(),
           "the flow the scheduler is switched INTO was written to the park document — its decision state is "
           "live in decide.c and not in its blob, so its recipe would be short every arm it has taken since its "
           "last suspend");
    /* WRITTEN ONCE. The host stores ONE value per bundle, so a flow named twice in it comes back as TWO flows
       standing on the same path — a duplicate of itself that re-forks every branch it already took, growing the
       frontier on every visit. This is the once-per-session assert the whole-frontier park used to make about
       the DOCUMENT (`g_park == NULL`), said about the FLOW instead: the document is appended to now, and it is
       the flow that may not be in it twice. */
    DCHECK(!f->paged,
           "a flow was written to the park document twice — the host stores one value per bundle, so the second "
           "record resumes it as a SECOND flow standing on the same path, which re-forks every branch it has "
           "already taken and grows a duplicate of the frontier on every visit");
    /* WHAT A RECIPE CANNOT CARRY IS ASSERTED HERE, at the one point where it would otherwise be written
       out wrong — never softened into a flow that resumes as something else. An @S CANDIDATE SESSION used to
       be on this list; it is a record kind now (park_rec_cand below), which is what the crash asked for. */
    DCHECK(f->deliver == NULL,
           "a flow holding a routed cross-document record was parked — the peer's message is not in its "
           "recipe and no replay can invent one, so it is dropped exactly as it would be at a finish. A "
           "delivery is a work item on the one frontier: park it as one (the record and the sender origin "
           "the trusted zone stamped are both text) or refuse the park while one is outstanding");
    /* AND THE OTHER HALF OF THAT SHAPE, WHICH OWES AN ANSWER. flow.h says a cross-agent operation "is the same
       shape as the delivery above and for the same reason, with one thing added" — the asking instance's flow
       is SUSPENDED on the answer this one's program will produce. Dropping the record therefore does not merely
       lose this flow's work: it parks a flow in ANOTHER instance forever, on a completion nothing will ever
       send. The delivery was asserted here and this was not, which is the asymmetry a reader of the pair would
       have to notice for themselves. */
    DCHECK(f->perform == NULL && f->answer_token == NULL,
           "a flow holding a CROSS-AGENT OPERATION was parked — the record and the rendezvous token are not in "
           "its recipe, so the operation is dropped and the flow that ASKED for it, in another instance, stays "
           "suspended on an answer nothing will ever send. Park it as what it is: the record and the token are "
           "text and cross as text, and the answer is the program's COMPLETION, so a resumed flow re-queues the "
           "program and answers the same token");
    DCHECK(!f->started || f->dec_blob != NULL,
           "a flow that has RUN was parked with no suspended decision state — its path is unrecorded, so "
           "its record would name no segment and it would resume as a from-baseline flow, re-exploring the "
           "un-forked path and never reaching the branch it was suspended past");

    seg = f->dec_blob ? decide_blob_seg(f->dec_blob) : NULL;
    id = seg ? park_emit_chain(seg) : -1;
    /* THE REWARD TRAVELS WITH THE FLOW because the ONE global frontier is ordered by it across sessions —
       §scheduler's "a high-value flow suspended last week resumes ahead of a low-value fresh one today".
       It is what the flow EMITTED, which is history and not a live value, so it is the one number here
       that is neither re-derived nor re-fetched, and it is what breaks the tie at the instant a resumed
       frontier starts: every member is then unrun (cpu 0, full optimism bonus), so without it the WFQ
       would pick by registry position and call that value-of-information.
       A REPLAY RE-EMITS, so a resumed flow is credited again for findings it is carrying credit for. That
       is a real inflation and it is deliberately not corrected here: it applies to every resumed flow in
       proportion to what it emitted, so the ORDER — which is the only thing a weight decides — is the one
       this number exists to preserve. Correcting it would mean deciding which of this session's emissions
       were "the same finding" as last session's, which is a question about the SERVER's surface today, not
       about the flow. */
    DCHECK(f->val == f->val && f->val >= 0.0 && f->val < 1e300,
           "a flow was parked with a reward that is not a number the WFQ can order by — a NaN compares "
           "false in both directions, so the resumed frontier's order would depend on array position, and "
           "an infinity does not survive the round trip as a number at all");
    /* A DISCOVERY PROBE PARKS AS ITS ADDRESS, and that is the whole recipe because that is the whole flow: it
       stands on no decision, holds no delta worth the name and has one thing left to do — read the document at
       that URL. A resumed one re-issues the GET, which is not a weaker resume than the replay every other
       record gets: §Time-travel-resume says a resumed flow "re-derives example VALUES from CURRENT sources", and
       for this flow the fetch IS the work, so the residue comes back reading TODAY's published surface. */
    if (f->disc_url) {
        DCHECK(!f->started && f->dec_blob == NULL,
               "a discovery probe was parked carrying a decision path — a probe compiles no program and so can "
               "reach no branch, which means this flow ran something it has no business running and its record "
               "would resume it as a probe with a path nothing will replay");
        park_rec_url(f->val, f->disc_url);
        g_parked_census.probes++;
    } else if (f->cand_src) {
        /* AN @S CANDIDATE PARKS AS ITS SUBSTITUTION AND ITS PATH — see park_rec_cand. It is asked about after
           the probe and before the plain flow because those three are the whole of what a flow's identity can
           be, and each pair is asserted disjoint where it is written rather than left to this order. */
        park_rec_cand(f, id);
        g_parked_census.cands++;
    } else {
        if (id < 0) snprintf(rec, sizeof rec, "f-,%.17g", f->val);
        else        snprintf(rec, sizeof rec, "f%ld,%.17g", id, f->val);
        park_rec(rec);
        g_parked_census.flows++;
    }
    f->paged = 1;   /* its recipe exists: flow_release may now let its parked continuation go (flow.h) */
}

void cold_park(void)
{
    Flow *f;
    int i, wseg = 0, wsegf = 0;

    /* AN EMPTY FRONTIER WRITES NOTHING, AND NOTHING IS A POSITIVE ANSWER — "[]" tells the host this document is
       fully explored and DELETES its cold entry. That is the right answer for a frontier that drained and the
       wrong one for a park the host asked for, so the two are separated where they differ rather than where
       they are read. */
    DCHECK(flow_count() > 0,
           "the host asked this engine to page out a frontier with no members — an empty park document is how a "
           "fully-explored document deletes its cold entry, so this stores 'nothing left to do' over whatever "
           "residue the origin had");
    world_segment_stats(&wseg, &wsegf);
    /* ANOTHER INSTANCE'S FLOW STATE LIVES HERE, AND IT HAS NO RECIPE. A foreign world's segment is a peer
       document's timeline materialized in this instance; it belongs to a flow this frontier does not contain,
       so no record written below names it and no replay of ours re-derives it. It is asked HERE and not in the
       per-flow primitive because it is a fact about this ENGINE leaving memory, which is what a whole-frontier
       park is and what a partial one is not. */
    DCHECK(wseg == 0,
           "the frontier was parked while this instance holds a segment of a FOREIGN world — that is a peer "
           "document's flow state living here, no record below names it, and paging this engine out drops it. "
           "Build the cross-instance park: a foreign segment travels with the WORLD's name (solver/world.h), "
           "not with this document's flows, and the offscreen is what re-routes it to the instance that "
           "rebuilds the peer");
    for (i = 0; (f = flow_at(i)) != NULL; i++) cold_park_flow(f);
}

const char *cold_park_recipes(void)
{
    /* THE DOCUMENT ITSELF, in the one language this file both writes and reads. A host that can store a string
       stores this and hands it straight back to engine_sched_begin; nothing between the two translates, so
       there is no second grammar for the round trip to disagree across. "" is the positive answer for a
       frontier that drained, exactly as "[]" is on the transport side — engine_sched_begin reads an empty one
       as "no residue" and seeds a boot flow instead. */
    return g_park ? g_park : "";
}

long cold_park_records(void) { return g_park_recs; }

const char *cold_park_json(void)
{
    const char *p;
    char *o;
    size_t need;

    if (!g_park) return "[]";
    /* THE TRANSPORT, RENDERED FROM THE RECORDS. Each ';' becomes `","` (+2 bytes), and the whole is wrapped in
       `["` … `"]` (+4) with a NUL — a bound, not a guess, so the write below cannot run past its buffer.
       Idempotent because it is a pure function of the records: result.c asks once to size its own buffer and
       once to fill it, and the two must be the same bytes or it truncates the document. */
    need = g_park_len + 2 * (size_t)g_park_recs + 8;
    g_park_json = realloc(g_park_json, need);
    CHECK(g_park_json, "the cold tier could not render the park document for the host to store — the residue "
                       "exists and every flow in it would be dropped at the boundary that was to save it");
    o = g_park_json;
    *o++ = '['; *o++ = '"';
    for (p = g_park; *p; p++) {
        if (*p == ';') { *o++ = '"'; *o++ = ','; *o++ = '"'; continue; }
        /* THE ESCAPE CLAIM, ASSERTED WHERE THE JSON IS MADE. Four writers each state what their own fields may
           hold (park_rec's digits, park_rec_url's address bytes, park_rec_cand's sink letters, park_hex's two
           hex characters), and the property they exist to guarantee is this one: no record needs escaping, so
           no escape path is needed and a byte that would have needed one is a writer that got its charset
           wrong. Said once, here, rather than trusted four times over. */
        DCHECK(*p != '"' && *p != '\\' && (unsigned char)*p >= 0x20 && (unsigned char)*p < 0x7f,
               "a park record holds a byte that would need JSON escaping — the recipe grammar exists so that "
               "cannot happen, so one of the record writers accepted a field it should have refused and the "
               "host is about to store a value that will not parse");
        *o++ = *p;
    }
    *o++ = '"'; *o++ = ']'; *o = 0;
    DCHECK((size_t)(o - g_park_json) < need, "the rendered park document overran the bound its own records set");
    return g_park_json;
}

/* ONE FIELD SEPARATOR, asked for where it is required. A record that does not have it is a record this file
   did not write, and reading past it would silently take the NEXT field's digits as this one's. */
static const char *park_comma(const char *p)
{
    DCHECK(*p == ',', "a park record is missing a field separator — the record was not written by this engine, "
                      "or it was split by a character the grammar forbids");
    return p + 1;
}

/* ONE REBUILT FLOW, LANDED AND CHECKED. Every record kind that names a flow goes through this, because the
   thing that has to be true of all of them is the same thing and it is §scheduler's razor: a resume APPENDS.
   It was three copies of these four lines when there were three kinds, which is exactly where a fourth kind
   drops the assert and nothing says so. */
static Flow *park_flow_add(JSContext *ctx, double val, int before, long flows)
{
    Flow *fl = flow_add(ctx, JS_UNDEFINED, WORLD_NONE);
    /* IT WENT ON THE END, WHICH IS THE HALF OF THE MERGE A LIVE FRONTIER CARES ABOUT. A rebuilt flow must be
       an addition and never a substitution: the registry appends, so this one belongs at `before` plus however
       many this document has rebuilt so far. If a member were ever displaced — a reorder, a swap-remove, a
       registry that reused a slot — the flow that lived there would be gone from the frontier with nothing
       else in the engine able to say so. Asked at the add rather than counted at the end, so the assert names
       the record that did it. */
    DCHECK(flow_at((int)(before + flows)) == fl,
           "a rebuilt flow did not land at the end of the frontier — a resume APPENDS, so anything else means "
           "a live member was displaced by a parked one and is now unreachable");
    fl->val = val;
    return fl;
}

/* WHAT THE LAST REBUILD PRODUCED, PER RECORD KIND. `@RESUMED <n>` is printed for the extension, which reads it
   off stdout because a line of text is the only channel that zone has into a wasm instance — and one total
   cannot say WHICH arms of the grammar ran. That distinction is the whole of what a resume has to prove here:
   a residue of nothing but 'f' records exercises neither park_unhex nor solve_resume_candidate nor the probe
   rebuild, and a run reporting `@RESUMED 4` looks identical either way. An in-process host asks this instead of
   parsing the engine's own stdout. */
static ColdResumed g_resumed;

void cold_resumed(ColdResumed *out)
{
    DCHECK(out != NULL, "the cold tier was asked to report a rebuild into nothing");
    *out = g_resumed;
}

void cold_resume(JSContext *ctx, const char *recipes)
{
    void **seg = NULL;
    long seg_n = 0, seg_cap = 0, flows = 0, k;
    const char *p = recipes;
    const int before = flow_count();

    DCHECK(recipes != NULL && *recipes != '\0',
           "the cold tier was asked to resume an empty park document — a document with no residue DELETES its "
           "cold entry, so a caller reaching here has confused 'fully explored' with 'paged out'");
    memset(&g_resumed, 0, sizeof g_resumed);
    /* THIS IS AN APPEND, NOT A SEED, and the two used to be one act. It asserted the frontier was EMPTY and
       assigned its park-local ordinals into it as though the document were the whole of what exists — which is
       true of the session-start rebuild and false by construction of the thing this tier is for: a PARTIAL
       self-park writes the lowest-value TAIL, and that tail comes back to a frontier still holding every flow
       that was NOT paged out, possibly several times over a session.
       THE SEEDING CLAIM DID NOT DISAPPEAR, IT MOVED TO THE PLACE THAT MAKES IT. "The boot flow and the residue
       are alternatives" is a statement about STARTING a session, so engine_sched_begin asserts the frontier is
       empty and then chooses one of the two; a rebuild has no business asserting anything about flows it did
       not create.
       NOTHING LIVE IS RENUMBERED, and the reason is structural rather than checked: the ordinals in the
       document name segments in a table THIS CALL builds (`seg` below), every entry of which decide_seg_new
       has just allocated. There is no lookup by pointer anywhere in the rebuild, so no live flow's segment can
       be reached by an ordinal, and no live flow is read or written at all. What IS checked is the frontier's
       shape either side of the merge: every rebuilt flow lands at the end, and the count grows by exactly the
       number rebuilt. */

    while (*p) {
        const char *end = strchr(p, ';');
        const char *q;
        char *ep;
        char kind = *p;

        if (!end) end = p + strlen(p);
        q = p + 1;
        if (kind == 's') {
            long id, bid;
            signed char *arms;
            int n;

            id = strtol(q, &ep, 10); q = ep;
            q = park_comma(q);
            if (*q == '-') { bid = -1; q++; }
            else { bid = strtol(q, &ep, 10); q = ep; }
            q = park_comma(q);
            n = (int)(end - q);
            DCHECK(id == seg_n,
                   "a park document names its segments out of order — the ordinals are dense and ascending in "
                   "emission order, and a gap means a segment nothing wrote is about to be stood on");
            DCHECK(bid < id,
                   "a decision segment stands on one that has not been read yet — a base is written before "
                   "every user of it, so this document was not written by this engine's park");
            DCHECK(n > 0,
                   "a park document holds a decision segment with no arms — the chain never freezes an empty "
                   "head, so every flow standing on this one would replay its path one decision short");
            arms = malloc((size_t)n);
            CHECK(arms, "the cold tier could not rebuild a parked decision segment's arms");
            for (k = 0; k < n; k++) {
                DCHECK(q[k] == '0' || q[k] == '1',
                       "a park document holds something that is not an arm — a decision is 0 or 1, and "
                       "anything else resumes a flow onto a path it never took");
                arms[k] = (signed char)(q[k] - '0');
            }
            if (seg_n >= seg_cap) {
                seg_cap = seg_cap ? seg_cap * 2 : 64;
                seg = realloc(seg, (size_t)seg_cap * sizeof *seg);
                CHECK(seg, "the cold tier could not grow its rebuilt-segment table");
            }
            seg[seg_n++] = decide_seg_new(bid >= 0 ? seg[bid] : NULL, arms, n);
            free(arms);
        } else if (kind == 'f') {
            long sid;
            double val;
            Flow *fl;

            if (*q == '-') { sid = -1; q++; }
            else { sid = strtol(q, &ep, 10); q = ep; }
            q = park_comma(q);
            val = strtod(q, &ep); q = ep;
            DCHECK(sid < seg_n,
                   "a parked flow stands on a decision segment this document never wrote — it would resume "
                   "from the baseline instead of from its own path");
            /* A RESUMED FLOW IS NOT A THIRD KIND OF FLOW. It is `started` — it stands on a recorded chain, so
               the scheduler RESUMES it — with no frame and cursor 0, which is what makes the resume a REPLAY:
               it re-runs the document from its first script, consumes one recorded arm at each branch it
               re-reaches, and forks like any other flow the moment the recipe runs out. Its constraint is
               empty because it re-derives every pin and every decided predicate from the gates it replays.
               ITS WORLD IS A ROOT, not a child of whatever it was forked from, and that is sound for exactly
               the reason the park asserts at its own end: a world's ancestry exists so ANOTHER INSTANCE can
               materialize this flow's segment by forking the nearest ancestor it already holds, and a park is
               refused while any foreign segment lives here. Nothing across the tier is holding an edge into
               this frontier, so there is no edge to carry. When cross-instance park is built, the world NAME
               is what will travel — never this document's flow ancestry. */
            fl = park_flow_add(ctx, val, before, flows);
            fl->started = 1;
            fl->dec_blob = decide_blob_new(sid >= 0 ? seg[sid] : NULL);
            fl->pin_blob = concolic_pins_blob_empty();
            flows++; g_resumed.flows++;
        } else if (kind == 'c') {
            /* AN @S CANDIDATE SESSION COMES BACK AS ITS SUBSTITUTION AND ITS PATH — see park_rec_cand for what
               crosses and, just as load-bearing, which two candidate fields deliberately do not. Everything the
               'f' arm does is done here too, because a candidate IS a flow with a path; the three extra fields
               are what make the replay inject rather than explore. */
            long sid;
            double val;
            Flow *fl;
            const char *sb, *xb;
            char sname[32];
            size_t sl;

            if (*q == '-') { sid = -1; q++; }
            else { sid = strtol(q, &ep, 10); q = ep; }
            q = park_comma(q);
            val = strtod(q, &ep); q = ep;
            q = park_comma(q);
            for (sb = q; q < end && *q != ','; q++)
                ;
            sl = (size_t)(q - sb);
            DCHECK(sl > 0 && sl < sizeof sname,
                   "a parked @S candidate names no sink class, or one longer than any this engine has — the "
                   "name is solve.c's own table text and is re-bound through it, so anything else is a record "
                   "this writer did not produce");
            memcpy(sname, sb, sl);
            sname[sl] = 0;
            q = park_comma(q);
            for (xb = q; q < end && *q != ','; q++)
                ;
            DCHECK(sid < seg_n,
                   "a parked @S candidate stands on a decision segment this document never wrote — it would "
                   "resume from the baseline instead of from the path its search had reached");
            fl = park_flow_add(ctx, val, before, flows);
            fl->cand_src = park_unhex(xb, q);
            q = park_comma(q);
            fl->cand_payload = park_unhex(q, end);
            /* THE SINK BINDS BACK TO THE TABLE'S OWN POINTER, and the same call re-registers the sink as
               pending-and-tried — solve.h says why those are one call and not two. `cand_verifying` is not set
               here: solve_flow_begin sets it from `cand_src` on the switch-in, before this flow runs an
               opcode. `cand_fired` is not set here either, and that is the whole point — the replay has to
               observe the fire again or nothing is recorded. */
            fl->cand_sink = solve_resume_candidate(fl->cand_src, sname);
            /* AND IT IS REBUILT EXACTLY AS AN 'f' IS, including a `-` segment. The temptation here is to say
               that `-` means "never scheduled" and leave such a candidate un-started, the way
               solve_seed_candidates leaves a fresh one — but the record CANNOT distinguish that from a flow
               that ran, took no branch, and froze an empty chain, and both write `-`. Inventing the
               distinction in this arm alone would make one record kind answer a question the grammar does not
               ask, and would do it differently from the kind it is otherwise identical to. The two are the
               same state anyway: decide_resume over an empty chain and decide_enter both start a replay at
               cursor 0 with no arms and no pins (the empty pin blob is what makes the second half of that
               true), so there is nothing between them to get wrong. */
            fl->started = 1;
            fl->dec_blob = decide_blob_new(sid >= 0 ? seg[sid] : NULL);
            fl->pin_blob = concolic_pins_blob_empty();
            flows++; g_resumed.cands++;
        } else if (kind == 'd') {
            /* A DISCOVERY PROBE COMES BACK AS ITS ADDRESS. It is NOT `started`: it stands on no path, so there
               is nothing to replay — its first step parks on the URL exactly as the session that seeded it did,
               and the document it reads is today's. The candidate is the last field and runs to the record's
               end, which is what lets an address carry the ',' a query legitimately can. */
            double val;
            Flow *fl;
            size_t ul;

            val = strtod(q, &ep); q = ep;
            q = park_comma(q);
            ul = (size_t)(end - q);
            DCHECK(ul > 0,
                   "a parked discovery probe names no candidate address — the address IS the flow, so this "
                   "record would resume a member of the frontier with nothing whatever to do");
            fl = park_flow_add(ctx, val, before, flows);
            fl->disc_url = malloc(ul + 1);
            CHECK(fl->disc_url, "the cold tier could not rebuild a parked discovery candidate");
            memcpy(fl->disc_url, q, ul);
            fl->disc_url[ul] = 0;
            flows++; g_resumed.probes++;
        } else {
            DFAIL("a park record names a kind this grammar does not have — the recipe holds SEGMENTS ('s'), "
                  "FLOWS ('f'), @S CANDIDATE SESSIONS ('c') and DISCOVERY PROBES ('d') and nothing else, so "
                  "this document came from another writer or was truncated");
        }
        p = (*end == ';') ? end + 1 : end;
    }
    /* THE REBUILDER'S REFERENCES GO BACK, leaving every segment held by the flows that stand on it and by the
       segments above it. One that nothing references is freed here — which is the honest outcome for a
       document holding a prefix whose only user did not survive to be written. */
    for (k = 0; k < seg_n; k++) decide_seg_release(seg[k]);
    free(seg);
    DCHECK(flows > 0,
           "a parked frontier rebuilt no flows at all — the document held segments with nobody standing on "
           "them, so the whole residue it was written to save is unreachable");
    /* THE OTHER SIDE OF THE MERGE, AND IT IS THE RAZOR ITSELF: "a resume that drops, starves, skips, reorders
       or forgets ANY flow is a CAP". Every record that named a flow produced one, and every member that was
       already here is still here — one equality says both, because the registry only ever appends and the
       per-record assert above proved each addition landed past the live members. */
    DCHECK(flow_count() == before + (int)flows,
           "the frontier did not grow by exactly the flows this document rebuilt — a parked flow was dropped on "
           "the way in, or a live one left while the residue was landing, and either way the union of every "
           "flow from every session is no longer what the frontier holds");
    /* THE ONE OBSERVABLE THAT THE RESUME RAN, and it had a READER and no WRITER. `extension/bridge.js` parses
       `@RESUMED <n>` and reports it in the per-run engine log beside the park count, so that "the full
       park->persist->rehydrate->resume SEQUENCE across all engines is observable" — its words. Nothing has
       ever printed it, so that field was a zero for every session there has ever been, and a rehydration that
       silently rebuilt nothing would have looked exactly like one that rebuilt four thousand flows. It is the
       same defect the file's own comment records one field over (`_orphans`, `_work`, `_parked`: "which
       nothing on the engine side has ever written, so the whole diagnostic reported three zeroes forever").
       Printed here rather than counted into the result document because the result is built when the session
       ENDS and this is a fact about how it BEGAN — a session that goes on to crash still has to have said that
       it resumed, or the residue looks like it was never read back. */
    /* …AND THE SAME FACT DECOMPOSED, for a host that can ask rather than read. The sum is asserted against the
       count the merge itself kept, so an arm added to the grammar without a counter is caught here instead of
       silently reporting a kind that ran as one that did not. */
    g_resumed.segs = seg_n;
    DCHECK(g_resumed.flows + g_resumed.cands + g_resumed.probes == flows,
           "the rebuild's per-kind census does not add up to the flows it landed — a record kind produced a "
           "flow without counting itself, so a host asking which arms of the grammar ran is told one did not");
    printf("@RESUMED %ld\n", flows);
    fflush(stdout);
}

void cold_free(void)
{
    free(g_park); g_park = NULL; g_park_len = g_park_cap = 0; g_park_recs = 0;
    free(g_park_json); g_park_json = NULL;   /* the transport is a render of the records; it dies with them */
    /* THE ORDINAL COUNTER GOES BACK TO ZERO WITH THE DOCUMENT IT NUMBERED, and the names it handed out died
       with the segments (decide.c's `park_id`), which flow_registry_free has just asserted are all gone. */
    g_park_segs = 0;
    memset(&g_parked_census, 0, sizeof g_parked_census);   /* the census is OF the document; it dies with it */
    free(g_walk); g_walk = NULL; g_walk_n = g_walk_cap = 0;
}
