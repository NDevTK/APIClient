/* The cold tier — the snapshot census, and the recipe that crosses the tier. See cold.h. */
#include "solver/cold.h"
#include "solver/flow.h"
#include "solver/cow.h"
#include "solver/dom_cow.h"
#include "solver/decide.h"
#include "solver/concolic.h"
#include "solver/world.h"
#include "solver/solve.h"     /* …and the @S sink table a parked candidate's class is re-bound through */
#include "solver/pending.h"   /* the session's context, which a flow's JS-valued queues are read through */
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

        out->job_count += flow_job_pending(f);
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
        /* THE ROUTED DELIVERIES ARE NOT COUNTED HERE, and the two `strlen` rows that were are gone with the
           two `char *` they measured. The queue is a JS Array of [record, senderOrigin] pairs now, so its
           bytes are quickjs's and its strings are shared with everything else that names them — the same
           reason the unstarted-operation queue beside it contributes nothing to this walk, and the same rule
           the pending register keeps by asking ITSELF (pending_bytes) rather than restating its shape here.
           THE QUEUED JOBS ARE OFF IT TOO, with the `jobcap * sizeof(FlowJob)` row that used to measure them:
           the queue is a JS Array of records now, so its bytes are quickjs's and its arguments are the same
           values everything else in the heap names. `job_count` above is unchanged and is what the pager
           actually reads. */
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
 * ends, and every one of the resume arms below — the 's'/'c' rebuilds, park_unhex, solve_resume_candidate
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
               *p == 'e' || *p == 'E' || *p == 'f' || *p == 'g',
               "a park record holds a character the recipe grammar does not have — it would need JSON escaping "
               "on the way out, or (a ';') would split into two records on the way back in");
    park_open_rec();
    park_str(s);
}

/* THE WORLD-NAME GENERATION THIS DOCUMENT IS WRITTEN UNDER — see cold.h, and solver/world.h for what a name
   without one does to a peer that never left memory. Written by whichever park runs FIRST, which is why it is
   here and not in cold_park: a PARTIAL self-park writes a tail while the engine keeps running, and the tail is
   as much of a residue as the whole frontier is. Once per document, and first in it, because cold_resume has
   to install the generation before it rebuilds a flow — a flow rebuilt under the old namespace is exactly the
   collision this record exists to prevent, so the ordering is asserted at both ends. */
static int g_park_gen_written;

static void park_gen_rec(void)
{
    char rec[32];

    if (g_park_gen_written) return;
    g_park_gen_written = 1;
    DCHECK(g_park_recs == 0,
           "the generation record is not the first in the park document — every record after it is named under "
           "the generation above it, so a reader that met a flow first would rebuild it in the ended session's "
           "namespace and hand a peer a name it already holds a segment for");
    snprintf(rec, sizeof rec, "g%u", world_session());
    park_rec(rec);
}

/* TEXT THIS FILE DID NOT COMPOSE CROSSES AS HEX, and it is the one thing in this document whose charset the
   grammar cannot state. Every other field is composed of digits by this file and park_rec asserts exactly
   that. Two kinds reach here and neither has a charset: an @S BREAKOUT, and a ROUTED RECORD with the TABs of
   its own transport fields and a peer's base64 payload in it. A BREAKOUT can contain anything: `';X9()//` holds
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
           "a hex field of the park document is empty or has an odd number of digits — it was truncated on the "
           "way out or split on the way back. Half an @S breakout substitutes nothing while still reporting as "
           "a candidate that was tried; half a routed record is a message that decodes to bytes no peer sent");
    out = malloc(n / 2 + 1);
    CHECK(out, "the cold tier could not rebuild a parked candidate's text");
    for (i = 0; i < n; i += 2) {
        int hi = park_hexval(p[i]), lo = park_hexval(p[i + 1]);
        DCHECK(hi >= 0 && lo >= 0,
               "a hex field of the park document is not lower-case hex — this file wrote it and nothing else "
               "may, so the document did not come from this engine's park");
        out[i / 2] = (char)((hi << 4) | lo);
    }
    out[n / 2] = 0;
    return out;
}

/* AN @S CANDIDATE SESSION'S RECIPE. It is an 'f' record — a flow standing on a decision chain, carrying its
   reward — PLUS the four things that make it a candidate rather than an exploration flow: the SOURCE its
   payload replaces, the DELIVERY ROOT those bytes reach the victim by, the PAYLOAD, and the SINK CLASS its
   fire is recorded against. It needs both halves: a candidate is an ordinary member of the one frontier, so it
   can branch and be preempted like anything else, and a record holding only the substitution would resume it
   from the baseline having lost every arm it took.
   THE ROOT IS THE FIELD THIS RECORD WAS MISSING, and it did not degrade — it aborted. A resumed candidate
   opens its sink search through solve_resume_candidate, and a verifying flow does not DETECT, so nothing else
   in the resuming session had a value to read a root off: every emit of that search hit solve.c's
   emit_delivery assert and took the whole @RESULT down with it. In release the same record rendered the
   silence that MEANS "no component carries or transforms these bytes on the way in" over a payload whose
   delivery the ended session knew exactly, and a fire-verified PoC took the NULL with it into the finding.
   IT IS ASKED OF solve.c AND NOT READ OFF THE FLOW, because the root belongs to the SEARCH: it is inherited
   unchanged through every derivation, so one sink's N candidates have one root between them and a copy on each
   Flow would be N owned strings whose only content is that they are equal. The document still writes one per
   record, and that is a different thing — a record is rebuilt alone, so it has to be whole.
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
    const char *root;

    DCHECK(f->cand_src && *f->cand_src && f->cand_payload && *f->cand_payload && f->cand_sink,
           "an @S candidate was parked missing part of its substitution — the source, the payload and the sink "
           "class are one identity, and a record holding two of the three resumes a flow that injects nothing "
           "or cannot say what it fired");
    /* AND THE FOURTH FIELD, which this writer does not hold and asks for — see above for why it lives on the
       search. solve_candidate_root asserts that the search is there and knows its route; this asserts that
       what came back is a field the grammar can carry, so a record short of the whole identity cannot be
       written. */
    root = solve_candidate_root(f->cand_src, f->cand_sink);
    DCHECK(root && *root,
           "an @S candidate was parked with no delivery ROOT — the source, the root, the payload and the sink "
           "class are ONE identity, and a record holding three of the four resumes a flow that fires and then "
           "cannot say how the attacker's bytes reach the victim");
    /* THE DISJOINTNESS OF THE THREE IDENTITIES IS NOT ASSERTED HERE ANY MORE — it moved to park_kind_of, which
       is the one place that CHOOSES between them and is therefore the one place asked about every flow rather
       than only about the flows that already reached this writer. */
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
    /* THE ROOT CROSSES AS HEX FOR park_hex's OWN REASON and not because anybody has looked at what a root
       spells today. It is a source identity like `cand_src` beside it, minted by whichever component declared
       the source, and the grammar's promise is that no field needs a charset predicate three consumers have to
       be kept in step with. Two characters a byte is the price of never being right about a set of bytes. */
    park_hex(root);
    park_str(",");
    /* THE PAYLOAD IS LAST AND STAYS LAST. The 'c' arm reads it to the record's `end` and not to a separator,
       which is what makes it the field a new one goes BEFORE: appended after, its hex would be read as the
       payload's own and every resumed candidate would inject a string no session ever built. */
    park_hex(f->cand_payload);
}

/* A FOREIGN WORLD'S SEGMENT — the one record in this document that names nothing of this frontier. See cold.h
   for the grammar and solver/world.h for why a VECTOR is the whole of such a segment's recipe. It is written
   like park_rec_cand's text fields and not through park_rec: a world vector holds the colons and commas
   world_serialize writes and whatever the host called the root document, so it crosses as hex rather than as a
   fifth charset this grammar has to be kept in step with. */
static void park_rec_world(const char *vector)
{
    DCHECK(vector != NULL && *vector,
           "a foreign segment was written to the park document with no vector — the vector IS the segment's "
           "recipe, so a record without one names a peer timeline the resume cannot rebuild");
    park_open_rec();
    park_str("w");
    park_hex(vector);
}

/* A DRIVEN ORPHAN'S FUNCTION — the one thing about such a flow that a decision vector cannot reproduce, written
 * immediately AFTER the 'f' or 'c' record of the flow it belongs to and bound to it by POSITION, exactly as an
 * unmade delivery is.
 *
 * WHY IT IS NOT A RECORD KIND, which is the shape this looks like it should be. Being a driven orphan is not an
 * ALTERNATIVE to being an exploration flow or an @S candidate session — it is a fact ABOUT one. An orphan drive
 * forked inside a candidate session carries both identities at once (engine_sibling_assemble copies each), so a
 * third arm of park_kind_of's choice would have had to pick one of two things the engine legitimately holds
 * together, and whichever it picked the other would be silently gone. A field bound by position composes: an
 * orphan drive of an ordinary flow is 'f' then 'o', an orphan drive inside a candidate session is 'c' then 'o',
 * and the reader binds each to the flow it last rebuilt.
 *
 * WHAT THE LOCATOR IS AND WHY IT IS NOT A POSITION. Neither name a session already has survives it: the
 * function object is a live heap reference, and the index of the take that produced it is a fact about one heap
 * at one instant. quickjs's JS_OrphanHash composes what the BUNDLE determines instead — the script, the
 * position in that script, and the body's own text — which is stable across sessions for the reason the whole
 * tier rests on: the same bytes compile the same way. An older sketch of this record carried a hash of the
 * SOURCE TEXT alone, and that is the one composition that does not work: a minified bundle repeats
 * `function(e){return e.default}` dozens of times, so the name would have identified a SET and the resume would
 * have driven whichever member of it the heap walk met first.
 *
 * SIXTEEN LOWER-CASE HEX DIGITS, FIXED WIDTH AND SEPARATOR-FREE — the same self-delimiting property that makes
 * a decision slot nine characters, and for the same reason: the field is the whole record, so a reader that
 * finds a different width is holding a document another writer produced and refuses it rather than reading a
 * short name as a valid one. Written like park_rec_cand's text fields and not through park_rec, whose charset
 * is the digit grammar and does not admit the hex letters 'a' through 'd'. */
static void park_rec_orphan(const Flow *f)
{
    static const char H[] = "0123456789abcdef";
    char rec[17];
    int d;

    DCHECK(f->orphan,
           "the cold tier was asked to name the function of a flow that is not a driven orphan — the record "
           "would tell a later session to call something this flow never called");
    /* THE LOCATOR EXISTS, asserted where it is written rather than trusted from where it was stamped.
       engine_orphan_fork stamps it at the one point a drive is created and a fork inherits it with the mark, so
       a drive without one is a third path into being an orphan that nobody has named — and the record it would
       write is sixteen zeros, which is a perfectly valid name for a body that does not exist. */
    DCHECK(f->orphan_hash != 0,
           "a driven orphan was parked with no function locator — its record would name a body no session can "
           "find, so the resumed drive would wait for a function that is never handed to it and finish having "
           "called nothing, which is precisely the silent drop this record exists to prevent");
    for (d = 0; d < 16; d++) rec[d] = H[(f->orphan_hash >> (60 - 4 * d)) & 15u];
    rec[16] = 0;
    park_open_rec();
    park_str("o");
    park_str(rec);
}

/* AN UNMADE ROUTED DELIVERY — one record per [record, senderOrigin] pair still on the flow's queue, written
   immediately AFTER the 'f' or 'c' record of the flow that holds it and belonging to it. A message a peer sent
   is the one work item on this frontier that NO REPLAY can re-derive: the asking side of a host request
   re-issues it and a reaction is re-enqueued by the code that queued it, but nothing in the receiving document
   re-sends someone else's message. So it crosses as what it is — both halves are TEXT, which is exactly what
   the refusal this replaces named as the way out.
   BOUND BY POSITION AND NOT BY AN ORDINAL, unlike the segments: a segment is SHARED (a base is named by every
   chain above it) and so needs a name, while a delivery belongs to exactly one flow and is written with it.
   The reader binds each to the flow it last rebuilt and refuses an 'm' that has no flow before it, which is
   the whole of what the binding can get wrong.
   ORDER IS THE QUEUE'S ORDER, because it is the order the page must observe (HTML §9.3.3 "Posting messages"
   queues each post on the posted message task source, which §8.1.7.1 "Definitions" serializes) — so the
   records are written front-first and the reader appends in the order it reads them.
   BOTH FIELDS CROSS AS HEX for park_hex's own reason: a routed record holds the TABs of its own transport
   fields and a base64 payload, and a serialized origin is whatever origin_serialize wrote — neither is a
   charset this grammar can state, and a ';' in either would split the record in two on the way back. */
static void park_rec_deliver(const char *record, const char *sender_origin)
{
    DCHECK(record != NULL && *record && sender_origin != NULL && *sender_origin,
           "a routed delivery was written to the park document missing its record or the sender ORIGIN the "
           "trusted zone stamped — the origin is the one field every `event.origin` check in every bundle is "
           "written against, and this zone may not invent one on the way back in");
    park_open_rec();
    park_str("m");
    park_hex(record);
    park_str(",");
    park_hex(sender_origin);
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
        const uint32_t *keys = decide_seg_keys(cur);
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
        /* A SLOT IS NINE CHARACTERS: the arm, then the eight lower-case hex digits of the question it answers.
           SELF-DELIMITING AND SEPARATOR-FREE, which is what keeps the field one field — the arms already ran to
           the end of the record with no count in front of them, and a fixed-width slot preserves that (the
           reader divides). The key is decide.c's 32-bit FNV-1a of the constraint key, and it is the reason the
           column crosses at all: the divergence it catches is a resumed flow asking a different question here
           than the run that recorded this arm, which is a comparison no session can make against the other
           session's heap. Lower case because park_hexval refuses anything else, deliberately — see park_hex. */
        for (k = 0; k < n; k++) {
            static const char H[] = "0123456789abcdef";
            char slot[9];
            int d;
            DCHECK(arms[k] == 0 || arms[k] == 1,
                   "a decision slot holds something that is not an arm — the vector records 0 or 1 per branch, "
                   "and anything else resumes a flow onto a path it never took");
            slot[0] = (char)('0' + arms[k]);
            for (d = 0; d < 8; d++)
                slot[1 + d] = H[(keys[k] >> (28 - 4 * d)) & 15u];
            park_raw(slot, sizeof slot);
        }
        decide_seg_set_park_id(cur, g_park_segs++);
        g_parked_census.segs++;
    }
    id = decide_seg_park_id(seg);
    DCHECK(id >= 0, "a decision chain was walked and the segment it started from still has no ordinal");
    return id;
}

/* WHICH RECORD KIND A FLOW IS — asked in ONE place, because the park and the preview both have to answer it and
   a second copy of a two-way choice is a third answer waiting to happen. The two identities are the whole of
   what a flow can be to this file. It was a THREE-way choice while an engine-seeded discovery probe was a
   member of the frontier (PARK_KIND_PROBE, grammar letter 'd'); active discovery is the trusted zone's again
   (extension/lib/discovery-probe.js), so there is no such flow to write and the letter is gone from BOTH ends
   of the round trip — a grammar that can still emit a kind it can no longer parse fails only on a resume in a
   later session. */
typedef enum { PARK_KIND_CAND, PARK_KIND_FLOW } ParkKind;

static ParkKind park_kind_of(const Flow *f)
{
    if (f->cand_src) return PARK_KIND_CAND;
    return PARK_KIND_FLOW;
}

/* DOES THIS FLOW'S RECORD NAME A SEGMENT — i.e. is it one of the flows that makes a park write 's' records at
   all. It is the same question cold_park_flow asks one line before it writes (`f->dec_blob ? decide_blob_seg
   : NULL`), asked of the RUNNING flow as well, and that second half is the reason a host may not write this
   loop itself: decide.c keeps the running flow's evolving vector in its own globals and flow_switch_in FREES
   its blob, so at the instant the host is consulted exactly one member has a NULL `dec_blob` for a reason that
   has nothing to do with its path. cold_census makes the identical split for the identical reason. */
static int park_flow_deep(const Flow *f)
{
    long e = 0, b = 0;

    if (f->dec_blob) return decide_blob_seg(f->dec_blob) != NULL;
    if (f == flow_running()) { decide_live_stats(&e, &b); return e > 0; }
    return 0;   /* a flow that has not run, or ran and froze an empty chain: its record carries `-` */
}

void cold_park_preview(ColdPreview *out)
{
    const Flow *f;
    int i;

    DCHECK(out != NULL, "the cold tier was asked what a park would write into nothing");
    memset(out, 0, sizeof *out);
    for (i = 0; (f = flow_at(i)) != NULL; i++) {
        int deep = park_flow_deep(f);

        switch (park_kind_of(f)) {
        case PARK_KIND_CAND:  out->cands++;  if (deep) out->deepcands++;  break;
        case PARK_KIND_FLOW:  out->flows++;  break;
        }
        if (deep) out->deep++;
        /* AND THE DRIVES AMONG THEM, which is a row of its own for the reason park_rec_orphan gives: it is not
           a KIND, so it is not counted by the switch above and a host reading only the two kinds would be
           shown a residue one record per drive smaller than the one it is about to be handed. */
        if (f->orphan) out->orphans++;
    }
    /* AND THE ROW THAT IS NOT A MEMBER OF THE FRONTIER. A foreign segment belongs to a PEER's flow, so no walk
       of this registry can find it and the host would otherwise be shown a residue smaller than the one it is
       about to be handed. Asked of the LIVE table (world.h's world_segments_held), never of the
       materialization history beside it — the two agree only until a world dies. */
    out->worlds = world_segments_held();
}

/* PARK ONE FLOW — the primitive, and the whole-frontier park below is the loop over it. See cold.h. */
void cold_park_flow(Flow *f)
{
    const void *seg;
    long id;
    char rec[64];

    DCHECK(f != NULL, "the cold tier was asked to write the recipe of no flow at all");
    /* THE NAMESPACE THE RESIDUE RESUMES INTO IS THE FIRST THING IN IT. Before any segment ordinal and before
       any flow, because both are read back in one forward pass and a flow rebuilt before the generation is
       installed mints under the ended session's names. */
    park_gen_rec();
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
    /* A ROUTED DELIVERY USED TO BE ON THIS LIST TOO, and it is off it for the reason the candidate is: it is a
       record kind now ('m', park_rec_deliver below), which is what that crash asked for in as many words —
       "park it as one (the record and the sender origin the trusted zone stamped are both text)". A document
       with an outstanding cross-document delivery had NO LEGAL MOVE under RAM pressure while this stood. */
    /* AND THE OTHER HALF OF THAT SHAPE, WHICH OWES AN ANSWER. flow.h says a cross-agent operation "is the same
       shape as the delivery above and for the same reason, with one thing added" — the asking instance's flow
       is SUSPENDED on the answer this one's program will produce. Dropping the record therefore does not merely
       lose this flow's work: it parks a flow in ANOTHER instance forever, on a completion nothing will ever
       send.
       A TOKEN MAY NEVER ENTER A RECIPE, which is why this is not a record kind and must not become one: a
       token is the trusted zone's name for an entry in its in-memory routing table, it carries no generation,
       and it does not outlive that zone's session — strictly shorter than this residue's life. A recipe
       carrying one would emit, on resume, an answer under a name that zone never minted, which is the
       identical defect the 'g' record closes for a WorldId and remote_object.c REFUSES for an export id. So
       the question is HANDED BACK before any park, per flow (engine.c's engine_retract_flow), and the two
       asserts below state that it was rather than tolerating what it was supposed to have removed.
       TWO ASSERTS AND NOT ONE, because `flow_owes_answer` is a disjunction — an operation still QUEUED on the
       arrival slot, or one already STARTED with its token on a program's row — and a single assert over both
       would report whichever fired under the message written for the other. Two facts with two failure modes;
       one mechanism closes them. */
    DCHECK(flow_perform_pending(f) == 0,
           "a flow was parked still holding an UNSTARTED cross-agent operation — the park hands every question "
           "back to the trusted zone before it writes anything (engine_retract_flow), so an entry here is a "
           "question the zone has been told it owns and this instance is about to page out anyway");
    /* AND STARTED IS NOT A DIFFERENT KIND OF DEBT, which is what the refusal that stood here asked to be told.
       A half-run peer program is not different from every other suspended frame this tier regenerates, and the
       reason is OWNERSHIP: its partial work is this flow's own COW delta, and the delta LEAVES WITH THE FLOW,
       so a program abandoned at the park has produced nothing that outlives the park. What the peer is owed is
       not a value either — HTML §7.2.1.3.5 "CrossOriginGet ( O, P, Receiver )" ends "Return ? Call(getter,
       Receiver)", so what this instance owes is a CALL, and a call abandoned before it completes has been made
       zero times. The zone re-asks and it is made once. */
    DCHECK(!flow_owes_answer(f),
           "a flow was parked still holding a STARTED cross-agent operation — its program's row still carries "
           "the zone's rendezvous token, which may not be written into a recipe (it is the zone's name, it has "
           "no generation, and it does not outlive that zone's session), so the hand-back that runs before "
           "every park (engine_retract_flow) did not reach this row and the flow that ASKED for it, in another "
           "instance, stays suspended on an answer nothing will ever send");
    /* AND THE QUEUED JOBS, WHICH ARE NOT ALL THE SAME KIND OF DEBT — the row cold.h writes for them ("re-
       enqueued by the same reactions, on the same flow's queue") is TRUE of every job the replayed program
       CAUSES and false of exactly one. A promise reaction, a timer callback, a custom-element reaction and a
       fetch's continuation are all re-caused by re-running the code that queued them, and their arguments —
       the capability functions, the settled value, the callee — are re-created with them; that is why the
       queue is released at flow_release rather than written down, and the argument holds.
       A ROUTED CROSS-DOCUMENT DELIVERY IS NOT CAUSED BY THIS FLOW'S CODE. The trusted zone handed it in, and
       engine.c's flow_deliver takes it off `deliver_q` and turns it into a §9.3.3 task in ONE step — so from
       that instant until the task runs, the message exists nowhere but as a job. The 'm' record this park
       writes reads `deliver_q`, which is empty by then, and the release frees the task under `paged`: a peer's
       message silently gone, which is the drop §scheduler's razor forbids and is indistinguishable from a page
       that registered no handler.
       WHAT WOULD CLOSE IT is the same primitive the 'm' record already is, moved one step later: the job
       record would have to carry the delivery's own TEXT — the record the zone routed and the origin it
       stamped, both of which cross a park — so a park could write the 'm' and the resumed session re-deliver
       it. What CANNOT be written is what the delivery has already been turned into by then: the arguments of
       that task are a live MessageEvent init record holding a WindowProxy for the sending document, and a live
       heap reference has no identity outside this session (cold.h says it of the pending register's `resolve`
       and it is the same sentence here). So the park refuses, rather than writing a recipe that resumes a
       document one message short with nothing to say so. */
    DCHECK(flow_job_external(f) == 0,
           "a flow was parked holding a task that a REPLAY WILL NOT RE-CAUSE — a routed cross-document "
           "delivery already turned into a §9.3.3 task, so it is off `deliver_q` (nothing writes an 'm' record "
           "for it) and the release frees it under `paged`. Its arguments are a live event record and cannot "
           "be written; the delivery's own record and stamped origin CAN, so carry them on the job and write "
           "the 'm' from there — or retract the delivery back onto `deliver_q` before the park, the way an "
           "unstarted cross-agent operation is handed back");
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
    /* THE SAME TWO-WAY CHOICE THE PREVIEW MAKES, made by the same function — it was an if/else chain here and
       the preview would have been a second copy of it, which is how the two ends of one round trip come to
       disagree about what a flow is. */
    switch (park_kind_of(f)) {
    case PARK_KIND_CAND:
        /* AN @S CANDIDATE PARKS AS ITS SUBSTITUTION AND ITS PATH — see park_rec_cand. */
        park_rec_cand(f, id);
        g_parked_census.cands++;
        break;
    case PARK_KIND_FLOW:
        if (id < 0) snprintf(rec, sizeof rec, "f-,%.17g", f->val);
        else        snprintf(rec, sizeof rec, "f%ld,%.17g", id, f->val);
        park_rec(rec);
        g_parked_census.flows++;
        break;
    }
    /* …AND WHICH FUNCTION THIS FLOW IS DRIVING, if it is driving one. See park_rec_orphan for why this is a
       field bound by position and not a third arm of the choice above.
       THE LOCATOR NAMES THE FUNCTION THE FLOW ACTUALLY HOLDS, checked here rather than assumed from the stamp:
       this is the one place both halves of the pair are in one hand, and the failure it catches is the whole
       failure mode of the record — a name that does not match its function resumes a drive of the WRONG body
       under this flow's recorded arms, which runs, emits, and is wrong in a way nothing downstream can see.
       Asked only of a drive that HAS its function: a resumed one that has not yet been handed one carries the
       locator alone, which is exactly the state this record exists to create. */
    if (f->orphan) {
        DCHECK(!JS_IsUndefined(f->fn) || f->orphan_want,
               "a driven orphan holds no function and is not waiting for one — those are the only two states a "
               "drive has, so this flow lost its function while still claiming to be driving it and its record "
               "would name a body nothing in this session is calling");
        DCHECK(JS_IsUndefined(f->fn) || JS_OrphanHash(pending_ctx(), f->fn) == f->orphan_hash,
               "a driven orphan's recorded locator does not name the function it is driving — the record about "
               "to be written would send a later session to a different body and run this flow's recorded path "
               "through code that never took it");
        park_rec_orphan(f);
        g_parked_census.orphans++;
    }
    /* …AND THE MESSAGES THIS TIMELINE WAS HANDED AND HAS NOT DELIVERED, immediately after its own record and
       in queue order, because the reader binds each to the flow it last rebuilt. Written for BOTH kinds: a
       candidate session is an ordinary member of the frontier and a peer posts to it like any other. */
    {
        JSContext *qctx = pending_ctx();
        int q = flow_deliver_pending(f), k;

        for (k = 0; k < q; k++) {
            JSValue e = flow_deliver_entry(f, k);
            JSValue rv = JS_GetPropertyUint32(qctx, e, 0);
            JSValue ov = JS_GetPropertyUint32(qctx, e, 1);
            const char *record = JS_ToCString(qctx, rv);
            const char *origin = JS_ToCString(qctx, ov);

            CHECK(record != NULL && origin != NULL,
                  "the cold tier could not read a routed delivery off the flow it is parking — a message that "
                  "cannot be written is one the resumed session never receives");
            park_rec_deliver(record, origin);
            JS_FreeCString(qctx, record);
            JS_FreeCString(qctx, origin);
            JS_FreeValue(qctx, rv);
            JS_FreeValue(qctx, ov);
            JS_FreeValue(qctx, e);
        }
    }
    f->paged = 1;   /* its recipe exists: flow_release may now let its parked continuation go (flow.h) */
}

void cold_park(void)
{
    Flow *f;
    int i;
    /* WHAT THIS PARK IS ABOUT TO WRITE, AND WHAT IT DID — the two-sided half of the preview's contract. The
       host DECIDED to evict on the strength of the preview, so the residue it gets has to be the residue it was
       shown; the census ACCUMULATES across the several parks a session may take (a partial self-park writes a
       tail and the whole-frontier park writes the rest into the same document), which is why the comparison is
       against a delta and not against a total. */
    ColdPreview would;
    ColdParked before, after;
    long deep_written = 0;

    /* AN EMPTY FRONTIER WRITES NOTHING, AND NOTHING IS A POSITIVE ANSWER — "[]" tells the host this document is
       fully explored and DELETES its cold entry. That is the right answer for a frontier that drained and the
       wrong one for a park the host asked for, so the two are separated where they differ rather than where
       they are read. */
    DCHECK(flow_count() > 0,
           "the host asked this engine to page out a frontier with no members — an empty park document is how a "
           "fully-explored document deletes its cold entry, so this stores 'nothing left to do' over whatever "
           "residue the origin had");
    cold_park_preview(&would);
    cold_parked(&before);
    /* ANOTHER INSTANCE'S FLOW STATE LIVES HERE, AND IT CROSSES BY NAME. A foreign world's segment is a peer
       document's timeline materialized in this instance; it belongs to a flow this frontier does not contain,
       so no 'f' or 'c' record below names it and no replay of ours re-derives it. This file REFUSED the park
       over it — correctly, while nothing carried it — and the refusal is deleted rather than kept beside the
       mechanism: what a segment IS across the tier is the VECTOR it was materialized from, world_segment is a
       pure function of that vector, and re-running it in the resumed session rebuilds the segment the way the
       first arrival built it (solver/world.h). The invariant the refusal was standing in for did not
       disappear — a vector is the whole recipe only while the segment holds no WRITES — and it is asserted at
       its origin, on the segment, where both the delta and its name are in one hand.
       WRITTEN HERE AND NOT IN THE PER-FLOW PRIMITIVE, because it is a fact about this ENGINE leaving memory:
       a PARTIAL self-park writes a tail of flows and the instance keeps running, so it holds its peers'
       segments exactly as it did before. That is also what the preview's `worlds` row is for — it is the one
       row the two parks answer differently.
       FIRST IN THE DOCUMENT AFTER THE GENERATION, and in MATERIALIZATION order, so the resume's forward pass
       has every ancestor in place before it rebuilds anything forked from one. */
    {
        const char *const *carried;
        int n_carried = world_segments_park(&carried), k;

        park_gen_rec();
        for (k = 0; k < n_carried; k++) {
            park_rec_world(carried[k]);
            g_parked_census.worlds++;
        }
    }
    for (i = 0; (f = flow_at(i)) != NULL; i++) {
        if (park_flow_deep(f)) deep_written++;
        cold_park_flow(f);
    }
    cold_parked(&after);
    /* THE RESIDUE IS THE ONE THE HOST WAS SHOWN. Not a restatement of the loop above: the preview walks the
       frontier and the park writes RECORDS, so this is where "one record per member, and of the kind the host
       was told" is stated. It fires on a member that left between the two walks, on a kind added to one
       selection and not the other, and on a cold_park_flow that returned without writing. */
    DCHECK(after.flows - before.flows == would.flows &&
           after.cands - before.cands == would.cands &&
           after.worlds - before.worlds == would.worlds &&
           after.orphans - before.orphans == would.orphans &&
           deep_written == would.deep,
           "the park wrote a different residue from the one its own preview described — the host evicted this "
           "engine on the strength of that description, so whatever it is storing is not what it was told it "
           "was storing, and the difference is per record KIND rather than a count it could notice");
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
        /* THE ESCAPE CLAIM, ASSERTED WHERE THE JSON IS MADE. Three writers each state what their own fields
           may hold (park_rec's digits, park_rec_cand's sink letters, park_hex's two
           hex characters), and the property they exist to guarantee is this one: no record needs escaping, so
           no escape path is needed and a byte that would have needed one is a writer that got its charset
           wrong. Said once, here, rather than trusted three times over. */
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
    /* A RESUME IS A REBUILD AND NOT AN EMISSION, so every point of that reward was INHERITED from the session
       that parked it — this flow has produced nothing yet in this one. It is the same statement the fork makes
       (flow.c's flow_fork_inherit), made at the other place a flow is handed another flow's account, so the
       census row counting members that have emitted something themselves does not read a whole resumed
       frontier as productive on the strength of last session's findings. */
    fl->val_born = val;
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
    int gen_seen = 0;
    /* THE FLOW AN 'm' RECORD BELONGS TO — the one this pass last rebuilt, which is the binding the writer
       states by writing a flow's deliveries immediately after it. NULL until the first 'f' or 'c', so an 'm'
       that arrives before one is refused rather than attached to whatever came next. */
    Flow *last_flow = NULL;

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
        if (kind == 'g') {
            /* THE NAMESPACE THIS RESIDUE RESUMES INTO, INSTALLED BEFORE ANYTHING IS NAMED UNDER IT. The record
               carries the WRITER's generation and world_session_resume mints one above it — the successor is
               computed here rather than written out there, so a document cannot state a namespace that is not
               strictly beyond the one its own flows ran in. */
            DCHECK(p == recipes,
                   "a park document's generation record is not its first — every flow read before it would be "
                   "rebuilt in the ENDED session's namespace, and a peer that never left memory already holds "
                   "a segment for each of those names (solver/world.h)");
            DCHECK(!gen_seen,
                   "a park document names TWO generations — one document is one session's residue, however "
                   "many partial parks appended to it, so a second is two sessions' flows merged into one");
            gen_seen = 1;
            world_session_resume((uint32_t)strtoul(q, &ep, 10));
            DCHECK(ep == end, "a generation record carries something after its number — the record is one "
                              "unsigned integer and nothing else");
        } else if (kind == 'w') {
            /* A PEER'S TIMELINE, RE-MATERIALIZED FROM THE ONE THING ABOUT IT THAT HAS AN IDENTITY OUTSIDE THE
               SESSION THAT HELD IT. The vector is handed straight back to world_segment, which is the same
               call the first arrival made: it forks the nearest ancestor this instance already holds and
               starts from the baseline when it holds none — so a document's records rebuild in one forward
               pass, in the order they were materialized, with nothing to patch up.
               IT IS NOT A FLOW and is deliberately outside `flows`: no member of the frontier is added, no
               ordinal is assigned, and the merge's shape asserts below are about members. A residue of nothing
               but 'w' records is refused by those asserts exactly as it should be — an instance that held
               peers' segments and no flows of its own has nothing to resume. */
            WorldId w;
            const WorldId *anc;
            int n_anc;
            char *vec = park_unhex(q, end);

            n_anc = world_parse(vec, &w, &anc);
            world_segment(ctx, w, anc, n_anc);
            free(vec);
            g_resumed.worlds++;
        } else if (kind == 's') {
            long id, bid;
            signed char *arms;
            uint32_t *keys;
            int n, span;

            id = strtol(q, &ep, 10); q = ep;
            q = park_comma(q);
            if (*q == '-') { bid = -1; q++; }
            else { bid = strtol(q, &ep, 10); q = ep; }
            q = park_comma(q);
            span = (int)(end - q);
            n = span / 9;                       /* one arm char + eight hex digits per slot — see the writer */
            DCHECK(id == seg_n,
                   "a park document names its segments out of order — the ordinals are dense and ascending in "
                   "emission order, and a gap means a segment nothing wrote is about to be stood on");
            DCHECK(bid < id,
                   "a decision segment stands on one that has not been read yet — a base is written before "
                   "every user of it, so this document was not written by this engine's park");
            /* THE WIDTH IS THE VERSION CHECK, AND IT IS ASKED BEFORE THE COUNT, because a document written
               before the question-key column has a SHORT field rather than an empty one — two arms are two
               characters, which divides to zero slots, and asking the count first would report that residue as
               a segment with no arms and send the reader hunting an empty freeze that never happened. A slot is
               nine characters and nothing else this grammar writes is, so this is the whole version check and
               the reason there is no version field. Such a residue's arms are real and its questions are
               unrecoverable, which is not a document to read arms out of. */
            DCHECK(span % 9 == 0,
                   "a park document's decision segment is not a whole number of slots — each slot is an arm "
                   "character followed by eight hex digits naming the question it answers, so a field that "
                   "does not divide is residue from a session that recorded arms with no questions. Those arms "
                   "cannot be checked against a replay and must not be stood on");
            DCHECK(n > 0,
                   "a park document holds a decision segment with no arms — the chain never freezes an empty "
                   "head, so every flow standing on this one would replay its path one decision short");
            arms = malloc((size_t)n);
            CHECK(arms, "the cold tier could not rebuild a parked decision segment's arms");
            keys = malloc((size_t)n * sizeof *keys);
            CHECK(keys, "the cold tier could not rebuild the questions a parked decision segment's arms answer");
            for (k = 0; k < n; k++) {
                const char *slot = q + (size_t)k * 9;
                uint32_t kh = 0;
                int d;
                DCHECK(slot[0] == '0' || slot[0] == '1',
                       "a park document holds something that is not an arm — a decision is 0 or 1, and "
                       "anything else resumes a flow onto a path it never took");
                arms[k] = (signed char)(slot[0] - '0');
                for (d = 1; d <= 8; d++) {
                    int hv = park_hexval(slot[d]);
                    DCHECK(hv >= 0,
                           "a park document holds a decision slot whose question is not lower-case hex — this "
                           "file wrote it and nothing else may, so the document did not come from this "
                           "engine's park and the arm beside it names a path this engine cannot verify");
                    kh = (kh << 4) | (uint32_t)hv;
                }
                keys[k] = kh;
            }
            if (seg_n >= seg_cap) {
                seg_cap = seg_cap ? seg_cap * 2 : 64;
                seg = realloc(seg, (size_t)seg_cap * sizeof *seg);
                CHECK(seg, "the cold tier could not grow its rebuilt-segment table");
            }
            seg[seg_n++] = decide_seg_new(bid >= 0 ? seg[bid] : NULL, arms, keys, n);
            free(arms);
            free(keys);
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
               ITS WORLD IS A ROOT, not a child of whatever it was forked from, and what makes that sound is
               the GENERATION rather than an absence of peers. A peer holding segments for this document is the
               ORDINARY case, not the excluded one — Level-1 eviction gives up ONE document's engine and the
               instance that was reading from it stays exactly where it was — so the resumed flow's names must
               be disjoint from the ended session's rather than continuous with them. They are: the 'g' record
               above installed a namespace strictly beyond the one those names were minted in, so this root is
               a world no peer can be holding and its segment is materialized from that peer's baseline, which
               is the truth about a flow that is re-running the document from its first script. The ancestry
               that is NOT carried is the ended session's fork edges, and they belong to flows that are gone. */
            fl = park_flow_add(ctx, val, before, flows);
            fl->started = 1;
            fl->dec_blob = decide_blob_new(sid >= 0 ? seg[sid] : NULL);
            fl->pin_blob = concolic_pins_blob_empty();
            flows++; g_resumed.flows++;
            last_flow = fl;
        } else if (kind == 'm') {
            /* A MESSAGE A PEER SENT AND THIS DOCUMENT HAD NOT YET RECEIVED, put back on the queue of the flow
               it belonged to, in the order it was written — which is the order the page must observe (HTML
               §9.3.3 "Posting messages" / §8.1.7.1 "Definitions"), and the reason this appends rather than
               choosing a position. Both halves come back as TEXT and the ORIGIN is one of them: it is the
               trusted zone's stamp and this file may not re-derive it, so a residue that lost it would leave
               the resumed session inventing the one field every `event.origin` check is written against. */
            const char *comma;
            char *record, *origin;

            DCHECK(last_flow != NULL,
                   "a park document holds a routed delivery before any flow — an 'm' record belongs to the "
                   "flow written immediately before it, so one at the head of the document names nothing and "
                   "would be attached to whichever flow happened to come next");
            for (comma = q; comma < end && *comma != ','; comma++)
                ;
            DCHECK(comma < end,
                   "a parked routed delivery has no field after its record — the sender ORIGIN the trusted "
                   "zone stamped is the second half of this record and no reader may invent one");
            record = park_unhex(q, comma);
            origin = park_unhex(comma + 1, end);
            flow_deliver_push(ctx, last_flow, record, origin);
            free(record);
            free(origin);
        } else if (kind == 'o') {
            /* THE FUNCTION A DRIVEN ORPHAN WAS DRIVING, put back on the flow it belonged to — which is the flow
               written immediately before it, the same binding an unmade delivery uses and for the same reason.
               THE FLOW IS NOT DRIVING IT YET, and cannot be: this document's own scripts have not run, so the
               closure does not exist in this heap at all. What is installed is the WAIT. The flow replays the
               document exactly as an 'f' does — which is what re-creates the closure and what consumes its
               recorded arms — and at the point its work runs out, which is the point the drive stood at in the
               session that recorded it, engine_orphan_fork hands it the body this locator names and it builds
               its own call. That ordering is the whole reason this is a wait and not a call: adopting earlier
               would put the call in front of programs the replay still owes and consume their arms with it.
               NOTHING HERE MAKES THE FUNCTION APPEAR, and that is the honest part. A bundle that changed under
               the residue no longer contains this body, the claim is never met, and the flow finishes having
               driven nothing — which is what a resumed flow re-deriving from CURRENT sources means when the
               source is the code itself. The pair of numbers the engine reports (`orphanClaims` against
               `orphanClaimsMet`) is what makes that visible instead of silent. */
            uint64_t hash = 0;
            int d;

            DCHECK(last_flow != NULL,
                   "a park document holds an orphan locator before any flow — an 'o' record belongs to the flow "
                   "written immediately before it, so one at the head of the document names nothing and would "
                   "attach a drive to whichever flow happened to come next");
            DCHECK(end - q == 16,
                   "a parked orphan locator is not sixteen hex digits — the name is fixed width and is the "
                   "whole record precisely so a short one cannot be read as a valid name, so this document was "
                   "not written by this engine's park");
            for (d = 0; d < 16; d++) {
                int hv = park_hexval(q[d]);
                DCHECK(hv >= 0,
                       "a parked orphan locator is not lower-case hex — this file wrote it and nothing else "
                       "may, so the name would send this drive to a body no session ever recorded");
                hash = (hash << 4) | (uint64_t)hv;
            }
            DCHECK(!last_flow->orphan,
                   "two orphan locators were written for one flow — a flow drives ONE function, so the second "
                   "would silently replace the first and the drive the residue was saved for would be gone");
            DCHECK(hash != 0,
                   "a parked orphan locator is all zeroes — no body hashes to the value this file uses to mean "
                   "'not a drive', so this record names nothing and the flow would wait for ever");
            last_flow->orphan = 1;
            last_flow->orphan_hash = hash;
            last_flow->orphan_want = 1;
            g_resumed.orphans++;
        } else if (kind == 'c') {
            /* AN @S CANDIDATE SESSION COMES BACK AS ITS SUBSTITUTION AND ITS PATH — see park_rec_cand for what
               crosses and, just as load-bearing, which two candidate fields deliberately do not. Everything the
               'f' arm does is done here too, because a candidate IS a flow with a path; the four extra fields
               are what make the replay inject rather than explore, and say how the injected bytes arrive. */
            long sid;
            double val;
            Flow *fl;
            const char *sb, *xb, *rb;
            char *root;
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
            /* THE ROOT IS READ BEFORE THE PAYLOAD because the payload is read to `end` — see park_rec_cand. */
            for (rb = q; q < end && *q != ','; q++)
                ;
            /* AND A RESIDUE FROM A BUILD THAT WROTE NO ROOT FIELD IS NAMED HERE RATHER THAN LEFT TO THE
               SEPARATOR CHECK. Such a record ends at its payload, so this scan runs to `end` and the payload's
               own hex would be taken for the root — park_comma would then refuse it as "not written by this
               engine", which is true and tells the reader nothing about the real cause. It is a real document
               sitting in a real IndexedDB, exactly like the 'd' record below, so it is refused by NAME. There
               is no version field and none is wanted: a record short of a field a reader needs must fail
               rather than resume a candidate that injects a string no session ever built. */
            DCHECK(q < end,
                   "a parked @S candidate has no field after its source — this is a residue from a build whose "
                   "'c' record carried no delivery ROOT, and every field after the source is one place to the "
                   "left of where this reader looks for it. Drop the residue and let the frontier re-seed");
            root = park_unhex(rb, q);
            q = park_comma(q);
            fl->cand_payload = park_unhex(q, end);
            /* THE SINK BINDS BACK TO THE TABLE'S OWN POINTER, and the same call re-registers the sink as
               pending-and-tried AND tells it how these bytes arrive — solve.h says why those are one call and
               not three. `cand_verifying` is not set here: solve_flow_begin sets it from `cand_src` on the
               switch-in, before this flow runs an opcode. `cand_fired` is not set here either, and that is the
               whole point — the replay has to observe the fire again or nothing is recorded.
               THE ROOT DOES NOT LAND ON THE FLOW, it lands on the SEARCH, which is the same asymmetry
               park_rec_cand reads it back out of: one sink's N resumed candidates hand the same root to the
               same entry and cand_learn_root asserts they agree. So this string is ours to free, and freeing
               it here is what says the flow never owned it. */
            fl->cand_sink = solve_resume_candidate(fl->cand_src, root, sname);
            free(root);
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
            last_flow = fl;
        } else {
            /* 'd' IS NOT AN ARM ANY MORE AND IS NOT SILENTLY IGNORED EITHER. It named an engine-seeded
               DISCOVERY PROBE — one flow per candidate document address — and active discovery is the trusted
               zone's again (extension/lib/discovery-probe.js), so no writer can produce one. A residue written
               by a session that still could is a real document sitting in a real IndexedDB, and it lands here:
               the letter is named in the abort so a reader is told which capability wrote it rather than being
               told the document is corrupt. */
            DFAIL("a park record names a kind this grammar does not have — the recipe holds a GENERATION ('g'), "
                  "FOREIGN WORLD SEGMENTS ('w'), SEGMENTS ('s'), FLOWS ('f'), @S CANDIDATE SESSIONS ('c'), "
                  "ORPHAN FUNCTION LOCATORS ('o') and "
                  "UNMADE ROUTED DELIVERIES ('m') and nothing else. A 'd' record "
                  "is a DISCOVERY PROBE written by a session in which the engine seeded its own document "
                  "fetches; that flow kind no longer exists, so this residue predates the change or came from "
                  "another writer");
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
    /* AND IT SAID WHICH NAMESPACE IT CAME FROM. A residue written by a session that had no generation to state
       is one whose names collide with this one's by construction — the same failure the 'd' arm refuses, from
       the other direction: a reader that filled the gap with 0 would hand every peer that never left memory
       the exact names it already holds segments for. */
    DCHECK(gen_seen,
           "a park document names no world-name GENERATION — a WorldId is (document, generation, serial) and "
           "the document name is stable across a park by requirement, so a residue that cannot say which "
           "session wrote it resumes into names a surviving peer already keys its segments on, and answers "
           "every rebuilt flow inside the timeline of a flow that no longer exists");
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
    DCHECK(g_resumed.flows + g_resumed.cands == flows,
           "the rebuild's per-kind census does not add up to the flows it landed — a record kind produced a "
           "flow without counting itself, so a host asking which arms of the grammar ran is told one did not");
    /* THE ORPHAN LOCATORS ARE DELIBERATELY OUTSIDE THAT SUM, because an 'o' record does not produce a flow —
       it is a FIELD of the flow written before it, so it can only ever be a subset of them. Stated rather than
       folded in: a reader who saw the two counters side by side and added all three would be asserting that a
       document with a drive in it rebuilt one flow too many. */
    DCHECK(g_resumed.orphans <= flows,
           "the rebuild read more orphan locators than it rebuilt flows — an 'o' record names the flow before "
           "it, so more of them than there are flows means at least one was bound to a flow that already had "
           "one and a drive the residue was saved for is gone");
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
    g_park_gen_written = 0;   /* …and so does the statement of which namespace it was written under */
    memset(&g_parked_census, 0, sizeof g_parked_census);   /* the census is OF the document; it dies with it */
    free(g_walk); g_walk = NULL; g_walk_n = g_walk_cap = 0;
}
