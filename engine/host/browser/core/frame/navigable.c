/* HTML §7.4 — see navigable.h. */
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/frame/navigable.h"
#include "core/frame/window_proxy.h"
#include "core/frame/policy_container.h"
#include "core/frame/sandboxing.h"
#include "core/frame/window_features.h"
#include "core/frame/opener_policy.h"   /* §7.1.3's policy and §7.1.3.2's group-switch decision */
#include "core/frame/embedder_policy.h" /* §7.1.4's obtain, which §7.1.7 step 4 makes a container's own item */
#include "core/frame/browsing_context_group.h"   /* and §7.1.3.2's swap, which is what that decision reaches */
#include "core/frame/remote_object.h"   /* the ONE grammar a navigable's IDENTITY crosses an instance in */
#include "core/frame/secure_context.h"  /* §8.1.3.5, which §7.1.3 step 2 asks of the reserved environment */
#include "core/frame/session_history.h" /* §7.4.2.2 step 15's test — the arm THIS function must never serve */
#include "core/fetch/headers.h"         /* the response's header list, as the field lines it crosses the ABI in */
#include "core/dom/document.h"
#include "core/dom/node.h"           /* §7.3.1.3's container is an ELEMENT, and §7.1.4.2 wants its node document */
#include "core/html/html_base_element.h"  /* §4.2.3 step 2's shape, which the rules below assert an element's
                                             target has already been through — see navigable_open */
#include "core/html/html_iframe.h"   /* §7.2.2.2's document-tree child navigables — §7.1's walk descends them */
#include "core/html/html_parse.h"    /* the ONE place a Document is parsed — that header owns the token bytes */
#include "core/html/html_script.h"   /* §4.12.1.1's encoding-parse of `src`, stated once for its three callers */
#include "core/loader/document_load_decode.h"  /* §13.2.3.2: WHICH DECODER a navigated response's bytes need */
#include "core/loader/document_scripts.h"  /* §4.12.1's script inventory: what a parsed Document's programs ARE */
#include "core/loader/document_load_type.h"  /* §7.4.5: a response's COMPUTED TYPE, and which document it loads as */
#include "core/loader/document_load.h"       /* §7.4.5's load-a-document: the §7.5 subsection that arm runs */
#include "quickjs-step.h"            /* §7.4 step 14's load is a step machine on the one frontier */
#include "core/url/url.h"
#include "core/encoding/encoding.h"   /* §6's UTF-8 decode, which §7.4.2.3.2 step 3 runs on a percent-decoding */
#include "core/fetch/fetch.h"          /* §2.2.5's body: a response's bytes, which is what a Document is parsed from */
#include "core/idl_args.h"
#include "core/realm.h"              /* §8.1.3.1's environment: the creator's top-level creation URL */
#include "solver/engine.h"
#include "solver/flow.h"             /* the realm teardown's assert: what the frontier still names by HANDLE */
#include "solver/world.h"
#include "solver/solve.h"            /* the @S URL class's ONE detector — §7.2.2.1 step 4 is an arrival at it */
#include "solver/concolic.h"         /* and the ONE predicate that asks whether a destination was computed */
#include "core/dom/node_interface.h"   /* the ONE place a Document is made — see that header */

static RealmBuilder g_realm_builder;

/* THE CHILD REALMS THIS AGENT HAS LIVE, and it BORROWS every one of them. A realm is kept alive by its own
   function objects — each holds a counted reference to the realm that defined it (js_call_c_function reads
   `p->u.cfunc.realm`) — and those are reachable from the Window that the navigable's WindowProxy holds. So a
   realm lives exactly as long as its NAVIGABLE is reachable, which is the spec's answer, and when the navigable
   goes the realm is a garbage CYCLE that the collector breaks.
   THE AGENT MUST NOT OWN ONE, and that is the whole mechanism rather than a preference. An agent-held reference
   is an EXTERNAL ROOT: gc_decref answers "reachable" for the realm and therefore for everything the realm can
   reach — which includes, through the child Document's own `proxy` field, the very WindowProxy whose collection
   would have said the navigable was gone. A design that owns the realm and watches for its proxy to die is
   waiting for something it is itself preventing. Ownership is handed to the navigable in navigable_realm, and
   what is left here is a list of borrowed pointers whose only jobs are counting and answering "is this one
   mine" at teardown.
   THIS LIST IS NOT WHAT KEEPS A REALM ALIVE AND NOT WHAT TEARS ONE DOWN. A realm's teardown reaches the host
   through JS_SetContextTeardownHook, because the last reference to a realm is normally released by the
   COLLECTOR and no host call precedes it. The teardown is SPLIT BY PHASE in quickjs.c for that reason: the
   reference releases (this hook among them) run inside the collection, the table and memory frees run in the
   post-collection sweep. Measured before any of that existed: one src'd iframe in a forced-execution fixture
   asks for one child realm per flow, the list only grew, and at 4022 flows the next child Document could not
   allocate.
   IT WAS NEVER WHERE THIS ENGINE'S MEMORY GOES — measured, because the paragraph above reads like an
   explanation of the whole heap and was taken for one. The default smoke fixture builds ZERO child realms over
   14003 flows (`childRealms` on the @HEAP line) while the C allocator goes from 4.6 MB to 3.39 GB, of which
   quickjs's own allocator holds 1.27 GB in 7.4 MILLION live blocks that no JS object, property, shape, string
   or bytecode accounts for. So that growth is ~500 leaked allocations per CREATED flow in two places — inside
   the runtime and in the host's own malloc — and none of it is here. A reader who arrives with "why does this
   run take gigabytes" is told so here rather than after a day spent on realms.
   THE LAST INVISIBLE EDGE OF THAT CYCLE IS CLOSED, and this paragraph records what it was because the shape
   recurs: a realm's Document record is reached through JS_GetContextOpaque and holds `doc_obj`, `win_obj`,
   `proxy` and `impl` as COUNTED references, and a malloc'd record is not a GC object, so JS_MarkContext used to
   walk a realm without ever naming them. gc_decref therefore never subtracted the record's reference to its own
   WindowProxy, the proxy read as externally rooted, and the cycle record→proxy→Window→function objects→realm
   survived a collection that had nothing else holding it — while the ONLY thing that releases the record is the
   realm's teardown, so the record was preventing the event that frees the record. Every child navigable's realm
   in this engine was therefore immortal, which the gc_obj_list walk reported as a whole page leaked with no
   owner named. The mechanism is quickjs's JS_SetContextMarkHook, the mirror of the teardown hook declared
   below; core/dom/document.c declares its records through it and asserts at each record's BIRTH that the hook
   is there. Any host record holding counted references of its realm must do the same.
   §scheduler's cold tier is a further sentence again, and its own mechanism: a realm is not a snapshot, so
   paging one is not a rewording of paging a flow. */
static JSContext **g_realms;
static int         g_realms_n, g_realms_cap;
/* AND THE TWO NUMBERS A LIVE COUNT CANNOT STATE, WITHOUT WHICH THE ONE ABOVE ANSWERS TWO QUESTIONS WITH ONE
   BIT. `g_realms_n` is how many child realms are live RIGHT NOW, and a small one is the answer for "this run
   built none" and for "this run built a hundred thousand and reclaimed every one of them" alike — which are
   opposite facts about the ceiling, and the second is the whole of what the reclamation below is for. Both
   this file's OOM CHECK and the fixture that exercises the removal draw a conclusion from that number that a
   live count is not entitled to support: the CHECK tells its reader "if it is small, this OOM is a bystander",
   which is false of a run whose realms churned, and the fixture's own note asks a reader to compare the count
   against how many flows have run, which nothing emitted.
   MADE is monotone — every realm this component has ever recorded — and PEAK is the high-water live. Together
   they make reclamation a MEASUREMENT rather than an argument: `made` equal to `peak` says every realm this
   run ever built was live at one instant, so not one was ever reclaimed, and `made` above `peak` says at least
   one realm died while others were being made. That comparison is the assertion the reclamation owes, and it
   is a fact about a RUN rather than about a state, which is why it is read by a fixture row (test_forced.c's
   `realm-reclaim`) and not by a DCHECK: there is no instant at which "a realm has been reclaimed by now" is an
   invariant, so a should-never-happen here would fire on a run that had merely not got there yet.
   NEITHER IS A BOUND. Nothing reads them to decide whether a realm may be built, and no code path branches on
   either — they are counted and published, which is what §NO BOUNDS distinguishes from a cap. */
static int         g_realms_made, g_realms_peak;

void navigable_set_realm_builder(RealmBuilder b) { g_realm_builder = b; }

int navigable_realm_count(void) { return g_realms_n; }
int navigable_realm_made(void)  { return g_realms_made; }
int navigable_realm_peak(void)  { return g_realms_peak; }

/* A REALM OF THIS AGENT IS BEING TORN DOWN — quickjs's realm-teardown hook, and the one moment a Document can
   be released. It is reached from the phase-safe half of the realm's own teardown, which is where a host's
   reference releases belong; document_free releases exactly that (the Document object, the Window, the
   WindowProxy, the Lexbor trees this realm created) and frees its own record.
   IT IS ASKED ABOUT EVERY REALM, this agent's first one included, because a Document dying with its realm is a
   fact about realms rather than about children — and document_free answers for a realm that never had one. The
   list below is consulted only to stop counting a child realm that is gone; a realm that is not in it is not an
   error, it is the root, or a realm some other part of the host built. */
static void navigable_realm_teardown(JSRuntime *rt, JSContext *cctx)
{
    int i;

    (void)rt;
    /* THE LIST IS WALKED FIRST NOW, AND document_free LAST, because the ASSERT below reads the record that
       call releases and is only answerable for a realm this component built. A realm that is not in the list
       is the root's, or one some other part of the host made, and neither has a navigable behind it. */
    for (i = 0; i < g_realms_n; i++) {
        if (g_realms[i] != cctx) continue;
        /* THE ASSERT THAT MAKES THE FREE SAFE, AND IT IS ABOUT THE FRONTIER RATHER THAN ABOUT THIS REALM.
           Reaching here means the collector proved this realm unreachable, so every holder it can SEE is
           already gone — a flow suspended inside this document holds it through COUNTED references (its
           frames, its jobs, its parked continuation, the dups its COW delta took at each capture), so a realm
           a flow can resume into cannot arrive here at all. That is the whole reason §7.5.10's reclamation is
           a reference DROP and never a free, and the reason no flow is terminated or paged to make one.
           WHAT THE COLLECTOR CANNOT SEE IS A DOCUMENT HANDLE. A flow's program queue names the document each
           row is compiled in as a `uint32_t` (solver/flow.h's `dyn_doc`), which holds nothing and stays
           perfectly readable after the realm behind it is gone — so it is the one way the frontier can still
           be standing on a document the collector has just decided is garbage, and that row would later be
           compiled through the doc->realm row document_free is about to clear. */
        DCHECK(flow_programs_for_document(document_doc(cctx)) == 0,
               "a realm was torn down while the frontier still held QUEUED PROGRAMS for its document — a "
               "program row names its document by HANDLE and holds no reference, so the collector could not "
               "see it and compiling that row afterwards asks a doc->realm row this teardown clears. Whatever "
               "still owns those rows has to own a reference to the document too, or drop them with it");
        /* AND THE THREE-NUMBER LAW, AT THE ONE SITE THAT LOWERS THE LIVE COUNT. It is asserted at both
           mutation sites rather than in a helper the two share, because a helper would report ITS line for
           whichever of them broke and these are the only two there are: a realm is recorded in
           nav_create_finish and forgotten here. A member reaching this line means at least one was recorded,
           so `made` cannot be zero and `peak` cannot be below the count that is about to drop — if either is,
           the recording site ran without counting and every reading built on these numbers, the fixture's
           reclamation row included, is a reading of a register nothing maintains. */
        DCHECK(g_realms_made > 0 && g_realms_peak >= g_realms_n,
               "a child realm reached its teardown while this component's own census disagreed that it had "
               "ever been recorded — `made` is monotone and `peak` is the high-water of the live count, so a "
               "member leaving a list with a zero `made`, or with a `peak` below the live count it is about "
               "to lower, means nav_create_finish recorded a realm into the list without counting it. Every "
               "reader of these three numbers (this file's OOM CHECK, the result document's `_heap`, the "
               "fixture row that asks whether reclamation ever ran) is then reporting a register that is not "
               "being kept");
        /* AND THE PROXY SIDE OF §7.5.10 STEP 9, WHICH IS THE OTHER HALF OF A BRACKET. The first half is inside
           window_proxy_set_destroyed: after the write it asserts that the record names NEITHER a realm nor a
           Window. This is the same pairing asked from the far end — at the one instant a realm actually dies,
           no record may still name it while having already given the Window back. The two fields are a pair
           because the Window is what KEEPS a child realm alive (its C function objects each hold the realm
           that defined them), so `realm` is a raw borrow valid only while `window` is set; the other
           combination is a live navigable holding a JSContext this hook is freeing, and the very next read
           through that navigable — proxy_realm returns the borrow without materialising anything when it is
           non-NULL — answers out of freed memory with nothing anywhere to say so.
           IT IS ASKED HERE, AT THE ORIGIN, and there is exactly ONE call site that can reach this abort, which
           is why the question is a predicate and the assert is not inside it: a DCHECK stamps the line it is
           WRITTEN at, and the line that has to be named is the teardown, not the reader.
           IT IS NOT "the proxy no longer names this realm" and it is not `window_proxy_destroyed` — see
           core/frame/window_proxy.h, which states which routes into this hook leave each of those false. */
        DCHECK(!window_proxy_realm_dangling(document_window_proxy(cctx), cctx),
               "a realm was torn down while its navigable's §7.2.3 WindowProxy still BORROWED it with the "
               "Window already given back — HTML §7.5.10 \"Destroying documents\" step 9 (\"Set document's "
               "node navigable's active session history entry's document state's document to null\") clears "
               "the two together for exactly this reason, so a record in the split state is a live navigable "
               "naming a JSContext this hook is about to free. Whatever released the Window has to release "
               "the realm borrow in the same write");
        g_realms[i] = g_realms[--g_realms_n];   /* order means nothing here; membership does */
        break;
    }
    document_free(cctx);
}


/* THE CHILD'S ADDRESS AND ORIGIN, from ONE parse. HTML §7.3.2.1 "Creating browsing contexts"' DETERMINE THE
   ORIGIN decides the second: an `about:blank` navigable — which is what `open()` with no URL creates, and what
   an `<iframe>` with no `src` creates — gets the CREATOR'S ORIGIN RECORD, the same record and therefore the
   same identity, which is the whole reason a same-origin popup or frame can be scripted at all AND the reason
   a `data:` document's about:blank child is same origin with it (both hold one opaque origin, and §7.1.1 step 1
   compares identity). Any other URL contributes URL §4.7 "Origin"'s answer for itself.
   IT IS RESOLVED HERE, not by the host. `open("/admin")` and `<iframe src=a.html>` are how most real uses are
   written, and a relative reference has neither an origin nor a meaning outside the document that wrote it —
   handing the host the raw text would make it resolve against something, and the only base it could pick is a
   guess. Returns false when the reference does not parse; `*out_url` is owned and `*out_origin` is BORROWED
   (an origin lives for the agent).
   `sandbox_flags` IS THE FLAG SET OF THE DOCUMENT BEING CREATED, NOT OF THE CREATOR — §7.3.2.1's determine the
   origin takes `sandboxFlags`, and §7.3.2.1's create passes it the NEW browsing context's creation sandboxing
   flags, whose SANDBOXED ORIGIN BROWSING CONTEXT FLAG is what mints a fresh opaque origin. Handing it the
   creator's set instead would make `<iframe sandbox>` leave the child same-origin with its embedder (the
   creator is not itself sandboxed) and would make a `sandbox allow-same-origin` frame of a sandboxed page
   opaque (the creator is). It is an input of the operation, which is why it is a parameter here and not a read
   off anything.

   `out_javascript` IS THE SCHEME DISPATCH, ANSWERED BY THE COMPONENT THAT PARSES AND NEVER BY A CALLER'S `if`.
   HTML §7.4.2.2 "Beginning navigation" branches on the scheme — "if url's scheme is `javascript`: queue a
   global task on the navigation and traversal task source … to navigate to a javascript: URL" and RETURN — so
   §7.3.2.1's determine-the-origin never sees one, and `*out_origin` is NULL for that arm rather than an origin
   nobody may use. Each caller then either SERVES the arm (navigable_navigate does) or asserts it is
   unreachable (navigable_create does), which is the shape a dispatch over "what is this destination" has to
   have: an entry that skips the question does not report an absent capability, it reports an unrelated
   subsystem failing on input that subsystem should never have been shown.
   WHICH ANSWER IT WOULD OTHERWISE PRODUCE IS WHY THIS IS WORSE THAN A MISSING BRANCH. URL §4.7 "Origin"
   switches on the scheme and its last arm is "Otherwise: Return a new opaque origin", with the standard's own
   note: "This does indeed mean that these URLs cannot be same origin with themselves." So a `javascript:` URL
   run through the determine below is given an OPAQUE origin, child_in_this_agent compares it against this
   agent's and answers false, and §7.4's create provisions the popup as a CROSS-ORIGIN PEER INSTANCE — a second
   engine, for a URL that was never a document. Everything after that is a correct consequence of the one wrong
   answer, which is what makes it hard to read backwards: the page's own handle is a REMOTE WindowProxy, so
   §7.2.1's cross-origin member list rejects a property set on it where the spec has a same-realm write, and
   every read of it is a cross-instance suspend where the spec has a synchronous one. None of those symptoms
   names a scheme.
   IT IS ASKED OF THE PARSED SCHEME AND NEVER OF THE CALLER'S BYTES, which is why it is here and not one line
   above the parse where it used to be. `<a href="JavaScript:x()">` is a `javascript:` URL — URL §4.4's scheme
   state lowercases as it appends — and so is one written with leading C0 controls or spaces, which §4.4 step 1
   strips; core/html/hyperlink.c hands this the RAW attribute value, so a byte compare on the argument answered
   NO for both and sent them into the determine. The SERIALIZED url is the exact operand: URL §4.5's serializer
   opens with "let output be url's scheme, followed by U+003A (:)", so the prefix test below is the scheme
   comparison written once. */
static bool child_address(JSContext *ctx, const char *url, SandboxFlags sandbox_flags, char **out_url,
                          const Origin **out_origin, bool *out_javascript)
{
    UrlRecord base, rec;
    const char *base_url;
    bool have_base, ok = false;

    *out_javascript = false;
    /* THE about:blank CASE ANSWERS WITHOUT A BASE, and must be checked FIRST. Reading the document's address
       up here evaluated it even for `open()` with no argument — the one call that needs no address at all —
       and in a host with no document installed that is an assert, not a value. It aborted sixteen spec files
       whose only sin was calling open(). */
    if (!url || !*url || !strcmp(url, "about:blank")) {
        *out_url = strdup("about:blank");
        CHECK(*out_url != NULL, "navigable: OOM naming an about:blank child");
        /* §7.3.2.1's determine-the-origin STEP 4, WITHOUT A PARSE: this IS `about:blank` with a non-null source
           origin, so the answer is the source origin — this agent's — and no URL record is needed to say so.
           The sandbox flag is asked first because that algorithm's step 1 comes first and would mint instead of
           inherit. */
        *out_origin = (sandbox_flags & SANDBOX_ORIGIN) ? origin_determine(NULL, true, NULL) : origin_agent();
        return true;
    }
    base_url = document_base_url(ctx);
    url_record_init(&base);
    have_base = base_url && url_parse(&base, base_url, strlen(base_url), NULL);
    url_record_init(&rec);
    if (url_parse(&rec, url, strlen(url), have_base ? &base : NULL)) {
        *out_url = url_serialize(&rec, false);
        ok = *out_url != NULL;
        /* §7.4.2.2's SCHEME DISPATCH, over the parse this function already made. `*out_origin` stays NULL for
           it because §7.4.2.2 returns before §7.3.2.1's determine-the-origin is ever reached — the value does
           not exist, which is a different fact from an origin that could not be computed, and a caller that
           read one would be reading the opaque origin URL §4.7 mints for a URL that is not a document. */
        if (ok && strncmp(*out_url, "javascript:", sizeof "javascript:" - 1) == 0) {
            *out_javascript = true;
            *out_origin = NULL;
        } else if (ok) {
            *out_origin = origin_determine(&rec, (sandbox_flags & SANDBOX_ORIGIN) != 0, origin_agent());
        }
    }
    url_record_free(&rec);
    url_record_free(&base);
    return ok;
}

/* §7.4's CREATE A NEW NAVIGABLE, as ONE operation with ONE result — see navigable.h for why it is synchronous
   and why the host is told rather than asked. Both callers are here: `window.open` below, and §4.8.5's iframe
   insertion steps. They differed only in which fields they filled into the request, which is exactly the kind
   of duplication that lets two call sites drift into two protocols. */
/* WHERE THE CHILD LIVES, and it is decided by its ORIGIN because an instance is an ORIGIN-KEYED AGENT. It is
   §7.1.1's SAME ORIGIN and nothing else — so two DISTINCT opaque origins are two instances (a sandboxed
   document must not share a heap with the document that sandboxed it, the same rule SECURITY.md states for the
   credentialed-read principal) while ONE opaque origin held by two Documents is ONE agent, which is what
   §7.3.2.1's inheritance cases produce and what a serialized comparison could not express: it answered "another
   instance" for a `data:` document's own about:blank child, and no host provisions that peer. */
static bool child_in_this_agent(const Origin *child_origin)
{
    return origin_same(origin_agent(), child_origin);
}

/* THE CHILD'S DOCUMENT — the parse of what the address served, or §7.4's initial about:blank when it served
   nothing. A REAL LEXBOR PARSE either way, because tree construction always produces <html><head><body> and a
   child whose body is missing is not a document a page can append to.
   THE PARSE IS THE ENGINE'S AND THE BYTES ARE THE HOST'S. That is CLAUDE.md's split — Lexbor and quickjs own
   what a document IS, the host owns the network — and it is why the bytes ARRIVE HERE rather than being asked
   for here: fetching is a suspend, this runs inside a property read that cannot suspend, and the one caller
   that can suspend does the asking (js_nav_load_step).
   NO BYTES IS NOT AN ERROR. `about:blank` served none, and neither did an address whose fetch failed — a
   browser showing an error page still has a navigable, with a document, in the tree.

   AND WHICH PARSE IT IS, IS §7.4.5's TO DECIDE AND NOT THIS FUNCTION'S. "Let type be the computed type of
   navigationParams's response" — an HTML MIME type loads an HTML document (§7.5.2) and an XML MIME type loads
   an XML document (§7.5.3), and this function used to run the HTML parser over BOTH. That is not a missing
   feature wearing a wrong answer, it is a wrong answer with nothing to say so: `/common/dummy.xml` is the
   twenty-nine bytes `<foo>Dummy XML document</foo>\n`, the HTML parser synthesises `<html><head></head><body>`
   around it and puts the trailing newline — XML §2.1's `Misc` AFTER the document element, which is not in the
   DOM at all — inside that `<body>`, so `documentElement.textContent` answered `"Dummy XML document\n"` for a
   document containing no such string. No crash, no log, and a tree real enough that every read after it looked
   like a measurement.
   `computed_type` IS §7.4.5's COMPUTED TYPE for the response, and NULL means THERE WAS NO RESPONSE — §7.4's
   initial about:blank, or an address whose fetch failed. Those are HTML documents by §7.4 and not by a
   default: the `EMPTY` skeleton below is the tree §7.4 gives them, and there is nothing to compute a type
   from. A response that carried no `Content-Type` is a DIFFERENT fact and does not arrive here as NULL — MIME
   Sniffing §7 computed a type for it out of its bytes, which is why this function no longer sees a header
   value at all. */
static lxb_html_document_t *child_document(const char *body, size_t body_len, const MimeType *computed_type,
                                           int encoding, DocumentLoad **out_load)
{
    static const char EMPTY[] = "<!doctype html><html><head></head><body></body></html>";
    lxb_html_document_t *dom;

    DCHECK(out_load != NULL,
           "a child navigable's Document was asked for without the handle its §7.5 load stands in — the parse "
           "is a PULL and the loop over its items belongs to whoever can park between them, so a caller that "
           "takes no handle is one that was going to drive it to completion");

    /* THE TWO TRAVEL TOGETHER OR NEITHER DOES. A response is bytes AND a computed type; one without the other
       is a caller that computed the type from something that is not this response, or that has a response it
       forgot to sniff — and the second reads as §7.4's initial about:blank, which parses HTML and says
       nothing. */
    DCHECK((body != NULL) == (computed_type != NULL),
           "a child navigable's Document was built from a response with only one of its two halves — MIME "
           "Sniffing §7's computed type is a fact about the same bytes, so bytes with no type is a load that "
           "skipped the sniff and a type with no bytes is one computed for another response");
    /* §7.4.5's LOAD A DOCUMENT — the dispatch AND the §7.5 subsection it routes to, both from
       core/loader/document_load.h, run at the parse below. This site used to hold its own copy of the arm
       test and its own crash, as the other two entries that build a Document out of a response did; one
       component owns both now, the arm this build has no loader for crashes by name THERE, and each §7.5
       loader re-asks the dispatch at its own parse so a route that skips the router still crashes. */
    dom = dom_document_create();

    /* WHAT ACTUALLY FILLED THE HEAP WHEN THIS FIRES IS NOT DOCUMENTS — see the realm list in navigable_realm.
       The message says so, because a `@WHY` is read at the site and "OOM" alone sends the reader here. It names
       what this component contributed, and it says WHERE TO LOOK FIRST, because it does not know what else in
       the run was allocating: the same fixture with no src'd navigable in it at all builds zero realms and
       still reaches gigabytes (navigable_realm's note has the numbers), so a reader who arrives here must
       check the realm count before believing this paragraph is their answer.
       WHERE THAT NUMBER IS, AND IT IS NOT WHERE THIS MESSAGE USED TO SEND PEOPLE. It named the heap-census
       LINE, which is printed by `run_scheduler` — a loop only `engine_run` enters, so the extension, which is
       where a real page reaches this OOM, prints no such line at all. An instruction that is accurate about
       the ENGINE and wrong about THIS HOST is the stale-DFAIL failure with an allocation under it: the reader
       is standing at the crash and is sent to look for output nothing wrote. The census rides the result
       document now (solver/result.c's `_heap`), which every host publishes, so the message names a FIELD
       rather than a stream. */
    CHECK(dom != NULL,
          "OOM creating a child navigable's Document — CHECK the result document's `_heap.childRealms`, "
          "`childRealmsMade` and `childRealmsPeak` TOGETHER (a host whose output is a stream of lines prints "
          "those same bytes). READING ONE OF THEM DECIDES NOTHING, and this message used to say it did: "
          "`childRealms` is the count that is LIVE RIGHT NOW, so a small one is the answer for a run that "
          "built no child realm at all AND for a run that built a hundred thousand and reclaimed every one, "
          "which are opposite facts about this crash. `childRealmsMade` is monotone and `childRealmsPeak` is "
          "the high-water live, so the three of them separate the three cases. MADE 0: this component built "
          "nothing, this OOM is a bystander, and the memory is elsewhere in the run (measured: the smoke "
          "fixture once built zero child realms over 14003 flows and still reached 3.39 GB). MADE HIGH AND "
          "PEAK LOW: reclamation is running and realms are CHURN rather than a working set — look at what the "
          "churn allocates outside the realm, not at the realms. PEAK EQUAL TO MADE: not one realm was ever "
          "reclaimed, which is the ceiling itself — the working set is REACHABLE NAVIGABLES, a flow per src'd "
          "iframe each holding its document, and the question is then what still names them rather than "
          "whether reclamation exists: HTML §7.5.10 \"Destroying documents\" step 9 has the navigable let go "
          "of the Document (core/frame/window_proxy.c's window_proxy_set_destroyed), after which a realm is a "
          "garbage CYCLE its function objects and its Window hold together, the collector breaks it, and the "
          "teardown that follows is split by phase in quickjs.c so the reference releases run inside the "
          "collection and the tables in the sweep after it");
    /* FLOW-PRIVATE: `dom_document_create` above and this parse are one uninterrupted operation, so the child
       navigable's Document is a tree no other flow can reach while §13.2.6 builds it (solver/dom_cow.h).
       THE TWO CALLS ARE §7.4.5's LOAD-A-DOCUMENT AND §7.4's INITIAL about:blank, and they are different
       algorithms rather than one with a null argument: a RESPONSE is dispatched on its computed type, and a
       child with no response is an HTML document by §7.4 with no type to compute and nothing to dispatch on.
       The status is CHECKed on both, which is what makes the router's release behaviour real — a `DFAIL` is
       compiled out at APICLIENT_DEV=0 and this CHECK is not. */
    /* §13.2.4.5 ENABLED ON BOTH ARMS, AND THIS IS THE SITE THAT PROVES THE FLAG IS NOT `root_kind`. The
       declaration beside it is PRIVATE and correct — no other flow can reach this tree while §13.2.6 builds
       it — and this Document is an `<iframe>`'s, whose scripts this engine runs. Deriving the scripting mode
       from the COW declaration would give every framed document §13.2.6.4.7's Disabled arm. §7.4's initial
       `about:blank` takes it too: a page writes into that document and §13.2.6.4.4 PREPARES the written
       `script`, so it is script-running before it has a single byte of its own. */
    /* AND THE RESPONSE'S ARM IS OPENED RATHER THAN RUN. A document parse is O(document), so completing one
       here held the thread for the length of the response inside a flow's slice — core/loader/document_load.h
       is where that is stated and where the assert that refuses it now stands. What comes back is the load
       STANDING AT ITS FIRST ITEM; the loop belongs to the step machine above, which parks between items.
       §7.4's INITIAL about:blank IS NOT THAT and takes no handle: its markup is the fifty-three byte constant
       on the line above, which is a fact about this build and not about any response, so it is O(1) in the
       one quantity that matters and there is nothing for a driver to step. */
    if (body) {
        *out_load = document_load_begin(dom, DOM_PARSE_ROOT_PRIVATE, HTML_SCRIPTING_ENABLED, computed_type,
                                        encoding, (const lxb_char_t *)body, body_len);
        CHECK(*out_load != NULL,
              "a child navigable's response took an HTML §7.4.5 arm this build has no §7.5 loader for — the "
              "router has already named the subsection in a dev build, and in release this is where the "
              "navigation stops rather than handing the response to a parser that does not serve it");
    } else {
        *out_load = NULL;
        CHECK(html_parse_document(dom, DOM_PARSE_ROOT_PRIVATE, HTML_SCRIPTING_ENABLED,
                                  (const lxb_char_t *)EMPTY, sizeof EMPTY - 1) == LXB_STATUS_OK,
              "a child navigable's initial about:blank did not parse");
    }
    return dom;
}

/* THE PASSES, IN THE ORDER §13.2.7 RUNS THEM.
 *   PARSE_POSITION — an inline classic script ("immediately execute the script element") and the `pending
 *     parsing-blocking script`, whose fetch AND evaluation everything later in the document waits for: §13.2.6.4.8
 *     'The "text" insertion mode' blocks the tokenizer and spins the event loop until that script's `ready to be
 *     parser-executed` becomes true.
 *   WHEN_PARSED — the `list of scripts that will execute when the document has finished parsing`, which §13.2.7
 *     runs IN ORDER, after the parse and before DOMContentLoaded.
 *   ASAP — the `set of scripts that will execute as soon as possible`, which has no position at all: §13.2.7
 *     waits for that set only before the load event, so any arrival order is a correct one and these park on
 *     their replies instead of taking a slot. It is the one schedule script_sched_is_ordered answers "no" for. */
enum { SCRIPT_PASS_PARSE_POSITION = 0, SCRIPT_PASS_WHEN_PARSED, SCRIPT_PASS_ASAP };

static int script_sched_pass(ScriptSchedule s)
{
    if (!script_sched_is_ordered(s))   return SCRIPT_PASS_ASAP;
    if (s == SCRIPT_SCHED_WHEN_PARSED) return SCRIPT_PASS_WHEN_PARSED;
    DCHECK(s != SCRIPT_SCHED_IN_ORDER_ASAP,
           "a PARSED document's script inventory holds a member of the `list of scripts that will execute in "
           "order as soon as possible` — §4.12.1 reaches that list only for an element that is NOT "
           "parser-inserted, and the HTML parser gives every element it inserts a parser document, so this "
           "schedule was read off something that is not a parse product");
    return SCRIPT_PASS_PARSE_POSITION;
}

/* A DOCUMENT'S OWN SCRIPTS ARE PROGRAMS OF THE ONE FRONTIER, AND SEEDING THEM IS THE DOCUMENT'S OWN BEHAVIOUR
 * RATHER THAN A HOST EDGE. §7.11's create-and-initialize-a-Document ends by handing the response's bytes to the
 * parser, and what the parser does with a `<script>` is §4.12.1 — so a Document this agent built out of a
 * response and never ran the scripts of is not a document that has been analysed at all. It is here because
 * this is the ONE place a same-origin Document of this agent comes into existence.
 *
 * IT WAS IN THE HOST, AND ONLY IN ONE OF THE THREE. `wpt_runner.c`'s child-realm builder seeded them and
 * `main.c`'s and `test_forced.c`'s did not — the identical drift core/realm.h was written to abolish for the
 * per-realm intrinsic list, arriving a second time through the one seam that is still hand-copied per host.
 * The cost of it was not a WPT-only detail: in the SHIPPED extension a same-origin `<iframe src>` got a realm,
 * a parsed Document and a WindowProxy, and its own bundle never ran — measured on real Chrome over a
 * same-origin fixture, where the child document contributed ZERO endpoints and the run's document list named
 * only the parent, while the CROSS-origin spelling of the same page (a second instance, whose `qjs_init` seeds
 * its own scripts) reported the child's. A host builds a platform surface; WHICH programs a Document runs is
 * the Document's, and the two hosts that never wrote the line are the reason it may not live there.
 *
 * THE ORDER IS THE DOCUMENT'S ORDER, AND IT IS NOT ONE WALK OF THE INVENTORY. §4.12.1's last steps sort each
 * element into one of the Document's four queues, and §13.2.7 "The end" runs those queues — so the document's
 * run order is the PASSES below, and a `defer`red script written between two inline ones runs after both of
 * them. Every entry of the ordered passes becomes a POSITION in the flow's sequence: an inline script's program
 * in place, an external script's ADDRESS in place (engine_queue_docscript_url), with the flow stopping there
 * until the reply fills it. That last half is what this seam did not have: an external script could only be
 * PARKED on, joining the sequence when its reply drained — after everything queued in this pass and in ARRIVAL
 * order among the replies — so an inline script after a parser-blocking `<script src>` ran before the bundle it
 * is written after, and two ordered externals ran in whichever order the network answered. Both were aborts.
 *
 * `doc` NAMES THE DOCUMENT AND THEREFORE THE REALM the program is compiled in (solver/engine.h): a child's
 * script compiled in its creator's realm defines the child's globals on the parent and reads the parent's back
 * as the child's. */
static void navigable_seed_scripts(JSContext *cctx, lxb_html_document_t *dom, uint32_t doc)
{
    DocScripts ds;
    int pass, i;

    DCHECK(cctx != NULL && dom != NULL, "a Document's scripts were seeded with no realm or no tree");
    DCHECK(document_doc(cctx) == doc,
           "a child Document's scripts were seeded naming a document its own realm does not answer with — the "
           "name decides which realm the program is compiled in, so the two disagreeing compiles the child's "
           "code in another document's Window");
    ds = document_exec_scripts(dom);
    for (pass = SCRIPT_PASS_PARSE_POSITION; pass <= SCRIPT_PASS_ASAP; pass++)
    for (i = 0; i < ds.n; i++) {
        char *abs_url;

        if (script_sched_pass(ds.sched[i]) != pass) continue;
        /* §8.1.4.4 "Calling scripts"'S TWO ALGORITHMS, AND BOTH NOW HAVE A ROUTE. The row carries the
           element's type (solver/flow.h's `dyn_type`), so this document's `<script type=module>` reaches
           run-a-module-script instead of being handed to the CLASSIC entry, where the child's own `import`
           would have come back a SyntaxError from a parser that is perfectly correct. The DCHECK that stood
           here aborted the whole engine on any child navigable whose document had one — one of three, with
           engine_join_document's and core/html/html_script.c's, all three naming this column. */
        DCHECK(script_type_executes(ds.types[i]),
               "a document's script inventory holds a row whose type executes nothing — document_exec_scripts "
               "drops an import map, a set of speculation rules and §4.12.1.1's null type before any of them "
               "becomes a row, so a fourth answer here means the types column was never written for this row");
        if (ds.bodies[i]) {
            /* WHERE AN INLINE ENTRY LANDS IS ITS SCHEDULE'S ANSWER, AND THE TWO ANSWERS ARE DIFFERENT SPEC
               STEPS. §4.12.1.1 "Processing model" reaches `immediately execute the script element` only for
               what falls past "If el's type is `classic` and el has a src attribute, or el's type is
               `module`" — so an inline CLASSIC script runs where it stands, and an inline MODULE is appended
               to the `list of scripts that will execute when the document has finished parsing` (every element
               of this scan is parser-inserted) and takes a POSITION in the sequence instead. Not one call with
               a flag: the standard runs them at two different times, and a module has a graph to LOAD before
               its result exists, which is exactly why the standard does not run it in place. */
            if (ds.sched[i] == SCRIPT_SCHED_IMMEDIATE) {
                DCHECK(ds.types[i] == SCRIPT_TYPE_CLASSIC,
                       "an inline entry of a document's script inventory reached `immediately execute the "
                       "script element` with a type other than classic — §4.12.1.1 sends every module to one "
                       "of the three lists before that step, so the schedule and the type disagree about one "
                       "element");
                /* THE ELEMENT ENTRY, because this row HAS an element: the element-less entry
                   (engine_queue_fetched_script) is for a program no `<script>` caused, and a row seeded out of
                   a document's inventory is never one.
                   Same position (this seed builds the sequence in document order, so APPEND *is* in place) and
                   the same type the assert above has just pinned; what it adds is the element §4.12.1.1's
                   "execute the script element" switches on and sets §3.1.7's `currentScript` to.
                   THE LENGTH IS THE `strlen` FOR THIS SEAM, AND THAT IS THE TOKENIZER'S GUARANTEE. Every row
                   of `ds` came out of a Lexbor parse of this document's bytes, so an inline `<script>`'s text
                   is what HTML §13.2.5.4 "Script data state" emitted — and its U+0000 NULL row is "This is an
                   unexpected-null-character parse error. Emit a U+FFFD REPLACEMENT CHARACTER character token",
                   with §13.2.5.84 "Numeric character reference end state" answering the same for `&#0;`. A
                   program that CAN carry a NUL never comes from here; the seams that do (an injected element's
                   `.textContent`, a `javascript:` URL, an @S candidate) each hand this queue a real length. */
                engine_queue_element_script(doc, ds.bodies[i], strlen(ds.bodies[i]), ds.types[i], ds.els[i]);
            } else {
                DCHECK(ds.types[i] == SCRIPT_TYPE_MODULE,
                       "an inline entry of a document's script inventory is scheduled somewhere other than its "
                       "own parse position and is not a module — §4.12.1.1 owes no fetch for a classic script "
                       "whose source it already has, so nothing else can reach a list from an inline row");
                /* …AND THE SAME LENGTH FOR THE SAME REASON — see the classic arm above. */
                engine_queue_element_script(doc, ds.bodies[i], strlen(ds.bodies[i]), ds.types[i], ds.els[i]);
            }
            continue;
        }
        DCHECK(ds.srcs[i] != NULL,
               "a document's script inventory holds an entry that is neither an inline body nor an external "
               "address — document_exec_scripts states one of the two for every executable entry");
        abs_url = script_src_absolute(cctx, ds.srcs[i], strlen(ds.srcs[i]));
        if (!abs_url) continue;
        /* FETCH §4.3 SCHEME FETCH IS ASKED BY THE PARK, NOT HERE. §8.1.4.2 "Fetching scripts" runs the same
           algorithm `fetch()` does, so the switch on the URL's scheme decides who answers these bytes here as
           well — and both destinations below reach the flow's pending register, where solver/engine.c's
           pending_park_request runs §4.3 and places its response on the record for the same delivery that
           reads the trusted zone's. A `<script src="data:text/javascript,…">` in a child navigable's document
           therefore loads, at its own position, out of bytes already in this address space. */
        /* THE SET PARKS AND THE ORDERED PASSES TAKE A SLOT — the whole difference between having a position and
           not needing one. A parked reply becomes a program of this flow whenever it drains, which is the
           arrival order a SET is entitled to; a slot holds the sequence at this script until its reply fills it. */
        if (pass == SCRIPT_PASS_ASAP) engine_pending_script_url(cctx, abs_url, ds.types[i], ds.els[i]);
        else                          engine_queue_docscript_url(doc, abs_url, ds.types[i], ds.els[i]);
        free(abs_url);
    }
    doc_scripts_free(&ds);
}

/* BUILD A CHILD NAVIGABLE'S REALM AROUND A RESPONSE — see navigable.h. Two callers, and they are the two
   documents a navigable has: the load job, which has the bytes §7.4 step 14 fetched, and `proxy_realm`, which
   is materializing the initial about:blank and has none. THE POLICY TRAVELS WITH THE TREE, because §7.2.6's
   container is built from both and a builder handed only the tree would judge the document against its
   `<meta>` policies alone: core/dom/document.c's `document_policy_new` WALKS the parsed tree for CSP §3.3's
   `<meta>` policies, which is why the parse has to be FINISHED before the realm is built and why this cannot
   be reordered into §7.5's own sequence (create the Document, then fill its input byte stream).
 *
 * AND THAT IS WHY IT IS A WORK RECORD AND NOT A CALL. The parse in the middle of it is O(document), so the
 * whole of this ran inside one un-preemptible span of a flow's slice — core/loader/document_load.h states the
 * consequence and carries the assert that now refuses it. The loop belongs to the step machine that can park,
 * and that machine's FRAME cannot hold what the other side of the suspension needs: §7.4.5 determines the
 * address, the origin, the policy container, the opener policy and the sandboxing flag set into C locals, and
 * core/frame/policy_container.h says in as many words that its serialization's text is BORROWED and that "a
 * live value crosses neither an instance, nor a session, nor a PARK". So this record COPIES what it will still
 * need afterwards and the machine carries ONE pointer across the stage boundary instead of fifteen.
 *
 * `navigates` IS A FACT ABOUT THE OPERATION AND NOT ABOUT THE NAVIGABLE. A load hands the navigable its new
 * active document once that Document exists; `proxy_realm` is MATERIALIZING the initial about:blank the
 * navigable already has and must move no binding at all. Neither is recoverable by inspecting the navigable,
 * which is CLAUDE.md's "an operation that becomes a work item takes its inputs with it" exactly — so the
 * caller states it, the same way the address and the initiator's policy container already travel with the job. */
typedef struct {
    uint32_t doc;
    char *url, *top_level_url, *about_base_url;
    /* §7.1.7's CONTAINER, ITEM BY ITEM AND COPIED. Its serialization borrows every string from whoever built
       it (policy_container.h), and this record outlives that frame — so the items are held rather than the
       struct, and the struct is rebuilt from them at the finish. */
    char *csp, *self_origin, *coep_endpoint, *coep_report_only_endpoint;
    /* PERMISSIONS POLICY §9.1's TWO RESPONSE HEADER FIELD VALUES, copied for the same reason `csp` is: HTML
       §7.5.1 creates the Document from them and the creation is finished on the far side of however many
       suspensions the parse takes, by which time the header list this frame read them out of is gone. NULL for
       the no-response arm, which is §9.1 step 3's empty ordered map and is the whole of what §7.3.2.1's initial
       about:blank has to say about a header. */
    char *permissions_policy, *permissions_policy_report_only;
    EmbedderPolicyValue coep_value, coep_report_only_value;
    /* §7.1.1's ORIGINS, HELD AS POINTERS AND NOT COPIED, which is a statement about origin records rather than
       an exception to the paragraph above: an origin belongs to the agent and outlives every frame that names
       one (nothing in the load job frees these), and its IDENTITY is what §7.3.2.1's determine the origin
       returns for an `about:` destination — a copy would mint a second record and make the loaded document
       cross-origin to the document that navigated it. */
    const Origin *origin, *top_level_origin;
    OpenerPolicyValue opener_policy;
    SandboxFlags sandbox_flags;
    /* DOM §4.5's TYPE AND CONTENT TYPE for the Document this create is making — HTML §7.5.1's own two
       arguments, decided by §7.4.5's arm at the BEGIN and held until the realm exists. It is a VALUE and not
       a pointer to the computed type for the reason the policy items above are copied: the record outlives
       the frame that computed it, and a snapshot of this work item outlives the session. */
    DocumentKind kind;
    bool navigates;
    /* HTML §13.2.3.2's ANSWER FOR THIS RESPONSE, and the UTF-8 the parser is handed because of it. -1 is "no
       response", which is §7.4's initial about:blank and an address whose fetch failed: those get DOM §4.5
       Interface Document's default ("unless stated otherwise, a document's encoding is the utf-8 encoding"),
       which doc_rec_new already wrote, and there is nothing here to determine. */
    int encoding;
    /* THE DECODED ENTITY, OWNED HERE AND BORROWED BY THE LOAD until it is finished — core/loader/
       document_load.h's contract, and the reason this cannot be freed where the decode happens any more. */
    char *decoded;
    lxb_html_document_t *dom;
    DocumentLoad *load;   /* NULL for a destination with no response — §7.4's initial about:blank */
} NavCreateWork;

/* A COPY OR NOTHING. Every string this record holds is optional in exactly one sense — `about_base_url` and
   the CSP text are legitimately absent — and a NULL copy is the same positive statement the original was. */
static char *nav_strdup(const char *s)
{
    char *p;
    size_t n;
    if (s == NULL) return NULL;
    n = strlen(s);
    p = malloc(n + 1);
    CHECK(p != NULL, "navigable: OOM copying a §7.5.1 creation item across a suspension");
    memcpy(p, s, n + 1);
    return p;
}

static void nav_create_free(NavCreateWork *w)
{
    free(w->url); free(w->top_level_url); free(w->about_base_url);
    free(w->csp); free(w->self_origin); free(w->coep_endpoint); free(w->coep_report_only_endpoint);
    free(w->permissions_policy); free(w->permissions_policy_report_only);
    free(w->decoded);
    free(w);
}

/* HTML §7.5.1 "Shared document creation infrastructure"'s CREATE AND INITIALIZE A DOCUMENT OBJECT, OPENED —
   every item copied, the response decoded, the Document made, and its §7.5 subsection standing at its first
   item. Nothing has been parsed when this returns. */
static NavCreateWork *nav_create_begin(JSContext *ctx, uint32_t doc, const char *url, const char *top_level_url,
                                       const Origin *origin, const Origin *top_level_origin,
                                       OpenerPolicyValue opener_policy, bool navigates,
                                       const char *body, size_t body_len,
                                       const HeaderList *response_headers, const MimeType *computed_type,
                                       SerializedPolicyContainer policy,
                                       SerializedResponsePermissionsPolicy permissions_policy,
                                       const char *about_base_url, SandboxFlags sandbox_flags)
{
    NavCreateWork *w;
    int encoding = -1;
    char *decoded = NULL;
    size_t decoded_len = 0;

    /* A REALM THIS AGENT BUILDS IS A DOCUMENT OF THIS AGENT'S PRINCIPAL. §7.1.1's check says so here, at the
       one place a second realm is made, which is what lets the host boundary below carry only a SERIALIZATION:
       the identity never has to survive that round trip because it is already known to be this agent's. */
    DCHECK(origin != NULL && origin_same(origin, origin_agent()),
           "a realm was built for a document whose origin is not this agent's — an instance is an ORIGIN-KEYED "
           "agent cluster, so a cross-origin document is a second INSTANCE the host provisions and never a "
           "second realm in this heap");
    DCHECK(top_level_url != NULL && *top_level_url,
           "a child navigable's realm was asked for with no TOP-LEVEL CREATION URL — §8.1.3.5 reads it to "
           "decide whether the environment is a secure context, so the realm's platform surface depends on it "
           "and a builder handed nothing would install a surface belonging to no document");
    DCHECK(url != NULL,
           "a child navigable's Document was asked for with no ADDRESS — §7.5.1's creationURL is what "
           "`document.URL`, `location.href` and every relative URL this Document resolves are read off, and a "
           "Document created without one resolves them against nothing");
    DCHECK(g_realm_builder != NULL,
           "a same-origin child navigable was reached in an agent whose host declared no realm builder — a "
           "same-origin document is a second REALM in this heap, and only the host knows which platform "
           "surface a document of this build has; declare it with navigable_set_realm_builder");
    /* §7.1.5 STATES A CONSEQUENCE OF THE FLAG SET THIS AGENT CANNOT HONOUR, AND IT IS ASSERTED RATHER THAN
       IGNORED: the SANDBOXED ORIGIN flag "forces content into an opaque origin", and an opaque origin is same
       origin with nothing — so a Document carrying it is a document of a DIFFERENT agent cluster and belongs
       in another instance. Reaching here with it set means §7.3.2.1's determine-the-origin was asked with a
       different flag set than the one the Document is being created with, which is the "an operation takes
       its inputs with it" split getting one of its two answers from the wrong place.
       IT IS ASKED BEFORE THE PARSE and not after it, which is where it used to sit: a Document this build
       refuses is a Document it must not spend a response building. */
    DCHECK(!(sandbox_flags & SANDBOX_ORIGIN),
           "a realm of THIS agent was built for a Document whose active sandboxing flag set has the sandboxed "
           "origin browsing context flag — §7.1.5 forces such a Document into a fresh OPAQUE origin, which is "
           "same origin with nothing, so it is a second INSTANCE the host provisions and never a second realm "
           "in this heap; the origin this call was handed was determined from a different flag set");
    /* AND §7.1.7's CONTAINER, WHOLE, which crosses as bytes for the same reason the principal above does — see
       RealmBuilder. It is REQUIRED rather than derivable here: the two callers of this function differ in
       exactly this argument, because WHICH container a Document is created with belongs to the operation
       (§7.1.7's determine step over the response's own for a load, §7.3.2.1's clone of the creator's for a
       materialized about:blank), and no inspection of the navigable being built could recover it. */
    DCHECK(serialized_policy_container_exists(policy),
           "a realm was built with no §7.1.7 POLICY CONTAINER — every Document is created with one, and CSP "
           "§2.2 gives its list a self-origin, so a Document without one resolves `'self'` against nothing and "
           "reports its own scripts as blocked by its own policy");
    DCHECK(policy.embedder.endpoint != NULL && policy.embedder.report_only_endpoint != NULL,
           "a §7.1.4 EMBEDDER POLICY reached §7.5.1's creation with a null endpoint — the section spells an "
           "absent endpoint as the EMPTY STRING, so a null is a second absence with no meaning and the copy "
           "this record takes could not tell the two apart");

    /* HTML §13.2.3.2 "Determining the character encoding", THEN §13.2.3.1 "Parsing with a known character
       encoding" — the two halves of what this site used to do by handing the response's bytes to a parser that
       takes UTF-8 and hoping. The comment at the fetch (js_nav_load_step) named this as the decode still owed
       once the bytes stopped being destroyed a zone earlier, and this is it: the response's `Content-Type`,
       its BOM and its first 1024 bytes decide the encoding (core/html/html_encoding_sniff.h), and Encoding
       §6.1 Legacy hooks for standards' `decode` turns the body into the UTF-8 lexbor's tokenizer requires.
       THE CONTAINER DOCUMENT'S ENCODING IS §13.2.3.2's STEP 6, and `ctx` is the realm of the document that is
       performing this navigation — which for a child navigable IS the container document. Its origin is the
       same as this one's by the assertion at the top of this function (an instance is an ORIGIN-KEYED agent
       cluster, so a realm built here can only be same-origin), which is the first half of the step's own
       condition; the second half is the algorithm's and is applied there.
       A DECODE IS NOT A RE-ENCODE OF THE SAME BYTES: `decoded` is what the tree is built from, so the token
       byte ownership html_parse.h describes attaches to the decoded buffer's contents through lexbor's own
       copies — and this buffer is now OWNED BY THIS RECORD rather than freed at the call, because the §7.5
       load BORROWS it across every suspension until it is finished. */
    if (body) {
        /* ONE COMPONENT FOR BOTH STEPS, AND FOR THE `Content-Type` READ THEY RUN ON — this site used to hold
           the sniff, the decode and its own read of the header, and it was the ONLY entry that held any of
           them, so the other two handed a tokenizer that reads UTF-8 the response's own bytes. A rule spelled
           at one of three entries is not a rule; core/loader/document_load_decode.h is where it lives now,
           beside the WHICH-DOCUMENT dispatch it is the other half of. */
        encoding = document_load_decode(&decoded, &decoded_len, response_headers, body, body_len,
                                        document_encoding(ctx));
    }

    w = calloc(1, sizeof *w);
    CHECK(w != NULL, "navigable: OOM opening HTML §7.5.1's create and initialize a Document object");
    w->doc                      = doc;
    w->url                      = nav_strdup(url);
    w->top_level_url            = nav_strdup(top_level_url);
    w->about_base_url           = nav_strdup(about_base_url);
    w->csp                      = nav_strdup(policy.csp);
    w->self_origin              = nav_strdup(policy.self_origin);
    w->coep_endpoint            = nav_strdup(policy.embedder.endpoint);
    w->coep_report_only_endpoint = nav_strdup(policy.embedder.report_only_endpoint);
    w->permissions_policy       = nav_strdup(permissions_policy.enforced);
    w->permissions_policy_report_only = nav_strdup(permissions_policy.report_only);
    w->coep_value               = policy.embedder.value;
    w->coep_report_only_value   = policy.embedder.report_only_value;
    w->origin                   = origin;
    w->top_level_origin         = top_level_origin;
    w->opener_policy            = opener_policy;
    w->sandbox_flags            = sandbox_flags;
    w->navigates                = navigates;
    w->encoding                 = encoding;
    w->decoded                  = decoded;
    /* DOM §4.5's TYPE AND CONTENT TYPE, TAKEN HERE AND CARRIED, for CLAUDE.md's "an operation that becomes a
       work item takes its inputs with it": the pair is a fact about the RESPONSE this create was given, the
       computed type it is read from is the caller's stack record and is freed when that frame returns, and
       the realm this pair is for is not built until a later STEP of this same work item. Reading it back off
       anything at that point would be reading it at the wrong time. §7.3.2.1 "Creating browsing contexts"'s
       constant is the answer for the no-response arm — this create's own §7.4 initial `about:blank`. */
    w->kind = computed_type ? document_load_kind(computed_type) : document_kind_initial_about_blank();
    w->dom = child_document(body ? decoded : NULL, decoded_len, computed_type, encoding, &w->load);
    return w;
}

/* HAS THE DOCUMENT'S §7.5 SUBSECTION NOTHING LEFT TO FILL? TRUE with no load at all, which is §7.4's initial
   about:blank and a positive statement rather than a hole: that Document came from no response. */
static bool nav_create_ended(const NavCreateWork *w)
{
    DCHECK(w != NULL, "nav_create_ended was asked of no creation");
    return w->load == NULL || document_load_ended(w->load);
}

static void nav_create_step(NavCreateWork *w)
{
    DCHECK(w != NULL, "nav_create_step was asked of no creation");
    DCHECK(!nav_create_ended(w),
           "§7.5.1's creation was stepped after its §7.5 subsection had nothing left to fill — "
           "nav_create_ended is what a driver's loop tests, and asking past it is a driver that did not");
    document_load_step(w->load);
}

/* THE FLOW THAT WAS BUILDING THIS DOCUMENT IS GONE — the machine's teardown, and the only place that owns
   what a half-built creation holds. NOTHING ELSE CAN EVER COLLECT IT: no realm was built, so no navigable and
   no world row names this Document, and the parse's own lexbor parser is reachable from it alone. */
static void nav_create_abandon(NavCreateWork *w)
{
    if (w->load) {
        document_load_abort(w->load);
        w->load = NULL;
    }
    if (w->dom) {
        dom_document_destroy(w->dom);
        w->dom = NULL;
    }
    nav_create_free(w);
}

/* §7.5.1's CREATE AND INITIALIZE A DOCUMENT OBJECT, FINISHED — the parse's own end, then the realm the host
   builds around the finished tree, then §7.4.6.2's update the document for a navigation. */
static JSContext *nav_create_finish(JSContext *ctx, NavCreateWork *w, JSValueConst nav_proxy)
{
    SerializedPolicyContainer policy;
    JSContext *cctx;

    DCHECK(w != NULL, "nav_create_finish was asked of no creation");
    DCHECK(nav_create_ended(w),
           "§7.5.1's creation was finished with response bytes still unfilled — the realm is built around the "
           "PARSED tree (document_policy_new walks it for CSP §3.3's `<meta>` policies), so a Document handed "
           "over here would be judged under the policies of a prefix of itself");
    if (w->load) {
        CHECK(document_load_finish(w->load) == LXB_STATUS_OK, "a child navigable's Document did not parse");
        w->load = NULL;
    }
    /* THE CONTAINER, REBUILT FROM THE ITEMS THIS RECORD COPIED. It is assembled here rather than held as a
       struct because what a struct of borrowed pointers survives is a call and not a park. */
    policy = serialized_policy_container(w->csp, w->self_origin,
                                         serialized_embedder_policy(w->coep_value, w->coep_endpoint,
                                                                    w->coep_report_only_value,
                                                                    w->coep_report_only_endpoint));
    /* THE HOST IS HANDED THE SERIALIZATION, because a host builds a platform surface and does not decide a
       principal — and because the identity it would have to carry is this agent's, asserted at the begin. */
    cctx = g_realm_builder(JS_GetRuntime(ctx), w->dom, w->url, w->top_level_url, origin_serialized(w->origin),
                           w->kind, policy,
                           serialized_response_permissions_policy(w->permissions_policy,
                                                                  w->permissions_policy_report_only),
                           w->sandbox_flags, w->doc, nav_proxy);
    CHECK(cctx != NULL, "the host's realm builder produced no realm for a same-origin child navigable");
    /* AND §13.2.3.2's ANSWER ONTO THE DOCUMENT IT IS ABOUT, for the same reason and in the same place as the
       about base URL below: it is a fact the OPERATION determined and the host's realm builder cannot answer.
       It is written here rather than before the parse because the Document RECORD is what carries it and the
       record is the realm builder's product — the bytes the parse consumed were already decoded with it. */
    if (w->encoding >= 0) document_set_encoding(cctx, w->encoding);
    /* HTML §7.4's ABOUT BASE URL, WRITTEN BEFORE ANYTHING RESOLVES A URL IN THIS DOCUMENT. It is §2.4.3's
       fallback base URL for a Document addressed `about:blank`, so §4.2.3's freeze and every relative
       reference read it — which is why it is set here, between the realm's construction and the scripts
       seeded below, rather than left for the first reader. document_set_about_base_url asserts the half of
       that ordering it can see (nothing has frozen a base element's URL yet).
       IT DOES NOT GO THROUGH THE RealmBuilder, and that is deliberate rather than a shortcut: the builder is
       the HOST's — it declares which platform surface a document of this build gets — and whose base URL a
       created Document inherits is a fact about the OPERATION §7.4 is performing, which is this component's.
       A host cannot answer it and would have to be told, which is a second protocol for one fact. */
    if (w->about_base_url && *w->about_base_url) document_set_about_base_url(cctx, w->about_base_url);
    if (g_realms_n == g_realms_cap) {
        int cap = g_realms_cap ? g_realms_cap * 2 : 8;
        JSContext **g = realloc(g_realms, (size_t)cap * sizeof *g);
        CHECK(g != NULL, "navigable: OOM recording a child realm");
        g_realms = g;
        g_realms_cap = cap;
    }
    g_realms[g_realms_n++] = cctx;
    /* THE CENSUS THIS SITE OWES, AT THE ONE PLACE A CHILD REALM COMES INTO EXISTENCE. `made` never falls and
       `peak` never falls, so the pair states what the live count above cannot: how many this run has built and
       how many were alive at once. The assert is the same three-number law the teardown states from the other
       end, written here rather than shared, so that a break reports the site that broke it. */
    g_realms_made++;
    if (g_realms_n > g_realms_peak) g_realms_peak = g_realms_n;
    DCHECK(g_realms_n <= g_realms_peak && g_realms_peak <= g_realms_made,
           "a child realm was recorded and this component's census came out inconsistent — the live count can "
           "never exceed the high-water it just set, and that high-water can never exceed the monotone total "
           "of realms ever recorded. Reaching this means the list gained a member through a path that is not "
           "this one, so `made` under-counts and the run's reclamation reading (test_forced.c's "
           "`realm-reclaim` row, `_heap.childRealmsMade`) is measured against a total that is not the total");
    /* AND THE DOCUMENT RUNS ITS OWN SCRIPTS — see navigable_seed_scripts. AFTER the realm is recorded, because
       a program names its document and the document's realm has to be the one this agent is holding by then;
       BEFORE the reference handoff below, so nothing here reads a realm whose only reference has just gone.
       The initial about:blank reaches this with an empty tree and seeds nothing, which is §7.4's own fact
       rather than a case to test for: that Document came from no response and has no scripts by construction. */
    navigable_seed_scripts(cctx, w->dom, w->doc);
    /* THE BUILDER'S REFERENCE IS HANDED TO THE NAVIGABLE — see the list's own note for why the agent must not
       keep one. What holds the realm afterwards is its own graph: the WindowProxy holds the child's Window, the
       Window's methods are C function objects, and every one of those holds a counted reference to the realm
       that defined them. So the count cannot be at one here, and if it were, this line would free the realm
       that all three of this function's callers are about to read. */
    DCHECK(JS_ContextRefCount(cctx) > 1,
           "a child realm was built holding nothing but the builder's own reference — its own function objects "
           "are what keep a realm alive, and a realm with none of them is not a realm this agent can hand to a "
           "navigable");
    JS_FreeContext(cctx);
    /* §7.4.6.2's UPDATE THE DOCUMENT, and §7.5.1's OPENER POLICY ROW for the Document this navigation creates
       — "opener policy … navigationParams's cross-origin opener policy", which is §7.4.5's responseCOOP. It
       moves with the rest of the binding because a navigation is what replaces a navigable's active document,
       and it is HERE rather than at the load job's frame because the frame that determined it has already
       parked and come back: the record is what carried those five facts across the suspension. */
    if (w->navigates)
        window_proxy_navigate(ctx, nav_proxy, cctx, w->doc, w->url, w->top_level_url,
                              w->top_level_origin, w->origin, w->opener_policy);
    nav_create_free(w);
    return cctx;
}

/* THE ONE-CALL FORM, for the caller with no response and therefore nothing to step: `proxy_realm`
   materializing §7.4's initial about:blank, whose markup is a constant of this build. A caller arriving here
   WITH a response has skipped the pull — the parse of a response is O(document) and the loop over it belongs
   to the step machine that can park between its items — and that is asserted rather than described. */
JSContext *navigable_realm(JSContext *ctx, uint32_t doc, const char *url, const char *top_level_url,
                           const Origin *origin, JSValueConst nav_proxy, const char *body, size_t body_len,
                           const char *content_type, const MimeType *computed_type,
                           SerializedPolicyContainer policy,
                           const char *about_base_url, SandboxFlags sandbox_flags)
{
    NavCreateWork *w;

    (void)body; (void)body_len; (void)content_type; (void)computed_type;
    DCHECK(body == NULL && body_len == 0 && content_type == NULL && computed_type == NULL,
           "a Document was created from a RESPONSE through the one-call realm entry — §7.4.5's response arm is "
           "the load job's, where it is opened with nav_create_begin, stepped one item at a time and finished "
           "on the other side of however many suspensions the frontier asked for. This entry exists for the "
           "destination that has no response at all, and driving a response through it would hold the thread "
           "for the length of the document inside whatever flow happened to be running");
    /* NO RESPONSE, SO NO §9.1 HEADER VALUES — and that is §9.6 answering rather than this call declining to.
       §9.1 step 3 gives an absent header the empty ordered map, so §9.6's merge copies nothing and the initial
       about:blank's permissions policy is exactly §9.5's, which is what §7.3.2.1 creates it with. It is stated
       here rather than defaulted because serialized_response_permissions_policy is the only constructor and a
       caller that stops stating it stops compiling. */
    w = nav_create_begin(ctx, doc, url, top_level_url, origin, NULL, OPENER_POLICY_UNSAFE_NONE, /*navigates*/false,
                         NULL, 0, NULL, NULL, policy,
                         serialized_response_permissions_policy(NULL, NULL), about_base_url, sandbox_flags);
    DCHECK(nav_create_ended(w),
           "§7.4's initial about:blank was opened as a creation with items left to fill — its markup is a "
           "constant of this build and reaches child_document as the no-response arm, which takes no load "
           "handle at all, so a steppable one here is a response that arrived through the entry above");
    return nav_create_finish(ctx, w, nav_proxy);
}

/* §7.4 STEP 14's NAVIGATE IS A SCHEDULED WORK ITEM, and it has to be one because NOT ONE of its three callers
 * can wait. §4.8.5's iframe insertion runs inside the tree-construction steps, which are plain C with no flow
 * to suspend; `window.open()` hands back a WindowProxy at its own call site; §4.6.3's activation behaviour runs
 * inside a dispatch. The spec says the same thing from the other end — `open()` RETURNS before the destination
 * loads, and a page that reads `contentDocument` in the creating turn sees the initial about:blank rather than
 * where the frame is going.
 *
 * SO THE LOAD IS A JOB, which in this engine is a call-root FLOW: preemptible, forkable, parkable. That last
 * one is the point, and it is what the fetch needs. §7.4 step 14 FETCHES the destination, the network belongs
 * to the host, and a host-owed answer SUSPENDS the asking flow — so the load asks `document.fetch<TAB><provenance><TAB><url>`
 * and returns JS_STEP_YIELD, siblings run while the response is in flight, and the job resumes with `{body,
 * csp}` in hand. That is why none of the three callers could have done this themselves: each of them is a
 * place where nothing can suspend, and the job is a place where everything can.
 *
 * A HOST THAT DOES NOT ANSWER LEAVES THE FLOW PARKED, which is the honest state and a visible one — the
 * navigable exists, named and counted, showing the document it had. The alternative this replaced was a
 * SYNCHRONOUS fetcher the host installed, which only a host with a synchronous network could implement: the
 * product host's network is the trusted zone's and parks on every request, so it installed none and a DCHECK
 * fired the moment anything navigated. One shape now serves both, because it is the shape every other
 * host-owed answer in this engine already has.
 *
 * ONE OPERATION, AND THE JOB CARRIES ITS DESTINATION. The job is handed the ADDRESS and the ORIGIN it is
 * loading, never the ones already on the proxy: a navigation's destination is not the navigable's current
 * address, and a first version of this read the address off the proxy whenever the proxy had no realm yet.
 * That is right for §7.4's create — the navigable's address IS the destination there — and silently WRONG for
 * §7.1's choose-an-existing-navigable, where `window.open("post-to-top.html","iExist")` navigates a srcless
 * `<iframe name=iExist>` whose own address is about:blank and whose realm nothing has touched. The destination
 * was discarded and the navigable materialized empty; the test timed out waiting for a message from a document
 * that was never loaded. The address travels with the job.
 *
 * WHAT THE NAVIGABLE'S STATE STILL DECIDES IS ITS DOCUMENT'S IDENTITY, and only that. A navigable with a
 * realm already is being NAVIGATED, and §7.4.2.2 supersedes a document, so the new one gets a NEW name — the
 * world registry names documents rather than navigables, and a flow parked in the superseded one must still be
 * able to say where it is. A navigable with no realm yet is receiving its FIRST document, whose name §7.4
 * already minted and adopted when it created the navigable. Either way the realm is built at the job's address
 * and all five facts of the binding move together.
 *
 * THE CALLEE IS MINTED IN THE ENQUEUING REALM. A C function runs in the realm that DEFINED it, and this one
 * reads §7.2.6's inherited policy off `ctx` — one held in a static would clone the wrong document's. */
typedef struct {
    JSStepHdr hdr;
    uint32_t  req;   /* the host-owed response, 0 before it is asked for */
    /* §7.5.1's CREATION, ONCE §7.4.5 HAS DETERMINED EVERYTHING IT IS MADE OF — the one thing that crosses this
       machine's stage boundary. It is a POINTER and not the fifteen values it holds because those values are
       borrowed text and C locals, and a park is exactly what neither survives (see NavCreateWork). NULL until
       the response has been turned into one, and NULL again the moment it is finished. */
    NavCreateWork *create;
} NavLoadState;

/* WHERE THIS MACHINE RESTS. It is §7.4 step 14's load as a JOB, and it rests in TWO places because the
   standard's own §7.4.5 hands off to a §7.5 subsection whose work is O(DOCUMENT). The wait for the host's
   answer is a sub-sequence WITHIN the first (`req` is its cursor) and not a step of its own — no page code of
   this document runs across it, because the document does not exist yet.
   THE SECOND STAGE IS A STAGE PER ITEM AND NOT A SPAN, which is JSTrampStepDef.steps' own rule: "a span over
   anything of the PAGE'S SIZE (a list, a tree, a string, a collection, a PARSE) is not a range at all: it is a
   stage per step, and the stage that walks returns JS_STEP_YIELD at every turn, so the scheduler is ASKED at
   each one and answers from the frontier rather than from the algorithm". It used to be inside the first
   stage, which made that stage a declaration this machine could not keep: a response-sized parse with no rest
   point in it, inside a flow's slice, unpreemptible and unparkable. */
#define NAV_LOAD_ALGORITHM "HTML §7.4 step 14's load"
#define NAV_LOAD_STAGES(X) \
    X(NAV_LOAD_FETCH, "HTML §7.4 step 14 → §7.4.5 populate a session history entry (fetch the destination, " \
                      "then §7.1.7's policy container, §7.1.3's opener policy, and OPEN §7.5.1's create and " \
                      "initialize a Document object over the response)") \
    X(NAV_LOAD_CREATE, "HTML §7.5.2 \"Loading HTML documents\" / §7.5.3 \"Loading XML documents\" / §7.5.4 " \
                       "\"Loading text documents\" (fill the parser's input byte stream with the fetched " \
                       "bytes, one item per step), then §13.2.7 \"The end\" and §7.4.6.2's update the " \
                       "document)")
enum { NAV_LOAD_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const NAV_LOAD_STEPS[] = { NAV_LOAD_STAGES(JS_STEP_STAGE_LABEL) NULL };

/* HTML §7.1.4.2 "Embedder policy checks"' STEPS 1 AND 2 — the half of check-a-navigation-response's-adherence-
 * to-its-embedder-policy that is about the NAVIGABLE — and then the check itself, which is §7.1.4's
 * (core/frame/embedder_policy.h states why the algorithm is split exactly here).
 *
 * BOTH READS HAPPEN AT THE CHECK, OFF THE NAVIGABLE THE STANDARD HANDS THE ALGORITHM, and neither is taken
 * from the job. That is the one thing to get right here. The load job carries an INITIATOR's policy container
 * — §7.1.7's clone, whose owner the OPERATION decides — and reading its embedder policy as parentPolicy would
 * be right for every navigation a parent starts in its own frame and WRONG for `frames[0].location = …` or a
 * `target`ed form, where the initiator is a third document and the container document is whoever's element
 * presents this navigable. §7.1.4.2 dereferences the navigable, so the answer is the container document THIS
 * MOMENT: an arm of a fork that removed the `<iframe>` reaches no container at all, and its sibling still does,
 * out of the one per-flow relation window_proxy_container already answers.
 *
 * WHAT THE NAVIGABLE'S OWN STATE DECIDES IS EXACTLY THIS — which tree it hangs in — and nothing about the
 * response, which is why the response's policy arrives as an argument from the frame that fetched it. */
static void nav_check_embedder_policy_adherence(JSContext *ctx, JSValueConst proxy,
                                                SerializedEmbedderPolicy response_policy)
{
    JSValue parent_nav, container;
    const lxb_dom_node_t *n;
    const PolicyContainer *container_policy;
    bool adheres;

    /* Step 1: "If navigable is not a child navigable, then return true." §7.3.1.3 "Child navigables" defines
       the term over the PARENT — a navigable "is a child navigable", "which means that its parent is non-null"
       — not over the container, so this is asked of the parent link. The two agree for everything §7.3 creates
       (§7.3.1.3's create-a-new-child-navigable is the only algorithm handed an element, and §7.3.1.7 step 8's
       auxiliary navigable is a top-level traversable nested through nothing), and asking the one the definition
       names is what keeps them from having to. */
    parent_nav = window_proxy_parent_navigable(ctx, proxy);
    if (!window_proxy_is(parent_nav)) {
        JS_FreeValue(ctx, parent_nav);
        return;
    }
    JS_FreeValue(ctx, parent_nav);
    /* Step 2: "Let parentPolicy be navigable's CONTAINER DOCUMENT's policy container's embedder policy."
       §7.3.1.3's container document is "navigable's container's node document" — the active document of the
       navigable this one is nested in, reached through the element rather than through the parent navigable
       because that is what the definition says and because the element is what a per-flow detach clears. */
    container = window_proxy_container(ctx, proxy);
    DCHECK(!JS_IsNull(container),
           "§7.1.4.2 step 2 needs the CONTAINER DOCUMENT of a navigable whose parent is non-null, and "
           "§7.3.1.3's container reads back null — a child navigable IS the content navigable of its container, "
           "so the two halves of that sentence have come apart. Either §7.3.1.6's destroy-a-child-navigable "
           "severed the relation and this navigation is being populated into a destroyed navigable, or a child "
           "navigable was created somewhere that did not record its element (core/frame/navigable.c's create is "
           "the one writer)");
    n = node_of(container);
    /* The node belongs to the container document's tree and not to the wrapper, so it outlives the reference
       released here — an element that is a navigable's container is by definition still in that tree. */
    JS_FreeValue(ctx, container);
    DCHECK(n != NULL && n->type == LXB_DOM_NODE_TYPE_ELEMENT && n->owner_document != NULL,
           "§7.3.1.3's container of a navigable is a NAVIGABLE CONTAINER ELEMENT in a document, and what this "
           "navigable holds is not one — the create that records it is handed the element itself, so anything "
           "else is a slot written by something that is not that algorithm");
    container_policy = document_policy_of(n->owner_document);
    DCHECK(container_policy != NULL,
           "§7.1.4.2 step 2 read the policy container of a navigable's CONTAINER DOCUMENT and there is none — "
           "§7.1.7 gives every Document a policy container, initially a new one holding a new embedder policy, "
           "so an absent one is a Document created by a path that did not state it rather than a document "
           "without policies");
    adheres = embedder_policy_check_navigation_response(
                  serialized_embedder_policy_of(policy_container_embedder(container_policy)), response_policy);
    /* §7.1.4.2's own steps QUEUE before they answer, so the call is not side-effect-free and must not stand
       inside a DCHECK's condition — a release build compiles the condition away and the check would then never
       run at all. It is made here, unconditionally, and its answer is read below. */
    (void)adheres;
    /* §7.1.4.2 step 6's FALSE, which §7.4.5 turns into a blocked navigation: "set entry's document state's
       document to the result of creating a document for INLINE CONTENT THAT DOESN'T HAVE A DOM … make document
       unsalvageable given … \"navigation-failure\"". None of that exists here — §7.5.7 is named by
       core/loader/document_load_type.c's DOC_LOAD_EXTERNAL arm and built by nothing — so a false answer would
       otherwise fall through to child_document below and load the frame a browser refuses.
       IT SITS BEHIND THE CRASH INSIDE THE CHECK, and that is §7.1.4.2's own order rather than dead code: step 5
       queues the enforce report before step 6 returns, so whoever builds Reporting arrives here next. */
    DCHECK(adheres,
           "HTML §7.4.5 \"Populating a session history entry\" BLOCKS this navigation — §7.1.4.2's check a "
           "navigation response's adherence to its embedder policy returned false, because the container "
           "document enforces a `Cross-Origin-Embedder-Policy` compatible with cross-origin isolation and this "
           "response opted into none. The navigable must get §7.5.7 \"Loading a document for inline content "
           "that doesn't have a DOM\"'s Document instead of the response's, marked unsalvageable with "
           "\"navigation-failure\", with the reserved environment discarded — build that error Document as a "
           "value this load carries, the same shape §7.4.5's network-error arm above still needs");
}

/* THE RESPONSE IS `{url, body, headers}` — one answer, because everything §7.5.1 creates a Document from is a
   property of THE RESPONSE and asking for any of it separately is asking a second time and may get a second
   answer. It was `{body, csp}`, one policy the trusted zone had pulled out of the list, and that is why a
   navigated Document could not have an opener policy, an embedder policy or an `Origin-Agent-Cluster` answer
   at all: the seam had thrown the rest away. `headers` is the HTTP field lines the response delivered, which
   is the same form qjs_init takes for the root document's response — one shape, one parse (core/fetch/
   headers.h), and Fetch's own `get` is then what decides what a REPEATED header means. A missing or null
   `body` is a fetch that did not load, which is a document the navigable still gets (an error page is a
   document).

   `url` IS Fetch §2.2.6 "Responses"' RESPONSE URL — "a pointer to the last URL in response's URL list" — AND
   IT IS NOT THE ADDRESS THIS JOB REQUESTED. Every algorithm below that this frame runs over the destination
   is written by the standard over the RESPONSE's URL and not over the request's: §7.4.5's determine-the-origin
   ("given response's URL, finalSandboxFlags, and entry's document state's initiator origin"), §7.1.7's
   determine-navigation-params-policy-container ("given a URL responseURL"), CSP §2.2.2's self-origin
   ("self-origin is response's URL's origin"), and §7.5.1's Document address. A redirect is what separates the
   two, and it separates them by ORIGIN: a request to this instance's own origin that 302s off it produced a
   Document this frame determined the origin of from the address it ASKED for, so the cross-agent question one
   algorithm down was answered `true` for a Document that belongs to a PEER, and the Document was created in
   this heap under a principal the response contradicts. That is the defect the origin's move from the enqueue
   to this job was made to fix, and it was only half fixed: the move corrected the TIME the question is asked
   and left it reading the wrong URL, which is silent in both hosts because a request that does not redirect
   makes the two strings equal.
   IT IS THE RESPONSE'S AND NOT THE LOCATION URL, which §7.4.5 states in its own note beside the step: "if
   response is a redirect, then response's URL will be the URL that led to the redirect to response's location
   URL; it will not be the location URL itself." A host that FOLLOWS redirects reports the URL of the response
   it finally received, which is that same URL for the response this frame is handed; a host that follows none
   reports the request's, which is a positive statement about its own network and not an absence. */
static int js_nav_load_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    NavLoadState *s = st;
    JSValueConst proxy = step_arg(&s->hdr, 0);
    JSValueConst answer = JS_UNDEFINED;
    const char *addr, *tlu;
    /* Fetch §2.2.6 "Responses"' RESPONSE URL, OWNED — the string the answer above carries, released with
       `addr` at the one exit this frame has past the fetch. NULL for a destination that made no request, which
       is the same fact `fetches` states one line down and is why the borrowed pointer below is separate. */
    const char *resp_url = NULL;
    /* AND THE URL EVERY ALGORITHM BELOW IS RUN OVER, BORROWED from one of the two above. §7.4.5 builds
       navigation params three ways and only create-navigation-params-BY-FETCHING has a response: its
       responseURL is the response's, while the srcdoc and non-fetch-scheme constructors are written over
       ENTRY'S URL, which is the address this job was enqueued with. So the `about:` arm taking `addr` is the
       standard's own other constructor and not this frame defaulting past an absent field. */
    const char *dest_url;
    const uint8_t *body = NULL;
    /* §7.4.5's `responseOrigin` and §7.4.2.2's `initiatorOriginSnapshot`, which are TWO values and were one.
       The second rides the job (navigable_load_enqueue); the first is DETERMINED below, at the response, out
       of the second and the final sandboxing flag set — which is the order §7.4.5 states and the only order in
       which both of §7.3.2.1's inputs exist. */
    const Origin *origin = NULL, *initiator, *tlo;
    /* §4.7's RECORD FOR THE DESTINATION, because §7.3.2.1's determine-the-origin reads the URL and not its
       serialization: its steps 3 and 4 are §2.4.1's two match relations over a parsed record, and step 5 is
       §4.7 over one. */
    UrlRecord dest;
    bool dest_parsed;
    /* CSP §2.2's SELF-ORIGIN FOR THE RESPONSE'S OWN LIST, which §2.2.2 defines as "response's URL's origin" —
       a URL's origin SERIALIZED, and never the origin §7.3.2.1's determine-the-origin answered. The two agree
       for every response that sends no `sandbox` directive, and that is exactly the coincidence this frame
       used to rest on. */
    char *response_self = NULL;
    JSValue bodyv = JS_UNDEFINED, headersv = JS_UNDEFINED, urlv = JS_UNDEFINED;
    /* §7.1.7's CLONE OF THE INITIATOR'S CONTAINER, carried on the job because whose clone it is belongs to the
       OPERATION — the CREATOR's when §7.4 creates a navigable, the INITIATOR's when §7.4.2.2 navigates one —
       and by the time this job runs the only document it could ask is the one being replaced. It is ALWAYS
       there: every operation that enqueues a load has a document, and every document has a container. */
    SerializedPolicyContainer inherited;
    /* AND §7.1.7's create-a-policy-container-from-a-fetch-response for THIS load's own response. Its CSP list
       is §2.2.2's pair — the response's policies and the destination's origin — which is also the right answer
       when the destination had no response at all, since §7.1.7 step 5's new container needs a self-origin too
       and this Document's own address's origin is what §2.2.2 would have given. */
    SerializedPolicyContainer response;
    /* §7.1.7's DETERMINE NAVIGATION PARAMS POLICY CONTAINER over those two — the container this Document is
       actually created with, and the one §7.4.5 reads the CSP-derived sandboxing flags off. */
    SerializedPolicyContainer policy;
    const char *inherited_csp = NULL;
    const char *inherited_self = NULL;
    /* §7.1.4's EMBEDDER POLICY of the initiator's container, item by item, because the job carries the whole
       container and §7.1.7's clone moves every item at once. The two VALUES arrive as §7.1.4's own tokens
       rather than as integers — see embedder_policy.h — and the two endpoints as the strings they are. */
    const char *inherited_coep = NULL;
    const char *inherited_coep_endpoint = NULL;
    const char *inherited_coep_report_only = NULL;
    const char *inherited_coep_report_only_endpoint = NULL;
    SerializedEmbedderPolicy inherited_ep;
    /* §7.1.7's create-a-policy-container-from-a-fetch-response step 4's OWN item for THIS load's response —
       "the result of obtaining an embedder policy given response and environment". FILLED WHERE THE ANSWER IS
       DECIDABLE rather than here: §7.1.4's obtain needs the environment's secure-context answer, which is not
       known until `tlu` is, and the record OWNS two strings so an init above this frame's two yields would
       allocate a pair on every resume. */
    EmbedderPolicy response_ep;
    const char *about_base = NULL;
    const char *field_lines = NULL;
    /* THE RESPONSE'S WHOLE HEADER LIST, because several standards read different names out of ONE list and
       Fetch's own `get` is what decides what a repeated header means — the same sentence main.c's qjs_init
       makes about the root document's response, and the reason this seam carries field lines rather than one
       extracted policy. It carried `{body, csp}`, one policy pulled out by the trusted zone, so §7.1.3's
       opener policy and §7.1.4's embedder policy could not be obtained for a navigated Document at all. */
    HeaderList response_headers;
    /* §7.4.5's "let responseCOOP be a NEW opener policy" — the value every arm keeps except a top-level
       traversable's fetch, which is the only place §7.4.5 obtains one. See below. */
    OpenerPolicy response_coop;
    char *resp_csp = NULL;   /* owned by header_list_get and freed with free(), NOT a JS_ToCString */
    /* PERMISSIONS POLICY §9.1 step 1 and Fetch's half of its step 2, for BOTH names — §7.5.1 runs §9.6 with the
       enforced value and §10.1 inserts a second call with the report-only one, so one response feeds two
       policies and this frame reads both. Same ownership as `resp_csp`, and freed with it. */
    char *resp_pp = NULL, *resp_pp_report_only = NULL;
    /* §7.4.5's COMPUTED TYPE for this response — a record this frame owns and frees on every path.
       `computed_defined` is "there was a response to compute it from" as a positive statement: a fetch that
       did not load has no resource, which is a DIFFERENT fact from a response that carried no `Content-Type`
       (that one HAS bytes and is exactly what MIME Sniffing §7 exists to answer for). */
    MimeType computed;
    bool computed_defined = false;
    /* §7.1.3.2's `swapGroup`, once its arm has RUN — so the whole of the load below it is skipped. It is not a
       second copy of the predicate's answer: a load that swapped has no Document to create in this heap and no
       binding to move, and one flag is what keeps those two facts from being asked twice. */
    bool swapped = false;
    size_t body_len = 0;
    SandboxFlags final_flags, csp_flags;
    uint32_t doc, oid = 0;
    int orc;
    bool fetches;

    (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);
    mime_type_init(&computed);
    memset(&response_headers, 0, sizeof response_headers);
    opener_policy_init(&response_coop);   /* §7.4.5: "Let responseCOOP be a new opener policy" */
    DCHECK(window_proxy_is(proxy), "the document-load job was given something that is not a WindowProxy");
    /* THE ENTRY-STAGE TRANSFER, GENERATED FROM THE STAGE LIST — there is no `else` and no un-named span, so a
       stage added to the declaration with no arm does not link and an arm naming no stage does not compile.
       Everything above it runs on EVERY entry, which is a statement about those lines rather than an accident
       of where a guard happened to land. */
    STEP_DISPATCH(NAV_LOAD_STAGES, s->hdr.stage, NAV_LOAD_ALGORITHM, JS_STEP_ABRUPT);

    STEP_ARM(NAV_LOAD_FETCH);
    addr = JS_ToCString(ctx, step_arg(&s->hdr, 1));
    if (!addr) return JS_STEP_ABRUPT;
    /* CLAUDE.md §A-REQUEST-CARRIES-THE-PROVENANCE's word for this navigation, RIDING THE JOB — asserted once
       here for both readers of it below (the request the host is asked to perform, and §7.1.3.2's swap record
       when the response's opener policy forces a new browsing context group). It is stated by the OPERATION
       that enqueued this load and never asked of the flow running now: this is a task, and the flow that runs
       it need not be the flow that queued it. */
    DCHECK(JS_IsString(step_arg(&s->hdr, 10)),
           "a document load carried no PROVENANCE — navigable_load_enqueue asserts one on every path and puts "
           "it on the job because §scheduler's \"an operation that becomes a work item takes its inputs with "
           "it\" applies to what a request is evidence of exactly as it applies to its address; the trusted "
           "zone decides whether to spend the network, and whose session, on this one word");
    /* IS THERE ANYTHING TO FETCH — one spec fact about the SCHEME, asked where the fetch is. `about:blank` has
       no response and no content, so navigating to it produces a Document from nothing: asking the host for it
       would be a GET of the literal text `about:blank`, and every host would answer 404 or nothing at all.
       It reaches here because §7.4.2.2 navigates to `about:blank` for real (the corpus does it while an
       initial load is still pending); §7.4's create skips the job entirely for such an address, which is a
       different decision — deferring materialization — made for a different reason. */
    fetches = strncmp(addr, "about:", 6) != 0;
    /* AND `about:` IS NOT THE ONLY NON-FETCHING SCHEME, WHICH IS WHY THE QUESTION ABOVE IS ASSERTED AND NOT
       JUST ANSWERED. §7.4.2.2 "Beginning navigation" branches on the scheme BEFORE anything this job does —
       "if url's scheme is `javascript`: queue a global task on the navigation and traversal task source …
       to navigate to a javascript: URL … " and RETURN — and §7.4.2.3.2 "The javascript: URL special case"
       says of the only request it ever builds: "This is a synthetic request solely for plumbing into the next
       step. It will never hit the network."
       SO A `javascript:` URL ARRIVING HERE IS A NAVIGATION THAT SKIPPED THAT BRANCH, and what this job would
       do with it is put the PROGRAM ITSELF on the wire: a `javascript:` URL has an opaque path and no host, so
       the request target is the script source — spaces, quotes and all — and the authority falls back to a
       default. The reply cannot be a document, and the popup that was supposed to run the program instead
       reports a load that failed, which is a symptom with no `javascript:` anywhere in it.
       WHERE THE ROUTE IS: navigable_navigate serves §7.4.2.2 step 21's arm and every navigation converges
       there, so a `javascript:` URL reaching THIS job is one that got here without passing through it — which
       today means §7.4's create, whose fold of §7.3.1.7 step 8 and §7.2.2.1 step 15 crashes at child_address
       with its own message rather than enqueuing a load. This assert is what keeps a THIRD route from being
       added without the branch. */
    DCHECK(strncmp(addr, "javascript:", sizeof "javascript:" - 1) != 0,
           "§7.4 step 14's document-load job reached its FETCH with a `javascript:` URL — §7.4.2.2 "
           "\"Beginning navigation\" step 21 routes that scheme to §7.4.2.3.2 \"The javascript: URL special "
           "case\" and returns, and that section's own request never hits the network. This job would send the "
           "script source as the request target. The branch is in navigable_navigate, which every navigation "
           "converges on, so whatever enqueued this load bypassed it");
    if (fetches && !s->req) {
        /* `document.fetch<TAB><provenance><TAB><url>` — THE ADDRESS LAST, WHICH IS THE PENDING LINE'S OWN
           SHAPE (solver/engine.h joins `METHOD<TAB>DESTINATION<TAB>INITIATOR<TAB>PROVENANCE<TAB>CREDENTIALS<TAB>URL`) and is
           the only ordering a host can split without knowing how many fields there will be next time: every
           fixed-vocabulary token comes first and the URL is the remainder. This record carried the address
           ALONE, and a host handed an address and nothing else has no way to tell a navigation a real client
           makes from one that exists only because a gate was forced — so one host declined all of them and the
           other fetched all of them with the person's cookies. */
        const char *prov = JS_ToCString(ctx, step_arg(&s->hdr, 10));
        char *op;

        CHECK(prov != NULL, "navigable: OOM taking §7.4.5's provenance off the load job");
        op = malloc(strlen(addr) + strlen(prov) + 20);
        CHECK(op != NULL, "navigable: OOM building §7.4 step 14's fetch request");
        sprintf(op, "document.fetch\t%s\t%s", prov, addr);
        s->req = engine_host_request(ctx, op);
        free(op);
        JS_FreeCString(ctx, prov);
        JS_FreeCString(ctx, addr);
        return JS_STEP_YIELD;   /* park on the response; siblings run meanwhile */
    }
    if (fetches && !engine_host_answered(s->req, &answer)) {
        JS_FreeCString(ctx, addr);
        return JS_STEP_YIELD;
    }
    /* §7.4.2.2's initiatorOriginSnapshot, CARRIED AS A HANDLE. It is an INPUT of the operation — the origin of
       the document whose script ran — and a handle is what preserves its identity across a JSValue: §7.3.2.1's
       determine-the-origin steps 3 and 4 return this record ITSELF for an `about:` destination, and a
       serialization would mint a second one and make the loaded document cross-origin to the document that
       navigated it. */
    orc = JS_ToUint32(ctx, &oid, step_arg(&s->hdr, 2));
    (void)orc;
    DCHECK(orc == 0 && oid != 0, "the document-load job carried no INITIATOR ORIGIN handle — §7.4.2.2 snapshots "
                                 "the source document's origin before anything is fetched, and §7.3.2.1's "
                                 "determine the origin inherits that record for a destination with no response");
    initiator = origin_by_id(oid);
    if (fetches) {
        /* Fetch §2.2.6 "Responses"' RESPONSE URL, WHICH IS THE FIRST THING TAKEN OFF THIS ANSWER BECAUSE EVERY
           ALGORITHM BELOW IS WRITTEN OVER IT. See this function's own header for the redirect that separates
           it from the address requested, and for why reading the request's URL here was a Document created in
           this heap under a principal its response had moved off.
           DCHECKed rather than defaulted: a host that fetched knows the URL of the response it received, and
           a host that could not fetch at all still knows the URL it was asked for — "the request URL is a fact
           even when the reply is not" is the trusted zone's own sentence about the list this field is the last
           item of (extension/lib/safe-fetch.js). So there is no response for which this field is legitimately
           absent, and an `|| addr` here would put the two apart exactly where they differ. */
        urlv = JS_GetPropertyStr(ctx, answer, "url");
        DCHECK(JS_IsString(urlv),
               "§7.4 step 14's response answer carries no `url` field — Fetch §2.2.6 \"Responses\" gives every "
               "response a URL (the last of its URL list) and the trusted zone is the only party that saw the "
               "redirect chain, so a missing one is a producer that stopped writing it. Without it §7.4.5's "
               "determine-the-origin, §7.1.7's determine-navigation-params-policy-container and CSP §2.2.2's "
               "self-origin all run over the address this job REQUESTED, and a response that moved off this "
               "origin becomes a Document created in this heap under a principal it does not have");
        resp_url = JS_ToCString(ctx, urlv);
        CHECK(resp_url != NULL, "navigable: OOM taking §7.4 step 14's response URL");
        DCHECK(*resp_url != '\0',
               "§7.4 step 14's response answer carries an EMPTY response URL — Fetch §2.2.6's URL list is empty "
               "only for a response no request was ever made for, and this job made one; an empty string here "
               "would parse as no URL at all and §7.3.2.1's determine the origin would mint an opaque origin "
               "for a document the server answered normally");
        bodyv = JS_GetPropertyStr(ctx, answer, "body");
        /* THE HEADER LIST, AS THE HTTP FIELD LINES THE RESPONSE DELIVERED — `name: value`, one per line, the
           one form a header list crosses an ABI in (core/fetch/headers.h) and exactly what qjs_init takes for
           the root document's response. DCHECKed rather than defaulted: the trusted zone always states one
           (the empty string IS a response that carried no headers), so a missing field is a producer that
           stopped writing it, and a `|| ""` here would silently give every navigated Document the policy
           container, opener policy and sandboxing flags of a response with no headers at all. */
        headersv = JS_GetPropertyStr(ctx, answer, "headers");
        DCHECK(JS_IsString(headersv),
               "§7.4 step 14's response answer carries no `headers` field — the trusted zone states the "
               "response's HTTP field lines (the empty string is a response that carried none), and §7.5.1 "
               "creates a Document out of them: without the list this navigation gets no policy container, no "
               "opener policy and no CSP-derived sandboxing flags, with nothing to say so");
        field_lines = JS_ToCString(ctx, headersv);
        header_list_parse_field_lines(&response_headers, field_lines);
        /* Fetch §2.2.2's `get`, which JOINS repeated headers with ", " — CSP §2.2's own serialization of a
           policy LIST, which policy_container.c splits apart again. The zone that used to extract this read
           one map entry and could not express two `Content-Security-Policy` headers at all. */
        resp_csp = header_list_get(&response_headers, "content-security-policy");
        /* THE SAME `get`, FOR THE HEADER THAT DECIDES WHAT THIS DOCUMENT MAY USE. It is read HERE, at the child
           navigable's own response, because §9.6 takes navigationParams's RESPONSE and this frame is where a
           child's response is read — the framing document's copy of this question is answered at
           core/frame/navigation_params.c and says nothing about the framed document's own headers. */
        resp_pp = header_list_get(&response_headers, "permissions-policy");
        resp_pp_report_only = header_list_get(&response_headers, "permissions-policy-report-only");
        /* THE RESPONSE'S `Content-Type` IS READ TWICE, BY TWO STANDARDS, AND NEITHER READ IS THIS FRAME'S ANY
           MORE. Fetch §2.2.2 "Headers"' `get` JOINS every value with a comma and a space, which is what HTML
           §13.2.3.2 "Determining the character encoding" runs on; MIME Sniffing §5.1 "Interpreting the
           resource metadata" takes the LAST value UNJOINED, because §5's check-for-apache-bug flag is a
           byte-exact comparison a joined list can never satisfy. Each read now lives inside the component
           whose algorithm defines it — core/loader/document_load_decode.h and
           core/loader/document_load_type.h — and this frame carries the LIST, so no entry can hand one
           algorithm the other one's operand. It used to read the joined value here and pass it down, which is
           the shape that let two of the three entries that build a Document out of a response never ask this
           question at all. */
        /* THE RESPONSE'S BYTES, WHICH IS WHAT A DOCUMENT IS PARSED FROM. This was `JS_ToCStringLen` over a
           field the trusted zone had built with Fetch §5.3 "Body mixin"'s `text()` — a UTF-8 decode run before
           HTML could run its own (§5.2 stood on this sentence and is "BodyInit unions", which EXTRACTS a body
           rather than consuming one, so the number named the opposite direction while reading as checked)
           — so a document served in any other encoding reached lexbor already replaced with U+FFFD
           and the algorithm that decides its encoding had nothing left to decide. §2.2.5's body is a byte
           sequence and the host now hands one over (core/fetch/fetch.h), and HTML §13.2.3.2 "Determining the
           character encoding" runs on it in nav_create_begin — the BOM sniff, then the transport layer's
           charset, then the prescan of the first 1024 bytes — with Encoding §6.1 Legacy hooks for standards'
           `decode` producing the UTF-8 lexbor's tokenizer takes. So the pair that travels from here is the
           BYTES AND THE HEADER VALUE, never a string somebody already decoded: reinstating a decode on this
           side would put the answer back out of §13.2.3.2's reach. */
        if (!JS_IsUndefined(bodyv) && !JS_IsNull(bodyv)) body = fetch_body_bytes(ctx, bodyv, &body_len);
        /* §7.4.5's "Let type be the COMPUTED TYPE of navigationParams's response" — MIME Sniffing §7's MIME
           type sniffing algorithm, which §8.1 "Sniffing in a browsing context" names as the one a navigation
           runs. It is computed HERE, where the response is, and travels to the realm builder as a value: it is
           a fact about this response and about nothing the navigable holds, which is the split CLAUDE.md's
           "an operation that becomes a work item takes its inputs with it" is about.
           ONLY WHEN THERE ARE BYTES. A fetch that did not load has no resource to sniff and gets §7.4's
           initial-about:blank treatment below; that is a different fact from a response that carried no
           `Content-Type`, which HAS bytes and is exactly what §7 exists to answer for. */
        if (body) {
            document_load_computed_type(&computed, &response_headers, body, body_len);
            computed_defined = true;
        }
    }
    /* AND THE URL EVERYTHING BELOW IS RUN OVER, DECIDED ONCE, HERE. §7.4.5's three constructors differ in
       exactly this: create-navigation-params-by-fetching has a response and states its URL, while the srcdoc
       and non-fetch-scheme ones have none and are written over ENTRY'S URL. `fetches` is that same split
       already made — a destination with a response has one, an `about:` destination does not — so the arm is
       the standard's own and not this frame reading past an absent field. */
    dest_url = fetches ? resp_url : addr;
    /* §7.1.7's TWO CANDIDATE CONTAINERS, ASSEMBLED HERE AND CHOSEN BETWEEN BY §7.1.7'S OWN ALGORITHM BELOW.
       This site used to make the choice itself, with the predicate "did the response carry a policy" — which is
       precisely the approximation core/frame/policy_container.h names: it puts a cross-origin child whose
       response sent no CSP under its EMBEDDER's policy, which is the common shape on any CSP-bearing page that
       frames a third party. The standard's predicate is `responseURL is local`, and there is one function that
       applies it; a rule spelled at three entries is three rules.
       THE INHERITED HALF IS ALWAYS PRESENT. §7.1.7's `initiatorPolicyContainer is not null` is a question about
       a CONTAINER, not about policy text, and every operation that enqueues a load has a document to clone
       from — so the job carries one whether or not it holds any policies. */
    DCHECK(JS_IsString(step_arg(&s->hdr, 3)) && JS_IsString(step_arg(&s->hdr, 4)),
           "a document load carried no §7.1.7 POLICY CONTAINER for its initiator — the operation that enqueued "
           "it had a document, every document has a container, and §7.1.7 step 3 asks whether that container "
           "is null rather than whether it holds any policies; a load without one gives an `about:blank` "
           "destination an empty container of its own instead of the clone its creator's CSP travels in");
    /* AND §7.1.4'S ITEM OF THAT SAME CONTAINER, ON THE SAME JOB AND FOR THE SAME SENTENCE. §7.1.7's clone
       moves EVERY item of the initiator's container, so an inherited container whose embedder policy was left
       behind at the enqueue is a clone that silently downgrades a `require-corp` creator's `about:blank` child
       to `unsafe-none` — the item is exactly as much a fact about the OPERATION as the CSP list is, and by the
       time this job runs the only document it could ask is the one being replaced. */
    DCHECK(JS_IsString(step_arg(&s->hdr, 6)) && JS_IsString(step_arg(&s->hdr, 7)) &&
           JS_IsString(step_arg(&s->hdr, 8)) && JS_IsString(step_arg(&s->hdr, 9)),
           "a document load carried no §7.1.4 EMBEDDER POLICY for its initiator's §7.1.7 container — the "
           "container travels whole or it is not a container, and every operation that enqueues a load has a "
           "document whose container holds one (initially a new embedder policy, never absent)");
    inherited_csp  = JS_ToCString(ctx, step_arg(&s->hdr, 3));
    inherited_self = JS_ToCString(ctx, step_arg(&s->hdr, 4));
    inherited_coep = JS_ToCString(ctx, step_arg(&s->hdr, 6));
    inherited_coep_endpoint = JS_ToCString(ctx, step_arg(&s->hdr, 7));
    inherited_coep_report_only = JS_ToCString(ctx, step_arg(&s->hdr, 8));
    inherited_coep_report_only_endpoint = JS_ToCString(ctx, step_arg(&s->hdr, 9));
    {
        EmbedderPolicyValue v = EMBEDDER_POLICY_UNSAFE_NONE, ro = EMBEDDER_POLICY_UNSAFE_NONE;
        /* §7.1.4's THREE STRINGS, READ BACK. A token this job carries was written by embedder_policy_value_token
           at the enqueue, so a token that names no value is not a fail-open of §7.1.4.1's kind (that one is
           about a HEADER a server sent) — it is this engine's own record disagreeing with itself, which is the
           two-readers-of-one-format defect and crashes rather than defaulting.
           THE PARSE IS A STATEMENT AND THE ASSERT ONLY READS IT: a DCHECK condition is compiled out in release,
           so a parse inside one is a parse the shipped build does not perform. */
        bool ok = embedder_policy_value_of_token(inherited_coep, &v);
        bool ok_ro = embedder_policy_value_of_token(inherited_coep_report_only, &ro);

        (void)ok; (void)ok_ro;
        DCHECK(ok && ok_ro,
               "a document load carried an §7.1.4 embedder policy VALUE that names none of the three strings "
               "the section defines — this token was written by this engine at the enqueue, so it is a record "
               "that disagrees with its own writer rather than a header that failed to parse");
        inherited_ep = serialized_embedder_policy(v, inherited_coep_endpoint, ro,
                                                  inherited_coep_report_only_endpoint);
    }
    inherited = serialized_policy_container(inherited_csp, inherited_self, inherited_ep);
    /* §7.4.5's ABOUT BASE URL for THIS navigation — "if url matches about:blank or about:srcdoc, set
       aboutBaseURL to initiatorBaseURL" — which is why the INITIATOR's base URL rides the job and is used only
       when the destination is an `about:` URL. `fetches` is exactly that test already made, from the other
       side: a destination with a response has one, and §2.4.3 gives its Document a null about base URL. */
    if (!fetches) {
        /* ASKED OF THE VALUE AND NOT OF THE STRING IT CONVERTS TO. `JS_ToCString(JS_NULL)` answers "null" —
           a five-character URL that parses — so a job that carried no base would have set one silently. The
           absent case is a JS_NULL slot, which is a question about the slot. */
        DCHECK(JS_IsString(step_arg(&s->hdr, 5)),
               "§7.4.2.2 navigated to an `about:` destination with no INITIATOR base URL on the job — §7.4.5 "
               "takes the about base URL from the initiator, and without it every relative URL in the loaded "
               "Document resolves against `about:blank`, whose opaque path makes the parse FAIL");
        about_base = JS_ToCString(ctx, step_arg(&s->hdr, 5));
    }
    /* WHERE THE NEW DOCUMENT'S ENVIRONMENT SITS, which HTML §8.1.3.1 answers by asking what this navigable IS
       rather than what it is loading. Navigating a TOP-LEVEL TRAVERSABLE moves the top-level environment to
       the address being loaded — it IS the top. Navigating a NESTED navigable does not: its environment's
       top-level creation URL is the one it was created with and belongs to its ancestor chain, which is why
       loading an `https` document into an `http` page's iframe leaves it a non-secure context (Secure Contexts
       §4.2 — otherwise an iframe plus postMessage is a shim around every gated API).
       This is the one thing about the operation the TARGET legitimately decides, exactly like the document's
       identity below: whether a navigable is nested is a fact about the navigable and not about the load.
       AND THE ADDRESS IT MOVES TO IS THE RESPONSE'S, not the one requested: §7.5.1 "Shared document creation
       infrastructure" sets a top-level environment's top-level creation URL to the SAME `creationURL` it gives
       the Document ("let topLevelCreationURL be creationURL"), so the two are one string by the standard's own
       step — and a top-level traversable whose request redirected would otherwise decide Secure Contexts §4.2
       for a document at an address it is not at. */
    tlu = window_proxy_is_top_level(proxy) ? dest_url : window_proxy_top_level_url(proxy);
    DCHECK(tlu != NULL && *tlu,
           "a nested navigable was navigated with no top-level creation URL on its proxy — §7.4 gives every "
           "navigable one when it creates it, so a proxy without one was minted somewhere that did not");
    /* §7.4.5's FINAL SANDBOXING FLAG SET, which is what §7.5.1 hands the new Document as its ACTIVE
       SANDBOXING FLAG SET: "let finalSandboxFlags be the union of targetSnapshotParams's sandboxing flags and
       policyContainer's CSP list's CSP-derived sandboxing flags". The first half is this navigable's creation
       sandboxing flags (§7.4.2.1 snapshots them off the target), the second is the `sandbox` directive of the
       CONTAINER this Document is created with, which is §7.1.7's determine step's answer and not a third
       reading of "did the response carry a policy" — the flags follow the container, so they are read off the
       container rather than off whichever half of it this frame happened to hold. */
    /* §7.1.7's CREATE-A-POLICY-CONTAINER-FROM-A-FETCH-RESPONSE, ASSEMBLED HERE RATHER THAN AT THE HEADER READ
       ABOVE, because its step 4 needs an ENVIRONMENT and the environment's answer is not known until `tlu` is.
       §7.1.4's obtain step 2 returns the default policy for a NON-SECURE CONTEXT whatever the response sent,
       and §8.1.3.5 answers that over the environment's TOP-LEVEL CREATION URL — which for a nested navigable
       is its ancestor's and not this address, the same fact the opener-policy arm below reads the same way.
       Building the container where `resp_csp` was read would have had to answer it from `addr`, and an https
       document in an http page's iframe would then have obtained a `require-corp` a browser refuses it.
       ONLY WHERE THERE IS A RESPONSE. §7.1.7 runs this algorithm on one; a destination with no response gets
       step 5's new container, whose embedder policy item §7.1.4 makes "a new embedder policy" — the else arm.
       IT IS INITIALISED HERE AND NOT AT THE TOP OF THIS FRAME, unlike `response_coop` beside it, and that is a
       fact about the two records rather than a style: §7.1.4's endpoints are STRINGS whose initial value is the
       empty one, so an init ALLOCATES — and this step machine re-enters from the top on every resume, so an
       init above the two yields would allocate a pair per park and free one pair at the end. §7.1.3's endpoint
       is initially NULL, which is why the line above may stand where it does. */
    if (fetches)
        embedder_policy_obtain(&response_ep, &response_headers,
                               secure_context_url_potentially_trustworthy(tlu));
    else
        embedder_policy_init(&response_ep);
    /* §4.7's RECORD FOR THE DESTINATION, PARSED ONCE AND READ BY BOTH OF THE ALGORITHMS BELOW. `dest_url` is an
       ABSOLUTE serialization — the trusted zone's report of Fetch §2.2.6's response URL, or url_serialize's
       output at the enqueue for a destination that made no request — so it parses with no base, and a failure
       here is a serializer and a parser disagreeing about one string rather than anything a page can cause. */
    url_record_init(&dest);
    dest_parsed = url_parse(&dest, dest_url, strlen(dest_url), NULL);
    (void)dest_parsed;   /* the parse is the STATEMENT; a DCHECK condition is compiled out in release */
    DCHECK(dest_parsed,
           "a document load's destination would not re-parse — it is either §4.4's own serializer's output from "
           "the enqueue or the URL the trusted zone reported the response arrived from, so a URL record that "
           "cannot be recovered from it is a parser and a serializer disagreeing about one string, and "
           "§7.3.2.1's determine the origin has no URL to answer over");
    /* CSP §2.2.2's SELF-ORIGIN FOR THIS RESPONSE'S OWN LIST, WHICH IS THE RESPONSE URL'S ORIGIN AND NOT THE
       ORIGIN §7.3.2.1 DETERMINES. §2.2.2 "Parse response's Content Security Policies" states it from outside
       the bytes — "self-origin is response's URL's origin" — and the determined origin can differ from it by
       exactly one header: a `sandbox` directive without `allow-same-origin` mints a fresh OPAQUE origin for the
       Document while `'self'` still resolves against the URL it came from. This frame used to pass the
       determined origin and assert the difference away; the assert is gone because the difference is REAL and
       §2.2.2's own sentence is what stands in its place.
       AND "RESPONSE'S URL" IS NOW THE RESPONSE'S. `dest` is parsed from the reported response URL, so a policy
       delivered by a redirected response resolves `'self'` against the origin that delivered it rather than
       against the address the navigation asked for — which for a cross-origin redirect is a `'self'` naming a
       server that sent no policy at all. */
    response_self = origin_serialize_of_url(&dest);
    CHECK(response_self != NULL, "navigable: OOM stating CSP §2.2.2's self-origin for a navigation response");
    /* §2.2.2's pair for this load's own response: its policies and that self-origin, beside §7.1.4's item
       obtained just above. */
    response = serialized_policy_container(resp_csp, response_self,
                                           serialized_embedder_policy_of(&response_ep));
    /* §7.1.7's DETERMINE NAVIGATION PARAMS POLICY CONTAINER, WHOSE FIRST ARGUMENT THE SECTION NAMES
       `responseURL` — "to determine navigation params policy container given a URL responseURL and four policy
       container-or-nulls". Its two remaining predicates are `responseURL is about:srcdoc` and `responseURL is
       local`, and both change answer under a redirect: a request for a `data:`-or-`about:` local URL cannot
       redirect, but a request for an http URL that redirects TO one would have been judged non-local off the
       address requested and handed the response's container where §7.1.7 clones the INITIATOR's. */
    policy = policy_container_determine_navigation_params(dest_url, response, inherited);
    csp_flags = policy_csp_derived_sandboxing_flags(policy.csp, policy.csp ? strlen(policy.csp) : 0);
    final_flags = window_proxy_creation_sandbox_flags(proxy) | csp_flags;
    /* HTML §7.3.2.1 "Creating browsing contexts"' DETERMINE THE ORIGIN, RUN HERE BECAUSE HERE IS WHERE ITS
       INPUTS EXIST. §7.4.5 "Populating a session history entry", inside create-navigation-params-by-fetching:
       "Set responsePolicyContainer to the result of creating a policy container from a fetch response … Set
       finalSandboxFlags to the union of targetSnapshotParams's sandboxing flags and responsePolicyContainer's
       CSP list's CSP-derived sandboxing flags. Set responseOrigin to the result of determining the origin given
       RESPONSE'S URL, finalSandboxFlags, and entry's document state's initiator origin." All three lines are
       the three above, in that order, and the order is the whole point: the flag set is a fact about the
       RESPONSE, so the origin cannot be known before one exists.
       AND SO IS THE URL. The section names the argument RESPONSE'S URL, and this call ran over the address the
       job REQUESTED until `dest` was parsed from the reported response URL — two strings that a redirect makes
       different and that a same-origin request makes equal, which is why nothing said so. The move of this
       call from the enqueue to the job fixed the TIME the question is asked; this fixes the VALUE, and both
       halves were named by the same sentence of §7.4.5.
       IT USED TO BE ANSWERED AT THE ENQUEUE, and a `Content-Security-Policy: sandbox` was the header that made
       that observable — the Document ran under a principal the response had revoked. That is fixed by position
       rather than by a check, which is why the check that named it is deleted rather than moved.
       THE `about:` ARM IS THE SAME CALL AND NOT AN EXCEPTION: a destination that fetched nothing reaches
       step 4, which returns the INITIATOR's record itself, so the inheritance that used to be performed at the
       enqueue is performed here by the same algorithm with the same source. */
    origin = origin_determine(&dest, (final_flags & SANDBOX_ORIGIN) != 0, initiator);
    /* AND WHICH INSTANCE THE INCOMING DOCUMENT BELONGS TO IS ASKED OF THAT ANSWER — the first point in this
       navigation at which the question has one. An instance is an ORIGIN-KEYED AGENT CLUSTER, so a Document
       whose §7.3.2.1 origin is not this agent's is a PEER's, and this side may not build its realm.
       WHAT IS MISSING IS NOT THE CREATE NOTICE, AND ROUTING THIS THROUGH ONE WOULD BE WRONG. §7.4's create
       provisions a NEW navigable and its peer FETCHES the address for itself; this navigable ALREADY EXISTS in
       this instance, in the same browsing context group, presented by an element of this document, and its
       WindowProxy is an object the page is holding across the navigation (§7.2.3 — that is what a WindowProxy
       is for). Three things have to be built and none of them is a field on an existing record:
         (1) A RECORD THAT NAMES A PEER A DOCUMENT FOR A NAVIGABLE IT DOES NOT OWN — the ADDRESS, this origin,
             §7.1.7's `policy` container the Document is created WITH, §8.1.3.1's `tlu` and top-level origin,
             §7.3.1.3's parent navigable, Permissions Policy §9.5's answer for the container element, and
             `final_flags`. IT DOES NOT CARRY THE BYTES, AND THAT IS SECURITY.md's RULE RATHER THAN A
             SIMPLIFICATION — this line used to say the opposite and it was a spec-wrong instruction to the
             next reader. SECURITY.md's §The principal is MINTED in one place states it in general ("a message
             from an untrusted zone carries MATERIAL … and never identity") and bridge.js's own §7.1.3.2 swap
             arm states the combined form this record would break: an untrusted engine that could supply BYTES
             and an ADDRESS the zone then derives a principal from could name any origin's document into
             existence — attacker bytes at a victim origin, in a heap the zone believes is the victim's. So the
             engine names the address and the TRUSTED ZONE loads it, exactly as it already does for §7.4's
             create notice, and the principal comes from the load that zone performed.
             THE SECOND REQUEST THAT LEAVES IS A REAL RESIDUAL AND IT IS THE ZONE'S TO CLOSE, not this record's:
             §7.4.5 runs the fetch once, so a peer re-fetching is a request the server may answer differently.
             bridge.js already names the closure for the identical residual on the swap arm — the zone remembers
             its OWN reply to the `document.fetch` this navigation already made, keyed by (instance, address) —
             and that memo is what makes a redirected load correct rather than merely cheap, since the reply it
             replays carries the RESPONSE's URL this frame determined the origin over.
         (2) A LOCAL-TO-REMOTE TRANSITION FOR AN EXISTING WindowProxy. window_proxy_navigate takes a REALM, and
             window_proxy_is_remote asserts a remote proxy holds neither realm nor Window — so the transition
             has to RELEASE both, which is §7.4.6.1 "Updating the traversable"'s deactivate-a-document-for-a-
             cross-document-navigation and not a free: a flow parked in the outgoing Document resumes there.
             WHERE THE UNLOAD IS QUEUED IS NO LONGER A SUBPROBLEM OF IT: engine_unload_document takes the
             OUTGOING document alone, and document_lifecycle_unload_replaced derives the navigable from that
             document's own realm, which is §7.5.9 "Unloading documents" step 6's answer ("queue a global task
             … given document's relevant global object") and is local for every navigation. So this transition
             may call it for a navigation whose incoming Document is a peer's, and what is left to build here
             is the transition itself — release the realm and the Window through §7.4.6.1's deactivate rather
             than through a free, since a flow parked in the outgoing Document resumes there.
         (3) A HOST THAT ROUTES IT. Both hosts provision a peer for `navigable.create` and `navigable.swap`
             (wpt_runner.c spawns a child process; bridge.js roots a cluster), and neither can carry this one:
             both of those CREATE a navigable in the peer, and this one must attach a Document to a navigable
             whose identity, parent and container already exist over here.
       THE SAME QUESTION IS ASKED ONE ALGORITHM EARLIER BY §7.4's CREATE, OFF THE REQUEST URL, AND THIS CHECK
       CLOSES EXACTLY THE HALF OF IT THAT REACHES A FETCH THIS INSTANCE PERFORMS. §7.4 creates the navigable
       with the initial about:blank and NAVIGATES it at step 14, so a create whose address is same-origin takes
       the local arm, enqueues that navigation, and its response arrives HERE — a redirect off-origin then
       fires this line rather than silently loading a peer's document into this heap.
       THE OTHER ARM IS THE HOST'S AND IS ANSWERED WHERE THE HOST STATES THE PRINCIPAL. A create whose REQUEST
       url is already cross-origin emits the notice, and the peer's root Document is handed to it as BYTES the
       host fetched rather than through this job — so the address that instance is rooted at, and therefore the
       principal `origin_agent_adopt` takes, is the host's to state, and it must be the URL the response came
       from. That is a claim about other files and it is where the answer lives: read them rather than build
       from this line (each host's provisioning of a peer, and core/platform.c's adopt). */
    DCHECK(child_in_this_agent(origin),
           "§7.4.5 determined this navigation's RESPONSE ORIGIN and it is not this agent's, so the incoming "
           "Document belongs to a PEER instance — an instance is an origin-keyed agent cluster and the "
           "navigable stays here while its ACTIVE DOCUMENT moves. This is not §7.4's create notice: that one "
           "provisions a NEW navigable whose peer fetches for itself, and this navigable already exists in "
           "this group, presented by an element of this document, behind a WindowProxy the page holds across "
           "the navigation. Build the three named above this line — a record that names a peer this ADDRESS "
           "together with §7.1.7's container, §8.1.3.1's pair, §7.3.1.3's parent and §9.5's container answer, "
           "and NOT the bytes (SECURITY.md: an untrusted engine that supplies bytes for an address the zone "
           "derives a principal from can name any origin's document into existence); a local-to-remote "
           "transition for this proxy that runs §7.4.6.1's deactivate-a-document-for-a-cross-document-"
           "navigation rather than freeing a realm a parked flow resumes into; and the host route for it");
    /* AND §8.1.3.1's TOP-LEVEL ORIGIN, asked of the same fact about the navigable that `tlu` is: §7.11 gives a
       TOP-LEVEL traversable's new environment the origin of the document it is loading — this document IS the
       top-level environment, and §7.3.2.1's answer for the response is the one just computed — while a NESTED
       navigable keeps the pair its creation gave it, which is what makes a cross-origin frame's permission key
       its EMBEDDER's (Permissions §5.1: "Most powerful features grant permission to the top-level origin and
       delegate access to the requesting document via Permissions Policy").
       IT IS READ HERE AND NOT BESIDE `tlu` BECAUSE ITS INPUT IS NOW THE RESPONSE'S ORIGIN, which does not
       exist until the line above; the pair is still one decision and the two halves still answer the same
       question about the navigable. */
    tlo = window_proxy_is_top_level(proxy) ? origin : window_proxy_top_level_origin(proxy);
    /* HTML §7.4.5's OPENER-POLICY ARM, and its CONDITION is the whole shape of it: "If navigable is a TOP-LEVEL
       TRAVERSABLE: set responseCOOP to the result of obtaining an opener policy given response and request's
       reserved client. Set coopEnforcementResult to the result of ENFORCING the response's opener policy …".
       A child navigable never obtains one, which is why an `<iframe src>` that happens to be served a COOP
       header keeps §7.4.5's "new opener policy" — and it is the same fact §7.1.3.2's obtain-a-browsing-context-
       to-use-for-a-navigation-response states from the other end in its step 2 ("if browsingContext is not a
       top-level browsing context, then return browsingContext"). The `about:` arms of §7.4.5 keep it too, each
       under its own "let coop be a NEW opener policy".
       §8.1.3.5's SECURE-CONTEXT ANSWER IS OVER THE TOP-LEVEL CREATION URL, which for a top-level traversable
       is the address being loaded — §7.1.3 step 2 returns the default policy for a non-secure context, so a
       COOP header served over plain http decides nothing. */
    if (fetches && window_proxy_is_top_level(proxy)) {
        opener_policy_obtain(&response_coop, &response_headers,
                             secure_context_url_potentially_trustworthy(tlu));
        /* §7.4.5, in the same arm and immediately after the enforcement: "If finalSandboxFlags is not empty and
           responseCOOP's value is not `unsafe-none`, then set response to an APPROPRIATE NETWORK ERROR and
           break" — the standard's own note: "one cannot simultaneously provide a clean slate to a response
           using opener policy and sandbox the result of navigating to that response". This engine has no
           network-error response to substitute: a failed load reaches §7.5.1 as absent bytes, which is a
           DIFFERENT thing (a navigable showing an error page whose policy came from somewhere). */
        DCHECK(final_flags == 0 || response_coop.value == OPENER_POLICY_UNSAFE_NONE,
               "§7.4.5 turns this response into a NETWORK ERROR — its opener policy is not `unsafe-none` and "
               "the final sandboxing flag set is not empty, and the standard refuses to give a clean slate to "
               "a response it is also sandboxing. Build §7.4.5's network-error response as a value the load "
               "carries (it is not the same as a fetch that failed, which is a real response this engine "
               "already models as absent bytes)");
        /* §7.1.3.2's ENFORCE A RESPONSE'S OPENER POLICY, reduced to the one item its consumer reads — "needs a
           browsing context group switch" — over the enforcement result §7.4.5 starts this navigation with:
           "url … navigable's active document's URL; origin … navigable's ACTIVE DOCUMENT's origin; opener
           policy … navigable's ACTIVE DOCUMENT's opener policy". Both of those come off the navigable, which
           is what window_proxy.h's row is for and why a lazily-materialized initial about:blank can still
           answer. `isInitialAboutBlank` is §7.4.4 step 4's, read from the navigable's own side. */
        if (opener_policy_switch_required(!window_proxy_ever_navigated(proxy), origin,
                                          window_proxy_origin(proxy), response_coop.value,
                                          window_proxy_opener_policy(proxy))) {
            /* §7.1.3.2's SWAP ARM, performed by core/frame/browsing_context_group.c — a new top-level browsing
               context in a NEW group, announced to the host as a SECOND INSTANCE, and this navigable's browsing
               context discarded. THIS NAVIGATION THEN CREATES NO DOCUMENT HERE, which is what `swapped` gates
               below: nav_create_begin/_finish would build the swapped Document's realm in this heap, which is the
               second agent cluster in one JSRuntime SECURITY.md's key forbids, and `window_proxy_navigate`
               would move this navigable's binding onto it — precisely the update §7.1.3.2 withholds, and the
               update whose ABSENCE is what makes the opener's handle answer `closed`. The document NAME is not
               minted either: §7.5.1's own note says the Window, Document and agent on the swapped-past side
               "will not end up being used". */
            swapped = true;
            /* AND IT CARRIES THIS NAVIGATION'S OWN PROVENANCE, because §7.1.3.2's swap is not a second act:
               it is THIS load, arriving at a host that must build its Document in another instance. The host
               re-fetches the address to do that (it may not take bytes from an untrusted engine), so it makes
               the identical firing decision the create arm makes and needs the identical field. Reading it off
               the job rather than off the flow running now is §scheduler's rule again — by here the response
               has come back, and the flow standing at this line need not be the one that navigated. */
            {
                const char *swap_prov = JS_ToCString(ctx, step_arg(&s->hdr, 10));

                CHECK(swap_prov != NULL, "navigable: OOM taking §7.1.3.2's provenance off the load job");
                browsing_context_group_swap(ctx, proxy, dest_url, origin, final_flags, swap_prov);
                JS_FreeCString(ctx, swap_prov);
            }
        }
    }
    /* THE RESPONSE'S HEADER LIST OUTLIVES THIS POINT NOW, and it has to: HTML §13.2.3.2 "Determining the
       character encoding" reads the `Content-Type` header in a form of its own (Fetch §2.2.2 "Headers"' get,
       joined) and that read belongs INSIDE the component that runs the algorithm, not to this frame. The list
       is therefore released beside the other bytes this frame owns, after the creation below has taken what
       it needs — see the free at the end of this function. */
    if (!swapped) {
        /* HTML §7.4.5 "Populating a session history entry", from the list of conditions that block a navigation
           before its Document is created: "navigationParams's RESERVED ENVIRONMENT is non-null and the result
           of checking a navigation response's adherence to its embedder policy given navigationParams's
           response, navigable, and navigationParams's POLICY CONTAINER's embedder policy is false".
           `fetches` IS THE RESERVED-ENVIRONMENT TEST. §7.4.5 builds navigation params three ways and only
           create-navigation-params-BY-FETCHING states one ("reserved environment … request's reserved client");
           its srcdoc and non-fetch-scheme constructors both state null. An `about:` destination here made no
           request and has no response, which is the same fact from the other side — and also why there would be
           nothing for the violation this check reports to name.
           THE POLICY IS THE CONTAINER'S, NEVER `response_ep`. §7.1.7's determine step above may have chosen the
           INITIATOR's container for a local URL, and §7.4.5 names the container the Document is created WITH;
           judging a Document against a policy it is not created under is the same substitution one item along
           that the self-origin travelling beside the CSP text exists to prevent.
           IT RUNS AFTER §7.1.3.2's SWAP, in the standard's order: the swap happens while navigationParams is
           being built and this list is asked of the finished params — and a swapped navigation creates no
           Document in this heap for the check to be about. */
        if (fetches) nav_check_embedder_policy_adherence(ctx, proxy, policy.embedder);
        if (window_proxy_materialized(proxy)) {
            doc = world_mint_doc(window_proxy_doc(proxy));
            world_doc_adopt(doc);
        } else {
            doc = window_proxy_doc(proxy);   /* §7.4 minted and adopted it when it created the navigable */
        }
        /* AND THE DOCUMENT'S ADDRESS IS §7.5.1 "Shared document creation infrastructure"'s `creationURL`, which
           the section builds out of exactly the two URLs a redirect separates: "let creationURL be
           navigationParams's response's URL. If navigationParams's request is non-null, then set creationURL to
           navigationParams's request's CURRENT URL" — and Fetch §2.2.5 "Requests" makes a request's current URL
           "a pointer to the last URL in request's URL list", which the redirect loop appends the new address
           to. So both arms name the address the response came from, and the address the navigation ASKED for is
           the FIRST item of that list. It is what `document.URL`, `location.href` and every relative URL this
           Document resolves are read off. */
        /* §7.5.1's CREATION IS OPENED HERE AND FINISHED IN THE NEXT STAGE. Everything §7.4.5 determined is
           handed over in this one call — including §7.5.1's opener policy row and the top-level origin, which
           §7.4.6.2's update the document needs on the far side of however many suspensions the parse takes —
           because the record COPIES what a park does not carry, and this frame's locals are all freed below.
           `navigates` is TRUE: this is a navigation, so the navigable takes the Document as its active one. */
        s->create = nav_create_begin(ctx, doc, dest_url, tlu, origin, tlo, response_coop.value,
                                     /*navigates*/true, (const char *)body, body_len, &response_headers,
                                     computed_defined ? &computed : NULL,
                                     policy,
                                     serialized_response_permissions_policy(resp_pp, resp_pp_report_only),
                                     about_base, final_flags);
    }
    opener_policy_free(&response_coop);
    embedder_policy_free(&response_ep);
    /* THE INHERITED CONTAINER'S BYTES, WHICH THIS FRAME OWNS AND THE REALM HAS ALREADY COPIED. They are freed
       unconditionally because the job always carries a container: the conversions above are the only
       producers of these pointers and none of them is conditional. */
    JS_FreeCString(ctx, inherited_csp);
    JS_FreeCString(ctx, inherited_self);
    JS_FreeCString(ctx, inherited_coep);
    JS_FreeCString(ctx, inherited_coep_endpoint);
    JS_FreeCString(ctx, inherited_coep_report_only);
    JS_FreeCString(ctx, inherited_coep_report_only_endpoint);
    JS_FreeCString(ctx, about_base);
    free(resp_csp);
    free(resp_pp);
    free(resp_pp_report_only);
    /* AND THE LIST ITSELF, HERE RATHER THAN BEFORE THE CREATION ABOVE — see the note at the site it moved
       from. The creation reads it through core/loader/document_load_decode.h at its OPEN and never after, so
       the list owes nothing to the far side of a suspension. */
    header_list_free(&response_headers);
    /* CSP §2.2.2's SELF-ORIGIN BYTES, WHICH THIS FRAME OWNS AND THE CONTAINER HAS COPIED — the same ownership
       as `resp_csp` beside it, and freed after nav_create_begin for the same reason: the container the
       creation record was opened with names these bytes until that record has taken its own copy — which it
       does at the OPEN and not at the finish, because the finish is on the far side of a suspension and
       policy_container.h's serialization is borrowed text that does not cross one. */
    free(response_self);
    /* Initialised at the top of this frame and freed unconditionally — §4.4 leaves an empty record behind on
       failure, so a `free` of one that was never filled is the same operation as a free of one that was, and
       there is no path out of here that can skip it. The destination's record is the same statement: `dest` is
       url_record_init'd before the parse, so it is freeable whether or not the parse filled it. */
    url_record_free(&dest);
    mime_type_free(&computed);
    JS_FreeCString(ctx, field_lines);
    /* NO `JS_FreeCString(ctx, body)`: `body` points INTO `bodyv`'s own buffer and owns nothing, so its whole
       lifetime is that value's — which is why the release below must stay AFTER nav_create_begin, which is
       where §13.2.3.2's decode copies the bytes into a buffer the creation record owns. */
    JS_FreeValue(ctx, headersv);
    JS_FreeValue(ctx, bodyv);
    JS_FreeValue(ctx, urlv);
    if (s->req) {
        /* §7.4 step 14's response is the HOST'S OWN — it fetched the bytes; it did not run a peer's program —
           so a throw completion here is a host that answered a network request with something other than the
           network's answer, not a document that threw. */
        int completion = ENGINE_COMPLETION_NORMAL;
        JSValue taken = engine_host_take(ctx, s->req, &completion);
        DCHECK(completion == ENGINE_COMPLETION_NORMAL,
               "a document load's fetch was answered with a THROW completion — this request is answered by the "
               "trusted zone out of the network, and there is no program of a peer's for it to have thrown in");
        JS_FreeValue(ctx, taken);
        s->req = 0;
    }
    /* THE TWO ADDRESSES THIS FRAME OWNS — the one the navigation asked for and the one the response came from.
       `resp_url` is NULL for a destination that made no request, which is the same fact `fetches` states and
       the reason `dest_url` borrows rather than owns: one of these two is what it points at, and a free of the
       borrow would be a double free of whichever it was. */
    JS_FreeCString(ctx, resp_url);
    JS_FreeCString(ctx, addr);
    /* AND THE STAGE ENDS WITH EVERYTHING THIS FRAME OWNED ALREADY RELEASED, which is what makes the rest point
       below a rest point at all: the creation record is the only thing left alive, the host register has been
       taken and cleared, and a flow parked here holds no borrowed string and no unanswered request. A load
       that SWAPPED browsing context groups creates no Document in this heap and has no creation to finish. */
    if (s->create == NULL)
        return JS_STEP_DONE;
    /* THE TRANSFER IS MADE IN THIS SAME ENTRY AND IS NOT ITSELF A REST POINT — what it crosses is the arm
       boundary and nothing else, which is the one thing STEP_JUMP is for. The rest point is INSIDE the arm,
       at each item, so a destination with no response (§7.4's `about:blank`, whose markup is a constant of
       this build) finishes in this entry exactly as it did before there were two stages, and a response
       yields after its first item. */
    STEP_GOTO(s->hdr.stage, NAV_LOAD_CREATE, NULL);
    STEP_JUMP(NAV_LOAD_CREATE);

    /* §7.5.2 / §7.5.3 / §7.5.4, ONE REST UNIT PER ENTRY. The scheduler is asked between every one of them, so
       a higher-value sibling preempts a page-sized parse, the cooperative quantum can take the thread back
       inside it, and the flow that is doing it is an ordinary member of the ONE frontier rather than a C loop
       nothing can interrupt. There is no second driver here and no loop: this arm performs ONE unit and
       returns, and what decides whether it runs again is the frontier.
       WHAT A UNIT IS, IS NOT ASKED HERE AND MUST NOT BE. solver/rest_unit.h owns the count — a networking
       task's worth of bytes for the two HTML-parser arms, per §7.5.2's and §7.5.4's own sentence — because it
       trades the cost of this round trip against the delay before a due suspension is taken, and both of those
       are the scheduler's quantities rather than a loader's or a driver's. */
    STEP_ARM(NAV_LOAD_CREATE);
    DCHECK(s->create != NULL,
           "§7.4 step 14's load resumed at its creation stage with no creation — the fetch stage transfers "
           "here only after nav_create_begin has produced one, so a null here is a stage reached by something "
           "that is not this machine's own transfer");
    if (!nav_create_ended(s->create)) {
        nav_create_step(s->create);
        return JS_STEP_YIELD;
    }
    /* §13.2.7 "The end", the realm the host builds around the finished tree, this document's own scripts, and
       §7.4.6.2's update the document — all of it inside nav_create_finish, which is where the five facts this
       frame determined before the suspension come back out of the record. */
    nav_create_finish(ctx, s->create, proxy);
    s->create = NULL;
    return JS_STEP_DONE;
}

/* WHAT THIS MACHINE OWNS: nothing that is a JSValue. Its five arguments are the header's and the flow owns
   those, the response is the host register's until it is taken, and the load leaves its results on the
   navigable rather than in this state. The declaration is still REQUIRED and is not a formality — a machine
   with no visit cannot be FORKED, so a concolic branch reached from inside the load would abort the fork
   instead of exploring both arms, and check_step_visits is what says so before anything is compiled.
   THE CREATION RECORD IS NOT NAMED HERE AND CANNOT BE. JSStepVisit has no operation for a half-built Document
   — the same hole core/dom/element.c's fragment parse names — so what answers the fork is `unforkable` below
   and what answers the TEARDOWN is `fini`, which are different consumers and different questions. */
static void js_nav_load_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    (void)ctx; (void)st; (void)v;
}

/* THE FLOW DRIVING THIS LOAD IS GONE — abandoned mid-parse, or unwound by a throw. The state holds no JSValue,
   so this exists for exactly one thing: the creation record, which owns the decoded entity, an open §7.5 load
   with a lexbor parser inside it, and the half-built Document itself. NOTHING ELSE NAMES ANY OF THEM — the
   realm has not been built, so no navigable and no world row points at that Document — and a step state is
   plain memory no gc walk and no refcount report can reach, which is exactly the leak class §C-stack's
   ownership rule is about. There is no completion to state: this machine's result is a navigable holding a
   document, never a value. */
static JSValue js_nav_load_fini(JSContext *ctx, void *st, bool take_result)
{
    NavLoadState *s = st;
    (void)ctx; (void)take_result;
    if (s->create) {
        nav_create_abandon(s->create);
        s->create = NULL;
    }
    return JS_UNDEFINED;
}

/* WHY THIS MACHINE MUST NOT BE FORKED WHILE IT IS CREATING A DOCUMENT — core/dom/element.c's fragment parse
   states the whole argument at fragment_parse_unforkable and this is the same object: between two items of §7.5.2's
   fill, the machine holds an lxb_html_parser_t standing at a position with an open-element stack and an
   insertion mode behind it, and lexbor exposes no copy of one; it also holds the half-built Document itself,
   which JSStepVisit has no operation for, so a sibling arm would share ONE tree with the original and both
   teardowns would destroy it. IT IS A PROPERTY OF THE ENGINE AND NOT OF THE PAGE: no page code runs across a
   fill, but RAM pressure, a cold-tier eviction and a cross-session resume never ask what the page is doing,
   and a fork can arrive from anywhere the scheduler puts one. */
static const char *js_nav_load_unforkable(const void *st)
{
    const NavLoadState *s = st;
    return s->create
         ? "§7.4 step 14's document load cannot be forked while it is creating its Document — between two "
           "items of §7.5.2/§7.5.3/§7.5.4's fill this machine owns the decoded entity, an open §7.5 load "
           "holding a parser standing at a position, and the half-built Document those bytes are going into. "
           "js_nav_load_visit declares none of them, because JSStepVisit has no operation for a PRIVATE DOM "
           "TREE and none for a lexbor parser, so the sibling arm would share one Document with the original: "
           "two arms filling one tree and two teardowns destroying it. WHAT TO BUILD IS core/dom/element.c's "
           "fragment_parse_unforkable list, unchanged and already ordered — the `v->tree` operation whose clone deep-"
           "copies a subtree through a node->node map (core/html/tree_construction.c's copy_subtree), and "
           "then HTML §13.2.5's tokenizer as an engine component whose state is a spec-named enum rather than "
           "a raw code pointer into lexbor's 182 static state functions. Both halves serve that machine and "
           "this one, which is why neither is named twice"
         : NULL;
}

static const JSTrampStepDef js_nav_load_def = { sizeof(NavLoadState), js_nav_load_step, js_nav_load_fini, 0,
                                                .visit = js_nav_load_visit,
                                                .algorithm = NAV_LOAD_ALGORITHM,
                                                .steps = NAV_LOAD_STEPS,
                                                .unforkable = js_nav_load_unforkable };
static int g_nav_load_stepid = -1;

/* `inherit_policy` is HTML §7.1.7's CLONE OF THE INITIATING DOCUMENT'S POLICY CONTAINER, and WHOSE it is
   depends on the OPERATION rather than on the navigable's state — the CREATOR's when §7.4 creates a navigable,
   the INITIATOR's when §7.4.2.2 navigates one. So the caller states it and it travels with the job, which is
   the same sentence as the address travelling: everything about where this load is going belongs to the
   operation that started it, and nothing about it can be read back off the navigable being loaded.
   IT IS ALWAYS STATED, EVEN WHEN THE CONTAINER HOLDS NO POLICIES. §7.1.7 step 3 asks whether the initiator's
   container IS NULL, not whether it holds anything, and this seam used to answer that question from the POLICY
   TEXT — so a creator with an empty CSP list crossed as "there is no container", and its `about:blank` child
   got a new container of its own instead of the clone. Nothing observable turned on it while the CSP list was
   the container's only item, which is exactly why it had to be fixed before there was a second one.
 *
 * IT IS A TASK ON THE NAVIGATION AND TRAVERSAL TASK SOURCE, WHICH IS WHAT §7.4.2.2 SAYS — "queue a global task
 * on the navigation and traversal task source" — and it was a MICROTASK, which is a different position in the
 * event loop and not a smaller one. A microtask runs inside the enqueuing flow's own checkpoint, so a load
 * jumped ahead of every task already queued (a timer that had expired, a delivered message, an event fire) and
 * ran before the script that called `open()` had reached its next task; §8.1.7's two queues exist so that
 * cannot happen, and quickjs.c states the same rule at the queues themselves.
 * IT IS ALSO WHAT MAKES A DOCUMENT'S OWN MARKUP WORK. §4.8.5's insertion steps run for every `<iframe>` in the
 * INITIAL MARKUP — at document install, before this agent's frontier is seeded — so this call happens with no
 * flow to own the callback. A TASK queued with no owner is BASELINE work that the first flow adopts
 * (quickjs.c's baseline_call_list); a MICROTASK queued with no owner is a should-never-happen, because a
 * microtask is queued by running script. Queueing this one as a microtask therefore did not merely mis-order
 * it: it put §7.4 step 14's load on quickjs's global list, which this engine never drains, so every page whose
 * own markup carried an `<iframe src>` aborted at the first line of the scheduler's session — which asserts
 * exactly that a job may not already be queued when a session begins. */
/* `about_base` is §7.4.5's INITIATOR BASE URL, carried for the same reason the inherited policy beside it is:
   a destination that matches `about:blank` has no response to take a base from, and by the time the JOB runs
   the only document it could ask is the one being replaced. §CLAUDE.md's "an operation that becomes a work
   item takes its inputs with it" is exactly this field. NULL for a destination that fetches. */
/* AND THE PROVENANCE RIDES FOR THE SAME SENTENCE, WHICH IS THE WHOLE REASON IT IS TAKEN HERE AND NOT AT THE
   FETCH. CLAUDE.md §A-REQUEST-CARRIES-THE-PROVENANCE makes an outbound request's provenance one of the inputs
   the OPERATION has, and solver/flow.h's `path_forced` is monotone — so a flow that enqueues a load and takes
   a contradicted arm afterwards built that navigation on the path it had THEN, while a job that asked at its
   own fetch would file it under a path reached later, in a flow that need not even be the one that enqueued
   it (this is a TASK, and a task queued with no owner is adopted by whichever flow runs first).
   IT IS STATED BY THE CALLER rather than composed here, because the three callers are three OPERATIONS: a
   §7.4.2.2 navigate, a §7.4.3 reload and §7.3.1.3 "Child navigables"' create. They all reach the same
   composition today (solver/engine.h's engine_provenance_of_running_path), and taking it as a parameter is
   what keeps that from being an assumption this function makes on their behalf the day one of them differs —
   a document-install create, whose navigation no flow produced, is already the one that differs in kind. */
static void navigable_load_enqueue(JSContext *ctx, JSValueConst proxy, const char *addr, const Origin *origin,
                                   SerializedPolicyContainer inherit_policy, const char *about_base,
                                   const char *provenance)
{
    JSValueConst argv[11];
    JSValue fn, url, org, csp, self, about, prov;
    JSValue coep, coep_endpoint, coep_ro, coep_ro_endpoint;

    if (g_nav_load_stepid < 0)
        g_nav_load_stepid = JS_RegisterStepDef(JS_GetRuntime(ctx), &js_nav_load_def);
    fn = JS_NewCFunction2(ctx, NULL, "load", 11, JS_CFUNC_step, g_nav_load_stepid);
    CHECK(!JS_IsException(fn), "the document-load job's callee could not be allocated");
    url = JS_NewString(ctx, addr);
    CHECK(!JS_IsException(url), "the document-load job's address could not be allocated");
    /* §7.4.2.2's initiatorOriginSnapshot — "set initiatorOriginSnapshot to sourceDocument's origin" — WHICH IS
       NOT THE DESTINATION'S. The job determines the destination's with §7.3.2.1 when the response arrives, out
       of this value and the response's own final sandboxing flag set (§7.4.5); what rides here is the INPUT
       that determination takes, and it is an input of the OPERATION in exactly §scheduler's sense — the
       initiator is the document whose script ran, and by the time the job runs the only document it could ask
       is the one being replaced.
       IT TRAVELS AS A HANDLE, NOT AS ITS SERIALIZATION, because §7.3.2.1's steps 3 and 4 return sourceOrigin
       ITSELF: a serialization would mint a second record for an inherited origin, and an `about:blank` child
       would come back cross-origin to the document that navigated it. */
    DCHECK(origin != NULL, "a document load was enqueued with no §7.4.2.2 INITIATOR ORIGIN — every operation "
                           "that enqueues a load has a source document, and §7.3.2.1 steps 3 and 4 return that "
                           "document's origin record for an `about:` destination");
    /* AND IT IS THIS AGENT'S, WHICH IS §7.4.2.2's OWN AGENT STEP READ FROM THE OTHER SIDE: the algorithm
       continues in the agent of the navigable's active document, and an instance is an ORIGIN-KEYED agent
       cluster (SECURITY.md), so every document that can reach this call has this agent's principal. Asserted
       rather than assumed because the value now DECIDES something — it is what §7.3.2.1 inherits for an `about:`
       destination — where before it was merely carried. */
    DCHECK(origin_same(origin, origin_agent()),
           "a document load was enqueued with an INITIATOR ORIGIN that is not this agent's — an instance is an "
           "origin-keyed agent cluster, so a document at another principal is another INSTANCE and its "
           "navigations are that instance's to begin (§7.4.2.2 queues them onto its event loop, not ours)");
    org = JS_NewUint32(ctx, origin_id(origin));
    CHECK(!JS_IsException(org), "the document-load job's origin could not be allocated");
    /* §7.1.7'S CONTAINER, ITEM BY ITEM, ONTO THE JOB. A container with no policies crosses as the EMPTY STRING
       and never as null: the reader reconstitutes a container from these slots, and a null slot would say
       "there was no container" — §7.1.7 step 3's question, answered from the wrong field.
       CSP §2.2's SELF-ORIGIN travels as its SERIALIZATION and not as a handle, unlike the destination's origin
       above. That origin travels as a handle because §7.3.2.1's answer for the destination is an IDENTITY a
       re-derivation would lose; this one is measured only against a URL's origin (§6.7.2.8's `'self'` arm is
       its sole reader), and both bullets of that arm read components an OPAQUE origin has none of — so the
       identity a serialization drops decides nothing here, and the bytes are what the realm boundary below
       takes anyway. */
    DCHECK(serialized_policy_container_exists(inherit_policy),
           "a document load was enqueued with NO §7.1.7 policy container for its initiator — the operation that "
           "enqueued it has a document and every document has a container, so an absence here is an operation "
           "that read one off the navigable being loaded instead of carrying its own");
    csp = JS_NewString(ctx, inherit_policy.csp ? inherit_policy.csp : "");
    CHECK(!JS_IsException(csp), "the document-load job's inherited policy could not be allocated");
    self = JS_NewString(ctx, inherit_policy.self_origin);
    CHECK(!JS_IsException(self), "the document-load job's inherited self-origin could not be allocated");
    /* §7.1.4'S ITEM, ALL FOUR OF IT, BECAUSE §7.1.7'S CLONE MOVES A CONTAINER WHOLE. The two VALUES ride as the
       section's own TOKENS rather than as the enum's integers: a job is a record like the `navigable.create`
       notice is, and a record carrying `1` for `require-corp` is a contract whose two ends agree only by
       accident of declaration order (embedder_policy.h states the rule for both). */
    coep = JS_NewString(ctx, embedder_policy_value_token(inherit_policy.embedder.value));
    CHECK(!JS_IsException(coep), "the document-load job's inherited embedder policy could not be allocated");
    coep_endpoint = JS_NewString(ctx, inherit_policy.embedder.endpoint);
    CHECK(!JS_IsException(coep_endpoint),
          "the document-load job's inherited embedder reporting endpoint could not be allocated");
    coep_ro = JS_NewString(ctx, embedder_policy_value_token(inherit_policy.embedder.report_only_value));
    CHECK(!JS_IsException(coep_ro),
          "the document-load job's inherited report-only embedder policy could not be allocated");
    coep_ro_endpoint = JS_NewString(ctx, inherit_policy.embedder.report_only_endpoint);
    CHECK(!JS_IsException(coep_ro_endpoint),
          "the document-load job's inherited report-only embedder endpoint could not be allocated");
    /* THE INITIATOR'S BASE URL, RESOLVED NOW AND NOT IN THE JOB — the same sentence the address above is
       resolved by: by the time the job runs, the document it would ask is the one being replaced. */
    DCHECK(about_base == NULL || *about_base,
           "a document load was enqueued with an EMPTY about base URL — §7.4.5's null is the absence of the "
           "value, which is what a destination that fetches has, and an empty string is neither");
    about = about_base ? JS_NewString(ctx, about_base) : JS_NULL;
    CHECK(!JS_IsException(about), "the document-load job's about base URL could not be allocated");
    /* CLAUDE.md §A-REQUEST-CARRIES-THE-PROVENANCE's word for this navigation, asserted rather than defaulted:
       the trusted zone's whole decision about whether to spend the network — and whose session — on this
       address is read off this one field, so a job that carried none would reach a host that has nothing to
       decide from and no way to say that is what happened. */
    DCHECK(provenance != NULL && *provenance,
           "a document load was enqueued with NO PROVENANCE — every operation that enqueues one either has a "
           "running flow whose path says whether a gate was forced, or has none and is therefore an act no "
           "code produced, and solver/engine.h's engine_provenance_of_running_path answers both; an absent "
           "field is a caller that stopped stating it, and the trusted zone decides whether to LOAD this "
           "address and whether to send the person's cookies with it on exactly this word");
    prov = JS_NewString(ctx, provenance);
    CHECK(!JS_IsException(prov), "the document-load job's provenance could not be allocated");
    argv[0] = proxy;
    argv[1] = url;
    argv[2] = org;
    argv[3] = csp;
    argv[4] = self;
    argv[5] = about;
    argv[6] = coep;
    argv[7] = coep_endpoint;
    argv[8] = coep_ro;
    argv[9] = coep_ro_endpoint;
    argv[10] = prov;
    JS_EnqueueCallTask(ctx, fn, 11, argv);   /* §7.4.2.2: the navigation and traversal task source */
    JS_FreeValue(ctx, prov);
    JS_FreeValue(ctx, coep_ro_endpoint);
    JS_FreeValue(ctx, coep_ro);
    JS_FreeValue(ctx, coep_endpoint);
    JS_FreeValue(ctx, coep);
    JS_FreeValue(ctx, about);
    JS_FreeValue(ctx, self);
    JS_FreeValue(ctx, csp);
    JS_FreeValue(ctx, org);
    JS_FreeValue(ctx, url);
    JS_FreeValue(ctx, fn);
}

/* §7.4.2.2's NAVIGATE, for a navigable this agent already holds. It RESOLVES the destination and ENQUEUES the
 * load; the document arrives later, which is what the spec says from both ends — `open()` hands back a
 * WindowProxy at its own call site, and the navigable it names is still showing what it was showing.
 *
 * THE ADDRESS IS RESOLVED AGAINST THE NAVIGATING DOCUMENT, not against the target's — §8.1.3.2 "Environment
 * settings objects"' API base URL (§4.4 stood here and is "Grouping content")
 * belongs to the document whose script ran, which for `open("/x", "_self")` happens to be the same document
 * and for `open("/x", "someFrame")` is not. Resolving it HERE and not in the job is that sentence: by the time
 * the job runs, the only document it could resolve against is the one being replaced. */
JSValue navigable_navigate(JSContext *ctx, JSValueConst proxy, const char *url)
{
    char *addr = NULL;
    const Origin *origin = NULL;
    /* §7.4.2.2 step 21's scheme test, answered by the one component that parses this destination — see
       child_address, and the branch below for why it is served HERE and asserted unreachable at the create. */
    bool is_javascript = false;

    DCHECK(window_proxy_is(proxy), "something that is not a WindowProxy was navigated");
    /* AND IT IS ONE OF THIS AGENT'S, asked BEFORE anything is read off it. §7.1's rules for choosing a
       navigable can hand this a navigable whose active document a PEER instance holds, and every fact the
       navigation needs — its target snapshot params, its policy container, the Document it creates — belongs
       to that instance. The check that used to catch this stood after the destination was resolved and spoke
       about the DESTINATION's origin, which is a different question with a different answer. */
    DCHECK(!window_proxy_is_remote(proxy),
           "§7.4.2.2's navigate was asked to navigate a navigable whose ACTIVE DOCUMENT a PEER instance holds "
           "— the navigation's target snapshot params, its policy container and the Document it creates are "
           "all that instance's, so this is a host route like §7.4's create notice and not a load this agent "
           "can perform");
    /* §7.4.2.1's SNAPSHOT TARGET SNAPSHOT PARAMS: "sandboxing flags — the result of determining the creation
       sandboxing flags given targetNavigable's active browsing context and targetNavigable's CONTAINER". The
       container is the `<iframe>` element, and this engine's WindowProxy does not carry one: what it holds is
       the set computed when the navigable was CREATED. Those agree for every navigable created and navigated
       in one operation, and §4.8.5 says they can disagree afterwards — "When an iframe element's sandbox
       attribute is set or changed while it has a non-null content navigable, the user agent must parse the
       sandboxing directive given the attribute's value and the iframe element's iframe sandboxing flag set",
       and "These flags only take effect when the content navigable of the iframe element is navigated", which
       is this call. So a re-snapshot is owed exactly
       when a SANDBOXED navigable is navigated a second time, and that is where it crashes rather than reading
       a set that may be one attribute write out of date. */
    DCHECK(window_proxy_creation_sandbox_flags(proxy) == 0,
           "a SANDBOXED navigable was navigated, and §7.4.2.1 re-snapshots its container's IFRAME SANDBOXING "
           "FLAG SET at every navigation while this proxy carries only the set its creation computed — §4.8.5 "
           "lets the two disagree the moment a page writes `sandbox`. Give the navigable its CONTAINER "
           "(core/html/html_iframe.c holds the element) so this call can re-parse the attribute, rather than "
           "navigating a document under flags from before the write");
    if (!child_address(ctx, url, window_proxy_creation_sandbox_flags(proxy), &addr, &origin, &is_javascript)) {
        free(addr);
        return JS_UNDEFINED;   /* §7.4 step 4: the caller turns this into a SyntaxError */
    }
    /* §7.4.2.2 STEP 21 — "If url's scheme is `javascript`: QUEUE A GLOBAL TASK on the navigation and traversal
     * task source given navigable's active window to NAVIGATE TO A javascript: URL … and RETURN."
     *
     * THIS SCHEME TEST WAS WRITTEN AS STEP 16 EVERYWHERE IN THIS FILE, AND 16 NAMES A DIFFERENT STEP OF THE
     * SAME ALGORITHM: "If navigable's parent is non-null, then set navigable's is delaying load events to
     * true". That is not this reading's own count against the tree's — core/dom/document.c corrected its own
     * citation of this algorithm and named 16 the is-delaying-load-events step and 17 the target-snapshot one,
     * which is the same numbering the scheme test is 21 under. Counted over the TOP-LEVEL items of
     * `navigate`'s one step list, with the sub-lists under its earlier steps kept at their own depth.
     *
     * IT IS THE WHOLE OF WHAT THIS SCHEME GETS, and the return is the load-bearing half: nothing is fetched,
     * no session history entry is built for the URL (HTML §7.4.2.3.2's own note — "javascript: URLs are never
     * stored in session history, and so can never be traversed to"), and the document the navigable is
     * showing stays exactly where it is unless the program's completion value is a string.
     *
     * WHAT MADE THIS THE RIGHT SITE is that every navigation converges here — core/frame/location.c's twelve
     * algorithms, core/html/hyperlink.c's activation behaviour and core/html/html_iframe.c's process-the-
     * iframe-attributes all reach §7.4.2.2 through this one function — so the branch is written once. A copy at
     * whichever caller a failure happened to name would be one builtin answering differently depending on how
     * the page spelled the navigation, which is exactly the shape the load job below already crashes about.
     *
     * §7.4.2.3.2's STEPS 1-3 ARE NOT THIS BUILD'S: step 1's assert is over a historyHandling this function does
     * not carry (core/frame/location.c's loc_resolve_history_handling resolves `javascript:` to "replace"
     * before it gets here, which is §7.4.2.1's "the navigation must be a replace"'s first disjunct), and steps
     * 2-3 read and clear §7.4.2.5's ONGOING NAVIGATION, which this build does not hold — its only two writers
     * in the standard are §7.4.2.2 step 21 and §7.4.6.1's cross-document apply-the-history-step, and the second
     * does not exist here, so a field written by one of two writers would answer §8.4.1 step 8 with a fact
     * about half a mechanism.
     * STEP 4 IS BUILT AND IS A SECURITY RETURN, not a formality: "if initiatorOrigin is not same
     * ORIGIN-DOMAIN with targetNavigable's active document's origin, then return". The initiator is the
     * document whose script ran, which is `ctx`, and window_proxy_same_origin_domain_of is §7.1.1's other
     * algorithm asked of the TARGET from the asking realm — the same comparison `iframe.contentDocument` is
     * filtered by, and the reason `document.domain` participates.
     * STEPS 5-6 ARE A COMPUTED "Allowed" RATHER THAN AN OMISSION, and they are ONE test in two steps: step 5's
     * request exists only to be handed to step 6, which the standard says in its own note ("this is a synthetic
     * request solely for plumbing into the next step. It will never hit the network"). Step 6 is CSP §4.2.4
     * "Should navigation request of type be blocked by Content Security Policy?", which runs each directive's
     * PRE-NAVIGATION CHECK, and `form-action` (CSP §6.4.1.1 "form-action Pre-Navigation Check") is the only
     * directive that defines one — its first step past the assert is "if navigation type is `form-submission`",
     * and §7.4.2.2 step 1 makes cspNavigationType "other" for every navigation with no form data entry list. A
     * navigation reached through this function has none (core/html/html_form.c submits through its own path),
     * so no policy of any document can block it and the answer is "Allowed" for the reason the standard gives
     * rather than for want of the check.
     * STEP 7 IS THE EVALUATION, and steps 9-14's new Document is where the completion value goes — which is
     * the ONE place that value exists, so the string arm crashes by name in solver/engine.c rather than here.
     * STEP 8 IS THE ARM EVERY PROGRAM WITH A NON-STRING COMPLETION VALUE TAKES, and its INNER step is the one
     * thing this function cannot answer: "if initialInsertion is true and targetNavigable's active document's
     * is initial about:blank is true, then run the iframe load event steps given targetNavigable's container".
     * `initialInsertion` is an optional boolean of §7.4.2.2's navigate that nothing below it carries, and the
     * only caller that passes true is §4.8.5's post-connection steps ("process the iframe attributes for
     * insertedNode, with initialInsertion set to true") — an `<iframe>` or `<frame>` whose `src` is already a
     * `javascript:` URL at the moment it is inserted, which navigable_create refuses outright by the same fold
     * that residual names. So the branch is
     * unreachable behind an existing crash rather than skipped, and the diff that unfolds the create owes this
     * function the parameter in the same breath: without it a markup `javascript:` frame would run its program
     * and never fire its `load` event, which is a frame whose `onload` never runs and nothing to say why.
     *
     * IT IS WRITTEN ABOVE STEP 15'S ASSERTION AND THE TWO ARMS ARE DISJOINT, which is why the order here does
     * not restate the standard's. Step 15's third conjunct is "url equals navigable's ACTIVE SESSION HISTORY
     * ENTRY's URL with exclude fragments set to true", and no entry's URL is ever a `javascript:` URL —
     * §7.4.2.3.2's own note again — so a destination cannot satisfy both tests and neither arm can hide the
     * other.
     *
     * IT RUNS IN THE TARGET NAVIGABLE'S REALM, which step 7's OWN algorithm states outright — "evaluate a
     * `javascript:` URL" is defined inside §7.4.2.3.2 and numbered from one of its own, and its step 4 is "let
     * settings be targetNavigable's active document's relevant settings object". The two step lists are not
     * interchangeable and reading them as one is how this block came to cite §7.4.2.3.2's step 5 (a synthetic
     * request) for a sentence about a settings object. It is not always `ctx`: an
     * `<a href="javascript:x=1" target="frame">` writes `x` into the FRAME's global, where a later script of
     * that document reads it. The proxy is asserted local above, so the realm is this heap's. */
    if (is_javascript) {
        JSContext *target;

        if (!window_proxy_same_origin_domain_of(ctx, proxy)) {
            free(addr);
            return JS_DupValue(ctx, proxy);              /* §7.4.2.3.2 step 4's return */
        }
        /* THE TARGET'S ACTIVE DOCUMENT IS MATERIALIZED BY THE ASK, WHICH IS WHY NOTHING HERE DEMANDS IT WAS
           ALREADY. "Evaluate a `javascript:` URL" step 4 wants the settings object of the target navigable's
           ACTIVE DOCUMENT, and a navigable nothing has read through yet HAS one — the initial about:blank
           §7.3.2.1 created it with. What it does not have is a REALM, because this engine builds that lazily on
           the first read that reaches through the navigable (navigable.h states which navigable is materialized
           WHEN), and window_proxy_realm IS such a read: window_proxy.c's proxy_realm builds this Document from
           no response, which is the same Document and the same absence of a fetch a property read through the
           same proxy would produce.
           A DCHECK STOOD HERE REQUIRING THE NAVIGABLE TO BE MATERIALIZED ALREADY, and it was wrong twice. It
           was a precondition on its own successor — the line below is what establishes it — and its message
           instructed the next reader to materialize "here (navigable_realm, with no response)", which is the
           call proxy_realm already makes and would have made this the SECOND caller of a function that has
           exactly one. `iframe.src = "javascript:…"` on a frame inserted without one is the ordinary spelling
           that took it: §4.8.5's attribute change steps reach §7.4.2.2 with the frame still holding the
           about:blank its insertion created, which is precisely the document the standard names. */
        target = window_proxy_realm(ctx, proxy);
        DCHECK(target != NULL,
               "materializing the target navigable's active document answered no realm — nav_create_finish "
               "CHECKs that the host's realm builder produced one, so a null here is that guarantee having "
               "come apart between the build and this read");
        /* `addr` AND NOT `url`, because "evaluate a `javascript:` URL" step 1 is "let urlString be the result
           of running the URL SERIALIZER on url" and its step 2 removes a leading `javascript:` from THAT
           (§7.4.2.3.2's own steps 1-2 are the historyHandling assert and the ongoing-navigation test, and this
           block cited them for this sentence until the two lists were told apart) — core/html/hyperlink.c
           hands this function a raw `href` attribute value, so `<a href="JavaScript:x()">` reaches here with a
           prefix step 2 would not find and the whole URL would run as the program. */
        navigable_evaluate_javascript_url(target, addr);
        free(addr);
        return JS_DupValue(ctx, proxy);
    }
    /* HTML §7.3.2.1 "Creating browsing contexts"' DETERMINE THE ORIGIN IS NOT ASKED HERE, AND THAT IS
       §7.4.5's ORDER RATHER THAN A DEFERRAL.
       §7.4.2.2 "Beginning navigation" never determines the destination's origin — it snapshots the INITIATOR's
       ("set initiatorOriginSnapshot to sourceDocument's origin") and puts it on the document state. The
       destination's origin is born one algorithm later, inside §7.4.5 "Populating a session history entry"'s
       create-navigation-params-by-fetching: "set responseOrigin to the result of determining the origin given
       RESPONSE'S URL, finalSandboxFlags, and entry's document state's initiator origin" — after the manual
       redirect loop, and out of a flag set that the response's own CSP contributes to.
       ASKING IT HERE ASKED A QUESTION WHOSE ANSWER DID NOT EXIST YET, and the answer it got was wrong in two
       ways that are not edge cases: a request to this origin that 302s off it is a cross-instance document
       this side would have called local, and a response carrying `Content-Security-Policy: sandbox` mints a
       fresh opaque origin that no reading of the REQUEST url can produce. The load job below is where both
       inputs exist, so §7.3.2.1 runs there, once, and the cross-agent question is asked of ITS answer.
       WHAT THE JOB CARRIES IS THEREFORE THE INITIATOR'S ORIGIN — §7.4.2.2's initiatorOriginSnapshot, which is
       an INPUT of this operation in the sense §scheduler means (this realm's document is the one whose script
       ran, and by the time the job runs the only document it could ask is the one being replaced), where the
       destination's origin is an OUTPUT of the fetch the job performs. */
    /* AND THE ONE ARM WHERE THE TWO RUNS OF §7.3.2.1 MUST AGREE IS ASSERTED, so `child_address`'s answer is read
       rather than discarded: an `about:` destination has no response, so the job's run sees the same null-URL/
       about:blank case with the same source origin and must reach the same RECORD — steps 3 and 4 return
       `sourceOrigin` itself, and that identity is what makes a `data:` document's about:blank child same origin
       with it. The creation flag set is asserted empty above, so step 1 cannot separate them either. */
    DCHECK(strncmp(addr, "about:", 6) != 0 || origin == origin_agent(),
           "§7.3.2.1's determine-the-origin answered something other than the INITIATOR's origin record for an "
           "`about:` destination — steps 3 and 4 return sourceOrigin ITSELF, and the load job re-runs the same "
           "algorithm over the same URL with the same source, so a second answer here means the two runs have "
           "come apart and the loaded Document would be cross-origin to the document that navigated it");
    /* §7.1.7's INITIATOR POLICY CONTAINER — this document's, because THIS realm is the one whose script ran.
       It is the whole container and never one of its items: §7.1.7's clone moves every item at once, and the
       operation is what says whose clone it is. */
    /* §7.4.2.2 "Beginning navigation" STEP 15 IS ASKED BY THE CALLER AND CLOSED HERE, which is the shape a
       dispatch over "what is this destination" has to have: one component answers it (core/frame/
       session_history.h's session_history_is_fragment_navigation, over the ACTIVE SESSION HISTORY ENTRY's URL)
       and every consumer that must not serve an arm asserts it is unreachable for that arm. The number read 11
       at every site that named this test, and step 11 is the lazy-load one — "if container is an iframe element
       and will lazy load element steps given container returns true, then stop intersection-observing a lazy
       loading element container and set container's lazy load resumption steps to null" — which has nothing to
       do with fragments, so 11 is written down here rather than merely replaced and cannot be "corrected" back.
       core/frame/location.c carries the depth-tracked count 15 came from and the two counts of this same
       algorithm it agrees with.
       WITHOUT THIS ASSERTION the fragment arm is silent rather than absent: a destination that differs from
       the current address only in its fragment would be FETCHED, and the response would install a SECOND
       Document over the one whose script is mid-flight —
       every `location.hash = "#/route"` router torn down by its own route change, with nothing to say so.
       IT IS NOT A SECOND OPINION. The predicate has one implementation and this is a reader of it, so a caller
       that routes correctly and a caller that does not are told apart HERE rather than each deciding for
       itself. `open(url, name)` at an address that differs only in its fragment is the route that reaches this:
       §7.4.2.2 owes it §7.4.2.3.3 too, and what it needs is the same conversion core/frame/location.c's members
       took — the member becomes a machine, because §7.4.2.3.3 runs the page's `navigate` and `popstate`
       listeners and this function cannot suspend.
       IT IS ASKED ONLY OF THIS REALM'S OWN NAVIGABLE, because §7.4.2.2 step 15 compares against the TARGET
       navigable's active session history entry and this engine's session history is a PER-REALM record: asking
       `ctx` about a navigable whose record is another realm's would answer a different navigable's question and
       assert on the answer. A cross-navigable navigate (`open(url, "someFrame")`) therefore reaches this
       unasked, which is a gap named at the one site that could close it rather than a silently different
       answer — core/frame/session_history.c's sh_entries is where the one-navigable-per-record assumption is
       already asserted, and giving the predicate a navigable is what lifts both. */
    DCHECK(JS_VALUE_GET_PTR(proxy) != JS_VALUE_GET_PTR(document_window_proxy(ctx)) ||
           !session_history_is_fragment_navigation(ctx, addr),
           "§7.4.2.2's navigate reached its cross-document arm with a destination its own step 15 says is a "
           "FRAGMENT NAVIGATION — the address equals this navigable's active session history entry's URL with "
           "exclude fragments set to true and has a non-null fragment, so the spec runs §7.4.2.3.3's NAVIGATE "
           "TO A FRAGMENT and RETURNS. That algorithm is built (core/frame/session_history.h's "
           "session_history_fragment_nav_*) and this call site cannot drive it, because it fires the page's "
           "`navigate` and `popstate` listeners and this is a plain C function with no flow to suspend. Route "
           "the CALLER: make it a step machine that asks session_history_is_fragment_navigation before it gets "
           "here and drives session_history_fragment_nav_run when the answer is yes, exactly as "
           "core/frame/location.c's setters and its assign/replace do");
    /* §7.4.5: "if url matches about:blank or about:srcdoc, set aboutBaseURL to initiatorBaseURL" — the
       INITIATOR is the document whose script ran, which is this realm, and its base URL is what a relative URL
       inside the loaded `about:blank` Document must resolve against. It is decided HERE for the reason the
       address above is decided here rather than in the job. */
    /* AND SO IS THE PROVENANCE, WHICH IS THIS OPERATION'S AND NOT THE NAVIGABLE'S. §7.4.2.2's navigate is
       reached by RUNNING the page's code — a `location` assignment, a form submission, a link activation, an
       `open()` at a named navigable — so the only question left is whether the path that ran stood on an arm
       its own concrete example contradicts. */
    navigable_load_enqueue(ctx, proxy, addr, origin_agent(),
                           serialized_policy_container_of(document_policy(ctx)),
                           strncmp(addr, "about:", 6) == 0 ? document_base_url(ctx) : NULL,
                           engine_provenance_of_running_path());
    free(addr);
    return JS_DupValue(ctx, proxy);
}

/* ---- HTML §7.4.3 "Reloading and traversing"'s RELOAD ------------------------------------------------------
 *
 * See navigable.h for why it is its own algorithm and why it is a machine. What is here is the four steps and
 * the one place they collapse.
 *
 * ONE REST POINT: step 1.4's navigate event, whose own rest points belong to its work record. Steps 2-4 run
 * none of the page's code and are the tail of the entry the dispatch answers on. */
#define NAV_RELOAD_STAGES(X)                                                                                \
    X(NAV_RELOAD_EVENT, "HTML §7.4.3 step 1 (fire a push/replace/reload navigate event at the navigation "   \
                        "API with navigationType \"reload\", isSameDocument false, destinationURL the "      \
                        "navigable's active session history entry's URL, and navigationAPIState that "       \
                        "entry's navigation API state), then steps 2-4")
enum { NAV_RELOAD_STAGES(JS_STEP_STAGE_ENUM) };
/* NO STEPS ARRAY BESIDE THE ENUM, and that is the sub-machine shape rather than an omission: a label list is
   what a MEMBER's declaration carries into JSTrampStepDef so a parked flow can say where it is, and this
   record's stage is a byte inside the member that hosts it (core/frame/location.c's, whose own list names the
   stage this one rests under). core/frame/session_history.c's §7.4.2.3.3 record is declared the same way. */
#define NAV_RELOAD_ALGORITHM "HTML §7.4.3 reload a navigable"

void navigable_reload_start(NavigableReloadWork *w)
{
    /* A zeroed JSValue is the INTEGER 0 and not undefined (JS_TAG_INT is 0), so a slot read before it is
       written would hand the page a real value — the same rule and the same reason as every other work record
       in this neighbourhood. */
    w->stage = NAV_RELOAD_EVENT;
    w->url = JS_UNDEFINED;
    navigate_event_fire_work_start(&w->fire);
}

/* THE WHOLE OWNERSHIP DECLARATION, and there is no `release` beside it: everything this record holds is a
   JSValue this visit names, so the teardown frees them through the one list. */
void navigable_reload_visit(JSContext *ctx, NavigableReloadWork *w, JSStepVisit *v)
{
    v->val(ctx, &w->url);
    navigate_event_fire_work_visit(ctx, &w->fire, v);
}

void navigable_reload_begin(JSContext *ctx, NavigableReloadWork *w)
{
    JSValue state;
    const char *addr;

    DCHECK(JS_IsUndefined(w->url),
           NAV_RELOAD_ALGORITHM " was begun twice over one work record — a reload holds ONE destination and "
           "fires ONE navigate event, and the second begin would leave the first event ongoing at the "
           "Navigation with nothing left holding its destination");
    /* HTML §7.4.3 Reloading and traversing's STEP 1.2 AND STEP 1.3, WHICH ARE ONE READ OF THE ACTIVE ENTRY:
       "Let destinationNavigationAPIState be navigable's active session history entry's navigation API state.
       If navigationAPIState is not null, then set destinationNavigationAPIState to navigationAPIState."
       §7.2.4's `reload()` passes null, so the entry's state IS the answer — which is what a page's own
       `navigate` listener reads back out of `event.destination.getState()`.
       STEP 1.4's destinationURL is the same entry's URL, for the reason navigable.h gives: it and the
       Document's address are two fields with two writers. */
    w->url = session_history_active_entry_url(ctx);
    state = session_history_active_entry_navigation_state(ctx);
    addr = JS_ToCString(ctx, w->url);
    CHECK(addr != NULL, "navigable: §7.4.3's destination could not be read out of the active entry");
    /* STEP 1.4: "let continue be the result of FIRING A PUSH/REPLACE/RELOAD NAVIGATE EVENT at navigation with
       navigationType set to \"reload\", isSameDocument set to FALSE, userInvolvement set to userInvolvement,
       destinationURL set to navigable's active session history entry's URL, navigationAPIState set to
       destinationNavigationAPIState, and apiMethodTracker set to apiMethodTracker."
       ITS classicHistoryAPIState IS NULL, which is the wrapper's own default and the same asymmetry
       §7.4.2.3.3 has: §7.2.5's `pushState` is the one caller that serializes bytes to pass, and §7.4.1.1 says
       an entry's classic history API state "is never carried over". */
    navigate_event_fire_push_replace_reload_begin(ctx, &w->fire, "reload", addr,
                                                  /*is_same_document*/ false, /*classic_state*/ NULL, state);
    JS_FreeCString(ctx, addr);
    JS_FreeValue(ctx, state);
}

/* §7.4.3 STEPS 2-4, IN THE ONE SHAPE THIS ENGINE POPULATES A DOCUMENT IN — and the assertion below is what
 * says where that shape and the standard's part company, at the site rather than in prose.
 *
 * WHAT THE STANDARD DOES: step 2 sets the active entry's document state's RELOAD PENDING, step 3 takes the
 * traversable, and step 4 appends the traversal steps that apply the reload history step — §7.4.6.1's entry
 * point, which re-enters apply-the-history-step at the CURRENT step with navigationType "reload". Inside it
 * `reload pending` does exactly one thing: it is the second conjunct of the update-only test ("if
 * displayedEntry IS targetEntry and targetEntry's document state's reload pending is FALSE"), which is what
 * stops a reload from taking the exit a `pushState` takes and makes it re-populate instead. Then §7.4.5's
 * populate the history entry's document fetches, and §7.4.6.1's second half deactivates the displayed Document
 * (pageswap, unload-a-document-and-its-descendants, pagehide) and activates the entry over the new one.
 *
 * WHAT THIS ENGINE DOES: the populate, the deactivate and the activate are ONE operation here — §7.4 step 14's
 * document-load job, which fetches, builds the Document and its realm, moves the navigable's binding
 * (window_proxy_navigate) and unloads the outgoing document. §7.4.2.2's own cross-document navigate takes
 * exactly the same collapse, so a reload built on it is not a second approximation; it is the same one.
 *
 * RELOAD PENDING IS THEREFORE NOT WRITTEN, and that is a decision rather than an omission. Its only reader in
 * the standard is a test inside apply-the-history-step, and this path does not go through apply-the-history-
 * step at all — a field with a writer and no reader is the mirror of the defect CLAUDE.md's §Architecture
 * names, and it would read as evidence that the traversal machinery had been wired up when it had not. It
 * lands with §7.4.6.1's cross-document arm (core/frame/session_history.c already crashes by name where that
 * arm belongs), which is also the diff that gives this function somewhere to append traversal steps TO.
 *
 * WHAT THE COLLAPSE COSTS IS THE ENTRY, and that is what the last assertion measures. §7.4.3 re-populates the
 * navigable's EXISTING entry and keeps its document state, so a reload preserves `history.length`,
 * `history.state` and every entry a page pushed; this build populates by installing a NEW Document in a NEW
 * realm, and session_history_install_document starts that realm's list fresh at step 0. The two agree exactly
 * while the active entry IS step 0 — which is every reload of a page that has not called `pushState` — and
 * disagree silently the moment it is not, so that is the condition, at the site, in a form the next reader can
 * re-run rather than take on trust. */
static void nav_reload_enqueue(JSContext *ctx, const char *addr)
{
    JSValueConst proxy = document_window_proxy(ctx);

    DCHECK(window_proxy_is(proxy),
           NAV_RELOAD_ALGORITHM " ran in a realm whose Document has no navigable — §7.2.4's `reload()` returns "
           "at its own step 2 when the relevant Document is null, so a realm with no WindowProxy at all "
           "reached this past a check that was supposed to have stopped it");
    /* §7.4.6.1 says a reload "is always treated as if it were done by the navigable ITSELF, even in cases like
       parent.location.reload()", so the target is this realm's own navigable and never a peer's — which is
       also what makes the initiator facts below this realm's to state. */
    DCHECK(!window_proxy_is_remote(proxy),
           NAV_RELOAD_ALGORITHM " was asked to reload a navigable whose ACTIVE DOCUMENT a PEER instance holds "
           "— §7.4.5's populate builds that Document, its policy container and its origin in the instance that "
           "holds the navigable, so this is a host route and not a load this agent can perform");
    /* §7.4.6.1's "let targetSnapshotParams be the result of SNAPSHOTTING TARGET SNAPSHOT PARAMS given
       navigable", whose sandboxing flags are determine-the-creation-sandboxing-flags over the navigable's
       CONTAINER — the same re-snapshot navigable_navigate owes and for the same reason (§4.8.5 lets a written
       `sandbox` attribute and the set computed at creation disagree, and the attribute "only takes effect when
       the content navigable is navigated", which a reload is). */
    DCHECK(window_proxy_creation_sandbox_flags(proxy) == 0,
           "a SANDBOXED navigable was RELOADED, and §7.4.6.1 re-snapshots its container's IFRAME SANDBOXING "
           "FLAG SET while this proxy carries only the set its creation computed — §4.8.5 lets the two "
           "disagree the moment a page writes `sandbox`. Give the navigable its CONTAINER "
           "(core/html/html_iframe.c holds the element) so this call can re-parse the attribute, rather than "
           "re-populating a document under flags from before the write");
    DCHECK(session_history_active_entry_step(ctx) == 0,
           "§7.4.3's reload re-populates the navigable's EXISTING session history entry and keeps its document "
           "state — so `history.length`, `history.state` and every entry a page pushed survive it — and this "
           "build populates by installing a NEW Document in a NEW realm, whose "
           "session_history_install_document starts a fresh ONE-ENTRY list at step 0 and asserts one Document "
           "per realm. The active entry is past step 0, so those entries are about to be discarded and "
           "`history.length` will answer 1. BUILD THE CARRY: the entries are ordinary JS objects in one heap, "
           "so the load job can hand the incoming Document's install the list and the active entry it is "
           "re-populating instead of letting that install mint a first entry — which is the same diff that "
           "makes a cross-document NAVIGATION stop resetting the history, and the diff §7.4.6.1's "
           "cross-document apply-the-history-step needs under it");
    /* §7.4.5's INITIATOR BASE URL for an `about:` destination, and §7.1.7's initiator policy container: for a
       reload BOTH are this document's own, which is not the §scheduler shortcut it would be for a navigation.
       §7.4.6.1 states it — "we treat this situation as if navigable navigated itself" and
       "potentiallyTargetSpecificSourceSnapshotParams … the result of snapshotting source snapshot params given
       navigable's ACTIVE DOCUMENT" — so the operation's initiator IS the target here, and reading the target
       is reading the operation's own input. They are still read at the ENQUEUE and never inside the job, for
       the reason navigable_load_enqueue states: by the time the job runs the only document it could ask is the
       one being replaced.
       THE PROVENANCE IS THIS RELOAD'S OWN PATH. §7.4.3's reload is `location.reload()` and the traversal
       members beside it — the page's own code, every time — so the fact that decides it is the same one every
       other running-code act asks. A reload of a document that only exists because a gate was forced is still
       a request no client makes, which is why this is the running path's answer and not the address's. */
    navigable_load_enqueue(ctx, proxy, addr, origin_agent(),
                           serialized_policy_container_of(document_policy(ctx)),
                           strncmp(addr, "about:", 6) == 0 ? document_base_url(ctx) : NULL,
                           engine_provenance_of_running_path());
}

int navigable_reload_run(JSContext *ctx, NavigableReloadWork *w, JSValue in, JSValue **out_cb, int *out_argc)
{
    bool proceed = false;
    const char *addr;
    int r;

    STEP_DISPATCH(NAV_RELOAD_STAGES, w->stage, NAV_RELOAD_ALGORITHM, JS_STEP_ABRUPT);

    STEP_ARM(NAV_RELOAD_EVENT);
    DCHECK(JS_IsString(w->url),
           NAV_RELOAD_ALGORITHM " was driven over a record nothing began — navigable_reload_begin is what takes "
           "the destination off the active session history entry, and an algorithm with no destination has "
           "nothing to ask the page about and nothing to populate");
    /* §7.4.3 STEP 1.4, DRIVEN. It owns every rest point past this line — the `navigate` dispatch, and
       §7.2.6.8's abort at either of the two steps that perform one — and re-enters itself at whichever of them
       its own record holds, so a resume here runs none of this algorithm's steps again. */
    r = navigate_event_fire_run(ctx, &w->fire, in, out_cb, out_argc, &proceed);
    if (r != 0) return r;
    /* STEP 1.5: "If continue is false, then RETURN." A cancelled reload is not an error and has no observable
       of its own — §7.2.4's `reload()` answers undefined either way — so this is the same 0 a completed
       reload returns. */
    if (!proceed) return 0;
    addr = JS_ToCString(ctx, w->url);
    CHECK(addr != NULL, "navigable: §7.4.3's destination could not be read back after its navigate event");
    nav_reload_enqueue(ctx, addr);
    JS_FreeCString(ctx, addr);
    return 0;
}

/* §7.4.2.3.2's EVALUATE A JAVASCRIPT: URL — see navigable.h for why it is not a load and not a fetch. */
void navigable_evaluate_javascript_url(JSContext *ctx, const char *url)
{
    static const char SCHEME[] = "javascript:";
    const char *encoded;
    char *source;
    size_t n = 0;

    DCHECK(url != NULL, "evaluate a javascript: URL was handed no URL — step 1 serializes one that exists");
    /* Steps 1-2: the URL serializer, then "remove the leading `javascript:` from urlString". The caller parsed
       the URL, so the scheme is already decided and this asserts rather than re-deciding it — two answers to
       "is this a javascript: URL" is exactly how a caller ends up running the wrong bytes. */
    DCHECK(strncmp(url, SCHEME, sizeof SCHEME - 1) == 0,
           "evaluate a javascript: URL was handed a URL of another scheme — step 2 removes a prefix that is "
           "not there, and what would run is the whole URL");
    encoded = url + (sizeof SCHEME - 1);
    /* Step 3: "Let scriptSource be the UTF-8 decoding of the percent-decoding of encodedScriptSource."
       TWO ALGORITHMS, AND THE SECOND IS NOT A FORMALITY. URL §1.3's percent-decode answers a BYTE SEQUENCE —
       "let output be an empty byte sequence … append byte to output" — and `%XX` reaches every one of the 256,
       so what comes back is not text and is not this engine's UTF-8. The decode is what makes it a string, and
       the standard's own note beside percent-decode says why it cannot be skipped: "using anything but UTF-8
       decode without BOM when input contains bytes that are not ASCII bytes might be insecure".
       IT IS ENCODING §6's UTF-8 DECODE, NOT THE WITHOUT-BOM HOOK the rest of this tree runs. HTML links this
       step at `#utf-8-decode`, whose first two steps are "let buffer be the result of peeking three bytes from
       ioQueue … if buffer is 0xEF 0xBB 0xBF, then read three bytes from ioQueue" — and §6 says which callers
       get which: "for decoding, UTF-8 decode is to be used by new formats. For identifiers or byte sequences
       within a format or protocol, use UTF-8 decode without BOM". A script source is a format's text; a host
       and a urlencoded name are identifiers within one, which is why url.c runs the other hook.
       WHAT THIS ENGINE DID BEFORE, and it is not a rounding error: the percent-decoded bytes went to the
       compiler as they came, so `javascript:%FF` handed the lexer a byte no UTF-8 sequence contains and the
       page got a SyntaxError from quickjs's `next_token` ("invalid UTF-8 sequence") where the "replacement"
       error mode puts one U+FFFD in the source and the program RUNS; and `javascript:x='%ED%A0%80'` went the
       other way, compiling a lone surrogate — which that decoder accepts and this one replaces — into a string
       no browser's decoder can produce. A `javascript:` URL is a sink whose breakout is constructed from the
       bytes that survive to it, so a decode that disagrees with the browser's mis-places every one of them. */
    {
        size_t rawn = 0;
        char *raw = url_percent_decode(encoded, strlen(encoded), &rawn);
        source = encoding_utf8_decode(raw, rawn, &n);
        free(raw);
    }
    /* AND THE RESULT IS A SCALAR VALUE STRING, which is what the error mode buys and is asserted rather than
       assumed: "replacement" answers U+FFFD for every malformed sequence, so nothing ill-formed can leave that
       hook. It is asserted HERE because of what this source reaches — a lexer that rejects an ill-formed byte
       outright (quickjs.c's `invalid_utf8`), so the day the two disagree the page loses its program and the
       only symptom is a SyntaxError the browser does not have. */
    DCHECK(encoding_is_scalar_value_string(source, n),
           "the UTF-8 decode of a javascript: URL's script source answered bytes that are not well-formed — "
           "Encoding §6's hook runs its decoder in \"replacement\" error mode, which makes every malformed "
           "sequence a U+FFFD, so this is the decoder contradicting its own error mode and what it reaches is "
           "a compiler that refuses the byte rather than the program");
    /* A U+0000 IN THIS SOURCE IS ORDINARY AND IS NO LONGER ASSERTED AGAINST. A `strlen(source) == n` DCHECK
       stood here and named what to build — "build the queue over a length" — and the queue is built over one:
       engine_queue_javascript_url takes `(source, n)` and the row carries both to the compiler. The assertion
       is DELETED rather than weakened, because the state it forbade is the state the spec produces: step 3's
       percent-decode is URL §1.3 "Percent-encoded bytes", whose output is a BYTE SEQUENCE that `%XX` reaches
       every value of, and ECMAScript §11.1 "Source Text" says every code point from U+0000 up may occur in
       source text. `javascript:x='%00'` is a program, not a malformed URL. */
    /* Steps 4-7: the classic script is created with the target navigable's active document's settings and API
       base URL — this document's, which is what makes this the same-navigable case the header names — and RUN.
       Its completion value decides step 9, and the scheduler is the only place that value exists (engine.h). */
    /* IN THE TARGET NAVIGABLE'S ACTIVE DOCUMENT, which step 5 names as the settings object the script is
       created with. `ctx` is that document's realm — the activation behaviour that reached here ran in it — so
       the program is a program OF that document and is compiled there. */
    engine_queue_javascript_url(document_doc(ctx), source, n);
    free(source);
}

/* §7.1's RULES FOR CHOOSING A NAVIGABLE, for the four KEYWORD targets — the ones whose leading underscore says
 * "reuse a navigable" rather than "name a new one". `_blank` is the odd one: it is the only keyword that means
 * CREATE, which is why it answers JS_UNDEFINED here and every other answer is a navigable to navigate.
 * A NON-KEYWORD name that matches an existing navigable is the fifth rule and is NOT built: it needs the
 * familiar-with walk over the navigable tree, and the tree's children are the iframe elements' — so it is the
 * next piece, and until it is here such a name creates a new navigable and is GIVEN that name, which is what
 * the same rule says for a name that matches nothing. */
/* §7.1'S FIFTH RULE — a target that is a NAME rather than a keyword names a navigable to REUSE when one with
 * that name is FAMILIAR WITH the source. What follows is that search, and both halves of it are the spec's.
 *
 * WHERE IT LOOKS. A navigable is reachable two ways and neither is a registry keyed by document: DOWN the
 * document tree (a navigable's children are its active document's iframes, walked from the tree on every ask so
 * the answer is this flow's), and ACROSS the browsing context group (the top-level traversables this agent has
 * opened, which no document holds a reference to — a page may drop `open()`'s return value and the window is
 * still there to be targeted, so the GROUP holds them, exactly as a browser does).
 *
 * THE GROUP LIST IS A JS ARRAY, and that is load-bearing rather than convenient. A navigable one forked arm
 * opened must be invisible to its sibling — otherwise `open(url, "x")` in arm A is found and navigated by arm
 * B, which is two timelines sharing a window. An array's mutations are property writes, so the COW delta
 * captures them per flow for free and the list parks and resumes with the flow like everything else; a
 * malloc'd list would have needed its own delta kind and would have leaked its entries past every unapply.
 *
 * FAMILIAR WITH reduces to "in this agent" here, and that is not a simplification: an instance IS an
 * origin-keyed agent cluster (SECURITY.md), so every navigable this walk can reach is same origin with the
 * source by construction, which is §7.1's first familiar-with clause. A cross-origin navigable is a peer's and
 * is not in this heap to be found.
 *
 * IT DOES NOT MATERIALIZE ANYTHING. An unmaterialized navigable holds the about:blank Document §7.4 created it
 * with, which has no iframes, so there is nothing below it to walk — see window_proxy.h. */
static JSValue g_group = JS_UNDEFINED;   /* the browsing context group's top-level traversables (baseline) */

/* THE ONE WALK OVER THE NAVIGABLE TREE, in HTML §7.3.1.5 "Related navigable collections" order, as an Array of
   WindowProxy (owned). Every navigable reachable from `root`, `root` first.
   PRE-ORDER, and that is the spec's own construction rather than a choice: inclusive descendant navigables of a
   Document is «that Document's node navigable» followed by its DESCENDANT navigables, and the descendants step
   walks the navigable containers in shadow-including tree order EXTENDING with each one's content navigable's
   active document's INCLUSIVE descendant navigables — one container's whole subtree before the next container,
   which is depth-first pre-order. §7.3.1.5 states the contract it wants callers to read: "The return values of
   these algorithms are ordered so that parents appears before their children. Callers rely on this ordering."
   Children are pushed in REVERSE so they pop in tree order; a stack walked forwards reports a container's
   frames back to front.
   THE WALK IS ITERATIVE, over an explicit worklist, and that is the rule rather than a preference: a tree walk
   written as a self-call is C-to-C recursion whose depth is the PAGE's iframe nesting, which is the page's to
   choose — the same reason every call in this engine trampolines onto the heap. The worklist IS the stack, in a
   place a walk can be measured and, when this becomes a step machine, suspended.
   IT FILTERS ONLY WHAT IS NOT A NAVIGABLE TO VISIT AT ALL — something that is not a WindowProxy, and a closed
   one, which §7.3.1.6 destroyed. Everything else is EMITTED and each caller applies its OWN filter; what they
   share is the ORDER, which is the spec's and not theirs.
   IT DOES NOT MATERIALIZE ANYTHING: an unmaterialized navigable still holds the initial about:blank Document
   it was created with, which has no navigable containers, so "not materialized" and "no children" are one
   answer (window_proxy.h). That test is also what keeps a PEER's navigable out of the descent — a proxy over a
   document this agent does not hold carries no realm, so window_proxy_realm, which crashes for one, is never
   reached with one. */
static JSValue nav_preorder(JSContext *ctx, JSValueConst root)
{
    JSValue out = JS_NewArray(ctx), stack = JS_NewArray(ctx);
    uint32_t nout = 0, ntop = 0;

    CHECK(!JS_IsException(out) && !JS_IsException(stack),
          "navigable: OOM walking the navigable tree");
    JS_SetPropertyUint32(ctx, stack, ntop++, JS_DupValue(ctx, root));
    while (ntop > 0) {
        JSValue proxy = JS_GetPropertyUint32(ctx, stack, --ntop);

        JS_SetPropertyUint32(ctx, stack, ntop, JS_UNDEFINED);
        if (!window_proxy_is(proxy) || window_proxy_closed(ctx, proxy)) { JS_FreeValue(ctx, proxy); continue; }
        if (window_proxy_materialized(proxy)) {
            JSContext *realm;
            int i, n;

            /* ASSERTED BEFORE THE REALM IS ASKED FOR, because window_proxy_realm CRASHES for a peer's and would
               then report the descent as a missing capability rather than as this walk's broken assumption. */
            DCHECK(!window_proxy_is_remote(proxy),
                   "a navigable this agent does not hold answered as materialized — the walk would descend "
                   "into a peer's document out of this heap");
            realm = window_proxy_realm(ctx, proxy);
            DCHECK(realm != NULL, "a materialized navigable answered with no realm — window_proxy_materialized "
                                  "is what says one is there to answer with");
            n = iframe_child_navigable_count(realm);
            for (i = n - 1; i >= 0; i--)
                JS_SetPropertyUint32(ctx, stack, ntop++, iframe_child_navigable(realm, i));
        }
        JS_SetPropertyUint32(ctx, out, nout++, proxy);   /* the list takes this reference */
    }
    JS_FreeValue(ctx, stack);
    return out;
}

/* HTML §7.3.1.7 "Navigable target names", find a navigable by target name: "For each navigable of the inclusive
   descendant navigables of documentToSearch … if navigable's target name is name, then return navigable." The
   FIRST match in §7.3.1.5's order wins, and that order is pre-order — for a root with children A and B where A
   has a child named `t` and B is itself named `t`, the answer is A's child. It was breadth-first here, which
   answered B, justified by a claim that the spec wants the navigable NEAREST the source; §7.3.1.7 and §7.3.1.5
   contain no such notion, so the comment that said so is gone with the loop it described.
   IT FILTERS NOTHING nav_preorder emits. An UNMATERIALIZED navigable is a candidate — §7.3.1.7 reads a target
   name off the navigable's active session history entry's document state, which the create stated, and no part
   of that needs a realm. It is exactly the window `open(url, "t")` made that nothing has read through yet, so
   skipping it would open a SECOND window under a name that is already taken. */
static JSValue nav_find_in_tree(JSContext *ctx, JSValueConst root, const char *name)
{
    JSValue all = nav_preorder(ctx, root), hit = JS_UNDEFINED, len;
    uint32_t i, n = 0;

    len = JS_GetPropertyStr(ctx, all, "length");
    JS_ToUint32(ctx, &n, len);
    JS_FreeValue(ctx, len);
    for (i = 0; i < n && JS_IsUndefined(hit); i++) {
        JSValue proxy = JS_GetPropertyUint32(ctx, all, i);
        /* §7.4 gave this navigable its name and §7.11 lets a document rename its own — one record, read here. */
        const char *nm = window_proxy_name(proxy);

        if (nm && !strcmp(nm, name)) hit = proxy;
        else JS_FreeValue(ctx, proxy);
    }
    JS_FreeValue(ctx, all);
    return hit;
}

/* See navigable.h. The ORDER is nav_preorder's; what is this walk's own is the FILTER — a navigable this
   instance has not materialized has no document to run a lifecycle for, and a peer's is not this agent's to
   answer for at all. */
/* One tree of `out`, filtered — see navigable_tree_order. `nout` is carried across the trees so the caller
   still receives ONE list in ONE order. */
static void nav_append_tree(JSContext *ctx, JSValueConst out, uint32_t *nout, JSValueConst root)
{
    JSValue all = nav_preorder(ctx, root), len;
    uint32_t i, n = 0;

    len = JS_GetPropertyStr(ctx, all, "length");
    JS_ToUint32(ctx, &n, len);
    JS_FreeValue(ctx, len);
    for (i = 0; i < n; i++) {
        JSValue proxy = JS_GetPropertyUint32(ctx, all, i);

        if (window_proxy_is_remote(proxy) || !window_proxy_materialized(proxy)) JS_FreeValue(ctx, proxy);
        else JS_SetPropertyUint32(ctx, (JSValue)out, (*nout)++, proxy);   /* the list takes this reference */
    }
    JS_FreeValue(ctx, all);
}

/* See navigable.h. The ORDER is nav_preorder's; what is this walk's own is the FILTER and the SET OF ROOTS.
 *
 * IT IS EVERY TOP-LEVEL TRAVERSABLE OF THE GROUP AND NOT ONLY THE ASKING DOCUMENT'S, and that is the whole
 * difference between "this document's frames" and "this agent's documents" — which is what all three of its
 * consumers are about. §8.1.1 gives a similar-origin window agent ONE event loop, so §8.1.7.3's document list,
 * §8.7's timer task source and §13.2.7's load lifecycle are agent facts; each of them asked this function and
 * each of them got one TREE. An AUXILIARY navigable is a top-level traversable of its own (§7.3.1.7 step 8),
 * reachable from the group and from nothing else, so a `window.open()`ed popup was in NONE of those three
 * lists: its document never left "loading", so it never fired DOMContentLoaded, never fired `load`, never
 * fired `pageshow`, and its timers and rendering opportunities did not exist either. A popup that posts back to
 * its opener from a `load` listener — which is how WPT's own /common/PrefixedPostMessage.js resource documents
 * are written — therefore posted nothing, ever, and the opener waited.
 * THE ASKING DOCUMENT'S TREE COMES FIRST and the group's other roots follow it, so the order within the tree
 * every existing caller was already seeing is unchanged; what is added is appended. The root traversable is
 * skipped when the group also holds it, exactly as navigable_choose_name's step 9 loop skips it, because a
 * duplicate here is a document whose lifecycle stage would be asked for twice in one pass.
 * THE FILTER IS UNCHANGED AND IS WHAT KEEPS THIS FREE: an unmaterialized navigable is still not in the list, so
 * widening the roots adds the popups a page actually opened and not one entry per navigable a forced-execution
 * frontier ever created. */
JSValue navigable_tree_order(JSContext *ctx)
{
    JSValue top = window_proxy_top_navigable(ctx, document_window_proxy(ctx));
    JSValue out = JS_NewArray(ctx), len;
    uint32_t i, n = 0, nout = 0;

    CHECK(!JS_IsException(out), "navigable: OOM walking this agent's navigables");
    nav_append_tree(ctx, out, &nout, top);
    DCHECK(JS_IsArray(g_group), "the browsing context group's list was read before navigable_init built it");
    len = JS_GetPropertyStr(ctx, g_group, "length");
    JS_ToUint32(ctx, &n, len);
    JS_FreeValue(ctx, len);
    for (i = 0; i < n; i++) {
        JSValue other = JS_GetPropertyUint32(ctx, g_group, i);

        if (!JS_IsSameValue(ctx, other, top)) nav_append_tree(ctx, out, &nout, other);
        JS_FreeValue(ctx, other);
    }
    JS_FreeValue(ctx, top);
    return out;
}

/* HTML §7.3.1.7 "Navigable target names" spells its four reserved tokens as "an ASCII case-insensitive match",
 * and this component compared them with `strcmp` — so `window.open(u, "_SELF")` opened a window where the spec
 * navigates the current navigable, and `open(u, "_ToP")` did the same. The corpus tests this by name:
 * `choose-_self-002.html`, `choose-_parent-004.html` and `choose-_top-003.html` all carry "(case-sensitivity)"
 * in their titles, and `choose-_top-003-iframe-2.html` opens with the literal spelling `"_ToP"`.
 * NOT `strcasecmp`, WHICH IS THE LOCALE'S — the spec says ASCII, and a Turkish locale folds `I` to `ı`. The
 * keyword is always the lower-case literal, so only the left operand needs folding. */
static bool target_name_is(const char *name, const char *keyword)
{
    size_t i;

    for (i = 0; keyword[i]; i++) {
        char c = name[i];

        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != keyword[i]) return false;
    }
    return name[i] == '\0';
}

/* §7.3.1.7 "Navigable target names", FIND A NAVIGABLE BY TARGET NAME, given `name` and this navigable.
 *
 * THE SUBTREES ARE SEARCHED IN REVERSE ORDER, AND THAT IS THE HALF THIS FUNCTION DID NOT HAVE. Step 3 offers an
 * IMPLEMENTATION-DEFINED choice of two subtree lists (the spec's own Issue #10848 tracks settling it), and this
 * engine takes the first, « currentNavigable's traversable navigable, currentNavigable » — which step 6 then
 * walks "in reverse order", so THIS navigable's own subtree is searched FIRST and the traversable's whole tree
 * second. This function started at the traversable and never looked at the requestor's subtree at all, so a
 * name held by BOTH a child of the requestor and a sibling of it answered the SIBLING — the requestor's own
 * frame lost to a stranger's. `duplicate-name-order.html` asserts exactly that, in exactly those words:
 * `subtree first`, then `then the rest of the tree`, then `then other pages`.
 * The first match wins in each tree rather than the best — §7.3.1.7 and §7.3.1.5 define no ranking. */
static JSValue navigable_choose_name(JSContext *ctx, const char *name)
{
    JSValueConst self = document_window_proxy(ctx);
    /* §7.3.1.7's steps 3 and 9 name "currentNavigable's TRAVERSABLE NAVIGABLE", which is the tree relation and
       NOT §7.2.2.4's `top` getter — window_proxy.h keeps the two spellings apart and this file already asks
       for the walk's one everywhere else it walks. The getter answers a Window for a top-level document (a
       navigable's own proxy is spelled as its global) and null for a destroyed one, and nav_preorder skips
       anything that is not a WindowProxy — so BOTH of this function's uses of the answer were dead: the
       second-subtree search walked an empty list, and step 9's "if currentTopLevelBrowsingContext is
       topLevelBrowsingContext, then continue" never matched, so the traversable's whole tree was searched a
       second time inside the group loop. */
    JSValue top = window_proxy_top_navigable(ctx, self);
    JSValue hit;
    uint32_t i, n = 0;
    JSValue len;

    /* THE KEYWORDS NEVER REACH THE SEARCH, and that is a rule of the algorithm rather than of its callers:
       steps 4-6 answer `_self`/`_parent`/`_top` before step 7 exists, and step 7 itself excludes `_blank`. If
       one arrives here the rules have been re-ordered, and the visible symptom would be a page naming a frame
       `_top` and then being HANDED it — a navigable answering to a token the spec reserves. */
    DCHECK(!target_name_is(name, "_self") && !target_name_is(name, "_parent") &&
           !target_name_is(name, "_top") && !target_name_is(name, "_blank"),
           "§7.3.1.7's find-a-navigable-by-target-name was asked for one of the four reserved keywords — the "
           "rules for choosing a navigable consume all four before the search, so a keyword here means a "
           "navigable can be given a name the spec never lets it answer to");
    /* §7.3.1.7 compares target names for EQUALITY. Only the four keywords above are matched ASCII
       case-insensitively, so nav_find_in_tree's strcmp is the spec's own comparison and not an oversight. */
    hit = nav_find_in_tree(ctx, self, name);
    /* THE TWO SUBTREES ARE ONE SUBTREE FOR A TOP-LEVEL DOCUMENT, which is most of them: `self` IS the
       traversable, so the second walk would re-ask the identical question of the identical tree. The spec's
       list is « traversable, currentNavigable » and a duplicate member costs a walk and can change no answer,
       since the first match in a pre-order over the same root is the same navigable. */
    if (JS_IsUndefined(hit) && !JS_IsSameValue(ctx, self, top)) hit = nav_find_in_tree(ctx, top, name);
    if (!JS_IsUndefined(hit)) { JS_FreeValue(ctx, top); return hit; }
    DCHECK(JS_IsArray(g_group), "the browsing context group's list was read before navigable_init built it");
    len = JS_GetPropertyStr(ctx, g_group, "length");
    JS_ToUint32(ctx, &n, len);
    JS_FreeValue(ctx, len);
    for (i = 0; i < n; i++) {
        JSValue other = JS_GetPropertyUint32(ctx, g_group, i);

        /* §7.3.1.7's step 9 loop over the group opens "If currentTopLevelBrowsingContext is
           topLevelBrowsingContext, then continue" — the two searches above already covered this tree, and
           walking it again is the same answer computed twice for every miss. */
        if (JS_IsSameValue(ctx, other, top)) { JS_FreeValue(ctx, other); continue; }
        hit = nav_find_in_tree(ctx, other, name);
        JS_FreeValue(ctx, other);
        if (!JS_IsUndefined(hit)) { JS_FreeValue(ctx, top); return hit; }
    }
    JS_FreeValue(ctx, top);
    return JS_UNDEFINED;
}

/* A TOP-LEVEL TRAVERSABLE JOINS THE GROUP. A child navigable does not: it is reachable down its parent's
   document tree, and putting it here as well would make the walk visit it twice. */
static void navigable_group_add(JSContext *ctx, JSValueConst proxy)
{
    uint32_t n = 0;
    JSValue len;

    DCHECK(JS_IsArray(g_group), "a navigable was created before navigable_init built the group's list");
    len = JS_GetPropertyStr(ctx, g_group, "length");
    JS_ToUint32(ctx, &n, len);
    JS_FreeValue(ctx, len);
    JS_SetPropertyUint32(ctx, g_group, n, JS_DupValue(ctx, proxy));
}

/* §7.3.1.7's STEPS 4-6 — the three keywords that answer with an existing navigable, ASCII case-insensitively
 * and in the spec's own order. `_blank` is NOT here: step 7 excludes it from the search and step 8 creates, so
 * it is the caller's fall-through rather than a fourth arm returning "nothing".
 * NOR IS ANY OTHER `_`-PREFIXED TOKEN. This function used to be reached by `target[0] == '_'`, which handed it
 * `_foo` and got JS_UNDEFINED back — so `open("", "_foo")` minted a window with NO NAME, and a second call
 * minted another. §7.3.1.7 reserves exactly four tokens; `_foo` is not one of them, so step 7 SEARCHES for it
 * and step 8 gives the created navigable that very name. (A leading `_` makes it not a "valid navigable target
 * name" — that is an authoring-conformance sentence in the same section, and the algorithm below it does not
 * consult it.) */
static JSValue navigable_choose_keyword(JSContext *ctx, const char *target)
{
    JSValueConst self = document_window_proxy(ctx);

    DCHECK(target != NULL, "the rules for choosing a navigable were given a null name — §7.3.1.7 is written "
                           "over a STRING, and navigable_open turns an absent target into the empty one so "
                           "that step 4's two spellings of \"no target\" arrive here as one");
    if (!*target || target_name_is(target, "_self")) return JS_DupValue(ctx, self);
    if (target_name_is(target, "_parent")) {
        /* §7.3.1.7 step 5: "currentNavigable's parent, if any, and currentNavigable otherwise" — a navigable
           with no parent is its own parent, so the keyword never reaches past the top. */
        /* THE TREE RELATION, NOT §7.2.2.4's GETTER — the same distinction navigable_choose_name draws above.
           §7.3.1.7 step 5 has no null arm, while the getter's own step 2 answers null for a navigable §7.5.10
           destroyed; reading the getter here would make `open(url, "_parent")` from a removed frame answer the
           frame itself where the section names its parent. window_proxy_parent_navigable is JS_UNDEFINED at the
           top of the tree, which is step 5's "and currentNavigable otherwise". */
        JSValue p = window_proxy_parent_navigable(ctx, self);
        if (window_proxy_is(p)) return p;
        JS_FreeValue(ctx, p);
        return JS_DupValue(ctx, self);
    }
    /* Step 6's "currentNavigable's traversable navigable" — the walk's spelling for the same reason, and here
       the getter's null would fall past `window_proxy_is(chosen)` in the caller and MINT A NEW WINDOW where
       the section names one that already exists. */
    if (target_name_is(target, "_top")) return window_proxy_top_navigable(ctx, self);
    return JS_UNDEFINED;
}

JSValue navigable_create(JSContext *ctx, const char *url, const char *name, bool is_child,
                         const WindowFeatures *feat, SandboxFlags iframe_sandbox_flags,
                         JSValueConst container)
{
    const PolicyContainer *creator_container = document_policy(ctx);
    /* §7.1.7's CLONE OF THE CREATOR'S CONTAINER, in the form it travels — the ONE value this whole function
       hands on, to the local navigable's WindowProxy, to §7.4 step 14's load, and across the notice that
       provisions a peer instance. It is serialized ONCE, here, so those three cannot disagree about what the
       creator's container held, and so that an item added to a container reaches all three or none.
       It is the CREATOR's and never the child's, which is the whole content of CSP §2.2's note about the
       self-origin: a document that INHERITED its policy resolves `'self'` against the origin the policy came
       FROM, and the initial about:blank is exactly such a document. */
    SerializedPolicyContainer creator_policy;
    /* CLAUDE.md §A-REQUEST-CARRIES-THE-PROVENANCE's word for the navigation this create FOLDS IN — §7.3.1.3
       "Child navigables"' step 14, whose load reaches the network exactly as §7.4.2.2's navigate does.
       COMPUTED ONCE, HERE, AND READ BY BOTH ARMS BELOW. The two arms are one operation seen from two sides: a
       SAME-ORIGIN child's load is enqueued as a job in this heap and a CROSS-ORIGIN child's is announced to a
       host that provisions a peer, and it would be incoherent for the agent boundary to change what this
       navigation is evidence of. Asking twice is what would let them disagree, and the field is a firing
       decision at both ends.
       A DOCUMENT-INSTALL CREATE HAS NO RUNNING FLOW, and that is answered rather than asserted against:
       §4.8.5's insertion steps run for every `<iframe>` in the initial markup before this agent's frontier is
       seeded (see navigable_load_enqueue's own note about the task that has no owner), so the path that would
       say whether a gate was forced does not exist and no gate was forced — which is what
       engine_provenance_of_running_path answers `derived` for, in the under-claiming direction. */
    const char *provenance = engine_provenance_of_running_path();
    char *addr = NULL;
    const Origin *origin = NULL;
    /* §7.4.2.2's scheme dispatch, as child_address answered it — see the assertion below for why a create is
       the arm that has no answer for it rather than the arm that serves it. */
    bool destination_is_javascript = false;
    /* §7.1.5's DETERMINE THE CREATION SANDBOXING FLAGS, and the two arms are §7.4's two shapes of navigable.
       A CHILD has an embedder element, so its set is the union of that element's IFRAME SANDBOXING FLAG SET
       and the embedder document's own ACTIVE SANDBOXING FLAG SET — which is how a sandbox is inherited down a
       frame tree with no keyword able to escape it. An AUXILIARY navigable has no embedder, so its set is the
       new top-level browsing context's POPUP SANDBOXING FLAG SET, which §7.1's rules for choosing a navigable
       fill from the OPENER's flags only when the propagate flag survived the parse — the one place
       `allow-popups-to-escape-sandbox` acts. */
    SandboxEmbedder embedder;
    SandboxFlags creation_flags;
    uint32_t child;
    JSValue proxy;
    char *op;
    /* §7.3.1.3's PARENT, in the form a navigable crosses an instance boundary in — filled only on the arm that
       emits the notice, because that is the only arm where a navigable's tree has to be described to someone
       who cannot see it. */
    char *parent_id = NULL;
    /* AND WHAT §7.3.1.3's CONTAINER ANSWERED, on the same arm and for the mirror reason: the peer holds the
       navigable and this instance holds the ELEMENT that presents it. Permissions Policy §9.5's result, as
       text; see the notice below. */
    char *container_policy = NULL;
    /* AND WHAT HTML §3.1.3 "Ancestor origins"' STEPS ANSWERED FOR THAT SAME NAVIGABLE, on the same arm and for
       the same reason twice over: every input those steps read — the parent Document's own list, its ORIGIN
       RECORD, the container element — is in THIS heap, and the one that could not be sent even if the others
       were is the origin record, which is what §7.1.1's same origin decides step 10 by. See the notice below. */
    char *ancestor_origins = NULL;
    /* AND §7.1.5's CREATION SANDBOXING FLAG SET FOR THAT SAME NAVIGABLE, as text, on the same arm and for the
       same reason as the three above: both of that section's inputs — the `<iframe sandbox>` ELEMENT and this
       document's own active set — are in this heap, and an element crosses no instance boundary. See the
       notice below. */
    char *sandbox_flags = NULL;
    /* HTML §7.3.2.1's CREATE A NEW BROWSING CONTEXT AND DOCUMENT, verbatim: "Let topLevelCreationURL be
       about:blank if embedder is null; otherwise embedder's relevant settings object's top-level creation
       URL." The embedder is the element this navigable is nested THROUGH, so `is_child` IS that condition —
       an auxiliary navigable has none and is its own top, and the about:blank it is created with is the
       address of the top-level environment until step 14's navigation moves it (js_nav_load_step).
       READ FROM THE CREATOR'S REALM, which is what "embedder's relevant settings object" says: this document
       is the embedder, `ctx` is its realm, and the field is the one its own environment was built with. */
    JSValue creator_tlu = realm_top_level_creation_url(ctx);
    const char *creator_tlu_s = JS_ToCString(ctx, creator_tlu);
    const char *tlu;
    /* AND THE OTHER HALF OF §8.1.3.1's PAIR, from the SAME sentence of §7.3.2.1 and answered differently: "Let
       topLevelOrigin be origin if embedder is null; otherwise embedder's relevant settings object's top-level
       origin", where `origin` is step 11's — the origin of the INITIAL about:blank Document this create makes
       the navigable with, which is the one this same function hands window_proxy_new below. So a CHILD
       inherits the creator's environment's field (read off the creator's own NAVIGABLE, which is where an
       environment's copy of it lives until a realm is built) and an AUXILIARY one is its own top-level
       environment. It is NOT the origin of `tlu`: §4.7 over `about:blank` mints a fresh opaque origin, and a
       navigable keyed to one is same origin with nothing — which is exactly how a popup's permission key came
       to disagree with its opener's. */
    const Origin *creator_tlo = window_proxy_top_level_origin(document_window_proxy(ctx));
    const Origin *tlo;
    /* §7.3.2.1's inherited opener policy for the initial about:blank this create makes the navigable with —
       computed below, beside the two §8.1.3.1 fields it is decided the same way as. */
    OpenerPolicyValue inherited_coop;

    DCHECK(creator_container != NULL,
           "§7.3.2.1 \"Creating browsing contexts\" was asked to clone a policy container from a document that "
           "has none — document_install builds "
           "one for every Document, including the initial about:blank, which is why the clone is an ordinary "
           "rule rather than an inheritance rule written for CSP");
    creator_policy = serialized_policy_container_of(creator_container);
    CHECK(creator_tlu_s != NULL, "navigable: this realm's top-level creation URL would not convert");
    tlu = is_child ? creator_tlu_s : "about:blank";
    tlo = is_child ? creator_tlo : origin_agent();
    DCHECK(is_child || iframe_sandbox_flags == 0,
           "§7.4's AUXILIARY navigable was created with an IFRAME SANDBOXING FLAG SET — an auxiliary navigable "
           "has no embedder element for one to come from, and §7.1.5 answers its creation flags from the POPUP "
           "sandboxing flag set instead");
    /* §7.3.1.3: "To create a new child navigable, given an ELEMENT element". A child navigable IS the content
       navigable of a navigable container, so the two halves of `is_child` are one fact and they are asserted
       against each other here rather than trusted separately — a child created without its element would leave
       §7.2.2.4's `frameElement` with nothing to return and §7.1.5's own flag set above with an embedder that
       cannot be named, and an auxiliary one created WITH an element would be presented by a frame it is not in.
       This is the same reason the sandbox assert above it exists, one link along. */
    DCHECK(is_child == JS_IsObject(container),
           "§7.4's create was given a navigable container element for an AUXILIARY navigable, or none for a "
           "CHILD one — §7.3.1.3 takes the element by name for a child and §7.3.1.7 step 8 creates an auxiliary "
           "navigable out of a target name with no element anywhere in the algorithm");
    embedder.iframe_flags = iframe_sandbox_flags;
    embedder.document_flags = document_active_sandbox_flags(ctx);
    creation_flags = sandbox_creation_flags(is_child ? &embedder : NULL,
                                            is_child ? 0 : sandbox_popup_flags(embedder.document_flags));
    if (!child_address(ctx, url, creation_flags, &addr, &origin, &destination_is_javascript)) {
        /* the reference does not parse; the caller decides what that means */
        JS_FreeCString(ctx, creator_tlu_s);
        JS_FreeValue(ctx, creator_tlu);
        free(addr);
        return JS_UNDEFINED;
    }
    /* §7.4.2.2's SCHEME ARM IS UNREACHABLE FROM A CREATE, AND THE REASON IS THE FOLD navigable.h ALREADY NAMES
       AS THE NEXT THING TO REMOVE. §7.3.1.7 "Navigable target names" step 8 creates a top-level traversable
       with NO url — the navigable is created holding §7.3.2.1's initial `about:blank` — and §7.2.2.1 "Opening
       and closing windows" step 15 navigates it afterwards, which is where the `javascript:` branch lives. So
       in the standard `window.open("javascript:x()")` reaches §7.4.2.3.2 through NAVIGATE, in a navigable that
       already has an active document to run the program in and to take its settings object and API base URL
       from. This engine folds step 8 and step 15 into one call that takes a url, so the same spelling arrives
       HERE with no active document yet, and there is no realm for the settings object §7.4.2.3.2's EVALUATE A
       `javascript:` URL takes at its step 4 ("let settings be targetNavigable's active document's relevant
       settings object", whose API base URL its step 5 then reads) to be. The section holds two algorithms —
       navigate-to-a-`javascript:`-URL and evaluate-a-`javascript:`-URL — so the one is named rather than the
       step number left bare.
       UNFOLDING IT IS THE BUILD, not a second evaluate here: make navigable_create take no url, give
       §7.2.2.1's step 15 and §4.6.5's follow-the-hyperlink step 11 the navigate they each already own
       (navigable.h states the asymmetry this leaves in the meantime), and this arm then reaches
       navigable_navigate below like every other destination. */
    DCHECK(!destination_is_javascript,
           "§7.4's create was asked to make a navigable whose ADDRESS is a `javascript:` URL — §7.3.1.7 step 8 "
           "creates a navigable with no url at all and §7.2.2.1 step 15 navigates it, which is the algorithm "
           "that routes the scheme to §7.4.2.3.2 \"The javascript: URL special case\". Nothing is fetched for "
           "one and no Document is created from one, so there is no address here to create a navigable at. "
           "Unfold the create and the navigate (navigable.h names it), so `window.open(\"javascript:…\")` and "
           "`<a href=\"javascript:…\" target=_blank>` reach navigable_navigate's scheme branch");
    /* MINTED FROM THE CREATOR, not from the instance root: this agent holds one realm per same-origin
       document, and naming every child "<root>.<n>" would collide the moment two realms both created one. */
    child = world_mint_doc(document_doc(ctx));
    /* WHO THE NAVIGABLE HANGS OFF, and the two shapes §7.4 distinguishes. A CHILD navigable (§4.8.5's iframe)
       is nested in this one: its `parent` is this Window and it has no opener. An AUXILIARY one (§7.4's popup)
       is a TOP-LEVEL traversable — it is its own parent and its own top — and what links it back here is
       `opener`. Getting this pair backwards is not a detail: `parent`/`top`/`opener` are how a frame and a
       popup tell each other apart, and how testharness.js finds the window it is running in. */
    if (child_in_this_agent(origin)) {
        /* A SECOND REALM IN THIS HEAP — HTML's similar-origin window agent, and the reason it is not a second
           instance: the corpus moves LIVE NODES across this boundary (a subframe created here and appended to
           the child's body, whose `parentNode` is then a node of the child) and assigns LIVE CLOSURES into the
           child's event handlers. Neither is a value that can be named across a transport; they are one object
           graph, and a browser's own model says so. */
        world_doc_adopt(child);   /* this agent holds it */
        /* §7.2.2.4: `parent` and `opener` ARE WindowProxies. They held the creator's GLOBAL, which made the
           `top` walk leave the proxies at the top and read a scriptable property to continue — and a popup's
           opener a Window rather than the navigable it belongs to. The creator's own proxy is what both are. */
        /* §7.4: `noopener` means the new navigable HAS NO OPENER — not that the opener is hidden, that there
           is not one — so the link is never made rather than made and filtered. A CHILD navigable (§4.8.5's
           iframe) has no opener in the first place and no features to ask. */
        /* §7.4 CREATES THE NAVIGABLE WITH THE INITIAL about:blank, and step 14 navigates it afterwards — so
           the navigable's own address is about:blank and the DESTINATION belongs to the navigation, never to
           the creation. Recording the destination here instead was not a shortcut with the same answer: the
           realm is materialized lazily, so a read reaching through this navigable before the load job ran
           would have built the DESTINATION document early, and the job would then have found a materialized
           navigable and loaded the same address a second time into a second document — two fetches, two
           realms, one navigable, and nothing to say so. With about:blank here that early read materializes
           the document §7.4 actually created (no response, no fetch) and the job supersedes it, which is both
           what the spec describes and the only version with one load in it. */
        /* §7.3.2.1's DETERMINE THE ORIGIN FOR THE DOCUMENT THIS NAVIGABLE IS CREATED WITH, WHICH IS THE
           INITIAL about:blank AND NOT THE DESTINATION. Its address is `about:blank` two lines down for
           exactly that reason, and its origin is the matching answer: about:blank with a non-null source
           origin returns the SOURCE origin, which is this agent's. The destination's origin belongs to the
           LOAD and travels with the job (which is why
           the job carries a handle); recording it here would give the initial Document a principal it never
           had — the same "an operation takes its inputs with it" split the address already makes. */
        /* HTML §7.3.2.1's INHERITED OPENER POLICY, the last step of create-a-new-browsing-context-and-
           document's "if creator is non-null" block: "If creator's origin is same origin with creator's
           relevant settings object's TOP-LEVEL ORIGIN, then set document's opener policy to creator's browsing
           context's TOP-LEVEL browsing context's active document's opener policy."
           THE CONDITION NAMES §8.1.3.1's FIELD, not the top navigable's active document's origin — the
           creator's own environment answers it without walking anywhere, and `creator_tlo` above is that
           field. The creator is a document of THIS instance, so its origin is the agent's.
           AND THE CONDITION IS WHAT MAKES THE READ LOCAL: same origin with the top-level origin means the
           top-level Document is same-origin with this one, hence in the same `(browsing context group,
           origin)` agent cluster, hence in this instance. The one shape that is not — a same-origin document
           nested through a cross-origin one — reaches window_proxy_opener_policy's remote crash, which names
           the cross-instance read to build rather than inventing a value.
           `unsafe-none` OTHERWISE is §7.1.3's initial value and a real answer: a creator that is cross-origin
           with its own top inherits nothing, which is why a `same-origin-allow-popups` page's cross-origin
           iframe does not pass that value on to the popups IT opens.
           A CROSS-ORIGIN CHILD IS THE PEER'S TO DECIDE and this arm is not reached for one: the notice below
           carries the creator's policy container and its top-level creation URL, and it does NOT yet carry
           this row, so a peer instance's initial about:blank starts at §7.1.3's initial value. That is a field
           to add to the notice beside the policy, and it is named here rather than guessed at from the peer's
           own response. */
        inherited_coop = OPENER_POLICY_UNSAFE_NONE;
        if (origin_same(origin_agent(), creator_tlo)) {
            JSValue top = window_proxy_top_navigable(ctx, document_window_proxy(ctx));

            DCHECK(window_proxy_is(top),
                   "§7.3.2.1 walked to this document's TOP-LEVEL browsing context and found no navigable — "
                   "every navigable is its own top or has one, and the walk ends at a WindowProxy either way");
            inherited_coop = window_proxy_opener_policy(top);
            JS_FreeValue(ctx, top);
        }
        proxy = window_proxy_new(ctx, child, "about:blank", origin_agent(), name, feat && feat->is_popup,
                                 /* §7.1.5's creation sandboxing flags, decided above. §7.3.2.1 hands
                                    exactly this set to the initial about:blank Document as its ACTIVE
                                    SANDBOXING FLAG SET, and it is kept on the navigable for the same reason
                                    the policy is: that Document's realm is materialized later, possibly by
                                    another document. */
                                 creation_flags,
                                 /* §7.3.2.1's INHERITED OPENER POLICY, decided above. It rides here for the
                                    same reason the flags and the policy do: the initial about:blank Document
                                    this navigable is created with is materialized later, and by then the
                                    creator's top-level Document may have been replaced by a navigation. */
                                 inherited_coop,
                                 /* §7.1.7/§7.3.2.1: a navigable created with no address has no response to
                                    carry a container, so it gets the CLONE OF THE CREATOR'S — the SAME value
                                    this function sends to a PEER instance for a cross-origin child, which is
                                    where the gap was: the remote path carried the creator's container and the
                                    local one did not. Taken now rather than at materialization, because a
                                    srcless child's realm is built later and by whichever same-origin document
                                    reads through it first, which need not be its creator. */
                                 creator_policy,
                                 /* §7.4's `creatorBaseURL`: "if creator is non-null, set creatorBaseURL to
                                    creator's DOCUMENT BASE URL" — this realm is the creator, and the initial
                                    about:blank Document this navigable is created with takes it as its ABOUT
                                    BASE URL. Taken now rather than at materialization, for the reason the
                                    policy container is: a srcless child's realm is built later and by
                                    whichever same-origin document reads through it first, whose base URL is
                                    not the creator's. It is the creator's BASE URL and not its ADDRESS, so a
                                    creator carrying `<base href>` passes the base on — which is what makes
                                    `open()` from such a page resolve its child's relative URLs the way a
                                    browser does. */
                                 document_base_url(ctx),
                                 /* HTML §8.1.3.1's TOP-LEVEL CREATION URL, and §7.4 states it from both ends.
                                    A CHILD navigable is NESTED, so its documents' environments inherit their
                                    creator's — this document's, read off its own realm — and an `about:blank`
                                    iframe of an `http` page is therefore not a secure context even though
                                    §3.2 hands a TOP-LEVEL `about:blank` a free pass. An AUXILIARY navigable
                                    IS a top-level traversable, so it is its own top and its environment's
                                    top-level creation URL is the address it was created at. Taken at creation
                                    like the policy container, and for the same reason: the realm is built
                                    later and by whichever same-origin document reads through it first. */
                                 tlu,
                                 /* AND §8.1.3.1's TOP-LEVEL ORIGIN, decided above from the same §7.3.2.1
                                    sentence. For an AUXILIARY navigable it is the origin of the initial
                                    about:blank Document this call is minting — the same `origin_agent()` this
                                    call passes as that Document's origin — which is what makes a popup's
                                    permission key its opener's rather than a fresh opaque one derived from
                                    the `about:blank` its top-level creation URL is. */
                                 tlo,
                                 is_child ? document_window_proxy(ctx) : JS_UNDEFINED,
                                 (is_child || (feat && feat->noopener)) ? JS_NULL
                                                                        : document_window_proxy(ctx));
        CHECK(!JS_IsException(proxy), "a navigable's WindowProxy could not be allocated");
        /* §7.4's DECISION IS A FACT ABOUT THE NAVIGABLE, so it is read back where it was written. A popup that
           does not know it is one answers `true` from all six of §7.2.2.5's BarProps, which is precisely how
           the corpus tells a popup from a tab — and a value written and then not there is worth catching at
           the write rather than 51 subtests downstream in another realm. */
        DCHECK(window_proxy_is_popup(proxy) == (feat != NULL && feat->is_popup),
               "a navigable did not keep §7.4's popup decision across its own creation");
        /* §7.1's search reaches a CHILD down its parent's document tree; a top-level traversable is reachable
           from nothing, so the group holds it — see navigable_choose_name. */
        if (!is_child) navigable_group_add(ctx, proxy);
        /* §7.4 STEP 14: NAVIGATE THE NEW NAVIGABLE TO url — and navigating is what makes the document RUN. It
           is a JOB rather than a call, for the reason js_nav_load_step states, but it is ENQUEUED here and
           unconditionally: a navigable's own scripts are an observable that owes nothing to the creator, and a
           popup posts back to its opener without the opener ever touching the proxy. Not scheduling this at
           all was twelve files in html/browsers reporting nothing — not a saving, a popup that never ran.
           A navigable with NO address enqueues NOTHING, and that is the same sentence read the other way: it
           holds the initial about:blank Document §7.4 created it with, that Document has no scripts by
           construction, and the ONLY way to observe it is a read through this proxy — which builds it then.
           The deferral has no observable there, and it is load-bearing: a forced-execution frontier runs this
           `open()` once per flow, so materializing every never-touched about:blank exhausted the heap at ~2030
           flows with the initial parse failing to allocate. The line between the two is what a navigable DOES,
           not what it costs.
           THE TEST IS "IS THERE ANYTHING TO FETCH", which for §7.4 is the `about:` scheme: about:blank has no
           response and no content, so navigating to it produces the Document the navigable already has. It is
           the same test the RealmBuilder applies to decide whether to fetch, and it is stated in both places
           because it is one spec fact about the scheme rather than a protocol between them. */
        if (strncmp(addr, "about:", 6) != 0)
            /* §7.1.7: the CREATOR's container, for §7.4's create — every item of it, as one value. */
            /* NO ABOUT BASE URL: the `if` above admits only a destination that is not an `about:` URL, so
               §7.4.5's aboutBaseURL stays null — §2.4.3's answer for a Document that comes from a response. */
            /* AND THE PROVENANCE THE CROSS-ORIGIN ARM PUTS ON ITS NOTICE, taken from the same local rather
               than recomputed: one operation, one answer. */
            navigable_load_enqueue(ctx, proxy, addr, origin, creator_policy, NULL, provenance);
    } else {
        /* THE NOTICE, and every field of it is load-bearing. The CHILD is the name the host provisions an
           instance under; the CREATOR names who made it, which is what the host routes replies through and what
           a browser would decide policy from; the URL is the child's initial address; the ORIGIN is the
           child's; the PARENT is §7.3.1.3's link, without which the peer's own navigable is a top-level
           traversable by that section's definition; the CONTAINER POLICY is what that section's OTHER link
           answered — Permissions Policy §9.5 run over the element this document presents the child with, whose
           result the peer cannot compute and must not guess; the SELF-ORIGIN and the POLICY are the two halves of
           HTML §7.3.2.1's CLONE OF THE CREATOR'S policy container ("Creating browsing contexts": "Set
           document's policy container to a clone of creator's policy container"), serialized — which the
           container can do precisely because it is a flat parse over one owned string plus the origin beside
           it, so the clone that crosses an instance and the clone that crosses a session are the same
           operation. */
        /* AND §8.1.3.1's TOP-LEVEL CREATION URL, which crosses for exactly the reason the policy container
           does: the child's ENVIRONMENT is decided by the operation that created it, the instance that will
           host the child cannot derive it, and a peer that answered from the child's own address would report
           an https document nested in an http page as a secure context. IT GOES BEFORE THE POLICY, because
           the policy is the RECORD'S REMAINDER: a raw CSP header may contain HTAB (this engine's own parser
           treats tab as source-list whitespace), so every reader of this record stops splitting at the policy
           and keeps the rest verbatim. A field after it would be swallowed by it. A URL cannot contain a tab —
           url_serialize percent-encodes one — so it is safe in a split field. */
        /* THE ORIGIN CROSSES AS ITS SERIALIZATION, and that is not a lapse — a record crosses an instance no
           more than a JSValue does, and what the peer needs is its own principal, which its host will state
           back. A peer's opaque origin is same origin with nothing here, which is exactly right: two Documents
           sharing ONE opaque origin share an agent cluster, so they are never on two sides of this line. */
        /* THE CLONE CROSSES AS A WHOLE CSP LIST, WHICH IS TWO FIELDS AND NOT ONE. CSP §2.2 "Policies" makes a
           CSP list "a struct consisting of policies (a list of policies) and a self-origin (an origin which is
           used when matching the 'self' keyword)", and the bytes of a serialized policy contain only the first
           half — §2.2.2 "Parse response's Content Security Policies" states the second from OUTSIDE them
           ("self-origin is response's URL's origin"). So a peer handed the text alone had exactly one place
           left to get it: its own address. That is the wrong origin by construction for an INHERITED list, and
           CSP §2.2's own note says so — the self-origin exists "to facilitate the 'self' checks of local scheme
           documents/workers that have INHERITED their policy but have an opaque origin". §6.7.2.8 "Does url
           match expression in origin with redirect count?" is the sole reader, and it answers `'self'` by
           comparing the request's URL against THAT origin, so a creator's `script-src 'self'` read against the
           child's address permits the child's origin and refuses the creator's — a sink this engine would
           report as CSP-blocked when it fires, or as live when it does not.
           IT SITS BEFORE THE POLICY for the reason §8.1.3.1's URL does: the policy is the record's REMAINDER.
           An origin's serialization cannot contain a tab (it is scheme, "://", host and an optional port, and
           the opaque one is the three bytes `null`), so it is safe in a split field.
           IT IS ALWAYS PRESENT, EVEN WHEN THE POLICY IS EMPTY, AND THAT IS WHAT SAYS THERE IS A CONTAINER AT
           ALL. §2.2 gives every CSP list a self-origin whether or not it holds policies, so a creator that
           sends no CSP still states one here — and the receiving side reads the PRESENCE of this field, not of
           the policy, to decide whether §7.1.7's clone happened, because a Document merges CSP §3.3's `<meta>`
           policies into that same list under that same self-origin. An empty policy with a self-origin beside
           it and an empty pair are two different facts and the record can say both.
           HTML §7.1.7's OWN "clone a policy container" DOES NOT RESTATE IT — its steps are "let clone be a new
           policy container" and "for each policy in policyContainer's CSP list, append a copy of policy into
           clone's CSP list", which move the POLICIES and are silent about the list's other half. That is an
           editorial hole rather than a licence to re-derive one at the far end: CSP §2.2's note states the
           intent directly, and this notice carries what the note requires. */
        DCHECK(serialized_policy_container_exists(creator_policy),
               "§7.3.2.1's clone of the creator's policy container was about to cross to a peer instance with "
               "no CSP §2.2 SELF-ORIGIN — every container this engine builds has one (policy_container_new "
               "requires it), so an absent one is a creator whose container was built somewhere that did not "
               "state it, and the peer would resolve `'self'` against its own address instead");
        /* AND §7.1.5's CREATION SANDBOXING FLAG SET, WHICH IS NOT AN ITEM OF THE CONTAINER BESIDE IT — §7.1.7
           gives a policy container a CSP list, an embedder policy, a referrer policy and two integrity
           policies, and no sandboxing flag set is among them. It is a SEPARATE statement about the same
           navigable, in the same class as the parent, the container answer and the ancestor list: computed
           HERE because both of §7.1.5's inputs are here — "if embedder is an element, then the flags set on
           embedder's iframe sandboxing flag set" and "…on embedder's node document's active sandboxing flag
           set" — and neither the `<iframe sandbox>` element nor this document's own set can cross.
           IT IS THE PEER'S §7.4.2.1 TARGET SNAPSHOT PARAMS, NOT A CONVENIENCE. "To snapshot target snapshot
           params given a navigable targetNavigable, return a new target snapshot params with: sandboxing flags
           — the result of determining the creation sandboxing flags given targetNavigable's active browsing
           context and targetNavigable's container", and §7.4.2.2 makes finalSandboxFlags "the union of
           targetSnapshotParams's sandboxing flags and policyContainer's CSP list's CSP-derived sandboxing
           flags". The peer computes the SECOND half from the response it loads; the first is this value and it
           can come from nowhere else, so a peer without it takes the empty set and runs the scripts, the form
           submissions and the `document.domain` relaxation the `sandbox` attribute forbids.
           A SANDBOXED CHILD IS ALWAYS ON THIS SIDE OF THE LINE, which is why this field is reachable at all
           rather than a case that never happens: the SANDBOXED ORIGIN flag makes the child's origin OPAQUE
           (child_address runs §7.3.2.1's determine-the-origin with it), an opaque origin is same origin with
           nothing, so §7.1.1 puts every such child in another agent cluster and child_in_this_agent sent it
           here. `sandbox=allow-same-origin` keeps the origin and can still leave fifteen other flags set, so
           the two arms of that keyword are the two ways this field is non-empty rather than one.
           IT CROSSES AS §7.1.5's OWN FLAG NAMES, COMMA-SEPARATED, and the separator is the one thing here a
           reader should not take on trust: those names CONTAIN SPACES, so the SPACE that joins §3.1.3's
           ancestor origins and Permissions Policy §9.5's answer beside it cannot delimit this one — a
           space-joined set does not parse. sandboxing.h states the whole rule and asserts it per name. */
        sandbox_flags = sandbox_flags_serialize(creation_flags);
        CHECK(sandbox_flags != NULL,
              "navigable: OOM stating §7.1.5's creation sandboxing flag set for a peer instance");
        /* NO TAB ASSERT BESIDE THE OTHERS, AND THAT IS NOT AN OMISSION: sandbox_flags_serialize asserts the
           absence of a tab (and of its own separator) per FLAG NAME, at the one place that can see the
           vocabulary, so a check here would be that invariant restated one caller along. */
        /* §7.1.4'S ITEM CROSSES THE NOTICE TOO, AND ITS FOUR FIELDS SIT WITH THE SELF-ORIGIN FOR THE SAME
           REASON: the policy is the record's REMAINDER, so everything that is not the policy comes before it.
           A `report-to` ENDPOINT IS SAFE IN A SPLIT FIELD and that is the standard's own guarantee rather than
           an assumption about what servers send: §7.1.4.1 makes the parameter a structured-field STRING, and
           RFC 8941 §3.3.3 "Strings" defines one as "zero or more printable ASCII characters (i.e., the range
           %x20 to %x7E)" and notes in as many words that "this excludes tabs, newlines, carriage returns".
           The VALUES cross as §7.1.4's three strings rather than as this enum's integers, so the peer that
           reads them is reading the standard's vocabulary and not this build's declaration order.
           IT IS THE CREATOR'S ITEM AND NOT THIS ENGINE'S RESPONSE'S: §7.1.7's clone moves the whole container
           and §7.3.2.1 clones the CREATOR's, so a peer that obtained one from the CHILD's response would be
           answering the wrong document's question — which is the same substitution the self-origin above
           exists to prevent, one item along. */
        DCHECK(!strchr(creator_policy.embedder.endpoint, '\t') &&
               !strchr(creator_policy.embedder.report_only_endpoint, '\t'),
               "§7.1.4's `report-to` reporting endpoint contains a TAB and this record is tab-delimited — RFC "
               "8941 §3.3.3 \"Strings\" excludes tabs from a structured-field string, so a byte that reached "
               "here is one the structured-field parser admitted and should not have; it would split this "
               "notice into more fields than it has and shift the creator's policy text onto the wrong one");
        /* HTML §7.3.1.3 "Child navigables" — THE ONE FACT THE PEER COULD NEVER DERIVE ABOUT ITS OWN NAVIGABLE,
           and the reason this field is the PARENT rather than a flag. The section defines the term over the
           link and not over a property of the navigable: "A navigable navigable is a child navigable of another
           navigable potentialParent when navigable's parent is potentialParent. We can also just say that a
           navigable 'is a child navigable', which means that its PARENT IS NON-NULL." A peer instance mints its
           root proxy for a navigable nothing there created, so its parent slot was JS_UNDEFINED — and a
           navigable whose parent is null IS a top-level traversable, everywhere, by that definition. That is
           not one wrong answer: §7.1.4.2 "Embedder policy checks" step 1 returns true for it, §7.2.2.4's
           `parent` and `top` answer the frame itself, §7.5.9/§7.5.10's subtree walks find nothing above it, and
           every one of those is a plausible answer about a top-level page rather than a crash.
           IT IS NOT THE CREATOR FIELD SAID TWICE, AND THE TWO MUST NOT BE COLLAPSED. The creator is a ROUTING
           fact — which instance made this, so a reply reaches it — and it is present for an AUXILIARY navigable,
           which has no parent at all; the parent is §7.3.1.3's TREE LINK and carries a whole identity (origin,
           name, parent, opener) rather than a name. They name one document for one shape of navigable, which is
           the same coincidence §7.1.4.2's parentPolicy rests on, and a reader who deleted either would be
           deleting the case where they differ.
           IT CROSSES AS THE NAVIGABLE'S IDENTITY, in the one grammar a navigable already crosses an instance
           boundary in (core/frame/remote_object.h's 'W' record), because a NAME is not enough to MINT one: the
           peer holds no proxy for this navigable, and remote_object.c states in as many words that minting
           needs the parent's own origin, name, parent and opener. The five base64 fields are '.'-terminated and
           the tag is one letter, so nothing in it can contain a tab.
           `u` — the grammar's own undefined — IS THE POSITIVE STATEMENT THAT THERE IS NO PARENT, and it is what
           an AUXILIARY navigable sends: §7.3.1.7 step 8 creates one out of a target name with no element
           anywhere in the algorithm, so it is its own top and what links it back here is `opener`. An empty
           field would be a hole a reader could default; a value in the encoding's own vocabulary is a fact.
           THE PARENT IS THIS DOCUMENT because §7.3.1.3's create-a-new-child-navigable runs in the embedder's
           realm — "let parentNavigable be element's node navigable" — which is the same proxy this call hands
           the child two lines below as its parent on the same-agent arm. */
        /* AND PERMISSIONS POLICY §9.5's ANSWER FOR THIS NAVIGABLE, WHICH IS THE OTHER FACT §7.3.1.3's CONTAINER
           HOLDS AND THE ONE THE PEER CANNOT GET AT ALL. §9.5 is "given null or an element (container) and an
           origin (origin)", and BOTH of its arguments are HERE: the element is `container`, in this document's
           tree, and the origin is the child's — the one this same create computed and puts on the record two
           fields along. The peer holds neither. So the algorithm runs ONCE, in the heap that has its
           arguments, and its RESULT crosses.
           IT IS THE RESULT AND NOT THE INPUTS, and the difference decides how much has to cross and how many
           places evaluate one algorithm. §9.7's steps 2 and 3 run §9.8 over the CONTAINER DOCUMENT's own
           permissions policy at two different origins, step 5 matches §9.4's container policy against the
           child's origin, and step 7 compares the two origins — so "the inputs" is that document's whole
           policy (inherited AND declared), its origin, and the `allow` attribute's bytes, and a peer given
           them would be a SECOND site running §9.7.
           WITHOUT IT THE PEER TOOK §9.7 STEP 1 — "if container is null, return `Enabled`" — for a navigable
           that HAS a container, which grants a cross-origin child every supported feature its embedder was
           never asked about, `cross-origin-isolated` included. It crashes there now instead, and this is what
           it crashes for the want of.
           A `null` CONTAINER IS SENT AS A WORD, not as an empty field: §7.3.1.7 step 8's AUXILIARY navigable
           has no element anywhere in its algorithm, which is a FACT about it, and the peer reads the two apart
           through permissions_policy_serialized_has_container. `is_child` is that condition — this function
           asserts the pairing with `container` at its head — and it is the same condition the parent field
           above splits on, which is not a duplication: they are two links of §7.3.1.3 that a child navigable
           has both of and an auxiliary one has neither of, and a peer that was told only one would have to
           infer the other. */
        /* AND HTML §3.1.3 "Ancestor origins"' INTERNAL ANCESTOR ORIGIN OBJECTS LIST FOR THIS CHILD, WHICH IS
           THE THIRD FACT §7.3.1.3's CREATE HOLDS AND THE PEER CANNOT REACH. §3.1.3's steps read the parent
           Document's own recorded list (step 5), that Document's ORIGIN RECORD (steps 9 and 10) and the
           CONTAINER ELEMENT (step 6). All three are in THIS heap and none of them crosses: a Document's
           ancestry is not carried by the navigable identity beside it, an element does not cross at all, and
           an origin RECORD is exactly what a serialization drops — §7.1.1 decides an opaque origin by
           identity and every opaque origin is the three bytes `null`.
           THAT LAST ONE IS WHY THE RESULT CROSSES AND NOT THE INPUTS, and it is not a corner: the commonest
           way to reach this arm at all is a `data:` iframe, whose child origin is a NEW OPAQUE ORIGIN and is
           therefore same origin with nothing — which is what routed it here. A peer re-running step 10 over
           serialized text would mask an ancestor that is not the parent's in precisely that case.
           §3.1.3's step 3 IS THE OTHER ARM AND IT IS A FACT, NOT AN ABSENCE: "let parentDoc be document's
           container document; if parentDoc is null, then return output". §7.3.1.7 step 8's AUXILIARY navigable
           has no element anywhere in its algorithm, so it has no container document and its list is EMPTY for
           ever. The record says so in that grammar's own word rather than by carrying nothing, and the peer
           reads the two apart through document_ancestor_origins_serialized_has_ancestors — the same split
           `is_child` makes for the container and the parent, because it is the same one link of §7.3.1.3. */
        if (is_child) {
            PermissionsPolicy *child_policy = document_permissions_policy_for_container(container, origin);

            container_policy = permissions_policy_serialize(child_policy);
            permissions_policy_free(child_policy);
            ancestor_origins = document_ancestor_origins_for_child(ctx, container, origin);
        } else {
            container_policy = strdup(PERMISSIONS_POLICY_SERIALIZED_NO_CONTAINER);
            ancestor_origins = strdup(DOCUMENT_ANCESTOR_ORIGINS_SERIALIZED_NONE);
        }
        CHECK(container_policy != NULL,
              "navigable: OOM stating §9.5's answer for this navigable's container to a peer instance");
        CHECK(ancestor_origins != NULL,
              "navigable: OOM stating §3.1.3's ancestor origins for this navigable to a peer instance");
        parent_id = remote_object_encode(ctx, is_child ? document_window_proxy(ctx) : JS_UNDEFINED);
        CHECK(parent_id != NULL, "navigable: OOM naming this navigable's §7.3.1.3 parent for a peer instance");
        /* THE RECORD, FIELD BY FIELD, AND ITS SIZE IS A FUNCTION OF THIS ARRAY. engine_notice_build walks it
           twice — once to size, once to write — so the TAB in front of every field is paid for by the same
           edit that adds the field, and `sizeof fields / sizeof fields[0]` is the count nobody restates.
           IT WAS A FORMAT STRING BESIDE A HAND-SUMMED LENGTH PLUS A SLACK CONSTANT, and the constant paid for
           the operation name, the NUL and ONE TAB PER FIELD. Adding the provenance field below moved the sum
           and left the constant, so the record was assembled one byte short and snprintf truncated it —
           silently, because truncating is what snprintf is for. The byte it dropped was the record's last, the
           last field is the policy, and the policy was EMPTY in the fixture that caught it: the final TAB
           went, and seventeen fields reached every reader as sixteen. With a non-empty policy that same byte
           comes off the END OF THE POLICY TEXT, which nothing counts and no reader can see.
           NO FIELD BEFORE THE LAST MAY CONTAIN A TAB, and that is asserted for ALL of them in
           engine_notice_build rather than for the three that were once spelled out here. The reasons are
           per-field and they are this record's grammar: an origin's serialization cannot hold one (URL §3.2
           "Host miscellaneous" makes TAB a forbidden host code point), nor can HTML §7.1.4's three fixed
           tokens, nor RFC 8941 §3.3.3 "Strings"' `report-to` endpoints, nor remote_object.c's one-letter tag
           over '.'-terminated base64, nor Permissions Policy §4.1's feature tokens and §4.2's
           `Enabled`/`Disabled`, nor HTML §3.1.3's ancestor list (origin serializations joined by SPACE, and
           §3.2 forbids SPACE in a host too), nor §7.1.5's flag names joined by COMMA. A tab in any of them is
           that grammar having changed under a record that splits on one, and the creator's policy text would
           land on the wrong field.
           AND THE FIFTEENTH FIELD IS WHAT THIS NAVIGATION IS EVIDENCE OF — CLAUDE.md
           §A-REQUEST-CARRIES-THE-PROVENANCE's `observed`/`derived`/`forced`, the same word the SAME-ORIGIN arm
           puts on its load job, taken from the same local.
           IT IS THE ONE FIELD THE HOST'S DECISION TO SPEND THE NETWORK IS MADE FROM, and until it existed the
           host had nothing: §Attacker-sources puts the WHOLE of a navigation's safety in the choice of address
           ("a navigation whose provenance is not established CRASHES at the decision rather than proceeding"),
           and a record that named the address and said nothing about who named it left one host declining
           every child navigable it was ever told about and the other firing every one of them CREDENTIALED.
           IT SITS BEFORE THE POLICY FOR THE REASON EVERYTHING ELSE DOES: the policy is the record's remainder
           because a raw CSP header may contain HTAB, and this vocabulary is three words of ASCII lowercase
           letters (solver/engine.h), so it can never be the field that cannot be split. */
        const char *const fields[] = {
            world_doc_name(child),
            world_doc_name(document_doc(ctx)),
            addr,
            origin_serialized(origin),
            tlu,
            creator_policy.self_origin,
            embedder_policy_value_token(creator_policy.embedder.value),
            creator_policy.embedder.endpoint,
            embedder_policy_value_token(creator_policy.embedder.report_only_value),
            creator_policy.embedder.report_only_endpoint,
            parent_id,
            container_policy,
            ancestor_origins,
            sandbox_flags,
            provenance,
            creator_policy.csp ? creator_policy.csp : "",
        };

        op = engine_notice_build("navigable.create", fields, sizeof fields / sizeof fields[0]);
        engine_host_notify(ctx, op);
        free(op);
        free(parent_id);
        free(container_policy);
        free(ancestor_origins);
        free(sandbox_flags);
        /* THROUGH THE ONE DOOR, so this navigable is the one a peer's answer resolves to. The child was just
           minted and nothing has named it yet, so this ask is the mint — but it is the ask that RECORDS it,
           and without the record `w.frames[0] === iframe.contentWindow` is false about the frame this line
           created the moment the peer answers an indexed read with the same navigable's identity. */
        proxy = window_proxy_for_document(ctx, child, origin, name,
                                          is_child ? document_window_proxy(ctx) : JS_UNDEFINED,
                                          (is_child || (feat && feat->noopener)) ? JS_NULL
                                                                                 : document_window_proxy(ctx));
        CHECK(!JS_IsException(proxy), "a navigable's WindowProxy could not be allocated");
    }
    /* §7.3.1.3's create-a-new-child-navigable: "Set element's content navigable to navigable." The ELEMENT's
       half of that link is the wrapper slot core/html/html_iframe.c writes when this returns; this is the
       NAVIGABLE's half, and it is written here — in the algorithm that was handed the element — for both arms,
       because a CROSS-ORIGIN child is presented by an element in THIS document exactly as a same-origin one is.
       Whose active document lives where is a fact about the child; which element presents it is a fact about
       this document, and the two are unrelated.
       AFTER THE MINT AND BEFORE ANYTHING CAN OBSERVE IT: the only code between the mint and this line is the
       group append and the load ENQUEUE, neither of which runs a program or reads through the navigable, so
       there is no moment at which a page sees a child navigable with no container. */
    if (is_child) window_proxy_set_container(ctx, proxy, container);
    JS_FreeCString(ctx, creator_tlu_s);
    JS_FreeValue(ctx, creator_tlu);
    free(addr);
    return proxy;
}

/* THE OTHER HALF OF §7.4's CREATE — the navigable an instance is ROOTED in, which the create above did not
 * make. See core/frame/navigable.h for why the parent is a parameter, why it crosses as an identity, and why
 * §7.1.4.2's step 2 is answerable here from the container the record carried. */
JSValue navigable_root(JSContext *ctx, uint32_t doc, const char *name, OpenerPolicyValue opener_policy,
                       const char *parent_navigable, SerializedPolicyContainer inherited,
                       const EmbedderPolicy *response_embedder, SandboxFlags creation_sandbox_flags)
{
    JSValue parent, proxy;

    DCHECK(parent_navigable != NULL && *parent_navigable,
           "a navigable was rooted with no §7.3.1.3 PARENT field at all — a navigable either has a parent or is "
           "a top-level traversable, both are facts, and core/frame/remote_object.h spells the second `u`. An "
           "empty field is a host that stopped stating it, and the document would silently become a top-level "
           "page in the only instance that holds it");
    DCHECK(response_embedder != NULL,
           "a navigable was rooted with no §7.1.4 EMBEDDER POLICY for its own response — §7.1.7's create-a-"
           "policy-container-from-a-fetch-response obtains one for every response and §7.1.4's own initial value "
           "answers for a document that has none, so an absent one is a host that did not run the obtain rather "
           "than a response that carried nothing");
    parent = remote_object_decode(ctx, parent_navigable);
    if (window_proxy_is(parent)) {
        /* §7.1.4.2 steps 2-6. The container is asserted to EXIST rather than read through a "none", whose
           embedder policy answers `unsafe-none` — a value that makes step 4 return true, so a lost container
           would turn this check off with nothing to say so. */
        bool adheres;

        DCHECK(serialized_policy_container_exists(inherited),
               "a CHILD navigable was rooted with no §7.1.7 POLICY CONTAINER to inherit — §7.3.1.3's create "
               "clones its creator's onto the record that provisions this instance and asserts it is there, so "
               "a child that arrives without one lost the clone in transit. §7.1.4.2 step 2 would then read "
               "`unsafe-none` for a container document that may have opted into cross-origin isolation, and the "
               "check would pass for every response");
        adheres = embedder_policy_check_navigation_response(inherited.embedder,
                                                            serialized_embedder_policy_of(response_embedder));
        /* §7.1.4.2's steps QUEUE before they answer, so the call stands outside the assert and only its ANSWER
           is read below: a DCHECK's condition is compiled out in release, and a check performed inside one is a
           check the shipped build never runs. */
        DCHECK(adheres,
               "HTML §7.4.5 \"Populating a session history entry\" BLOCKS this document — §7.1.4.2's check a "
               "navigation response's adherence to its embedder policy returned false, because the CONTAINER "
               "DOCUMENT of this navigable (which lives in another instance) enforces a "
               "`Cross-Origin-Embedder-Policy` compatible with cross-origin isolation and this response opted "
               "into none. The navigable must get §7.5.7 \"Loading a document for inline content that doesn't "
               "have a DOM\"'s Document instead of the response's, marked unsalvageable with "
               "\"navigation-failure\" — the same error Document js_nav_load_step above still owes for a child "
               "navigable of this instance, and it is one value both sites take rather than two");
    }
    /* §7.1.5's SET IS PAIRED WITH §7.3.1.3's PARENT AND THE PAIRING IS ASSERTED, exactly as this component
       already asserts the parent against the container: determine-the-creation-sandboxing-flags reads an
       EMBEDDER ELEMENT for a nested navigable and a POPUP SANDBOXING FLAG SET for a top-level one, and a
       top-level traversable's popup set is filled by nothing but §7.3.1.7's rules for choosing a navigable —
       which cannot have run for the navigable an INSTANCE was rooted in, because no `window.open` in any heap
       reaches a document the trusted zone provisioned. So flags with no parent is a host describing a nested
       navigable's sandbox over a navigable it also called top-level, and the two fields would then disagree
       about what kind of navigable this is. */
    DCHECK(creation_sandbox_flags == 0 || window_proxy_is(parent),
           "a navigable was rooted as a TOP-LEVEL TRAVERSABLE (§7.3.1.3 parent `u`) and given a non-empty "
           "§7.1.5 CREATION SANDBOXING FLAG SET — that section fills a top-level browsing context's set from "
           "its POPUP sandboxing flag set alone, which only §7.3.1.7's rules for choosing a navigable ever "
           "populate and which nothing can have populated for a navigable a host provisioned. The two fields "
           "come off one `navigable.create` record, so this is a host that read them from different rows");
    proxy = window_proxy_new_self(ctx, doc, name, opener_policy, parent, creation_sandbox_flags);
    JS_FreeValue(ctx, parent);
    return proxy;
}

/* THE OTHER LINK OF §7.3.1.3 FOR THAT SAME NAVIGABLE — its CONTAINER, which for the navigable an instance is
 * rooted in is an element in the CREATING instance's tree. See core/frame/navigable.h.
 *
 * THE TWO LINKS ARE ASSERTED AGAINST EACH OTHER HERE, and that is the whole reason this is a function of this
 * component rather than a straight write into the proxy. §7.3.1.3 defines a CHILD NAVIGABLE as one whose
 * parent is non-null, and §7.3.1.3's create is the only algorithm that makes one — it is handed an element and
 * links both. So "has a remote parent" and "has a container" are one fact said twice for a navigable rooted in
 * a peer, and a record where they disagree describes two different navigables. Neither direction is harmless:
 * a container with no parent is a top-level traversable presented by an element, and a remote parent with no
 * container is the §9.7 step 1 that grants a cross-origin child every feature its embedder holds. */
void navigable_root_container(JSContext *ctx, JSValueConst proxy, const char *container_policy)
{
    JSValue parent = window_proxy_parent_navigable(ctx, proxy);
    bool remote_parent = window_proxy_is(parent) && window_proxy_is_remote(parent);

    JS_FreeValue(ctx, parent);
    DCHECK(container_policy != NULL && *container_policy != '\0',
           "a navigable was rooted and then told NOTHING about its §7.3.1.3 CONTAINER — Permissions Policy "
           "§9.5's answer and \"there is no container\" are the two things this statement can make, and an "
           "empty field is a host that stopped making it rather than a navigable with no container");
    DCHECK(permissions_policy_serialized_has_container(container_policy) == remote_parent,
           "§7.3.1.3's two links contradict each other for the navigable this instance is rooted in — the "
           "section makes a CHILD NAVIGABLE one whose parent is non-null and its create links a container "
           "element in the same steps, so a navigable with a container in a peer instance has a parent there "
           "too and one without has neither. A container with no parent is a top-level traversable presented "
           "by an element; a remote parent with no container is §9.7 step 1 for a frame, which returns "
           "`Enabled` for every supported feature");
    /* "THERE IS NO CONTAINER" IS RECORDED BY RECORDING NOTHING, and that is not a hole a reader could fill:
       the navigable's own container slot is already JS_NULL, which is §7.3.1.3's answer and what §9.7 step 1
       reads. The branch is on what this statement SAYS rather than on which host made it, so there is no
       shape of caller it silently serves differently. */
    if (permissions_policy_serialized_has_container(container_policy))
        window_proxy_set_remote_container(proxy, container_policy);
}

/* AND HTML §3.1.3 "Ancestor origins"' INTERNAL ANCESTOR ORIGIN OBJECTS LIST FOR THAT SAME NAVIGABLE, composed
 * by the instance that holds its ancestors. See core/frame/navigable.h.
 *
 * THE PAIRING WITH §7.3.1.3's PARENT IS ASSERTED HERE FOR THE REASON THE CONTAINER'S IS, and it is the same
 * one link read through a different algorithm. §3.1.3's steps 2-3 return an EMPTY output exactly when there is
 * no container document, and §7.3.1.3 makes a navigable with a non-null parent a CHILD NAVIGABLE, which has
 * one. So "the parent is remote" and "the list is non-empty" are one fact said twice for a navigable rooted in
 * a peer, and a record where they disagree describes two navigables. Neither direction is harmless: ancestors
 * with no parent is a top-level traversable that reports a tree above it, and a remote parent with an empty
 * list is a cross-origin frame telling every `location.ancestorOrigins` read that it is the top of its own —
 * which is the answer an absent field would also give, and the whole reason this field exists.
 *
 * IT IS A SECOND STATEMENT RATHER THAN AN ARGUMENT TO THE ROOTING, on §7.3.1.3's own order: the section makes
 * the navigable and links it afterwards, and §7.3.2.1 runs §3.1.3's steps later still — when the DOCUMENT is
 * created, which is what they take. A host that skips it does not get an empty list; the install CRASHES,
 * naming this call. */
void navigable_root_ancestor_origins(JSContext *ctx, JSValueConst proxy, const char *ancestor_origins)
{
    JSValue parent = window_proxy_parent_navigable(ctx, proxy);
    bool remote_parent = window_proxy_is(parent) && window_proxy_is_remote(parent);

    JS_FreeValue(ctx, parent);
    DCHECK(ancestor_origins != NULL && *ancestor_origins != '\0',
           "a navigable was rooted and then told NOTHING about §3.1.3's ANCESTOR ORIGINS of its Document — the "
           "composed list and \"there are no ancestors\" are the two things this statement can make, and an "
           "empty field is a host that stopped making it rather than a document at the top of its tree");
    DCHECK(document_ancestor_origins_serialized_has_ancestors(ancestor_origins) == remote_parent,
           "§7.3.1.3's parent link and §3.1.3's ancestor list contradict each other for the navigable this "
           "instance is rooted in — the section makes a CHILD NAVIGABLE one whose parent is non-null, and "
           "§3.1.3's steps 2-3 return an empty list only for a Document with no container document, so a "
           "navigable whose parent is in a peer has ancestors there and one with no parent has none. "
           "Ancestors with no parent is a top-level traversable reporting a tree above it; a remote parent "
           "with no ancestors is a cross-origin frame reporting itself as that top");
    /* "THERE ARE NO ANCESTORS" IS RECORDED BY RECORDING NOTHING, and that is not a hole a reader could fill:
       core/dom/document.c reaches this slot only on the remote-parent arm, and a navigable with no parent
       takes §3.1.3's step 3 there without ever asking. The branch is on what this statement SAYS rather than
       on which host made it, so there is no shape of caller it silently serves differently. */
    if (document_ancestor_origins_serialized_has_ancestors(ancestor_origins))
        window_proxy_set_remote_ancestor_origins(proxy, ancestor_origins);
}

/* HTML §7.3.1.7 "Navigable target names" — THE RULES FOR CHOOSING A NAVIGABLE, given `target`, this document's
 * node navigable and `noopener` — followed by the navigation the two callers both want. It is its own function
 * because it has TWO callers and they are not variants of each other: §7.2.2.1's window open steps reach it
 * after parsing a features string, and §4.6.3's FOLLOWING A HYPERLINK reaches it from an `<a>`'s activation
 * behaviour with `noopener` read off `rel` instead. The rules are ONE algorithm; a second copy in the hyperlink
 * path would be the second answer that is always subtly wrong.
 * `feat` carries what only §7.2.2.1 supplies (the popup decision) and what both supply (`noopener`).
 *
 * THE STEP ORDER IS THE ALGORITHM, AND IT WAS INVERTED HERE. This function opened by testing `noopener` and
 * skipping the whole choice when it was set — "a request that must not be able to script its opener cannot be
 * answered with a navigable the opener already holds, so it always CREATES", which sounds like a reason and is
 * not the spec's. `noopener` guards STEP 7 alone, the search by target name; steps 4-6 run before it and are
 * unconditional. So `open(url, "_self", "noopener")` must navigate THIS navigable — a page cannot be given a
 * handle it already has, so there is nothing for the flag to withhold — and it was opening a new window
 * instead, silently turning a same-window navigation into a popup. The same inversion swallowed `_parent` and
 * `_top`.
 *
 * AN EMPTY TARGET IS `_self` HERE, WHICH IS STEP 4 AND NOT A CALLER'S BUSINESS. §4.6.5 passed the literal
 * "_self" for a missing `target` attribute — a second copy of step 4 living in the caller — while this function
 * read an empty target as "create", which is neither caller's rule. §7.2.2.1's window open steps map "" to
 * "_blank" in their own step 5 BEFORE applying these rules, so `window.open(url, "")` still opens a window:
 * that mapping belongs to §7.2.2.1 and now lives there.
 *
 * AND WHAT FOLLOWED THE RULES HERE IS GONE, WHICH IS THE OTHER HALF OF THAT SAME SENTENCE. This function used
 * to END with `navigable_navigate(ctx, chosen, url ? url : "")` on the existing-navigable arm, under a comment
 * asserting the opposite of what the standard says — "an absent url is the empty string, which resolves
 * against the document's own address — so `open(\"\", \"_self\")` reloads". §7.2.2.1 step 16.1 navigates an
 * existing navigable ONLY "if urlRecord is not null", and its step 3 leaves urlRecord null exactly when url is
 * the empty string, so that call must NOT reload. §4.6.5 step 9 navigates unconditionally, so the same line
 * WAS right for the hyperlink caller — one tail serving two algorithms that disagree, which is why it could
 * not be right for both and why the fix is a split rather than a condition. See navigable.h's WindowType.
 *
 * §4.6.5 IS "FOLLOWING HYPERLINKS" AND THIS FILE CALLED IT §4.6.3 IN EVERY LINE OF THIS BLOCK. §4.6.3 is "API
 * for hyperlink elements" — a real section, about `href`/`protocol`/`host`, which is the citation failure
 * CLAUDE.md rates worse than none: it reads as authoritative and sends the reader to a clause that does not
 * say this. Only this block and the header's are corrected; a sweep of the file's other citations would be a
 * diff nobody can check. */
JSValue navigable_open(JSContext *ctx, const char *url, const char *target, const WindowFeatures *feat,
                       lxb_dom_node_t *source_element, WindowType *out_window_type)
{
    const char *name = target ? target : "";
    const bool noopener = feat && feat->noopener;
    JSValue chosen;

    DCHECK(out_window_type != NULL,
           "§7.3.1.7's rules for choosing a navigable were applied by a caller that does not read their SECOND "
           "return value. §7.2.2.1 step 12 takes both and branches its steps 15-17 on windowType; §4.6.5 step "
           "6 takes only the first and says so. A caller that reads neither is running one arm of a two-arm "
           "algorithm, which is what left `open(url, \"<existing name>\")` with no opener");
    /* §7.3.1.7 step 2: "Let windowType be `existing or none`." Written before anything can answer, so every
       path out of this function has stated it. */
    *out_window_type = WINDOW_TYPE_EXISTING_OR_NONE;

    /* THE ASSERT AT THE CONSUMER, and the question it closes is the one this algorithm structurally cannot ask:
       §7.3.1.7 takes ANY string, so a name carrying smuggled markup is legal here — for `window.open`, which is
       what §7.2.2.1 specifies and what the corpus asserts by name. It is NOT legal from an element: §4.2.3's
       get an element's target resets exactly this shape to "_blank" before the name ever reaches these rules,
       so an element-sourced name that still has it is a name that never went through the algorithm. That was
       the whole defect — `<a target>` read its attribute raw and this function had no way to tell, so a page
       could name a navigable after the tail of an HTML injection and every later `window.open("", "<name>")`
       from the injected document addressed it.
       IT IS ASSERTED HERE RATHER THAN AT EACH CALLER because a caller that would forget the algorithm is
       exactly a caller that would forget the check: this is where every navigation converges, so a route added
       later FIRES instead of silently widening the old wrong answer. The predicate is §4.2.3's own, exported by
       core/html/html_base_element.h, so the shape this crashes on and the shape that algorithm resets cannot
       drift apart. */
    DCHECK(source_element == NULL || !html_base_element_target_is_dangling(name, strlen(name)),
           "§7.3.1.7's rules for choosing a navigable were given an ELEMENT's target that still contains both "
           "an ASCII tab or newline and a U+003C (<) — §4.2.3's get an element's target resets that shape to "
           "\"_blank\", so this name reached here without it. Route the call site through "
           "html_base_element_get_target (core/html/html_base_element.h); honouring the name is a "
           "dangling-markup smuggling primitive that no browser reproduces");
    (void)source_element;

    /* Steps 4-6: the empty string and the three keywords that answer with a navigable that already exists. */
    chosen = navigable_choose_keyword(ctx, name);

    /* Step 7: "if name is not an ASCII case-insensitive match for `_blank` and noopener is false, then set
       chosen to the result of finding a navigable by target name". Both halves are conditions on the SEARCH. */
    if (JS_IsUndefined(chosen) && !target_name_is(name, "_blank") && !noopener)
        chosen = navigable_choose_name(ctx, name);
    /* STEPS 4-7 ANSWERED, so windowType stays step 2's `existing or none` and the navigable is handed back
       UNNAVIGATED — §7.2.2.1 step 16.1 and §4.6.5 step 9 are the callers' own steps and they disagree about
       the empty url, so neither may be performed here on the other's behalf. */
    if (window_proxy_is(chosen)) return chosen;
    JS_FreeValue(ctx, chosen);
    /* Step 8: nothing answered, so a new top-level traversable is created. "Let targetName be the empty
       string. If name is not an ASCII case-insensitive match for `_blank`, then set targetName to name." —
       so every name that reached step 7 and found nothing NAMES the navigable it creates, `_foo` included,
       and only `_blank` creates an unnamed one. */
    /* §7.3.1.7 step 8's THIRD OPTION: "Set windowType to `new and unrestricted`." */
    *out_window_type = WINDOW_TYPE_NEW_AND_UNRESTRICTED;
    /* §7.3.1.7 step 8's OPENER-POLICY CLAUSE, WHICH IS NOT BUILT — and crashes here rather than answering
       `new and unrestricted` for a document it does not apply to. "Let currentDocument be currentNavigable's
       active document. If currentDocument's opener policy's value is `same-origin` or `same-origin-plus-COEP`,
       and currentDocument's origin is not same origin with currentDocument's relevant settings object's
       TOP-LEVEL ORIGIN: set noopener to true, set name to `_blank`, and set windowType to `new with no
       opener`." Three assignments, and every one of them changes what the caller does next — step 17's FIRST
       clause returns null on that windowType, so a page whose popup handle this engine hands back would be
       holding one real Chrome denies it.
       THE CONDITION IS ASKED, NOT ASSUMED. Only the policy half is testable from here — this agent is
       origin-keyed, so `currentDocument`'s origin IS the agent's and "not same origin with the top-level
       origin" is the same cross-origin-nested-document shape §7.3.2.1's inherited-opener-policy arm already
       resolves through window_proxy_opener_policy. Guarding on the policy alone makes the crash fire for a
       COOP document opening any window, which OVER-reports by exactly the same-origin-top case — and an
       over-reporting crash on a page nobody can serve yet is the right side to be wrong on, because the other
       side is a silent wrong windowType. */
    {
        OpenerPolicyValue coop = window_proxy_opener_policy(document_window_proxy(ctx));

        DCHECK(coop != OPENER_POLICY_SAME_ORIGIN && coop != OPENER_POLICY_SAME_ORIGIN_PLUS_COEP,
               "§7.3.1.7 step 8's OPENER-POLICY CLAUSE is reached and not built: this document's opener policy "
               "is `same-origin` or `same-origin-plus-COEP`, so the clause must compare its origin against "
               "§8.1.3.1's TOP-LEVEL ORIGIN and, when they differ, set noopener true, the name to `_blank` and "
               "windowType to `new with no opener` — on which §7.2.2.1 step 17's first clause returns null. "
               "Without it this call answers `new and unrestricted` and hands the page a handle to a popup it "
               "must not be able to script. BUILD the comparison (realm_top_level_creation_url gives the "
               "top-level origin; origin_same compares the records) and return WINDOW_TYPE_NEW_WITH_NO_OPENER "
               "— step 17 already reads it");
    }
    /* §7.1.5: an AUXILIARY navigable has NO EMBEDDER ELEMENT, so there is no iframe sandboxing flag set for
       it to inherit — its creation flags come from the popup sandboxing flag set, which navigable_create
       derives from this document's own active set through §7.3.1.7's propagate rule. */
    DCHECK(*name, "§7.3.1.7 step 4 answers the CURRENT navigable for an empty name, so nothing empty can reach "
                  "step 8 — an empty target arriving here would mint an unnamed window where the spec navigates "
                  "this one, which is how `<a target=\"\">` used to open a popup");
    /* §7.3.1.3's container is JS_NULL here and that is the whole difference between step 8's navigable and
       §4.8.5's: the rules for choosing a navigable are given a target NAME, never an element, so nothing
       presents the traversable they create and §7.2.2.4's `frameElement` is null in it for ever. */
    return navigable_create(ctx, url, target_name_is(name, "_blank") ? NULL : name, false, feat, 0, JS_NULL);
}

/* HTML §7.2.2.1 "Opening and closing windows" — `window.open`'s WINDOW OPEN STEPS, AS A STEP MACHINE, and it is
 * one again for a reason that did not exist when the note here said the opposite. That note read "nothing to
 * ask and nothing to suspend for", which was true while the only thing `open()` did was mint a name: the step
 * that NAVIGATES fetches, and a fetch is a host-owed answer that suspends the asking flow. A plain C body
 * cannot suspend, so it can only reach a SYNCHRONOUS fetch — which is why navigable.h's DCHECK says the shipped
 * host, whose network is the trusted zone's, cannot navigate at all. The machine is what removes that sentence.
 * IT DOES NOT PARK YET. The fetch below is still the host's synchronous one; what changed is the substrate
 * under it, so making that fetch a host request is a change to child_document and to nothing here.
 * THE SECTION NUMBER WAS §7.4 IN EVERY LINE OF THIS BLOCK, AND §7.4 IS "Navigation and session history" — a
 * real section, about a different algorithm, which is the citation failure CLAUDE.md calls worse than none:
 * it reads as authoritative and sends the reader somewhere that does not say this. The steps below are
 * §7.2.2.1's; the choice they delegate is §7.3.1.7's. Only this block is corrected — the rest of this file's
 * §7.4 citations are a separate reading, and a sweep would be a diff nobody can check. */
/* WHERE THIS MACHINE RESTS. The window open steps are seventeen and the only one that can suspend is the
   navigate, which the LOAD JOB below owns rather than this member — so open() has one stage today, and the
   machine exists so that step can become a park without this being rewritten. */
#define OPEN_STAGES(X) \
    X(OPEN_CHOOSE = IDL_STEP_FIRST, \
      "HTML §7.2.2.1 Opening and closing windows — the window open steps (the features, §7.3.1.7's rules for " \
      "choosing a navigable and their windowType, step 16's navigate-and-link-the-opener arm for an existing " \
      "navigable, and steps 17-19's three returns)")
enum { OPEN_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const OPEN_STEPS[] = { OPEN_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    JSValue result;   /* the chosen navigable's proxy (owned) */
    uint8_t noopener; /* §7.2.2.1 step 18 needs it after the navigable is made */
} OpenState;

static void open_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    v->val(ctx, &((OpenState *)st)->result);
}

static int js_win_open_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                            JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    OpenState *s = st;
    const char *url, *target, *features;
    WindowFeatures feat;
    /* §7.3.1.7's SECOND return value, which steps 15, 16 and 17 all branch on — see navigable.h's WindowType
       for what reading only the first cost. */
    WindowType window_type;
    /* §7.2.2.1 step 3's urlRecord, as the only thing steps 15 and 16.1 ask about it. */
    bool url_is_null;
    /* AND THE THIRD STATE OF THAT SAME RECORD, WHICH IS NOT EITHER OF ITS TWO — see the announcement below.
       `url_is_null` is step 3's fact and answers step 16.1's own condition; this one says the record was never
       COMPUTED. Two variables and not one, because steps 15.3 and 15.4 read them in OPPOSITE directions: "if
       urlRecord is null, then set urlRecord to a URL record representing about:blank" is TRUE of the empty
       string and FALSE of an address the run does not know, and a single flag would answer whichever of those
       two questions the next reader happened to ask. */
    bool url_is_unknown;

    (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);
    DCHECK(hdr->stage == OPEN_CHOOSE,
           "window.open resumed, and §7.2.2.1 gives it one stage here — steps 15-16's navigate is the load "
           "job's, and it is that job that parks");
    s->result = JS_UNDEFINED;
    /* §7.2.2.1 STEP 4's ENCODING-PARSE IS WHERE THE PAGE'S VALUE BECOMES A DESTINATION, so the question asked
       here is the one §7.2.4's `href` setter asks at its own step 2, and it is asked with the same two calls
       rather than with a second mechanism beside them. What it is NOT is a second detector: solver/solve.h
       states that the URL class has ONE context and therefore one written-down vector — "navigating executes
       the `javascript:` scheme and nothing else does" — so `window.open` is an ARRIVAL at that class and never
       a class of its own.
       IT IS A SINK FOR THE SAME REASON `location.href = x` IS, and the chain is three algorithms long with no
       branch in it that could decline: §7.2.2.1 steps 15.5 and 16.1 NAVIGATE to urlRecord; §7.4.2.2 "Beginning
       navigation" — "if url's scheme is `javascript`: QUEUE A GLOBAL TASK on the navigation and traversal task
       source given navigable's active window to NAVIGATE TO A javascript: URL … and return"; and §7.4.2.3.2
       "The javascript: URL special case" step 7 — "let newDocument be the result of EVALUATING A javascript:
       URL given targetNavigable, url, initiatorOrigin, and userInvolvement". So `window.open("javascript:X9()")`
       runs the program, and this member had no arrival at all.
       IT IS GATED ON STEP 4's OWN CONDITION AND ON NOTHING ELSE. "If url is not the empty string" is what makes
       the encoding-parse happen, so `window.open()` and `window.open("")` reach no destination and are not
       arrivals — counting them would raise solver/solve.h's `reached` for a step that does not run, which is
       the rung whose absence and whose zero read alike. An UNKNOWN url cannot answer that condition either way,
       and §Solver-half keeps the arm uncertainty leaves open, so it announces.
       THE ANNOUNCEMENT IS UNCONDITIONAL ON TAINT for the same reason §7.2.4's is: solver/solve.c's URL detector
       is BOTH the exploration-time recorder and the verification-time fire oracle, and a candidate run's value
       is a plain String by construction — declining to announce it would leave the oracle nothing to observe.
       Placed BEFORE the conversion below so the unknown is never asked for bytes it does not have. */
    url_is_unknown = argc > 0 && concolic_is(argv[0]) != 0;
    /* ASKING A CONCOLIC FOR A C STRING IS THE ToString THIS ENGINE HAS NO CONCOLIC SEMANTICS FOR — it hands
       back a plausible concrete address with the source identity and the domain taken off the triple, which is
       §Attacker-sources' "a source without its constraints yields PoCs that don't reproduce" performed on the
       destination itself. The unknown is carried as a FACT instead, and every step that reads it below states
       which of the two records it is asking about. */
    url    = (argc > 0 && !url_is_unknown) ? JS_ToCString(ctx, argv[0]) : NULL;
    /* §7.3.1.7's TARGET IS A NAVIGABLE NAME AND NOT A SINK, and it is exactly that difference that makes an
       unknown one a crash here rather than an announcement: there is no class for it to arrive at, and the
       rules for choosing a navigable compare the name for EQUALITY against every navigable in the group, which
       a stringified unknown answers with whatever bytes the ToString invented. */
    DCHECK(!(argc > 1 && concolic_is(argv[1])),
           "§7.2.2.1's window open steps were given unknown external input as the TARGET — step 13 hands it to "
           "§7.3.1.7's rules for choosing a navigable, whose steps 4-7 compare it for equality against the "
           "keywords and against every navigable's name, and converting it to bytes here decides all of those "
           "comparisons from a fabricated string. BUILD the name comparison over a concolic (solver/concolic.h "
           "carries the domain the equality would narrow) so `open(u, location.hash)` forks the arm that names "
           "an existing navigable and the arm that creates one, instead of picking one of them silently");
    /* §7.2.2.1 step 6 TOKENIZES the features, and step 15.1's check-if-a-popup-window-is-requested and step 9's
       noopener are both read out of the map that produces — so an unknown here decides whether the page gets a
       popup, and whether it gets an OPENER at all, from bytes nothing computed. */
    DCHECK(!(argc > 2 && concolic_is(argv[2])),
           "§7.2.2.1's window open steps were given unknown external input as the FEATURES string — step 6 "
           "tokenizes it and steps 9 and 15.1 read `noopener` and the popup decision straight out of that map, "
           "so stringifying it here answers both from a fabricated token list. BUILD the tokenization over a "
           "concolic (core/frame/window_features.h is where the map is made) so an unknown feature string "
           "forks the popup and the tab rather than choosing between them");
    target = argc > 1 ? JS_ToCString(ctx, argv[1]) : NULL;
    /* §3.6: an OPTIONAL argument given `undefined` is ABSENT. `open(url, name, undefined)` is how the
       corpus spells "no features", and stringifying it produced the literal "undefined" — one token,
       a non-empty map, and therefore a popup where the spec has a tab. */
    features = (argc > 2 && !JS_IsUndefined(argv[2])) ? JS_ToCString(ctx, argv[2]) : NULL;
    if (argc > 0 && !url_is_unknown && !url) return JS_STEP_ABRUPT;
    /* §7.2.2.1's THIRD ARGUMENT decides whether the new navigable is a POPUP — what §7.2.2.5's six BarProps
       answer from — and whether it gets an OPENER at all. */
    feat = window_features_parse(features);
    s->noopener = feat.noopener ? 1 : 0;
    /* §7.2.2.1's window open steps, STEP 5: "If target is the empty string, then set target to `_blank`." It
       belongs to THIS algorithm and not to §7.3.1.7's, whose own step 4 answers the CURRENT navigable for an
       empty name — which is what `<a target="">` gets and what `window.open(url, "")` must not. An omitted
       argument takes the same branch because the IDL's default for it IS "_blank"
       (open(optional USVString url = "", optional DOMString target = "_blank", …)), so the two spellings of
       "no target" reach one answer here rather than two further down. */
    /* NO SOURCE ELEMENT, and that is §7.2.2.1's own statement rather than a hole: the window open steps are
       reached from script, never from an element, so §4.2.3's get an element's target does not run and the
       target is NOT reset. `dangling-markup-window-name.html`'s first subtest is named for exactly that —
       "Dangling Markup in target is not reset when set by window.open". */
    /* §7.2.2.1 STEP 3: "Let urlRecord be null. If url is not the empty string: set urlRecord to the result of
       encoding-parsing a URL given url…". THE EMPTY STRING LEAVES IT NULL, and that is not bookkeeping — step
       16.1 navigates an existing navigable only "if urlRecord is not null", so `open("", "_self")` chooses this
       navigable and does NOTHING to it. The IDL default for an omitted `url` IS the empty string, so an absent
       argument takes the same branch. */
    url_is_null = !url_is_unknown && (url == NULL || *url == '\0');
    /* AND THE ARRIVAL ITSELF, at step 4's condition — see the block above the conversion. */
    if (argc > 0 && !url_is_null) solve_url_sink(ctx, argv[0]);
    /* THE ONE THING THIS MEMBER MUST NOT HAND ON, asserted where it is handed on. §7.3.1.7's rules for choosing
       a navigable do not read the url at all, so an unknown destination changes nothing about WHICH navigable
       this call answers with — what it changes is that there is no address to navigate that navigable TO, and
       the create below must therefore be given none rather than bytes a ToString invented. */
    DCHECK(!url_is_unknown || url == NULL,
           "an unknown destination was converted to bytes on its way into §7.2.2.1 step 13's choose — the "
           "address the created navigable would then carry is a fabricated string with no source identity and "
           "no domain left on it");
    s->result = navigable_open(ctx, url, (target && *target) ? target : "_blank", &feat, NULL, &window_type);
    if (features) JS_FreeCString(ctx, features);
    /* §7.2.2.1 step 4: "If urlRecord is failure, then throw a \"SyntaxError\" DOMException" — the PAGE's
       mistake, and the one this algorithm hands back rather than swallowing. */
    if (JS_IsUndefined(s->result)) {
        if (url) JS_FreeCString(ctx, url);
        if (target) JS_FreeCString(ctx, target);
        JS_ThrowDOMException(ctx, "SyntaxError", "the URL to open is not a URL");
        return JS_STEP_ABRUPT;
    }
    /* §7.2.2.1 STEPS 15 AND 16 — the two arms, split by windowType, which is the split this member did not
       have. Step 15's arm ("new and unrestricted" / "new with no opener") is performed by the create inside
       navigable_open: it sets is-popup from the tokenized features, and §7.4 step 14 enqueues the load that is
       step 15's navigate. Step 16's arm is HERE because it is the one an EXISTING navigable takes, and it has
       two steps that step 15 has no counterpart for. */
    if (window_type == WINDOW_TYPE_EXISTING_OR_NONE) {
        /* STEP 16.1: "If urlRecord is not null, then navigate targetNavigable to urlRecord using
           sourceDocument…". The condition is step 3's, read off `url_is_null` above.
           AND THE SECOND CONDITION IS NOT STEP 16.1's — it is the difference between a step whose CONDITION is
           false and a step whose condition is TRUE and which this engine cannot perform, and writing them as
           one `if` is what would hide that. An unknown destination IS "not null", so the standard asks for a
           navigate here; what it cannot be given is an address, and navigating on a guess is worse than not
           navigating — it replaces the document this flow is exploring for a destination the run never
           computed, which is the same answer core/frame/location.c gives §7.2.4's three whole-URL algorithms.
           RESIDUAL — the code is right and NARROWER. NOT COVERED: the exploring flow never loads the document
           at an unknown address, so the chosen navigable keeps showing what it was showing and the same is
           true of the create arm above, which is handed no url and stays at §7.3.2.1's initial about:blank.
           THE NEXT DIFF BUILDS a navigation whose DESTINATION is concolic — the whole-URL half of the same
           capability core/frame/location.c's component-setter assert names by URL §4.4's url_parse_override —
           so §7.4 step 14's load job forks over the domain instead of being skipped. HOW ITS ABSENCE SHOWS:
           `var w = window.open(location.hash.slice(1))` leaves `w` at about:blank for ever, so a document
           reachable ONLY through `window.open(tainted)` contributes no @H endpoints from exploration and is
           seen only when an @S candidate run substitutes real bytes at the source. */
        if (!url_is_null && !url_is_unknown) {
            JSValue r = navigable_navigate(ctx, s->result, url);
            JS_FreeValue(ctx, s->result);
            s->result = r;
        }
        /* STEP 16.2: "If noopener is false, then set targetNavigable's active browsing context's OPENER
           BROWSING CONTEXT to sourceDocument's browsing context." NOTHING PERFORMED THIS STEP, and nothing
           could: every opener this engine set was set at a create, so a navigable that already existed when
           `open()` named it was linked by no one. That is the whole of
           `embedded-opener-remove-frame.html :: opener of discarded nested browsing context` failing on its
           FIRST assert — `opener before removal expected object "[object Window]" but got null` — before the
           removal the file is named for ever happens.
           IT IS `sourceDocument`'s BROWSING CONTEXT, which is the navigable of the realm this member is running
           in, and that is document_window_proxy's answer rather than the chosen navigable's parent or top: a
           page can name a frame of any document it is familiar with, and what links back is always the one that
           CALLED open. */
        if (!s->noopener && !JS_IsNull(s->result) && !JS_IsUndefined(s->result))
            window_proxy_set_opener(ctx, s->result, document_window_proxy(ctx));
    }
    if (url) JS_FreeCString(ctx, url);
    /* §7.2.2.1 STEPS 17 AND 18 — TWO STEPS AND NOT ONE STEP'S TWO SENTENCES, which is what this block called
       them, and the second of them has a clause this member did not read. The window open steps run to
       NINETEEN: 17 and 18 are the two returns of null and 19 is "return targetNavigable's active WindowProxy",
       which is the fallthrough below.
       IT READS `target` AS THE PAGE WROTE IT, which is why the argument is still alive here: step 5's map of
       "" to "_blank" is applied at the CALL to the rules above and not to this variable, so an empty target is
       correctly not one of the three keywords.
       STEP 17: "If windowType is `new with no opener`, then return null." STEP 18: "If noopener is true and
       target is not an ASCII case-insensitive match for `_self`, `_parent`, or `_top`, then return null."
       THE EXCLUSION IS THE POINT. `open(url, "_self", "noopener")` navigates THIS navigable — navigable_open's
       own comment argues that at length — and a page cannot be denied a handle it already holds, so there is
       nothing for the flag to withhold and the step says so by listing the three keywords. This member nulled
       the result whenever `noopener` was set, so exactly those three calls answered null while doing the
       navigation, which is the same inversion the rules for choosing a navigable had already been repaired of
       one level down: the flag guards ONE thing, and it is not this one.
       `target` HERE IS THE ARGUMENT AS THE PAGE WROTE IT, before step 5's map of "" to "_blank" — an empty
       target is not one of the three keywords, so `open(url, "", "noopener")` returns null, and it is a new
       window rather than this one. */
    {
        const char *t = target ? target : "";

        if (window_type == WINDOW_TYPE_NEW_WITH_NO_OPENER ||
            (s->noopener && !target_name_is(t, "_self") && !target_name_is(t, "_parent")
                         && !target_name_is(t, "_top"))) {
            JS_FreeValue(ctx, s->result);
            s->result = JS_NULL;
        }
    }
    if (target) JS_FreeCString(ctx, target);
    *presult = s->result;
    s->result = JS_UNDEFINED;
    return JS_STEP_DONE;
}

static const IdlStepDecl OPEN_DECL = { js_win_open_step, sizeof(OpenState), open_visit, NULL,
                                      "HTML §7.2.2.1 open(url, target, features)", OPEN_STEPS };

static int g_id_open;

/* §7.2.2.1's IDL: open(optional USVString url = "", optional DOMString target = "_blank",
   optional [LegacyNullToEmptyString] DOMString features = "") -> WindowProxy?
   DECLARED ONCE PER AGENT — a member has one pool entry, and every realm's global installs that same one. */
void navigable_init(JSContext *ctx)
{
    static const IdlArgType OPEN_ARGS[3] = { IDL_USVSTRING, IDL_DOMSTRING, IDL_DOMSTRING };

    g_id_open = idl_method_id_step(ctx, OPEN_ARGS, 3, NULL, 0, &OPEN_DECL, 0);
    idl_optional_from(0);
    /* THE BROWSING CONTEXT GROUP'S LIST, built at INIT so it belongs to the pre-boot BASELINE: a flow that
       opens a window writes into it, and the COW delta captures that write for that flow alone. An array
       allocated lazily inside a flow would be that flow's own object and no sibling would ever see it. */
    if (JS_IsUndefined(g_group)) {
        g_group = JS_NewArray(ctx);
        CHECK(!JS_IsException(g_group), "the browsing context group's list could not be allocated");
    }
}

void navigable_install(JSContext *ctx, JSValueConst global, const char *origin)
{
    /* THE ORIGIN IS THE AGENT'S, NOT THE DOCUMENT'S — one record, held by core/url/origin.c, and this is where
       the host's per-document statement is CHECKED against it rather than kept as a second answer. An agent is
       origin-keyed, so every document installed into this instance has the same principal and a second install
       is a second DOCUMENT; a DIFFERENT origin arriving here would be two principals behind one instance, which
       SECURITY.md forbids and which would make an about:blank child inherit the wrong one.
       IT IS A SERIALIZATION COMPARISON, and that is sound in exactly this direction: a host that states a tuple
       origin states these bytes, and a host that states "null" is stating an opaque origin whose identity this
       agent MINTED at adopt — so the check that matters (two different TUPLE origins in one agent) is the one
       it can make, and the opaque case cannot arise because a second opaque document is a second instance. */
    DCHECK(origin != NULL && !strcmp(origin_serialized(origin_agent()), origin),
           "a second document was installed into this agent with a DIFFERENT origin — an agent is origin-keyed, "
           "so a cross-origin document is a second INSTANCE and never a second realm in this one");
    /* HOW A REALM'S DEATH REACHES THIS AGENT. Declared here because this is where a realm of this agent is
       first known, and declared for the RUNTIME because that is what a realm belongs to — the same declaration
       from a second document is the same function and quickjs says so. It outlives navigable_free on purpose:
       a realm still reachable when the agent's own state goes is torn down after it, and its Document must go
       with it or it is a leak the gc_obj_list walk reports with nothing to explain it. */
    JS_SetContextTeardownHook(JS_GetRuntime(ctx), navigable_realm_teardown);
    idl_install_method(ctx, global, "open", g_id_open);
}

void navigable_free(JSContext *ctx)
{
    /* THE GROUP'S LIST. It holds a reference to every top-level traversable's proxy, and a proxy keeps its
       Window — so releasing it here is releasing the last thing this component holds ON a navigable, which is
       what lets the realms behind those proxies become collectable. */
    JS_FreeValue(ctx, g_group);
    g_group = JS_UNDEFINED;
    /* THE LIST GOES, THE REALMS DO NOT. Every entry is BORROWED (navigable_realm hands ownership to the
       navigable), so there is nothing here to release and nothing here to free them with: a realm still
       reachable at this point outlives this agent's own state and is torn down by whoever holds it, releasing
       its Document through the teardown hook — which is deliberately left declared for exactly that. */
    free(g_realms);
    g_realms = NULL;
    g_realms_n = g_realms_cap = 0;
    /* AND THE CENSUS GOES WITH THE LIST, because it is a census of ONE AGENT and not of the process: a second
       agent built in this runtime starts having made none, and totals carried over from the first would make
       every reading of them (the OOM CHECK's, the result document's, the fixture's) a sum over two agents that
       is presented as one agent's working set. A reader that needs these numbers must take them before this
       call — test_forced.c's probes run above it for exactly that reason. */
    g_realms_made = g_realms_peak = 0;
    g_realm_builder = NULL;
}
