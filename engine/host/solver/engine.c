/* The dispatch loop — see engine.h. */
#include "core/fetch/fetch.h"
#include "core/mime/mime_type.h"   /* §4.4 "Is a JavaScript MIME type": what makes a reply a program rather
                                       than a Response, asked at the fetch delivery below */
#include "core/fetch/scheme_fetch.h"   /* Fetch §4.3 Scheme fetch: who answers a park's address — this agent
                                          or the host — asked once, at the one place a park takes one */
#include "solver/engine.h"
#include "quickjs-step.h"   /* a host answer is taken by a step machine, and a throw ends it JS_STEP_ABRUPT */
#include "core/html/unhandled_rejection.h"   /* HTML §8.1.4.7 Unhandled promise rejections: "notify about
                                                rejected promises" runs at every microtask checkpoint */
#include "core/events/report_exception.h"   /* HTML §8.1.4.4 step 8: what a classic script's abrupt completion
                                               owes the page, as the frame the scheduler runs it as */
#include "core/html/close_request.h"   /* HTML §6.10.1 Close requests: the global task its preamble queues,
                                          as the frame a modelled potential close request runs as */
#include "core/idl_args.h"   /* the one point every Web API member passes through — see idl_slowest_step */
#include "solver/result.h"
#include "solver/solve.h"
#include "solver/flow.h"
#include "solver/dyn_body.h"  /* a queued program's source text — one buffer, every timeline that holds it */
#include "solver/decide.h"
#include "solver/concolic.h"
#include "solver/cow.h"
#include "solver/world.h"   /* the routed record's world vector: whose timeline a delivery belongs to */
#include "solver/value_dump.h"   /* a host instrument's completion value, dumped with its examples resolved */
#include "core/dom/document.h"   /* which document a parked program belongs to: the realm it is compiled in */
#include "core/dom/document_current_script.h"   /* §4.12.1.1's bracket around the classic arm of a row */
#include "core/dom/node.h"       /* the element a row is the program of, crossing a park as its wrapper */
#include "core/html/html_script.h"   /* §4.12.1.1's encoding-parse of `src` against §8.1.3.2's API base URL */
#include "core/html/cors_settings_attribute.h"   /* HTML §2.5.4: the CORS settings attribute credentials
                                                   mode an element's `crossorigin` state names, which is what a
                                                   §4.12.1.1 park states as its request's credentials mode */
#include "core/html/nonce_attribute.h"   /* §4.12.1.1 reads el's [[CryptographicNonce]] slot, not its attribute */
#include "core/loader/script_fetch.h"   /* HTML §8.1.4.2: where a fetched body becomes a script's source text */
#include "core/loader/document_scripts.h"   /* §4.12.1.1's schedule, for engine_run's caller that states none */
#include "core/frame/window_message.h"   /* the receiving half of a routed `windowproxy.post` */
#include "core/frame/remote_op.h"        /* the receiving half of a cross-agent operation: what performs it */
#include "core/frame/remote_object.h"    /* …and the grammar its completion crosses back in */
#include "core/frame/navigable.h"   /* HTML §7.3.1 Navigables: tree order over this agent's navigables */
#include "core/frame/window_proxy.h" /* which realm of this agent a document name is, in the asking timeline */
#include "core/frame/document_lifecycle.h"   /* HTML §7.4.6.1: what a navigation does to the Document it replaces */
#include "core/timing/event_loop.h"   /* HTML §8.1.7.3 Processing model: the clock's second mover — the work the
                                         running flow retired, which the interpreter reports and this file relays */
#include "solver/dom_cow.h"   /* the DOM half of time-travel — swapped per-flow alongside the heap COW delta */
#include "solver/cold.h"      /* the park and the resume themselves; their census is composed in result.c */
#include "solver/quantum.h"   /* the cooperative quantum's asynchronous edge, and what this host can measure */
#include "solver/reclaim.h"   /* …and its RAM edge: which allocators can sell a flow rather than fail */
#include "solver/reply_decode.h" /* …and what the reply body itself teaches — see engine_provide */
#include "solver/pending_index.h" /* which records a reply answers, without walking the frontier to find out */
#include "solver/endpoint.h"    /* the @H surface, and one of the three tables no leak walk in this runtime can see */
#include "solver/attr_shadow.h" /* …and the taint shadow, whose entries are GC objects and are leaked by two hosts */
#include "solver/metrics.h"     /* the agent's metrics block, released with the agent */
#include "check.h"
#include <time.h>
#include <stdlib.h>
#include <malloc.h>   /* the allocator under the JS heap — see engine_c_alloc_live, read by result.c */
#include <stdio.h>
#include <string.h>

/* The DOM identity map's size, for the diagnostic line below. The DOM layer registers it, so the solver does not
   depend on lexbor (and through it every host that links the scheduler) for one statistic. A host with no
   document registers none and the diagnostic reports zero. */
static void (*g_wrap_stats)(long *n, long *cap);

/* One claimant; NULL gives the slot back. The DOM layer releases it at its own release, which the platform runs
   before solver_agent_free, and the assert makes that ordering checked. */
void engine_set_wrap_stats(void (*fn)(long *n, long *cap))
{
    DCHECK(fn == NULL || g_wrap_stats == NULL,
           "a second component claimed the wrapper-census hook — there is one identity map, and the second "
           "claim silently decides what every diagnostic line reports");
    g_wrap_stats = fn;
}


/* §3.1.7's answer for the row this flow is executing; defined with the other cursor accessors below and declared
   here because §8.1.6.7.3's descendant script fetch options need it at the module park. */
static lxb_dom_element_t *flow_dyn_el(const Flow *f);

/* HTML §4.12.1.1 Processing model: the three metadata members a `script` element's request carries, borrowed
 * from the element and released by the caller with script_csp_meta_free. All three script parks read them here.
 *
 * "Let cryptographic nonce be el's [[CryptographicNonce]] internal slot's value"; integrity metadata is the
 * `integrity` attribute's value or the empty string; parser metadata is "parser-inserted" if el is
 * parser-inserted and "not-parser-inserted" otherwise. A `<script>` therefore states its metadata (it is never
 * csp_request_metadata_unstated's), so `<script nonce=x>` under `script-src 'nonce-x'` is not blocked.
 * The nonce is the slot, not the attribute: HTML §2.5.6 Nonce attributes blanks the attribute after hiding while
 * the slot keeps the value (core/html/html_link.c reads it the same way).
 *
 * A NULL element is a stated answer. A dynamic `import()` takes §8.1.6.7.3's descendant script fetch options,
 * whose step 1 copies the referrer's options, so the nonce is the importing row's element's; NULL there is a
 * program no `<script>` produced (a lazy chunk, a `javascript:` URL, an @S candidate), whose options carry the
 * empty string. Its integrity (step 2, resolving a module integrity metadata) is the empty string because this
 * engine has no import map. */
typedef struct {
    CspRequestMetadata m;
    JSValue            nonce_slot;   /* owned by the holder */
    const char        *nonce;        /* owned by the holder */
} ScriptCspMeta;

static ScriptCspMeta script_csp_meta(JSContext *ctx, lxb_dom_element_t *el)
{
    ScriptCspMeta h;
    size_t integrity_n = 0;
    const lxb_char_t *integrity = NULL;
    /* Unplaced until the switch below states one (core/frame/policy_container.h's sentinel), so an enumerator
       nobody mapped aborts rather than taking whatever zero means. */
    CspParserMetadata parser = CSP_PARSER_METADATA_UNPLACED;

    h.nonce_slot = JS_UNDEFINED;
    h.nonce = NULL;
    /* The element arrives over a seam from three parks, so its type is asserted here as well as at the read:
       a dangling or foreign pointer is non-NULL, passes `if (el)`, and would reach
       core/html/nonce_attribute.c's slot read. That read's own assert names the bad operand; this one says the
       seam handed over something that is not an element at all, which is a repair in a different file. */
    DCHECK(el == NULL || lxb_dom_interface_node(el)->type == LXB_DOM_NODE_TYPE_ELEMENT,
           "§4.12.1.1's script metadata was composed for a node that is not an ELEMENT — every park that "
           "reaches this line holds an element of a live tree, so another node type means the pointer this "
           "seam carried names freed or foreign memory and the nonce read below walks it");
    if (el) {
        h.nonce_slot = nonce_attribute_current(ctx, el);
        /* An unknown nonce is an undecided predicate: §2.5.6's slot holds a JS value so that
           `s.nonce = location.hash.slice(1)` survives as a source, and CSP §6.7.2.3 step 3.1 asks whether it is
           identical to a policy's base64-value, which an unconstrained domain answers neither way. The arm must
           fork; a coerced example would report one world's verdict as both (core/html/html_link.c, same read). */
        DCHECK(!concolic_is(h.nonce_slot),
               "a `script` element's [[CryptographicNonce]] is an unknown external value, and CSP §6.7.2.3 "
               "step 3.1 compares it against this policy's nonce-source expressions for identity — which an "
               "unconstrained domain does not answer. Fork the request here into the matching and "
               "non-matching arms and let each carry the narrowed domain, the way an opaque operand forks "
               "every other predicate; a coerced example reports ONE arm's verdict as though it were both");
        /* The slot read can refuse, and a refusal is not OOM. engine/qjs/quickjs.c's string constructor bounds
           the length it is handed and throws a RangeError for an oversized one; that arrives here as an
           exception in the slot, and `JS_ToCString` of an exception is NULL, the same NULL an allocation failure
           gives. The two are separated so the message names a corrupt length rather than memory. */
        if (JS_IsException(h.nonce_slot)) {
            /* Fatal by design: a dev build has already aborted inside the string constructor with the caller's
               stack above it, and in release no narrower record exists, since §4.12.1.1's metadata grades the
               request and a nonce nobody could read would report a load a browser refuses. */
            CHECK_FAIL("§4.12.1.1: a `script` element's [[CryptographicNonce]] could not be READ — §2.5.6's "
                       "slot held bytes whose length the engine's own string constructor refused, which is a "
                       "corrupt or stale attribute length rather than an allocation failure. The dev build "
                       "aborts inside that constructor and its stack names the seam that carried the element");
        }
        h.nonce = JS_ToCString(ctx, h.nonce_slot);
        CHECK(h.nonce != NULL, "§4.12.1.1: OOM reading a script element's cryptographic nonce metadata");
        integrity = lxb_dom_element_get_attribute(el, (const lxb_char_t *)"integrity", 9, &integrity_n);
    }
    /* Fetch §2.2.5's parser metadata, computed by §4.12.1.1 from this element. CSP §6.7.1.1 step 1.3 decides
       a 'strict-dynamic' request by this field alone (Blocked for "parser-inserted", Allowed otherwise, host
       list never consulted), so its absence inverts a directive rather than narrowing it. It is read off the
       element rather than passed in because `engine_pending_docscript` runs from `flow_step` against a row, and
       a dynamic `import()` copies the importing script's options (§8.1.4.2 descendant script fetch options
       step 1); the element is what all three parks hold.

       Named residual. Not covered: a Document this engine loads never runs §4.12.1.1 over its own markup
       `<script>` elements (core/html/html_script.h's `html_script_parser_inserted` residual: the parse happens
       before the Document has a realm, so core/loader/document_scripts.c's inventory runs those rows). They
       answer unstated, and this writes §2.2.5's initial value, the empty string, where a browser writes
       "parser-inserted". Next diff builds: the Document (HTML §7.5.1 step 9) and realm (step 7.5) existing
       before §7.5.2 Loading HTML documents step 3 creates the parser; this read then answers parser-inserted
       unchanged. Absence shows as: a loaded document's markup `<script src>` under 'strict-dynamic' that
       matches no nonce-source or digest (§6.7.1.1 steps 1.1, 1.2) loads here and is refused by a browser.
       Injected scripts, the population @S constructs, are unaffected. */
    switch (html_script_parser_metadata(el)) {
    case HTML_SCRIPT_PARSER_METADATA_PARSER_INSERTED:     parser = CSP_PARSER_METADATA_PARSER_INSERTED; break;
    case HTML_SCRIPT_PARSER_METADATA_NOT_PARSER_INSERTED: parser = CSP_PARSER_METADATA_NOT_PARSER_INSERTED; break;
    case HTML_SCRIPT_PARSER_METADATA_UNSTATED:            parser = CSP_PARSER_METADATA_EMPTY; break;
    }
    DCHECK(parser != CSP_PARSER_METADATA_UNPLACED,
           "core/html/html_script.h's parser-metadata answer gained a member this mapping does not state — "
           "Fetch §2.2.5 gives the request field three values and each of them has a row above, so a fourth "
           "answer is an enumerator added on one side of the seam and not the other");
    h.m = csp_request_metadata(h.nonce ? h.nonce : "", h.nonce ? strlen(h.nonce) : 0,
                               integrity ? (const char *)integrity : "", integrity ? integrity_n : 0, parser);
    return h;
}

/* Fetch §2.2.5 Requests' credentials mode for a park that builds its own request record and whose reply is a
   program; every other producer's mode is asserted at `fetch_owe` (core/fetch/fetch.c).
   HTML §4.12.1.1: "Let module script credentials mode be the CORS settings attribute credentials mode for el's
   crossorigin content attribute", and the script fetch options carry it for classic and module scripts alike.
   §8.1.4.2's set up the classic script request copies it onto the request, and a dynamic `import()` inherits
   it (descendant script fetch options step 1 copies the options).
   This is §2.5.4's table, not §2.5.1's, and the two differ for the No CORS state: §2.5.1 answers `include`
   (a bare `<img src>` carries cookies), §2.5.4 answers `same-origin` (a `<script src>` without `crossorigin`
   carries the session same-origin only). core/html/cors_settings_attribute.h holds both.
   NULL is a row no `<script>` produced (a lazy chunk, an @S candidate, a `javascript:` URL): §8.1.4.2's default
   script fetch options say `same-origin`, which is also §2.5.4's No CORS row. */
static FetchCredentialsMode script_credentials_mode(lxb_dom_element_t *el)
{
    return el ? cors_settings_attribute_credentials(cors_settings_attribute_state(el))
              : FETCH_CREDENTIALS_SAME_ORIGIN;
}

/* Fetch §2.2.5 Requests' mode for a script park, which, unlike the credentials mode, depends on the script's
   type. HTML §8.1.4.2's fetch a classic script creates a potential-CORS request (§2.5.1), so an element with no
   `crossorigin` is `no-cors`; fetch a single module script creates a request whose mode is `cors`
   unconditionally. Set up the classic script request overwrites the credentials mode and leaves the mode alone.
   §2.5.4's note ("the request's mode is always `cors`") describes the credentials-only repurposing, not
   classic-script fetching; following it would let SRI §3.8.2's early-allow arm fire for a `<script src
   integrity>` with no `crossorigin`, the no-CORS script an integrity policy exists to refuse.
   No element is the No CORS state, §2.5.4's missing value default. */
static FetchMode script_request_mode(ScriptType stype, lxb_dom_element_t *el)
{
    if (stype == SCRIPT_TYPE_MODULE) return FETCH_MODE_CORS;
    return el ? cors_potential_request_mode(cors_settings_attribute_state(el)) : FETCH_MODE_NO_CORS;
}

static void script_csp_meta_free(JSContext *ctx, ScriptCspMeta *h)
{
    if (h->nonce) JS_FreeCString(ctx, h->nonce);
    JS_FreeValue(ctx, h->nonce_slot);
    h->nonce = NULL;
    h->nonce_slot = JS_UNDEFINED;
}

/* Which @H door a program park is: the one recorder whose mechanism cannot be a literal at the call, because
   it sees every kind solver/pending.h's `pending_kind_is_program` admits. The switch covers exactly that
   predicate's true arm, and the caller has refused everything else, so the fallthrough is a guard over an
   enumeration this codebase owns: what reaches it is a program kind added to the predicate and not here.

   Three tokens, because they are evidence of different things: a document's own external script is markup;
   an injected script and a dynamic `import()` are code this engine ran, and a lazy chunk reached through the
   third is what the runtime-only razor is most about.

   The first two are separated by the parser-inserted mark, not the kind. A kind is a queue position and a
   door is about origin (solver/pending.h's `PROV_OBSERVED`). HTML §4.12.1.1 tests "el has an async attribute
   or el's force async is true" before "el is not parser inserted" (core/loader/document_scripts.c's
   `script_block_schedule` keeps that order), so a parser-inserted `<script async src>` takes
   FLOW_PENDING_SCRIPT, and a script-created element whose `async` setter cleared force async takes
   FLOW_PENDING_DOCSCRIPT. Keyed on the kind, the first would over-credit the razor and the second would claim
   a markup scan reaches an address only page code composed. The module arm reads no mark because
   `pending_prov_compose` refuses a parser-inserted park of any other kind (§4.12.1.1 gives `parser document`
   to `script` elements only).

   Named residual. Not covered: a `<script src>` written by `document.write`. Its `parser document` is non-null
   (core/html/html_script.c prepares it at the tokenizer's `</script>`; core/html/document_open.c), so it takes
   `document-script`, classed `markup` by endpoint.h, although running code produced the bytes; the razor's
   floor falls. Next diff builds: producers stating whether the element's bytes are the response's, carried
   beside `parser_inserted` on the park. It may not be inferred from which producer parked, because
   core/loader/xml_document.c records that a load's own scripts return at §4.12.1.1 step 18 today and that
   order is due to change. Absence shows as: a document whose only external script was written by
   `document.write` reports a nonzero `epReach.markup`. */
static int program_load_door(int kind, int parser_inserted)
{
    switch (kind) {
    /* One kind test and one mark test for the two element doors: both queue positions hold parser-inserted
       and script-created elements (core/html/html_script.c chooses between them on the schedule), so the mark
       separates the doors and the kind only says the mark is readable. */
    case FLOW_PENDING_DOCSCRIPT:
    case FLOW_PENDING_SCRIPT:    return parser_inserted ? EPD_DOCUMENT_SCRIPT : EPD_INJECTED_SCRIPT;
    case FLOW_PENDING_MODULE:    return EPD_MODULE_IMPORT;
    }
    DCHECKF(0, "a park whose reply becomes a PROGRAM states the kind %d, which `pending_kind_is_program` "
               "admitted and this mapping has no door for — the two read the same enumeration one line "
               "apart, so this is a program kind added to that predicate's true arm without saying which "
               "mechanism composes its address, and the @H row for every such load would name a mechanism "
               "nothing ran", kind);
    /* The release arm returns the refused value rather than a plausible door: `endpoint_door_token`'s CHECK is
       fatal in every build, so a release engine aborts at the emit naming the door instead of publishing
       whichever door a fallback picked. The two components' release arms are designed together. */
    return EPD_UNSTATED;
}

/* Writes a park's address onto its record and asks Fetch §4.3 Scheme fetch who answers it, in one function.
 * §4.3 answers `about`, `blob` and `data` inside this agent (Fetch §2.1 URL: "A local scheme is `about`,
 * `blob`, or `data`"); only its HTTP(S) scheme row reaches HTTP fetch, which is all the trusted zone reading this
 * register fetches. Recording an address without asking would park a flow on an answer that can only be a
 * refusal, and the page would see a load failure for bytes already in this address space.
 *
 * Every entry that parks an address comes here: `fetch()`/`fetch_owe`, a document's external `<script src>`, an
 * injected or as-soon-as-possible script, a dynamic `import()`, and a browser algorithm's own subresource
 * (`engine_pending_resource_url`). A local answer is placed on the entry, `haveValue` included, exactly as
 * engine_provide places the host's, so flow_deliver_one_reply delivers both the same way and no entry needs a
 * `deliver` closure. The scheme switch is core/fetch/scheme_fetch.h's. The record's destination and provenance
 * are written whoever answers, because they belong to the request (a `data:` URL script is a code load). */
static void pending_park_request(JSContext *ctx, JSValue e, const FetchRequest *req)
{
    JSValue reply = JS_UNDEFINED;
    /* §4.1 step 6's answer, the address every later step reads. `up_url` is owned and NULL when step 6 left
       the request unchanged; `use_url` is what to read, `up_url` what to free. */
    char *up_url = NULL;
    const char *use_url;
    /* §4.3's request: this one with step 6's address, copied because the caller's record is `const`. */
    FetchRequest sf;
    /* The park's kind, read once for the program gate and the @H door. */
    int pkind;

    /* Method and URL are the request's identity and the park's key: the join lists the pair and engine_provide
       delivers against it (engine.h). Fetch §2.2.5 Requests: "Unless stated otherwise it is `GET`", so every
       request has a method; a park without one is a producer that dropped a field, and assuming GET would file a
       POST under a GET and collect the GET's body. */
    DCHECK(req != NULL && req->url != NULL && *req->url,
           "a park was given an address that is not one — every caller of this door resolved a URL before it "
           "reached here, and an entry with no address is one the join can neither list nor answer");
    DCHECK(req->method != NULL && *req->method,
           "a request parked on an ADDRESS without stating its METHOD — the reply seam is keyed on the pair "
           "(engine.h), so an unnamed method would park this request under another one's identity and settle "
           "it with that request's body. Name the method at the component that built the request");
    /* The destination, which a park cannot derive (Fetch §2.2.5 Requests). The empty string is a real
       destination; NULL is a producer that stated none. The field stays even though FLOW_PENDING_RESOURCE now
       separates code-bearing parks by kind, because one algorithm asks with many destinations: `as=script`,
       `as=style` and `as=font` are one FLOW_PENDING_RESOURCE and three CORB classes. */
    DCHECK(req->destination != NULL,
           "a request parked on an ADDRESS without stating its DESTINATION — Fetch §2.2.5 Requests gives every "
           "request one (\"unless stated otherwise it is the empty string\"), and the trusted zone reads it to "
           "decide whether the reply may be ingested as CODE. A park that does not say would have its reply "
           "classified by whichever arm the zone happens to write first, which is how a cross-origin HTML body "
           "gets compiled. Name the destination at the component that built the request");
    /* The credentials mode, which decides whose session pays. §2.2.5 defaults it to `same-origin`, a positive
       answer a producer may state, so the zero (core/fetch/fetch.h's `FETCH_CREDENTIALS_UNPLACED`) is refused
       rather than turned into it. And the mode, whose only reader is §4.1 step 7's Integrity Policy disjunct
       below: a zero produces no wrong wire value and no abort, it silently refuses a `<script src integrity
       crossorigin>` a browser loads. Both are the consumer's half of the guards `fetch_owe` makes
       (core/fetch/fetch.c), for the parks in this file that build their own record and never pass that door. */
    DCHECK(req->mode != FETCH_MODE_UNPLACED,
           "a request parked on an ADDRESS without stating its MODE — Fetch §2.2.5 \"Requests\" gives every "
           "request one, and §4.1 step 7's Integrity Policy check reads it in a conjunction with the "
           "request's integrity metadata, so a park that does not say takes the arm that REFUSES a request "
           "carrying integrity metadata a browser loads, with nothing downstream to crash and no gate here "
           "that would notice. State the mode the algorithm creating this request names "
           "(core/html/cors_settings_attribute.h holds HTML §2.5.1's answer over an element's `crossorigin` "
           "state, and §2.5.4's note fixes the mode at `cors` for the keywords on its path)");
    DCHECK(req->credentials != FETCH_CREDENTIALS_UNPLACED,
           "a request parked on an ADDRESS without stating its CREDENTIALS MODE — Fetch §2.2.5 Requests "
           "gives every request one and the trusted zone decides from it whether the person's session pays "
           "for this fetch, so a park that does not say would have that answered by whichever arm the zone "
           "writes as its else. State the mode the algorithm creating this request names "
           "(core/html/cors_settings_attribute.h holds HTML §2.5.1's and §2.5.4's two answers over an "
           "element's `crossorigin` state, and they differ)");
    /* The address as the @H surface sees it, for the three parks whose reply becomes a program. Every other
       request is recorded by the component that built it (core/fetch/fetch.c, core/xhr/xml_http_request.c,
       core/html/html_form.c, core/html/html_image.c, core/html/html_link.c), which holds the method, headers and
       body. A program load holds none of that: HTML §4.12.1.1 hands this register an address and an element, and
       its producers (an injected `<script src>`, a document's own external script, a dynamic `import()`) all
       arrive here and nowhere else, so this is the only line that sees the set. (`<link rel=modulepreload>` is
       recorded by its own component; this names the loads themselves, preloaded or not.)
       The grade is the park's own PEND_PROV, read rather than recomputed: `engine_prov_of_running_path` can never
       answer `observed` (solver/engine.h), and `observed`'s first conjunct, §4.12.1.1's `parser document`, is the
       parser-inserted parameter `pending_push` already composed. A second computation could disagree.
       Recorded before steps 6 and 7, as core/html/html_link.c and core/fetch/fetch.c do theirs: the endpoint is
       what the page's code composed, so a policy refusing it or an upgrade rewriting it leaves a request the
       bundle can still make. `pkind` is read once for the gate and the door. */
    pkind = (int)pending_get_int(e, PEND_KIND);
    if (pending_kind_is_program(pkind)) {
        /* The grade and the mark are read once each, for the same reason as `pkind`. */
        int prov = (int)pending_get_int(e, PEND_PROV);
        int door = program_load_door(pkind, (int)pending_get_int(e, PEND_PARSER_INS));
        JSValue uv = JS_NewString(ctx, req->url);
        CHECK(!JS_IsException(uv), "engine: OOM naming a program load for the endpoint surface");
        /* The two readings of one park agree: `observed` is the parser-inserted mark and an unforced path
           (solver/pending.h's `PROV_OBSERVED`), and the door is the same mark read off the same field, so an
           `observed` park whose door is not `document-script` means the two compositions came apart. Both sides
           are this codebase's own, so this is a DCHECK. The converse does not hold: a `document-script` row is
           graded `forced` whenever its path stood on a contradicted arm. */
        DCHECKF(prov != PROV_OBSERVED || door == EPD_DOCUMENT_SCRIPT,
                "a program park graded `observed` states the @H door %d rather than `document-script` — the "
                "grade's first conjunct is HTML §4.12.1.1 \"Processing model\"'s `parser document` and this "
                "door is now that same mark read off the same register field, so the two compositions have "
                "come apart over one park of kind %d, and the row about to be emitted would say a markup "
                "parse cannot reach an address its own element carries", door, pkind);
        /* HTML §8.1.4.2 sets no method, so it is Fetch §2.2.5's `GET`; `req->method` is what the producer
           stated, already asserted present at this function's entry. */
        endpoint_record(ctx, req->method, uv, NULL, 0, NULL, prov, door);
        JS_FreeValue(ctx, uv);
    }
    /* Fetch §4.1 Main fetch step 6, run before the key is composed. The reply seam is keyed on (method, url) and
       step 6 rewrites the url, so a PEND_URL composed from the pre-upgrade address would park the request under
       an address the host is never asked for, and the entry would silently never settle. It is asked at this
       consumer as well as at the producers, because the parks in this file pass no producer's door; it is
       idempotent (core/fetch/fetch.h), so a producer having run it changes nothing. */
    up_url = fetch_main_upgrade(ctx, req->url, req->destination, req->initiator);
    use_url = up_url ? up_url : req->url;
    pending_set(e, PEND_URL, JS_NewString(ctx, use_url));
    pending_set(e, PEND_METHOD, JS_NewString(ctx, req->method));
    pending_set(e, PEND_DESTINATION, JS_NewString(ctx, req->destination));
    /* `fetch_credentials_token` owns the one wire spelling (core/fetch/fetch.h) and is fatal on the unplaced
       zero, the release backstop under the DCHECK above. Written unconditionally, so a record still carrying
       pending.h's JS_NULL is one this door never ran on. */
    pending_set(e, PEND_CREDENTIALS, JS_NewString(ctx, fetch_credentials_token(req->credentials)));
    if (req->headers && req->headers->n > 0) {
        JSValue hl = pending_list_new();
        int hi;
        for (hi = 0; hi < req->headers->n; hi++)
            pending_list_add_pair(hl, req->headers->e[hi].name, req->headers->e[hi].value);
        pending_set(e, PEND_HEADERS, hl);
    }
    if (req->body) pending_set_bytes(e, PEND_BODY, req->body, req->body_len);

    /* Fetch §4.1 Main fetch step 7, asked at the consumer. It precedes §4.3 (step 12), and every async request
       passes here: a `fetch()` through fetch_owe's provider edge, and the three program parks and the
       browser-algorithm subresource park (`<img>`, `<link>`) directly. Asked only at the request-building
       components, it would miss any park that builds its own record, as §4.12.1.1's fetch once did, leaving CSP
       §6.7.1.1 Script directives pre-request check unasked for `<script src>`.
       For requests that came through fetch_owe this is a second evaluation of the same policy:
       fetch_main_blocked is a pure function of (the document's policy container, URL, destination, metadata),
       and the record carries the metadata its builder was checked with, so the two cannot disagree.
       A blocked request is §5.6's network error, JS_NULL. For the three program kinds that is §4.12.1.1's "null
       (representing an error)": flow_deliver_one_reply's null arm marks the element's result null and "execute
       the script element" step 4 fires `error` at it, so a refused script reaches the page's `onerror`. */
    DCHECK(req->metadata.nonce != NULL && req->metadata.integrity != NULL,
           "a request reached the park stating NO §2.2.5 metadata — both fields are non-NULL in every legal "
           "value (core/frame/policy_container.h), so a NULL is a zero-filled FetchRequest whose producer "
           "never said which of the two claims it is making, and §4.1 step 7 below would compare a policy's "
           "nonce-source expressions against a pointer nobody wrote");
    /* Step 7 judges the address step 6 left, never `req->url`: judging the pre-upgrade address would refuse an
       `<img src="http://…">` on an https page that a browser upgrades and loads, and answer a network error
       under a key naming the upgraded address. */
    if (fetch_main_blocked(ctx, use_url, req->destination, req->metadata, req->mode)) {
        pending_set(e, PEND_VALUE, JS_NULL);
        pending_set(e, PEND_HAVE_VALUE, JS_TRUE);
        free(up_url);
        return;
    }
    /* Fetch §4.3 Scheme fetch, over the upgraded address (a copy of the `const` record with `url` replaced).
       Handing it the pre-upgrade address would let a `data:` or `blob:` arm answer for one address while the key
       names another, and send the trusted zone the mixed-content address step 6 replaces. §5.4's captured blob
       URL entry is JS_UNDEFINED: only the Request constructor captures one, and a Request reaches `fetch_owe`,
       which runs §4.3 with its own entry and a `deliver` closure before touching this register
       (core/fetch/scheme_fetch.h). */
    sf = *req;
    sf.url = use_url;
    switch (scheme_fetch(ctx, &sf, JS_UNDEFINED, &reply)) {
    /* §4.3's HTTP(S) scheme row, "Return the result of running HTTP fetch given fetchParams": the one outcome
       the host is shown. The entry stays outstanding until engine_provide answers it. */
    case SCHEME_FETCH_NETWORK:
        free(up_url);
        return;
    /* §4.3 built the whole response inside this agent. It is the same reply record a host delivers and goes
       where a host's goes, as the standard has it: `data:text/javascript,…` is a 200 whose body is the URL's. */
    case SCHEME_FETCH_RESPONSE:
        pending_set(e, PEND_VALUE, reply);
        break;
    /* §4.3's network error: a `data:` URL §6 refuses, a `blob:` the store does not name, `about:config`, a
       `file:` URL, and every scheme the switch does not list. JS_NULL is the network-error value. */
    case SCHEME_FETCH_NETWORK_ERROR:
        pending_set(e, PEND_VALUE, JS_NULL);
        break;
    }
    free(up_url);
    /* Last: `haveValue` takes the record out of the frontier's outstanding set (solver/pending.c's
       pend_index_sync), so the address and method must already be on it, or the host is offered a request
       nobody is waiting for. */
    pending_set(e, PEND_HAVE_VALUE, JS_TRUE);
}

/* Fetch-await parking: a host fetch registers its resolve capability on the running flow's own pending register,
   with the URL the host must fetch. A flow that awaits it suspends; when the reply arrives through
   engine_provide, each awaiting async body's reaction enqueues as a job in that flow's queue and it resumes.
   Until then the flow cannot finish, which keeps the reply-gated code reachable. The register is per flow so one
   flow's delivery never resolves another's fetch, which would route the reaction into the wrong COW delta. */
void engine_pending_fetch_url(JSContext *ctx, JSValueConst resolve, JSValueConst value, const FetchRequest *req) {
    Flow *f = flow_running();
    /* The rest of the request (headers, body) rides the record for the host that issues it: `safeFetch`
       decides SOP/CORS/method/credentials and cannot decide about a method it was not told. */
    const char *url = req ? req->url : NULL;
    JSValue e;
    /* A park needs a flow to park on: `f->pending` is per flow, so a caller with none has nowhere to be owed an
       answer or to resume. Script and every enqueued job have one; the pre-boot baseline does not, and it is
       where this engine performs the steps a browser performs during tree construction. core/dom/document.c's
       parsed-tree walk runs there, and HTML §4.2.4.3 Fetching and processing a resource from a link element ends
       in "Fetch request" with no task or microtask, so a document whose only content is
       `<link rel=preload as=font>` reaches this with no flow. (`<img>` does not: §4.8.4.3.5's update the image
       data queues a microtask before it fetches, and a queued job is a flow.) The fix belongs at the baseline
       entry: the walk may record an address but not issue a request; the boot flow issues it.
       core/html/html_link.c asserts that at its own baseline entry. */
    DCHECK(f != NULL, "engine_pending_fetch_url: a live fetch issued outside a running flow — see the comment "
                      "above this line: the callers that HAVE a flow are script and enqueued jobs, and the "
                      "caller that does not is the pre-boot baseline tree walk performing a step HTML defines "
                      "over tree construction. Issue it from the boot flow, not from the walk");
    e = pending_push(&f->pending, FLOW_PENDING_RESOLVE, flow_path_forced(f), flow_path_pinned(f),
                     /*parser_inserted*/0, flow_work_seq_next());   /* a `fetch()` or an XHR — page code composed it */
    pending_set(e, PEND_RESOLVE, JS_DupValue(ctx, resolve));
    pending_set(e, PEND_VALUE, JS_DupValue(ctx, value));
    /* Which document asked: the realm this chokepoint was entered with. `flow_step`'s realm is the session's,
       so a delivery re-deriving it would compile the reply in whichever realm the session is rooted at. It is
       carried rather than defaulted because `PEND_DOC`'s default 0 is a real document id. */
    pending_set_int(e, PEND_DOC, (int)document_doc(ctx));
    /* The address and what travels with it go through the one door, which also asks §4.3. Requests reaching
       here always get NETWORK: `fetch_owe` and `fetch()` run §4.3 with their own `deliver` closure and return
       before touching this register when it answers locally, so a local answer here means one of those seams
       stopped asking. With no address, the caller supplied the value up front and owes the host nothing. */
    if (url) pending_park_request(ctx, e, req);
    else pending_set(e, PEND_HAVE_VALUE, JS_TRUE);
    JS_FreeValue(ctx, e);
}

/* Parks a dynamic `import()`. The same register and URL as a fetch, with a different delivery: a module load is
   owed source text, so the delivery settles `resolve` with the reply's body rather than the reply record a
   `fetch()` makes a Response from. */
void engine_pending_module_url(JSContext *ctx, JSValueConst resolve, JSValueConst reject, const char *url) {
    Flow *f = flow_running();
    /* The destination is `script` (HTML §8.1.6.7.3 HostLoadImportedModule: "Let destination be `script`";
       loadState is undefined outside a worklet or service worker), which is what makes a chunk a code load at
       the chokepoint. It is stated on the record, not in a side register of specifiers that only one producer
       would fill. The method is `GET`. */
    FetchRequest req = { "GET", url, PENDING_DESTINATION_SCRIPT, NULL, NULL, 0,
                         /* placed below, off the element */ { NULL, 0, NULL, 0, CSP_PARSER_METADATA_UNPLACED },
                         /* …and so is the credentials mode, off the same element */
                         FETCH_CREDENTIALS_UNPLACED };
    /* Fetch §2.2.5's metadata, which for a dynamic import is the referrer's. HostLoadImportedModule gets "the
       descendant script fetch options given originalFetchOptions, url, and settingsObject", an algorithm of
       §8.1.4.2 Fetching scripts: its step 1 copies the options, so the cryptographic nonce is the importing
       script's (the element whose program this flow runs), and its step 2 resolves module integrity metadata
       from an import map, which this engine does not have, so integrity is the empty string. flow_dyn_el is
       NULL for a program no `<script>` produced (a lazy chunk, an @S candidate, a `javascript:` URL), whose
       options carry the empty string. */
    ScriptCspMeta meta;
    JSValue e;
    DCHECK(f != NULL, "a dynamic import was issued outside a running flow");
    meta = script_csp_meta(ctx, flow_dyn_el(f));
    req.metadata = meta.m;
    /* The credentials mode is the referring script's, copied by the same step (see
       `script_credentials_mode`); a row no `<script>` produced takes the default script fetch options. */
    req.credentials = script_credentials_mode(flow_dyn_el(f));
    /* The mode: HTML §8.1.4.2's fetch a single module script creates the request with mode "cors" outright,
       so this is the module arm and never the element's. */
    req.mode = script_request_mode(SCRIPT_TYPE_MODULE, flow_dyn_el(f));
    DCHECK(url != NULL && *url, "a dynamic import parked with no module URL for the host to fetch");
    e = pending_push(&f->pending, FLOW_PENDING_MODULE, flow_path_forced(f), flow_path_pinned(f),
                     /*parser_inserted*/0, flow_work_seq_next());   /* a dynamic `import()` — §16.2.1.8 is reached by RUNNING code */
    pending_set(e, PEND_RESOLVE, JS_DupValue(ctx, resolve));
    /* The reject half, which carries the load's failure: this is the only park whose failure is a rejection
       rather than an event at an element. HTML §8.1.6.7.3 HostLoadImportedModule's onSingleFetchComplete makes
       a null moduleScript "ThrowCompletion(a new TypeError)", and ECMAScript §13.3.10.3 ContinueDynamicImport
       calls "promiseCapability.[[Reject]]" with it. Without this the delivery could only settle `resolve` with
       empty source text, evaluating an empty module, so `import(u).catch(h)` would never run `h`. */
    DCHECK(JS_IsFunction(ctx, reject),
           "a dynamic import parked with no REJECT capability — JS_NewPromiseCapability mints both halves and "
           "this park is what the failure arm reaches the second one through, so a record without it could "
           "answer a network error only by settling the SUCCESS half with bytes that never arrived");
    pending_set(e, PEND_REJECT, JS_DupValue(ctx, reject));
    /* And which document asked, as every other fetch park records it: the importing realm is the asking
       document's, and HTML §7.5.11 step 2 cancels this load with that document's other fetches. */
    pending_set_int(e, PEND_DOC, (int)document_doc(ctx));
    /* And §4.3 Scheme fetch with it: `import("data:text/javascript,export default 1")` is answered from the
       specifier's own bytes, settling the promise JS_ModuleLoadPending is about to be handed. */
    pending_park_request(ctx, e, &req);
    script_csp_meta_free(ctx, &meta);
    JS_FreeValue(ctx, e);
}

/* Parks a browser algorithm's own subresource fetch: HTML §4.2.4.3 Fetching and processing a resource from a
 * link element's built link types, and HTML §4.8.4.3.5 Updating the image data. Same register, dedup and stall
 * accounting as `fetch()`; the delivery differs in one step: the reply record goes to that algorithm's
 * completion steps and nothing is queued as a program (solver/pending.h's FLOW_PENDING_RESOURCE).
 *
 * It is an entry rather than a flag on `fetch_owe`, the shape the three program parks have: `fetch_owe` serves
 * `fetch()`-shaped parks, and a park owed to another algorithm builds its own record. Everything `fetch_owe`
 * does before the provider, `pending_park_request` does at the consumer (Fetch §4.1 Main fetch steps 6 and 7,
 * §4.3 Scheme fetch, the method/url/destination/mode/credentials guards); the destination enumeration, the one
 * guard that lived only at that door, is asked below.
 *
 * A `data:` URL is not answered in place: `pending_park_request` writes the record and `haveValue`, and the
 * delivery calls the closure on the next pass, the same route as `import("data:…")`. HTML §4.6.8.12's step 14
 * steps and §4.8.4.3.5's are a fetch's processResponse, which no browser runs inside the issuing algorithm. */
void engine_pending_resource_url(JSContext *ctx, JSValueConst deliver, const FetchRequest *req) {
    Flow *f = flow_running();
    JSValue e;
    /* A park needs a flow. core/html/html_link.c asserts the same at its trigger: HTML §4.2.4.3 ends in "Fetch
       request" with no task or microtask, so parsed link elements are served inside a flow rather than from
       the baseline tree walk. The image half queues §4.8.4.3.5's own microtask first, and every job is a flow.
       Both ends assert, because only one of them can name the entry that arrived without one. */
    DCHECK(f != NULL, "a browser algorithm's subresource fetch was issued outside a running flow — it ends in "
                      "a fetch and a fetch parks on the FLOW's own register, so a caller with none has "
                      "nowhere to be owed a reply. core/html/html_link.c's trigger names the four entries "
                      "HTML §4.2.4.3 reaches it from");
    DCHECK(JS_IsFunction(ctx, deliver),
           "a browser algorithm's subresource fetch parked with no completion steps — HTML §4.2.4.3's types "
           "and HTML §4.8.4.3.5 each state theirs as a fetch's processResponse (HTML §4.6.8.12's step 14, "
           "HTML §4.6.8.20's), and a record without them owes its reply to nobody: the element would never "
           "fire `load` or `error` and the page would wait forever");
    /* The destination enumeration, the one check `fetch_owe` made that this path does not pass. The trusted
       zone reads the destination to decide whether the reply may be ingested as code, and its consumers do not
       fail alike on an undefined word, so it is checked while the building component is on the stack. `fetch`
       is a potential destination (Fetch §2.2.7 Miscellaneous), which HTML §4.6.8.20 step 3 translates to the
       empty string. */
    DCHECK(fetch_is_destination_type(req->destination),
           "a browser algorithm's subresource fetch stated a DESTINATION that is not one Fetch §2.2.5 "
           "\"Requests\" enumerates — run Fetch §2.2.7 \"Miscellaneous\"' translate a potential destination "
           "over the `as` keyword before stating it, as HTML §4.6.8.20's step 3 does");
    /* A browser algorithm's subresource: no `script` element, so no §4.12.1.1 `parser document`. */
    e = pending_push(&f->pending, FLOW_PENDING_RESOURCE, flow_path_forced(f), flow_path_pinned(f),
                     /*parser_inserted*/0, flow_work_seq_next());
    pending_set(e, PEND_RESOLVE, JS_DupValue(ctx, deliver));
    /* Which document asked, as in the parks above: `PEND_DOC`'s default 0 is a real document id. */
    pending_set_int(e, PEND_DOC, (int)document_doc(ctx));
    pending_park_request(ctx, e, req);
    JS_FreeValue(ctx, e);
}

/* Parks an injected script: `document.body.appendChild(s)` with `s.src` set. There is no promise to settle; the
   reply is more program. The flow parks on the URL as a fetch does (same register, dedup and stall accounting)
   and the delivery queues the body as this flow's next script, so the loaded code runs in the world that
   injected it (its COW delta, its pins, its BFS position); a sibling that never took the branch never sees it.
   The set of scripts that will execute as soon as possible parks the same way: a member has no position to hold
   (§13.2.7 waits for the set only before the load event), so its reply becomes a program whenever delivered. A
   script with a fixed position takes a slot instead (engine_queue_docscript_url).
   The park carries the element's type, because §4.12.1.1's "execute the script element" switches on it
   (`<script type=module src>` is how a bundle loads a chunk), and the element itself, which would otherwise be
   lost across the park: the classic arm sets §3.1.7's `currentScript` to it. The element crosses as its wrapper
   (solver/pending.h's `scriptEl`) because the record is made of JS values. */
static void engine_pending_script_url_into(Flow *f, JSContext *ctx, const char *url, ScriptType stype,
                                           lxb_dom_element_t *el, int parser_inserted) {
    /* §8.1.4.2's fetch: the type decides the decode and the evaluation entry, not the request. The method is
       Fetch §2.2.5's `GET`, and the destination is `script` for both types: "fetch a classic script" creates the
       request with `script`, and "fetch an external module script graph" fetches a single module script with
       `script` too. Either way the reply becomes a program, which is what CORB keeps cross-origin data out of. */
    FetchRequest req = { "GET", url, PENDING_DESTINATION_SCRIPT, NULL, NULL, 0,
                         /* placed below, off the element */ { NULL, 0, NULL, 0, CSP_PARSER_METADATA_UNPLACED },
                         /* …and so is the credentials mode, off the same element */
                         FETCH_CREDENTIALS_UNPLACED };
    /* §4.12.1.1's three metadata members, off the element, so `<script nonce=x src=…>` under
       `script-src 'nonce-x'` is checked by step 7 at the park. */
    ScriptCspMeta meta;
    JSValue e;
    DCHECK(f != NULL, "a <script src> was parked on outside a running flow");
    DCHECK(url != NULL && *url, "a <script src> was parked on with no URL");
    DCHECK(script_type_executes(stype),
           "a <script src> was parked on for an element whose type executes nothing — §4.12.1.1 fires an error "
           "event at an `importmap` or `speculationrules` element with a `src` and returns without fetching, "
           "so no such element ever reaches a park");
    meta = script_csp_meta(ctx, el);
    req.metadata = meta.m;
    /* The credentials mode, off the same element by §4.12.1.1's own step (see `script_credentials_mode`), so
       `<script crossorigin=use-credentials src>` is fetched with the mode it names. */
    req.credentials = script_credentials_mode(el);
    /* The mode, which `stype` decides besides the decode: §2.5.1's mode off the element for a classic script,
       `cors` for a module (see script_request_mode). */
    req.mode = script_request_mode(stype, el);
    e = pending_push(&f->pending, FLOW_PENDING_SCRIPT, flow_path_forced(f), flow_path_pinned(f),
                     parser_inserted, flow_work_seq_next());
    pending_set_int(e, PEND_SCRIPT_TYPE, (int)stype);
    /* Which document's program the reply will be: the realm this chokepoint was entered with is the document
       the element was inserted into, not whichever realm the session is rooted at. */
    pending_set_int(e, PEND_DOC, (int)document_doc(ctx));
    /* Whose element it is. Every caller reaches this from §4.12.1.1's steps over an element, so NULL is
       asserted rather than defaulted past. */
    DCHECK(el != NULL, "a <script src> was parked on with no element — §4.12.1.1 reaches this destination only "
                       "from `prepare the script element`, whose whole subject is EL, and a row without one "
                       "would run the fetched program with this document's currentScript left null");
    pending_set(e, PEND_SCRIPT_EL, node_wrap(ctx, lxb_dom_interface_node(el)));
    /* The address last, through the one door that asks §4.3 Scheme fetch: every field above belongs to the
       element, so an injected `<script src="data:…">` answered on this line lands in a record that already knows
       its type, document and element. */
    pending_park_request(ctx, e, &req);
    script_csp_meta_free(ctx, &meta);
    JS_FreeValue(ctx, e);
}

/* The running flow's park; a document's seed names the flow it is building (engine_seed_scripts). */
void engine_pending_script_url(JSContext *ctx, const char *url, ScriptType stype, lxb_dom_element_t *el,
                               int parser_inserted) {
    engine_pending_script_url_into(flow_running(), ctx, url, stype, el, parser_inserted);
}

/* A peer holds a reference into this document (engine.h's engine_set_referenced). It belongs to the instance,
   not a session: set once by the host that provisioned the instance, it survives every session boundary. It is
   read by flow_step (a timeline that may not finish) and by the slice (a frontier that may not be declared
   exhausted). */
static int g_referenced;

/* The browser layer's document-load lifecycle, asked when a flow has run everything the document gave it. */
static int (*g_docdone_hook)(JSContext *ctx);
void engine_set_document_done_hook(int (*fn)(JSContext *ctx))
{
    /* One claimant, and NULL is the release: document_init claims the slot once per agent and
       document_agent_free gives it back. A second claim on a one-entry slot would silently stop the first from
       running. */
    DCHECK(fn == NULL || g_docdone_hook == NULL,
           "a second component claimed §13.2.7's document-load lifecycle step — one call of it answers for "
           "EVERY document of this agent (it walks navigable_tree_order), so the claim is one per agent and was "
           "being re-made once per DOCUMENT until core/dom/document.c moved the line into its declaration");
    g_docdone_hook = fn;
}

/* The event loop's timer step, registered by the timer component so the scheduler does not depend on the browser
   half. Asked only where this flow has nothing else to run, the one moment virtual time may move (timer.h). */
static int (*g_timer_hook)(JSContext *ctx);
void engine_set_timer_hook(int (*fn)(JSContext *ctx))
{
    DCHECK(fn == NULL || g_timer_hook == NULL,
           "a second component claimed §8.1.7's timer step — there is ONE event loop per agent, and this was "
           "being re-claimed once per REALM until core/timing/timer.c moved the line into its declaration");
    g_timer_hook = fn;
}

/* The event loop's other clock-driven source: §8.1.7.3's in-parallel half, which queues an update-the-rendering
   task on the rendering task source when a navigable has a rendering opportunity. Registered by the rendering
   component, like the timer step. It is asked immediately before the timer step and yields to a timer that
   expires first, so the one virtual clock runs its two sources in the order their moments fall. */
static int (*g_rendering_hook)(JSContext *ctx);
void engine_set_rendering_hook(int (*fn)(JSContext *ctx))
{
    DCHECK(fn == NULL || g_rendering_hook == NULL,
           "a second component claimed §8.1.7.3's in-parallel half — there is one rendering task source per "
           "event loop, and the second claim silently decides every frame the first was queueing");
    g_rendering_hook = fn;
}

/* The event loop's idle rung: Cooperative Scheduling of Background Tasks §5.1 and §5.2, registered by the
   component that owns them. It is asked below both clock-driven sources because §5.1 places it there ("when it
   determines that the event loop is otherwise idle"). It is not a third clock: an idle period is due whenever
   nothing else is, so it asks the virtual clock nothing and defers to both by position alone. */
static int (*g_idle_hook)(JSContext *ctx);
void engine_set_idle_hook(int (*fn)(JSContext *ctx))
{
    DCHECK(fn == NULL || g_idle_hook == NULL,
           "a second component claimed the event loop's idle rung — there is ONE event loop per agent and its "
           "idle period is one per Window, so the second claim silently stops the first ever being asked and "
           "every registered idle callback goes uninvoked with nothing to say so");
    g_idle_hook = fn;
}

/* The clock boundary's arrival counts. `queue-rendering-opportunity`, `fire-due-timer` and
   `start-or-run-an-idle-period` count outcomes (raised only when the hook takes the step), so their zero means
   either "asked, nothing due" or "never reached"; these count the ask, ahead of the gate, to separate the two.
   They count arrivals rather than invocations (the hook's NULL test is inside), so the chain identity below holds
   whichever components registered, and a missing registration shows as asks above zero with its outcome at zero.
   Never reset: they are asserted against `g_step_unit_runs`, which nothing releases, so a per-session reset
   would break the identity (the scheduler's `g_orphan_asks`, cleared per session, is not the precedent here).
   Counters of different reset scope are not differenced against each other; read per-session values as the
   difference of two censuses. A report only: nothing branches on them. */
static long g_clock_render_asks, g_clock_timer_asks, g_clock_idle_asks;

/* One function per rung rather than a shared helper: the three are different rungs of one `else if` chain, and
   counting them separately is what lets the identity pin the chain's order. */
static int engine_clock_render_asked(JSContext *ctx) {
    g_clock_render_asks++;
    return g_rendering_hook != NULL && g_rendering_hook(ctx);
}
static int engine_clock_timer_asked(JSContext *ctx) {
    g_clock_timer_asks++;
    return g_timer_hook != NULL && g_timer_hook(ctx);
}
static int engine_clock_idle_asked(JSContext *ctx) {
    g_clock_idle_asks++;
    return g_idle_hook != NULL && g_idle_hook(ctx);
}

/* HTML §4.6.8.20 Link type "preload"'s browsing-context-connected time for elements a parse produced,
   registered by the link component. It is asked ahead of everything else a flow could do, by position
   (see the arm in flow_step). */
static int (*g_link_connected_hook)(JSContext *ctx);
void engine_set_link_connected_hook(int (*fn)(JSContext *ctx))
{
    DCHECK(fn == NULL || g_link_connected_hook == NULL,
           "a second component claimed §4.6.8.20's connected time for parsed link elements — the inventory it "
           "drains is one list per AGENT, so the second claim silently stops the first draining and every "
           "document's own preloads go unrequested with nothing to say so");
    g_link_connected_hook = fn;
}

/* The end of a microtask checkpoint (HTML §8.1.7.3), registered by the browser component that owns what HTML
   invokes there; see engine.h. Its one caller is Indexed Database §2.7.1's transaction cleanup. */
static void (*g_checkpoint_hook)(JSContext *ctx);
void engine_set_checkpoint_hook(void (*fn)(JSContext *ctx))
{
    DCHECK(fn == NULL || g_checkpoint_hook == NULL,
           "a second component claimed the end-of-microtask-checkpoint step — HTML invokes ONE list there and "
           "this slot holds one entry, so the second claim silently stops the first from running");
    g_checkpoint_hook = fn;
}

/* The session. The dispatch loop is a state machine its host steps, not a function that drains: the cooperative
   quantum yield returns after a slice so the one thread can pump its message port, stream findings and
   interleave other documents, then resume the byte-identical frontier. A loop run to exhaustion would freeze the
   extension's worker. The loop's state lives here, in one place, so a return between iterations costs nothing
   and every function can see all of it (a router asking whether a session is live reads g_sess_live). */
static JSContext *g_sess_ctx;
/* Which document the sequence below belongs to. A session is opened over the instance's root document
   (world.h's world_local_doc, the one the host named the agent for); every other program a flow runs names its
   own document, which is what lets a child navigable's scripts be work items of this frontier. It is the handle,
   not the realm, because a handle survives a park and a realm does not. */
static uint32_t g_sess_doc;
/* The root document's script inventory: a seed, not a sequence. Every flow's programs are rows of its own dyn
   table, and this is only the template each flow of this document is seeded from when created;
   engine_seed_scripts is its one reader. One address space is required by HTML §4.12.1.1, whose "immediately
   execute the script element el, even if other scripts are already executing" puts a program `eval`ed or
   injected from the second-to-last `<script>` at a slot between two document scripts.
   The address is resolved once, here: §4.12.1.1 encoding-parses `src` once per element, and both the fetch
   (§8.1.4.2) and a module's record identity must get that answer. Resolving per seed would ask again under
   whichever flow's delta is applied, so one timeline's `<base href>` would re-point another's bundle.
   `body` and `url` are independent (HTML §8.1.4.1 Scripts: the base URL is "the URL from which the script was
   obtained, for external scripts, or the document base URL of the containing document, for inline scripts").
   `body` is NULL until a reply carries the source, which parks a flow on the row; `url` is NULL for a `<script>`
   without `src`. Both set is an external script whose response the host already handed over; dropping its
   address would compile it under the document's name, which for a module is the module map key. A `refused`
   row (`src` failed to encoding-parse) holds the attribute text as its body. The strings are owned: built in
   engine_sched_begin, freed in engine_session_close. */
typedef struct RootScript {
    /* This row's source text, or NULL while the flow is still owed it. One buffer for the session, because the
       table is read by every base flow (engine_seed_root_flow is the seed hook: the boot flow, each cold-resumed
       replay, each @S candidate session) and a document's own bundle is the largest program this engine holds. */
    DynBody *body;
    char *url;           /* §4.12.1.1's encoding-parsed address, or NULL for a `<script>` with no `src` */
    ScriptType type;     /* which of §8.1.4.4 "Calling scripts"'s two algorithms evaluates it */
    /* §4.12.1.1's schedule (core/loader/script_type.h): an external member of the as-soon-as-possible set
       holds no position, so the seed parks on it instead of giving it a row. */
    ScriptSchedule sched;
    /* §4.12.1.1's "If url is failure": the element runs no script, and the page is owed an `error` event, which
       the DYN_SCRIPT_FAILED row this seeds fires. */
    int refused;
    /* The `script` element the row is the program of: §4.12.1.1's "execute the script element" switches on el,
       and its classic arm sets HTML §3.1.7 `currentScript` to it for the run. By the time the scheduler holds a
       body the element is behind it, and this is the last table that knows it. Borrowed: the tree outlives the
       session. It is never parked, because the cold tier stores a recipe and replays from the first script, so a
       node address is never asked to mean anything outside the session that read it. */
    lxb_dom_element_t *el;
} RootScript;
static RootScript *g_root_scripts;
/* The name the next row of any flow's sequence takes is minted by solver/flow.c's g_work_seq: one counter per
   instance (sibling arms mint independently, and a register entry a fork shares must resolve to one row in
   every arm), starting at 1 so 0, the register field's default, names no row, and shared with a flow's queued
   callbacks so a row and a job of one flow compare by when they were queued. */
static int g_root_n;
/* The seed's partition of `g_root_n`: rows that already have their source text (an inline `<script>`, or an
   external one whose response the host handed over) and rows that do not, which the seed turns into
   DYN_SCRIPT_SRC rows that park the flow on the reply door (or a park with no row, for an as-soon-as-possible
   member). `rootProgramsAwaitedAtSeed` is the reply-door openings the document owes for its own bundle: a run
   whose `replyAsked` equals it issued no page `fetch()`, XHR or dynamic `import()` and learned nothing.
   The converse does not hold: `replyAsked` also counts openings the markup named that are not program rows
   (a `<link>` stylesheet), so the inequality alone is not evidence of learning. Subtract the markup doors
   (`document-script`, `link-element`) shown in the door histogram; `fetch`, `xhr`, `module-import` and
   `beacon` are the run's.
   Both arms are written at the one line `g_root_n` is, keyed on the same `rows[i].body` column
   engine_seed_scripts branches on; the identity against `g_root_n` is asserted at engine_frontier_census. They
   are a constant fixed at seed (hence `AtSeed`), neither a gauge nor a lifetime counter: they do not fall as
   replies land. The live companion is `engine_rows_awaiting_bytes` (`rowsAwaitingBytes`), a per-flow walk,
   because a delivery rewrites each flow's own table. There is no inequality between the two: forks copy rows,
   so the gauge stands above the constant on a forking frontier and below it once a member is sold. */
static int g_root_n_held, g_root_n_awaited;
/* …and the third arm: elements whose `src` did not encoding-parse, seeded as the row that fires `error`. */
static int g_root_n_refused;
static Flow *g_sess_cur;
static int g_sess_live;
/* The finish owed to `g_sess_cur`, deferred across one return to the host. A member's last moment is the only one
   at which its world is complete, and it is inside flow_finish's caller: flow_finish unapplies the COW and DOM
   deltas (`cow_unapply`, `dom_unapply`), so after it there is nothing to render, and before it the host cannot
   be handed a turn. The finish is split across the return: the slice yields with the member's deltas applied,
   the host renders, and the next entry performs the finish. It is a flag rather than a second `Flow *`, since
   `g_sess_cur` already names the member, and the pair is asserted where consumed. Every path to
   `engine_session_close` (the park at the slice's entry and the two exits below its loop) runs through the
   consumer first; a host that never steps again has its session torn down whole. */
static int g_sess_finish_owed;

/* A synchronous request only the host can answer, which a cross-document read rides. Unlike a fetch it blocks:
 * `iframe.contentWindow.document.body` must answer on the line after the append, and the answer lives in another
 * instance, so the flow suspends as at an await or a loop back-edge, siblings run, and it resumes with the value.
 * The rendezvous is an id, not the request text: an answer is computed under the asking flow's world (two arms
 * that navigated a frame differently must not resolve to one Window), so answers are never shared and two
 * identical questions are two requests. */
static uint32_t g_next_req = 1;

/* How many synchronous requests this document has issued, and how many the host has answered. `blocked` and
 * `owed` are levels and cannot tell a host paying promptly from one that never pays (members keep arriving in
 * both); starvation is a rate, and these are the counts that have one. `hostAnswered: 0` beside `blocked: 6583`
 * says the flows are unpaid, not mis-ranked.
 *
 * Cumulative, never reset (see engine_session_close), and read only by the census. They count one population:
 * the census asserts `answered <= asked`, so an ask is counted at `mint_req` (every id the host will be shown,
 * including a fork's re-issue in engine_sibling_assemble), and a payment is the one delivery that settles an id
 * (a further peer timeline's answer goes to g_host_answers_extra). */
static long g_host_asked;
static long g_host_answered;
/* Answers that landed on a request already settled: a different population. A peer's document state is its
 * flows, so one rendezvous gets one true answer per peer timeline (engine_host_answer's `extra` arm records
 * each and an arm forks over it), but only the first settles the ask: it writes the value, clears the flow's
 * host-owed mark and makes the flow askable again. Folding these into `g_host_answered` would make a chatty peer
 * read as a host paying more than it was asked. Counted because it is the only number saying how many peer
 * timelines this instance was answered out of. */
static long g_host_answers_extra;
/* Answers that arrived when nothing was left to consume them. A peer answers by running a program on its own
 * timeline, relayed by a zone that cannot know what became of the asker, and Level-1 eviction makes a peer
 * outliving the asker's park the ordinary case. The asking flow is a recipe whose replay re-issues the request
 * and gets today's value, so a late answer is refused (engine_host_answer) and counted here: `hostAnswersLate`
 * climbing while `hostAnswered` does not is a host paying an instance that stopped listening. */
static long g_host_answers_late;
/* Requests this engine withdrew before anybody answered them (Fetch §2 Infrastructure's terminate a fetch
 * controller; see engine_host_terminate). A withdrawn id is neither a payment nor an unpaid ask, so it has its
 * own count rather than being folded into either side of the rate. Not reset at a session boundary. */
static long g_host_terminated;

/* The one place a rendezvous id is minted, and so the one place an ask is counted. Its two callers are a machine
   issuing a request and a blocked flow's fork re-issuing the one record it may not share. The wrap is a CHECK:
   the answer is routed by this number alone, so a reused id would deliver one flow's cross-document read into
   another flow's call site, in a world that never asked. */
static uint32_t mint_req(void)
{
    uint32_t id = g_next_req++;
    g_host_asked++;
    CHECK(g_next_req != 0, "the host-request id counter wrapped — an answer would be delivered to the wrong "
                           "call site, in a world that never asked the question");
    return id;
}

/* What a sold flow took with it, split by debt because the two settle through different doors: a fetch reply is
 * paired by (method, url) against the host's pending list, a synchronous request is routed by id. A shared count
 * would let a sold host-request entry absorb a real mispairing that main.c's provide-pairing assert exists to
 * catch. On the fetch side only what is still owed counts: an answered entry sold with a flow is a reply the host
 * already sent (pending.h, pending_owed_replies).
 *
 * A flow blocked on the host is the cheapest member to page (its recipe re-issues the request on resume), so the
 * tail is often a flow the host is mid-fetch for, and its reply arrives for a URL nobody is parked on. That is a
 * park between ask and answer, not a mispairing; it is counted so the pairing assert stays armed and is answered
 * once per reply the sale made unnecessary. */
static long g_paged_owed;
/* The request side, reported rather than spent: one request may still be owed N answers by N peer timelines, so
 * a per-entry credit would run out and abort under another door's message. It is stated in engine_host_answer's
 * abort and on the @COLD census line, and decides nothing. */
static long g_paged_reqs;

/* Flows this instance has sold. `live < flows` is equally the signature of flows finishing, and `_park`'s record
   count sums two record kinds, so neither answers "did it page"; this counter does. */
static long g_flows_sold;
/* The two populations it sums, split like `g_finished`. A parked @S candidate comes back without its ladder
   (solver/flow.h: `cand_surv` and `cand_rung` are re-execution observations that do not cross the tier), so
   selling one costs the search a measured distance that selling an exploration flow does not. */
static long g_flows_sold_flows, g_flows_sold_cands;
/* Whether the refusal edge was asked, which `sold` cannot say: `sold: 0` reads the same for a run that never met
   the RAM floor, one whose refusal arrived outside the safepoint (the reclaim is armed only across the flow
   step, so elsewhere it is declined and the frontier never consulted), and one that met the floor holding only
   the running flow (the pager's real end). Every engine_reclaim_tail call returns after exactly one of the two
   refusals or a sale of exactly one flow, so `asks == unarmed + floor + sold`, asserted at
   engine_frontier_census so an arm added later without a counter fires there. Instance totals like
   `g_flows_sold`, never per session: the question is whether the edge has ever fired. */
static long g_reclaim_asks, g_reclaim_unarmed, g_reclaim_floor;

int engine_take_paged_owed(void) {
    /* The credit counts entries the host was shown and has not answered (engine_reclaim_tail), and this is its
       only spender, so below zero means a second spender or one reply spending two credits; after that the
       number no longer means "replies owed for flows this instance sold". */
    DCHECK(g_paged_owed >= 0,
           "the paged-reply debt went NEGATIVE — it is a count of unanswered entries that left with sold "
           "flows and this is its only spender, so more replies have been excused as sales than were ever "
           "sold, and qjs_provide's pairing assert is now excusing the host's real mispairings");
    /* Zero is an answer, not a broken invariant: qjs_provide asks this to tell a sale from the host's pairing
       being off, and "no sale explains this reply" is the case its DFAIL exists for. Aborting here would report
       the host's mispairing under the pager's message. */
    if (g_paged_owed == 0)
        return 0;
    g_paged_owed--;
    return 1;
}

uint32_t engine_host_request(JSContext *ctx, const char *op) {
    Flow *f = flow_running();
    JSValue e;
    uint32_t id;

    DCHECK(f != NULL, "a synchronous host request was issued outside a flow — there would be nothing to "
                      "suspend and nothing to resume with the answer");
    DCHECK(op != NULL && *op, "a synchronous host request carried no text for the host to route on");
    /* A synchronous cross-agent read names a request id and no markup. */
    e = pending_push(&f->pending, FLOW_PENDING_HOSTREQ, flow_path_forced(f), flow_path_pinned(f),
                     /*parser_inserted*/0, flow_work_seq_next());
    pending_set(e, PEND_OP, JS_NewString(ctx, op));
    id = mint_req();   /* the ask half of the rate above, counted at the mint */
    /* Both names of one rendezvous, equal at the first ask and not the same field. `PEND_REQ` is what this
       returns, so the asking machine stores it in its step state, inside a frame a fork clones verbatim;
       `PEND_REQ_HOST` is what the host is shown and quotes back. At the first ask there is one world and one
       asker, so one `mint_req()` serves both and the ask is counted once. solver/pending.h's REQ_HOST says why a
       branch fork moves one and not the other. */
    pending_set_int(e, PEND_REQ, id);
    pending_set_int(e, PEND_REQ_HOST, id);
    JS_FreeValue(ctx, e);
    /* The machine's name is unique on its own register: engine_host_answered and engine_host_take search this
       flow only and return the first match, so two HOSTREQs under one name would hand one machine the other's
       answer, a wrong value rather than a crash. It holds by construction (`mint_req` is monotone; a fork copies
       the register entry for entry), and this fires the day a second write to PEND_REQ appears. Computed under
       the dev guard, outside the DCHECK, because `pending_count` interns an atom. */
#if APICLIENT_DEV
    {
        int dups = 0, n = pending_count(f->pending);
        for (int i = 0; i < n; i++) {
            JSValue a = pending_entry(f->pending, i);
            if (pending_get_int(a, PEND_KIND) == FLOW_PENDING_HOSTREQ &&
                (uint32_t)pending_get_int(a, PEND_REQ) == id) dups++;
            JS_FreeValue(ctx, a);
        }
        DCHECKF(dups == 1, "minting host request %u left %d entr(y/ies) on this flow's register under that "
                           "MACHINE name — engine_host_answered returns the first match, so a second would "
                           "hand one step machine another's answer with nothing to say so", id, dups);
    }
#endif
    /* The request is what makes the flow blocked. Every caller returns JS_STEP_YIELD, which the driver re-enters
       immediately and asks nothing of (quickjs.c's arm for it), so the yield is a park only because
       preempt_hook's clause (0) sees this entry on the register. A pending kind without pending_blocked's
       agreement would turn every cross-instance read into a flow that burns the thread until the host answers,
       and the host is asked only between steps, so the answer could never arrive. Asserted here, where the
       blocking is claimed. */
    DCHECK(flow_blocked(f), "a synchronous host request left its flow RUNNABLE — the caller is about to return "
                            "JS_STEP_YIELD, which re-enters immediately, so the flow would spin on a question "
                            "that is only answered between steps");
    return id;
}

/* Has the host answered `req`? The asking machine's re-entry read. The value is borrowed: it stays on the register
   until the machine takes it, so a re-entry that yields again can read it once more. */
int engine_host_answered(uint32_t req, JSValueConst *out) {
    Flow *f = flow_running();
    DCHECK(req != 0, "a host request with no id was asked about");
    DCHECK(f != NULL, "a host request was asked about outside a flow");
    for (int i = 0, n = pending_count(f->pending); i < n; i++) {
        JSValue e = pending_entry(f->pending, i);
        if ((uint32_t)pending_get_int(e, PEND_REQ) == req) {
            int have = pending_get_int(e, PEND_HAVE_VALUE) != 0;
            /* Borrowed: the register holds the value, so the reference taken here is released before the return
               and a machine re-entered before it consumes may read it again. */
            if (have && out) { JSValue v = pending_get(e, PEND_VALUE); *out = v; JS_FreeValue(pending_ctx(), v); }
            JS_FreeValue(pending_ctx(), e);
            return have;
        }
        JS_FreeValue(pending_ctx(), e);
    }
    /* Not on this flow's register: the machine asks about an id it never issued, or one issued before a fork
       re-issued it under the sibling's world. Answering "no" would park the flow forever. The two readings take
       opposite work (the machine composed an id its frame never wrote, or the scheduler left a sibling's frame
       naming the parent's id), so the dev abort lists the register: a HOSTREQ at a different id is the fork
       reading; no HOSTREQ, or an empty register, is the machine reading. */
#if APICLIENT_DEV
    {
        /* Composed at the call site, not in a helper: `-Wformat-truncation` fires only where the destination's
           size and the format literal are both visible. The loop stops on snprintf's return, so a long register
           truncates at an entry boundary and `shown` says so beside `n`. */
        char reg[512];
        int off = 0, shown = 0, n = pending_count(f->pending);
        for (int i = 0; i < n; i++) {
            JSValue e = pending_entry(f->pending, i);
            int w = snprintf(reg + off, sizeof reg - (size_t)off, "%s%u~%u/k%d%s", off ? " " : "",
                             (uint32_t)pending_get_int(e, PEND_REQ),
                             (uint32_t)pending_get_int(e, PEND_REQ_HOST),
                             (int)pending_get_int(e, PEND_KIND),
                             pending_get_int(e, PEND_HAVE_VALUE) ? "+v" : "");
            JS_FreeValue(pending_ctx(), e);
            if (w < 0 || w >= (int)(sizeof reg - (size_t)off)) break;
            off += w;
            shown++;
        }
        if (!off) snprintf(reg, sizeof reg, "(nothing)");
        DFAILF("a machine asked about host request %u and it is not on its flow's register — it either never "
               "issued that request or it inherited the id across a fork, which re-issues under the sibling's "
               "own world. THE REGISTER HOLDS %d entr(y/ies), %d of them listed as MACHINE~HOST/kKIND with +v "
               "where the host has already answered: %s. A machine name EQUAL to the asked one with a "
               "different host name beside it is the split working; the two DIFFERING is this flow never "
               "having issued that question. The KIND integers are the FLOW_PENDING_* defines in "
               "solver/pending.h and are printed as numbers deliberately — a name table for them here would "
               "be a second copy of that header's list and would drift from it. A HOSTREQ (k3) at a DIFFERENT "
               "id is the FORK reading; no HOSTREQ at all, or an empty register, is the MACHINE reading.",
               req, n, shown, reg);
    }
#else
    DFAIL("a machine asked about a host request that is not on its flow's register — it either never issued "
          "the request or it inherited an id across a fork, which re-issues under the sibling's own world");
#endif
    return 0;
}

/* The machine takes its answer and the request leaves the register. Separate from the read above because a
 * machine may be re-entered several times before it is ready to consume.
 *
 * This removal is what the two aborts at the bottom of engine_host_answer observe: one rendezvous has one true
 * answer per peer timeline, the first ends the entry, and a later answer reaches a register naming nothing.
 * What this line asserts is that the entry carries no answer nobody has forked over: flow_step takes one arm per
 * recorded extra before it resumes this frame (flow_answer_fork), so `extra` is empty at every healthy take.
 *
 * Named residual. Not covered: a peer with more than one timeline whose answers do not all arrive before the
 * asking flow resumes; later answers have nowhere to land. Next diff builds two things: (i) the suspension, cloned
 * here while the machine is still at the read and held with the entry (an arm forked later from `f->frame`
 * would carry answer k past a call site computed with answer 0); (ii) a release criterion the protocol lacks,
 * since `remoteop.answer` and `remoteop.retracted` never say every token holder has answered: one line of the
 * answering side's arithmetic (flow_answer_perform clears the row's token; `perform_token_held` already decides
 * the last-holder question for engine_retract_span), scoped to the ask, not the token's spelling. Absence shows
 * as: engine_host_answer's door (3) reached with `extra` and `fixed` both zero under a zone relaying on arrival. */
/* The row a taken cross-instance answer owes, defined with the other commitment writer because what a receiving
   timeline has heard from is one question with one set of rules. */
static void answer_commit_taken(JSContext *ctx, Flow *f, const char *world);

JSValue engine_host_take(JSContext *ctx, uint32_t req, int *pcompletion) {
    Flow *f = flow_running();
    JSValue v;
    DCHECK(f != NULL, "a host answer was taken outside a flow");
    DCHECK(pcompletion != NULL,
           "a host answer was taken without reading the COMPLETION TYPE it carries — an answer is a completion "
           "record and a taker that reads only its value delivers a peer's THROW as `undefined` to the call "
           "site that parked on it");
    for (int i = 0, n = pending_count(f->pending); i < n; i++) {
        JSValue e = pending_entry(f->pending, i);
        if ((uint32_t)pending_get_int(e, PEND_REQ) == req) {
            JSValue c;
            DCHECK(pending_get_int(e, PEND_HAVE_VALUE), "a host answer was taken before it arrived");
            /* Type and value are one write (engine_host_answer), so an answered request without a type is a
               delivery that went around this file; reading it as a normal completion would be a silent lie. */
            c = pending_get(e, PEND_COMPLETION);
            DCHECK(JS_IsNumber(c), "an answered host request carries no completion type");
            JS_FreeValue(ctx, c);
            *pcompletion = (int)pending_get_int(e, PEND_COMPLETION);
            /* No peer timeline is destroyed with the entry: every triple in `extra` is a real completion that
               no arm carries yet, and pending_remove frees the list with the record. flow_step drains them into
               arms before resuming this frame, so a non-empty list is that ordering broken. Counted on its own
               line in the dev guard because pending_extra_count interns field atoms. */
#if APICLIENT_DEV
            {
                int unforked = pending_extra_count(e);
                DCHECK(unforked == 0,
                       "a synchronous request is leaving the register still holding a PEER's further answers "
                       "— each of those is a timeline of the answering document that no arm carries, and "
                       "freeing the record frees them, so those timelines are explored by nobody and nothing "
                       "anywhere says so. flow_step forks one arm per extra before it resumes this frame, so "
                       "the fork and the take have come apart");
            }
#endif
            /* Which sending timeline this flow is now in, recorded where it becomes true: a peer's answer is a
               fact about the one flow of that document that computed it, so a receiver that consumes it may not
               hear from a contradicting timeline (the claim a routed delivery makes, by the same field;
               answer_commit_taken says why the arm differs). After the fork, which the assert above
               established, so no arm inherits this row and the push is a plain append. A zone-computed answer
               (§7.4 step 14's load, XHR §3.5.6's fetch) names no timeline and commits the flow to nothing;
               engine_host_answer asserts the missing name agrees with the answer's source. */
            {
                JSValue wv = pending_get(e, PEND_ANSWER_WORLD);

                if (JS_IsString(wv)) {
                    const char *w = JS_ToCString(ctx, wv);

                    CHECK(w != NULL, "engine: OOM reading which peer TIMELINE answered a cross-instance "
                                     "request — a commitment that cannot be written is a receiving timeline "
                                     "that will go on to accept a message from a peer arm it never took");
                    answer_commit_taken(ctx, f, w);
                    JS_FreeCString(ctx, w);
                }
                JS_FreeValue(ctx, wv);
            }
            v = pending_get(e, PEND_VALUE);   /* the caller's reference; the register's goes with the entry */
            JS_FreeValue(ctx, e);
            pending_remove(&f->pending, i);
            return v;
        }
        JS_FreeValue(ctx, e);
    }
    DFAIL("a host answer was taken for a request that is not on this flow's register");
    return JS_UNDEFINED;
}

int engine_host_take_completion(JSContext *ctx, uint32_t req, JSValue *presult) {
    int completion = ENGINE_COMPLETION_NORMAL;
    JSValue v = engine_host_take(ctx, req, &completion);

    DCHECK(presult != NULL, "a host completion was taken with nowhere to place its value");
    if (completion == ENGINE_COMPLETION_THROW) {
        /* Re-raised at the call site that parked on it, so the page's own `try`/`catch` around the read, write
           or call handles it as it would a local throw. JS_Throw consumes the value. */
        *presult = JS_UNDEFINED;
        JS_Throw(ctx, v);
        return JS_STEP_ABRUPT;
    }
    DCHECK(completion == ENGINE_COMPLETION_NORMAL,
           "a host answer arrived under a completion type ECMA-262 6.2.4 does not have — a cross-instance "
           "operation completes normally or throws, and `return`/`break`/`continue` do not cross a call site");
    *presult = v;
    return JS_STEP_DONE;
}

/* Withdraws the rendezvous; engine.h says which of Fetch §2 Infrastructure's two controller operations this is.
 *
 * It walks the frontier, not the running flow: its two callers are teardowns (a step machine's `fini` under an
 * exception unwind, and HTML §7.5.10 Destroying documents), where `flow_running()` need not hold the id. As in
 * engine_host_answer, an unanswered synchronous request is the one record a fork does not share
 * (engine_sibling_assemble unshares it and mints a fresh id), so exactly one register names the id.
 *
 * An answer already on the entry is discarded: a terminate can race a delivery, and the value's only reader is
 * gone. What is freed is a reply, never a work item; the flow keeps its snapshot, programs and place, and is
 * unblocked here. XHR §3.5.1 step 10 ("A fetch can be ongoing at this point") and §3.5.6's network-error response
 * discard a reply by name in the same way.
 *
 * A session that is over is refused before the walk, as a late answer is (g_host_answers_late): no scheduler is
 * left to pick the flow and no one will drain the notice. It is still counted. */
void engine_host_terminate(JSContext *ctx, uint32_t req) {
    int held = 0;

    DCHECK(ctx != NULL, "a host request was withdrawn with no context to free its register entry through");
    DCHECK(req != 0, "a host request with no id was withdrawn — the id is the only thing that names which "
                     "rendezvous is being given back, so a 0 withdraws nothing and leaves its flow blocked");
    if (!g_sess_live) {
        g_host_terminated++;
        return;
    }
    for (int k = 0; ; k++) { Flow *f = flow_at(k); if (!f) break;
        for (int i = 0, n = pending_count(f->pending); i < n; i++) {
            JSValue p = pending_entry(f->pending, i);
            /* Matched on the host's name, because `req` came from the host (pending.h's REQ_HOST). An answer
               fork's arm shares the issuer's host name (hence the ANSWER_FIXED guard below); a branch fork's arm
               has its own, so a withdrawal reaches exactly the rendezvous the host withdraws. */
            if (pending_get_int(p, PEND_KIND) != FLOW_PENDING_HOSTREQ ||
                (uint32_t)pending_get_int(p, PEND_REQ_HOST) != req) { JS_FreeValue(ctx, p); continue; }
            /* An arm's entry is not the issuer's: an answer fork leaves the arm holding the same id (in the step
               state of the frame it cloned) with its answer fixed, so withdrawing the first hit could unblock an
               arm and leave the issuer parked, silently at both ends. */
            DCHECK(!pending_get_int(p, PEND_ANSWER_FIXED),
                   "a rendezvous was withdrawn through the register of an ARM rather than the ISSUER — the arm "
                   "carries the issuer's id with its answer already FIXED, so this would unblock a flow that "
                   "was never waiting and leave the one that is");
            JS_FreeValue(ctx, p);
            pending_remove(&f->pending, i);
            /* The flow is askable again: a withdrawal is the host event flow_set_host_owed's mark waits for.
               Without this the entry would be gone and the flow still skipped at every pick. */
            flow_clear_host_owed(f);
            held = 1;
            break;
        }
        if (held) break;
    }
    g_host_terminated++;
    /* The zone is asked to stop the transfer: XHR §3.5.7 The abort() method's "Cancels any network activity",
       which only the trusted zone can do. A one-way notice: the engine's withdrawal is already complete, and a
       transfer that lands anyway is refused at engine_host_answer's ENGINE_ANSWER_HOST arm. Sent whether or not
       a register held the id, since the transfer is the zone's state. */
    {
        char rec[48];
        snprintf(rec, sizeof rec, "hostreq.terminate\t%u", req);
        engine_host_notify(ctx, rec);
    }
}

/* Delivers an answer, routed by id to the call site that asked; never broadcast like a fetched body, because it
 * was computed under the asking flow's world.
 *
 * A second answer to one question is a second peer timeline, not a duplicate. A peer's document state is its
 * flows, so a cross-agent operation is performed by every live timeline and each completes with its own true
 * answer (`otherW.length` has N answers for N timelines); engine_sibling_assemble gives a sibling the answering
 * program's row, token and all (flow.h's `dyn_token`), because a branch there is a real peer timeline. Taking
 * the first would pick a timeline; overwriting would pick the last after the machine may have read the first.
 *
 * Every peer answer therefore names the timeline that computed it (world_serialize's name, the one the asker
 * writes on its requests and the peer keys its segments on), so "another timeline answered" can be told from
 * "one answer relayed twice". A host answer names none (NULL): the trusted zone computed it and there is one,
 * which `source` states rather than anything sniffed from the request text.
 *
 * The answer is recorded here and the fork is taken in flow_step. This runs between scheduler steps, where
 * `flow_running()` names whichever flow was last switched in, so `cow_delta_fork` and `dom_cow_fork` would clone
 * a stranger's timeline. Writing the completion onto the entry is a property write the COW delta and cold tier
 * carry; flow_answer_fork builds the arm with that flow switched in.
 *
 * Returns 1 when the answer was written onto an entry of a flow that can still consume it, 0 when it was not
 * delivered. After a park the flows are still allocated (cold_park writes recipes; teardown frees them), so the
 * session-live test below is what keeps a found entry from counting as a delivery. */
int engine_host_answer(JSContext *ctx, uint32_t req, const char *world, JSValueConst value, int completion,
                       int source) {
    int extra = 0, fixed = 0;

    DCHECK(req != 0, "the host answered a request with no id");
    DCHECK(completion == ENGINE_COMPLETION_NORMAL || completion == ENGINE_COMPLETION_THROW,
           "the host answered a request with a completion type that is neither normal nor a throw");
    DCHECK(source == ENGINE_ANSWER_HOST || source == ENGINE_ANSWER_PEER,
           "an answer arrived without saying who computed it — a value this zone computed has exactly one "
           "answer and a peer's has one per timeline, and only the deliverer knows which this is");
    /* The two sources differ exactly in whether a timeline produced the answer, so the name is required of one
       and forbidden of the other. An anonymous peer answer could not be told from a duplicate. */
    DCHECK((source == ENGINE_ANSWER_PEER) == (world != NULL && *world),
           "a cross-instance answer disagreed with its own source about whether a peer TIMELINE computed it — "
           "a peer answers BY RUNNING A PROGRAM on one of its flows and must name that flow's world, and a "
           "value the trusted zone computed itself has no flow to name");
    /* Parsed only where the assert is compiled: world_parse interns the document names it reads, a side effect
       that may not sit in a DCHECK's condition, and release has no reader for the result. */
#if APICLIENT_DEV
    if (world) {
        WorldId w;
        const WorldId *anc;
        (void)world_parse(world, &w, &anc);
        /* The answering timeline belongs to a peer. A world of a document this agent holds is a routing loop
           that would deliver this instance's own state to a call site that asked another agent, and fork over a
           peer timeline that does not exist. Mirror of engine_perform's assert on the asking world. */
        DCHECK(!world_doc_hosted(w.doc),
               "a cross-instance answer was computed in a world of a document THIS agent holds — the operation "
               "was routed back to the instance whose flow asked it, so the value came from this heap and the "
               "arm forked over it would be a timeline of the asking agent wearing the peer's name");
    }
#endif
    /* The session is over, so nothing can consume an answer: a handled state, unlike the aborts at the bottom.
       The registers still exist (a park leaves the flows for teardown), so refusing before the walk is what
       keeps the entry, the host-owed mark and `hostAnswered` untouched. It is not a drop: the parked flow's
       recipe re-issues the request when resumed and gets today's value, which is why a peer outliving the
       asker's park is ordinary under Level-1 eviction. Counted in g_host_answers_late (@COLD census). */
    if (!g_sess_live) {
        g_host_answers_late++;
        return 0;
    }
    for (int k = 0; ; k++) { Flow *f = flow_at(k); if (!f) break;
        for (int i = 0, n = pending_count(f->pending); i < n; i++) {
            JSValue p = pending_entry(f->pending, i);
            /* Matched on the host's name: `req` is the id this zone was shown and quotes back. */
            if (pending_get_int(p, PEND_KIND) != FLOW_PENDING_HOSTREQ ||
                (uint32_t)pending_get_int(p, PEND_REQ_HOST) != req) { JS_FreeValue(ctx, p); continue; }
            if (!pending_get_int(p, PEND_HAVE_VALUE)) {
                DCHECK(!pending_get_int(p, PEND_ANSWER_FIXED),
                       "a request whose answer was FIXED by an answer fork is UNANSWERED — the arm is built from "
                       "the answer it took, so the two are written together and one without the other is an arm "
                       "that would wait for a question nobody was asked");
                pending_set(p, PEND_VALUE, JS_DupValue(ctx, value));
                pending_set(p, PEND_COMPLETION, JS_NewInt32(ctx, completion));
                /* The timeline is written with the value, so a flow that took an answer can address its next
                   operation to that timeline and refuse the same one arriving again. */
                pending_set(p, PEND_ANSWER_WORLD, world ? JS_NewString(ctx, world) : JS_NULL);
                /* `haveValue` last and through the register, because it is the write that unblocks the flow.
                   The register carries the count `pending_blocked` answers from, and a record cannot find the
                   registers naming it, so the walk that found the register states it. The generic `pending_set`
                   refuses this field on a synchronous record (solver/pending.h), so a path settling one without
                   a register crashes instead of leaving a flow blocked with its answer on it. */
                pending_answer_sync(f->pending, p);
                JS_FreeValue(ctx, p);
                /* The flow is askable again: this is the event a flow parked on a synchronous request waits
                   for, so the mark comes off here, on the flow the answer reached (flow.h). The first answer
                   stops at the first flow because an unanswered synchronous request is not shared by a fork
                   (engine_sibling_assemble unshares it and mints a fresh id), so exactly one register names it.
                   Sharing is never a reason for a per-flow clear to stop early: an arm that shared a record
                   would still need its own mark cleared. */
                flow_clear_host_owed(f);
                g_host_answered++;   /* the pay half of the rate above: every delivery that reached a flow */
                return 1;
            }
            /* A CHECK, not a DCHECK: everything below records another timeline's answer, so a second host
               answer reaching it would be recorded as an unnamed peer timeline and fork an arm into a timeline
               that does not exist. Not proceeding is required in release too. */
            CHECK(source == ENGINE_ANSWER_PEER,
                  "the trusted zone answered one request TWICE. A value this zone computed — §7.4 step 14's "
                  "load, XHR §3.5.6's fetch — has exactly one answer, so the second is a delivery this zone "
                  "made after it had already made one, and the flow has by now run on the first");
            /* An arm's own answer is fixed. The arm holds the same request id (in the step state of the frame it
               cloned), so without this skip a third answer would fork from the arm as well as the issuer, giving
               a timeline that answered B and then C at one call site. */
            if (pending_get_int(p, PEND_ANSWER_FIXED)) { fixed++; JS_FreeValue(ctx, p); continue; }
            /* The same timeline may not answer one question twice: a peer performs the operation once per live
               flow (engine_perform) and spends the token off the row as it answers (flow_answer_perform), so a
               second completion under one name is a relay that duplicated. Forking over it would grow a twin of
               an existing arm. It is asserted, never dropped, because dropping hides the relay defect. */
            DCHECK(!pending_answer_world_seen(p, world),
                   "one peer TIMELINE answered one cross-instance request TWICE. Its flow spends the rendezvous "
                   "token off its row as it answers, so this instance was handed the same completion twice by "
                   "the zone that routes them — and the arm this would fork is a second flow exploring a "
                   "timeline of the answering document that another arm already holds");
            /* An answer beyond the first does not stop at the first flow, and is not written onto a shared
               record: the asker and each branch sibling still holding the request must fork over it, each by
               draining its own list, so the record is unshared here per flow, as a branch fork does. */
            JS_FreeValue(ctx, p);
            p = pending_unshare(f->pending, i);
            pending_extra_add(p, completion, JS_DupValue(ctx, value), world);
            JS_FreeValue(ctx, p);
            flow_clear_host_owed(f);
            /* Not a payment: the first answer settled the ask, and this is another peer timeline answering. */
            g_host_answers_extra++;
            extra++;
        }
    }
    if (extra) return 1;
    /* The session is live and no live register took this answer. Three states reach here; each has its own
       message because they are observed differently. */

    /* (1) Every entry naming the request is an arm, skipped because its answer is fixed. The missing entry is
       the issuer's, which left at the take while its arms live on. Reachable for a peer only; a host answering
       an answered entry aborts one loop above. What to build is engine_host_take's residual: the entry alone is
       not enough, because flow_answer_fork clones `f->frame`, which by the take is past the read. */
    if (fixed)
        DFAIL("a PEER's answer arrived for a request whose only live entries are ARMS — each of those was "
              "forked over an earlier answer and its answer is FIXED, so the entry that could still take one is "
              "the ISSUER's, and that one left the register when the issuing flow took its first answer. This "
              "peer timeline has nowhere to land and the arm it would have forked is silently missing. Build "
              "the two things engine_host_take's residual names: the issuer's entry AND a clone of its "
              "suspension, taken at the take while the machine is still at the read (the frame is what the arm "
              "is a clone of, and after the take it is past the call site), released on a `remoteop.exhausted` "
              "record this protocol does not yet carry — the answering instance stating that the LAST timeline "
              "holding the token has answered, which is perform_token_held's question and the one "
              "engine_retract_span already asks to decide a hand-back");

    /* (2) Nothing names it, which for a value this zone computed is innocent: the asker is gone. */
    if (source == ENGINE_ANSWER_HOST)
        return 0;

    /* (3) For a peer's answer this is the one gap, reached through three doors: the asking flow took its first
       answer (engine_host_take removes the entry), finished, or was sold while the request was outstanding.
       The abort states the sale count, the one door this engine can speak to. The zone does not drop a live
       asker's rendezvous (bridge.js keeps the token until the instance is finalized).
       The first door is ordinary: a peer with two timelines, a zone relaying answer 0 on arrival (it cannot know
       how many timelines the peer has), the asker resumed and its entry taken, then answer 1 lands here. A driver
       batching a token's answers hides it, but the protocol permits relay on arrival. Doors 2 and 3 need no
       fix (a resumed recipe re-issues the request); door 1 is engine_host_take's residual. */
    DFAILF("a PEER's answer arrived for a request NO register holds — the asking flow left and took the "
           "entry with it (it took its first answer, it finished, or it was paged out), so this timeline "
           "of the answering document has nowhere to land and the arm it would have forked is silently "
           "missing. If the flow is still live it TOOK its first answer, and what to build is named at "
           "engine_host_take: hold the entry AND a clone of the suspension the machine is standing in, and "
           "release both on a `remoteop.exhausted` record — the answering instance saying the last timeline "
           "holding the token has answered — which this protocol does not carry, so there is no criterion "
           "for how long to hold them and holding for ever or for a bound are the only readings left. This "
           "session sold %ld flow(s) owing %ld synchronous request(s), which is the only one of the three "
           "doors this engine can still see from here", g_flows_sold, g_paged_reqs);
    return 0;
}

/* What the host still owes, as `id\top\n` records. Pulled each step like the pending URLs and not deduplicated:
   two identical questions from two flows are two questions, each answered under its own world. */
const char *engine_host_requests(void) {
    static char *join;
    static size_t cap;
    size_t n_out = 0;
    Flow *f;

    /* One pass with a growing buffer: converting a JS string's text allocates, so a measure-then-write pair
       would convert every record twice. */
    if (!join) { cap = 256; join = malloc(cap); CHECK(join, "engine: OOM joining the outstanding host requests"); }
    join[0] = 0;
    for (int k = 0; (f = flow_at(k)) != NULL; k++)
        for (int i = 0, n = pending_count(f->pending); i < n; i++) {
            JSValue p = pending_entry(f->pending, i);
            JSValue o;
            const char *s;
            size_t ol = 0;
            char idbuf[24];
            int idlen;
            /* "Can the host still be asked", not "is this flow still waiting", via the one exported predicate.
               `PEND_HAVE_VALUE` alone is also true of an entry the trusted zone refused (a refusal answers "will
               you make this request"), so a declined rendezvous would be re-joined, declined again, and abort
               at engine_host_decline's `!pending_entry_declined`. The address-keyed join applies the same filter,
               which keeps a decline a fork rather than a spin. */
            if (pending_get_int(p, PEND_KIND) != FLOW_PENDING_HOSTREQ ||
                !pending_entry_host_owed(p)) { JS_FreeValue(pending_ctx(), p); continue; }
            /* The host's name is what the host is shown. This buffer is the only place an id leaves the engine,
               so every id the zone can quote back comes from here; the machine's name would give the host two
               arms' identical names and land one answer in two contradictory worlds. */
            idlen = snprintf(idbuf, sizeof idbuf, "%u\t", (uint32_t)pending_get_int(p, PEND_REQ_HOST));
            o = pending_get(p, PEND_OP);
            s = JS_ToCStringLen(pending_ctx(), &ol, o);
            DCHECK(s != NULL, "an outstanding host request has no text — the host routes on it");
            while (n_out + (size_t)idlen + ol + 2 > cap) {
                cap *= 2;
                join = realloc(join, cap);
                CHECK(join, "engine: OOM growing the outstanding-request join");
            }
            memcpy(join + n_out, idbuf, (size_t)idlen); n_out += (size_t)idlen;
            memcpy(join + n_out, s, ol); n_out += ol;
            join[n_out++] = '\n';
            join[n_out] = 0;
            JS_FreeCString(pending_ctx(), s);
            JS_FreeValue(pending_ctx(), o);
            JS_FreeValue(pending_ctx(), p);
        }
    return join;
}

/* The one-way line. A notice is not on a flow's pending register: nothing waits on it, and the document it
   announces exists whatever becomes of the flow that created it. One buffer, appended to and drained whole. */
static char  *g_notices;
static size_t g_notices_n, g_notices_cap;

/* The other one-way line, an instrument's: a host-instrument program's dumped completion value. It is not the
   notices buffer, which carries platform events the trusted zone acts on (a navigable created, a world died),
   and not the result document, which carries the product's findings; a test artifact belongs in neither. One
   buffer, appended to and drained whole. */
static char  *g_dumps;
static size_t g_dumps_n, g_dumps_cap;

void engine_host_notify(JSContext *ctx, const char *op) {
    size_t len;
    (void)ctx;
    DCHECK(op != NULL && *op, "a host notice carried no text for the host to route on");
    DCHECK(strchr(op, '\n') == NULL, "a host notice contains a newline, which is the record separator — the "
                                     "host would read it as two notices and route the tail as an operation");
    len = strlen(op);
    if (g_notices_n + len + 2 > g_notices_cap) {
        size_t cap = g_notices_cap ? g_notices_cap * 2 : 256;
        char *g;
        while (cap < g_notices_n + len + 2) cap *= 2;
        g = realloc(g_notices, cap);
        CHECK(g != NULL, "engine: OOM recording a host notice — a dropped notice is a document the host never "
                         "provisions, so every read through it parks its flow forever");
        g_notices = g;
        g_notices_cap = cap;
    }
    memcpy(g_notices + g_notices_n, op, len);
    g_notices_n += len;
    g_notices[g_notices_n++] = '\n';
    g_notices[g_notices_n] = 0;
}

/* See engine.h. Two walks over one array, the first sizing and the second writing, so the size is a function of
   the fields. There is deliberately no format string: its `%s` count is checked against the argument list, never
   against the record's grammar, so a record could compile and still be one delimiter short. */
char *engine_notice_build(const char *op, const char *const *fields, size_t nfields)
{
    size_t len, at, i, l;
    char *rec;

    DCHECK(op != NULL && *op, "a host notice was built under no operation name — the operation is what the "
                              "host routes on, so a record without one is a payload addressed to nobody");
    DCHECK(nfields == 0 || fields != NULL,
           "a host notice was built from a field count with no fields behind it");
    DCHECK(strchr(op, '\t') == NULL && strchr(op, '\n') == NULL,
           "a host notice's operation name contains one of the record's own delimiters, so the name the host "
           "routes on is not the name this record was built under");
    len = strlen(op);
    for (i = 0; i < nfields; i++) {
        DCHECK(fields[i] != NULL,
               "a host notice field is NULL — every field of a record is present or it is a different record, "
               "and a value that is absent is stated in that field's own vocabulary (`u`, `null`, `none`) "
               "rather than left out, because an empty field is a hole a reader can default");
        DCHECK(i + 1 == nfields || strchr(fields[i], '\t') == NULL,
               "a host notice's NON-FINAL field contains a TAB and the record is tab-delimited — the reader "
               "counts one field too many and reads every field after it at the wrong offset, which is the "
               "one corruption a field COUNT still passes. Only the LAST field is the remainder that may "
               "hold a tab, which is why the raw policy is last on every record that carries one");
        DCHECK(strchr(fields[i], '\n') == NULL,
               "a host notice field contains a NEWLINE, which is the record separator — the host would read "
               "one notice as two and route the tail as an operation");
        len += 1 + strlen(fields[i]);   /* the TAB that precedes this field, then the field itself */
    }
    rec = malloc(len + 1);
    CHECK(rec != NULL, "engine: OOM building a host notice — a dropped notice is a document the host never "
                       "provisions, so every read through it parks its flow forever");
    at = strlen(op);
    memcpy(rec, op, at);
    for (i = 0; i < nfields; i++) {
        rec[at++] = '\t';
        l = strlen(fields[i]);
        memcpy(rec + at, fields[i], l);
        at += l;
    }
    rec[at] = 0;
    DCHECK(at == len, "a host notice was assembled to a length its own sizing walk did not predict — the two "
                      "walks read different bytes, so a field changed between them and the record is either "
                      "truncated or holds the tail of a buffer nobody owns");
    return rec;
}

/* The death of a world, announced (engine.h). One notice per name, written only here: two spellings of the
   record would be two peers releasing different things. */
void engine_notify_worlds_gone(JSContext *ctx, const char *const *names, int n)
{
    int i;

    DCHECK(n == 0 || names != NULL, "worlds were announced dead by a list that is not there");
    for (i = 0; i < n; i++) {
        /* "world.gone" + TAB + the name + NUL. Sized from the name for world_gone_push's reason: a document
           name nests one component per navigable depth, so a constant here is a cap on the frame tree. */
        size_t cap = strlen(names[i]) + 12;
        char *rec = malloc(cap);
        CHECK(rec != NULL, "engine: OOM announcing a world's death — a peer never told holds that world's "
                           "segment for the rest of its process, and cannot park while it does");
        snprintf(rec, cap, "world.gone\t%s", names[i]);
        engine_host_notify(ctx, rec);
        free(rec);
    }
}

/* See engine.h. The vector, not the bare name, because the resume hands it back to world_segment and the
   ancestry is half of how a segment is materialized; a head alone names a world the rebuild starts from the
   baseline. */
void engine_notify_worlds_parked(JSContext *ctx, const char *const *vectors, int n)
{
    int i;

    DCHECK(n == 0 || vectors != NULL, "the worlds a park carries were announced by a list that is not there");
    for (i = 0; i < n; i++) {
        size_t cap = strlen(vectors[i]) + 14;   /* "world.parked" + TAB + the vector + NUL */
        char *rec = malloc(cap);
        CHECK(rec != NULL, "engine: OOM announcing a foreign segment this park carries — the zone never told "
                           "holds no death for that world while this document is cold, so the instance that "
                           "resumes rebuilds a segment for a timeline that has ended and keeps it forever");
        snprintf(rec, cap, "world.parked\t%s", vectors[i]);
        engine_host_notify(ctx, rec);
        free(rec);
    }
}

/* What the hand-back did, counted at the retraction itself so the numbers agree with the notices that left.
   engine.h says what each one states. */
static long g_retract_flows, g_retract_started, g_retract_back;

void engine_retract_census(long *flows, long *started, long *handed_back)
{
    DCHECK(flows != NULL && started != NULL && handed_back != NULL,
           "the retraction census was asked to fill nothing");
    *flows = g_retract_flows; *started = g_retract_started; *handed_back = g_retract_back;
}

/* How many program rows across the frontier still carry a rendezvous token (engine.h). Scanned, for
   flow_owes_answer's reason. */
long engine_operations_started(void)
{
    Flow *f;
    int i, k;
    long n = 0;

    for (i = 0; (f = flow_at(i)) != NULL; i++)
        for (k = 0; k < f->dyn_n; k++)
            if (f->dyn_token[k]) n++;
    return n;
}

/* Is this question still somebody's to answer? Asked of every member of the frontier, in both places a question
 * lives: queued on the arrival slot, or started with its token on a program's row.
 *
 * This is why a hand-back is not per flow. One operation is attached to every live timeline (engine_perform),
 * but it is one question and the zone routes by token, so a notice sent while another timeline still holds it
 * would make the zone forget a token that timeline is about to answer under (the zone then asserts "a peer
 * answered a cross-agent operation under a rendezvous token this zone never minted"). The notice belongs to the
 * last holder leaving. Asked after the strip, so it needs no exclusion: an emptied flow answers no about itself.
 *
 * Scanned rather than counted, for flow_owes_answer's reason: a refcount would drift at the fork
 * (perform_q_fork shares entries, engine_sibling_assemble strdups the row's token), the start (the token moves
 * from the queue to the row) and the answer. */
static int perform_token_held(JSContext *ctx, const char *token)
{
    Flow *f;
    int i, k;

    for (i = 0; (f = flow_at(i)) != NULL; i++) {
        for (k = 0; k < flow_perform_pending(f); k++) {
            JSValue e = JS_GetPropertyUint32(ctx, f->perform_q, (uint32_t)k);
            JSValue tv = JS_GetPropertyUint32(ctx, e, 1);
            const char *t = JS_ToCString(ctx, tv);
            int hit;

            CHECK(t != NULL, "engine: OOM reading a queued cross-agent operation's token while deciding whether "
                             "a question may be handed back — a token that cannot be read is one this walk "
                             "would report as absent, and the hand-back would race a timeline still answering");
            hit = !strcmp(t, token);
            JS_FreeCString(ctx, t);
            JS_FreeValue(ctx, tv);
            JS_FreeValue(ctx, e);
            if (hit) return 1;
        }
        for (k = 0; k < f->dyn_n; k++)
            if (f->dyn_token[k] && !strcmp(f->dyn_token[k], token)) return 1;
    }
    return 0;
}

/* The distinct questions a hand-back has taken off its flows, collected during the strip so notices are decided
   once per question rather than once per holder (engine_retract_span: linear rather than quadratic). */
static void retract_seen_add(char ***seen, int *n, int *cap, const char *token)
{
    int j;

    for (j = 0; j < *n; j++)
        if (!strcmp((*seen)[j], token)) return;
    if (*n == *cap) {
        int c = *cap ? *cap * 2 : 8;
        char **g = realloc(*seen, (size_t)c * sizeof *g);
        CHECK(g != NULL, "engine: OOM collecting the operations a park hands back");
        *seen = g;
        *cap = c;
    }
    (*seen)[*n] = strdup(token);
    CHECK((*seen)[*n] != NULL, "engine: OOM naming an operation a park hands back");
    (*n)++;
}

/* Hands back every question a set of flows holds: one member (`only`), or every member (`only` NULL). The pager
 * sells one flow while the instance keeps running; the whole-frontier park writes the residue and leaves. `only`
 * selects members, never an implementation.
 *
 * A started operation is handed back like a queued one. Its partial work is its flow's own COW delta, which
 * leaves with the flow, so nothing it did outlives the park. What the peer is owed is a call (HTML §7.2.1.3.5
 * CrossOriginGet ( O, P, Receiver ) ends "Return ? Call(getter, Receiver)"), and an abandoned call has been made
 * zero times: the zone re-asks and the call is made once. Answering later is the ordinary answer, since the
 * value is read off the active document at the moment of the call (§7.2.2.2 Indexed access on the Window
 * object's length getter).
 *
 * A row keeps its kind and loses its token, the state flow_answer_perform leaves after an answer, so a row that
 * ran anyway aborts in flow_answer_perform naming the missing token.
 *
 * Strip, then decide: the last-holder question asked per holder would walk the frontier per flow (quadratic,
 * on frontiers of thousands); asked per distinct question it is one walk per outstanding peer operation, a
 * number independent of the frontier's size. */
static void engine_retract_span(JSContext *ctx, Flow *only)
{
    char **seen = NULL;
    int seen_n = 0, seen_cap = 0, i, k;
    Flow *f;

    for (i = 0; (f = flow_at(i)) != NULL; i++) {
        int n, held = 0;

        if (only && f != only) continue;
        /* The flow may not be the one holding the thread. Both callers switch out first (the park before it
           walks, the pager by construction); asserted because a switched-in flow could still run its program to
           completion against a row whose token was just returned to the zone. */
        DCHECK(f != flow_running(),
               "a cross-agent operation was handed back on the flow the scheduler is switched into — that flow "
               "can still run its program to completion, and the completion would find a row whose token this "
               "call has already given back");
        n = flow_perform_pending(f);
        for (k = 0; k < n; k++) {
            JSValue e = JS_GetPropertyUint32(ctx, f->perform_q, (uint32_t)k);
            JSValue tv = JS_GetPropertyUint32(ctx, e, 1);
            const char *tok = JS_ToCString(ctx, tv);

            CHECK(tok != NULL, "engine: OOM reading a cross-agent operation's token at the park — a question "
                               "handed back without its name leaves the flow that asked, in another instance, "
                               "suspended on an answer nothing will send");
            held = 1;
            retract_seen_add(&seen, &seen_n, &seen_cap, tok);
            JS_FreeCString(ctx, tok);
            JS_FreeValue(ctx, tv);
            JS_FreeValue(ctx, e);
        }
        if (n) {
            /* The queue is emptied, which keeps cold.c's assert a statement rather than a tolerance. Written in
               the engine's write bracket for perform_q_take's reason: this is the scheduler's record about a
               flow, written outside any flow's delta, and a delta that captured it would restore the entries
               when a sibling switched in. */
            cow_engine_write_begin();
            JS_SetPropertyStr(ctx, f->perform_q, "length", JS_NewInt32(ctx, 0));
            cow_engine_write_end();
            DCHECK(flow_perform_pending(f) == 0,
                   "a flow's operation queue survived the retraction that emptied it — the park is about to "
                   "abort on a question this instance has already told the zone it is handing back");
        }
        for (k = 0; k < f->dyn_n; k++) {
            if (!f->dyn_token[k]) continue;
            held = 1;
            g_retract_started++;
            retract_seen_add(&seen, &seen_n, &seen_cap, f->dyn_token[k]);
            free(f->dyn_token[k]);
            f->dyn_token[k] = NULL;
        }
        g_retract_flows += held;
        /* The postcondition, asserted where it is produced as well as where cold.c reads it: both disjuncts of
           flow_owes_answer are addressed above, so a flow still owing one holds a token in a third place. */
        DCHECK(!flow_owes_answer(f),
               "a flow still owed a peer an answer after every question it held was handed back — a rendezvous "
               "token survives somewhere that is neither the arrival slot nor a program's row, so this flow "
               "parks with a debt the zone has been told is returned");
    }
    for (k = 0; k < seen_n; k++) {
        /* The last holder, asked after the strip: every flow this call was given now holds nothing, so a hit
           is a member that stays and will answer under this token. */
        if (!perform_token_held(ctx, seen[k])) {
            size_t cap = strlen(seen[k]) + 20;   /* "remoteop.retracted" + TAB + the token + NUL */
            char *rec = malloc(cap);

            CHECK(rec != NULL, "engine: OOM announcing an operation a park hands back");
            snprintf(rec, cap, "remoteop.retracted\t%s", seen[k]);
            engine_host_notify(ctx, rec);
            free(rec);
            g_retract_back++;
        }
        free(seen[k]);
    }
    free(seen);
}

/* One member: the pager's shape. */
static void engine_retract_flow(JSContext *ctx, Flow *f)
{
    DCHECK(f != NULL, "a question was handed back on no flow at all");
    engine_retract_span(ctx, f);
}

/* See engine.h. With the other two park-time announcements, which together say what this instance hands over:
   the residue (world.parked), the names it will never use again (world.gone), and the questions it was asked
   and did not answer (this). */
void engine_retract_operations(JSContext *ctx)
{
    engine_retract_span(ctx, NULL);
}

/* The inbound half (engine.h). It is not engine_route: a delivery becomes a work item of every live timeline and
   a death ends one, so there is nothing to seed, no sender origin to stamp (no page code runs) and no target
   document (the zone broadcasts, because the sender does not track which peers a flow reached). */
void engine_world_gone(JSContext *ctx, const char *world)
{
    WorldId w;
    const WorldId *anc;
    int n_anc;

    DCHECK(world != NULL && *world,
           "a world-gone notice carried no name — world_parse would answer out of whatever this instance last "
           "parsed, and the segment released would be a live peer's timeline");
    n_anc = world_parse(world, &w, &anc);
    (void)anc;
    /* A death names one world. An ancestor is alive while any descendant is, so releasing a chain would drop
       the segments a running sibling arm's next arrival forks from, and the peer would silently materialize an
       empty one. */
    DCHECK(n_anc == 0,
           "a world-gone notice carried an ANCESTRY — a death names ONE world, and its ancestors are alive "
           "exactly while any descendant is; releasing them here drops the segments a live sibling arm's next "
           "arrival is told to fork from");
    DCHECK(!world_doc_hosted(w.doc),
           "a world-gone notice came back to the instance that minted the world — a local flow's world is the "
           "flow's own and this instance has never held a foreign segment for it, so the zone broadcast a "
           "death to its own sender");
    world_release(ctx, w);
}

/* The world field of a routed record, copied out NUL-terminated; caller frees. Every routed record is
   `<op>\t<doc>\t<worlds>\t<tail>` (engine_route takes the same fields apart), and a queued entry holds the whole
   text. */
static char *record_worlds_dup(const char *record) {
    const char *doc, *worlds, *tail;
    char *out;

    doc = strchr(record, '\t');
    DCHECK(doc != NULL, "a routed record named no target document");
    worlds = strchr(doc + 1, '\t');
    DCHECK(worlds != NULL, "a routed record carried no world vector");
    worlds++;
    tail = strchr(worlds, '\t');
    DCHECK(tail != NULL, "a routed record carried nothing after its transport fields");
    out = malloc((size_t)(tail - worlds) + 1);
    CHECK(out != NULL, "engine: OOM reading a routed record's world vector");
    memcpy(out, worlds, (size_t)(tail - worlds));
    out[tail - worlds] = 0;
    return out;
}

/* May this receiving timeline still hear from the world `vec`? The question the commitment record beside a
 * flow's delivery queue answers (flow.h's `deliver_world_q`). It is a fact about the flow, not the queue: what a
 * timeline has received constrains it, and a queue forgets that the instant it is consumed, so a pairwise check
 * over queued records would answer differently depending on whether the router pumped between two records.
 *
 * Two kinds of commitment refuse different things:
 *   - received (taken): this timeline heard from that world, so it may not hear from an arm that contradicts
 *     it; two arms of one sender branch delivered in sequence would be state merging on the receiving side.
 *   - foreclosed: this timeline is the arm minted where its parent took that world, so nothing at or under it
 *     is this timeline's; the parent receives it, so refusing here drops nothing.
 * Independent is not a refusal: two peers posting to one page are in two forests (world.h).
 *
 * It returns the census row rather than a boolean, because the two refusals defer to different flows (a sibling
 * deliver_fork_arm minted; the parent that took the subtree) and only the first can be absent. The row is the
 * arm flow_deliver leaves through (solver/step_unit.h names every arm); `STEP_UNIT_ROUTED_DELIVERY` is the
 * admitting answer and a real row. */
static StepUnit deliver_admits(JSContext *ctx, const Flow *f, const char *vec)
{
    int n = flow_world_commits(f), i;
    StepUnit verdict = STEP_UNIT_ROUTED_DELIVERY;

    /* The first refusing commitment decides, so a flow holding both a contradicting received world and a
       foreclosed subtree reports whichever was pushed first: the rows partition refusals, not reasons. */
    for (i = 0; verdict == STEP_UNIT_ROUTED_DELIVERY && i < n; i++) {
        JSValue e = flow_world_commit_at(f, i);
        JSValue cv = JS_GetPropertyUint32(ctx, e, 0);
        JSValue tv = JS_GetPropertyUint32(ctx, e, 1);
        JSValue av = JS_GetPropertyUint32(ctx, e, 2);
        const char *c = JS_ToCString(ctx, cv);
        FlowCommitArm arm;
        WorldRel rel;

        CHECK(c != NULL, "engine: OOM reading which sending timeline a receiving flow is in — a commitment "
                         "that cannot be read is a timeline about to accept the arm it did not take");
        DCHECK(JS_VALUE_GET_TAG(tv) == JS_TAG_INT,
               "a delivery-world commitment carried no small-integer RECEIVED/FORECLOSED flag — the two kinds "
               "refuse different things, so a missing one would be read as whichever the first test asked");
        /* The minting mechanism is asked of every row, not only the one the refusal reads: a row without one
           would read JS_UNDEFINED as 0, FLOW_COMMIT_ARM_NONE, and fire the refusal's assert naming the delivery
           seam for a defect that is the pusher's. */
        DCHECK(JS_VALUE_GET_TAG(av) == JS_TAG_INT,
               "a delivery-world commitment carried no small-integer minting mechanism — every row states "
               "which mechanism minted the flow on the other side of its branch (flow.h's FlowCommitArm), and "
               "a row without one is a producer that never stated it. Read as an integer it would come back "
               "FLOW_COMMIT_ARM_NONE and abort the refusal below, reporting a delivery-seam defect for what "
               "is a missing field at whichever flow_world_commit_push wrote this row");
        arm = (FlowCommitArm)JS_VALUE_GET_INT(av);
        rel = world_vec_relate(vec, c);
        if (JS_VALUE_GET_INT(tv)) {
            if (rel == WORLD_REL_CONTRADICT) {
                /* The arm that should take a refused record exists: asserted here, the one line that knows the
                   record is refused. On the delivery path a row is pushed iff `!deliver_commit_implied`, and an
                   arm is minted iff `world_vec_fork_point` answers and the same `!deliver_commit_implied` holds
                   (deliver_fork_arm puts its row on the sibling, leaving this list untouched in between), so for
                   those rows the arm exists iff the vector names an ancestor. That holds for one producer only:
                   answer_commit_taken also pushes received rows, for a mechanism that mints its arm later or
                   never. So the row states its own minting mechanism (flow.h's FlowCommitArm) and this reads it
                   rather than the vector's shape, which also keeps the condition side-effect free
                   (`world_vec_fork_point` allocates, `world_vec_relate` interns).
                   The arm still holds this record: it inherited this flow's delivery queue when `c` was consumed
                   (flow_deliver_fork), the queue is FIFO, and flow.c's release asserts that no flow leaves
                   holding a routed delivery unless its recipe went to the cold tier.
                   FLOW_COMMIT_ARM_NONE is today a root world, a flow created from the baseline for which the
                   delivery-time fork declined to mint; it is not spelled as one because a later mechanism may
                   reach "no flow on the other side" another way. Roots are minted by every
                   `flow_add*(…, WORLD_NONE)`: this file's boot flow, a joined document's boot flow (same forest),
                   solve.c's @S candidate session, and each member a cold resume rebuilds; read which minted the
                   pair before repairing one. A rebuilt member cannot be minted as a child of its parked world
                   (world_mint_child asserts `parent.session == g_session`; world_vec_relate answers independent
                   across sessions on purpose); see solver/cold.c's park_flow_add. */
                /* An arm owed rather than minted is its own answer. A cross-instance answer records the timeline
                   it took (answer_commit_taken); the arm for the other timeline is flow_answer_fork's, built only
                   if that timeline answers. Aborting would fire on a peer that correctly answered once, and
                   filing it as a delivery fork would claim an arm nothing built. It refuses under its own row; a
                   real loss is caught by the exhaustion assert over `_routedZeroDelivery`, which names this case. */
                if (arm == FLOW_COMMIT_ARM_ANSWER_OWED) verdict = STEP_UNIT_ROUTED_ARM_OWED;
                else {
                    DCHECK(arm != FLOW_COMMIT_ARM_NONE,
                           "a routed delivery CONTRADICTS a world this timeline already received, and the row for "
                           "that world states that NO mechanism minted a flow on the other side of its branch — "
                           "so the arm that should receive this record does not exist and no timeline of this "
                           "document will receive it. The row is the producer's own statement about its own run "
                           "(flow.h's FlowCommitArm), so this is the mechanism's answer and not a reading of the "
                           "vector: a vector edited to look forked does not move it. Today the only producer that "
                           "writes NONE is the delivery-time fork declining a ROOT sending world, which means the "
                           "two sending worlds are ROOTS of one (document, generation) and roots name each other "
                           "nowhere. A root is minted by every flow_add(..., WORLD_NONE) — the sending agent's "
                           "boot flow, a JOINED document's boot flow, an @S candidate session, and each member a "
                           "cold resume rebuilds — so read WHICH of those minted the pair in front of you; the "
                           "block above says why 'rebuild the parked flow as a child of its parked world' is not "
                           "the repair");
                    verdict = STEP_UNIT_ROUTED_NOT_MINE;
                }
            }
        } else if (rel == WORLD_REL_SAME || rel == WORLD_REL_DESCENDANT) {
            /* At or under a subtree this arm foreclosed: the parent that took it delivers it. Its own row,
               because that parent is above this flow and holds the record by construction, so this refusal can
               never be a lost message, while the arm above can. */
            verdict = STEP_UNIT_ROUTED_PARENT_TOOK;
        }
        JS_FreeCString(ctx, c);
        JS_FreeValue(ctx, cv);
        JS_FreeValue(ctx, tv);
        JS_FreeValue(ctx, av);
        JS_FreeValue(ctx, e);
    }
    return verdict;
}

/* Is a commitment to receiving `vec` already implied by this timeline's record? One fact with two callers: the
 * arm a delivery forecloses and the commitment it records are minted together or not at all.
 *
 * A world's ancestors are totally ordered, so pairwise-comparable received worlds form a chain, and a commitment
 * at or above `vec` refuses everything `vec` would; the row is redundant, and so is the arm ¬`vec`, since the arm
 * minted for the deeper commitment forecloses a subtree containing `vec`. Foreclosed rows are not read: being on
 * the far side of some branch says nothing about whether this is the first delivery from `vec`.
 *
 * The test is deliberately not "any commitment below the vector's nearest named ancestor": world_ancestry
 * filters the chain to worlds that have themselves crossed as a head, so the nearest named ancestor is the
 * nearest one that sent, not a fork point, and twenty siblings naming one ancestor is the ordinary shape of a
 * peer whose boot flow forks. Suppressing on that would retire an arm on an observation about the receiver's own
 * commitments, which falsifies nothing about the sender.
 *
 * Named residual. Not covered: with the nearest named ancestor standing in for a fork point, k mutually
 * contradicting worlds under it produce k+1 arms where the sender's forest has k leaves, so one arm is a timeline
 * no sender was in; uncertainty keeps it. Next diff builds: a vector naming a retired fork point exactly when its
 * other subtree has sent something (not an unfiltered chain, which world.h explains grows with branches taken
 * rather than fork depth); then world_vec_fork_point answers about a real branch and the arms collapse to the
 * sender's leaves. Absence shows as: the receiver's live flow count tracking the peer's distinct sent worlds,
 * and refusals growing as their product, because engine_route attaches every record to every live flow.
 *
 * `covering`, when non-NULL, receives the weakest arm among the covering rows (written only when the answer
 * is 1). Deciding to push a row needs the bit; deciding that an arm exists needs the arm, and a second producer's
 * row (an answer commitment) can cover a delivery. Several received rows may cover one vector, so the answer is
 * the weakest of them rather than the first match, which would depend on push order. */
static int deliver_commit_implied(JSContext *ctx, const Flow *f, const char *vec, FlowCommitArm *covering)
{
    int n = flow_world_commits(f), i, implied = 0;
    FlowCommitArm arm = FLOW_COMMIT_ARM_DELIVERY_FORK;

    for (i = 0; i < n; i++) {
        JSValue e = flow_world_commit_at(f, i);
        JSValue cv = JS_GetPropertyUint32(ctx, e, 0);
        JSValue tv = JS_GetPropertyUint32(ctx, e, 1);
        const char *c = JS_ToCString(ctx, cv);
        WorldRel rel;

        CHECK(c != NULL, "engine: OOM reading a receiving timeline's commitment while deciding whether a "
                         "routed delivery is the first this timeline has had from that sending world");
        DCHECK(JS_VALUE_GET_TAG(tv) == JS_TAG_INT,
               "a delivery-world commitment carried no small-integer RECEIVED/FORECLOSED flag — a FORECLOSED "
               "row read as RECEIVED here would suppress the arm for a world this timeline has never heard "
               "from, which is the message that then reaches no timeline at all");
        rel = world_vec_relate(vec, c);
        if (JS_VALUE_GET_INT(tv) && (rel == WORLD_REL_SAME || rel == WORLD_REL_ANCESTOR)) {
            /* The covering arm is read only for a caller that asked, and that scope is what makes the assert
               below true. A root sending world's row carries no arm (deliver_fork_arm declines to mint where
               there is no branch) and covers itself: a second message from one root is WORLD_REL_SAME against
               it, so demanding an arm on every covering row would abort an ordinary repeated delivery. The one
               caller that wants the arm, deliver_fork_arm, has already returned for a root before it asks. */
            if (covering) {
                JSValue av = JS_GetPropertyUint32(ctx, e, 2);
                FlowCommitArm row_arm;

                DCHECK(JS_VALUE_GET_TAG(av) == JS_TAG_INT,
                       "a delivery-world commitment that COVERS an arriving world carried no small-integer "
                       "minting mechanism — the covering row is what says whether an arm for this world "
                       "already exists, so a missing field would be read as FLOW_COMMIT_ARM_NONE and report "
                       "that no timeline can take the record this row is about to suppress the arm for");
                row_arm = (FlowCommitArm)JS_VALUE_GET_INT(av);
                JS_FreeValue(ctx, av);
                DCHECK(row_arm != FLOW_COMMIT_ARM_NONE,
                       "a RECEIVED commitment COVERS an arriving sending world for a caller that is about to "
                       "suppress the arm it would otherwise mint, and that row states that NO flow is on the "
                       "other side of its own branch. A row with no arm is the delivery-time fork declining a "
                       "ROOT world, and the only caller that reads this answer has already returned for a "
                       "root before asking — so the arriving world names a fork point while the row covering "
                       "it names none, and suppressing here would leave the sibling subtree with no timeline "
                       "at all. Read which producer wrote the covering row: a NONE it did not take from "
                       "world_vec_fork_point is a mechanism naming a branch that was never there");
                if (row_arm == FLOW_COMMIT_ARM_ANSWER_OWED) arm = FLOW_COMMIT_ARM_ANSWER_OWED;
            }
            implied = 1;
        }
        JS_FreeCString(ctx, c);
        JS_FreeValue(ctx, cv);
        JS_FreeValue(ctx, tv);
        JS_FreeValue(ctx, e);
    }
    if (implied && covering) *covering = arm;
    return implied;
}

/* The per-record half of the delivery census, declared here because engine_route registers the arrival and the
   ledger lives beside the two counters it cannot be derived from. */
static void routed_rec_arrived(const char *record);

/* The inbound half of the one-way line: a record the trusted zone routed to this instance because it holds the
 * named document. It is the exact text the sending instance emitted as a notice, plus the sender's origin, which
 * only that zone may stamp (SECURITY.md: an origin the untrusted engine computed for a foreign message is a
 * forgery every `event.origin` check would trust).
 *
 * It becomes a work item on the one frontier: a delivery seeds, in the receiver, a world of receiver baseline ∧
 * the sending flow's vector, where the receiver baseline is the receiving document's own timelines (the page's
 * `message` listener was registered by a script, so it lives in some flow's delta, not the pre-boot baseline).
 * So this attaches the record to every live timeline and returns. Delivering here would run page code under
 * whichever flow was last switched in. The frontier is the inbound queue, so the delivery is parkable, rankable
 * and resumable like every flow.
 *
 * The segment is the world registry's: a world minted elsewhere has one segment here (what it has written in
 * this instance), materialized on first arrival by forking the nearest ancestor this instance holds, which is
 * what the record's ancestry is for. Asking for it now lets a later arrival from a world forked off this one
 * inherit these writes; the registry owns it for the world's life. */
void engine_route(JSContext *ctx, const char *record, const char *sender_origin)
{
    char *dup, *doc, *worlds, *tail;
    int n;

    DCHECK(record != NULL && *record, "a routed record carried no text to route on");
    DCHECK(sender_origin != NULL && *sender_origin,
           "a routed record arrived without the sender's origin — only the trusted zone can stamp one, and a "
           "delivery without it would have to invent the one field a page's check is written against");
    /* Two ways a session ends, two different failures, two asserts, told apart by whether the frontier was
       written out. Parked: the frontier is recipes and this delivery is in none of them. Unlike an owed
       request, which the asking code re-issues on replay, nothing in this document re-sends someone else's
       message, so it must park as what it is or be refused while a park is in flight (cold.c's per-flow park
       asserts the same pair for a delivery a flow holds). */
    DCHECK(g_sess_live || !engine_frontier_paged(),
           "a record was routed into an instance whose frontier was PARKED — the session is closed and its "
           "flows are recipes, so there is nothing to seed the delivery on and no replay that re-derives it: a "
           "message is not re-sent by the document that receives it. Build the inbound half of the cross-"
           "instance park — the record and the sender origin the trusted zone stamped are both TEXT and cross "
           "as text — or have the trusted zone hold the record until the instance it names is resumed");
    /* Drained: every timeline finished, so the document can no longer receive. engine_set_referenced exists to
       prevent that (a document a peer holds a WindowProxy for keeps its last timeline waiting), so reaching here
       means nothing told this instance it was still referenced. */
    DCHECK(g_sess_live,
           "a record was routed into an instance whose frontier had DRAINED — every timeline of the document "
           "finished, so there is no flow to seed the delivery on and no scheduler to run it. A document a peer "
           "can still post to is a REFERENCED document and engine_set_referenced holds its last timeline open "
           "for exactly this, so reaching this line means the zone that PROVISIONED this instance did not say "
           "that a peer holds a WindowProxy for its document — which is the one fact this engine cannot see for "
           "itself and the one the routing table has by construction");

    /* Every routed record's first two fields are the transport's: the target document (which instance) and the
       sending flow's world (whose timeline). The rest belongs to the component the op names, so a second op is
       routed without a second router. */
    dup = strdup(record);
    CHECK(dup != NULL, "engine: OOM routing a record");
    doc = strchr(dup, '\t');
    DCHECK(doc != NULL, "a routed record named no target document — the trusted zone routes on that field, so a "
                        "record without one could not have arrived at this instance on purpose");
    *doc++ = 0;
    worlds = strchr(doc, '\t');
    DCHECK(worlds != NULL, "a routed record carried no world vector — the delivery would belong to no timeline");
    *worlds++ = 0;
    tail = strchr(worlds, '\t');
    DCHECK(tail != NULL, "a routed record carried nothing after its transport fields — there is no delivery in it");
    *tail = 0;   /* only to bound the vector for the checks here; flow_deliver re-splits its own copy */

    /* Routed to the wrong instance is the trusted zone's bug, and delivering anyway would fire a message the
       page never received. Which of this agent's documents it names is not decided here: an instance is an
       origin-keyed agent cluster with several documents, and the delivering realm is selected where the
       delivery runs (flow_deliver), since a hosted navigable's realm is materialized by whichever flow first
       reads through it (navigable.h). */
    DCHECK(world_doc_hosted(world_doc_intern(doc)),
           "a record was routed to an instance that does not hold the document it names — the offscreen is the "
           "only zone that knows which instance holds which document, and it sent this one to the wrong place");
    {
        WorldId w;
        const WorldId *anc;
        int n_anc = world_parse(worlds, &w, &anc);
        /* Against every document this agent holds, not its root: a message from a same-origin child is
           delivered in this heap and never reaches a transport. */
        DCHECK(!world_doc_hosted(w.doc), "a record was routed back to the instance whose flow sent it — a "
                                         "message to one's own document is delivered locally and never leaves");
        /* Receiver flow world ∧ sender vector, as two real objects: the delta of the flow that will make the
           delivery, and this instance's segment for the sender's world. Both are chains rooted at this baseline
           and neither is an ancestor of the other, so their conjunction is a join, which a CowDelta (a linear
           chain of frozen segments under one head) cannot express. It is the identity when the segment is empty,
           the case for a world that has only posted; that is asserted because it stops holding once this
           instance runs page code under a foreign world. The segment is materialized on its own line, outside
           the DCHECK, so release and dev builds create the same segments. */
        CowDelta *seg = world_segment(ctx, w, anc, n_anc);
        (void)seg;
        DCHECK(cow_delta_empty(seg),
               "the sending world has WRITTEN in this instance, so the delivery's world is the receiving flow's "
               "timeline conjoined with those writes — a JOIN of two deltas both rooted at this baseline, "
               "neither an ancestor of the other, which the linear delta chain cannot express: stacking the "
               "segment over the flow would overwrite the flow's own value in any slot both touched, and the "
               "unapply restores the BASELINE rather than the flow's value, destroying what the flow recorded "
               "with nothing to say so. BUILD THE JOIN — a merge of the two entry lists where they touch "
               "disjoint slots, and a CONTRADICTION where they touch the same one (two timelines that disagree "
               "about a slot conjoin to no timeline, so there is no delivery to make in that world)");
    }
    free(dup);

    /* The message arrives in every timeline of the receiving document. A delivery seeded as a fresh flow from
       the baseline would find no `message` listener (it was registered in some flow's delta) and fail silently.
       Attached rather than delivered here because this runs between scheduler steps: each flow delivers when it
       next steps, under its own delta, and the resulting task lands on its own queue.
       Registered before attaching, so a record whose attach loop aborts is still on the ledger as one no
       timeline admitted. */
    routed_rec_arrived(record);
    n = flow_count();
    DCHECK(n > 0, "a message was routed to a document whose every timeline had already finished — a document "
                  "that can still receive is not done, so the receiving flows must stay live while a peer holds "
                  "a WindowProxy for them: build that before this can be delivered");
    for (int i = 0; i < n; i++) {
        Flow *f = flow_at(i);
        /* Appended, because two messages from one world are sequential: HTML §9.3.3 Posting messages ends the
           window post message steps by queueing a global task on the posted message task source, and §8.1.7.1
           Definitions gives a task a source to "group and serialize related tasks", so the page observes one
           sender's posts in order. Senders whose worlds contradict are not refused here: this runs between
           scheduler steps and can neither fork nor read a timeline, so each timeline decides at its own delivery
           step (flow_deliver) which arm is its own and mints the sibling that takes the other, reading what it
           has received off the timeline (flow.h's `deliver_world_q`). */
        flow_deliver_push(ctx, f, record, sender_origin);
        /* And it is askable again: a host-owed flow is out of the pick until the host does something for it,
           and this record is that (flow.h). A delivery attached to a flow the scheduler will not pick is a
           message the document never receives. */
        flow_clear_host_owed(f);
    }
}

/* The cold tier's half of that; engine.h says why registration and push are one entry. The order is
   engine_route's, for engine_route's reason. */
void engine_routed_rebuilt(JSContext *ctx, struct Flow *f, const char *record, const char *sender_origin)
{
    DCHECK(f != NULL, "a parked routed delivery was rebuilt onto no flow — an 'm' record belongs to the flow "
                      "written immediately before it, so one with no flow names nothing to deliver to");
    DCHECK(record != NULL && *record && sender_origin != NULL && *sender_origin,
           "a parked routed delivery was rebuilt missing its record or the sender ORIGIN the trusted zone "
           "stamped — both cross the tier as TEXT, so an absent one is a residue this reader may not complete "
           "by inventing the field every `event.origin` check in every bundle is written against");
    routed_rec_arrived(record);
    flow_deliver_push(ctx, (Flow *)f, record, sender_origin);
}

/* Which realm of this agent answers for a document a peer names. An instance is an origin-keyed agent cluster
 * (SECURITY.md) with several documents, and `event.source` names a child navigable's document as often as the
 * root's; answering in the root's realm would hand back the root's `length` as the child's. HTML §7.3: "the
 * navigable whose active document is node's node document", so a document identifies exactly one. Asked where the
 * work runs, never where the record arrived, because a hosted navigable's realm is materialized by whichever flow
 * first reads through it (navigable.h).
 *
 * The caller's site travels with the ask: the `doc_realm` macro below expands at the call, so every abort here
 * renders the asking `file:line` into its reason (solver/cow.h's `_at` convention), while the record's `at` field
 * still names the check. Both parameters are required, never defaulted. All callers call directly; a forwarder
 * added later threads the pair it was given and never spells __FILE__ here. */
static JSContext *doc_realm_at(uint32_t doc, const char *file, int line)
{
    JSContext *realm;

    /* At every macro-expanded call this folds to true and is compiled out; its live population is a
       hand-written call that bypasses the macro. It sits outside the dev partition because in release a DCHECK
       is `(void)sizeof(cond)`, which still references `file` and `line` and keeps -Wextra quiet about them. */
    DCHECK(file != NULL && line > 0,
           "doc_realm_at was reached with no caller site — the `doc_realm` macro spells __FILE__ and __LINE__ "
           "at the call, so a null file is a hand-written call that bypassed it, and an abort below would then "
           "name THIS function for a sixteen-caller lookup, which is the defect this entry exists to end");
    realm = window_proxy_realm_of_document(doc);

    /* The answer is this timeline's: window_proxy_realm_of_document reads the flow's own navigables. It never
       materializes a realm, because three callers sit inside DCHECK conditions and a realm built only in dev
       builds would be a side effect. A navigable answers for its active Document, so in a timeline that
       navigated, the outgoing document's name answers NULL from the moment window_proxy_navigate moves `doc`;
       programs still queued for it are the residue core/frame/navigable.c's teardown assert names, discharged
       by §7.5.10 step 5's removal of a destroyed document's queued tasks. A sibling that did not navigate still
       answers. A NULL is partitioned by window_proxy_document_state, and each arm names the action its state
       admits. Destroyed is its own state: §7.5.10 Destroying documents sets the browsing context to null and
       window_proxy_set_destroyed nulls the realm, leaving a navigable with no active document, which a
       navigation never does (window_proxy_navigate refuses a destroyed navigable by name). */
#if APICLIENT_DEV
    if (realm == NULL) {
        switch (window_proxy_document_state(doc)) {
        case WP_DOC_DESTROYED:
            /* Named residual. A record-only route already answers `windowproxy.get` of `length` and `closed`
               (flow_perform asks window_proxy_record_member_of_document before any realm); both answers are
               ECMAScript §6.1 primitives, which remote_object_encode converts identically in any realm.
               Not covered: (1) a §7.2.1 member whose answer is an object of the destroyed realm. §7.2.1.3.1
               CrossOriginProperties ( O ) lists thirteen names, nine with [[NeedsGetter]]; a record is untrusted
               text, and remote_op_parse admits every listed name, so `location`, `opener`, `parent`, `top`,
               `window`, `self` and `frames` can arrive. §7.2.1.3.4 runs the getter on O, the destroyed
               document's Window. (2) An `object.*` verb on an object lent out of that realm. The object is not
               gone (core/frame/remote_object.c's export table holds a reference for the whole session) and the
               verb takes no realm of the target (remote_op_program runs on whatever realm it is handed), so the
               answer is a value, not a throw.
               Next diff builds: (2) first, deciding which realm of this agent performs it: `world_local_doc()`
               is realm-independent, but `ref_mint` and the navigable decoder mint in the realm they are handed
               and engine_queue_into's row document picks the world segment. Then (1), with a cross-instance
               answer for a navigable that has none.
               Absence shows as: this abort with flow_perform's asking site. A routed `windowproxy.post` to a
               destroyed document is answered at flow_deliver's §7.5.10 arm
               (`routed-delivery-the-target-document-was-destroyed`); both callers run from flow_step, so read
               the printed asking site, not the frame list. */
            DFAILF("a cross-agent operation or a queued program named a document whose active Document THIS "
                   "TIMELINE DESTROYED, asked at %s:%d — §7.5.10 \"Destroying documents\"' "
                   "set-the-Document's-browsing-context-to-null step nulled it "
                   "and window_proxy_set_destroyed nulled the navigable's realm with it, so there is no realm "
                   "now and there will not be one: proxy_realm refuses to materialize a destroyed navigable by "
                   "name. A sibling arm that did not close still answers, which is why this is one timeline's "
                   "state and not the document's. THE RECORD-ONLY ROUTE EXISTS AND THIS OPERATION IS NOT ONE "
                   "IT ANSWERS: flow_perform routes a `windowproxy.get` for `length` or `closed` through "
                   "window_proxy_record_member_of_document ahead of this lookup, so reaching here means either "
                   "a §7.2.1 member whose answer is an OBJECT of the realm that is gone, or an `object.*` verb "
                   "on an object lent out of it — two different answers, split by WHOSE REALM THE OPERATION "
                   "NEEDS: §7.2.1.3.4 runs a member's getter ON the target's own Window, and an `object.*` verb "
                   "runs on an object the export table still holds and takes no realm of the target at all. "
                   "READ THE ASKING SITE ABOVE BEFORE BUILDING EITHER: a ROUTED DELIVERY reaches this lookup "
                   "from flow_deliver and is answered there under §7.5.10 step 5, so a frame list with "
                   "flow_step on it does not say which of the three callers this is. The residual above this "
                   "line records what each population needs AND which half of its own remedy clause was "
                   "refuted — do not build a THROW for the object verbs on its say-so. Do not ask for an "
                   "ACTIVE DOCUMENT, because this navigable has none", file, line);
            break;
        case WP_DOC_INITIAL:
            DFAILF("a navigable this agent holds was asked for the realm of its active document and THIS "
                   "TIMELINE has never materialized one, asked at %s:%d — §7.4 created it with the initial "
                   "about:blank Document, and only a read THROUGH that navigable's own WindowProxy builds the "
                   "realm, per flow (core/frame/navigable.h). A sibling arm having built one says nothing "
                   "about this one. A caller that must materialize holds the NAVIGABLE and calls "
                   "window_proxy_realm, and this lookup takes a NAME: resolve it with "
                   "window_proxy_of_document first", file, line);
            break;
        case WP_DOC_REMOTE:
            DFAILF("the realm of a document a PEER INSTANCE holds was asked for at %s:%d — no realm of this "
                   "heap is it, now or ever, and `world_doc_hosted` is the gate every caller of this lookup "
                   "is supposed to have passed before reaching it", file, line);
            break;
        case WP_DOC_NO_NAVIGABLE:
            DFAILF("a document name resolved to no navigable of THIS TIMELINE at all, asked at %s:%d — it "
                   "names a document no timeline of this instance has opened, or one THIS timeline has "
                   "navigated away from (a navigation moves the navigable's `doc` from the instant "
                   "window_proxy_navigate runs, so the outgoing name answers nothing in the timeline that "
                   "navigated, and the programs still queued for it are the residue "
                   "core/frame/navigable.c's teardown assert names), or the caller is standing at HOST TIME "
                   "with no owner named (engine_unload_document's residual says what that costs and what "
                   "closes it)", file, line);
            break;
        case WP_DOC_ACTIVE:
            DFAILF("the realm lookup answered NULL for a navigable whose own record says its active document "
                   "IS materialized, asked at %s:%d — the two reads walk one table in one applied delta, so "
                   "they cannot disagree unless something between them moved the delta", file, line);
            break;
        }
    }
#endif
    return realm;
}
/* The capture at every ask. It takes the entry's old name, so the calls are unchanged text and each threads its
   own pair; the parenthesised parameter keeps the calls inside DCHECK conditions expanding as one expression.
   Nothing takes this entry's address. */
#define doc_realm(doc_) doc_realm_at((doc_), __FILE__, __LINE__)

/* The flow a queued callback belongs to when the queuer is the user agent: set only inside
   engine_unload_document's per-flow bracket, the same shape as flow_job_external_begin/_end. Everywhere a
   program queues the callback the owner is `flow_running()`; the exception is an operation the trusted zone
   reports between two scheduler steps. There `flow_running()` still names the last switched-in flow
   (engine_sched_step returns the quantum yield without switching it out, so it can resume byte-identically),
   and that flow did not cause the callback. The named owner wins because of causation: the host's operation
   is a task of the flow the bracket names and no other. */
static Flow *g_enqueue_owner;

/* HTML §7.4.6.1 Updating the traversable's deactivate a document for a cross-document navigation, made a work
 * item of every live timeline of this instance (engine_route's and engine_perform's shape).
 *
 * A destruction is per flow because every piece of it is state a page observes: §7.5.9 fires `pagehide` and
 * `unload` at listeners a script registered (in that flow's delta); "clear window's map of active timers" clears
 * that timeline's timers; §7.5.10 step 6's browsing-context-null writes the WindowProxy record, whose COW
 * capture makes it per flow (document_lifecycle.h). Run in one timeline, every other flow would go on running a
 * document the browser replaced. Nothing runs inside this call: each flow unloads when next scheduled, under its
 * own delta, so two arms' listeners are two suspensions, not one drive to completion.
 *
 * The owner is named rather than current. §7.5.9 and §7.5.10 queue global tasks through JS_EnqueueCallTask, which
 * asks the scheduler for the owning flow, and between steps `flow_running()` is a flow that did not cause this.
 * Declining the hook would put the task on quickjs's global list, which nothing drains; seeding a fresh flow from
 * the baseline would lose the page's listeners. No flow is dropped or paged: a suspended flow keeps its snapshot,
 * §7.5.10 step 5 removes only queued tasks, and the realm is kept alive by references a suspended continuation
 * holds (core/frame/navigable.c's teardown).
 *
 * The incoming document is not an argument: §7.5.9 Unloading documents' unload-a-document-and-its-descendants
 * step 6 queues on the unloaded document's relevant global object (the body asserts it at its step 1), and
 * `newDocument` only feeds the document unload timing info this user agent does not carry (step 3 handles its
 * absence). A cross-origin incoming Document loads into a peer, so it has no realm here; the outgoing one is
 * local by construction. §7.5.10 step 5 does not eat the operation: the unload reaches it from inside its own
 * body (§7.5.9 step 20), after the task left the queue, and the removal is scoped to the destroying timeline
 * (engine_drop_jobs). */
void engine_unload_document(uint32_t doc)
{
    JSContext *dctx;
    int n, i;

    DCHECK(g_sess_live,
           "a Document was reported REPLACED to an instance with no live session — the unload is a task of "
           "every timeline of that document and there is no scheduler to run one, so the destruction would be "
           "queued onto nothing and the replaced document would go on answering for this instance");
    DCHECK(world_doc_hosted(doc),
           "a Document this agent does not hold was reported replaced — the trusted zone is the only zone that "
           "knows which instance holds which document, and it named this one to the wrong instance; unloading "
           "here would destroy a document the browser did not navigate away from");
    n = flow_count();
    DCHECK(n > 0,
           "a Document was replaced while every timeline of this instance had already finished — there is no "
           "flow to run its unload in, so its `pagehide` and `unload` listeners fire in no timeline at all and "
           "§7.5.10's destruction happens nowhere. A document a navigation can still replace is a document "
           "whose flows are still live, which is the same statement engine_set_referenced makes for a document "
           "a peer still holds a reference into");
    for (i = 0; i < n; i++) {
        Flow *f = flow_at(i);

        /* The bracket closes on every path because the body has one: anything aborting between these lines
           aborts the process, so no stale owner can outlive a freed flow. */
        g_enqueue_owner = f;
        /* The realm is asked inside the bracket because the answer is the asking timeline's
           (core/frame/window_proxy.h), not a per-document fact.
           Named residual. Not covered: the realm answered is the applied delta's, and at host time that is
           whichever flow the last slice left switched in, so for a navigable whose Document was materialized per
           flow every iteration asks one timeline's question; `g_enqueue_owner` names the enqueue's owner but
           cannot read another flow's delta (solver/cow.h offers no read of an unapplied delta). Next diff builds:
           the enqueue takes the document name and resolves its realm when the task runs
           (core/frame/document_lifecycle.h's unload entry over a `uint32_t doc`, the subtree walk per flow under
           its own delta); switching each flow in around the body would be a second scheduler. Absence shows as:
           doc_realm's NULL partition aborting with this line as the asking site and no flow-time caller on the
           frame list, for a navigation of an engine-created navigable whose Document only another timeline read
           through. A host-installed document cannot show it: window_proxy_new_self adopts its realm at the
           mint, uncaptured. */
        dctx = doc_realm(doc);
        document_lifecycle_unload_replaced(dctx);
        g_enqueue_owner = NULL;
        /* Askable again, for engine_route's reason: a flow holding its own document's unload and never picked
           is a destruction that never runs. */
        flow_clear_host_owed(f);
    }
}

/* The sibling this delivery forecloses: two arms of a sender's fork posting two messages belong to two
 * contradictory worlds, and merging them would fabricate a timeline neither sender was in. Minted at the delivery,
 * the one moment the receiving flow is switched in.
 *
 * A world with a fork point above it is one arm of a branch whose other arm was minted at the same instant
 * (world_mint_child mints both children and retires the point), so a sending timeline that did not send this
 * exists, and the receiver has a timeline that did not receive it: this arm, the receiver's state up to here (via
 * the delta fork) with the other side of the sender's branch. A root world names no fork point and gets no arm.
 *
 * One arm, not one per ancestor: it forecloses the whole subtree at `vec`; higher branches got their arms at the
 * deliveries of the worlds on them. Its decision state is decide_fork_same_path's, since no predicate was asked.
 * As decide.h says of the answer fork, a decision vector cannot record which sender arm each arm took, so the
 * mapping crosses a park in the commitment record (flow.h) and is not carried across a re-run of the document.
 * `f` makes the delivery in this same step; at most one arm per delivery. */
static Flow *engine_sibling_assemble(JSContext *ctx, Flow *parent, JSValue *clone,
                                     void *dec_blob, void *pin_blob);
/* What the step names itself (defined with the scheduler below). A refusal is a step that consumed a record and
   delivered nothing, and it says so. */
static StepUnit g_step_unit;

/* Returns which mechanism minted the other side, because this is the one line that knows: the caller writes it
   on the commitment row and deliver_admits reads the row rather than re-deriving it from the vector. "Did this
   fork run" and "does this vector name a branch" agree for this producer but are different questions. */
static FlowCommitArm deliver_fork_arm(JSContext *ctx, Flow *f, const char *vec)
{
    char *fork_point = world_vec_fork_point(vec);
    FlowCommitArm covering;
    Flow *sib;

    /* A root world: no branch, so no other side to be on. */
    if (fork_point == NULL) return FLOW_COMMIT_ARM_NONE;
    free(fork_point);
    /* This timeline has not already minted it. The arm for a (flow, world) pair is minted at the flow's first
       delivery from that world, together with its commitment, decided by one predicate (deliver_commit_implied)
       over a list neither call changes (the arm's own row goes on the sibling), so an arm cannot exist without
       its row. The converse holds only where the vector names an ancestor: a root world is pushed as a
       commitment with no arm, which is the state deliver_admits' refusal asserts against.
       An implied commitment is covered by the deeper one's arm, whose foreclosure is a subtree containing this
       world, and which mechanism minted that arm is the covering row's statement, not a constant: a
       cross-instance answer also pushes received rows (answer_commit_taken), and its arm is owed, not minted. */
    if (deliver_commit_implied(ctx, f, vec, &covering)) return covering;
    DCHECK(f->frame == NULL,
           "a delivery-time fork was taken by a flow INSIDE a program — a delivery is made between programs "
           "(flow_step reaches it only with no frame), so a parent holding one means this ran somewhere else "
           "and the arm would be marked hot with nothing to resume");
    DCHECK(!JS_HasActivation(JS_GetRuntime(ctx)),
           "a delivery-time fork was taken with page code on the stack — the arm re-enters its scheduler step "
           "and re-reaches this delivery, which is only true of a fork asked between tasks");
    sib = engine_sibling_assemble(ctx, f, NULL,
                                  decide_fork_same_path("(a cross-document message arrived from one arm of a "
                                                        "sender's branch — no predicate was asked)"),
                                  concolic_pins_suspend());
    /* The one thing that makes the arm a different timeline, written after assembly so the arm's inherited
       record is the parent's as of before this delivery. Its other side is `f`, so the arm value is this same
       mechanism: a foreclosed row defers to the parent, and the parent is here. */
    flow_world_commit_push(ctx, sib, vec, 0, FLOW_COMMIT_ARM_DELIVERY_FORK);
    return FLOW_COMMIT_ARM_DELIVERY_FORK;
}

/* This timeline's own half: from here on it is in the world it is about to hear from. A commitment already at or
   below this world makes the push redundant (worlds comparable with a chain's deepest member are comparable with
   all of it), which keeps the record O(the sender's distinct worlds) rather than O(its messages). It is not a
   seen-set: a world arriving again is admitted as the first was; only a duplicate row is skipped. A deeper
   world arriving after a shallower entry leaves that entry standing, because both are true and removing one
   would mutate an Array the arms of this flow share. */
static void deliver_commit_taken(JSContext *ctx, Flow *f, const char *vec, FlowCommitArm arm)
{
    /* The same predicate the arm above is minted on, so the row and the arm cannot drift apart: they are the two
       halves of "this timeline has heard from `vec`". `arm` is what the fork above observed about its own run,
       passed down, never re-derived here. */
    if (!deliver_commit_implied(ctx, f, vec, NULL)) flow_world_commit_push(ctx, f, vec, 1, arm);
}

/* The same half for a cross-instance answer: the second producer of a received row, and why flow.h's vocabulary
 * has a member naming an obligation rather than an outcome.
 *
 * An answer to a cross-document read is computed by one timeline of the peer and true of it alone, so a flow
 * that took it is in that sending world, exactly as after a routed message, and a later `postMessage` from a
 * contradicting peer timeline must be refused. The arm differs: deliver_fork_arm mints the other side at the
 * delivery, but an answer's other side is flow_answer_fork's, built only if another timeline's answer arrives
 * (a peer with one timeline never produces one), so the row states the obligation.
 *
 * Pushed at the take, not the answer: the flow has received nothing until it consumes, and flow_step drains every
 * recorded extra into an arm before resuming the consuming frame (engine_host_take asserts it). So when
 * flow_answer_fork clones the parent, the parent has no row for its own answer, the arm inherits nothing false,
 * and each side pushes its own row at its own take. A push at the answer would put the issuer's world on the
 * arm's list, contradicting the world the arm took. The suppression is the delivery path's predicate. */
static void answer_commit_taken(JSContext *ctx, Flow *f, const char *world)
{
    DCHECK(world != NULL && *world,
           "a cross-instance answer was recorded as a commitment with no answering TIMELINE — a peer answers "
           "by running a program on one of its flows, and an answer that names none cannot say which of that "
           "document's timelines this one is now in");
    if (!deliver_commit_implied(ctx, f, world, NULL))
        flow_world_commit_push(ctx, f, world, 1, FLOW_COMMIT_ARM_ANSWER_OWED);
}

/* The delivery itself, made by the receiving flow's own step: it runs with that flow switched in, under its
   delta, and the task it enqueues lands on that flow's queue. This is the dispatch on the op and the only place a
   routed op becomes a call; an op with no component here is a transport carrying something nothing receives.
   The two outcomes of a routed record meeting a timeline are counted at the two lines that are those outcomes
   (engine.h says why neither is readable alone). */
static long g_routed_delivered, g_routed_refused;

/* Was this record admitted by any timeline at all? The two counters above are per (record, timeline)
 * attachment, so no arithmetic over them answers a per-record question: one record admitted by N timelines pays
 * for N-1 admitted by none. This makes "lost messages" an equality against zero that no multiplicity or schedule
 * can move.
 *
 * A gauge, not a lifetime count: records handed to this instance that no timeline has admitted yet. It rises on
 * arrival and falls when some timeline admits one, so a record in flight reads like a lost one; only a drained
 * receiver makes it a loss, and a host must drain first and say so. Its two neighbours are lifetime counts.
 *
 * Keyed on the record text, because identical records share a fate: deliver_admits reads nothing but those bytes,
 * and a timeline that admitted the first copy relates SAME to the second. Named residual. Not covered: if every
 * flow that admitted a text leaves the frontier between two arrivals of it, the second copy is lost while the
 * ledger says admitted. Next diff builds: an id per routed record, carried as a third element of the
 * [record, senderOrigin] entry, refcounted at flow_deliver_fork and retired at the last consume (a change to
 * solver/flow.c's delivery entry). Absence shows as: this reading zero on a run whose receiver finished a
 * timeline between two identical posts. Process-lifetime, like `g_genforks`: one row per distinct record text. */
typedef struct { char *record; long routed, admitted; } RoutedRec;
static RoutedRec *g_routed_recs; static int g_routed_recs_n, g_routed_recs_cap;

static RoutedRec *routed_rec_find(const char *record) {
    int i;
    for (i = 0; i < g_routed_recs_n; i++)
        if (!strcmp(g_routed_recs[i].record, record)) return &g_routed_recs[i];
    return NULL;
}

/* A record this instance was handed, raised at engine_route once per record before the attach loop, so a record
   that aborts inside the loop still counts as admitted by nobody. */
static void routed_rec_arrived(const char *record) {
    RoutedRec *r = routed_rec_find(record);

    if (r == NULL) {
        if (g_routed_recs_n == g_routed_recs_cap) {
            int cap = g_routed_recs_cap ? g_routed_recs_cap * 2 : 16;
            RoutedRec *g = realloc(g_routed_recs, (size_t)cap * sizeof *g);
            CHECK(g != NULL, "engine: OOM recording that a peer's message arrived — the ledger this feeds is "
                             "the only statement that every routed record reached a timeline, so a record "
                             "missing from it is a loss that reports as a clean run");
            g_routed_recs = g; g_routed_recs_cap = cap;
        }
        r = &g_routed_recs[g_routed_recs_n++];
        r->record = strdup(record);
        CHECK(r->record != NULL, "engine: OOM naming a routed record in the delivery ledger");
        r->routed = r->admitted = 0;
    }
    r->routed++;
}

/* One a timeline admitted, raised at the delivery beside `g_routed_delivered`, the line that knows a record
   became §9.3.3 step 8's task. */
static void routed_rec_admitted(const char *record) {
    RoutedRec *r = routed_rec_find(record);

    DCHECK(r != NULL,
           "a routed record was DELIVERED that this instance has no arrival for, so the ledger beside it "
           "under-reports every loss by exactly the population this fired on. Three causes and they are "
           "different repairs: the delivery queue was rebuilt from the COLD TIER without replaying the "
           "arrival (build the inbound half of the cross-instance park, which engine_route names by DCHECK); "
           "something pushes onto a delivery queue OUTSIDE engine_route, which is a second inbound path; or "
           "the record did not survive its round trip through the queue's JS string, in which case this key "
           "is the wrong key and the per-attachment ticket the ledger names as its residual is the repair "
           "rather than a different comparison");
    if (r != NULL) r->admitted++;
}

void engine_routed_census(long *delivered, long *refused, long *zero_delivery) {
    int i;

    DCHECK(delivered != NULL && refused != NULL && zero_delivery != NULL,
           "the routed-delivery census was asked for some of its three numbers — a delivery count with no "
           "refusal count beside it cannot say whether the rest of the frontier declined the record or never "
           "saw it, and neither of them can say whether any ONE record reached a timeline, which is the only "
           "form of the question a lost message has an answer to. ALL THREE OR NONE, for the reason "
           "engine_routed_task_census gives for its four");
    *delivered = g_routed_delivered; *refused = g_routed_refused;
    *zero_delivery = 0;
    for (i = 0; i < g_routed_recs_n; i++)
        if (g_routed_recs[i].admitted == 0) *zero_delivery += g_routed_recs[i].routed;
}

/* What became of the tasks those deliveries queued (engine.h). Counted here rather than in window_message.c
   because their sum is compared against the pair above, and one conservation law is kept in one file. */
static long g_routed_task_end[ROUTED_TASK_END_N];

void engine_routed_task_end(int end) {
    DCHECK(end >= 0 && end < ROUTED_TASK_END_N,
           "§9.3.3 step 8's task reported an end this census does not name — the ends are declared in engine.h "
           "and a fifth is a path through the delivery task that nothing counts, which is exactly the hole the "
           "census exists to close");
    g_routed_task_end[end]++;
}

void engine_routed_task_census(long *ends) {
    int i;

    DCHECK(ends != NULL,
           "the routed-task census was asked for nothing to write into — all four ends or none, because a "
           "fired count with no declined count beside it cannot tell a delivery the spec refused from one the "
           "scheduler lost");
    for (i = 0; i < ROUTED_TASK_END_N; i++) ends[i] = g_routed_task_end[i];
}

static void flow_deliver(JSContext *ctx, Flow *f)
{
    JSValue entry, rv, ov;
    const char *record, *origin;
    char *dup, *doc, *worlds, *tail, *vec;
    WorldId w;
    const WorldId *anc;
    uint32_t doc_id;
    int n_anc;
    /* Which arm this delivery leaves through, held because the line that decides it is not the one that
       records it. */
    StepUnit verdict;
    CowDelta *seg;
    JSContext *rctx;

    /* Asserted before the queue is touched, because the take mutates: an abort after it would consume a peer's
       message and leave the residue missing the work item the crash is about. */
    DCHECK(flow_running() == f, "a routed delivery was made while another flow was switched in — it would run "
                                "against that flow's delta and its task would land on that flow's queue");
    /* Whose timeline the oldest record belongs to is decided before it is taken, because two of the three
       answers are not a delivery: read, decide, then consume. */
    entry = flow_deliver_entry(f, 0);
    rv = JS_GetPropertyUint32(ctx, entry, 0);
    record = JS_ToCString(ctx, rv);
    CHECK(record != NULL, "engine: OOM reading a routed delivery's record — a record that cannot be read is a "
                          "peer's message this document never receives");
    vec = record_worlds_dup(record);
    JS_FreeCString(ctx, record);
    JS_FreeValue(ctx, rv);
    JS_FreeValue(ctx, entry);
    verdict = deliver_admits(ctx, f, vec);
    if (verdict != STEP_UNIT_ROUTED_DELIVERY) {
        /* Not this timeline's message, and not a dropped work item: it belongs to the other side of a sender
           branch this timeline took a side at, and the flow there holds its own copy (engine_route attaches to
           every live flow; a fork hands the arm its own Array naming the same entries). deliver_admits asserts
           that side exists. Consuming it is the whole of the work: §9.3.3's ordering is per receiving timeline. */
        free(vec);
        entry = flow_deliver_take(ctx, f);
        JS_FreeValue(ctx, entry);
        /* Counted, because "declined" and "never offered" are the two readings of a low delivery count and
           take opposite actions (engine.h). One counter for both refusal kinds, because `_routedRefused` pairs
           with `_routedDelivered` and engine/route.mjs differences them against the records a zone handed a
           document; the step unit partitions the same population per arm. It leaves through the arm
           deliver_admits named: the line that read the commitment decides it, once. */
        g_routed_refused++;
        g_step_unit = verdict;
        return;
    }
    /* The arm that does not receive it is minted before this timeline commits to receiving it, so the arm
       inherits a record that does not yet name this world, and the mint has happened (or been declined) by the
       time its answer is written down. Two statements, because C does not order the arguments of one call. */
    {
        FlowCommitArm arm = deliver_fork_arm(ctx, f, vec);
        deliver_commit_taken(ctx, f, vec, arm);
    }
    free(vec);
    /* The oldest unmade delivery, taken from the front and owned here. One per step: the record becomes a task
       at the receiving Window (window_message_route), so two posts from one sender enqueue two tasks in post
       order, as §9.3.3's task source guarantees. */
    entry = flow_deliver_take(ctx, f);
    rv = JS_GetPropertyUint32(ctx, entry, 0);
    ov = JS_GetPropertyUint32(ctx, entry, 1);
    record = JS_ToCString(ctx, rv);
    origin = JS_ToCString(ctx, ov);
    CHECK(record != NULL && origin != NULL,
          "engine: OOM reading a routed delivery off its queue — a record that cannot be read is a peer's "
          "message this document never receives");
    dup = strdup(record);
    CHECK(dup != NULL, "engine: OOM splitting a routed delivery's transport fields");
    doc = strchr(dup, '\t');    *doc++ = 0;
    worlds = strchr(doc, '\t'); *worlds++ = 0;
    tail = strchr(worlds, '\t'); *tail++ = 0;
    /* The receiving document's realm is what "delivered to that document" means: the event and `event.source`
       are made in it and the task is enqueued at its Window. It must be a document this agent holds, asked
       again here because a delivery off the cold tier never passed engine_route in this session: a resumed flow
       replays from its first script, so a record naming a child navigable can be taken before the replay has
       re-created it. This assert names that ordering (a parked delivery must be made where the replay reached
       its arrival); doc_realm's DFAIL would instead name a navigable without a realm. */
    doc_id = world_doc_intern(doc);
    DCHECK(world_doc_hosted(doc_id),
           "a routed delivery names a document this session does not hold — the record survived the cold tier "
           "but the DOCUMENT it names has not been re-created yet, because a resumed flow replays from its "
           "first script and this delivery is taken off the queue before any of it runs. Make a parked "
           "delivery at the position in the replay where its message arrived, rather than ahead of the "
           "programs that build the navigable it names");
    /* HTML §7.5.10 Destroying documents step 5, performed while the record is still a record: "Remove any tasks
       whose document is document from any task queue (without running those tasks)". §9.3.3's window post
       message steps step 8 queues a global task given targetWindow, so the delivery is that task, and §8.1.7.1
       Definitions makes it unrunnable anyway ("A task is runnable if its document is either null or fully
       active"). The record is consumed and no task queued. Asked before the realm, as flow_perform does, since
       a destroyed navigable has no realm and nothing above needs one.
       `destroyed` is COW-captured on the navigable's record (core/frame/window_proxy.h), so a sibling that never
       destroyed the Document delivers its own copy; the count is per arm.
       It does not raise `ROUTED_TASK_TARGET_GONE`: that end belongs to tasks that existed (window_message.c when
       the task ran, solver/flow.c when step 5's walk drops a queued job), and the end census's law is
       `sum >= g_routed_delivered`; an end with no delivery would be slack masking a queued task that never ran.
       It is counted in `g_routed_refused`, as above, and makes `_routedZeroDelivery` nonzero for a correct
       reason when every receiving timeline destroyed the Document; this step unit is the discriminator. */
    if (window_proxy_document_state(doc_id) == WP_DOC_DESTROYED) {
        /* The state read and the realm read must agree; doc_realm_at's WP_DOC_ACTIVE arm asserts the other
           direction. Both are this codebase's own values off one record in one applied delta, and neither read
           takes a ctx or captures anything. A failure would discard a message that had a Document to fire at. */
        DCHECK(window_proxy_realm_of_document(doc_id) == NULL,
               "a navigable whose own record says THIS TIMELINE destroyed its active Document still answered "
               "with a realm — the state partition and the realm read walk ONE table in ONE applied delta, so "
               "they cannot disagree unless something between them moved the delta, and this delivery is about "
               "to be discarded under §7.5.10 \"Destroying documents\" step 5 for a Document that is still "
               "there to fire at");
        g_routed_refused++;
        g_step_unit = STEP_UNIT_ROUTED_TARGET_DESTROYED;
        /* The record is not marked admitted: no timeline heard it, and `_routedZeroDelivery` says so. */
        free(dup);
        JS_FreeCString(ctx, record);
        JS_FreeCString(ctx, origin);
        JS_FreeValue(ctx, rv);
        JS_FreeValue(ctx, ov);
        JS_FreeValue(ctx, entry);
        return;
    }
    rctx = doc_realm(doc_id);
    /* The sending document is the head of the world vector: a world is minted by a flow of exactly one
       document, so a second field for it could disagree. */
    n_anc = world_parse(worlds, &w, &anc);
    /* The other half of this delivery's world, asked again where it is consumed: other flows have run since
       engine_route asked, and the answer is a property of the run. Only f's timeline is installed, so an empty
       sender segment is what makes that the whole conjunction. Looked up on its own line so release and dev
       builds materialize the same segments. */
    seg = world_segment(ctx, w, anc, n_anc);
    (void)seg;
    DCHECK(cow_delta_empty(seg),
           "a delivery ran in the receiving flow's timeline alone while the sending world holds writes in this "
           "instance — the message arrives at a document missing everything its sender did here. Build the join "
           "of the two deltas that engine_route names");
    /* The one place work enters this flow from outside its own program: every other job is caused by code the
       flow ran (a `.then`, a timer, a fetch), so replaying that code re-causes it, which is cold.h's claim for
       the queue. A routed record's task is the one work item a recipe does not regenerate, so the enqueue is
       bracketed as external here, the only moment its origin is knowable.
       §9.3.3 Posting messages step 8 queues exactly one global task ("queue a global task on the posted message
       task source given targetWindow"): zero tasks is a message the page never receives, two is a handler run
       twice, and both are silent elsewhere. It is an equality, not a ceiling, because the targetOrigin check
       (step 8.1) happens inside the task (core/frame/window_message.c), the target having possibly navigated, so
       the enqueue is unconditional. A host comparing its routed count to handler invocations is not measuring
       this engine (N admitting timelines give N invocations); it compares against `g_routed_delivered`. */
    {
        /* Read on its own line and voided below, because the DCHECK that reads it vanishes in release. */
        int jobs0 = flow_job_pending(f);
        flow_job_external_begin();
        if (!strcmp(dup, "windowproxy.post"))
            window_message_route(rctx, tail, world_doc_name(w.doc), origin);
        else
            DFAIL("a record was routed with an op no component receives — the sending half emits it, so the "
                  "receiving half is the unbuilt one; build it rather than dropping the delivery");
        flow_job_external_end();
        DCHECK(flow_job_pending(f) == jobs0 + 1,
               "a routed delivery did not become exactly ONE task on the receiving timeline's queue — HTML "
               "§9.3.3 Posting messages step 8 queues one global task on the posted message task source, so "
               "zero is a peer's message this document never receives (and cannot know it did not) and two is "
               "one message the page's handler runs for twice, which no count outside this line can tell from "
               "two messages");
        (void)jobs0;
        /* The delivery is counted where it happened, one per task queued, which is one per handler invocation
           (engine.h). */
        g_routed_delivered++;
        /* And the record is marked as heard by some timeline of this document: the fact a lost message is
           answered by. `record` is the exact text engine_route registered; this function split a copy. */
        routed_rec_admitted(record);
    }
    free(dup);
    JS_FreeCString(ctx, record);
    JS_FreeCString(ctx, origin);
    JS_FreeValue(ctx, rv);
    JS_FreeValue(ctx, ov);
    JS_FreeValue(ctx, entry);
}

/* ---- Asked to perform one: the half with an answer --------------------------------------------------- */

/* The unstarted-operation FIFO (flow.h's perform_q): a JS Array of immutable [record, token] Arrays. Three
 * operations only, since an entry is never edited after its push: append at arrival, take from the front at the
 * start, and give a fork's arm its own Array naming the same entries. Every mutation runs inside
 * cow_engine_write_begin/end for pending.h's reason: this is the scheduler's record about a flow, the host
 * writes it outside any flow's delta (engine_perform walks every flow), and a delta that captured it would
 * truncate an entry away when a sibling switched in. */
static void perform_q_push(JSContext *ctx, Flow *f, const char *record, const char *token) {
    JSValue e = JS_NewArray(ctx);
    CHECK(!JS_IsException(e), "engine: OOM allocating a cross-agent operation record — a dropped record is a "
                              "peer's flow suspended at the read that asked, for the rest of its process");
    cow_engine_write_begin();
    JS_SetPropertyUint32(ctx, e, 0, JS_NewString(ctx, record));
    JS_SetPropertyUint32(ctx, e, 1, JS_NewString(ctx, token));
    if (!JS_IsObject(f->perform_q)) {
        JS_FreeValue(ctx, f->perform_q);
        f->perform_q = JS_NewArray(ctx);
        CHECK(!JS_IsException(f->perform_q), "engine: OOM allocating a flow's operation queue");
    }
    JS_SetPropertyUint32(ctx, f->perform_q, (uint32_t)flow_perform_pending(f), e);
    cow_engine_write_end();
}

/* Takes the oldest, which makes this a queue rather than the register's set: the asking side parks on each
   operation in turn and expects answers in arrival order, so a swap-remove would answer the second question
   first. The entry is owned by the caller. */
static JSValue perform_q_take(JSContext *ctx, Flow *f) {
    int n = flow_perform_pending(f), i;
    JSValue e;

    DCHECK(n > 0, "an operation was taken from a flow whose queue is empty — the caller tested the queue to "
                  "get here, so the two reads have come apart");
    e = JS_GetPropertyUint32(ctx, f->perform_q, 0);
    DCHECK(JS_IsObject(e), "a flow's operation queue held something that is not a [record, token] pair");
    cow_engine_write_begin();
    for (i = 1; i < n; i++)
        JS_SetPropertyUint32(ctx, f->perform_q, (uint32_t)(i - 1), JS_GetPropertyUint32(ctx, f->perform_q, (uint32_t)i));
    JS_SetPropertyStr(ctx, f->perform_q, "length", JS_NewInt32(ctx, n - 1));
    cow_engine_write_end();
    return e;
}

/* The arm's own queue: a new Array naming the parent's entries, for pending_fork's reason. Each arm takes an
   entry when it starts that operation, so a shared Array would expose each arm's consumption to the other;
   entries never change after the push, so they are shared. The length comes from flow_perform_pending, the
   queue shape's one reader, which checks the tag. An empty queue forks as JS_UNDEFINED, the common case of a
   flow never asked an operation; perform_q_push mints the Array if the arm is ever asked one. */
static JSValue perform_q_fork(JSContext *ctx, const Flow *parent) {
    int n = flow_perform_pending(parent), i;
    JSValue out;

    if (n == 0) return JS_UNDEFINED;
    out = JS_NewArray(ctx);
    CHECK(!JS_IsException(out), "engine: OOM forking a flow's operation queue — an arm that lost it runs a "
                                "peer's operation and tells nobody");
    cow_engine_write_begin();
    for (i = 0; i < n; i++)
        JS_SetPropertyUint32(ctx, out, (uint32_t)i,
                             JS_GetPropertyUint32(ctx, parent->perform_q, (uint32_t)i));
    cow_engine_write_end();
    return out;
}


/* engine_route carries a one-way delivery; this one is asked, and its shape follows from that.
 *
 * It is not a request/response pair, because a document's state is its flows: `otherW.length` is the child
 * navigable count of the peer's active document, and the peer's N timelines hold N such documents, so there are
 * N true answers. The record is attached to every live flow, as a delivery is, and each answers under its own
 * delta; the asking side forks per answer (engine_host_answer).
 *
 * The answer is a program's completion: the operation is queued as the flow's next program (flow_perform) and
 * the completion is read where the scheduler reads one (flow_step), so it is an ordinary flow on the one
 * frontier, preemptible and parkable, which is what lets a peer answer a read that must suspend. A serve loop
 * would be a second scheduler.
 *
 * `token` is the trusted zone's and opaque here: echoed on the answer so the zone routes the completion back to
 * the asking instance and request. It is not the asker's request id, which is unique only within that instance. */
/* See engine.h. The rows are the flow's and the relation is world.h's ("which of two sending timelines continues
   the other" is a fact about the world forest); what lives here is the commitment, since engine.c holds both a
   record's vector and the receiving flow (the split solver/flow.h states from the other side). */
char *engine_flow_addressee(JSContext *ctx, Flow *f, const char *doc_name)
{
    int n, i, ambiguous = 0;
    char *best = NULL;

    DCHECK(f != NULL, "the addressee of a cross-instance operation was asked of no flow — a commitment is a "
                      "fact about ONE receiving timeline and there is no such thing as a flowless one");
    DCHECK(doc_name != NULL && *doc_name,
           "the addressee of a cross-instance operation was asked without naming the document it addresses — "
           "a flow may be committed to timelines of several peers at once, and an unkeyed answer would "
           "address one peer's operation to another peer's world");
    n = flow_world_commits(f);
    for (i = 0; i < n; i++) {
        JSValue e = flow_world_commit_at(f, i);
        JSValue cv = JS_GetPropertyUint32(ctx, e, 0);
        JSValue tv = JS_GetPropertyUint32(ctx, e, 1);
        const char *c = JS_ToCString(ctx, cv);

        CHECK(c != NULL, "engine: OOM reading which sending timeline a flow is in while addressing a "
                         "cross-instance operation — an unreadable commitment would emit the operation "
                         "unaddressed, which is the cross-product this field exists to close");
        /* A foreclosed row names a subtree this timeline is not in and is never addressed: the flow on the
           other side took it, and a record sent under that name would reach the timelines this flow refused
           to hear from. Only a received row says where this flow is. */
        if (JS_VALUE_GET_TAG(tv) == JS_TAG_INT && JS_VALUE_GET_INT(tv)) {
            const WorldId *anc;
            WorldId w;

            world_parse(c, &w, &anc);
            (void)anc;
            if (!strcmp(world_doc_name(w.doc), doc_name)) {
                if (best == NULL) {
                    best = strdup(c);
                    CHECK(best != NULL, "engine: OOM naming the peer timeline a cross-instance operation is "
                                        "addressed to");
                } else {
                    WorldRel rel = world_vec_relate(c, best);

                    DCHECK(rel != WORLD_REL_CONTRADICT,
                           "two RECEIVED commitments to one sending document CONTRADICT on a flow that is "
                           "addressing its next operation — flow_world_commit_push aborts on exactly that "
                           "pair at the append, so a pair reaching this walk means a row got onto the list "
                           "without going through it (the cold tier's replay is the producer that can)");
                    /* Deepest wins: world_vec_relate(a, b) answers DESCENDANT when `b` is an ancestor of `a`, so
                       the candidate speaks for a strictly narrower subtree. SAME and ANCESTOR keep the
                       incumbent. */
                    if (rel == WORLD_REL_DESCENDANT) {
                        char *d = strdup(c);
                        CHECK(d != NULL, "engine: OOM naming the peer timeline a cross-instance operation is "
                                         "addressed to");
                        free(best);
                        best = d;
                    } else if (rel == WORLD_REL_INDEPENDENT) {
                        /* Two generations of one document, which no order resolves (engine.h). Recorded and read
                           after the walk, so a later deeper row cannot un-ambiguate it. */
                        ambiguous = 1;
                    }
                }
            }
        }
        JS_FreeCString(ctx, c);
        JS_FreeValue(ctx, cv);
        JS_FreeValue(ctx, tv);
        JS_FreeValue(ctx, e);
    }
    if (ambiguous) { free(best); best = NULL; }
    return best;
}

/* What the ask side did, as two lifetime counts over this process: `asks`, the cross-agent operations that
   reached this instance, and `attached`, the (operation x timeline) pairs engine_perform's walk made. The
   arrival is counted as the first statement of the body, before any gate (the parse, the routing assert, the
   attach loop's contradiction exclusion) can decline, because an instrument over a gated operation counts the
   ask, not the outcome. Together they split engine_retract_census's `flows` of 0 at a park into its three
   readings: (i) no question arrived (the asking path or the zone's routing); (ii) one arrived and every live
   timeline contradicted the addressee the peer named (the `attached > 0` DCHECK at this function's foot, in
   release); (iii) one was attached and every holder left the frontier before the park walked. */
static long g_perform_asks, g_perform_attached;

void engine_perform_census(long *asks, long *attached)
{
    DCHECK(asks != NULL && attached != NULL, "the cross-agent ask census was asked to fill nothing");
    *asks = g_perform_asks; *attached = g_perform_attached;
}

void engine_perform(JSContext *ctx, const char *token, const char *record)
{
    RemoteOp *op;
    char *addressee = NULL;
    int n, attached = 0;

    /* The ask, raised before anything can decline: a malformed arrival is still an arrival. */
    g_perform_asks++;

    DCHECK(record != NULL && *record, "a cross-agent operation arrived with no text to perform");
    DCHECK(token != NULL && *token, "a cross-agent operation arrived with no rendezvous token — the completion "
                                    "would have nothing to name and the asking flow would park forever");
    DCHECK(strchr(token, '\t') == NULL && strchr(token, '\n') == NULL,
           "a rendezvous token carries a record separator — the answer notice would be read as two notices and "
           "the tail routed as an operation");
    DCHECK(g_sess_live, "a cross-agent operation was asked of an instance with no live session — there is no "
                        "scheduler to run the program that answers it");

    op = remote_op_parse(record);
    /* Routed to the wrong instance is the trusted zone's bug, and answering would return this document's frame
       count as the other's. Which of this agent's documents it names is the operation's business: the realm is
       selected where the program is queued (flow_perform), with the answering flow switched in, after other
       flows have had their turn to materialize a navigable this one may reach through. */
    DCHECK(world_doc_hosted(world_doc_intern(remote_op_doc(op))),
           "a cross-agent operation was routed to an instance that does not hold the document it names — the "
           "offscreen is the only zone that knows which instance holds which document, and it sent this one to "
           "the wrong place");
    {
        WorldId w;
        const WorldId *anc;
        int n_anc = world_parse(remote_op_worlds(op), &w, &anc);
        /* The asking world belongs to a peer, checked against every document this agent holds: an agent is an
           origin-keyed cluster, so an operation on a same-origin child's object is performed in this heap at its
           call site and never reaches a transport. */
        DCHECK(!world_doc_hosted(w.doc), "a cross-agent operation came back to the instance whose flow asked "
                                         "it — an operation on this agent's own object is performed locally "
                                         "and never leaves");
        /* The asking world's segment here, materialized on its own line for engine_route's reason. Its
           conjunction with the answering flow's timeline is the join engine_route names, the identity while the
           segment is empty. */
        CowDelta *seg = world_segment(ctx, w, anc, n_anc);
        (void)seg;
        DCHECK(cow_delta_empty(seg),
               "the asking world has WRITTEN in this instance, so the operation must be performed in the "
               "conjunction of the answering flow's timeline with those writes — the same JOIN engine_route "
               "names, and the same reason: stacking one over the other overwrites whichever slot both touched "
               "and unapplies to the baseline rather than to the flow's value");
    }
    /* Which of this instance's timelines the asker is in, copied off the record before it is freed (`addr`
       points into the parse's text). ENGINE_ADDRESSEE_NONE is a real answer and the common one (engine.h): a
       flow that has taken no cross-instance answer is in none of our timelines, so every one may answer, and the
       fan-out is required, since `otherW.length` differs in each and all are true. */
    {
        const char *addr = remote_op_addressee(op);

        DCHECK(addr != NULL && *addr,
               "a cross-agent operation arrived with an EMPTY addressee field — the grammar spells the "
               "unaddressed case as a token (solver/engine.h's ENGINE_ADDRESSEE_NONE) precisely so that "
               "\"this flow addresses nobody\" is something a writer SAYS rather than a hole a reader fills, "
               "and an empty field is the two ends of this wire disagreeing about which of the two it is");
        if (strcmp(addr, ENGINE_ADDRESSEE_NONE) != 0) {
            addressee = strdup(addr);
            CHECK(addressee != NULL, "engine: OOM copying the timeline a peer addressed a cross-agent "
                                     "operation to — without it the operation would be performed by every "
                                     "timeline this instance has, including the ones that peer has already "
                                     "refused to hear from");
        }
    }
    remote_op_free(op);

    n = flow_count();
    DCHECK(n > 0, "a cross-agent operation was asked of a document whose every timeline had already finished — "
                  "a document a peer still holds a reference into can still be asked, so its flows must stay "
                  "live while that reference exists: build that before this can be answered");
    for (int i = 0; i < n; i++) {
        Flow *f = flow_at(i);
        /* A timeline the addressee contradicts is not one of them: the mirror of deliver_admits, over live
           flows instead of one flow's commitments, with world.h's criterion (a continuation is SAME or ANCESTOR;
           anything else is CONTRADICT). It is not identity: a fork retires its world and mints children for
           both arms, so every live world is a leaf, and the world the asker was answered from stops naming any
           flow the first time that timeline branches; its descendants are that timeline continued. Independent
           is not a refusal either (two forests, or a generation a park ended): uncertainty keeps the arm, so a
           wrong-agent addressee narrows nothing. */
        if (addressee != NULL && world_vec_relate_held(addressee, f->world) == WORLD_REL_CONTRADICT) continue;
        attached++;
        g_perform_attached++;
        /* Appended, because successive operations are sequential: the asking side parks on each in turn, and
           each is answered under its own token, which rides the program's row from the moment the operation
           starts (flow.h). Two questions from one peer in turn are not a fork. */
        perform_q_push(ctx, f, record, token);
        /* Askable again, which engine_set_referenced depends on: a referenced document's last timeline reports
           host-owed instead of finishing so it is waiting when an operation arrives, and this is the arrival. */
        flow_clear_host_owed(f);
    }
    free(addressee);
    /* It reached somebody: the addressed timeline is among the live ones, a stronger claim than the DCHECK
       above. It fails when every continuation of the answering timeline has left the frontier while siblings
       still run, leaving the asking flow parked at the read. The capability to build is keeping the timelines a
       document answered from alive while a peer holds a reference; never widen the refusal to admit a
       contradicting timeline, which would answer out of a world neither agent was in. */
    DCHECK(attached > 0,
           "a cross-agent operation named a timeline of this document that every LIVE flow CONTRADICTS — the "
           "peer addressed the world one of our flows answered its last question from, and every "
           "continuation of that world has since left the frontier while other timelines of this document "
           "are still running. The asking flow is now parked on a question no timeline of ours will answer");
    (void)attached;
}


const char *engine_host_notices(void) {
    static char *drained;
    free(drained);
    drained = g_notices;
    g_notices = NULL;
    g_notices_n = g_notices_cap = 0;
    return drained ? drained : "";
}

/* See engine.h. The same drain as the notices one, kept apart for the reason at the buffer: the platform speaks
   to the zone that routes it, the instrument to the driver that asked. */
const char *engine_take_dumps(void) {
    static char *drained;
    free(drained);
    drained = g_dumps;
    g_dumps = NULL;
    g_dumps_n = g_dumps_cap = 0;
    return drained ? drained : "";
}

/* Is this a method? Fetch §2.2.1 Methods: "a byte sequence that matches the method token production", RFC 9110
   §5.6.2 Tokens. Asked of what the host hands back at the provide edge: a delivery keyed on the pair cannot
   survive a key that is not the join's shape, and a host sending an address where the method goes would match
   nothing, silently. Not under `#if APICLIENT_DEV`: a DCHECK's condition still type-checks in release
   (`sizeof(cond)`), so a helper only a DCHECK calls must still be declared there. */
static inline int method_is_token(const char *m) {
    static const char TCHAR_EXTRA[] = "!#$%&'*+-.^_`|~";
    if (!m || !*m) return 0;
    for (; *m; m++) {
        unsigned char c = (unsigned char)*m;
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            memchr(TCHAR_EXTRA, c, sizeof TCHAR_EXTRA - 1))
            continue;
        return 0;
    }
    return 1;
}

/* Fetch §2.2.5 Requests' destination type is asked of core/fetch/fetch.h, which owns the enumeration, in both
   directions: the join will not write a value the spec does not define and the split will not believe one.
   `method_is_token` stays static until it has a second caller, then moves to the component whose record carries
   the method. */

/* Is this destination script-like? §2.2.5: "A request's destination is script-like if it is `audioworklet`,
   `paintworklet`, `script`, `serviceworker`, `sharedworker`, or `worker`." CORB is decided by the trusted zone
   (safe-fetch.js asks the same question); here it decides which member of a deduplicated set the line states. */
static int destination_is_script_like(const char *d, size_t n) {
    static const char *const SCRIPT_LIKE[] = {
        "audioworklet", "paintworklet", "script", "serviceworker", "sharedworker", "worker"
    };
    size_t i;
    for (i = 0; i < sizeof SCRIPT_LIKE / sizeof *SCRIPT_LIKE; i++)
        if (n == strlen(SCRIPT_LIKE[i]) && !memcmp(d, SCRIPT_LIKE[i], n)) return 1;
    return 0;
}

/* Replaces one field of an already-joined line, shifting what follows: the dedup's one write half for every token
   field. No field can be overwritten in place: the destination vocabulary is the spec's (`` -> `script` grows the
   line by six bytes), the provenance's three tokens have three widths, and the initiator pair's equal width is a
   coincidence not worth asserting. A caller rewriting several fields of one line goes back to front, since a
   rewrite shifts everything after it. `off` is an offset, not a pointer, because growth may realloc the buffer. */
static void join_set_field(char **join, size_t *n_out, size_t *cap, size_t off, size_t old_len,
                           const char *tok) {
    size_t tl = strlen(tok), tail_at = off + old_len, tail_len;

    DCHECK(tail_at <= *n_out, "the join was asked to rewrite a field that ends past the text it has written — "
                              "the offset came from a line in another buffer");
    tail_len = *n_out - tail_at;
    /* Guarded on growth rather than written as one expression: with a shrinking field, `*n_out + tl - old_len`
       wraps in size_t and only happens to come back right. A shrinking field needs no room. */
    while (tl > old_len && *n_out + (tl - old_len) + 1 > *cap) {
        *cap *= 2;
        *join = realloc(*join, *cap);
        CHECK(*join, "engine: OOM widening a pending line's field");
    }
    if (tl != old_len) memmove(*join + off + tl, *join + tail_at, tail_len + 1);   /* +1 carries the NUL */
    memcpy(*join + off, tok, tl);
    *n_out += tl - old_len;
}

/* The provenance's wire spelling, the one place the record's number becomes the line's token, so engine.h's
   vocabulary and pending.h's numbering cannot drift apart. Exported because the @H surface prints the same
   three words (engine.h). */
const char *engine_provenance_token(int prov) {
    switch (prov) {
    case PROV_OBSERVED: return PENDING_PROVENANCE_OBSERVED;
    case PROV_DERIVED:  return PENDING_PROVENANCE_DERIVED;
    case PROV_FORCED:   return PENDING_PROVENANCE_FORCED;
    }
    /* A CHECK, not a DCHECK, for engine_pending_split's reason: the trusted zone's firing decision reads this
       field, and a release fallthrough would emit whatever was left in the register. */
    CHECK_FAILF("engine: a park's provenance is %d, which is none of the three pending.h defines — the trusted "
                "zone's firing decision reads this field", prov);
}

/* See engine.h. The same three words for an act that is not a park, through the same mapping, so a navigation or
   a route declaration cannot spell a provenance a pending line could not. It reads the running flow rather than
   taking one, because the fact is about the path standing here; `flow_path_forced` is monotone and idempotent
   (solver/flow.h), so the answer is the path's at the moment of the act, and the answer travels with the
   operation, never the lookup. */
int engine_prov_of_running_path(void)
{
    const Flow *f = flow_running();

    return f != NULL && flow_path_forced(f) ? PROV_FORCED : PROV_DERIVED;
}

const char *engine_provenance_of_running_path(void)
{
    return engine_provenance_token(engine_prov_of_running_path());
}

/* See engine.h. The witness mark's wire spelling, in the one place the bit becomes the line's token. */
const char *engine_pinned_token(int pinned)
{
    switch (pinned) {
    case 0: return PENDING_PINNED_NO;
    case 1: return PENDING_PINNED_YES;
    }
    /* A CHECK, not a DCHECK, for engine_provenance_token's reason: the firing decision reads this field. The
       mark comes from `flow_path_pinned`, written in one place as a constant, so a third value is memory this
       field does not own, and the next step is a security decision made from it. */
    CHECK_FAILF("engine: a request's witness mark is %d, which is neither bit engine.h defines — the trusted "
                "zone's firing decision reads this field", pinned);
}

/* See engine.h for why the flow-less arm is not free to be the safe word. It is 0 because
   engine_prov_of_running_path answers `derived` on the same state and `path_pinned` is nested inside
   `path_forced`; a 1 would compose a pair on which safe-fetch.js's `_firingRefusal` kills the fetch path in
   release, where this DCHECK cannot name the caller first. */
int engine_pinned_of_running_path(void)
{
    const Flow *f = flow_running();

    DCHECK(f != NULL,
           "a request built by running the page's code asked whether its address may rest on a witness this "
           "engine chose, with no flow standing — the witness is a fact about ONE path, so there is no path "
           "here to have chosen one, and every caller of this reaches it while the page's own code is what is "
           "happening (core/xhr/xml_http_request.c's §3.5.6 send is the first). An arrival here is a route "
           "that composed a request outside a flow and would be told `unpinned`, which is the word that lets "
           "a widened origin SPEND the act");
    return f != NULL && flow_path_pinned(f);
}

/* The same trip back, for a token already in the join; length-delimited because the caller holds a field of a
   line, not a NUL-terminated string. */
static int prov_of_token(const char *tok, size_t n) {
    if (n == sizeof PENDING_PROVENANCE_OBSERVED - 1 && !memcmp(tok, PENDING_PROVENANCE_OBSERVED, n))
        return PROV_OBSERVED;
    if (n == sizeof PENDING_PROVENANCE_DERIVED - 1 && !memcmp(tok, PENDING_PROVENANCE_DERIVED, n))
        return PROV_DERIVED;
    if (n == sizeof PENDING_PROVENANCE_FORCED - 1 && !memcmp(tok, PENDING_PROVENANCE_FORCED, n))
        return PROV_FORCED;
    CHECK_FAILF("engine: a line this join wrote carries the provenance token `%.*s`, which it cannot have "
                "written — the buffer is this function's alone", (int)n, tok);
}

/* The same trip back for Fetch §2.2.5's credentials mode, length-delimited for `prov_of_token`'s reason, and
   derived from core/fetch/fetch.h's own wire spelling (`fetch_credentials_token`'s and nobody else's) rather
   than a second list of the three words. */
static FetchCredentialsMode cred_of_token(const char *tok, size_t n) {
    FetchCredentialsMode m;
    for (m = FETCH_CREDENTIALS_OMIT; m <= FETCH_CREDENTIALS_INCLUDE; m++) {
        const char *w = fetch_credentials_token(m);
        if (n == strlen(w) && !memcmp(tok, w, n)) return m;
    }
    CHECK_FAILF("engine: a line this join wrote carries the credentials token `%.*s`, which it cannot have "
                "written — the buffer is this function's alone", (int)n, tok);
}

/* The set's credentials mode is its least credentialed member. The dedup is over (method, url), so one reply
   satisfies every park in the set; the initiator and provenance fold toward the most observed (a claim about
   evidence), but this is a claim about whose session pays, and the errors are asymmetric: fetching credentialed
   for a set containing an `omit` park hands that park a personalised body its algorithm forbade, while fetching
   uncredentialed for an `include` park only narrows. The fold is written over the enumerators, not their
   numbers, because core/fetch/fetch.h does not declare its order a rank.
   Named residual. Not covered: two parks on one (method, url) with different modes are two requests a browser
   makes twice, and this folds them into one. Next diff builds: the mode in the dedup key beside method and url,
   with engine_provide pairing on the same key (it would otherwise answer only the first line). Absence shows
   as: a page whose `<img crossorigin=use-credentials src=u>` and `fetch(u)` name one address learns the
   logged-out body for the image, and the census counts one request where a browser's log shows two. */
static FetchCredentialsMode cred_narrower(FetchCredentialsMode a, FetchCredentialsMode b) {
    if (a == FETCH_CREDENTIALS_OMIT || b == FETCH_CREDENTIALS_OMIT) return FETCH_CREDENTIALS_OMIT;
    if (a == FETCH_CREDENTIALS_SAME_ORIGIN || b == FETCH_CREDENTIALS_SAME_ORIGIN)
        return FETCH_CREDENTIALS_SAME_ORIGIN;
    DCHECK(a == FETCH_CREDENTIALS_INCLUDE && b == FETCH_CREDENTIALS_INCLUDE,
           "a pending line's credentials mode is neither of Fetch §2.2.5 Requests' three — the unplaced zero "
           "is what a producer that stated nothing leaves, and pending_park_request refuses it, so reaching "
           "this fold with one is that door having stopped asserting");
    return FETCH_CREDENTIALS_INCLUDE;
}

/* Is this initiator token the parser-inserted one? Asked of a line's field and of the entry's own token through
   the same comparisons, so the join cannot decide it two ways. A membership test, not a length test. */
static int ini_is_parser(const char *tok, size_t n) {
    if (n == sizeof PENDING_INITIATOR_PARSER - 1 && !memcmp(tok, PENDING_INITIATOR_PARSER, n)) return 1;
    if (n == sizeof PENDING_INITIATOR_SCRIPT - 1 && !memcmp(tok, PENDING_INITIATOR_SCRIPT, n)) return 0;
    CHECK_FAILF("engine: a pending line carries the initiator token `%.*s`, which is neither token engine.h "
                "declares", (int)n, tok);
}

/* The same reader for the pinned mark, which the dedup fold asks of a line already in the buffer. Fatal on an
   unknown word, for `ini_is_parser`'s reason: "not pinned" for an unrecognised token would be the permissive arm
   of whether an act may be spent, and the buffer is written by the loop below, so an unreadable word means
   reader and writer parted. */
static int pinned_is_yes(const char *tok, size_t n) {
    if (n == sizeof PENDING_PINNED_YES - 1 && !memcmp(tok, PENDING_PINNED_YES, n)) return 1;
    if (n == sizeof PENDING_PINNED_NO - 1 && !memcmp(tok, PENDING_PINNED_NO, n)) return 0;
    CHECK_FAILF("engine: a pending line carries the pinned token `%.*s`, which is neither token engine.h "
                "declares", (int)n, tok);
}

/* The URLs the host owes, newline-joined across every live flow and deduplicated over (method, url), or "".
   Several flows park on one request (a candidate re-fire re-runs the exploring flow's fetches) and engine_provide
   fills every entry that names it, so the list is a set of requests, not of waiters; deduplicating over the URL
   alone would drop a POST behind a GET and settle the POST with the GET's body. The buffer is this function's,
   valid until the next call. Each line is
   `METHOD<TAB>DESTINATION<TAB>INITIATOR<TAB>PROVENANCE<TAB>PINNED<TAB>CREDENTIALS<TAB>URL`. */
const char *engine_pending_fetches(void) {
    static char *join;
    static size_t cap;
    size_t n_out = 0;
    Flow *f;

    /* One pass and one conversion, for engine_host_requests' reason. */
    if (!join) { cap = 256; join = malloc(cap); CHECK(join, "engine: OOM joining the pending requests"); }
    join[0] = 0;
    for (int k = 0; (f = flow_at(k)) != NULL; k++)
        for (int i = 0, n = pending_count(f->pending); i < n; i++) {
            JSValue pe = pending_entry(f->pending, i);
            JSValue uv = pending_get(pe, PEND_URL);
            JSValue mv = pending_get(pe, PEND_METHOD);
            JSValue dv = pending_get(pe, PEND_DESTINATION);
            JSValue cv = pending_get(pe, PEND_CREDENTIALS);
            size_t ul = 0, ml = 0, dl = 0, cl = 0;
            const char *u = JS_IsString(uv) ? JS_ToCStringLen(pending_ctx(), &ul, uv) : NULL;
            const char *m = JS_IsString(mv) ? JS_ToCStringLen(pending_ctx(), &ml, mv) : NULL;
            /* Fetch §2.2.5's destination, off the record the park stamped it on, not derived from the kind: one
               kind of park can carry opposite answers to "may this reply be compiled". */
            const char *d = JS_IsString(dv) ? JS_ToCStringLen(pending_ctx(), &dl, dv) : NULL;
            /* The credentials mode, off the same record for the same reason: only the creating algorithm knows
               it. pending.h's default is JS_NULL and `pending_park_request` always writes it, so a NULL here is a
               park that never took an address, already dropped by `skip` on `!u`. */
            const char *c = JS_IsString(cv) ? JS_ToCStringLen(pending_ctx(), &cl, cv) : NULL;
            /* HTML §4.12.1.1's `parser document`, read off the record (pending.h's `parserIns`); engine.h's
               tokens say why the engine states it and does not act on it. Not derived from the kind: §4.12.1.1
               puts a non-parser-inserted element in the in-order list when the async setter clears force async,
               and a parser-inserted `<script async src>` in the ASAP set. Reading the pusher's flag also keeps
               this token and the provenance beside it from being two derivations of one fact. */
            const char *ini = pending_get_int(pe, PEND_PARSER_INS)
                              ? PENDING_INITIATOR_PARSER : PENDING_INITIATOR_SCRIPT;
            /* What the request is evidence of, read off the record (pending.h's PROV field) and never
               recomputed: it was true at the instant the request was built, and this walk runs later, over a flow
               that may since have taken a contradicted arm. */
            int prov_v = (int)pending_get_int(pe, PEND_PROV);
            const char *prov = engine_provenance_token(prov_v);
            /* Whether the address may rest on a witness this engine chose, off the record for the provenance's
               reason: a pin taken after parking could not have reached this address's bytes. */
            int pinned_v = (int)pending_get_int(pe, PEND_PINNED);
            const char *pinned = engine_pinned_token(pinned_v);
            size_t il = strlen(ini), pl = strlen(prov), nl = strlen(pinned);
            /* A request the zone already refused is not listed, which keeps a decline a fork rather than a spin:
               unanswered entries are re-joined every step, and each decline reaching the engine forks another
               failure arm. A refusal answers "will you make this request", and this list is what the host can
               still be asked; the record keeps the reason, and the parked flow its snapshot. */
            int skip = (!u || pending_get_int(pe, PEND_HAVE_VALUE) || pending_entry_declined(pe));
            if (!skip) {
                char *q = join, *stop = join + n_out;
                /* A synchronous host request has no address and belongs to engine_host_requests: listed here, the
                   host would fetch a URL to settle a rendezvous that answers a program. It is skipped by carrying
                   no URL, so this asserts the reason. */
                DCHECK(pending_get_int(pe, PEND_KIND) != FLOW_PENDING_HOSTREQ,
                       "a synchronous host request carries an ADDRESS — it is answered by engine_host_answer "
                       "against a request id, not by a fetched body against a (method, url) pair, so listing "
                       "it here would have the host answer one seam's question through the other");
                /* The producer's half, asserted where the record is read: an address with no method is a dropped
                   field, and the split could not make sense of the line. */
                DCHECK(m != NULL,
                       "an outstanding request carries an ADDRESS and no METHOD — the reply seam is keyed on the "
                       "pair, so this entry cannot be listed at all. The park that created it must state its "
                       "method (Fetch §2.2.5 Requests: unless stated otherwise it is `GET`)");
                /* The line grammar holds by construction and is checked: URL Standard §4.4 URL parsing removes
                   all ASCII tab and newline, and a method is an RFC 9110 §5.6.2 token. A URL that breaks it never
                   went through the parser (a concolic shape carried as an address), and the line would split into
                   two records the host fetches. */
                DCHECK(!memchr(u, '\t', ul) && !memchr(u, '\n', ul),
                       "an outstanding request's URL holds a TAB or a NEWLINE — URL Standard §4.4 URL parsing "
                       "removes both from its input, so this string never went through the parser and the joined "
                       "line splits into records nobody parked on");
                DCHECK(method_is_token(m),
                       "an outstanding request's METHOD is not a token — Fetch §2.2.1 Methods, and RFC 9110 "
                       "§5.6.2 Tokens is the production; the joined line would not split back into the pair");
                /* The destination half of the contract: without it the request cannot be classified at the
                   chokepoint, and is fetched as data, so a script park's cross-origin body would be compiled. */
                DCHECK(d != NULL,
                       "an outstanding request carries an ADDRESS and no DESTINATION — Fetch §2.2.5 Requests "
                       "gives every request one and the trusted zone reads it to decide whether the reply may "
                       "be ingested as CODE. The park that created it must state it (core/fetch/fetch.h's "
                       "request record carries the field; `` is a real answer and means DATA)");
                DCHECK(fetch_is_destination_type(d),
                       "an outstanding request's DESTINATION is not one Fetch §2.2.5 Requests enumerates — the "
                       "consumer decides the CORB class by asking whether this value is SCRIPT-LIKE, so a "
                       "value outside the enumeration is read as `not code` by default and a script's reply is "
                       "ingested as data");
                /* The credentials half: the park writes the field unconditionally, so a NULL means the park door
                   was bypassed, a different repair from a producer that stated nothing. */
                DCHECK(c != NULL,
                       "an outstanding request carries an ADDRESS and no CREDENTIALS MODE — the field is "
                       "written unconditionally by pending_park_request (this file) after it has refused the "
                       "unplaced zero, so a record with an address and no mode is that door having been "
                       "bypassed. Route the park through it; a producer that merely stated nothing aborts "
                       "there instead, naming the algorithm that owes the answer");
                /* The enumeration is checked by making the call: `cred_of_token` is fatal on a word §2.2.5 does
                   not define (whether the person's session pays is at stake), so a DCHECK around it would compare
                   a value with the only value it can return. */
                (void)cred_of_token(c, cl);
                /* Fetch §4.3 Scheme fetch's closure, at the consumer. The trusted zone fetches only HTTP(S)
                   (Fetch §2.1 URL) and its refusal looks like a network failure, so a URL §4.3 answers inside
                   this agent reaching this line is a park that never ran §4.3, owed an answer that can only be
                   `blocked`. Asked once here, of everything, a park added later fires.
                   `pending_park_request` places the local answer; core/fetch/fetch.c's `fetch_owe` handles a
                   request with a `deliver` closure. */
                DCHECK(scheme_fetch_is_network(pending_ctx(), u),
                       "a request whose URL's scheme Fetch §4.3 Scheme fetch answers INSIDE this agent is "
                       "about to be handed to the HOST'S network. The trusted zone fetches nothing but an "
                       "HTTP(S) scheme (Fetch §2.1 URL) and its refusal is indistinguishable from a network "
                       "failure, so this flow would park for the rest of the session on an answer that can "
                       "only be `blocked`. The park that created this entry did not run §4.3 — route it "
                       "through pending_park_request (this file), which places §4.3's own response on the "
                       "record as PEND_VALUE + haveValue and the delivery reads it exactly as it reads the "
                       "host's");
                while (q < stop) {
                    char *e = memchr(q, '\n', (size_t)(stop - q));
                    char *lend = e ? e : stop;
                    /* The line is split where it was joined, by scanning TABs: neither a method (RFC 9110 §5.6.2)
                       nor a URL (URL Standard §4.4) can contain one, so the scan is exact whatever the token
                       fields' widths. */
                    char *t1 = memchr(q, '\t', (size_t)(lend - q));
                    char *t2 = t1 ? memchr(t1 + 1, '\t', (size_t)(lend - t1 - 1)) : NULL;
                    char *t3 = t2 ? memchr(t2 + 1, '\t', (size_t)(lend - t2 - 1)) : NULL;
                    char *t4 = t3 ? memchr(t3 + 1, '\t', (size_t)(lend - t3 - 1)) : NULL;
                    char *t5 = t4 ? memchr(t4 + 1, '\t', (size_t)(lend - t4 - 1)) : NULL;
                    char *t6 = t5 ? memchr(t5 + 1, '\t', (size_t)(lend - t5 - 1)) : NULL;
                    DCHECK(t6 != NULL,
                           "a line already in this join carries fewer than six TABs — the buffer is this "
                           "function's alone and every line it writes is `METHOD<TAB>DESTINATION<TAB>"
                           "INITIATOR<TAB>PROVENANCE<TAB>PINNED<TAB>CREDENTIALS<TAB>URL`, so this is the writer and the "
                           "reader having parted");
                    if ((size_t)(t1 - q) == ml && !memcmp(q, m, ml) &&
                        (size_t)(lend - (t6 + 1)) == ul && !memcmp(t6 + 1, u, ul)) {
                        /* The set's initiator is the most observed of its members, a statement about the
                           request: one reply fills every entry on the pair, so when a document's `<script src>`
                           and an arm's `fetch()` name one address the line states the fact that survives both (a
                           real load makes this request), independent of walk order. The upgrade is monotone.
                           The provenance folds the same way, most observed rather than most forced: if any
                           member's path stood on no contradicted arm, a real client makes exactly this request
                           and the server's answer is evidence about the app; labelling it forced would be false
                           and would cost active discovery. What a forced flow does with the reply is that flow's
                           `path_forced` fact, not the bytes'.
                           The destination folds differently, toward the strictest ingestion: if any member will
                           compile the reply, the bytes must pass the code rule (a cross-origin non-JS body
                           refused for a `fetch()` is a narrowing; accepted for a `<script src>` it is code
                           execution), so script-like (§2.2.5) wins. Where no member is script-like the line keeps
                           what it has, since no consumer distinguishes, say, `image` from `font`; a consumer that
                           does would need the register keyed on the destination.
                           Every offset and verdict is taken before the first write, and the five writes run back
                           to front (credentials, witness, provenance, initiator, destination), because a rewrite
                           shifts everything after its field. */
                        size_t d_at = (size_t)(t1 + 1 - join), d_len = (size_t)(t2 - (t1 + 1));
                        size_t i_at = (size_t)(t2 + 1 - join), i_len = (size_t)(t3 - (t2 + 1));
                        size_t p_at = (size_t)(t3 + 1 - join), p_len = (size_t)(t4 - (t3 + 1));
                        size_t n_at = (size_t)(t4 + 1 - join), n_len = (size_t)(t5 - (t4 + 1));
                        size_t c_at = (size_t)(t5 + 1 - join), c_len = (size_t)(t6 - (t5 + 1));
                        /* The (provenance, witness) pair folds as a pair, taking one member's whole, so it
                           cannot state a pair no member held. The facts are nested (solver/flow.h declares
                           `path_pinned` inside `path_forced`; `pending_pinned_compose` asserts it), so the legal
                           pairs are (observed|derived|forced, unpinned) and (forced, pinned). Folding each by its
                           own extremum could give (derived, pinned), on which extension/lib/safe-fetch.js's
                           `_firingRefusal` CHECK kills the fetch path for the session; a candidate re-fire
                           (forced and pinned, re-running the explorer's fetches) generates exactly such a set.
                           The order is lexicographic, provenance first, so the provenance still folds toward
                           most observed, and the witness mark is read off the member that won. Taking the
                           cleanest member is sound for the mark: it is an over-approximation recorded at the
                           flow and cannot be read off the address (`pin_mint` returns a bare primitive), so a
                           member that composed the same bytes with its mark clear refutes it for this address. */
                        /* Every reader below takes the computed offset, never a `tN + 1` of its own, so the
                           offsets and tab pointers are not two spellings of one position that could drift. */
                        int cur_prov = prov_of_token(join + p_at, p_len);
                        int cur_pinned = pinned_is_yes(join + n_at, n_len);
                        int take_new = prov_v < cur_prov || (prov_v == cur_prov && !pinned_v);
                        int mo_prov = take_new ? prov_v : cur_prov;
                        int mo_pinned = take_new ? pinned_v : cur_pinned;
                        /* The nesting is checked where the pair is made, the one place able to compose a pair
                           neither the park nor the chokepoint saw; it fails if either field is again folded by
                           an extremum of its own. */
                        DCHECKF(!mo_pinned || mo_prov == PROV_FORCED,
                                "this join folded a (provenance, witness) pair no park can hold — provenance "
                                "%d with the witness mark SET, where solver/flow.h declares `path_pinned` "
                                "strictly inside `path_forced`. The fold takes ONE member's pair whole for "
                                "exactly this reason, so a pair that disagrees is a field folded by an "
                                "extremum of its own again, and extension/lib/safe-fetch.js's "
                                "`_firingRefusal` CHECK kills the fetch path on the line this writes",
                                mo_prov);
                        int mo_parser = ini_is_parser(join + i_at, i_len) || ini_is_parser(ini, il);
                        int widen_dst = destination_is_script_like(d, dl) &&
                                        !destination_is_script_like(join + d_at, d_len);
                        /* The credentials mode folds the other way from the token fields (see `cred_narrower`
                           for the rule and its residual). */
                        FetchCredentialsMode mo_cred = cred_narrower(cred_of_token(join + c_at, c_len),
                                                                    cred_of_token(c, cl));
                        join_set_field(&join, &n_out, &cap, c_at, c_len, fetch_credentials_token(mo_cred));
                        join_set_field(&join, &n_out, &cap, n_at, n_len,
                                       engine_pinned_token(mo_pinned));
                        join_set_field(&join, &n_out, &cap, p_at, p_len, engine_provenance_token(mo_prov));
                        join_set_field(&join, &n_out, &cap, i_at, i_len,
                                       mo_parser ? PENDING_INITIATOR_PARSER : PENDING_INITIATOR_SCRIPT);
                        if (widen_dst) join_set_field(&join, &n_out, &cap, d_at, d_len, d);
                        /* Both rewrites can realloc, so every pointer this scan holds (`q`, `stop`, `t1`..`t6`) is
                           dead from here; the break is what makes that safe. */
                        skip = 1; break;
                    }
                    if (!e) break;
                    q = e + 1;
                }
            }
            if (!skip) {
                while (n_out + ml + dl + il + pl + nl + cl + ul + 8 > cap) {
                    cap *= 2;
                    join = realloc(join, cap);
                    CHECK(join, "engine: OOM growing the pending-request join");
                }
                memcpy(join + n_out, m, ml); n_out += ml;
                join[n_out++] = '\t';
                memcpy(join + n_out, d, dl); n_out += dl;
                join[n_out++] = '\t';
                memcpy(join + n_out, ini, il); n_out += il;
                join[n_out++] = '\t';
                memcpy(join + n_out, prov, pl); n_out += pl;
                join[n_out++] = '\t';
                memcpy(join + n_out, pinned, nl); n_out += nl;
                join[n_out++] = '\t';
                memcpy(join + n_out, c, cl); n_out += cl;
                join[n_out++] = '\t';
                memcpy(join + n_out, u, ul); n_out += ul;
                join[n_out++] = '\n';
                join[n_out] = 0;
            }
            if (u) JS_FreeCString(pending_ctx(), u);
            if (m) JS_FreeCString(pending_ctx(), m);
            if (d) JS_FreeCString(pending_ctx(), d);
            if (c) JS_FreeCString(pending_ctx(), c);
            JS_FreeValue(pending_ctx(), cv);
            JS_FreeValue(pending_ctx(), dv);
            JS_FreeValue(pending_ctx(), mv);
            JS_FreeValue(pending_ctx(), uv);
            JS_FreeValue(pending_ctx(), pe);
        }
    return join;
}

void engine_pending_split(char *line, const char **method, const char **destination, const char **initiator,
                          const char **provenance, const char **pinned, const char **credentials,
                          const char **url) {
    char *tab, *tab2, *tab3, *tab4, *tab5, *tab6;

    DCHECK(line && method && destination && initiator && provenance && pinned && credentials && url,
           "a pending line was split with nowhere to put its fields");
    tab = strchr(line, '\t');
    /* CHECKs, not DCHECKs: release has no defined answer, because the fields key the delivery, and a line not
       in this engine's grammar would key it on a method nobody asked for. Each missing TAB is checked on its own
       so the message says which field would have been misread; a URL cannot contain a TAB (URL Standard §4.4),
       so the URL is the remainder of the line. */
    CHECK(tab != NULL,
          "engine: a host split a pending line that carries no TAB — engine_pending_fetches joins "
          "`METHOD<TAB>DESTINATION<TAB>INITIATOR<TAB>PROVENANCE<TAB>PINNED<TAB>CREDENTIALS<TAB>URL` lines and this "
          "one is none of that");
    tab2 = strchr(tab + 1, '\t');
    /* Without the second TAB the initiator would become the address. */
    CHECK(tab2 != NULL,
          "engine: a host split a pending line carrying one TAB — engine_pending_fetches joins "
          "`METHOD<TAB>DESTINATION<TAB>INITIATOR<TAB>PROVENANCE<TAB>PINNED<TAB>CREDENTIALS<TAB>URL` and this host is "
          "reading a shorter grammar that preceded it, so its address is about to be a destination");
    tab3 = strchr(tab2 + 1, '\t');
    /* Without the third the provenance would become the address, and the firing decision could not see whether
       a running-code park was derived or forced. */
    CHECK(tab3 != NULL,
          "engine: a host split a pending line carrying two TABs — engine_pending_fetches joins "
          "`METHOD<TAB>DESTINATION<TAB>INITIATOR<TAB>PROVENANCE<TAB>PINNED<TAB>CREDENTIALS<TAB>URL` and this host is "
          "reading a shorter grammar that preceded it, so its address is about to be an initiator token");
    tab4 = strchr(tab3 + 1, '\t');
    /* Without the fourth the CORB class would be decided on a field that is not there. */
    CHECK(tab4 != NULL,
          "engine: a host split a pending line carrying three TABs — engine_pending_fetches joins "
          "`METHOD<TAB>DESTINATION<TAB>INITIATOR<TAB>PROVENANCE<TAB>PINNED<TAB>CREDENTIALS<TAB>URL` and this host is "
          "reading the four-field grammar that preceded it, so its address is about to be a provenance token "
          "and its CORB class is about to be decided on a field that is not there");
    tab5 = strchr(tab4 + 1, '\t');
    /* Without the fifth the credentials mode would become the address, and whose session pays would be
       decided by silence (Fetch §2.2.5 Requests: only the creating algorithm knows it). */
    CHECK(tab5 != NULL,
          "engine: a host split a pending line carrying four TABs — engine_pending_fetches joins "
          "`METHOD<TAB>DESTINATION<TAB>INITIATOR<TAB>PROVENANCE<TAB>PINNED<TAB>CREDENTIALS<TAB>URL` and this host is "
          "reading the five-field grammar that preceded it, so its address is about to be a credentials "
          "token and whose session pays for this fetch is about to be decided on a field that is not there");
    tab6 = strchr(tab5 + 1, '\t');
    /* Without the sixth the pinned mark would become the address, and the zone could not tell a lazy chunk of
       the page's own bundle from a data request whose address rests on a witness this engine picked. */
    CHECK(tab6 != NULL,
          "engine: a host split a pending line carrying five TABs — engine_pending_fetches joins "
          "`METHOD<TAB>DESTINATION<TAB>INITIATOR<TAB>PROVENANCE<TAB>PINNED<TAB>CREDENTIALS<TAB>URL` and this "
          "host is reading the six-field grammar that preceded it, so its address is about to be a pinned "
          "token and whether this act may be spent is about to be decided on a field that is not there");
    *tab = 0;
    *tab2 = 0;
    *tab3 = 0;
    *tab4 = 0;
    *tab5 = 0;
    *tab6 = 0;
    *method = line;
    *destination = tab + 1;
    *initiator = tab2 + 1;
    *provenance = tab3 + 1;
    *pinned = tab4 + 1;
    *credentials = tab5 + 1;
    *url = tab6 + 1;
    /* Fetch §2.2.5's enumeration, asked of the line: the join asked it before writing, and both parties check a
       contract. The empty string passes: it is §2.2.5's default and what `fetch()` has. */
    DCHECK(fetch_is_destination_type(*destination),
           "a pending line's DESTINATION is not one Fetch §2.2.5 Requests enumerates — the host decides from "
           "it whether this reply may be ingested as CODE, and a value it does not know takes the `not "
           "script-like` arm, which is how a script's cross-origin body gets read as data and compiled");
    DCHECK(!strcmp(*pinned, PENDING_PINNED_YES) || !strcmp(*pinned, PENDING_PINNED_NO),
           "a pending line's PINNED MARK is neither of the two tokens engine.h declares — a zone reads this "
           "field to decide whether an ACT MAY BE SPENT on an address that may rest on a witness this engine "
           "chose, and a value it does not know would be answered by whichever of its arms is the default, "
           "which for a firing question is the permissive one");
    DCHECK(!strcmp(*initiator, PENDING_INITIATOR_PARSER) || !strcmp(*initiator, PENDING_INITIATOR_SCRIPT),
           "a pending line's INITIATOR is neither of the two tokens engine.h declares — a zone reads this "
           "field for what a REAL LOAD of the document makes, and a value it does not know would be answered "
           "by whichever of its arms happened to be the default");
    DCHECK(!strcmp(*provenance, PENDING_PROVENANCE_OBSERVED) ||
           !strcmp(*provenance, PENDING_PROVENANCE_DERIVED) ||
           !strcmp(*provenance, PENDING_PROVENANCE_FORCED),
           "a pending line's PROVENANCE is none of the three tokens engine.h declares — the trusted zone's "
           "FIRING decision reads this field, and CLAUDE.md forbids a reply to a FORCED request ever being "
           "reported as OBSERVED, which a defaulted fourth value is exactly how it would be");
    /* The credentials mode, through core/fetch/fetch.h's one mapping, `fetch_credentials_of_token`, which is
       fatal on an undefined word, so the call is the check. */
    (void)fetch_credentials_of_token(*credentials);
}

/* Is anything outstanding for the host on the whole frontier? The scheduler asks this before it may call a
 * frontier exhausted. It is the engine's own question, not a host callback: the lists a host would answer from
 * are this file's walks of this file's registers (engine_pending_fetches, engine_host_requests), and a host
 * restating them can omit one; a frontier suspended entirely inside cross-instance reads must report stalled.
 *
 * Asked of the flows through `pending_host_outstanding`, the register-arity form of the joins' own skip
 * condition, so the two cannot drift. That is not `pending_outstanding` (pend_owed, "is this flow still
 * waiting"): the two differ on a declined entry, which the joins do not list, so asking pend_owed would report
 * a stall the host is handed no record for, a livelock in release and main.c's "the scheduler asked this host to
 * PAY and named nothing owed" in dev. The dev walk asserts every outstanding entry is tellable. */
int engine_host_owes(void) {
    for (int i = 0; i < flow_count(); i++) {
        Flow *f = flow_at(i);
        if (!pending_host_outstanding(f->pending)) continue;
#if APICLIENT_DEV
        for (int j = 0, m = pending_count(f->pending); j < m; j++) {
            JSValue e = pending_entry(f->pending, j);
            JSValue uv = pending_get(e, PEND_URL);
            /* A declined entry is deliberately untellable and is not a defect. Otherwise every unanswered entry
               must be on one of the two lists the host is handed, or its flow waits for the session. A refusal
               makes the wait deliberate: the zone answered no, the joins stop listing it so it is not re-asked,
               and the flow keeps its snapshot, a parked search that fires the day the origin is widened; the
               record's `declined` reason accounts for it. */
            int tellable = pending_get_int(e, PEND_HAVE_VALUE) ||
                           pending_entry_declined(e) ||
                           JS_IsString(uv) ||
                           pending_get_int(e, PEND_KIND) == FLOW_PENDING_HOSTREQ;
            JS_FreeValue(pending_ctx(), uv);
            JS_FreeValue(pending_ctx(), e);
            DCHECK(tellable,
                   "a flow is waiting on a register entry the host can never be shown — engine_pending_fetches "
                   "lists the unanswered entries that carry an ADDRESS and engine_host_requests the unanswered "
                   "SYNCHRONOUS ones, so an entry that is neither makes this report a stall the host is handed "
                   "no record for, and the frontier waits on it for the rest of the session");
        }
#endif
        return 1;
    }
    return 0;
}

/* Is every member parked on something no host event can clear? The frontier-arity counterpart of the bill above
 * and the other answer a `-inf` can carry. They are asked in that order and never both hold: a refused member
 * is invisible to both joins, so it is never what a bill names, and one billable member answers the first.
 * Every member rather than any, because one runnable or billable member falsifies the claim.
 * `pending_declined_outstanding` (pending.h) is asked directly rather than composed as
 * `outstanding && !host_outstanding`, which differs on a mixed register. An empty frontier answers 0: exhaustion
 * is a different fact with its own exit, and a vacuous 1 would satisfy the -inf assert through that state. */
int engine_frontier_settled(void) {
    if (flow_count() == 0) return 0;
    for (int i = 0; i < flow_count(); i++)
        if (!pending_declined_outstanding(flow_at(i)->pending)) return 0;
    return 1;
}

/* Delivers a body for the request `(method, url)` into every flow parked on it. The value lands on each flow's own
   pending entry, so the reaction the resolve enqueues belongs to that flow and its COW delta. The method is half
   the key: matching on the URL alone would settle a POST's promise with a GET's reply (SECURITY.md §Network
   records the same correction for the XHR seam). Returns how many entries it filled. */
int engine_provide(JSContext *ctx, const char *method, const char *url, JSValueConst value) {
    PendIndexNode *node;
    int n = 0, matched = 0, i, m;
    /* The reply's own grade, joined from the records it answers (see the fill loop). `-1` is "no record read
       yet", which the tail refuses to hand on. */
    int reply_prov = -1;
    DCHECK(url != NULL, "a body was provided for no URL");
    /* What the host sends back must be what the join emitted: an absent method is a host not converted to the
       pair (the JS bridge's `Provide` carries one), and an address in the method's place is that host with its
       operands shifted. Both would match nothing, and the parked flow would silently wait for the session. */
    DCHECK(method != NULL,
           "a reply was provided with NO METHOD — the reply seam is keyed on `(method, url)`; the host that "
           "fetched it is the only zone that knows which request it answers, and it is handed both halves by "
           "engine_pending_fetches. A host still sending an address alone has not been converted");
    DCHECK(method_is_token(method),
           "a reply was provided with a METHOD that is not a token — Fetch §2.2.1 Methods, RFC 9110 §5.6.2 "
           "Tokens. A host sending the URL where the method goes has its operands shifted by one");
    /* One lookup in the frontier's outstanding set (solver/pending_index.h) rather than a walk of every flow's
       register: the walked set does not shrink (a flow keeps an answered entry until it delivers, and @S
       re-fires re-issue their fetches), so a walk costs every request the session ever made, per reply.
       Membership is the predicate the walk used (an entry with an address, not a synchronous request, no value
       yet), and a record leaves only when answered or when the last register naming it is gone, so nothing is
       dropped. */
    node = pending_index_find(method, url);
    /* Taken before the fills move either: `m` is the outstanding members, `matched` adds the answered ones, so
       "answered twice" (matched, none fillable) reads differently from "answered for nobody" (nothing matched).
       A fill moves a record from one term to the other, so the sum is stable across the loop. */
    m = node ? pending_index_node_count(node) : 0;
    matched = node ? m + (int)pending_index_node_answered(node) : 0;
    /* Bounded by the count above and always taking the head, because each fill removes its own record (the
       `haveValue` write reaches pending.c, which tells the index): one per iteration, so the loop visits each
       member once whatever the writes do. The closing assert checks that assumption. */
    for (i = 0; i < m; i++) {
        JSValue p = pending_index_node_member(node, 0);
        /* The index's half of its own contract, asked where it is spent: an indexed record has no answer on it,
           so an answered one here means the index kept a member the `haveValue` write should have removed, and
           this reply would overwrite a body a flow already resumed on. */
        DCHECK(!pending_get_int(p, PEND_HAVE_VALUE),
               "the frontier's outstanding set handed a reply an ALREADY-ANSWERED record — a record leaves that "
               "set on the write that answers it, so this member outlived its own answer and the body about to "
               "be written would replace one a flow has already resumed on");
        /* The producer's half of the contract the drain checks: the delivery asserts a fetch entry carries a
           §2.2.6 URL list when delivered, this asserts it did when written, so whichever fires says whether the
           host built a bad record or a good one was changed after landing (a COW or lifetime bug here). Program
           kinds are excluded (they are owed bytes and read `body`); the subresource kind is included, since an
           `<img>` or `<link>` record comes from the same two producers as a `fetch()`'s (qjs_provide's parse and
           Fetch §4.3 Scheme fetch, core/fetch/scheme_fetch.c), both of which build a URL list. */
#if APICLIENT_DEV
        if (((int)pending_get_int(p, PEND_KIND) == FLOW_PENDING_RESOLVE ||
             (int)pending_get_int(p, PEND_KIND) == FLOW_PENDING_RESOURCE) && JS_IsObject(value)) {
            JSValue ul = JS_GetPropertyStr(ctx, value, "urlList");

            DCHECKF(JS_IsArray(ul),
                    "a reply with no `urlList` is being written onto a fetch or subresource entry — the "
                    "HOST built this record, so the producer is the trusted zone's reply path and not "
                    "this file's register. request=%s %s", method, url);
            JS_FreeValue(ctx, ul);
        }
#endif
        /* What this reply is evidence of: the join of the records it answers, by the same rule and over the same
           set as the line that asked for it. A reply inherits its request's grade, and the request the host
           issued is engine_pending_fetches' deduplicated line, graded most observed over the records on the pair
           (the argument is at that join). The sets are the same by construction: the join lists a record exactly
           when it has an address and no value, which is the index's membership predicate. Most observed is `min`,
           because pending.h numbers PROV_OBSERVED < PROV_DERIVED < PROV_FORCED as the join's order. */
        {
            int rp = (int)pending_get_int(p, PEND_PROV);

            /* The range, asked through the mapping that owns it; fatal in release because a grade outside the
               vocabulary would reach the @H surface and decide whether a learned value is reported as a real
               client's. */
            (void)engine_provenance_token(rp);
            if (reply_prov < 0 || rp < reply_prov) reply_prov = rp;
        }
        pending_set(p, PEND_VALUE, JS_DupValue(ctx, value));
        pending_set(p, PEND_HAVE_VALUE, JS_TRUE);
        n++;
        JS_FreeValue(ctx, p);
    }
    DCHECK(!node || pending_index_node_count(node) == 0,
           "a reply filled every record the frontier's outstanding set held for its request and the set still "
           "holds one — the fill is what removes a record, so a survivor is a member whose `haveValue` write "
           "did not reach this index, and the host would be shown this request again for ever");
    /* Every flow waiting on this address is askable again: the reply is on its register, the event its
       host-owed mark waits for (flow.h). The whole frontier, because the mark is per flow and the fill per
       record: N arms share one record and all observe one write, and the index is a set of requests that does
       not say which flows hold a record. flow_clear_host_owed_all is O(1) on a generation stamp. An early clear
       is the only possible error and is bounded: a flow still owed something else is picked once, reports
       host-owed again and is re-marked, with nothing dropped or reordered; a mark kept past its fact would cost
       the flow its whole timeline. */
    if (n) flow_clear_host_owed_all();
    /* Learning from replies: this is the one point every fetched reply crosses once (a URL two flows parked on
       is answered once), so learning happens here rather than per waiter. What the body says about addresses
       the page will fetch (solver/reply_decode.h) is a fact about the server, not a flow's world, so it takes
       no COW capture, like the endpoint surface. An API's own rejection (`google.rpc.Status`) is read in the
       trusted zone by extension/lib/req2proto.js: it answers a deliberately malformed request, which this
       engine, whose only network edge is a GET through safeFetch, cannot make.
       It is told the reply's grade, which everything learned carries. A missing grade crashes rather than
       being filled in: the fold runs once per fill and `n` counts the fills, so `n` with no grade is a record
       written without passing the fold, and the stand-in would be `observed`, the strongest grade. */
    DCHECKF(!n || reply_prov >= 0,
            "a reply filled %d parked record(s) and none of them yielded a provenance — the fold runs on every "
            "record this call writes, so a fill with no grade is a record that reached the write without "
            "passing it. request=%s %s", n, method, url);
    if (n) reply_decode_learn(ctx, method, url, value, reply_prov);
    /* A request answered twice, told apart from one answered for nobody. Every entry naming the request already
       has a reply, so this call wrote none. `n == 0` with nothing matched is the host's pairing being off, the
       caller's assert (it owns the paged-sale credit that can explain it); this case no credit excuses, since a
       request leaves the join once answered. It catches a host with two overlapping lists, which is why facts
       about a request travel on the request itself. */
    if (matched && !n)
        DFAILF("a reply was provided for a request every parked entry has ALREADY been answered for — the "
               "join drops a request as soon as it carries a value, so the host is answering one it was shown "
               "once, twice. request=%s %s", method, url);
    return n;
}

/* The other answer a host can give one of these requests: this zone will not ask (engine.h).
 *
 * It writes a fact and forks nothing, the split engine_host_answer makes: this runs between scheduler steps,
 * where the running flow, applied delta and live DOM head are whichever flow last ran, so an arm minted here
 * would be a stranger's timeline. The refusal lands on the record; flow_decline_fork builds the pair of arms
 * later from each parked flow's own step.
 *
 * One record is one member of the outstanding set however many arms hold it (pending_index.h), so marking the
 * pair's members refuses the request for every timeline at once, as one reply fills every entry. Each timeline
 * still owes its own arm, so the "this register has had its fork" mark is per register and not written here.
 * Every flow parked on it is askable again: a refusal is a host event, and without the clear the fork it exists
 * to make would never be taken. */
int engine_decline(JSContext *ctx, const char *method, const char *url, const char *reason) {
    PendIndexNode *node;
    int matched, marked = 0, i;
    JSValue rv;

    DCHECK(method != NULL && url != NULL,
           "a refusal was recorded for no request — a decline answers the same (method, url) pair a reply "
           "answers, because it is an answer to the same question, and one that names neither half refuses "
           "nothing and leaves every flow parked");
    DCHECK(method_is_token(method),
           "a refusal names a METHOD that is not a token — Fetch §2.2.1 Methods, RFC 9110 §5.6.2 Tokens. A "
           "host sending the URL where the method goes has its operands shifted by one");
    /* The reason is required and non-empty: it is the only account of a request nobody made, whose flow will
       not drain this session, and it decides what happens next (`blocked-signal` names the signal and value of
       the person's per-origin egress control that holds it, `blocked-destructive` a refusal nothing reopens);
       only the zone that applied the rule knows which. Nothing here matches on the string (trusted.mjs asks the
       policy for the grade, never a `statusText`). */
    DCHECK(reason != NULL && *reason != '\0',
           "a request was refused with no REASON — the party that refused is the party that knows why, and an "
           "unnamed refusal leaves a frontier parked with nothing anywhere to say which rule holds it");
    node = pending_index_find(method, url);
    matched = node ? pending_index_node_count(node) : 0;
    rv = JS_NewString(ctx, reason);
    CHECK(!JS_IsException(rv), "engine: OOM recording the trusted zone's refusal of a request");
    /* Bounded by the count above and always taking the head, as in engine_provide's fill loop: each mark removes
       its own record (the `declined` write reaches solver/pending.c, which tells the index), so the list shrinks
       by one per iteration. An increasing index would skip one member per mark (pend_unkey fills a hole with the
       last member), and a skipped member would stay listed and be refused again every round, forking a failure
       arm each time. */
    for (i = 0; i < matched; i++) {
        JSValue p = pending_index_node_member(node, 0);
        /* An already-refused record cannot be a member: the refusal takes it out of the pair. So a fresh park on
           the same address (an @S re-fire re-issues its fetches) puts an unrefused record in the node, and no
           waiting arm can have its reason replaced by one written for a different park. */
        DCHECK(!pending_entry_declined(p),
               "the frontier's outstanding set holds an ALREADY-REFUSED record — a record leaves its pair on "
               "the write that refuses it, so this member outlived its own refusal, and marking it again would "
               "replace the reason its waiting arms are going to report with one written for a different park");
        DCHECK(!pending_get_int(p, PEND_HAVE_VALUE),
               "the frontier's outstanding set holds an ANSWERED record — a record leaves that set on the "
               "write that answers it, so this member outlived its own answer and would be refused after "
               "the server had already replied to it");
        pending_set(p, PEND_DECLINED, JS_DupValue(ctx, rv));
        marked++;
        JS_FreeValue(ctx, p);
    }
    JS_FreeValue(ctx, rv);
    /* The set is empty for this pair, engine_provide's closing assert for refusals: a survivor would stay listed,
       be refused again next slice, and fork another failure arm each time. */
    DCHECK(!node || pending_index_node_count(node) == 0,
           "a refusal marked every record the frontier's outstanding set held for its request and the set "
           "still holds one — the refusal is what removes a record, so this member's `declined` write did not "
           "reach the index, and the host will be shown this request again on the next slice and refuse it "
           "again, one failure arm per round for the rest of the session");
    if (marked) flow_clear_host_owed_all();
    /* Nobody is parked on it: the shapes engine_provide's caller separates, with the same credit. A refusal for
       a pair no flow asked for is the host's pairing off by one; one for a flow paged out to the cold tier is the
       mechanism working, and consumes that sale as a reply does. */
    if (matched == 0 && !engine_take_paged_owed())
        DFAILF("a refusal was recorded for a request no flow is parked on and none was paged out — the host's "
               "pending/decline pairing is off, and refusing nothing leaves the flow that IS parked waiting "
               "for a reply this zone has already decided not to fetch. request=%s %s reason=%s",
               method, url, reason);
    return marked;
}

/* The same refusal for a synchronous request; engine.h says why it is keyed on the rendezvous and is a second
   entry rather than a third completion. The walk is engine_host_answer's, the one other writer that reaches a
   HOSTREQ record from outside the asking flow, so it is the statement of where such a record can be. It stops at
   the first match: exactly one register names an unanswered rendezvous, because a fork unshares it and mints a
   fresh id. */
int engine_host_decline(JSContext *ctx, uint32_t req, const char *reason) {
    DCHECK(req != 0, "a synchronous request with no id was refused — the id IS the request's identity for this "
                     "kind, since a host request has no address to be keyed by, so a refusal naming none "
                     "refuses nothing and leaves the asking machine parked");
    /* The reason is required, for the address-keyed refusal's reason. */
    DCHECK(reason != NULL && *reason != '\0',
           "a synchronous request was refused with no REASON — the party that refused is the party that knows "
           "why, and an unnamed refusal leaves a machine parked with nothing anywhere to say which rule holds "
           "it");
    if (!g_sess_live) return 0;
    for (int k = 0; ; k++) { Flow *f = flow_at(k); if (!f) break;
        for (int i = 0, n = pending_count(f->pending); i < n; i++) {
            JSValue p = pending_entry(f->pending, i);
            /* Matched on the host's name: a refusal is the zone answering about an id it was shown. */
            if (pending_get_int(p, PEND_KIND) != FLOW_PENDING_HOSTREQ ||
                (uint32_t)pending_get_int(p, PEND_REQ_HOST) != req) { JS_FreeValue(ctx, p); continue; }
            /* A refusal and an answer cannot both be true of one rendezvous: the asking machine would take an
               answer while a sibling arm waited for one. */
            DCHECK(!pending_get_int(p, PEND_HAVE_VALUE),
                   "a synchronous request carries BOTH an answer and a refusal — the trusted zone answered this "
                   "rendezvous and then refused it, or refused one whose answer had already been written, and "
                   "the asking machine would consume a value while an arm waited for the real one");
            DCHECK(!pending_entry_declined(p),
                   "a synchronous request was refused TWICE — the second reason would replace the one the "
                   "waiting arm is going to report, and that arm was built from the first");
            {
                JSValue rv = JS_NewString(ctx, reason);
                CHECK(!JS_IsException(rv),
                      "engine: OOM recording the trusted zone's refusal of a synchronous request");
                pending_set(p, PEND_DECLINED, rv);
            }
            JS_FreeValue(ctx, p);
            /* The flow is askable again, on the flow the refusal reached, as for an answer: a refusal is one of
               the two things that can change what it told the scheduler, and `flow_decline_fork` runs on a flow
               the scheduler picks. */
            flow_clear_host_owed(f);
            return 1;
        }
    }
    /* Nobody is parked on it. Unlike the address-keyed refusal this does not abort: that one answers a record
       the join listed, while a rendezvous id handed out by `qjs_host_requests` may outlive its asking flow, as
       `qjs_host_answer` says of the same miss ("a zero return means that flow is gone, which is not an error:
       nobody is waiting on the answer"). */
    return 0;
}
/* What kind of program a queued body is. One queue, because all of them are code the page caused to run (or, for
   the last, the host asked to run); the kind decides whether the program may fail to compile and whether
   anything reads its completion value.
   - DYN_PAGE_SCRIPT, DYN_CANDIDATE, DYN_JAVASCRIPT_URL: page programs.
   - DYN_CROSS_AGENT_OP: the engine's own text, so a compile failure is this engine's bug, and its completion is
     the answer a peer's flow is parked on.
   - DYN_SCRIPT_SRC: an external script's address, holding the position its program will run at (HTML
     §4.12.1.1's order for every document of this agent; §13.2.6.4.8 The "text" insertion mode blocks on a
     parser-blocking script). The flow waits at it (flow_step), and the reply replaces the address with source
     text and the kind with DYN_PAGE_SCRIPT (flow_deliver_one_reply).
   - DYN_SCRIPT_FAILED: an element whose §4.12.1.1 result is "null (representing an error)"; it runs nothing,
     and "execute the script element" step 4 fires `error` at it when the cursor reaches its position, so
     `onerror` is ordered by where the element stands, not when the reply landed. Its body is its address, the
     account of which load failed.
   - DYN_VALUE_DUMP: a host instrument's program (testing/render_diff.js's collector, across the ABI). A compile
     failure is the instrument's bug; the completion is dumped as JSON with unknowns on their examples
     (solver/value_dump.h) for the host to drain. A row, not a C evaluation between steps (preempt_hook asserts
     that no slice-less evaluation consults the policy), attached to every live timeline, since each flow's DOM
     answers differently and each record carries its world. */
typedef enum { DYN_PAGE_SCRIPT = 0, DYN_CANDIDATE, DYN_JAVASCRIPT_URL, DYN_CROSS_AGENT_OP,
               DYN_SCRIPT_SRC, DYN_SCRIPT_FAILED, DYN_VALUE_DUMP } DynKind;

/* Is any of this flow's pending fetches deliverable? A flow with only host-owed entries has no work and stalls
   rather than spinning on a delivery that would resolve nothing. */
static int flow_pending_ready(const Flow *f) { return pending_ready(f->pending); }
/* The arrival stamp of the oldest deliverable one, which puts the reply register inside the arrival order. Kept
   beside the predicate above because both are read on one line of flow_step, and solver/pending.c writes the
   walk once. */
static uint64_t flow_pending_ready_next_seq(const Flow *f) {
    return pending_ready_next_seq(f->pending);
}

/* HTML §8.1.4.2's processResponseConsumeBody step that makes source text from a fetched byte sequence. Undecoded
   bytes would turn one invalid UTF-8 byte into a SyntaxError for a whole bundle, where Encoding's decoder
   answers U+FFFD. Which algorithm runs is §8.1.4.2's split: a classic script decodes with the response's
   `Content-Type` charset as a label (Fetch §3.5's legacy extract an encoding, then Encoding §6.1 decode, whose BOM
   sniff can overrule the label); a module script is UTF-8 whatever the response says. `doc_ctx` is the realm of
   the program's document, read only on the classic arm (§4.12.1.1: "if el's type is `module`, this encoding
   will be ignored"). `bodyBytes` is the raw byte sequence (§2.2.5, crossing as bytes per fetch.h), read with no
   transform in between. Returns malloc'd source text the caller frees; `*out_n` is its length. */
static char *reply_source_text(JSContext *ctx, JSValueConst reply, ScriptType stype, JSContext *doc_ctx,
                               size_t *out_n)
{
    JSValue bv = fetch_reply_body(ctx, reply);
    size_t body_len = 0;
    const uint8_t *body = fetch_body_bytes(ctx, bv, &body_len);
    char *src;

    if (stype == SCRIPT_TYPE_MODULE) {
        src = script_fetch_module_source_text((const char *)body, body_len, out_n);
    } else {
        HeaderList hl = { 0 };
        char *content_type;

        fetch_reply_header_list(ctx, reply, &hl);
        content_type = header_list_get(&hl, "content-type");   /* NULL is Fetch's "values is null" = failure */
        src = script_fetch_classic_source_text((const char *)body, body_len, content_type,
                                               document_encoding(doc_ctx), out_n);
        free(content_type);
        header_list_free(&hl);
    }
    JS_FreeValue(ctx, bv);
    return src;
}

/* HTML §8.1.4.2 Fetching scripts' fetch a single module script has a condition the classic algorithms lack.
   After the shared two-item list ("bodyBytes is null or failure; or response's status is not an ok status") it
   extracts a MIME type and sets moduleScript only "If mimeType is a JavaScript MIME type and moduleType is
   "javascript-or-wasm"", so a non-JavaScript type leaves moduleScript null and §8.1.6.7.3 HostLoadImportedModule
   turns it into a TypeError. There is no scheme exemption: the classic gate applies only to HTTP(S) URLs ("other
   fetch schemes are exempted from MIME type checking for historical web compatibility reasons"), the module
   one to every scheme, so `data:text/plain` fails where `data:text/javascript` loads. An absent `Content-Type` is
   extraction failure (mime_type_extract's contract) and so a rejection. Nothing is asserted on the type: a
   server chose it, so a refused type is the algorithm's own failure. */
static bool module_reply_is_javascript(JSContext *ctx, JSValueConst reply)
{
    HeaderList hl = { 0 };
    char *content_type;
    MimeType mt;
    bool ok;

    fetch_reply_header_list(ctx, reply, &hl);
    content_type = header_list_get(&hl, "content-type");   /* joined by "get a header", which is what extract takes */
    mime_type_init(&mt);
    ok = mime_type_extract(&mt, content_type) && mime_type_is_javascript(&mt);
    mime_type_free(&mt);
    free(content_type);
    header_list_free(&hl);
    return ok;
}

/* The program queue carries `len` bytes, NUL or not: `dyn_body_adopt` takes the decode's `(src, src_n)`, the row
   keeps both, and flow_step hands both to JS_FlowNew / JS_FlowEvalModule, so the length never becomes a `strlen`
   before the compiler. Real scripts contain U+0000 (ECMAScript §11.1 Source Text), and running one only to its
   first NUL would be a silently shorter program. dyn_body_adopt's invariant is about the guard byte. */

/* A reply that is a program joins the sequence, so the delivery below reaches queue entries defined past it.
   Declared rather than moved: the queue belongs beside the other queue entries, the delivery beside the
   register it scans. */
static void engine_queue(uint32_t doc, const char *body, size_t body_n, DynKind kind, ScriptType stype,
                         const char *url, TaskSource src, DynPos pos);
/* …and the same entry for a row an element put there (engine_queue_el below). */
static void engine_queue_el(uint32_t doc, const char *body, size_t body_n, DynKind kind, ScriptType stype,
                            const char *url, TaskSource src, DynPos pos, lxb_dom_element_t *el,
                            int parser_inserted);
/* …and the one for a caller already holding the decoded text as a shared body, which is every reply that
   arrives as a program: the delivery adopts the decode and hands it over without a second copy. */
static void engine_queue_el_body(uint32_t doc, DynBody *body, DynKind kind, ScriptType stype, const char *url,
                                 TaskSource src, DynPos pos, lxb_dom_element_t *el, int parser_inserted);

/* Delivers one answered entry, then returns; the selector finds it, nothing drains. Every delivery runs through
 * JS_CallAsFlow, a call-root flow whose resolving function can park at each re-entry: 27.5.1.3
 * CreateResolvingFunctions ( toResolve )'s resolveSteps reads `Get(resolution, "then")` (step 2.f) off an object
 * whose prototype the page owns. Delivering reply B while A's settle is parked would trigger B's reactions first
 * (27.5.1.4 FulfillPromise step 7, 27.5.1.8 TriggerPromiseReactions step 1.b), and §9.5.5 HostEnqueuePromiseJob
 * runs jobs in enqueue order, so `.then` handlers would run in an order the preempt chose. One per step is also
 * the event loop's model: a completed fetch is delivered by a task, and HTML §8.1.7.3 Processing model runs a
 * microtask checkpoint after each. flow_step resumes the parked continuation first and flow_checkpoint_due runs
 * the microtasks before the next reply; the entry stays on the register until the step that delivers it.
 *
 * `pending_next_deliverable` is the one answer to "which reply is next": `pending_ready` asks whether there is
 * one, `pending_ready_next_seq` its stamp (for the ladder), and this its index, so all three name one entry. A
 * HOSTREQ's answer is skipped by the selector's predicate and never removed here: it stays where the machine
 * parked at the call site will take it (engine_host_take).
 *
 * Named residual. Not covered: the order among answered entries. §8.1.7.1 Definitions puts a fetch's task on one
 * source and §8.1.7.3 step 2.3 takes "the first runnable task in taskQueue", so replies should deliver in arrival
 * order; this selects by register position, which swap-removes (pending_remove, engine_host_take) permute (A B C
 * D deliver A, D, B, C). Next diff builds: an arrival ordinal on the record (a PENDING_FIELDS entry, PEND_SHARE
 * for `value`'s reason) minted monotonically in `pend_set_field`, where both write doors (`pending_set` and
 * `pending_answer_sync`) meet and pend_index_sync dispatches, and the selector choosing the smallest. Absence
 * shows as: engine/solvergate.mjs's `lastreply` schedule disagreeing with `direct` on a corpus document whose
 * two concurrently answered fetches' `.then` handlers observe each other. */
static void flow_deliver_one_reply(JSContext *ctx, Flow *f) {
    int i = pending_next_deliverable(f->pending);
    if (i >= 0) {
        JSValue p = pending_entry(f->pending, i);
        JSValue pv;
        int kind;
        /* No entry is delivered while a settle of this register is still parked: its reactions are not queued
           yet (27.5.1.4 FulfillPromise step 7), and anything delivered first would enqueue ahead of them. Both
           homes of a park are asked (on the Flow while switched out, in the runtime's FIFO while switched in), as
           flow_answer_fork and flow_finish do. It catches a caller arriving with a continuation outstanding,
           which flow_step's resume arm exists to prevent. */
        DCHECK(!JS_HasParkedFlow(JS_GetRuntime(ctx)) && f->parked == NULL,
               "a reply is about to be delivered while a settle from this flow's register is still PARKED — "
               "the parked settle has not triggered its promise's reactions yet, so this delivery's reactions "
               "would be enqueued in front of them and the page would observe an order the forced preempt "
               "chose. Resume the parked continuations first (flow_step's resume arm)");
        /* Under this flow's own delta, because the delivery runs the page's code; flow_deliver asserts the same
           for a routed message. */
        DCHECK(flow_running() == f,
               "a reply was delivered while another flow was switched in — the page's resolving function would "
               "run against that flow's delta and the reactions it triggers would be queued on that flow");
        /* Taken off the register before delivery, which is the record's lifetime: the page code the settle runs
           (resolveSteps' `Get(resolution, "then")`) can issue another fetch onto this register, and the JS
           reference held here keeps the record alive across that. The removal is a swap-remove and the call ends
           at the delivery; the next step selects again over whatever the register became. */
        pending_remove(&f->pending, i);
        kind = (int)pending_get_int(p, PEND_KIND);
        pv = pending_get(p, PEND_VALUE);
        /* The program kinds' failure arm: §8.1.4.2's three fetch algorithms open with the same two-item list
           ("fetch a classic script" step 5.2, "fetch a classic worker script"'s processResponseConsumeBody step 2,
           "fetch a single module script" step 13.1): `bodyBytes` "is null or failure", or "response's status is
           not an ok status", then "run onComplete given null". A network error crosses as JS_NULL (Fetch §5.6,
           core/fetch/fetch.h), as does a request the zone refused (flow_decline_fork). For a script element
           onComplete is "Mark as ready el given result" with a null result, and "execute the script element" step
           4 fires `error` and returns, so the element runs nothing and `s.onerror` fallbacks run. Without this a
           failed load would decode fetch_reply_body's empty bytes, or a 404's error page, and run as a program.
           The status range is core/loader/script_fetch.h's (core/html/html_link.c asks the same); no assert
           stands on it, since a server chose it. `JS_IsNull` comes first: a network error has no record to read
           a status or headers off.
           A module adds the MIME gate (module_reply_is_javascript), asked last as the only member reading
           headers: a chunk served `application/json` must reject the load, so `import(u).catch(h)` runs `h`,
           rather than throw a SyntaxError inside a module that loaded.
           The fetch and `<link>` kinds are excluded: their failure is the answer (HTML §4.6.8.12 Link type
           "modulepreload" step 14.1, §4.6.8.20 Link type "preload"'s network-error branch), read by
           core/html/html_link.c's deliveries. The test stays negative so the DCHECK below can fail: a new kind
           delivered with no arm of its own crashes on it. */
        if (kind != FLOW_PENDING_RESOLVE && kind != FLOW_PENDING_RESOURCE &&
            (JS_IsNull(pv) || !script_fetch_status_ok(fetch_reply_status(ctx, pv)) ||
             (kind == FLOW_PENDING_MODULE && !module_reply_is_javascript(ctx, pv)))) {
            DCHECK(kind == FLOW_PENDING_SCRIPT || kind == FLOW_PENDING_DOCSCRIPT ||
                   kind == FLOW_PENDING_MODULE,
                   "a FAILED answer reached the program kinds' failure arm for a kind that is not one of them "
                   "— the synchronous kind is skipped above and the fetch kind is excluded by this condition, "
                   "so a fourth answer means a park of some new kind is being delivered with no arm of its own");
            if (kind == FLOW_PENDING_MODULE) {
                /* The one program kind with no element to fire at, so its failure is a rejection. HTML
                   §8.1.6.7.3 HostLoadImportedModule's onSingleFetchComplete sets "completion to
                   ThrowCompletion(a new TypeError)" for a null moduleScript and performs ECMAScript §16.2.1.11
                   FinishLoadingImportedModule, whose dynamic-import arm, §13.3.10.3 ContinueDynamicImport, calls
                   "promiseCapability.[[Reject]]". The TypeError's message is this engine's (the standard gives
                   none) and names the address, the one fact a handler cannot re-derive. Run as a flow, because
                   the rejection triggers the page's `catch` reactions, where bundles keep fallback hosts. */
                JSValue rej = pending_get(p, PEND_REJECT);
                JSValue uv = pending_get(p, PEND_URL), err;
                const char *u;

                DCHECK(JS_IsFunction(ctx, rej),
                       "a dynamic import's park carries no REJECT capability — engine_pending_module_url "
                       "asserts both halves at the push, so a record here without one was pushed by something "
                       "that is not that park, and its network error has no way to reach the page's `catch`");
                u = JS_IsString(uv) ? JS_ToCString(ctx, uv) : NULL;
                JS_ThrowTypeError(ctx, "Failed to fetch dynamically imported module: %s",
                                  u ? u : "(the park carried no address)");
                err = JS_GetException(ctx);
                if (u) JS_FreeCString(ctx, u);
                JS_FreeValue(ctx, uv);
                if (JS_CallAsFlow(ctx, rej, err) < 0) {
                    JSValue exc = JS_GetException(ctx);
                    JS_FreeValue(ctx, exc);   /* a rejected load is the page's to observe, not this step's */
                }
                JS_FreeValue(ctx, err);
                JS_FreeValue(ctx, rej);
            } else {
                if (kind == FLOW_PENDING_DOCSCRIPT) {
                    /* The row is converted in place, never removed: a failed load still holds its element's
                       position against the scripts around it (flow_step's DYN_SCRIPT_FAILED arm is this engine's
                       "execute the script element" for it). The register names it by `dyn_id`. */
                    int di = flow_dyn_row_by_id(f, (uint64_t)pending_get_int(p, PEND_SCRIPT_ROW));

                    DCHECK(di >= 0,
                           "an external document script's failure named a row this flow does not hold — the "
                           "entry was queued on one flow and is being delivered into another, or its row was "
                           "taken by a destroy that did not take the entry with it");
                    DCHECK(f->dyn_cand[di] == DYN_SCRIPT_SRC,
                           "an external document script's failure named a row that is not awaiting a program "
                           "— the row already holds one, so this is a second answer to one request");
                    DCHECK(f->dyn_el[di] != NULL,
                           "an external document script's row carries no element — §4.12.1.1's `execute the "
                           "script element` is a switch on EL and its step 4 fires `error` AT el, so a row "
                           "without one was queued by something that is not engine_queue_docscript_url or the "
                           "seed's address arm, both of which take the element that owns the address");
                    f->dyn_cand[di] = DYN_SCRIPT_FAILED;
                    /* The row is runnable from now: §4.12.1.1 step 4 is what this step runs at it, and HTML
                       §13.2.6.4.8 The "text" insertion mode waits until "the script's ready to be
                       parser-executed becomes true", which a failed load also makes true. */
                    f->dyn_run[di] = flow_work_seq_next();
                } else {
                    /* An injected `<script src>` has no row yet (its reply creates one), so its failure creates
                       the row that runs nothing, at the tail as the program would: §4.12.1.1's list of scripts
                       that will execute in order as soon as possible holds these elements' places, and a failed
                       member still executes (step 4) at its place. The body is the address; the address column
                       stays null because a row that runs nothing has no §8.1.4.2 created-script base URL. */
                    JSValue ev = pending_get(p, PEND_SCRIPT_EL);
                    JSValue uv = pending_get(p, PEND_URL);
                    lxb_dom_node_t *en = node_of(ev);
                    const char *u;
                    lxb_dom_element_t *el;

                    DCHECK(en != NULL && en->type == LXB_DOM_NODE_TYPE_ELEMENT,
                           "an injected <script src> failed for a park carrying no element — the park writes "
                           "the element's wrapper and §4.12.1.1's step 4 fires `error` AT it, so a record "
                           "without one is a record something that is not that park pushed");
                    DCHECK(JS_IsString(uv),
                           "an injected <script src> failed for a park with no address — the park writes the "
                           "resolved URL and the reply is matched on it, so a record without one names no "
                           "script and the row below would hold no account of what did not load");
                    el = lxb_dom_interface_element(en);
                    u = JS_ToCString(ctx, uv);
                    CHECK(u != NULL, "engine: OOM reading a failed injected script's address off its park");
                    engine_queue_el((uint32_t)pending_get_int(p, PEND_DOC), u, strlen(u), DYN_SCRIPT_FAILED,
                                    (ScriptType)pending_get_int(p, PEND_SCRIPT_TYPE), NULL,
                                    TASK_SOURCE_NETWORKING, DYN_POS_APPEND, el,
                                    /*parser_inserted*/0);   /* the fetch already happened and FAILED */
                    JS_FreeCString(ctx, u);
                    JS_FreeValue(ctx, uv);
                    JS_FreeValue(ctx, ev);
                }
            }
            /* The entry is done either way: it left the register before this arm ran, and the page has been
               told by the algorithm its kind names. */
            JS_FreeValue(ctx, pv);
            JS_FreeValue(ctx, p);
            return;
        }
        if (kind == FLOW_PENDING_DOCSCRIPT) {
            /* An external script of some document of this agent, at its position in this flow's sequence
               (DYN_SCRIPT_SRC). The reply fills this timeline's own row; engine_provide already filled and
               un-marked every flow whose register names the address, so one host fetch answers every waiting
               member. */
            int di = flow_dyn_row_by_id(f, (uint64_t)pending_get_int(p, PEND_SCRIPT_ROW));
            size_t src_n = 0;
            char *src;
            DCHECK(di >= 0,
                   "an external document script replied for a row this flow does not hold — the entry was "
                   "queued on one flow and the reply is being delivered into another, or its row was taken by "
                   "a destroy that did not take the entry with it");
            DCHECK(f->dyn_cand[di] == DYN_SCRIPT_SRC,
                   "an external document script replied for a sequence position that is not awaiting one — the "
                   "slot holds a program already, so this reply is a second answer to one request and the "
                   "program it overwrites would never run");
            /* Which of §8.1.4.2 Fetching scripts' two decodes runs is the row's own type: a module's bytes are
               UTF-8 whatever the response says, a classic script's go through the response's charset label. The
               encoding realm is the entry's document's. */
            DCHECK(script_type_executes((ScriptType)f->dyn_type[di]),
                   "an external document script replied for a row whose type executes nothing — only a "
                   "`<script>` element of an executing type ever becomes a DYN_SCRIPT_SRC row, so this row's "
                   "type column was written by something that is not one of the two seams that queue them");
            src = reply_source_text(ctx, pv, (ScriptType)f->dyn_type[di], doc_realm(f->dyn_doc[di]), &src_n);
            CHECK(src, "engine: OOM storing an external document script");
            /* The address moves out of the body column into the address column, at the one moment it can: the
               row's body is its URL until here. §8.1.4.2 creates the script with the response's URL ("creating a
               classic script given sourceText, settingsObject, response's URL, options, mutedErrors, and url"),
               so a nested `import('./chunk.js')` in a bundle served from `/assets/app.js` resolves to
               `/assets/chunk.js`, and for a module the address is the module map key that keeps two
               `<script type=module src>` two modules. It is copied because the body text is shared
               (solver/dyn_body.h) by the arms of a fork parked on this row; the source text itself is adopted. */
            DCHECK(f->dyn_url[di] == NULL,
                   "an external document script's row already held an address before its reply arrived — the "
                   "row's body is its URL until this delivery takes it, so a second one means the row was queued "
                   "with an address column it may not have or a reply was delivered into it twice");
            {
                /* The row names a live body at every point of the swap: the replacement is built before the old
                   one is released, so nothing walking this flow's rows (a census, a release, a sale) can read a
                   freed buffer. */
                DynBody *nb = dyn_body_adopt(src, src_n);   /* the decode above already made this buffer */
                CHECK(nb, "engine: OOM adopting an external document script's source text");
                /* The one place a body is read as a C string says so: a DYN_SCRIPT_SRC row's body is its
                   address, a NUL-terminated string, while a program body is (text, len) and may hold U+0000. An
                   address whose length is not its `strlen` came by some route other than the two that queue
                   one, and every nested `import()` would resolve against a prefix of it. */
                DCHECK(strlen(dyn_body_text(f->dyn[di])) == dyn_body_len(f->dyn[di]),
                       "an external document script's row holds an address with a U+0000 in it — the row's "
                       "body is its URL until this delivery takes it across, and a URL is the one body in this "
                       "column that is read as a NUL-terminated string, so this row was filled by something "
                       "that is not engine_queue_docscript_url or the seed");
                f->dyn_url[di] = strdup(dyn_body_text(f->dyn[di]));
                CHECK(f->dyn_url[di], "engine: OOM keeping an external script's address as its base URL");
                dyn_body_unref(f->dyn[di]);
                f->dyn[di] = nb;
            }
            f->dyn_cand[di] = DYN_PAGE_SCRIPT;
            /* The row becomes a runnable work item here, which is its arrival: until now it held an address no
               step could start. HTML §13.2.6.4.8 The "text" insertion mode blocks only the tokenizer while the
               bytes are outstanding, then spins the event loop until "the script's ready to be parser-executed
               becomes true", and §8.1.7.3 resumes the blocked algorithm by queueing a task when the wait ends. So
               every callback queued while the bytes were in the air is older than this row and runs first; the
               stamp says so. `dyn_id` is not touched: the register entry above resolved the row by it, and
               every forked arm's entries still name it by it. */
            f->dyn_run[di] = flow_work_seq_next();
        } else if (kind == FLOW_PENDING_SCRIPT) {
            /* The reply is a program: it joins this flow's script sequence. The document is the one the element
               was inserted into, which the park recorded, so its encoding decodes these bytes. */
            uint32_t doc = (uint32_t)pending_get_int(p, PEND_DOC);
            ScriptType st = (ScriptType)pending_get_int(p, PEND_SCRIPT_TYPE);
            JSValue uv = pending_get(p, PEND_URL);
            const char *u;
            size_t src_n = 0;
            char *src;
            DCHECK(script_type_executes(st),
                   "an injected <script src> replied for a park carrying a type that executes nothing — the "
                   "park writes the element's type and §4.12.1.1 fetches for exactly the two that run, so a "
                   "third answer means this record was pushed by something that is not that park");
            /* Must be a string, not something that coerces to one: `JS_ToCString` answers "undefined" for an
               absent field, which a URL parser would accept as a base. */
            DCHECK(JS_IsString(uv),
                   "an injected <script src> replied for a park with no address — the park writes the resolved "
                   "URL and the reply is matched on it, so a record without one names no script and the "
                   "program below would be created with its document's base URL as its own");
            u = JS_ToCString(ctx, uv);
            CHECK(u != NULL, "engine: OOM reading an injected script's address off its park");
            src = reply_source_text(ctx, pv, st, doc_realm(doc), &src_n);
            /* The decode answers NULL when it cannot allocate. */
            CHECK(src, "engine: OOM storing an injected script");
            /* §8.1.4.2's created script is based on the response's URL, so the program carries the address its
               bytes came from: the base for a nested `import('./chunk.js')`, and for a module the module map key. */
            {
                /* The element the park carried goes back onto the row: §4.12.1.1's "execute the script element"
                   switches on it when the sequence reaches the row. */
                JSValue ev = pending_get(p, PEND_SCRIPT_EL);
                lxb_dom_node_t *en = node_of(ev);
                /* Adopted, not copied (solver/dyn_body.h); the row is the body's first holder. */
                DynBody *b;
                DCHECK(en != NULL && en->type == LXB_DOM_NODE_TYPE_ELEMENT,
                       "an injected <script src> replied for a park carrying no element — the park writes the "
                       "element's wrapper and this is the only reader, so a record without one is a record "
                       "something that is not that park pushed");
                b = dyn_body_adopt(src, src_n);
                CHECK(b, "engine: OOM adopting an injected script's source text");
                /* The networking task source (§8.1.7.4 Generic task sources): §8.1.7.1 Definitions, "When an
                   algorithm fetches a resource, if the fetching occurs in a non-blocking fashion then the
                   processing of the resource once some or all of the resource is available is performed by a
                   task." */
                engine_queue_el_body(doc, b, DYN_PAGE_SCRIPT, st, u, TASK_SOURCE_NETWORKING, DYN_POS_APPEND,
                                     lxb_dom_interface_element(en), /*parser_inserted*/0);   /* the reply is here */
                dyn_body_unref(b);
                JS_FreeValue(ctx, ev);
            }
            JS_FreeCString(ctx, u);
            JS_FreeValue(ctx, uv);
        } else if (kind == FLOW_PENDING_MODULE) {
            /* The reply is a module's source: the load's promise is settled with the text (not the bytes; "let
               sourceText be the result of UTF-8 decoding bodyBytes" stands between them), and the import's own
               reaction compiles, links and continues the flow that wrote `await import(...)`. */
            size_t src_n = 0;
            char *src = reply_source_text(ctx, pv, SCRIPT_TYPE_MODULE, ctx, &src_n);
            JSValue sv = JS_NewStringLen(ctx, src, src_n);
            JSValue resolve = pending_get(p, PEND_RESOLVE);
            free(src);
            if (JS_CallAsFlow(ctx, resolve, sv) < 0) {
                JSValue exc = JS_GetException(ctx);
                JS_FreeValue(ctx, exc);   /* a rejected load is the page's to observe, not this step's */
            }
            JS_FreeValue(ctx, resolve);
            JS_FreeValue(ctx, sv);
        } else if (kind == FLOW_PENDING_RESOURCE) {
            /* A browser algorithm's own subresource: the record goes to that algorithm's completion steps and
               nothing is queued, since none of these algorithms evaluates anything now. HTML §4.6.8.12 Link type
               "modulepreload" "places the result into the appropriate module map for later evaluation" (its
               example: "the module is already ready (but not evaluated) in the module map"); §4.6.8.20 Link type
               "preload" fills the preload cache; §4.8.4.3.5 Updating the image data decodes an image. No code is
               lost: a preloaded chunk is imported by the document's own module script, whose park is
               FLOW_PENDING_MODULE. A network error is still delivered: §4.6.8.12 step 14.1 fires `error` for a
               null result and link_module_deliver reads a non-ok status itself. */
            JSValue deliver = pending_get(p, PEND_RESOLVE);
            DCHECK(JS_IsObject(pv) || JS_IsNull(pv),
                   "a browser algorithm's subresource reply arrived as something other than the host's reply "
                   "record — "
                   "qjs_provide parses the trusted zone's JSON into one and §4.3's scheme fetch builds one, "
                   "and JS_NULL is the network error both spell; a bare string here is a host still "
                   "delivering only bytes, which core/html/html_link.c's deliveries assert against too");
            /* As a flow: §4.6.8.12 step 14's steps fire an event at the element, running the page's listener,
               which a plain call would run with no flow base. */
            if (JS_CallAsFlow(ctx, deliver, pv) < 0) {
                JSValue exc = JS_GetException(ctx);
                JS_FreeValue(ctx, exc);   /* a listener's throw is the page's to observe, not this step's */
            }
            JS_FreeValue(ctx, deliver);
        } else {
            /* A synchronous answer is taken, never delivered here: the asking machine resumes through its park
               and consumes it with engine_host_take. A HOSTREQ reaching this line would be settled as a fetch,
               through a `resolve` it does not have. */
            DCHECK(kind == FLOW_PENDING_RESOLVE,
                   "a synchronous host request's answer reached the reply delivery — its asking machine never "
                   "resumed to take it, so its parked continuation is the thing to look for");
            /* As a flow, not a JS_Call: 27.5.1.3 CreateResolvingFunctions ( toResolve )'s resolveSteps reads
               `Get(resolution, "then")` (step 2.f) off the Response, whose prototype the page owns, so
               `Object.prototype.then = { get(){…} }` runs page code there; prototype pollution is a gadget class
               the solver runs. It is also where the settle can park (js_promise_resolvefn_step's PRF_THEN),
               which is why this function delivers exactly one entry.
               The reply is delivered as the trusted zone made it (qjs_provide parses the record whole), with its
               status, headers and URL list; JS_NULL is the network error the delivery machine rejects with. */
            JSValue resolve = pending_get(p, PEND_RESOLVE);
            DCHECK(JS_IsObject(pv) || JS_IsNull(pv),
                   "a fetch reply arrived as something other than the host's reply record — qjs_provide parses "
                   "the trusted zone's JSON, and a bare string here is a host still delivering only bytes");
            /* It must be a reply record, asked here rather than three frames later where fetch.c would abort
               with no way to say which entry produced it. `urlList` is the field only the trusted zone can
               answer and every producer must fill (fetch.h), so a record without it was not built by
               fetch_reply_new. Asked in a dev block because the read allocates. */
#if APICLIENT_DEV
            if (JS_IsObject(pv)) {
                JSValue ul = JS_GetPropertyStr(ctx, pv, "urlList");
                JSValue uv2 = pending_get(p, PEND_URL);
                const char *u2 = JS_IsString(uv2) ? JS_ToCString(ctx, uv2) : NULL;
                char fields[320];
                size_t fi = 0;
                JSAtom cn = JS_ATOM_NULL;
                const char *cns = NULL;
                /* The message names what the record is: its own property list identifies each candidate
                   (`body,csp` is a peer's §7.4 navigation answer, `resolve,value,url,…` a pending entry in the
                   value slot, `urlList,status,…` minus the list a record changed after it was built, `{}` an
                   object nobody filled). The class separates the empty list's two families: a platform object
                   keeping its state in a C opaque (a Response, a Promise, a Headers) from a plain `Object` nobody
                   filled or whose shape changed, a Fetch question versus a memory one. The pointer can be
                   matched against an allocator report. */
                if (JS_IsObject(pv)) {
                    cn = JS_GetClassName(JS_GetRuntime(ctx), JS_GetClassID(pv));
                    cns = JS_AtomToCString(ctx, cn);
                }
                /* The field list has no bound of its own, so it says where it stopped rather than ending on
                   whichever name happened to fit. */
                memcpy(fields, "(not an object)", 16);
                if (JS_IsObject(pv)) {
                    JSPropertyEnum *tab = NULL;
                    uint32_t pn = 0, pi;
                    fields[0] = '\0';
                    if (JS_GetOwnPropertyNames(ctx, &tab, &pn, pv, JS_GPN_STRING_MASK) == 0) {
                        for (pi = 0; pi < pn; pi++) {
                            const char *nm = JS_AtomToCString(ctx, tab[pi].atom);
                            int w = snprintf(fields + fi, sizeof fields - fi, "%s%s", pi ? "," : "",
                                             nm ? nm : "?");

                            if (nm) JS_FreeCString(ctx, nm);
                            if (w < 0 || (size_t)w >= sizeof fields - fi) break;
                            fi += (size_t)w;
                        }
                        if (pi < pn) {
                            const char more[] = ",…";
                            size_t at = fi + sizeof more <= sizeof fields ? fi : sizeof fields - sizeof more;

                            memcpy(fields + at, more, sizeof more);
                        }
                        JS_FreePropertyEnum(ctx, tab, pn);
                    }
                    if (pn == 0) memcpy(fields, "(none)", 7);
                }
                DCHECKF(JS_IsArray(ul),
                        "a fetch reply carrying no `urlList` is about to be delivered — the record on "
                        "this entry was not built by fetch_reply_new, so some other writer reached a "
                        "FLOW_PENDING_RESOLVE entry. url=%s kind=%d tag=%d class=%s ptr=%p fields=%s",
                        u2 ? u2 : "(none)", kind, (int)JS_VALUE_GET_TAG(pv),
                        cns ? cns : "(not an object)",
                        JS_IsObject(pv) ? JS_VALUE_GET_PTR(pv) : NULL, fields);
                if (cns) JS_FreeCString(ctx, cns);
                if (cn != JS_ATOM_NULL) JS_FreeAtom(ctx, cn);
                if (u2) JS_FreeCString(ctx, u2);
                JS_FreeValue(ctx, uv2);
                JS_FreeValue(ctx, ul);
            }
#endif
            /* A reply whose bytes are JavaScript is a program: a fetch whose body is JavaScript is always fetched
               and executed (a lazy chunk reveals real endpoints), whether or not the page hands it to a
               `<script>`. engine_queue_fetched_script runs it. Here, not in reply_decode_learn, which runs on the
               host's time outside any flow, where `flow_running()` would name whichever flow last yielded
               (engine_queue_el_body's DCHECK would not fire) and the row would land in an arbitrary member's
               table. Here the flow that made the fetch is switched in, so each of N flows parked on one URL
               compiles the chunk on its own timeline. Before the settle, so the row takes the networking task
               source's position ahead of the page's own reaction, as the `<script src>` arm gives an
               element-loaded chunk. The type is the host's `computedType`, stamped at fetch_reply_new; sniffing
               belongs to the trusted zone (SECURITY.md), so a type that does not parse is not a program. */
            {
                uint32_t sdoc = (uint32_t)pending_get_int(p, PEND_DOC);
                char *ct;
                MimeType cm;
                /* This door was asked, recorded before any type is read: the denominator of the queue below,
                   counted ahead of the gate because declining a non-JavaScript reply is correct (solver/engine.h's
                   `net_prog_*` block). Counted only for a reply record, since the XHR door returns before its
                   program block for a network error, so both doors' denominators count one population. */
                if (JS_IsObject(pv)) engine_note_net_prog_fetch_ask();
                ct = fetch_reply_computed_type(ctx, pv);
                if (mime_type_extract(&cm, ct)) {
                    if (mime_type_is_javascript(&cm)) {
                        JSValue suv = pending_get(p, PEND_URL);
                        const char *su = JS_IsString(suv) ? JS_ToCString(ctx, suv) : NULL;
                        /* The address is required: engine_queue_fetched_script refuses without one, because
                           §8.1.4.2 creates the script with the response's URL and §8.1.4.1 keeps it as the base
                           URL. A park that reached the network has one (pending_park_request wrote it); a
                           value-carrying park with none never reached the network and is not a program. */
                        if (su && *su) {
                            size_t src_n = 0;
                            /* §8.1.4.2's decode, through the reader the script arms use. */
                            char *src = reply_source_text(ctx, pv, SCRIPT_TYPE_CLASSIC, doc_realm(sdoc),
                                                          &src_n);
                            CHECK(src, "engine: OOM decoding a fetched JavaScript reply into a program");
                            /* Named residual. Not covered: engine_queue_fetched_script takes `const char *` and
                               engine_queue_el copies it into a fresh DynBody, so a chunk delivered to N flows
                               parked on one URL is decoded and copied N times. Next diff builds: an entry beside
                               engine_queue_fetched_script taking a built DynBody (already refcounted,
                               solver/dyn_body.h). Absence shows as: resident bytes rising with the number of
                               flows parked on one chunk address rather than the number of distinct addresses. */
                            engine_queue_fetched_script(sdoc, src, src_n, su);
                            /* This door queued one; counted beside the call because the entry cannot see which
                               transport carried the bytes. */
                            engine_note_net_prog_fetch_queued();
                            free(src);
                        }
                        if (su) JS_FreeCString(ctx, su);
                        JS_FreeValue(ctx, suv);
                    }
                }
                mime_type_free(&cm);
                free(ct);
            }
            if (JS_CallAsFlow(ctx, resolve, pv) < 0) {
                JSValue exc = JS_GetException(ctx);
                JS_FreeValue(ctx, exc);   /* a rejected delivery is the page's to observe, not this step's */
            }
            JS_FreeValue(ctx, resolve);
        }
        JS_FreeValue(ctx, pv);
        JS_FreeValue(ctx, p);
        /* One, and the step is over. The settle may have parked (the `then` read is page code), and then nothing
           else may run: flow_step's next pass resumes it first. If it completed and enqueued reactions, the
           checkpoint arm runs them first, as §8.1.7.3 does at the end of the delivering task. */
        return;
    }
    /* The two questions agree: `pending_ready` told the caller there was something to deliver and the selector
       over the same register found nothing. One changed without the other, and the symptom would be a flow
       reporting progress every step while delivering nothing, a livelock that looks like slowness. */
    DFAIL("a reply delivery found nothing to deliver — pending_ready answered that this flow had an answered "
          "non-HOSTREQ entry and the scan over the same register did not find one");
}

/* Snapshot-fork handoff: solver_decide stashes the sibling's hot decision and pins here at a forking branch; the
   interpreter then clones the frame and calls engine_fork_finalize, which assembles the sibling. */
static void *g_fork_dec = NULL, *g_fork_pins = NULL;
/* Who is asking: the interpreter and the step driver hold a resume point and clone it just after asking, a plain
   C body holds none, and all call the same two symbols. The two wrappers below raise this across their call and
   nothing else does, so raised means "a clone is coming" and clear means "there is no activation here". It is the
   caller's declaration, not an inference from the flow's state. */
static int g_fork_snapshot_owed;
/* Whether this session forks at all: the explore/verify bit, written beside JS_SetFlowControlHooks. */
static int g_sess_forking;
/* Read by the seam itself. The hook table carries the policy into the interpreter and step driver, which have
   their own non-forking answers for a missing hook (-1 then ToBool, and outcome 0), but a caller asking the
   decision seam by symbol consults neither, so decide.c asks here, where the session's policy is written, rather
   than holding a second copy of the bit. It selects no implementation. */
int engine_session_forks(void) { return g_sess_forking; }
/* engine_sibling_assemble is declared above the delivery fork, its first caller in this file. */
/* The handoff is filled and emptied within one fork: the two pointers are a slot with one legal occupant. A second
   prepare with the first still there means the interpreter took the forked bit and never reached its fork hook,
   and overwriting would leak a decision blob and pin blob nothing names, keeping the frozen decision prefix under
   them alive. A backstop: the two ways to reach it without a consumer crash where they are born. */
int engine_prepare_fork(JSContext *ctx, void *dec_blob, void *pin_blob, const char *asked, int restartable,
                        const char *site) {
    Flow *f = flow_running();

    DCHECK(f != NULL, "a sibling was prepared with no flow running — the blobs were frozen off whatever "
                      "decision state the last switched-in flow left behind, so they describe no timeline");
    DCHECK(g_sess_forking,
           "a fork was prepared inside a NON-FORKING session — §@S's candidate re-fire is ONE concrete path, "
           "so no member of the frontier may be minted from inside one. The seam now ASKS the session "
           "(engine_session_forks) and answers a new question with the arm the SITE declared, so reaching this "
           "line means the answer was declared and the seam forked anyway — decide_arm's non-forking branch is "
           "the one that must have been taken");
    if (!g_fork_snapshot_owed) {
        /* Nobody will clone a frame, so the sibling's resume point must be the flow's own state, which holds for
           engine code re-reached by re-running the flow's scheduler step: HTML §8.1.7.3 Processing model step
           2.1 chooses "in an implementation-defined manner", between tasks, with nothing on the stack. The
           sibling has no frame and its step re-runs the same walk, where the arm recorded here replays.
           Assembled now, because no later moment has the parent still switched in. */
        if (restartable) {
            DCHECK(f->frame == NULL,
                   "a restartable fork was asked for by a flow holding a live program frame — the caller "
                   "promised its computation is re-reached by re-running the flow's step, and a flow inside a "
                   "program re-enters that frame instead, so the sibling would never re-reach the ask");
            DCHECK(!JS_HasActivation(JS_GetRuntime(ctx)),
                   "a restartable fork was asked for while page code was on the stack — the caller was "
                   "reached THROUGH an activation (a queued job's callback, a listener inside a rendering "
                   "step), which the flow's frame handle cannot show because it is NULL for those too. Its "
                   "sibling would resume at a scheduler step that re-reaches nothing, and the arm recorded "
                   "for it would be replayed by whatever question that step asks first");
            engine_sibling_assemble(ctx, f, NULL, dec_blob, pin_blob);
            return 0;
        }
        /* A C body mid-algorithm has neither: it is inside its own activation, with no machine state to snapshot
           and no way to re-reach the ask by re-running anything. It must become a step machine
           (JS_CFUNC_STEP_DEF), with the ask moved into the machine's branch seam, after which the driver holds the
           resume point. A question arriving here through solver_decide is a branch question keyed by the
           operand's identity (decide_key), so its seam is `step_tobool_run` (JS_FORK_KIND_TOBOOL), not
           `step_fork_run`, which keys by (operand, operation, completion), is for a machine's own completions, and
           per quickjs-step.h would fork again over a predicate the flow may have fixed.
           `site` names the ask, because the predicate alone names nobody (the population is one helper in
           core/dom/abort.c, whose call population is derived there). Read the remedy as an end state: at the
           sites localised so far the builtin was already a step machine (a member taking a signal in a
           dictionary or sequence must be one, since Web IDL conversion runs page code), and the repair was
           routing the ask onto its existing branch seam. So check the body's `JSTrampStepDef` first; the
           remaining callers are getters, constructors and engine helpers that reach a signal they were handed. */
#if APICLIENT_DEV
        {
            char name[192];
            size_t i;
            /* The key is length-prefixed with control-byte separators (decide.c's concolic_ident_compose), so the
               predicate's name is copied printable-only for the reader, who is looking for an identifier; check.h
               would escape the bytes anyway. */
            for (i = 0; i + 1 < sizeof name && asked && asked[i]; i++)
                name[i] = (asked[i] >= 0x20 && asked[i] < 0x7f) ? asked[i] : '.';
            name[i] = '\0';
            DFAILF("a C builtin forked over unknown input from inside its own activation, so the sibling has "
                   "nowhere to resume: nothing will clone a frame for it and re-running the flow's step does "
                   "not re-reach the ask. Declare that builtin a step machine (JS_CFUNC_STEP_DEF) and move "
                   "this question into the machine's BRANCH seam, step_tobool_run — not step_fork_run, which "
                   "keys by (operand, operation, completion) and is for a machine's own completions — so the "
                   "driver snapshots the machine AT the ask. The ask site was: %s. The question was: %s",
                   site ? site : "(no ask site)", asked ? name : "(no source identity)");
        }
#endif
        (void)asked;
    }
    DCHECK(g_fork_dec == NULL && g_fork_pins == NULL,
           "a sibling's snapshot state was prepared while a PREVIOUS one was still unconsumed — the branch that "
           "prepared it never reached its fork hook, and this assignment drops that flow's decision and pin "
           "blobs on the floor with nothing naming them");
    g_fork_dec = dec_blob; g_fork_pins = pin_blob;
    return 1;
}

/* Generator-state fork stash: clone_deep_flow calls gen_fork for each generator body frame it clones (during
   JS_FlowClone), before engine_fork_finalize has the sibling's delta. Appended here; the finalize drains them onto
   the new sibling's delta and resets. Filled and fully drained within one fork. */
typedef struct { JSValueConst genobj; void *g0, *g1; } GenForkRec;
static GenForkRec *g_genforks = NULL; static int g_genfork_n = 0, g_genfork_cap = 0;
void engine_gen_fork(JSContext *ctx, JSValueConst genobj, void *base_gd, void *cur_gd) {
    (void)ctx;
    if (g_genfork_n >= g_genfork_cap) {
        g_genfork_cap = g_genfork_cap ? g_genfork_cap * 2 : 8;
        g_genforks = realloc(g_genforks, (size_t)g_genfork_cap * sizeof(GenForkRec));
        CHECK(g_genforks, "engine: OOM generator-fork stash");
    }
    g_genforks[g_genfork_n].genobj = genobj; g_genforks[g_genfork_n].g0 = base_gd; g_genforks[g_genfork_n].g1 = cur_gd;
    g_genfork_n++;
}

/* The reclaim safepoint's switch, defined with the partial self-park below. Two operations here hold a position in
   the frontier while allocating (the sibling assembly below and the job-drop walk) and close the safepoint
   around themselves with it. */
static int engine_reclaim_set(int v);

/* Assembles a sibling from a parent and a frame: everything a new timeline of this flow is, nothing about why it
 * exists. A flow forks over a predicate (the interpreter's concolic OP_if, the sibling carrying the other arm),
 * over a value it did not choose (one cross-instance read has N true answers, one arm per distinct answer,
 * flow_answer_fork), and over code the page never ran (engine_orphan_seed, whose `clone` is a fresh call frame
 * from JS_FlowNewCall). All need the same construction: the frame, the O(1) delta and DOM-segment fork, the
 * generator swaps, the inherited chunks, jobs and register, and a child world of the parent's so a peer can
 * materialize the arm's segment by forking the asker's. Only the decision state differs, so it is a parameter.
 * `dec_blob` and `pin_blob` are consumed; they become the sibling's. */
static Flow *engine_sibling_assemble(JSContext *ctx, Flow *parent, JSValue *clone,
                                     void *dec_blob, void *pin_blob) {
    DCHECK(parent != NULL, "a sibling was assembled with no parent flow — every field below is copied from one");
    DCHECK(dec_blob != NULL, "a sibling was assembled with no decision state — it would resume its parent's "
                             "frame standing on nothing, and every branch its parent had already taken would "
                             "be re-asked as a new one");
    /* A frameless sibling is safe only over a frameless parent. A parent inside a program would give a hot
       sibling (`started` is set below) with nothing to resume, and flow_step would compile the row at
       `script_i` again and replay its side effects. A parent with no frame is between programs, and so is its
       sibling, which re-enters its scheduler step where the parent was: a fork asked between tasks
       (engine_prepare_fork), the cold tier's rebuilds (flow.h), and the orphan drive's fresh call frame over a
       frameless parent all fit. */
    DCHECK(clone != NULL || parent->frame == NULL,
           "a fork of a flow that is INSIDE a program arrived with no frame snapshot — the sibling would be "
           "marked hot with nothing to resume, and its first step would compile that program's row again and "
           "replay every side effect the parent has already performed");
    /* Unstarted cross-agent operations and routed deliveries are asserted at the branch hook, not here: an
       answer fork happens at a park, where the zone may have attached one. */
    /* The sibling joins the frontier on flow_add's first line, so nothing may page it out while the dozen
       allocations below construct it: a newborn carries the full optimism bonus and ranks near the top, so a
       reclaim at the refusal edge could free `sib` under these writes. The job-drop walk takes the same
       precaution for the same reason. */
    int prev_reclaim = engine_reclaim_set(0);
    /* Both arms get a child world and the fork point is retired: the sibling's by flow_add, the parent's on the
     * line after, from the same parent name. The edge is recorded so another instance holding a segment for the
     * fork point can materialize each arm's by forking it. Retiring the point matters twice: a peer asking
     * whether two senders contradict must not see the arms as ancestor and descendant, the relation a flow's
     * two sequential posts have (§9.3.3 Posting messages, serialized by §8.1.7.1's task source); and a peer
     * forks the nearest ancestor it holds with cow_delta_fork, which freezes that head, so an ancestor no flow
     * holds cannot change under it (world_ancestry asserts it). The cost is one minted row per fork; the vector
     * does not grow, because world_ancestry names only ancestors that have crossed the seam. */
    WorldId fork_point = parent->world;
    /* Unseeded: an arm inherits its parent's sequence (copied below), so seeding would allocate the root
       document's rows onto it and leak them under the copy. */
    Flow *sib = flow_add_unseeded(ctx, parent->fn, fork_point);
    parent->world = world_mint_child(fork_point);
    /* Its account is a child of the parent's too: the WFQ's two terms, taken over at the branch. First, because
       it decides where the flow enters the queue. */
    flow_fork_inherit(sib, parent);
    sib->started = 1;                 /* hot: resume from the blobs (and the cloned frame, when there is one) */
    sib->frame = clone;               /* the frame snapshot taken at the branch; NULL for a fork between tasks */
    sib->script_i = parent->script_i; /* same position in the script sequence */
    /* What that frame is: a branch inside an `error` listener is inside its parent's §8.1.4.4 step 8 report on
       the same row, so the arm must finish the report and take the row's completion out of it. At zero it would
       read the report frame as the row's program, advance the cursor twice and restore `currentScript` where
       §4.12.1.1 does not. */
    sib->reporting = parent->reporting;
    /* The same for the other two frame facts: an arm branching in a watcher's `cancel` handler stands in its
       parent's HTML §6.10.1 Close requests task and must take that task's completion; an arm forked after a
       modelled close request reached §6.10.1 step 9 found nothing watching and must not model the arrival
       again. */
    sib->close_req = parent->close_req;
    sib->close_req_none = parent->close_req_none;
    /* How far the sequence has been run, so the no-replay assert at the compile site holds for an arm, which has
       run what its parent ran; a frameless sibling is the shape that can reach that compile. */
    sib->last_compiled = parent->last_compiled;
    /* No parse in flight: a branch is taken by running bytecode, and a compiling flow runs none, so a parent
       holding one means a fork from inside the compiler. Asserted rather than zeroed. */
    DCHECK(parent->compile == NULL,
           "a flow was forked while its program was still being PARSED — a branch is taken by running "
           "bytecode and a compiling flow is running none, so either the fork came from inside the parser or "
           "the compile handle outlived the program it belongs to");
    DCHECK(sib->compile == NULL, "a freshly built sibling already held a suspended compile");
    /* Where the program it stands in has already interposed (flow.h): the arm inherits the parent's `dyn` rows
       and so their interpositions; without this pair it would compute its next immediate slot as if none had
       happened. A pair, because `imm_next` means nothing without the cursor it was computed against. */
    sib->imm_at = parent->imm_at; sib->imm_next = parent->imm_next;
    sib->delta = cow_delta_fork(ctx, (CowDelta *)parent->delta);   /* O(1) shared base segment, then diverges */
    /* Generator-state swaps built by clone_deep_flow for this fork, recorded on the sibling's delta so the shared
       generator object resolves to the sibling's own cloned execution state while it runs. */
    for (int i = 0; i < g_genfork_n; i++)
        cow_delta_add_gendata(ctx, (CowDelta *)sib->delta, g_genforks[i].genobj, g_genforks[i].g0, g_genforks[i].g1);
    g_genfork_n = 0;
    /* The DOM analog of the heap fork: the parent's live DOM head is frozen into a shared refcounted base
       segment (refcount 2: the parent gets a fresh empty head over it), so the sibling inherits the pre-fork
       document writes in O(1) and each diverges on its own head. */
    sib->dom_base = dom_cow_fork();
    sib->dec_blob = dec_blob;
    sib->pin_blob = pin_blob;
    /* The @S substitution travels too: the sibling resumes the same candidate session from the fork point, in a
       heap where the payload was already read, into the other arm. Without `cand_src` solve_flow_begin (which
       installs the running flow's substitution unconditionally) would re-read the source as an ordinary concolic
       value, solve_flow_end would never record the arm's PoC, and endpoint_suppress (keyed on the same field)
       would let it record @H endpoints built from a breakout string. `cand_fired` travels (a fire before the
       branch is both timelines'; record_sink dedups), unlike at the cold tier; `cand_verifying` does not, since
       solve_flow_begin re-derives it from `cand_src` at every switch-in. */
    if (parent->cand_src) {
        DCHECK(parent->cand_payload && parent->cand_sink,
               "a candidate session forked holding only part of its substitution — the source, the payload and "
               "the sink class are one identity and the arm would inject nothing or be unable to say what fired");
        sib->cand_src = strdup(parent->cand_src);
        sib->cand_payload = strdup(parent->cand_payload);
        CHECK(sib->cand_src && sib->cand_payload, "engine: OOM forking an @S candidate session's substitution");
        sib->cand_sink = parent->cand_sink;   /* solve.c's static table text: not owned, not copied */
        sib->cand_fired = parent->cand_fired;
        /* Where the payload came from is a fact about the bytes, so both arms have it; an arm without it would
           claim to be seeded from this session's search list with no row there, which solve.c's arrival assert
           refuses at the first branch a resumed candidate takes. */
        sib->cand_resumed = parent->cand_resumed;
    }
    /* Being a driven orphan travels: an arm of an orphan drive is that drive continued, no more reproducible from
       a decision vector than its parent, so the cold tier must refuse it too. */
    sib->orphan = parent->orphan;
    /* The name of the function it drives travels by the same sentence: the arm's recipe must say which body to
       re-take, and the park asserts a drive has a name. */
    sib->orphan_hash = parent->orphan_hash;
    /* The wait travels too: a resumed drive replays the document first, and a replay branches, so a flow still
       waiting for its function forks arms that are each the same drive; without the wait an arm would never build
       its call and the park would write an 'o' for a `fn` it lacks. The take consumes the `entered` bit, not the
       function, so N arms each call it. `fn` is carried by flow_add_unseeded; `orphan_argc` belongs to that body. */
    sib->orphan_want = parent->orphan_want;
    sib->orphan_argc = parent->orphan_argc;
    /* A field added to the candidate identity is an obligation here: the fields are copied one by one so that a
       new one is visible, and this checks they arrived. */
    DCHECK(!!sib->cand_src == !!parent->cand_src && !!sib->cand_payload == !!parent->cand_payload &&
           (sib->cand_sink != NULL) == (parent->cand_sink != NULL) &&
           sib->cand_resumed == parent->cand_resumed,
           "an arm of an @S candidate session left the fork without the substitution that makes it one — it "
           "would explore the other arm as an ordinary flow inside a heap the payload has already been read "
           "into, report its requests as observed endpoints, and be unable to record a fire; or it kept the "
           "substitution and lost the record of WHERE those bytes came from, which makes a resumed candidate's "
           "own arm indistinguishable from a candidate assembled outside the search's two doors");
    /* The rank that substitution is the denominator of: flow.h's "every non-candidate stands at exactly 0",
       asserted here, where it is transiently false and made true again. flow_fork_inherit runs first and copies
       the parent's ladder position, so between that line and the copy above the sibling holds a distance and no
       payload. It catches a candidate identity that stops being copied while the rank still is, which would carry
       up to a full point of fitness (the comparator's whole range) into ordinary exploration. Written over
       flow_distance rather than a list of rungs, so a rung added to the comparator is covered the day it is
       added. */
    DCHECK(sib->cand_payload != NULL || flow_distance(sib) == 0.0,
           "an arm stands on the @S ladder while carrying no payload to have stood there with — the fitness is "
           "a reading of bytes THIS flow injected, so an arm that inherited the rank without the substitution "
           "outranks the whole frontier on a measurement belonging to an identity it does not have");
    /* The answer token travels with the queue: the sibling resumes the operation's program and owes the same peer
       an answer of its own (engine_host_answer records the extras). The token and the document asked are on the
       program's row (`dyn_token`, `dyn_doc`, flow.h), so the queue copy below carries them. */
    if (parent->dyn_n) {              /* inherit the lazy chunks loaded up to the branch */
        sib->dyn = malloc((size_t)parent->dyn_n * sizeof(DynBody *)); CHECK(sib->dyn, "engine: OOM fork dyn");
        /* The table's columns come with the bodies; a column added to the queue is an obligation at every clone,
           free and finish site. The kinds, so an inherited dead breakout is not re-armed as a page script. */
        sib->dyn_cand = malloc((size_t)parent->dyn_n); CHECK(sib->dyn_cand, "engine: OOM fork dyn flags");
        /* Which of §8.1.4.4 Calling scripts' two algorithms runs each, so a module row is not handed to the
           classic entry on one arm. */
        sib->dyn_type = malloc((size_t)parent->dyn_n); CHECK(sib->dyn_type, "engine: OOM fork dyn script types");
        /* The address each program's bytes came from: §8.1.4.2's created-script base URL and a module row's
           module map key. */
        sib->dyn_url = malloc((size_t)parent->dyn_n * sizeof(char *));
        CHECK(sib->dyn_url, "engine: OOM fork dyn script addresses");
        /* The `script` element each row is the program of (§4.12.1.1's "execute the script element" switches on
           el; §3.1.7 `currentScript`). Copied, not duplicated: the element is the same node in the same tree,
           reachable through the DOM base segment the parent's fork froze. */
        sib->dyn_el = malloc((size_t)parent->dyn_n * sizeof(lxb_dom_element_t *));
        CHECK(sib->dyn_el, "engine: OOM fork dyn script elements");
        /* The document each row belongs to, so a child navigable's script is not compiled in the session's
           realm. */
        sib->dyn_doc = malloc((size_t)parent->dyn_n * sizeof(uint32_t));
        CHECK(sib->dyn_doc, "engine: OOM fork dyn documents");
        /* Each row's name, copied, never re-minted: a register entry outstanding at the fork is shared by both
           arms (solver/pending.h) and names its row, so a fresh name would leave it resolving to nothing in one
           arm. */
        sib->dyn_id = malloc((size_t)parent->dyn_n * sizeof(uint64_t));
        CHECK(sib->dyn_id, "engine: OOM fork dyn row names");
        /* When each row became runnable, copied for the name's reason: an inherited row arrived when the
           parent's did, and a fresh stamp would let an arm change its place in the arrival order by being forked.
           The re-mint belongs to readiness, owned by flow_deliver_one_reply. */
        sib->dyn_run = malloc((size_t)parent->dyn_n * sizeof(uint64_t));
        CHECK(sib->dyn_run, "engine: OOM fork dyn row arrival stamps");
        /* The rendezvous token of any row still owing an answer: the arm answers the same peer under the same
           token. */
        sib->dyn_token = malloc((size_t)parent->dyn_n * sizeof(char *));
        CHECK(sib->dyn_token, "engine: OOM fork dyn tokens");
        /* Whether each row is a task or the synchronous tail of the program that caused it: the arm inherits the
           parent's cursor and owed microtask checkpoint, so an inherited row must keep its order against that
           checkpoint. */
        sib->dyn_pos = malloc((size_t)parent->dyn_n);
        CHECK(sib->dyn_pos, "engine: OOM fork dyn positions");
        /* The ten arrays are one table with one length, allocated, grown and freed together, so a missing column
           is asserted rather than defaulted: a zero kind is DYN_PAGE_SCRIPT, a real kind, and would compile a
           candidate or an address (DYN_SCRIPT_SRC) as a page script. */
        DCHECK(parent->dyn_cand != NULL && parent->dyn_type != NULL && parent->dyn_url != NULL &&
               parent->dyn_el != NULL && parent->dyn_doc != NULL && parent->dyn_id != NULL &&
               parent->dyn_run != NULL && parent->dyn_token != NULL && parent->dyn_pos != NULL,
               "a flow holds queued programs with no kind, script type, address, document, name, arrival "
               "stamp, token or position column — the ten arrays are one table and are allocated together, so "
               "the arm would "
               "inherit bodies whose kind, evaluation algorithm, resolution base, realm, identity, waiting "
               "peer or place against the microtask checkpoint are lost");
        for (int i = 0; i < parent->dyn_n; i++) {
            /* The program text is referenced, not copied: a program's bytes are the same on every timeline that
               holds it and no flow writes them (solver/dyn_body.h), so a fork costs `dyn_n` pointers rather than
               the total script bytes. */
            sib->dyn[i] = dyn_body_ref(parent->dyn[i]);
            sib->dyn_cand[i] = parent->dyn_cand[i];
            sib->dyn_type[i] = parent->dyn_type[i];
            sib->dyn_el[i] = parent->dyn_el[i];
            sib->dyn_doc[i] = parent->dyn_doc[i];
            sib->dyn_id[i] = parent->dyn_id[i];
            sib->dyn_run[i] = parent->dyn_run[i];
            sib->dyn_pos[i] = parent->dyn_pos[i];
            sib->dyn_url[i] = NULL;
            if (parent->dyn_url[i]) {
                sib->dyn_url[i] = strdup(parent->dyn_url[i]);
                CHECK(sib->dyn_url[i], "engine: OOM forking an external script's address");
            }
            sib->dyn_token[i] = NULL;
            if (parent->dyn_token[i]) {
                sib->dyn_token[i] = strdup(parent->dyn_token[i]);
                CHECK(sib->dyn_token[i], "engine: OOM forking a cross-agent operation's rendezvous token");
            }
        }
        sib->dyn_n = sib->dyn_cap = parent->dyn_n;
    }
    /* The lifecycle is not copied: it lives on each Document as a heap write, so the sibling inherits every
       document's readiness through the delta it forks.
       Replies in flight, queued jobs, unstarted operations, unmade deliveries and sender commitments are all
       inherited, each as the same split: the arm gets its own array naming the parent's records. Both arms wait
       on one request (engine_pending_fetches deduplicates the pair; engine_provide fills every entry naming it)
       and each delivers on its own timeline, since the resolve function's already_resolved latch and the
       promise's settlement are per-flow state the COW delta captures. A job queued before the branch (a `.then`,
       a custom-element reaction, a listener task) is a work item both timelines owe; dropping it would show as a
       rejection reported unhandled in the arm whose `.catch` never arrived. */
    sib->jobs = flow_job_fork(ctx, parent);
    /* The array is per flow because the host walks every flow's register from outside any delta and each arm
       removes an entry when it delivers; the records are shared because a record never changes after its push
       except for the answer, which both arms wait on and observe. */
    sib->pending = pending_fork(parent->pending);
    /* The unstarted operations by the same split: a question a peer asked of this document before the branch
       was asked of the arm too, and the arm answers under its own delta (§7.2.1's multiplicity when a document's
       state is its flows). Each arm starts its own copy; the [record, token] entries are shared. */
    sib->perform_q = perform_q_fork(ctx, parent);
    /* The unmade deliveries by the same split: a message that arrived in this timeline before the branch arrived
       in the arm too, which is one message in two timelines, not one delivered twice. */
    sib->deliver_q = flow_deliver_fork(ctx, parent);
    /* Which sending timelines the arm is in, which is what makes sharing the queue above safe: an arm with the
       queue but not the commitments would re-admit a message its timeline foreclosed and deliver both arms of
       one sender branch. Same split: each arm commits on its own from here; a [vector, taken, arm] row is never
       edited, and `arm` is a per-row fact, so copying entries carries it. A per-arm field added to the row would
       be an obligation here and at flow_release. */
    sib->deliver_world_q = flow_world_commit_fork(ctx, parent);
    /* An unanswered synchronous request is re-issued, never inherited: its answer is computed under the asking
       flow's world, and the sibling's world differs from here on (two arms that navigated a frame differently
       must not resolve to one Window). An already-answered one keeps its id: both arms observed that answer. */
    for (int i = 0, n = pending_count(sib->pending); i < n; i++) {
        JSValue p = pending_entry(sib->pending, i);
        if (pending_get_int(p, PEND_KIND) == FLOW_PENDING_HOSTREQ && !pending_get_int(p, PEND_HAVE_VALUE)) {
            /* The one record the sibling cannot share, because this id is the field the arms must disagree on. */
            JS_FreeValue(ctx, p);
            p = pending_unshare(sib->pending, i);
            /* An ask like any other: the host is shown this id by engine_host_requests and must pay it, so
               `mint_req` counts it. Only the host's name moves: the machine's name is in the step state of the
               frame this sibling cloned, out of reach here (solver/pending.h's REQ_HOST). */
            pending_set_int(p, PEND_REQ_HOST, mint_req());
        }
        JS_FreeValue(ctx, p);
    }
    /* The arm's register is the parent's entry for entry, which flow_step's networking-first arm rests on: it
       defers this flow's script sequence by the register's length, bounded by "what this flow itself asked
       for", which holds only because a fork duplicates a register rather than lengthening it. This is the
       consumer's precondition, checked by different code from pending_fork's loop. Read in the dev guard
       because `pending_count` interns an atom. */
#if APICLIENT_DEV
    {
        int par_pend_n = pending_count(parent->pending);
        int sib_pend_n = pending_count(sib->pending);
        DCHECK(sib_pend_n == par_pend_n,
               "a fork left the arm's pending register a different length from its parent's — the arm is that "
               "timeline continued, so it names the same records in the same order, and a fork that lengthens "
               "one defers that arm's script sequence by replies no flow on its path ever asked for");
        /* The re-issue moved exactly one of the two names, checked entry by entry: only here are both registers
           in hand with equal lengths, so position i is one record in both. A machine name that moved strands the
           arm's frame; a host name that did not move hands one answer to two arms in contradictory worlds. An
           answered record keeps its host name, by the re-issue's rule. */
        for (int i = 0; i < sib_pend_n; i++) {
            JSValue pp = pending_entry(parent->pending, i);
            JSValue sp = pending_entry(sib->pending, i);
            if (pending_get_int(sp, PEND_KIND) == FLOW_PENDING_HOSTREQ) {
                uint32_t pm = (uint32_t)pending_get_int(pp, PEND_REQ);
                uint32_t sm = (uint32_t)pending_get_int(sp, PEND_REQ);
                uint32_t ph = (uint32_t)pending_get_int(pp, PEND_REQ_HOST);
                uint32_t sh = (uint32_t)pending_get_int(sp, PEND_REQ_HOST);
                int answered = pending_get_int(sp, PEND_HAVE_VALUE) != 0;
                DCHECKF(sm == pm, "a fork moved the arm's MACHINE name for host request %u to %u — that name "
                                  "lives in the asking machine's step state, inside the frame this arm is a "
                                  "clone of, so nothing out here can rewrite the asker and the arm's next "
                                  "re-entry would poll a register that no longer holds its question", pm, sm);
                DCHECKF(answered || sh != ph,
                        "a fork left an UNANSWERED host request sharing its parent's HOST name %u — the answer "
                        "is computed under the asking flow's world and the arm's world is not the parent's from "
                        "this instant, so one answer would be delivered into two call sites in two "
                        "contradictory worlds", ph);
            }
            JS_FreeValue(ctx, pp);
            JS_FreeValue(ctx, sp);
        }
    }
#endif
    engine_reclaim_set(prev_reclaim);   /* the sibling is fully assembled: it may be paged like any other member */
    DCHECK(g_genfork_n == 0,
           "a fork finished with generator-state swaps still in the stash — the sibling's cloned gen_data was "
           "never recorded on its delta, so it is owned by nobody and its body frame is unreachable");
    return sib;
}

/* JSFlowControlHooks.fork: the branch's fork, the assembly above plus an arm to record. The handoff statics are
   drained before the assembly, so no decision blob is ever both filled and unconsumed. */
static void engine_fork_finalize(JSContext *ctx, JSValue *clone) {
    Flow *parent = flow_running();
    void *dec, *pins;

    DCHECK(parent != NULL && g_fork_dec != NULL, "engine_fork_finalize: fork without a running flow / prepared state");
    /* A frameless parent is ordinary: a flow whose base is a module body or a job continuation holds no frame
       (flow_step's module arm leaves the row once §8.1.4.4's run a module script Evaluate returns; a job runs
       only under `!f->frame`), so the clone is an activation that is not a row's program. JS_FlowResume's
       FLOW_BASE_ASYNC_CALL arm is that clone's home, and consumers ask JS_FlowIsProgram rather than
       `!JS_FlowIsCall`, since `base_kind` has three values.
       Unmade deliveries and unstarted operations are inherited, not refused: engine_route attaches to every
       live flow, including one suspended mid-frame, and the arm is that timeline continued, so it holds the
       message and the question too and answers under its own delta (engine_host_answer records the extra
       answers under one token). A started operation's token travels too: a branch inside the answering program
       is a real peer timeline. */
    dec = g_fork_dec; g_fork_dec = NULL;
    pins = g_fork_pins; g_fork_pins = NULL;
    engine_sibling_assemble(ctx, parent, clone, dec, pins);
}

/* One arm per distinct answer: the asking side of a peer document with more than one timeline, and why
 * engine_host_answer only records. A cross-instance operation is performed by every live peer timeline
 * (engine_perform), so `otherW.length` comes back N times, each true of one timeline: a value the asking flow must
 * explore all of, the branch-fork primitive taken over an answer instead of a predicate.
 *
 * It runs from flow_step with `f` switched in, the only moment `cow_delta_fork`, `dom_cow_fork` and
 * `decide_fork_same_path` all describe this flow, and before the flow resumes its frame, since the frame is what
 * the arm clones and the machine takes the answer off the register once re-entered. One arm per step; the
 * scheduler re-ranks between them. The arm's world is a child of the asker's (flow_add records the edge), and the
 * arm takes the answering timeline's world (PEND_ANSWER_WORLD, carried back by flow_answer_perform) with its
 * answer; answer_commit_taken records it as a received commitment at the take.
 *
 * A later operation from the arm is addressed: window_proxy.c's cross-document read and remote_object.c's traps
 * write engine_flow_addressee's answer into the record (field index 3, after the world vector, because
 * engine/route.mjs reads fields 1 and 2 positionally and the tail is variadic), and engine_perform attaches it
 * only to flows the addressee does not contradict, so the arm is not answered from off-diagonal worlds. The
 * first read addresses nobody and fans out over every peer timeline, correctly: N answers are N facts
 * (HTML §7.2.2.1's `closed` is true in some arms and false in others). */
/* Named residual. Not covered: across a park the addressee is inert. A park bumps the generation, so a resumed
 * member's world is independent of every world a peer named before it, and independent is never refused (two
 * forests were never one); a resumed read fans out over the peer's whole frontier. Narrowing independent would
 * delete other peers' real timelines. Next diff builds: a meaning for `sent` across a generation, so a resumed
 * member that has posted carries an ancestry a peer can fork at (world_session_resume asserts the minted table
 * empty, so world_ancestry's filter drops every edge; the same gap leaves deliver_fork_arm writing
 * FLOW_COMMIT_ARM_NONE for a resumed sender at the receiving peer). Absence shows as: in a drive that parks and
 * resumes, the answering-timeline count engine/route.mjs logs for a read equal to the peer's whole frontier after
 * the park while the same read before it reported a strict subset. */
static int flow_answer_fork(JSContext *ctx, Flow *f) {
    int n = pending_count(f->pending), i;

    for (i = 0; i < n; i++) {
        JSValue e = pending_entry(f->pending, i), av = JS_UNDEFINED, aw = JS_UNDEFINED, se;
        JSValue *clone;
        void *parked;
        int in_program, in_job;
        Flow *sib;
        int completion;

        if (pending_extra_count(e) == 0) { JS_FreeValue(ctx, e); continue; }
        DCHECK(flow_running() == f, "an answer fork was taken while another flow was switched in — the arm would "
                                    "clone that flow's delta and DOM head and call the result the asker's");
        /* Where this flow's suspension is, asked of both homes. A flow inside a program holds its call site in
           `frame`; a flow inside a job holds no frame (flow_step runs a job only under `!f->frame`) and its call
           site is the parked continuation of the activation the job entered, as for a `.then` handler making a
           cross-instance read. The park rides the Flow while switched out and sits in the runtime's queue while
           switched in, which this is, so the runtime answers and `f->parked` must be empty. */
        in_program = f->frame != NULL;
        in_job = JS_HasParkedFlow(JS_GetRuntime(ctx));
        DCHECK(f->parked == NULL,
               "a flow holds parked continuations on its own slot while it is SWITCHED IN — the switch hands "
               "the whole set to the runtime and clears the slot, so a set here belongs to a switch-out that "
               "did not happen and the arm below would fork the runtime's set instead of this flow's");
        DCHECK(in_program || in_job,
               "a flow holding a peer's second answer is suspended in NEITHER of the two homes — it has no "
               "program frame and no parked continuation, so the call site the arm exists to resume is gone "
               "and the answer belongs to a timeline that cannot be re-entered");
        /* And not both: a job runs only while the flow holds no frame, and flow_step's resume arm drains a parked
           continuation before any program is compiled. If both ever hold, the arm needs both cloned, in the order
           flow_step resumes them. */
        DCHECK(!(in_program && in_job),
               "a flow is suspended in BOTH homes at once — it holds a program frame AND a parked "
               "continuation, so the arm below would resume one of two live suspensions and silently abandon "
               "the other. Clone both onto the arm, in the order flow_step resumes them");
        /* A routed delivery is not refused: engine_sibling_assemble forks the delivery queue, so the arm, this
           timeline continued, has its own copy. */
        /* Taken from the parent first, so the parent cannot fork over this answer again; the arm's inherited
           copy of the list is cleared below. */
        completion = pending_extra_pop(e, &av, &aw);
        JS_FreeValue(ctx, e);

        /* The arm is a clone of whichever home holds the suspension (the asserts above established exactly one).
           Each is a CHECK: a clone this engine cannot build is a peer timeline dropped off the frontier. */
        clone = in_program ? JS_FlowClone(ctx, (JSValue *)f->frame) : NULL;
        parked = in_job ? JS_CloneParkedFlows(ctx) : NULL;
        CHECK(!in_program || clone != NULL,
              "engine: a flow suspended on a cross-instance read could not be cloned — the peer's other "
              "timeline has an answer and no arm to carry it");
        CHECK(!in_job || parked != NULL,
              "engine: a flow PARKED on a cross-instance read inside a job could not have its continuation "
              "cloned — the peer's other timeline has an answer and no activation to resume with it");
        sib = engine_sibling_assemble(ctx, f, clone,
                                      decide_fork_same_path("(a peer's answer arrived — no predicate was "
                                                            "asked)"),
                                      concolic_pins_suspend());
        /* The arm's own continuation goes on the slot a switched-out flow keeps it in: its first switch-in hands
           it to the pump, and flow_step's resume arm re-enters it before anything else, where it reads the answer
           written below. The assembly takes a frame but not a park; a third caller needing a park would be the
           reason to give it that parameter (orphan drives and branch arms carry none). */
        DCHECK(sib->parked == NULL,
               "a freshly assembled arm already holds parked continuations — the assembly builds a flow that "
               "has never run, so a set on it came from somewhere that is not this fork and the write below "
               "would drop it");
        sib->parked = parked;
        /* The arm's own answer, the one record it cannot share: pending_fork shares an answered entry (an answer
           from before the fork was computed in a world both arms were in), but here the arms hold different
           answers to one question. */
        se = pending_unshare(sib->pending, i);
        pending_set(se, PEND_VALUE, av);
        pending_set_int(se, PEND_COMPLETION, completion);
        /* Whose answer it is, from the same triple: the arm belongs to a named pair of timelines, and its next
           operation is addressed with this (engine_flow_addressee). */
        pending_set(se, PEND_ANSWER_WORLD, aw);
        /* No answers beyond its own, both ways: the inherited remaining list would make the arm fork over answers
           the parent will fork over, and its answer is fixed, so a later answer belongs to the issuer. Together
           these make a peer with N timelines cost exactly N-1 arms. */
        pending_set(se, PEND_EXTRA, JS_NULL);
        pending_set(se, PEND_ANSWER_FIXED, JS_TRUE);
        DCHECK(pending_get_int(se, PEND_HAVE_VALUE) != 0,
               "an arm was forked over a peer's answer onto a request that is still UNANSWERED — the assembly "
               "re-issues an unanswered request under a fresh id, so this arm would be waiting on a question "
               "nobody has been asked");
        JS_FreeValue(ctx, se);
        return 1;
    }
    return 0;
}

/* One arm per declined request: the fork a refusal owes. The trusted zone grades each refusal on one axis: would a
 * real browser making this request also get Fetch §5.6 Fetch methods' network error? Where it would (a blocked
 * scheme, a CORS failure, a CORB-blocked body) the reply arrives as that error. Where it would not (the firing
 * policy declining an act at an unwidened origin, the destructive deny list, a method the chokepoint cannot
 * issue) the request is declined and the outcome is unobserved, so both arms run:
 *   - the waiting arm, the sibling assembled below, keeps the unanswered record and stays parked at the line that
 *     asked, a parked search that fires the day the origin is widened. It holds no reply, status or example: a
 *     synthesized 200 would be an invented datum.
 *   - the failure arm, this flow, takes §5.6's network error (JS_NULL) and runs the page's `catch`, where bundles
 *     keep fallback endpoints, retry hosts and degraded-mode configuration. Its path is marked forced in the
 *     same operation as the fork (flow_mark_forced_arm), so nothing it builds is ever graded derived; the
 *     waiting arm inherits the path as it stood before.
 * It runs where flow_answer_fork does, for the same reasons, one arm per step. Every kind takes the same JS_NULL,
 * and flow_deliver_one_reply turns it into each kind's own failure (a rejected `fetch()`, §4.12.1.1's null result,
 * HostLoadImportedModule's TypeError, a `<link>`'s error branch), so this holds no per-kind knowledge of the
 * outcome beyond the synchronous kind's completion below. */
static int flow_decline_fork(JSContext *ctx, Flow *f) {
    int n = pending_count(f->pending), i;

    for (i = 0; i < n; i++) {
        JSValue e = pending_entry(f->pending, i), se, pe, reason;
        JSValue *clone;
        void *parked;
        int in_program, in_job, kind;
        Flow *sib;

        if (!pending_entry_declined(e) || pending_get_int(e, PEND_DECLINE_TAKEN)) {
            JS_FreeValue(ctx, e);
            continue;
        }
        DCHECK(flow_running() == f,
               "a decline fork was taken while another flow was switched in — the arm would clone that flow's "
               "delta and DOM head and call the result the refused flow's");
        /* A refusal and a reply cannot both be true of one request: `haveValue` says the server answered,
           `declined` that nobody asked it. */
        DCHECK(!pending_get_int(e, PEND_HAVE_VALUE),
               "a request carries BOTH a reply and a refusal — engine_decline writes the refusal only onto "
               "records the host still owes, so a record holding an answer as well is a decline that raced a "
               "delivery and the flow would run its failure path over a body it already has");
        kind = (int)pending_get_int(e, PEND_KIND);
        reason = pending_get(e, PEND_DECLINED);
        /* A synchronous request can be here: engine_host_decline reaches it by rendezvous id, since it has no
           address for `pending_index_find`. The failure side is kind-aware in exactly two writes below: the
           completion engine_host_take requires (type and value are one write), and `pending_answer_sync` instead
           of a bare `haveValue`, since the register carries `pending_blocked`'s count (solver/pending.h). For a
           §7.4 navigation the waiting arm holds the record with no value, so `engine_host_answered` keeps
           answering no, NAV_LOAD_CREATE never runs, and the navigable keeps the initial `about:blank` §7.3.1.3
           Child navigables created; the failure arm gets the error page a browser shows. Both names are asserted
           because a fork moves one of them. */
        DCHECK(kind != FLOW_PENDING_HOSTREQ ||
                   (pending_get_int(e, PEND_REQ) != 0 && pending_get_int(e, PEND_REQ_HOST) != 0),
               "a synchronous host request carries a refusal and NO rendezvous id — the id is the only identity "
               "this kind has, engine_host_decline found the record BY it, and an arm forked from a record that "
               "names none could not be matched to the machine that is parked. BOTH names are asserted because "
               "they are two facts and a fork moves one of them: a zero MACHINE name is a record no asker can "
               "ever poll, and a zero HOST name is one the zone can never be shown");
        /* Both homes of the suspension are asked and neither is required, unlike flow_answer_fork: a flow waiting
           on a reply usually holds neither, since a `fetch()` hands back a pending promise and the program runs
           on, leaving a flow between tasks whose continuation is the reaction the delivery will enqueue.
           engine_sibling_assemble takes that shape, and a frameless arm resumes its scheduler step. An injected
           `<script src>` parks inside the inserting program (a frame); a document's external script parks at
           its row (no frame). Both at once is refused, since the arm would carry one suspension and abandon the
           other. */
        in_program = f->frame != NULL;
        in_job = JS_HasParkedFlow(JS_GetRuntime(ctx));
        DCHECK(f->parked == NULL,
               "a flow holds parked continuations on its own slot while it is SWITCHED IN — the switch hands "
               "the whole set to the runtime and clears the slot, so a set here belongs to a switch-out that "
               "did not happen and the arm below would fork the runtime's set instead of this flow's");
        DCHECK(!(in_program && in_job),
               "a flow is suspended in BOTH homes at once — it holds a program frame AND a parked "
               "continuation, so the arm below would carry one of two live suspensions and silently abandon "
               "the other. Clone both onto the arm, in the order flow_step resumes them");
        clone = in_program ? JS_FlowClone(ctx, (JSValue *)f->frame) : NULL;
        parked = in_job ? JS_CloneParkedFlows(ctx) : NULL;
        CHECK(!in_program || clone != NULL,
              "engine: a flow parked on a DECLINED request could not be cloned — the arm that keeps waiting "
              "for the real answer is the only thing that makes a per-origin widening mean anything");
        CHECK(!in_job || parked != NULL,
              "engine: a flow parked on a DECLINED request inside a job could not have its continuation "
              "cloned — the waiting arm would have no activation to resume with");
        sib = engine_sibling_assemble(ctx, f, clone,
                                      decide_fork_same_path("(the trusted zone declined this request — no "
                                                            "predicate was asked)"),
                                      concolic_pins_suspend());
        DCHECK(sib->parked == NULL,
               "a freshly assembled arm already holds parked continuations — the assembly builds a flow that "
               "has never run, so a set on it came from somewhere that is not this fork");
        sib->parked = parked;
        /* The waiting arm's own copy of the record, marked as having had its fork. The record is shared by
           pending_fork, so both sides take a private copy; otherwise the first flow to step would consume a
           decline every parked timeline is owed. The copy leaves the outstanding set (no reply is coming) and
           keeps the reason, the account of a park that will not drain. */
        se = pending_unshare(sib->pending, i);
        pending_set_int(se, PEND_DECLINE_TAKEN, 1);
        DCHECK(!pending_get_int(se, PEND_HAVE_VALUE) && pending_entry_declined(se),
               "the waiting arm of a decline came out of the unshare holding an answer, or holding no refusal "
               "— it is the arm that goes on waiting for the real one, so an answered copy would resume it "
               "over a reply nobody sent and an unrefused copy would be re-listed to the host and re-declined");
        JS_FreeValue(ctx, se);
        /* This flow's copy, answered with §5.6's network error (JS_NULL, core/fetch/fetch.h). The delivery owns
           what each kind does with it; this states the outcome. */
        pe = pending_unshare(f->pending, i);
        pending_set_int(pe, PEND_DECLINE_TAKEN, 1);
        pending_set(pe, PEND_VALUE, JS_NULL);
        if (kind == FLOW_PENDING_HOSTREQ) {
            /* For this kind the completion and value are one write (engine_host_take asserts it). The network
               error is a normal completion carrying null, never a throw, which would re-raise at the parked call
               site and make a declined navigation look like a document that threw. And through the register
               (pending_answer_sync), since `haveValue` here is the write that unblocks the flow. */
            pending_set(pe, PEND_COMPLETION, JS_NewInt32(ctx, ENGINE_COMPLETION_NORMAL));
            pending_answer_sync(f->pending, pe);
        } else {
            pending_set_int(pe, PEND_HAVE_VALUE, 1);
        }
        JS_FreeValue(ctx, pe);
        /* What this path is evidence of from now: everything after stands on an unobserved outcome, so every
           request it builds is forced (pending_prov_compose reads the mark at the park) and every endpoint a
           forced sighting endpoint.c keeps out of the derived pool. Marked after the assembly, so the waiting arm
           inherits the path from before the refusal. */
        flow_mark_forced_arm();
        DCHECK(flow_path_forced(f),
               "the failure arm of a decline is standing on an unobserved outcome and its path does not say "
               "so — every request it goes on to build would declare itself DERIVED, which publishes a value "
               "reached only because this engine supplied an answer under the claim that the app's own code "
               "computed it");
        JS_FreeValue(ctx, reason);
        JS_FreeValue(ctx, e);
        return 1;
    }
    return 0;
}

/* There is no replay fork: re-running a nested flow from its start is not byte-identical (shared state can
   differ between the run and the re-run). A concolic branch inside an async body on the tramp chain DFAILs
   (branch_arm_fork) until the async-frame snapshot is built. */

/* Orphan invoke: the flows that make a page's uncalled code run, which is how the logged-in API surface is learned
 * while logged out. An SPA ships one bundle to everybody, so the admin handler, the billing route and the lazy
 * chunk's export are in a logged-out visitor's bytes, and nothing calls them.
 *
 * Each orphan becomes one ordinary flow, assembled by engine_sibling_assemble like an answer-fork arm, differing
 * only in the frame it resumes: a call of the orphan (JS_FlowNewCall) rather than a clone. It is preemptible,
 * forks, ranks and pages under the one policy; nothing loops over orphans.
 *
 * It runs at the one point where "nothing called this function" is a fact: the parent has no program, job,
 * timer, rendering opportunity or reply left. Earlier would drive a function the next program was about to call;
 * later would be a phase. One per step: the take hands over one function and this seeds one flow, so each seeded
 * flow is ranked against its siblings and the parent before the next is taken, and the delta it forks is that of
 * whichever flow reaches exhaustion next, the timeline where the uncalled fact holds.
 *
 * Arguments are unknown external input minted by concolic_new (domain-carrying, example-free), so a gate in the
 * body forks. Each argument has its own source identity, because the per-flow constraint tracker is keyed by
 * source: with one shared identity, `if (a=='x' && b=='y')` would record two contradicting constraints and prune
 * the arm that reaches the sink. The receiver is one more unknown, so a method's `this.field` does not throw.
 * The count is the callee's own declared formal parameter count.
 *
 * A body is taken once (JS_OrphanTakeOne consumes its bit), and the flow it becomes is ranked like everything
 * else: an orphan that emits nothing accumulates service and sinks. There is no seen-set, counter or cap.
 * Returns 1 if it seeded a flow; 0 means there was nothing to take, which lets the caller finish. */
typedef struct {
    JSValue fn;    /* the taken function object (an owned ref) */
    int     argc;  /* its declared formal parameter count */
    int     n;     /* how many the take handed over — one, asserted */
} OrphanTake;

/* The visitor only records. JS_OrphanTakeOne calls it from inside the walk of the runtime's object list, so
   creating a GC object here would insert into the list being iterated; the dup is a refcount bump and allocates
   nothing (JS_OrphanTakeOne asserts it). */
static void engine_orphan_record(JSContext *ctx, JSValueConst fn, int arg_count, void *opaque) {
    OrphanTake *t = (OrphanTake *)opaque;

    /* One function per take: each becomes a flow, so a second hand-over in one call would make the seeding step
       a loop with no suspend point, which the scheduler's seam assertion aborts on. This is the consumer's
       requirement on the walk, stated where a change to the walk would break it. */
    DCHECK(t->n == 0,
           "the orphan take handed over a SECOND function in one call — each one becomes a flow, so the step "
           "that seeds them is a loop again with no suspend point on it, which is the stretch the scheduler's "
           "seam assertion aborts on");
    t->fn = JS_DupValue(ctx, fn);
    t->argc = arg_count;
    t->n = 1;
}

/* How many orphans this document has driven, reported in the result document. A drive's unknowns are named by
   the body's locator, not an ordinal: decide.c's replay compares the question each recorded arm answers, and a
   question is composed from the source identities in it, so a name that changed across sessions would discard
   every recorded arm at the drive's first branch. The locator is unique per body and stable across sessions; two
   drives of one body share it, correctly, being the same call with the same unknowns. */
static long g_orphans_driven;
/* How many times a flow got as far as asking; read with the count above (see the raise site). It is the orphan
   surface's counterpart of solver/solve.h's @S arrival census. */
static long g_orphan_asks;

/* The orphan surface's census (engine.h). It sits beside the counters it reads, which are declared here. Both or
   neither: `driven == 0` alone is three findings (the bundle ships no uncalled code; no flow reached the end of
   its own work; the walk ran and the heap had none), and the pair separates the middle one, a scheduling result,
   from the others. The first and third differ too: an empty heap is a fact about the heap, which can differ from
   the bundle (see the residual at the take in engine_orphan_seed, and @COLD's `outOfProgramsAtTheLadder`). */
void engine_orphan_census(long *driven, long *asked) {
    DCHECK(driven != NULL && asked != NULL,
           "the orphan census was asked for one of its two numbers — a drive count with no ask count beside it "
           "cannot say whether the frontier ever reached the question, which is the difference between a page "
           "that ships no uncalled code and a scheduler that never got to it");
    *driven = g_orphans_driven; *asked = g_orphan_asks;
}

/* How many of those drives the walk's preferred pass chose: a body whose source resolves a network door's entry
   name against the global object. The count lives on the runtime (the walk is quickjs's) and is forwarded here,
   where the session's context and the drive count it is read against are. A run in which no body carries that bit
   behaves exactly like a run with no order, so without this row "the order fired" and "its preferred population
   was empty" read alike, and they take opposite work (read the drives' addresses, or find out why a bundle that
   resolves `fetch` has no function body spelling it). */
long engine_orphan_preferred(void) {
    DCHECK(g_sess_ctx != NULL, "the orphan order's witness was asked for outside a session — the count is on the "
                               "runtime this file holds, so with no context there is no walk to report about");
    return (long)JS_OrphanPreferredTakes(JS_GetRuntime(g_sess_ctx));
}

/* Over how many distinct scripts those drives were spread: the order's other witness. The preference bit says
   which kind of body was chosen; this says whether the walk kept choosing from one chunk. Read as a ratio
   against the drive count (all three rows are composed on one census line): one script per hundred drives is
   one chunk monopolising the drives; a count near the page's chunk count is the order working. */
long engine_orphan_scripts(void) {
    DCHECK(g_sess_ctx != NULL, "the orphan order's fairness witness was asked for outside a session — the table "
                               "is on the runtime this file holds, so with no context there is no walk to "
                               "report about");
    return (long)JS_OrphanScriptsDrawn(JS_GetRuntime(g_sess_ctx));
}

/* The generation at which this document last walked the heap. Creating a function object is the only event that
   can add to the orphan set (JS_OrphanGen), so a walk at an unchanged generation can only find what the previous
   one took, and a walk is O(live objects). It is not a memo about any orphan, and its only error (a 2^32 wrap)
   costs one redundant walk. */
static uint32_t g_orphan_gen_seen;
static int      g_orphan_gen_valid;
/* What each ask did. engine_orphan_seed has four exits past `g_orphan_asks++`, and solver/step_unit.h names only the
   two that produce work (`seed-one-orphan-flow`, `hand-a-parked-drive-its-function`), so the residue would be
   either the memo answering for an unchanged generation or the walk finding the heap empty, which take opposite
   work (js_closure2's generation bump and the asking rate, or the heap; see the residual at the take). Each row is
   raised at exactly one exit, and engine_step_unit_runs asserts they sum to the asks, which an early return added
   between the ask and the exits would break. Per session, like the pair they partition (unlike
   `g_step_unit_runs`), so the identity holds on a host that opens several sessions per instance. A report only.
   Named residual. Not covered: the split is readable only from a DCHECK message when the partition breaks, since
   no census publishes it. Next diff builds: the three rows beside `_orphansDriven`/`_orphansAsked` in
   solver/result.c's census, landed with extension/bridge.js's forward list and extension/popup.js's reader as one
   cross-boundary diff. Absence shows as: a document whose ask count exceeds its two named step-unit rows, with
   no way to say which mechanism produced the residue. */
static long g_orphan_asks_memo, g_orphan_asks_empty, g_orphan_asks_took;
/* Which exit each ask took (solver/engine.h says which two repairs this decides between). Returned whole,
   for engine_rival_miss's reason: a partition read through three calls is three moments. */
EngineOrphanExits engine_orphan_exits(void) {
    EngineOrphanExits e;

    e.memo = g_orphan_asks_memo; e.empty = g_orphan_asks_empty; e.took = g_orphan_asks_took;
    return e;
}
/* The round trip's two numbers: waits a hand-over satisfied, and waiting flows that finished without a body. The
   third, how many were rebuilt, is the cold tier's (cold_resumed().orphans). The unmet count is counted at the
   finish, not derived: a waiting drive forks arms while it replays, so more flows can be satisfied than there
   were records, and a difference would report that as a loss. A drive that finished still waiting is the loss
   exactly (the bundle no longer holds the body, or the locator does not name it), and on unchanged bytes between
   two sessions it is zero. */
static long g_orphan_claims_met, g_orphan_claims_unmet;
/* Whether any claim is still open: a latch set by a walk that found none, rather than a counter that flow_remove,
   flow_release and the park would all have to decrement. Only cold_resume opens claims, before any flow is
   picked, so once none is open none ever is. engine_orphan_born reads it at every function object the runtime
   builds, so the latch is what makes that hook cheap; it is set at seeding (engine_orphan_claim_open). */
static int g_orphan_claims_closed;

/* Hands this body to every resumed drive waiting for it and answers how many there were. Every one, not the first:
   a take consumes the `entered` bit, not the function, and the recording session had one function with N flows
   whose frames were clones of one call, so handing it to one would starve the other arms. A walk of the frontier
   rather than a table keyed by hash, for the reason cold.c gives about its deleted index: a `Flow *` value would
   dangle once its flow left by one of several doors, and the registry cannot be stale about which flows exist. */
static int engine_orphan_route(JSContext *ctx, JSValueConst fn, int argc, uint64_t hash);

/* Is any inherited claim still waiting for a body? The routing walk's `open` predicate, asked once at seeding so a
   session that inherited no drive never pays for the walk, which engine_orphan_born would otherwise run at every
   function object. */
static int engine_orphan_claim_open(void) {
    Flow *fl;
    int i;

    for (i = 0; (fl = flow_at(i)) != NULL; i++)
        if (fl->orphan_want && JS_IsUndefined(fl->fn)) return 1;
    return 0;
}

/* The same walk asked when a body is created (quickjs's JS_SetOrphanBornHook) rather than at a take. A take
   enumerates the heap, so it can hand over only a body a live reference holds; a nested declaration's closure is
   released with its frame and is takeable for one frame only. Whether an ask falls in that window is a fact
   about the schedule. A resumed outer drive consumes a recorded arm instead of forking, so its frame ends within
   one step and a nested body's claim would never be met (on the cold fixture's residue: 2 met, 1 unmet, a fixed
   point across sessions). This hook routes and never seeds: a body nobody claimed stays takeable, and seeding here
   would mint a member from inside a compile with no flow switched in, which the take asserts against. */
static int engine_orphan_born(JSContext *ctx, JSValueConst fn, int argc, void *opaque) {
    uint64_t hash;

    (void)opaque;
    /* The latch first, before the hash: this hook runs for every function object a page builds, so a session
       with no residue pays one load and one branch, not a fold over source text. */
    if (g_orphan_claims_closed) return 0;
    hash = JS_OrphanHash(ctx, fn);
    /* The locator is this codebase's composition over page bytes, so its well-formedness is ours to assert:
       solver/cold.c refuses the same reserved value on the way in, and the ends must agree or a claim could never
       match. */
    DCHECK(hash != 0,
           "a body this runtime has just built names itself with the value the cold tier writes to mean 'not a "
           "drive' — solver/cold.c refuses that locator when it reads a residue, so a claim could never match "
           "this body and the fold that produced it has collided with the one reserved answer");
    return engine_orphan_route(ctx, fn, argc, hash);
}

static int engine_orphan_route(JSContext *ctx, JSValueConst fn, int argc, uint64_t hash) {
    Flow *fl;
    int i, open = 0, n = 0;

    for (i = 0; (fl = flow_at(i)) != NULL; i++) {
        if (!fl->orphan_want || !JS_IsUndefined(fl->fn)) continue;
        open++;
        if (fl->orphan_hash != hash) continue;
        DCHECK(fl->orphan,
               "a flow is waiting for an orphan's body without being marked as a drive — the three fields are "
               "one identity, written together by the cold tier's rebuild, so this flow was assembled by "
               "something that set part of it");
        /* Only the function crosses here; the claimant builds its call later in its own timeline, because the
           receiver and arguments are concolic objects that, minted under this flow's stamp, would be this flow's
           private state, outside every delta. */
        fl->fn = JS_DupValue(ctx, fn);
        fl->orphan_argc = argc;
        n++;
    }
    /* The latch, set by the walk itself. `open` is counted before this pass satisfies anything, so a pass that
       satisfies the last claim latches on the next one. */
    if (!open) g_orphan_claims_closed = 1;
    g_orphan_claims_met += n;
    return n;
}

/* The call a driven orphan is: mints one concolic per declared parameter and one for the receiver, and returns the
 * flow base that calls `fn` with them. A parameter with no default has no value anywhere in the program (its caller
 * is what never ran), so a symbolic argument is the only honest one. A defaulted parameter is reached anyway: the
 * parser emits `OP_get_arg k; OP_dup; OP_undefined; OP_strict_eq; OP_if_false`, so the unknown meets the page's own
 * `=== undefined` gate and forks, one arm running the page's default. Handing this site a concrete value would
 * decide that gate and delete the default's arm; ORPHAN_BINDS_OPEN checks it.
 * It runs in the timeline of the flow that will drive it: the receiver and arguments are objects stamped with the
 * running flow's generation, private to it for the session, so minting them under another flow would put this
 * flow's writes outside every delta. The unknowns are named by the body's locator (see g_orphans_driven). */
static JSValue *engine_orphan_call(JSContext *ctx, JSValueConst fn, int argc, uint64_t hash) {
    JSValue *args = NULL, self, *base;
    char id[64];
    int k;

    DCHECK(hash != 0, "a driven orphan's call was minted with no locator — every unknown it supplies would be "
                      "named after a body that does not exist, and two drives of two different functions would "
                      "then share one identity");
    if (argc > 0) {
        args = (JSValue *)malloc((size_t)argc * sizeof(JSValue));
        CHECK(args, "engine: OOM minting a driven orphan's arguments");
    }
    /* What every value bound here must be: something that decides no branch the run did not prove. concolic.h
       grades HELD as "an example a document or a server SUPPLIED", and nothing supplied an orphan's parameter, so
       a HELD value here is invented and would mark a primary arm. `example_state_of` returns NONE for an undefined
       example, which is what this site passes, and HELD for any other unpinned, uncontradicted example, so passing
       a real example fires it. It deliberately does not also refuse every concrete value: `concolic_new` returns
       concrete bytes for a pinned source and for an @S candidate substituting this source, and no public
       predicate asks the latter (`cand_matches` is static to concolic.c), so a wider assert would abort candidate
       verification. Both of those answer NONE here. A diff replacing the mint with a concrete value is invisible
       to it. Spelled once and expanded at both mints. */
#define ORPHAN_BINDS_OPEN(v, nm) \
    DCHECKF(concolic_example_state(v) != CONCOLIC_EX_HELD, \
            "a driven orphan bound %s to a value carrying a HELD example — an orphan's parameter has no value " \
            "anywhere in the program, its caller being the thing that never ran, so nothing supplied this " \
            "example and it marks a PRIMARY arm for a value no session ever produced. Where the page itself " \
            "holds a value for a parameter it holds it as a DEFAULT INITIALIZER, and that arm is already run " \
            "by the fork over the page's own `=== undefined` gate; attaching an example here deletes it", (nm))

    for (k = 0; k < argc; k++) {
        snprintf(id, sizeof id, "{orphan%016llx.arg%d}", (unsigned long long)hash, k);
        /* This engine's own instrument: the argument stood in no byte anywhere, so an address composed from it is
           runtime-only but nothing was learned from it; the provenance says so, as the assert says it of the
           example. */
        args[k] = concolic_new(ctx, id, id, CONCOLIC_WHOSE_INSTRUMENT, JS_UNDEFINED);
        ORPHAN_BINDS_OPEN(args[k], id);
    }
    snprintf(id, sizeof id, "{orphan%016llx.this}", (unsigned long long)hash);
    /* The receiver is the same fact. */
    self = concolic_new(ctx, id, id, CONCOLIC_WHOSE_INSTRUMENT, JS_UNDEFINED);
    ORPHAN_BINDS_OPEN(self, id);
#undef ORPHAN_BINDS_OPEN

    /* Named residual. Not covered: a drive cannot read a module-level binding its own timeline never initialized.
       Module bindings are closure cells written through the delta (`cell_write`; `cow_capture_varref` has no
       flow-private skip), and a module's status is per flow too (`js_module_set_status` is the one status
       assignment and calls js_module_eval_capture first), so a world whose cells read uninitialized also reads
       the module unlinked. A flow that imports links it in its own world; a drive has no import (its world is
       the discovering flow's), so `OP_get_var_ref_check` throws at its first read. The structural half of a link
       (`loaded`, `func_created`, `func_obj`, the export `var_ref`s) is baseline and built once by design; read
       JS_ModuleEvalStateSave's field list before proposing to capture it.
       Two writes are involved. Function declarations' cells are written by the declaration prologue, the body's
       `OP_push_this; OP_if_false` arm selected by a truthy `this`, which only the link walk passes (the eval
       entries pass JS_UNDEFINED); §16.2.1.7.3.1 InitializeEnvironment initializes only function declarations.
       A `const` gets its value from §16.2.1.7.3.2 ExecuteModule, which §16.2.1.6.1.3 Evaluate runs. test_forced.c
       shows the split as `mod-drive-fn == 1` beside `mod-drive-const == 0` over /chunk/mdmod.js, and `mdSelf` /
       `mdFork` read a top-level `const` from its own default parameter inside a continuation.
       Next diff builds: a drive performs Link and then Evaluate for its body's module in its own world before
       building the call. That needs the module back-pointer §16.2.1.7.3.1 names ("Set the ScriptOrModule of
       moduleContext to module"), which JSFunctionBytecode does not carry.
       Absence shows as: an uncaught `<name> is not initialized` naming a module top-level declaration on a frame
       whose receiver renders `Concolic.` (necessary, not sufficient: the prefix is the receiver's type name,
       which every concolic receiver shares), or `mdSelf`/`mdFork`'s tokens missing or reading `mdWRONG`/`mdfWRONG`.
       A drive's parent is whichever flow ran out of work, the one engine_sibling_assemble caller whose parent is
       unrelated to the work, and this rung is also reached by a flow with a frame strictly ahead and a reply in
       the air, so parents are enriched for flows whose module graph is incomplete. A queued job is not such a
       door: it runs on the running flow and inherits its world. */

    /* take_result false: a drive is for what the body reaches, not what it returns. `cv` is read by flow_step arms
       that dispatch on its type (a string would reach the javascript: URL arm); the drive's completion is carried
       by the exception arms. */
    base = JS_FlowNewCall(ctx, fn, self, argc, (JSValueConst *)args, false);
    CHECK(base != NULL, "engine: a driven orphan's call frame could not be allocated — the bit that made "
                        "this function an orphan is already consumed, so there is no second chance at it");
    /* JS_FlowNewCall dup'd the receiver and every argument into the frame; the mints are ours to release. */
    JS_FreeValue(ctx, self);
    for (k = 0; k < argc; k++) JS_FreeValue(ctx, args[k]);
    free(args);
    return base;
}

/* The three things an orphan step can do, all progress and all distinct: seed a fresh drive of a body nothing
   called, hand a body back to the drives a park recorded for it, or build the call of a drive that has one.
   0 is "nothing to take", which lets the caller finish. */
#define ORPHAN_STEP_SEEDED   1
#define ORPHAN_STEP_ROUTED   2
#define ORPHAN_STEP_RESUMED  3

/* Two functions because they answer two questions. `engine_orphan_resume` is this flow's own work: a drive it is,
 * whose body was handed back, and whose call it must build before it can run anything. `engine_orphan_seed` is the
 * frontier's work: it consumes a bit on shared bytecode (JSFunctionBytecode.entered, one per body per runtime), it
 * hands the body to a member that need not be this one, and the performing flow gains nothing of its own.
 * Asked as one, a runtime-global set would gate a per-flow verdict: no flow could finish while any un-entered body
 * existed anywhere on the heap, and running page code keeps creating them, so the frontier would only grow.
 * flow_step asks the flow's own question and then the frontier's at one boundary, the last moment before the clock
 * may move, above the clock-driven sources so whether it is reached does not depend on the document's rendering
 * opportunities (see the rung in flow_step). */
static int engine_orphan_resume(JSContext *ctx, Flow *f) {
    JSValue *base;

    /* A session that explores nothing drives nothing, as engine_prepare_fork asserts. The seed states the
     * minting half where it adds a member; here, a resumed drive is a residue record standing on a recorded path,
     * which only a session seeded from recipes holds, and a non-forking session can neither drive it nor fork its
     * arms, so holding one is asserted. §@S's candidate re-fire is one concrete path, and a conformance run measures
     * a browser, which never calls a function nobody called. The call also mints example-free unknowns, the only
     * kind a host without the source overlay holds, which would reach §8.7's `setTimeout` as an uncomputed timeout
     * and leave core/timing/event_loop.c's order with two feasible arms and no fact to choose between. */
    if (!g_sess_forking) {
        DCHECK(!f->orphan_want,
               "a flow waiting to adopt an orphan's body is a member of a session that explores nothing — its "
               "recipe was recorded by a session that forks, and this one can neither drive it nor fork the "
               "arms its path is made of, so the residue was seeded into a session that cannot serve it");
        return 0;
    }

    /* A resumed drive that has its function back builds its own call here, where the flow has nothing left to
     * run, which is where the drive belonged in the recording session: the recorded arms the replay consumed run
     * out exactly here and the call's arms are the next in the same chain. Adopting earlier would put the call in
     * front of programs still to replay. It is this flow, not a sibling: the recipe named one flow, whose reward
     * and decision chain are the drive's. */
    if (f->orphan_want && !JS_IsUndefined(f->fn)) {
        DCHECK(f->orphan && f->orphan_hash != 0,
               "a flow is waiting to adopt an orphan without being one — `orphan_want` is written only by the "
               "cold tier's rebuild and only beside the mark and the locator, so this flow was assembled by "
               "something that set one third of an identity");
        DCHECK(flow_running() == f,
               "a resumed orphan drive built its call frame while another flow was switched in — the receiver "
               "and every argument are objects stamped with the running flow's generation, so they would be a "
               "stranger's private state for the rest of the session and no delta would capture the writes");
        g_orphans_driven++;
        base = engine_orphan_call(ctx, f->fn, f->orphan_argc, f->orphan_hash);
        f->frame = base;
        /* Not counted again here: `orphanClaimsMet` counts hand-overs by the routing walk (from a take or from a
           body's creation, engine_orphan_born), and an arm that inherited the function from a waiting parent also
           reaches this line, so counting built frames would inflate a number compared against the records. */
        f->orphan_want = 0;
        return ORPHAN_STEP_RESUMED;
    }
    return 0;
}

/* The frontier's orphan surface, served by whichever member has run out of its own work (see the note above
 * engine_orphan_resume). Its gate is the minting half: the line below adds a member to the frontier
 * (engine_sibling_assemble over decide_fork_same_path), and a non-forking session must gain none from inside itself,
 * which the fork seam asserts for every other door. */
static int engine_orphan_seed(JSContext *ctx, Flow *f) {
    OrphanTake t = { JS_UNDEFINED, 0, 0 };
    uint32_t gen = JS_OrphanGen(JS_GetRuntime(ctx));
    uint64_t hash;
    JSValue *base;
    Flow *sib;

    if (!g_sess_forking) return 0;

    /* A flow ran out of work and asked whether the page shipped code it never ran: counted here, before the memo,
       because a flow that reaches the memo has asked, and counting past it would make this a fact about the memo.
       Within a forking session only: the gate above returns first, so a non-forking session reads `asked == 0`
       however many flows ran out of work, a fourth state identical to "no flow reached the end of its work". A
       consumer that can run under both regimes (the conformance host starts non-forking, the shipped host forking)
       must read `engine_session_forks()` before naming the schedule as the cause of a zero. */
    g_orphan_asks++;
    if (g_orphan_gen_valid && gen == g_orphan_gen_seen) { g_orphan_asks_memo++; return 0; }
    /* Named residual. An empty walk is not "this bundle ships no uncalled code": the take can hand over only a
       body with a live function object of its own (JS_OrphanTakeOne walks rt->gc_obj_list's JS_GC_OBJ_TYPE_JS_OBJECT
       entries and reads `p->u.func.function_bytecode`). Not covered: a body whose only closure was released, the
       ordinary shape of a minified bundle's inner helpers. Its bytecode survives only while its enclosing chain's
       root is held (`cpool` is a parent-to-child edge and JSFunctionBytecode has no parent pointer), and for a
       document script the root is the program function object flow_step's completion arm releases through
       JS_FlowFree, kept alive longer only by a parked sibling's JS_FlowClone, so even that is schedule-dependent.
       Next diff builds: a descent from the objects this walk already visits, handing over the pair (a live
       enclosing closure `p`, a never-entered body in its bytecode's `cpool`). js_closure2's switch splits the
       environment: JS_CLOSURE_REF / JS_CLOSURE_GLOBAL_REF cells are observed and taken from `p->u.func.var_refs`;
       JS_CLOSURE_LOCAL / JS_CLOSURE_ARG cells belonged to the dead frame and become unknowns named from
       `cv->var_name` beside the parent's JS_OrphanHash, as detached JSVarRefs (get_var_ref's uncaptured shape).
       Minting the whole environment instead would replace module-scope config the page computed with unknowns.
       It needs a quickjs entry (nothing in quickjs.h yields a body's nested bodies or a closure's cell by index):
       plain C linkage, quickjs.h, quickjs.c and this file in one commit. Score it on an entered body held by a
       live object (`App.init` on a global with an uncalled nested helper), whose helper's endpoint should appear
       carrying `{orphan<locator>.argN}`; a bare declaration inside an IIFE may hold no live body at all. Its
       first output should be the count of live, never-entered, non-program bodies at ask time.
       Absence shows as: two documents differing only in whether the uncalled function's closure outlives its
       frame give `driven` 2 and 0 at nearly equal `asked`, the second missing the endpoints only those bodies
       name. A `driven` of 0 beside a large `asked` is this residual until the heap is checked. */
    /* The memo is recorded only by an empty walk: a one-at-a-time take leaves the rest of the set standing at
       the same generation (seeding a flow creates no function object), so recording it after a successful take
       would declare a heap of untaken orphans empty for the session. */
    if (!JS_OrphanTakeOne(ctx, engine_orphan_record, &t)) {
        g_orphan_gen_seen = gen; g_orphan_gen_valid = 1;
        g_orphan_asks_empty++;
        DCHECK(JS_IsUndefined(t.fn), "an orphan take that reported nothing still left a function on the record");
        return 0;
    }
    /* The take's own row, raised here rather than at either exit below: the two exits (route, seed) are already
       named by solver/step_unit.h, and the partition needs the take they share as a summand of the ask. */
    g_orphan_asks_took++;
    DCHECK(t.n == 1 && !JS_IsUndefined(t.fn),
           "an orphan take reported success without handing over a function — the bit that made it an orphan "
           "is already consumed, so it can never be driven again in this session");

    DCHECK(flow_running() == f,
           "orphans were taken while another flow was switched in — the arms below fork THIS flow's delta, DOM "
           "head and decision state, so they would be assembled out of a stranger's timeline and called the "
           "discovering flow's");
    DCHECK(f->frame == NULL,
           "a flow took an orphan with a suspended frame still live — the take happens where the flow has run "
           "out of the work the page ARRANGED for it, and the arms below fork THIS flow, so a live frame means "
           "the flow was suspended mid-call and the sibling would be assembled out of a half-finished "
           "expression. (It is NOT asserted because that is the only point at which 'nothing called this "
           "function' is a FACT — that argument is retired: it is a fact only when the run is over, a run that "
           "renders is never over, and waiting for it served the surface never.)");

    /* What this body is called across sessions, computed once per body (a body is taken once): every drive must
       be parkable and the locator is its recipe; the routing below reads the same number. One pass over the
       function's source text, so the session pays one pass over the bundle. */
    hash = JS_OrphanHash(ctx, t.fn);

    /* Whether a drive this session inherited was waiting for exactly this body. The recorded flow carries the
     * path, rank and arms but not the function (a heap reference dies with its session), so the take routes the
     * function to the flow whose recipe named it, which builds the call in its own timeline (the branch at the top
     * of engine_orphan_resume). Without routing, the first flow to run out of work would take the body and drive
     * it from a fresh path, and the claimant, whose recorded arms are the gates that made it reachable, would
     * finish having driven nothing, a loss nothing reports. Skipped when no claim can be open. engine_orphan_born
     * asks the same walk at a body's creation; the two are doors onto one function, not alternatives.
     * Named residual. Not covered: whether this door still fires. Claims open only in cold_resume, inside
     * engine_sched_begin above every compile, where the born hook is installed too, so every claimed body is
     * offered and marked `entered` at creation and never reaches a take; what is left is a body already live and
     * un-entered when the claim opened, which may not exist. Next diff builds: this branch deleted, with that
     * ordering asserted at the seeding. Absence shows as: `hand-a-parked-drive-its-function` in a resumed run's
     * step-unit histogram, nonzero if this door fires, zero on every run of every host if it is dead. */
    if (!g_orphan_claims_closed && engine_orphan_route(ctx, t.fn, t.argc, hash)) {
        /* No fresh drive beside them: the claimants drive the body from the recorded paths that reached it, and
           a second timeline calling the same function would stand on nothing. */
        JS_FreeValue(ctx, t.fn);
        return ORPHAN_STEP_ROUTED;   /* progress, as seeding one is */
    }

    g_orphans_driven++;
    base = engine_orphan_call(ctx, t.fn, t.argc, hash);
    /* The other side of the gate at this function's top, asserted where the member is minted, since a take, a
       hash and a route separate the two. */
    DCHECK(g_sess_forking,
           "a driven orphan was about to be added to the frontier of a session that explores nothing — the "
           "gate at the top of this walk returns before the take, so reaching this line means a second entry "
           "into the drive was built that does not go through it");
    sib = engine_sibling_assemble(ctx, f, base,
                                  decide_fork_same_path("(a function the page never called was driven — no "
                                                        "predicate was asked)"),
                                  concolic_pins_suspend());
    sib->orphan = 1;
    /* The function it drives, flow.h's `fn`, so a walk of the frontier can say which functions are being driven.
       The assembly dup'd the parent's (undefined), which is given back here. */
    JS_FreeValue(ctx, sib->fn);
    sib->fn = JS_DupValue(ctx, t.fn);
    /* The name that outlives it, written by the park and used by a later session to find the body. Stamped at
       the one place a drive is created; its arms inherit it with the mark (engine_sibling_assemble). */
    sib->orphan_hash = hash;
    /* Where it enters the queue, asserted at the one entry that manufactures its own work: a drive entering at
       virtual time zero would carry the full optimism bonus ahead of the backlog, and a bundle has thousands of
       uncalled functions. It is assembled as a sibling, so flow_fork_inherit hands it the discovering flow's
       account, which equals SFQ's v(t) because this flow is the one in service. Every coordinate is checked: the
       completed-unit count (the optimism term), its service with the window mark, and its family (the aging
       term's halves). */
    DCHECK(sib->cpu == f->cpu && sib->cpu_gen == f->cpu_gen &&
           sib->visits == f->visits && sib->family == f->family,
           "a driven orphan entered the frontier at a virtual time that is not the running flow's — it was "
           "ranked against a clock nobody chose, and a page with many uncalled functions can then promote the "
           "work it manufactures above every flow already waiting");

    JS_FreeValue(ctx, t.fn);   /* the take's reference; the sibling holds its own dup */
    return ORPHAN_STEP_SEEDED;
}

/* What that step did, for the seam assertion, which can say a step ran too long but not what it was doing. */
static StepUnit engine_orphan_unit(int r) {
    switch (r) {
    case ORPHAN_STEP_SEEDED:  return STEP_UNIT_ORPHAN_SEED;
    case ORPHAN_STEP_ROUTED:  return STEP_UNIT_ORPHAN_ROUTE;
    case ORPHAN_STEP_RESUMED: return STEP_UNIT_ORPHAN_RESUME;
    }
    DFAIL("the orphan step reported an outcome it does not have — the caller returns PROGRESS on anything "
          "non-zero, so a fourth outcome is a step whose work nothing in the run can name");
    return STEP_UNIT_NONE;
}

/* A modelled potential close request: HTML §6.10.1 Close requests' preamble, performed by an agent with no user.
 * §6.10.1's examples (Esc, an Android back gesture, an assistive technology's dismiss, a game controller's back
 * button) are all user acts, so a run that waits for one never reaches the page's `cancel` and `close` handlers,
 * a dialog's dismissal or a popover's light-dismiss; modelling the arrival is the same kind of act as driving an
 * uncalled function. The arrival is one frame on this flow (the task the preamble queues), preemptible, parkable
 * and forkable at step 7's history-action activation; nothing loops over close requests.
 *
 * This flow, not a sibling: there is one arrival to model and this flow has nothing left to run, unlike the
 * orphan take's many bodies. It stops with no counter or cap: §6.10.2 process close watchers takes the last group,
 * so a document with three groups is modelled three times, and a task that reaches step 9's "Otherwise, there was
 * nothing watching for a close request" has established that nothing is left; that completion value
 * (core/html/close_request.h) is latched on the flow. A flow that closed something is asked again next step, and a
 * page that adds a watcher in its own `close` handler keeps being modelled.
 *
 * Everything past it is forced: it rests on an arrival nobody performed, so the mark is taken in the same operation
 * as the frame, before one opcode runs, and from then pending_prov_compose stamps every park PROV_FORCED and
 * endpoint.c keeps sightings out of the derived pool. The mark is monotone. Returns 1 if it installed the task as
 * this flow's frame; 0 means nothing is left to model, which lets the caller finish. */
static int engine_close_request_fork(JSContext *ctx, Flow *f) {
    lxb_dom_node_t *document;

    /* The orphan seed's gate, here about the fork: step 7 asks §6.4.1 Data model's history-action activation,
       answered by a fork, so a non-forking session cannot run these steps to their end (an @S re-fire and a
       conformance run send no gesture). */
    if (!g_sess_forking) return 0;
    /* §6.10.1 step 9, as this timeline already answered it (see the header). */
    if (f->close_req_none) return 0;
    DCHECK(flow_running() == f,
           "a modelled close request was built while another flow was switched in — the task reads the close "
           "watcher manager out of the running flow's COW delta, so it would answer for a stranger's timeline "
           "and close a watcher this flow never established");
    DCHECK(f->frame == NULL,
           "a flow modelled a close request with a suspended frame still live — the arrival is modelled where "
           "the flow has nothing left to run, which is the only point at which 'this document is quiet' is a "
           "fact about this timeline, and a live frame means it was still going to run something");
    DCHECK(!f->close_req,
           "a flow is already inside a modelled close request's task and has no frame — the pair is written "
           "together here and cleared together at the task's completion, so one without the other is a "
           "completion path that took one of the two");
    /* Which document the arrival targets: a real one reaches the top-level traversable's active document, this
       realm's, the document every other arm of this ladder is about. The node, not the realm, because the steps
       derive their own realm from it. */
    document = node_of(document_object(ctx));
    DCHECK(document != NULL && document->type == LXB_DOM_NODE_TYPE_DOCUMENT,
           "the realm a flow ran its programs in does not answer with a Document node — every arm of this "
           "ladder is about that realm's document, so a flow that reached the end of its work without one is "
           "standing somewhere this arm cannot name a target in");
    f->frame = close_request_flow(ctx, document);
    f->close_req = 1;
    flow_mark_forced_arm();
    DCHECK(flow_path_forced(f),
           "a flow standing inside a modelled close request does not declare its path FORCED — every request "
           "it goes on to build would say DERIVED, which publishes a value reached only because this engine "
           "sent a gesture no user sent");
    return 1;
}


/* The @S search's ask, counted at the one site a candidate row is born: the denominator of `progStartsCand`
   (g_prog_starts). Without it a zero there is either no breakout reaching an executable position (a question for
   solve_html.c's derivation) or rows never handed the thread (a question for the WFQ). Counted at the ask, upstream
   of every step that may correctly decline to run it: the pick, the compile (a breakout that does not fit its
   sink's context correctly never parses, as engine_queue_candidate says), and solver/flow.c's destroyed-document
   walk. Keyed on the row's kind, not the calling entry, so a second door to queuing one is counted by being that
   kind. A report only. */
static long g_prog_queued_cand;

/* HTML §4.12.1.1 Processing model step 33: an external script's fetch begins when the element is prepared, not when
   its position is reached. Step 33 ("If el has a src content attribute:") fetches inside prepare the script element;
   step 35 decides only where the result executes (the as-soon-as-possible set, the in-order list, the list for
   after parsing, or the pending parsing-blocking script). A document reaches the engine fully parsed, so every
   markup `<script>` is prepared before the first executes, and engine_queue_into, the one site turning an element
   into a row, owes step 33 exactly once per element. §4.12.1.1's order is untouched: the row keeps its position,
   flow_step stops at it until its own bytes arrive, and document_exec_scripts lays rows down in §13.2.7's order.
   The realm is the row's document's (step 32: "Let settings object be el's node document's relevant settings
   object"). There is nothing to deduplicate against: the row's name is minted just before this call and never
   reused (solver/flow.h's `dyn_id`).
   Named residual. Not covered: the element reads this park makes (script_csp_meta's nonce slot and `integrity`, and
   Fetch §4.3's `blob:` lookup) are answered under whichever DOM delta is applied when the row is created, which for
   an @S candidate flow (created at the pick with the outgoing flow still switched in) and engine_join_document's
   boot flow (created by the flow that built the navigable) is not the new flow's own. The policy half is
   unaffected: a policy container is written once at creation and not COW-captured. Next diff builds: creation at
   baseline for those two sites, as the session's seeding and cold_resume have. Absence shows as: a page that writes
   `s.integrity` or `s.nonce` onto a markup `<script src>` and reaches a detected sink, where the candidate's
   request is admitted or refused on an attribute value its own baseline run never had. */
static void engine_pending_docscript(Flow *f, int at, int parser_inserted) {
    /* HTML §8.1.4.2 Fetching scripts' "fetch a classic script" sets no method, so it is Fetch §2.2.5's `GET`,
       stated because the seam is keyed on the pair. Its destination is `script`: it creates "a potential-CORS
       request given url, `script`, and corsSetting", and HTML §2.5.1 Terminology's create-a-potential-CORS
       request returns "a new request whose URL is url, destination is destination", so the trusted zone reads
       this reply as code. */
    JSContext *ctx = doc_realm(f->dyn_doc[at]);
    /* The row's body is its address until the reply takes it across (solver/flow.h), and this is the first read
       treating it as a NUL-terminated string, so it is asserted here as at the delivery. An address has no NUL:
       script_src_absolute serializes a URL record (URL §4.5 URL serializing), whose components went through
       percent-encode sets built on URL §1.3 Percent-encoded bytes' C0 control percent-encode set ("C0 controls
       and all code points greater than U+007E"), so a NUL is `%00`. */
    const char *url = dyn_body_text(f->dyn[at]);
    FetchRequest req = { "GET", url, PENDING_DESTINATION_SCRIPT, NULL, NULL, 0,
                         /* placed below, off the element */ { NULL, 0, NULL, 0, CSP_PARSER_METADATA_UNPLACED },
                         /* …and so is the credentials mode, off the same element */
                         FETCH_CREDENTIALS_UNPLACED };
    /* §4.12.1.1's three metadata members, off the row's own element (engine_queue_docscript_url and the seed's
       address arm both take it; flow_deliver_one_reply asserts it on the failure path). NULL is a stated answer:
       a host driving a synthesized program list has rows no `<script>` produced, and script_csp_meta answers the
       empty string. */
    ScriptCspMeta meta;
    JSValue e;

    DCHECK(url != NULL && *url, "an external document script entry carries no URL");
    DCHECK(strlen(url) == dyn_body_len(f->dyn[at]),
           "an external document script's row holds an address with a U+0000 in it — the row's body IS its URL "
           "until the reply takes it across, and this park reads it as a NUL-terminated string, so the request "
           "the host is shown would be a PREFIX of the address the element names");
    meta = script_csp_meta(ctx, f->dyn_el[at]);
    req.metadata = meta.m;
    /* The credentials mode, off the same element by the same step as the injected script's (see
       `script_credentials_mode`); a synthesized row takes §8.1.4.2's default script fetch options. */
    req.credentials = script_credentials_mode(f->dyn_el[at]);
    /* The mode, off the element: fetch a classic script creates a potential-CORS request, so a document's own
       `<script src>` without `crossorigin` is `no-cors`, which is what an integrity policy refuses. */
    req.mode = script_request_mode(SCRIPT_TYPE_CLASSIC, f->dyn_el[at]);
    e = pending_push(&f->pending, FLOW_PENDING_DOCSCRIPT, flow_path_forced(f), flow_path_pinned(f),
                     parser_inserted, flow_work_seq_next());
    /* Which row this park is for, by name: the row's `dyn_id` rather than its position, which holds only while
       the set is fixed (solver/flow.h), and this entry outlives §4.12.1.1's immediate interposition and
       §7.5.10's removal. */
    pending_set_int(e, PEND_SCRIPT_ROW, (int64_t)f->dyn_id[at]);
    /* And §4.3 Scheme fetch with the address: a document's own `<script src="data:text/javascript,…">` is
       answered at the park, so the entry may already carry its reply when this returns, and the flow's next step
       delivers it through the networking arm above the sequence, the same door as a host-answered entry. */
    pending_park_request(ctx, e, &req);
    script_csp_meta_free(ctx, &meta);
    JS_FreeValue(ctx, e);
}

/* Queues a program into flow `f`'s sequence; every program row is created here. Each fact about the row is a
   parameter because only the caller knows it:
   - `doc`, whose program it is (the realm it compiles in), since a flow runs programs of several documents;
   - `f`, the owner: a program the page caused belongs to the flow that ran the code, a joined document's own
     scripts to the boot flow minted for it;
   - `pos` (engine.h's DynPos), because the slot a caused program takes is a step of the causing operation;
   - `stype` (which of §8.1.4.4's two algorithms evaluates it; SCRIPT_TYPE_CLASSIC is §8.1.4.4's answer for every
     row that is not an element's) and `url` (the address its bytes came from; NULL for an inline row, whose base
     URL §4.12.1.1 takes from the document);
   - `el`, the `script` element it is the program of, or NULL for a row no element caused (flow.h's `dyn_el`);
   - `body`, the shared program text, on which this takes a reference (the caller keeps and releases its own),
     so a row costs O(1) at every seed and fork (solver/dyn_body.h);
   - `parser_inserted`, §4.12.1.1's `parser document` being non-null, passed to the address park at the foot,
     which grades the request with it. It is the caller's to state: core/html/html_script.h's slot is unstated
     for a loaded document's own markup, exactly the parser-inserted population. */
static void engine_queue_into(Flow *f, uint32_t doc, DynBody *body, DynKind kind, ScriptType stype,
                              const char *url, char *token, TaskSource src, DynPos pos,
                              lxb_dom_element_t *el, int parser_inserted) {
    int at;
    /* A program queued with no flow would be dropped: there is no global queue, so the caller names the flow whose
       sequence it joins (an injected <script>'s insertion, a document's load job, a fired PoC, a joined
       document's boot), and every caller establishes both preconditions. */
    DCHECK(f != NULL, "a program was queued naming no flow — a program is a work item of the ONE frontier and "
                      "there is no member to give it to, so it would be dropped without a trace");
    DCHECK(body != NULL, "a program was queued with no body — the caller has nothing to run and the queue "
                         "entry would be a slot the compile below dereferences");
    DCHECK(doc != 0, "a program was queued naming no document — the realm it is compiled in is a fact about "
                     "the document it belongs to, and a program with none would be compiled in whichever realm "
                     "the session happens to be rooted at and read that Window's globals as its own");
    /* The kind and the token are one statement, asserted both ways at the one site that creates a row: a
       cross-agent operation's row carries the token of the peer parked on its completion, and no other kind's
       completion is read as an answer. */
    DCHECK((kind == DYN_CROSS_AGENT_OP) == (token != NULL),
           "a queued program's kind and its rendezvous token disagree — a cross-agent operation's row must "
           "carry the token of the peer waiting on its completion, and no other kind may carry one, because "
           "only that kind's completion is ever read as an answer");
    /* The sequence holds only executable programs: §4.12.1.1's null type, an import map and speculation rules
       run nothing, and document_exec_scripts and html_script_prepare drop them before here. */
    DCHECK(script_type_executes(stype),
           "a program was queued with a script type that does not execute — §4.12.1.1's null type, an import "
           "map and a set of speculation rules are registered or ignored rather than evaluated, so none of "
           "them is a program, and §8.1.4.4 has no entry to run this row with");
    /* An address belongs to a row whose bytes came from one (§8.1.4.2 bases the script on the response's URL);
       an inline row has none and §4.12.1.1 answers from the document ("let base URL be el's node document's
       document base URL"). A DYN_SCRIPT_SRC row's address is its own body until the reply arrives. */
    DCHECK(kind != DYN_SCRIPT_SRC || url == NULL,
           "an entry holding a script's ADDRESS was queued with a second address beside it — the row IS the "
           "URL until flow_deliver_one_reply replaces it with the source text, and that delivery is what moves "
           "it into the address column, so a caller writing both is naming one script two ways");
    /* Which task source, if any, queued this row: HTML §8.1.7.1 Definitions' `source`, asserted at the one site
     * creating rows so an entry added later cannot answer by omission. TASK_SOURCE_UNSTATED is written by no
     * producer. §8.1.7.1 requires "every task source must be associated with a specific task queue", and a flow's
     * work is split across carriers by what it is (this sequence, `jobs`, the pending register), so the row states
     * its source. The answers are: NOT_A_TASK for a document's seeded scripts, an element's inline program, a held
     * external-script slot and a cross-agent operation's answer; SOLVER_CANDIDATE for an @S re-fire; NETWORKING for
     * a program a response produced; NAVIGATION_AND_TRAVERSAL for §7.4.2.3.2's `javascript:` URL, which
     * core/frame/navigable.c also queues on `jobs` for that section's document load (solver/engine.h states that
     * split's residual). The NETWORKING rows are queued by the pending register's delivery arm, which is itself
     * the networking source's arm, so §8.1.4.2's one fetch-then-run task spans two carriers; whether that is
     * observable is not settled here. */
    DCHECK(src != TASK_SOURCE_UNSTATED,
           "a program was queued without saying which task source, if any, put it there — HTML §8.1.7.1 gives "
           "a task a source so that one source is in one queue, and a row that states none cannot be ordered "
           "against the same source's work on another carrier or shown not to have any");
    /* A task takes the tail of its source's queue (§8.1.7.1). The one position that is not the tail is reached by
       queuing nothing, §4.12.1.1's "immediately execute the script element el, even if other scripts are already
       executing", so an immediate row naming a task source confuses running a program in place with queuing one. */
    DCHECKF(!task_source_is_task(src) || pos == DYN_POS_APPEND,
            "a queued program named the %s task source AND asked to run IMMEDIATELY — a task is queued and "
            "takes the tail of its source's queue, and the only position that is not the tail belongs to a "
            "program the causing algorithm runs in place, which by definition no task source queued",
            task_source_name(src));
    /* The two kinds the solver queues are the two that may not name a page source, asserted both ways: an @S
       candidate is a program the solver fired, and a cross-agent operation's row answers a peer's synchronous
       read; neither was queued by any algorithm of the standard, so a source would order it against the page's
       tasks. */
    DCHECKF((kind == DYN_CANDIDATE) == (src == TASK_SOURCE_SOLVER_CANDIDATE),
            "a queued program of kind %d says %s about who queued it — only an @S candidate is the solver "
            "re-firing a sink, and only it may say so, because a source is what orders a work item against "
            "the page's own tasks and an invented one orders it against tasks nobody observed",
            (int)kind, task_source_name(src));
    DCHECKF(kind != DYN_CROSS_AGENT_OP || src == TASK_SOURCE_NOT_A_TASK,
            "a cross-agent operation's program named the %s task source — the peer is suspended AT the "
            "operand of a synchronous cross-instance read, so this program is that read's continuation and "
            "no task queue holds it; giving it a source would order a peer's suspended expression against "
            "this document's tasks",
            task_source_name(src));
    if (f->dyn_n >= f->dyn_cap) {
        f->dyn_cap = f->dyn_cap ? f->dyn_cap * 2 : 8;
        f->dyn = realloc(f->dyn, (size_t)f->dyn_cap * sizeof(DynBody *));
        f->dyn_cand = realloc(f->dyn_cand, (size_t)f->dyn_cap);
        f->dyn_type = realloc(f->dyn_type, (size_t)f->dyn_cap);
        f->dyn_url = realloc(f->dyn_url, (size_t)f->dyn_cap * sizeof(char *));
        f->dyn_el = realloc(f->dyn_el, (size_t)f->dyn_cap * sizeof(lxb_dom_element_t *));
        f->dyn_doc = realloc(f->dyn_doc, (size_t)f->dyn_cap * sizeof(uint32_t));
        f->dyn_id = realloc(f->dyn_id, (size_t)f->dyn_cap * sizeof(uint64_t));
        f->dyn_run = realloc(f->dyn_run, (size_t)f->dyn_cap * sizeof(uint64_t));
        f->dyn_token = realloc(f->dyn_token, (size_t)f->dyn_cap * sizeof(char *));
        f->dyn_pos = realloc(f->dyn_pos, (size_t)f->dyn_cap);
        CHECK(f->dyn && f->dyn_cand && f->dyn_type && f->dyn_url && f->dyn_el && f->dyn_doc && f->dyn_id &&
              f->dyn_run && f->dyn_token && f->dyn_pos,
              "engine: OOM dynamic-script queue");
    }
    at = f->dyn_n;
    if (pos == DYN_POS_IMMEDIATE) {
        /* "Immediately" is relative to the running program, so it means nothing for another flow: the slot is the
           one after the cursor of the flow that has the thread. */
        DCHECK(f == flow_running(),
               "a program was queued to run IMMEDIATELY into a flow that is not the running one — "
               "`immediately` names the slot after the program that caused it, and the causing program is in "
               "the running flow, so this row would interpose into a stranger's sequence at its cursor");
        /* The cursor itself when no program is running, the slot after it when one is. A script element inserted
           from a `.then` reaction is at the cursor; one inserted from a running <script> is after it. A live frame
           is not a live program: `frame` holds three kinds of base (JS_FlowIsProgram), and a call (a driven
           orphan, a §8.1.4.4 step 8 report, a §6.10.1 task) or a clone of a module body is not a row, so for both
           the slot is the cursor, matching the frameless parent of such an arm (an arm queuing at a different
           slot from the timeline it continues would be a reorder). Those bases stand with the cursor at the end
           of the sequence, so `script_i + 1` would be `dyn_n + 1` and fail the CHECK below on an ordinary page
           (an SSR hydration shim's inline <script> from a module body). `f->frame &&` stays: JS_FlowIsProgram needs
           a base to read. */
        at = (f->frame && JS_FlowIsProgram((JSValue *)f->frame)) ? f->script_i + 1 : f->script_i;
        /* Behind what has already been interposed at that slot: two elements one program prepares run in the order
           prepared (HTML §4.12.1.1's "immediately execute the script element" happens inside the causing
           program), so `document.write("<script>a()</script><script>b()</script>")` and two `appendChild`s run
           `a` then `b`. The witness is the base slot, not the cursor: a frameless job at cursor i and the framed
           program at cursor i-1 share base i, correctly, since the job's script goes behind that program's; the
           framed program at cursor i has base i+1 and starts a fresh run. No per-row column could do this (flow.h):
           a row an ancestor interposed is DYN_POS_IMMEDIATE too. */
        if (f->imm_at == at) at = f->imm_next;
        else                 f->imm_at = at;
        f->imm_next = at + 1;
        /* The upper bound: the cursor names the row the flow is at, so the slot after it is at most one past the
           last row. A CHECK because a larger index would memmove outside the arrays. */
        CHECK(at <= f->dyn_n,
              "a program's IMMEDIATE slot is past the end of the flow's own queue — the cursor names the row "
              "the flow is at, so the slot after it is at most one past the last row, and a larger index "
              "means the cursor and the queue disagree about how many programs this flow has");
        /* Rows may shift under an outstanding reply: the register names a row by `dyn_id` (solver/flow.h), which
           survives every shift (§7.5.10's removal walk in flow.c is the twin). */
    }
    /* The row must be somewhere this flow can still reach. A row below the cursor is never compiled (flow_step's
       ladder walks forward from `script_i`) and flow_programs_unstarted_for_document, which selects
       `k >= script_i`, does not count it, so §7.5.10 step 5 would remove nothing for it; the program would vanish
       silently. Asked of the flow being queued into, not the running one: engine_perform appends to every live
       timeline, including flows that may never step again. It is a real check: the immediate arm derives `at`
       from the cursor, while an append takes `dyn_n` and consults the cursor nowhere. */
    DCHECKF(at >= f->script_i,
            "a program was queued into slot %d of a flow whose cursor already stands at %d — the queue walks "
            "FORWARD from the cursor, so this row is behind everything this flow will ever look at: it will "
            "not be compiled, it will not be run, and flow_programs_unstarted_for_document will not count it "
            "as unstarted either, because that count selects `k >= script_i`. Nothing anywhere will say a "
            "program was lost. The defect is not here — it is whatever advanced this flow's cursor past "
            "`dyn_n`, which ENGINE_LEAVE_ROW is the one place that can",
            at, f->script_i);
    if (at < f->dyn_n) {
        size_t tail = (size_t)(f->dyn_n - at);
        memmove(&f->dyn[at + 1],       &f->dyn[at],       tail * sizeof(DynBody *));
        memmove(&f->dyn_cand[at + 1],  &f->dyn_cand[at],  tail);
        memmove(&f->dyn_type[at + 1],  &f->dyn_type[at],  tail);
        memmove(&f->dyn_url[at + 1],   &f->dyn_url[at],   tail * sizeof(char *));
        memmove(&f->dyn_el[at + 1],    &f->dyn_el[at],    tail * sizeof(lxb_dom_element_t *));
        memmove(&f->dyn_doc[at + 1],   &f->dyn_doc[at],   tail * sizeof(uint32_t));
        memmove(&f->dyn_id[at + 1],    &f->dyn_id[at],    tail * sizeof(uint64_t));
        memmove(&f->dyn_run[at + 1],   &f->dyn_run[at],   tail * sizeof(uint64_t));
        memmove(&f->dyn_token[at + 1], &f->dyn_token[at], tail * sizeof(char *));
        memmove(&f->dyn_pos[at + 1],   &f->dyn_pos[at],   tail);
    }
    /* One reference, not one copy, so nothing here can fail for memory: a row costs a pointer whatever the
       program's size, and a document's bundle costs one buffer however many timelines run it. */
    f->dyn[at] = dyn_body_ref(body);
    f->dyn_cand[at] = (unsigned char)kind;
    f->dyn_type[at] = (unsigned char)stype;
    f->dyn_url[at] = NULL;
    if (url) { f->dyn_url[at] = strdup(url); CHECK(f->dyn_url[at], "engine: OOM queued script address"); }
    /* The element is borrowed, not copied. */
    f->dyn_el[at] = el;
    f->dyn_doc[at] = doc;
    /* The row is named at creation and never again (the shift above carried every moved row's name), and the
       name is also its arrival stamp (solver/flow.c's g_work_seq), so flow_step orders the row against this
       flow's queued callbacks. It is taken when the row joins the sequence, not when its bytes were asked for.
       An interposed row is therefore younger than rows below the cursor, which is never consulted: it is
       DYN_POS_IMMEDIATE, so flow_stack_empty is false and no task may begin in front of it. */
    f->dyn_id[at] = flow_work_seq_next();
    /* Its arrival as a runnable work item is the same instant for every kind but one, so it is assigned from
       `dyn_id` and the two are equal by construction. A DYN_SCRIPT_SRC row is not runnable until its bytes
       return; flow_deliver_one_reply re-mints this column at the two lines that take a row out of that kind. */
    f->dyn_run[at] = f->dyn_id[at];
    f->dyn_token[at] = token;   /* moved: one allocation from engine_perform to flow_answer_perform */
    /* The position is kept, not consumed: flow_checkpoint_due reads it, because §4.12.1.1's immediate program ran
       inside the causing program and §8.1.4.4's checkpoint is owed only once that program's stack empties. */
    f->dyn_pos[at] = (unsigned char)pos;
    f->dyn_n++;
    /* The ask is raised where the row exists (see g_prog_queued_cand). The parser-inserted flag is carried for
       the fetch a row owes, and only an address row owes one, so `0` at every other entry is a statement about
       the row; a kind that grows a fetch later crashes here instead of being graded on a flag nobody set. */
    DCHECKF(!parser_inserted || kind == DYN_SCRIPT_SRC,
            "a row of kind %d states that a parser inserted its element while owing no fetch — HTML "
            "§4.12.1.1 \"Processing model\"'s `parser document` is "
            "read only by the ADDRESS park at the foot of this function (solver/pending.h's "
            "pending_prov_compose), so a caller setting it here has stated a fact nothing will ever read",
            (int)kind);
    if (kind == DYN_CANDIDATE) g_prog_queued_cand++;
    /* §4.12.1.1 Processing model step 33 is owed here, once per element: an address row is an element whose
       `src` branch fetches (see engine_pending_docscript). After the row is complete, because the park reads its
       document, element, body and name. The kind decides; a kind that grows an address later reaches this line
       by being that kind. */
    if (kind == DYN_SCRIPT_SRC) engine_pending_docscript(f, at, parser_inserted);
}

/* The row a caller holds already-decoded bytes for. `body` is shared text, not taken over: the caller keeps and
   releases its reference, as engine_queue_into's caller does, so one program queued in several places is paid
   for once. */
static void engine_queue_el_body(uint32_t doc, DynBody *body, DynKind kind, ScriptType stype, const char *url,
                                 TaskSource src, DynPos pos, lxb_dom_element_t *el, int parser_inserted) {
    Flow *f = flow_running();   /* the running flow owns the lazy chunk it loads */
    DCHECK(f != NULL, "a program was queued with no flow running — a program is a work item of the ONE "
                      "frontier and there is no member to give it to, so it would be dropped without a trace");
    engine_queue_into(f, doc, body, kind, stype, url, NULL, src, pos, el, parser_inserted);
}

/* The row a caller holds as unshared bytes: a `javascript:` URL, an @S candidate, a cross-agent operation's
   program. The one copy they need is made and released here. It takes `(body, body_n)`, not a C string: a
   `javascript:` URL percent-decodes `%00` to a byte, a candidate is built from attacker input, and an injected
   `<script>`'s text is whatever the page assigned; none went through HTML §13.2.5.4 Script data state (which
   emits U+FFFD for a NUL), so each can hold one. */
static void engine_queue_el(uint32_t doc, const char *body, size_t body_n, DynKind kind, ScriptType stype,
                            const char *url, TaskSource src, DynPos pos, lxb_dom_element_t *el,
                            int parser_inserted) {
    DynBody *b;

    DCHECK(body != NULL, "a program was queued with no body — the caller has nothing to run and the queue "
                         "entry would be a slot the compile below dereferences");
    b = dyn_body_new(body, body_n);
    CHECK(b, "engine: OOM dynamic-script body");
    engine_queue_el_body(doc, b, kind, stype, url, src, pos, el, parser_inserted);
    dyn_body_unref(b);
}

/* The rows no element caused: a lazy chunk's reply, §7.4.2.3.2's `javascript:` URL, an @S candidate and a
   cross-agent operation's program are classic scripts §8.1.4.4 evaluates with no element, so §3.1.7
   `currentScript` stays null while they run. A separate entry so a caller holding an element cannot pass none by
   omission. */
static void engine_queue(uint32_t doc, const char *body, size_t body_n, DynKind kind, ScriptType stype,
                         const char *url, TaskSource src, DynPos pos) {
    /* No element, so no parser-inserted flag (§4.12.1.1 gives it to `script` elements); a statement about the
       row. */
    engine_queue_el(doc, body, body_n, kind, stype, url, src, pos, NULL, /*parser_inserted*/0);
}

/* A document's script inventory seeded as the rows of one flow's sequence: the root document's seeding
   (flow_set_seed_hook) and a joined document's boot flow are the same operation on two documents. HTML
   §4.12.1.1's order is the sequence's order: a row whose source text is here is a program queued in place, one
   still owed its bytes is a DYN_SCRIPT_SRC row in place holding its address and stopping the flow there. `urls`
   are §4.12.1.1's encoding-parsed addresses, resolved once per element by the caller, whose document they belong
   to. Which shape a row takes is asked of its body, not its address: HTML §8.1.4.1 Scripts gives a script source
   text and a base URL independently, so a host that already holds an external script's response hands over both
   and the row is a program at its own address. A refused row ("If url is failure, then queue an element task on
   the DOM manipulation task source given el to fire an event named error at el, and return") becomes the
   DYN_SCRIPT_FAILED row, whose step queues that task when the flow reaches the element. An external member of
   §4.12.1.1's as-soon-as-possible set is parked on rather than given a row: the set has no order and §13.2.7
   waits for it only before the load event, so its program joins the flow whenever its reply is delivered. */
static void engine_seed_scripts(Flow *f, uint32_t doc, const RootScript *rows, int n) {
    int i, queued = 0;

    DCHECK(f != NULL, "a document's scripts were seeded onto no flow");
    DCHECK(n == 0 || rows != NULL,
           "a document was seeded with a script count and no table to read it from");
    DCHECK(f->dyn_n == 0 && f->script_i == 0,
           "a document's scripts were seeded onto a flow that already holds programs or has moved past its "
           "first — a flow is seeded once, at creation, so a second seeding would interleave two documents' "
           "program orders into one sequence and leave the cursor pointing into neither");
    for (i = 0; i < n; i++) {
        DCHECK(script_type_executes(rows[i].type),
               "a script table holds a row whose type executes nothing — §4.12.1.1's null type, an import map "
               "and a set of speculation rules are registered or ignored rather than evaluated, so "
               "document_exec_scripts drops all three before they become rows");
        DCHECK(rows[i].body != NULL || rows[i].url != NULL,
               "a script table row holds neither source text nor an address — a refused row keeps its `src` "
               "text as its body, so a row that is nothing would park the flow on nothing for the rest of the "
               "session");
        DCHECK(!rows[i].refused || (rows[i].url == NULL && rows[i].el != NULL),
               "a refused script row carries an address or no element — §4.12.1.1's \"If url is failure\" "
               "leaves no URL to carry, and the `error` event it owes is fired AT el");
        /* The position a row is queued at is the position it is addressed at: `queued` counts the rows this
           seed has given a slot, in §4.12.1.1's order, and the cursor indexes the same table. Asserted per
           row. */
        DCHECK(f->dyn_n == queued, "a document's scripts left the seed out of document order — the row about "
                                   "to be queued is not the one the sequence is at, so the flow's cursor would "
                                   "reach this element at some other element's position");
        if (rows[i].refused) {
            /* The task is queued by the row's step, in this flow's own world, at the place the parser would
               have prepared the element. */
            engine_queue_into(f, doc, rows[i].body, DYN_SCRIPT_FAILED, rows[i].type, NULL, NULL,
                              TASK_SOURCE_NOT_A_TASK, DYN_POS_APPEND, rows[i].el, /*parser_inserted*/0);
            queued++;
            continue;
        }
        if (!rows[i].body && rows[i].sched == SCRIPT_SCHED_ASAP) {
            /* The set's member parks on this flow; every element of a document's own inventory was placed by
               its parser (core/loader/document_scripts.c asks script_block_schedule with parser_inserted). */
            engine_pending_script_url_into(f, doc_realm(doc), rows[i].url, rows[i].type, rows[i].el,
                                           /*parser_inserted*/1);
            continue;
        }
        /* The element travels with the row at both positions: "execute the script element" switches on el, and
           an external row is that element's program from the moment it takes its slot. NULL is a statement: a host
           driving a synthesized program list (wpt_runner.c's harness prologue and epilogue) has rows no `<script>`
           produced, and §3.1.7's answer while one runs is null.
           A program row with an address keeps it in the address column (§8.1.4.2: "creating a classic script given
           sourceText, settingsObject, response's URL, …"), so flow_dyn_url answers it at the compile and a module
           is keyed by its response URL. A row with only an address is the DYN_SCRIPT_SRC shape, whose body is its
           URL until flow_deliver_one_reply takes it across. The program row references the table's body; the
           address row makes a small body of its own, released here once engine_queue_into has its reference.
           No task source at either position: a document's own scripts are run by the parse that reached them
           (§8.1.7.1 Definitions: "The HTML parser tokenizing one or more bytes, and then processing any resulting
           tokens, is typically a task"), in the order §4.12.1.1 fixed, which the sequence is. */
        if (rows[i].body) {
            /* No fetch is owed: the row's source text is in the table. */
            engine_queue_into(f, doc, rows[i].body, DYN_PAGE_SCRIPT, rows[i].type, rows[i].url,
                              NULL, TASK_SOURCE_NOT_A_TASK, DYN_POS_APPEND, rows[i].el,
                              /*parser_inserted*/0);
            queued++;
        } else {
            /* An address is the one body whose length is its `strlen`: script_src_absolute serializes a URL record
               (URL §4.5 URL serializing), whose components went through percent-encode sets containing U+0000 (URL
               §1.3 Percent-encoded bytes' C0 control percent-encode set, "C0 controls and all code points greater
               than U+007E", is contained in every other set there), so a NUL is `%00`. flow_deliver_one_reply
               asserts it at the read that depends on it. */
            DynBody *addr = dyn_body_new(rows[i].url, strlen(rows[i].url));
            CHECK(addr, "engine: OOM seeding an external script's address as its row's body");
            /* Whether a parser inserted the element, for the fetch it owes, read from whether the table holds an
               element: every row came out of document_exec_scripts' walk of a parsed document, so an element here
               was placed by that document's parser, and rows without one are the host's synthesized list.
               Named residual. Not covered: a row whose element did not come from a parser's walk; none exists
               (both builders fill `el` from a script inventory, and the joined one asserts every row has one),
               but the reading is an inference from the table's contract. Next diff builds: a `parser_ins` column
               on RootScript, written by both builders from what the host said. Absence shows as: a builder
               accepting an element for a row no parser produced grades that row's program load `observed`. */
            engine_queue_into(f, doc, addr, DYN_SCRIPT_SRC, rows[i].type, NULL, NULL,
                              TASK_SOURCE_NOT_A_TASK, DYN_POS_APPEND, rows[i].el,
                              /*parser_inserted*/rows[i].el != NULL);
            dyn_body_unref(addr);
            queued++;
        }
    }
}

/* The root document's seeding: a fresh timeline of this instance's document. It is the seed hook flow_add asks
   (solver/flow.h), so the boot flow, every cold-resumed replay and every @S candidate session get one sequence by
   construction. */
static void engine_seed_root_flow(Flow *f) {
    DCHECK(g_sess_live, "a flow was seeded with the root document's programs outside a live session — the "
                        "table it reads is built when a session opens and given back when one closes");
    engine_seed_scripts(f, g_sess_doc, g_root_scripts, g_root_n);
}

/* Which kind the program at `script_i` is, for the compile and the resume. Re-derived from the cursor, which
   already says which program is running, rather than latched in a field that could lag. The cursor indexes one
   table; past the end is a real position (the accessors are read before the step decides what the flow does). */
static DynKind flow_dyn_kind(const Flow *f) {
    if (f->script_i >= f->dyn_n) return DYN_PAGE_SCRIPT;
    return (DynKind)f->dyn_cand[f->script_i];
}

/* How many program rows of the live frontier stand on an address now: the live companion to
   `rootProgramsAwaitedAtSeed`. That constant says what the document owed when its rows were laid down; this gauge
   says whether those bytes came: `17` beside `0` is a bundle that arrived whole (a run that stopped short is the
   order failing), `17` beside `17` is a bundle that never arrived (the fetch path). Per flow: a row's kind lives on
   each member's table (flow.h's `dyn_cand`) and a fork copies it, so one row awaited by N members counts N times,
   each independently stopped there. It is a second walk of the frontier, like `flow_host_owed_count()`, because the
   struct `engine_frontier_census` fills is documented as lifetime counts.
   Named residual. Not covered: this walks the members the registry holds, so a member the level-1 pager sold takes
   its awaited rows with it. Next diff builds: the same count taken as a member is written out, carried on the
   park's record beside `sold` (the residue is a recipe with no rows to recompute from, solver/cold.h). Absence
   shows as: on a run with nonzero `sold`, this row falling with no delivery made. */
long engine_rows_awaiting_bytes(void)
{
    long n = 0;
    Flow *f;

    for (int i = 0; (f = flow_at(i)) != NULL; i++) {
        /* The parallel arrays are allocated together (solver/flow.h), so a member with no rows has none. */
        if (!f->dyn_cand) continue;
        for (int r = 0; r < f->dyn_n; r++)
            if ((DynKind)f->dyn_cand[r] == DYN_SCRIPT_SRC) n++;
    }
    return n;
}

/* Which document it belongs to, from the same cursor. Past the end there is no row, and the flow's own document is
   the session's. */
static uint32_t flow_dyn_doc(const Flow *f) {
    if (f->script_i >= f->dyn_n) return g_sess_doc;
    return f->dyn_doc[f->script_i];
}

/* Which of §8.1.4.4 Calling scripts' two algorithms evaluates it, from the same cursor; the row answers for every
   position, the session document's own `<script type=module>` included. */
static ScriptType flow_dyn_type(const Flow *f) {
    if (f->script_i >= f->dyn_n) return SCRIPT_TYPE_CLASSIC;
    return (ScriptType)f->dyn_type[f->script_i];
}

/* The address its bytes came from: §8.1.4.2 Fetching scripts' created-script base URL, and a module row's module
   map key. NULL for an inline row and past the end, which the compile reads as §4.12.1.1's "let base URL be el's
   node document's document base URL". */
static const char *flow_dyn_url(const Flow *f) {
    if (f->script_i >= f->dyn_n) return NULL;
    return f->dyn_url[f->script_i];
}

/* The `script` element it is the program of: HTML §4.12.1.1's "execute the script element" switches on el, and its
   classic arm brackets the run with §3.1.7 `currentScript`. The run is a work item that may start and complete in
   different scheduler steps, so the element must be answerable at the completion, not held in a C local. NULL past
   the end of the sequence (where a driven orphan's call frame sits) and for every row no element caused, both
   §3.1.7's own answer. */
static lxb_dom_element_t *flow_dyn_el(const Flow *f) {
    if (f->script_i >= f->dyn_n) return NULL;
    return f->dyn_el[f->script_i];
}

/* Whether a reply ever became a program, per door, with each arm's denominator (solver/engine.h's `net_prog_*`
   block). The two `…_asks` counters are raised where a door holds a reply record, before any type is read, since
   both doors correctly decline non-JavaScript replies. The total is raised at engine_queue_fetched_script and the
   arms at the doors, so `fetch + xhr <= total`, not a partition: test_forced.c's `loadScript` host edge is a third
   caller that reaches no delivery arm. The inequality catches a door raising a queued arm without going through
   the entry, a second compile door. A report only. */
static long g_net_prog_queued, g_net_prog_fetch_asks, g_net_prog_fetch_queued;
static long g_net_prog_xhr_asks, g_net_prog_xhr_queued;

/* See solver/engine.h for why these are four entries rather than one taking a door. Each is one statement; the
   `…_ask` pair gives the `…_queued` pair a denominator. */
void engine_note_net_prog_fetch_ask(void)    { g_net_prog_fetch_asks++; }
void engine_note_net_prog_fetch_queued(void) { g_net_prog_fetch_queued++; }
void engine_note_net_prog_xhr_ask(void)      { g_net_prog_xhr_asks++; }
void engine_note_net_prog_xhr_queued(void)   { g_net_prog_xhr_queued++; }

/* A program whose bytes came from a response: a task, so the tail is its position (engine.h's DynPos), and a
   classic script, §8.1.4.4's answer for a program with no `<script>` behind it (engine_queue_element_script takes
   the element's type). §8.1.4.2 creates the script with the response's URL and §8.1.4.1 Scripts makes it the base
   URL ("the URL from which the script was obtained, for external scripts"), so the address is required: the
   compile reads a missing address as JS_EVAL_FLAG_INLINE_SCRIPT (rendered against this visitor's credentials) and
   would name the page as the throw site of every error the chunk raised. A timer's string handler is not queued
   here: HTML §8.7 Timers creates and runs it at the expiry, in step 9's task (substeps 9.8.7-9.8.8), so
   core/timing/timer.c compiles it there. */
void engine_queue_fetched_script(uint32_t doc, const char *body, size_t body_n, const char *url) {
    DCHECK(body != NULL, "a fetched program was queued with no source at all — the caller holds the reply's "
                         "bytes and there is no program in a null one");
    DCHECK(url != NULL && *url != '\0',
           "a program whose bytes came from a RESPONSE was queued with no address — §8.1.4.2 creates the "
           "script with the response's URL and §8.1.4.1 keeps it as the script's base URL, so a caller here "
           "with none has the bytes and has thrown away where they came from; nothing downstream can "
           "re-derive it, and the document's address is another script's answer rather than a weaker one");
    /* The total, raised at the one entry where a program from a response is born. Its row is a DYN_PAGE_SCRIPT like
       the document's own, so the kind cannot carry this as DYN_CANDIDATE carries `g_prog_queued_cand`; a caller is
       counted by calling, and engine_frontier_census compares the door arms against it. */
    g_net_prog_queued++;
    engine_queue(doc, body, body_n, DYN_PAGE_SCRIPT, SCRIPT_TYPE_CLASSIC, url, TASK_SOURCE_NETWORKING,
                 DYN_POS_APPEND);
}

/* The row a `<script>` element put there: the same position plus the element's type, since §4.12.1.1's "execute
   the script element" switches on it and flow_step routes to §8.1.4.4's run a classic script or run a module
   script. Its callers are the three ways a non-session Document of this agent gets its inline programs: a child
   navigable (core/frame/navigable.c), a joined document (engine_join_document) and an element page code inserted
   (core/html/html_script.c). The tail is §4.12.1.1's answer for every destination a module or `src` element
   reaches: the list for after parsing and the in-order as-soon-as-possible list hold their elements in order, and
   the as-soon-as-possible set has no position (§13.2.7 waits for it only before the load event). The one
   destination that is not the tail, immediate execution, has its own entry below. It carries the element, whose
   classic arm brackets the run with §3.1.7 `currentScript`. */
void engine_queue_element_script(uint32_t doc, const char *body, size_t body_n, ScriptType stype,
                                 lxb_dom_element_t *el) {
    DCHECK(el != NULL, "a `<script>` element's program was queued with no element — this entry is the one an "
                       "ELEMENT reaches; the element-less entry is engine_queue_fetched_script, so a caller "
                       "here with nothing to pass is a caller at the wrong entry");
    /* No fetch is owed, so no parser-inserted flag (engine_queue_into asserts only an address row carries one). */
    engine_queue_el(doc, body, body_n, DYN_PAGE_SCRIPT, stype, NULL, TASK_SOURCE_NOT_A_TASK,
                    DYN_POS_APPEND, el, /*parser_inserted*/0);
}

/* The one script source that is not a task: §4.12.1.1's prepare the script element ends "Otherwise, immediately
   execute the script element el, even if other scripts are already executing", so an inserted inline classic
   script runs at the slot after the inserting program, ahead of everything the sequence holds (html_script.c
   computes SCRIPT_SCHED_IMMEDIATE). The type is classic, not a parameter: "If el's type is `classic` and el has a
   src attribute, or el's type is `module`" sends every module to one of the lists above, and an inline module has a
   graph to load before its result exists. */
void engine_queue_script_immediate(uint32_t doc, const char *body, size_t body_n, lxb_dom_element_t *el) {
    DCHECK(el != NULL, "an inline classic script was queued to run IMMEDIATELY with no element — §4.12.1.1 "
                       "reaches `immediately execute the script element` from `prepare the script element`, "
                       "whose whole subject is EL");
    /* No fetch is owed (see engine_queue_element_script). */
    engine_queue_el(doc, body, body_n, DYN_PAGE_SCRIPT, SCRIPT_TYPE_CLASSIC, NULL, TASK_SOURCE_NOT_A_TASK,
                    DYN_POS_IMMEDIATE, el, /*parser_inserted*/0);
}

/* The external sibling, at the same kind of position with only an address (DYN_SCRIPT_SRC). The caller resolved
   the URL, because §8.1.3.2 Environment settings objects' API base URL belongs to the element's document. Append
   is §4.12.1.1's answer: this is the list of scripts that will execute in order as soon as possible, whose elements
   hold their places against one another. */
void engine_queue_docscript_url(uint32_t doc, const char *url, ScriptType stype, lxb_dom_element_t *el,
                                int parser_inserted) {
    /* The address is the row's body, not its address column: the row is the URL until the reply replaces it, and
       flow_deliver_one_reply moves it into the address column then (engine_queue_into asserts against both). */
    DCHECK(el != NULL, "an external document script took its slot with no element — the row is that element's "
                       "program from the moment it takes the position, not from the moment its bytes arrive, "
                       "and §4.12.1.1's `execute the script element` is a switch on EL");
    /* Its length is its `strlen`: script_src_absolute serialized it (URL §4.5), through percent-encode sets
       containing U+0000 (URL §1.3), and flow_deliver_one_reply asserts the pair again where the body becomes a C
       string. */
    engine_queue_el(doc, url, strlen(url), DYN_SCRIPT_SRC, stype, NULL, TASK_SOURCE_NOT_A_TASK,
                    DYN_POS_APPEND, el, parser_inserted);
}

/* An @S candidate, queued as the program it would be if it fired: the same queue (it is code the page caused to
   run) with one difference, that it may fail to compile. Most breakouts do not fit most sink contexts, and a
   candidate that does not parse simply never fires. The document is the session's: a candidate is the same
   document re-run with one attacker value substituted (a breakout in a child navigable is a candidate session of
   that document's instance). `pos` is the sink's semantics (engine.h): an eval sink's code runs inside the call
   expression (ECMAScript §19.2.1.1 PerformEval); a markup sink's auto-firing handler and a URL sink's `javascript:`
   navigation are tasks. `(body, body_n)`, because a candidate is built from attacker-shaped bytes (a decoded `%00`,
   a `\0` from a reply), and read to the first NUL it would fire a different program from the one the search chose. */
void engine_queue_candidate(const char *body, size_t body_n, DynPos pos) {
    engine_queue(g_sess_doc, body, body_n, DYN_CANDIDATE, SCRIPT_TYPE_CLASSIC, NULL,
                 TASK_SOURCE_SOLVER_CANDIDATE, pos);
}

/* HTML §7.4.2.3.2 evaluate a javascript: URL, steps 6-7: "let script be the result of creating a classic script
   given scriptSource … let evaluationStatus be the result of running the classic script script". The source is the
   page's own code, so it is a program of the running flow, preemptible, forkable and parkable. `doc` is the target
   navigable's active document (step 5: "let settings be targetNavigable's active document's relevant settings
   object"), not always the session's: a `<form action="javascript:…" target=frame>` runs in the frame's realm.
   Appended, because §7.4.2.2 Beginning navigation reaches this by "Queue a global task on the navigation and
   traversal task source given navigable's active window to navigate to a javascript: URL", so it is a task.
   `body_n` is step 3's answer: "Let scriptSource be the UTF-8 decoding of the percent-decoding of
   encodedScriptSource" reaches all 256 byte values, so `javascript:a=%00` has a U+0000, which ECMAScript §11.1
   Source Text permits. */
void engine_queue_javascript_url(uint32_t doc, const char *body, size_t body_n) {
    /* Classic: step 6 says "creating a classic script". */
    engine_queue(doc, body, body_n, DYN_JAVASCRIPT_URL, SCRIPT_TYPE_CLASSIC, NULL,
                 TASK_SOURCE_NAVIGATION_AND_TRAVERSAL, DYN_POS_APPEND);
}

/* A host instrument's program, queued into every live timeline (engine.h). Nothing runs inside this call: it is
   called between steps on the host's time, with no flow running and no slice open, and only appends a row to
   each flow's sequence; the scheduler compiles and evaluates it later with that flow switched in, so the answer
   is about that timeline's document. The document is the session's, as for engine_queue_candidate (a child
   navigable is reached by walking into it from the root, the instrument's business). Appended: two dumps asked in
   turn are two questions, not two worlds. `flow_clear_host_owed` is part of the ask: the flow now holds work it
   did not hold, and without the clear the flows parked on a reply, most of them on a real page, would never
   answer. */
void engine_request_dump(const char *program) {
    int n, i;
    DynBody *b;

    DCHECK(program != NULL && *program,
           "a host instrument asked for a dump with no program to run — there is nothing to evaluate and "
           "nothing to take a completion value from, so every flow would be given an empty row to compile");
    DCHECK(g_sess_live, "a dump was asked of an instance with no live session — there is no scheduler to run "
                        "the program and no timeline whose document it would be about");
    /* Its length is its `strlen` because the host wrote it: an instrument handed across an ABI of NUL-terminated
       strings, not page bytes, a percent-decoding or an attacker payload. */
    b = dyn_body_new(program, strlen(program));
    CHECK(b != NULL, "engine: OOM making the body of a host instrument's program");
    n = flow_count();
    DCHECK(n > 0,
           "a dump was asked of a document whose every timeline had already finished — there is no flow to "
           "run the program, so the answer would be an empty register the driver reads as a document that "
           "produced nothing rather than as a question nobody was left to answer");
    for (i = 0; i < n; i++) {
        Flow *f = flow_at(i);
        engine_queue_into(f, g_sess_doc, b, DYN_VALUE_DUMP, SCRIPT_TYPE_CLASSIC,
                          NULL, NULL, TASK_SOURCE_NOT_A_TASK, DYN_POS_APPEND, NULL,
                          /*parser_inserted*/0);   /* the host's own text, and no element */
        flow_clear_host_owed(f);
    }
    dyn_body_unref(b);   /* every row took its own reference */
}

/* An image of every live timeline (@PERWORLD; engine.h has the contract, flow.h's `paint_owed` the mark). Nothing
   runs inside this call: it writes N bits, and the host renders after the return the mark causes, so the solver
   neither paints nor holds a picture. The walk is engine_request_dump's, for its reason (the DOM is per flow), but
   without its `flow_clear_host_owed`: a picture is of the state a member is already in and adds no work, so
   clearing the mark would re-admit a member that still cannot progress, to be stepped, answer FLOW_STEP_OWED and
   be re-marked (the re-admission engine_sched_slice removed from the top of every slice). A member waiting on the
   host is photographed when the host answers it. A drained frontier is a DCHECK, for engine_request_dump's reason.
   The member whose finish is deferred is skipped: the discharge at FLOW_STEP_DONE clears its mark and returns
   with it still in the frontier, so re-marking it would photograph an ended world, which the deferred-finish
   path asserts against. The host already holds that member's picture, and cannot see g_sess_finish_owed, so the
   skip belongs here. With it, re-asking every round terminates: a mark is spent at a member's end (one return,
   one picture) or free at any yield, and re-marking a marked member writes the bit it has, so each member that
   ends costs exactly one extra return. */
static void paint_mark_standing_members(void) {
    int n, i;

    DCHECK(g_sess_live, "an image of every world was asked of an instance with no live session — there is no "
                        "scheduler to hand the thread back with a member standing, so no mark could ever be "
                        "discharged and the host would wait for pictures nobody is going to take");
    n = flow_count();
    DCHECK(n > 0,
           "an image of every world was asked of a document whose every timeline had already finished — there "
           "is no member left to be switched in, so the only world a host can render from here is the "
           "baseline, which is the very reach main.c's qjs_paint residual says this ask exists to widen");
    for (i = 0; i < n; i++) {
        Flow *f = flow_at(i);
        if (g_sess_finish_owed && f == g_sess_cur) continue;
        flow_set_paint_owed(f);
    }
}

/* One picture per world, for every world this run goes on to mint (@PERWORLD; engine.h). Two populations, two
   lines: the members standing now, which only a walk of the frontier names, and members not yet born, which only
   the mint can mark. The walk alone misses every arm forked later; the mode alone misses the boot flow, which before
   the first step is the whole frontier. It replaces a per-round re-ask, which cannot reach a member forked, run
   and ended between two of the host's returns (the host only sees round boundaries). Not a bound: the count of
   worlds photographed grows with the frontier, and each member pays one extra return at its own end. The price,
   one CSS 2.1 §E.2 Painting order walk per member that ends, on the engine's thread, is the host's to refuse by
   not asking. */
void engine_request_paint_every_world(void) {
    flow_paint_every_world();
    paint_mark_standing_members();
}

/* The answer record's one writer, declared ahead of the first of its two callers; its body sits beside the other,
   whose locals it was factored out of. */
static void perform_answer_notify(JSContext *ctx, JSContext *ectx, Flow *f, const char *token,
                                  int completion, JSValueConst cv);

/* The operation becomes this flow's next program. Not a call: a peer answers by running a program, and each is
   page code (an IDL getter, a page's setter or function) that needs a flow base. Queued with the flow switched in,
   so the operands are written into this flow's delta. It runs in the realm of the document the peer named: §7.2.1's
   member is read of the other navigable's active document, and answering from this instance's root would count
   the root's child navigables. The document travels as the wire's handle, which survives a park where a realm does
   not, on the row's `dyn_doc`. */
static void flow_perform(JSContext *ctx, Flow *f)
{
    JSValue e, rv, tv;
    const char *record, *token;
    RemoteOp *op;
    WorldId w;
    const WorldId *anc;
    int n_anc;
    CowDelta *seg;
    uint32_t doc;
    const char *member;
    JSValue answer;

    DCHECK(flow_running() == f, "a cross-agent operation was performed while another flow was switched in — its "
                                "operands would be written into that flow's delta and its program would run "
                                "against that flow's document");
    e  = perform_q_take(ctx, f);
    rv = JS_GetPropertyUint32(ctx, e, 0);
    tv = JS_GetPropertyUint32(ctx, e, 1);
    DCHECK(JS_IsString(rv) && JS_IsString(tv),
           "a queued cross-agent operation is missing its record or its rendezvous token — the pair is written "
           "in one bracket at arrival and never edited, so half of one is a queue something outside this file "
           "has written to");
    record = JS_ToCString(ctx, rv);
    token  = JS_ToCString(ctx, tv);
    CHECK(record != NULL && token != NULL,
          "engine: OOM reading a queued cross-agent operation — a record this instance cannot read is a peer's "
          "flow suspended at the read that asked, with nothing left that knows what it asked");
    op = remote_op_parse(record);
    doc = world_doc_intern(remote_op_doc(op));
    n_anc = world_parse(remote_op_worlds(op), &w, &anc);
    /* Asked again when it runs, for flow_deliver's reason: other flows have run since the record arrived. */
    seg = world_segment(ctx, w, anc, n_anc);
    (void)seg;
    DCHECK(cow_delta_empty(seg),
           "a cross-agent operation ran in the answering flow's timeline alone while the asking world holds "
           "writes in this instance — it answers about a document missing everything the asking flow did here. "
           "Build the join of the two deltas that engine_route names");
    /* §7.2.1's two record-answerable members (`length`, `closed`) need no program, so the realm lookup is asked only
       on the program route, where its answer is spent: a navigable whose Document §7.5.10 destroyed in this
       timeline has no realm (window_proxy_set_destroyed nulls it) and its record is still readable. The answer is
       the same as the program's: §7.2.1.3.4 CrossOriginGetOwnPropertyHelper ( O , P ) runs "the same steps as the
       getter of the IDL attribute P on object O", and core/frame/window.c's getters are
       `iframe_child_navigable_count(ctx)` and `window_proxy_closed(ctx, nav)` over the same record
       window_proxy_record_member_of_document reads. The program route would also materialize a realm per flow for a
       navigable no read reached through (proxy_get_step's hosted arm measured that cost).
       The program route stays, for the rest of the open population: §7.2.1.3.1 CrossOriginProperties ( O ) lists
       thirteen names, nine with [[NeedsGetter]], a record is untrusted text, and `location`, `opener`, `parent` and
       `top` answer objects of the target realm.
       `ctx` is this instance's root realm, a sibling of the named document's realm in the same agent, and
       core/frame/remote_object.c's encoder consults the realm it is handed only for an object
       (`window_proxy_navigable_of` resolves a Window against it; a generic export keys on `world_local_doc()`), so an
       object encoded here would be lent under the wrong document's name. A number or boolean reads no realm
       (JS_ToBoolFree and JS_ToFloat64FreeAt answer from the tag). The record route mints only JS_NewInt32 and
       JS_NewBool today; the DCHECK guards a widening to a member whose answer is an object. */
    member = remote_op_member(op);
    if (member != NULL && window_proxy_record_member_of_document(ctx, doc, member, &answer)) {
        DCHECK(JS_IsNumber(answer) || JS_IsBool(answer),
               "a §7.2.1 member was answered from the §7.2.3 record and the value is neither a NUMBER nor a "
               "BOOLEAN — this route converts it in THIS INSTANCE'S ROOT realm rather than in the named "
               "DOCUMENT's, which is sound only while the conversion reads no realm. An OBJECT does: "
               "remote_object_encode resolves a Window global against the realm it was handed and keys a "
               "generic export's name on world_local_doc(), so this one would be lent under the wrong "
               "document's name and the asker would hold a reference into a document that never lent it. "
               "A member whose answer is an object is answered by the PROGRAM route below, in the target's own "
               "realm — route it there rather than widening this one");
        perform_answer_notify(ctx, ctx, f, token, ENGINE_COMPLETION_NORMAL, answer);
        JS_FreeValue(ctx, answer);
    } else {
        /* The entry is spent and the token survives it: the record has become a program, and the token is copied
           onto the program's row, because the answer is the program's completion. Copied, not moved: a forked arm
           may still name the queue entry, so the row owns its own string. */
        char *own = strdup(token);
        JSContext *rctx = doc_realm(doc);
        /* The program is read before the row is made, because reading it sets the operation's operands on the
           answering realm's global (remote_op.c). Its length is its `strlen`: remote_op_program answers one of this
           engine's fixed programs, not page text. */
        const char *prog_src = remote_op_program(rctx, op);
        DynBody *prog = dyn_body_new(prog_src, strlen(prog_src));
        CHECK(own != NULL, "engine: OOM moving a cross-agent operation's rendezvous token onto its program");
        CHECK(prog != NULL, "engine: OOM making the body of a cross-agent operation's program");
        /* No element: this is the engine's own text, so the peer document's §3.1.7 `currentScript` stays null. */
        engine_queue_into(f, doc, prog, DYN_CROSS_AGENT_OP, SCRIPT_TYPE_CLASSIC,
                          NULL, own, TASK_SOURCE_NOT_A_TASK, DYN_POS_APPEND, NULL,
                          /*parser_inserted*/0);   /* a peer's operation, and no element */
        dyn_body_unref(prog);
    }
    remote_op_free(op);
    JS_FreeCString(ctx, record);
    JS_FreeCString(ctx, token);
    JS_FreeValue(ctx, rv);
    JS_FreeValue(ctx, tv);
    JS_FreeValue(ctx, e);
}

/* A host instrument's program has completed, and its value is the product: dumped as JSON with every unknown on its
   own example, recorded as `<world><TAB><json>`. The world is part of the answer: the program ran in every timeline,
   so the host gets N answers, and unnamed they would be interchangeable claims about one document.
   The name is `world_name`'s, not `world_serialize`'s (solver/world.h: "one names a world to a PEER and records that
   it did, the other names it to a READER and records nothing"). `world_serialize` reaches `world_ancestry`, which
   sets `sent` ("a peer MAY hold a segment keyed by this name"), and a world marked sent by being printed would join
   the ancestry of every vector minted below it and be announced dead to peers that never held it
   (`world_flow_gone`, `world_session_gone`). A driver needs no vector: testing/render_engine.mjs splits on the first
   tab and uses the field as an opaque key. The two share one field writer, so both name one timeline.
   The JSON has no tab or newline by construction (value_dump.h; json_buf.c escapes), so it is the last field; it is
   asserted because a separator inside a field passes a field count. The realm is the program's: Web IDL §3.7
   Interfaces gives every realm its own intrinsics. */
static void flow_emit_dump(JSContext *ctx, Flow *f, JSValueConst cv)
{
    char *world = world_name(f->world);   /* @WORLDSHOT: a reader's name (see above) */
    char *json;
    size_t wn, jn;

    json = value_dump_json(doc_realm(flow_dyn_doc(f)), cv);
    CHECK(json != NULL, "engine: a host instrument's completion value could not be dumped — value_dump_json "
                        "aborts rather than answering nothing, so a null here is that contract broken");
    DCHECK(strchr(json, '\n') == NULL && strchr(json, '\t') == NULL,
           "a dumped completion value carries a record separator — every string it holds goes through "
           "json_buf_str, which escapes every C0 byte, so a raw tab or newline in it means the dump wrote "
           "bytes some other way and the host would read this record's tail as a second one under no world");
    wn = strlen(world);
    jn = strlen(json);
    if (g_dumps_n + wn + jn + 3 > g_dumps_cap) {
        size_t cap = g_dumps_cap ? g_dumps_cap * 2 : 1024;
        char *g;
        while (cap < g_dumps_n + wn + jn + 3) cap *= 2;
        g = realloc(g_dumps, cap);
        CHECK(g != NULL, "engine: OOM recording a host instrument's dump — the artifact this flow just "
                         "computed is dropped at the one line that would have handed it over, and the driver "
                         "reads a timeline that answered nothing from one that was never asked");
        g_dumps = g;
        g_dumps_cap = cap;
    }
    memcpy(g_dumps + g_dumps_n, world, wn);
    g_dumps_n += wn;
    g_dumps[g_dumps_n++] = '\t';
    memcpy(g_dumps + g_dumps_n, json, jn);
    g_dumps_n += jn;
    g_dumps[g_dumps_n++] = '\n';
    g_dumps[g_dumps_n] = 0;
    free(json);
    free(world);   /* world_name allocates; see solver/world.h */
}

/* The answer record, written in one place. Its two callers (the program's completion in flow_answer_perform, and
   flow_perform's record-only answer for a §7.2.1 member) write one grammar: field layout, separator and order, as
   core/frame/remote_op.h argues for the question. `ectx` is the realm the value is converted in and `ctx` only the
   notice's: Web IDL §3.7 Interfaces gives every realm its own intrinsics ("for every interface that is exposed in a
   given realm … a corresponding property exists on the realm's global object"), while engine_host_notify ignores
   its ctx and appends to this instance's buffer. The answer names the timeline that computed it, since the
   operation ran in every timeline and N completions arrive under one token; the name is world_serialize's, the
   one spelling of a world on the wire (solver/world.h), so the asker can record it beside the answer and refuse a
   second delivery of it. */
static void perform_answer_notify(JSContext *ctx, JSContext *ectx, Flow *f, const char *token,
                                  int completion, JSValueConst cv)
{
    char world[1024];
    char *enc, *rec;
    size_t cap;

    DCHECK(token != NULL && *token,
           "a cross-agent operation's answer was written under no rendezvous token — the completion names no "
           "question, so the flow that asked would park on it forever");
    enc = remote_completion_encode(ectx, completion, cv);
    /* world_serialize crashes on truncation rather than sending a prefix, so this notice carries the same name as
       every other record of this world. */
    world_serialize(f->world, world, sizeof world);
    cap = strlen(token) + strlen(world) + strlen(enc) + 24;
    rec = malloc(cap);
    CHECK(rec != NULL, "engine: OOM writing a cross-agent operation's answer — a dropped answer parks the "
                       "asking flow on a question nothing will answer again");
    /* The world sits before the completion because the completion is the remainder: a value grammar may contain a
       tab, a world vector may not (its fields are ':' and ','). */
    snprintf(rec, cap, "remoteop.answer\t%s\t%s\t%s", token, world, enc);
    engine_host_notify(ctx, rec);
    free(rec);
    free(enc);
}

/* The completion, read where the scheduler reads one: `cv` is the program's value, or JS_EXCEPTION with the throw
   pending. A throw is the answer too: it is encoded with its type and raised in the asking flow at the line that
   asked, where the page's own try/catch is; reporting it as this document's page error would answer `undefined`.
   It crosses as a one-way emission, so nothing has to un-send it when this flow parks or is outranked. */
static void flow_answer_perform(JSContext *ctx, Flow *f, JSValueConst cv)
{
    JSValue thrown = JS_UNDEFINED;
    int completion = ENGINE_COMPLETION_NORMAL;
    /* The row the flow stands on is the question it answers (the cursor is the row index). No bounds guard beyond
       the assert: the caller read this row's kind to get here. */
    int row = f->script_i;
    char *token;

    DCHECK(row >= 0 && row < f->dyn_n,
           "a cross-agent operation was answered from a cursor that is not on the queue — the kind that "
           "selected this call is read from that same row, so the two cursors have come apart");
    token = f->dyn_token[row];
    DCHECK(token != NULL,
           "a cross-agent operation's program completed with no rendezvous token on its row — the completion "
           "names no question, so the flow that asked would park on it forever. Two operations differing only "
           "in the asking WORLD are two rows and nothing else tells them apart, so this cannot be recovered "
           "from the flow");
    if (JS_IsException(cv)) {
        thrown = JS_GetException(ctx);
        completion = ENGINE_COMPLETION_THROW;
        cv = thrown;
    }
    /* Encoded in the realm the program ran in, because the value is that realm's (Web IDL §3.7 Interfaces); the
       same realm it was compiled in, asked the same way. */
    perform_answer_notify(ctx, doc_realm(flow_dyn_doc(f)), f, token, completion, cv);
    JS_FreeValue(ctx, thrown);
    /* The row stops owing, not the flow: the flow's other rows are other questions with their own tokens. The kind
       stays DYN_CROSS_AGENT_OP (it is what the program is, and the cursor advances past it), so from here an
       answered row asserts that nothing asks it for a token again. */
    free(f->dyn_token[row]); f->dyn_token[row] = NULL;
}

/* Preempt hook: two orthogonal yield decisions at the one per-opcode suspend point.
   (1) Value yield: suspend the running flow the moment a parked flow outranks it (the WFQ, not a clock, decides).
       The rival is found by an O(flows) scan only when the frontier generation or the running flow changes; its
       weight is recomputed at every consultation.
   (2) Cooperative-quantum yield: a thread-sharing floor, so even a top-ranked flow breathes after a slice and the
       host loop can interleave, pump and snapshot. Not a step cap: no flow is dropped or reordered, and the flow
       resumes byte-identically. The expiry is asked of solver/quantum.h, which owns both the asynchronous edge
       that raises the request (a per-thread CPU timer natively) and the measure it is decided on: CPU consumed,
       since a descheduled thread denies nobody anything, and an asynchronous source, since straight-line call-free
       code raises no request of its own. */
static int64_t engine_now_ms(void);   /* the wall clock, for the gap census below: reported, never a verdict */
/* The rival is cached; the comparison is not. What is cached is the rival's identity, found by the O(flows) scan,
   and the scan reruns only when the generation or running flow changes. Both weights are recomputed at every
   consultation, because a parked flow's weight moves without a generation bump: `flow_silence_notch` reads the
   flow's own silence plus its family's `fam_us`, which `flow_age_running` writes after every step, and on a real
   page the whole frontier is one family (every flow descends from the boot flow). The notch is a floor of
   `(cpu_i + fam_us) / S`, so a step can carry the rival across a boundary while the running flow's stands still; a
   cached weight would then yield to a flow no longer better. A fitness write cannot cause that: it is made only on
   the running flow, through flow_observe_survival or flow_observe_rung, which both raise the generation (a fire
   with the generation equal to the ranked one is a notch straddle, not a distance write). The cached pointer is
   safe across the key: a flow leaving the frontier changes membership and raises the generation, asserted where
   the pointer is taken. */
static unsigned g_seen_gen = 0; static Flow *g_seen_cur = NULL; static Flow *g_rival = NULL;
/* Which half of that key moved each time (solver/engine.h explains the reading and why the third row is not a
   rounding row). Raised in the rescan branch after the key compare and before the walk, so they partition
   `scanRivalRuns` by construction. Lifetime counts, raised in every build; only the census reads them. */
static uint64_t g_rival_miss_gen = 0, g_rival_miss_cur = 0, g_rival_miss_both = 0;
/* What the rival was worth in every term the frontier generation is supposed to speak for, recorded where the
   pointer is taken and checked at every consultation until the generation moves. The mirror of `g_ranked_*`
   below: that block holds the incumbent, whose terms may move while it runs; this holds a member that is not
   running and must not move except through the one quantity that legitimately does (flow_silence_phase: the
   aging's carry bit, whose threshold is the family's). If a reward, completed-unit count, fitness distance or own
   service notch could move with the generation still, every sub-linear order over the frontier (flow_pick walks
   it today because flow_weight's summands move under a key) would already be wrong; this catches it cheaply on a
   member the hook holds anyway, and also checks the pointer's safety argument for more than membership. Written
   in every build and read only in dev, like `g_ranked_*`: four stores after an O(members) walk, and guarding them
   would put the assertion's operands behind a second condition. */
static int64_t g_rival_own_notch = 0, g_rival_visits = 0;
static double g_rival_val = 0.0, g_rival_dist = 0.0;
/* The fifth summand: flow_weight adds flow_branch_bonus, `1.0 / sub_born` over the member's top-level arm.
   `sub_born` is a bucket field, so one sibling forking re-ranks the whole arm at once. The integer is snapshotted,
   not the quotient (solver/flow.h): the term is a pure function of it and the comparison is exact. */
static long g_rival_branch_born = 0;
/* The weight itself, the one clause that survives a summand being added: the five above are fields of a computed
   value, and an assert over the accessor cannot be short a summand (the `g_ranked_*` block below was short one
   twice, found by the formula moving). It is one-sided by design: between two generations a member's own half
   stands still and the aging notch `(own + fam) / FLOW_SERVICE_US` only grows (flow_age_running only adds), so the
   weight is non-increasing; equality would fire on the one movement the interval permits. It fires on the state
   the five cannot see: a forgiveness of the family's silence with the generation unchanged (flow_credit_emit is
   that writer and bumps today), a summand that moves up, or a notch that decreases. The direction is the
   precondition for any cached maximum, heap or index over flow_index_key; flow.h's `sil_phases` prices the
   magnitude. Under today's term list it is entailed by the five, so its silence is not a measurement; what it
   buys is coverage when the list changes. Written in every build, read only in dev (in release the condition is
   inside a `sizeof`).
   Named residual. Not covered: every member but the cached rival. solver/flow.c's walk asserts each member's own
   half equal across a generation, but nothing asserts the direction of the whole weight for the rest. Next diff
   builds: a per-member weight stamp beside `key_last`, compared with `<=` at that walk, dev-only. Absence shows
   as: a member whose weight rises between two generations passes every check unless it is the cached rival, and
   an index over the frontier returns a member the walk would not have, with no assert naming why. */
static double g_rival_w = 0.0;
/* What the flow holding the thread was ranked on when it took it: the quantities the value yield's verdict is a
   pure function of, recorded at switch-in and read by the assertion in the hook's value clause. Not policy and not
   a cache (the hook answers the same without them); they make "a top-ranked flow runs on at about zero switch
   cost" a check. Anything summed into flow_weight belongs here, or the assertion reads a real rank change as an
   unchanged one. */
static unsigned g_ranked_gen = 0; static double g_ranked_val = 0.0;
/* The aging term's notch: this flow's silence (its own thread time since its last emission plus its family's since
   any arm emitted) in whole cooperative quanta (solver/flow.h's flow_silence_notch). One snapshot, because the
   weight reads one quantity, the sum. The unit is the quantum the thread is handed out in, so the rank can move on
   aging between two picks (see the charge's resolution assertion in engine_sched_slice). */
static int64_t g_ranked_silence = 0;
/* The completed-unit count, the optimism term's quantity (flow.h's `visits`). It moves when the scheduler credits a
   step that left the flow between units, and a flow can be re-picked without a switch-in, so it can change while
   the silence notch stands still: a flow finished a program, its bonus halved, and a sibling passed it. */
static int64_t g_ranked_visits = 0;
/* The fitness distance, a term of the weight (`reward + dist + ucb - silence`) and so a coordinate of it. */
static double g_ranked_dist = 0.0;
/* Suspend points reached: every call to this hook is one, which the seam assertion needs and quickjs's counters do
   not give (g_flow_preempt_requested counts preempts wanted, raised only where the hook returns true). A lifetime
   count raised in every build: the seam assertion reads its per-step delta, and the @WFQ census publishes it as
   `preemptAsksLifetime` (solver/result.c) beside `scanRivalRuns`, which solver/flow.c raises unconditionally, so a
   dev-only raise would print 0 in release. flow_rival_of's one caller, the rescan branch, runs after this is
   raised, so `scanRivalRuns <= preemptAsksLifetime` always holds and the quotient is the hook's cache miss rate:
   what fraction of consultations buy a frontier walk (near 1 the cache absorbs nothing; near 0 the rival scan
   count says how often the generation moves). That is distinct from `scanRivalRuns / scanNextRuns` (what a step
   pays) and `scanRivalRuns / rankChanges` (invalidations per generation). */
static uint64_t g_preempt_asked = 0;
/* The gap between suspend points, the quantity the contract is about, which is not how long a step ran: a step
   offering a point every few milliseconds for ten seconds is behaving (the scheduler declined, losslessly); five
   seconds between two consecutive offers is the violation. Reset when the step starts, updated at each
   consultation, closed with the tail after the last. */
static int64_t g_last_ask = 0, g_max_gap = 0;
/* The same two in the slice's own measure, the pair a verdict could decide on (solver/engine.h's
   `over_gap_arms`). The wall pair is dev-only and reports a seamless turn's whole duration; these are raised in
   every build and report the widest interval between two consecutive offers of a turn that offered some, in
   `quantum_thread_us()` microseconds, comparable with ENGINE_QUANTUM_MS. Declared here because preempt_hook writes
   them; their lifetime scalar is declared with `g_over_asks`, where it is read. They cost one clock read per
   consultation: on the generic and emscripten branch quantum_expired() already reads that clock, so it is a second
   read; on the native branch quantum_expired() reads a `volatile sig_atomic_t` and the read is new. The wall clock
   stays under the guard because nothing publishes it and emscripten answers clock_gettime from JS; this pair is
   published (`sliceOverrunGapUs`, `stepUnitOverrunGapArms`). */
static int64_t g_gap_last_us = 0, g_gap_max_us = 0;

/* The one of the three a census reads, exported for solver/result.c (published beside `scanRivalRuns`, so no second
   definition of "how often was the policy asked"). The two clocks stay private: nothing outside the seam message
   may read a wall quantity from this file. */
uint64_t engine_preempt_asks(void) { return g_preempt_asked; }

/* The partition of this hook's misses, in one call (solver/engine.h). */
EngineRivalMiss engine_rival_miss(void) {
    EngineRivalMiss m;
    m.gen = g_rival_miss_gen; m.cur = g_rival_miss_cur; m.both = g_rival_miss_both;
    return m;
}

/* The policy does not care which kind of point it was offered: both decisions ask whether this flow should still
   hold the thread. */
static int preempt_hook(int kind) {
    (void)kind;
    Flow *cur = flow_running();
    /* The policy is asked only by the scheduler, from this one point. Both decisions are about the flow the
       scheduler is running, and between two engine_sched_step calls there is none: the thread is the host's, and
       quantum_end disarms the edge over it. An entry reaching this hook on the host's time would be asking whether
       to park a flow nobody drives. It is asserted here because clauses (0) and (1) can return before the budget
       (and quantum_expired's own precondition) is reached. A hit is a new host-time entry into the interpreter:
       give it a driver that can act on the answer, or stop it consulting the policy; never widen the slice. */
    DCHECK(quantum_slice_open(),
           "the scheduler's preempt policy was consulted with NO SLICE OPEN — whoever is running this code is "
           "not the scheduler, so there is no flow whose rank or budget this answer could be about; some entry "
           "reached the interpreter on the HOST's own time between two steps");
    /* This consultation, counted (`preemptAsksLifetime`), before the rescan below so the containment its quotient
       rests on is an order of statements. */
    g_preempt_asked++;
    /* The interval since the last offer, in the slice's own measure (solver/engine.h's `over_gap_arms`), raised
       beside the count and above every clause, so an offer answered by clause (1) or the value yield still counts
       and both numbers bracket the same population. The max is taken before the reading is stored, so it is the
       interval between consecutive offers; `g_gap_last_us` is opened at the turn's bracket, so the first interval
       is the stretch before the first offer. */
    {
        int64_t gap_now = quantum_thread_us();
        if (gap_now - g_gap_last_us > g_gap_max_us) g_gap_max_us = gap_now - g_gap_last_us;
        g_gap_last_us = gap_now;
    }
    /* The wall gap census belongs to the seam message and is compiled out with it; the count and the
       slice-measure gap above are not, because solver/result.c publishes them in every build. `engine_now_ms()`
       per consultation feeds only a developer's message, and on the host that matters most emscripten answers
       clock_gettime from JS, so it stays under the guard. The two gap censuses measure two populations: the wall
       one reports a seamless turn's whole duration beside the work count (a wall number says whether the box was
       loaded); the other the widest interval between offers of a turn that made some. In release the count costs
       one increment, in a hook already performing two flow_weight calls. */
#if APICLIENT_DEV
    {
        int64_t now = engine_now_ms();
        if (now - g_last_ask > g_max_gap) g_max_gap = now - g_last_ask;
        g_last_ask = now;
    }
#endif
    /* The rival is one the thread can be handed to: a flow that told the scheduler it cannot progress until the
       host answers is not a candidate, since yielding to it hands the thread straight back and the two would
       thrash. It is the same question the pick asks (flow.h), so it is the same call. The cache cannot lag a
       mark: a mark change, laid or cleared (a clear happens at whatever host event answers it, even mid-step, e.g.
       flow_deliver_one_reply's shared document-script slot), raises the frontier generation (flow.c), so the
       rescan below covers it and the hook ranks against the set the pick used.
       The key is a disjunction whose halves are read apart (solver/engine.h's EngineRivalMiss), captured before
       the assignments below overwrite them. */
    const int rival_gen_moved = (flow_frontier_gen() != g_seen_gen);
    const int rival_cur_moved = (cur != g_seen_cur);
    if (rival_gen_moved || rival_cur_moved) {   /* (1) rescan for the rival only on change */
        g_seen_gen = flow_frontier_gen(); g_seen_cur = cur;
        /* Under the walk's own `cur` test, so these partition `scanRivalRuns`, not consultations: a miss with no
           incumbent calls no flow_rival_of. The identity is asserted at the census (solver/result.c). */
        if (cur) {
            if (rival_gen_moved && rival_cur_moved) g_rival_miss_both++;
            else if (rival_gen_moved)               g_rival_miss_gen++;
            else                                    g_rival_miss_cur++;
        }
        g_rival = cur ? flow_rival_of(cur) : NULL;
        /* The cached pointer's safety argument, asserted where it is established: it stays live only because a
           departure raises the generation. Not at the read, which is per opcode, where an O(flows) membership scan
           would make the dev hot path linear in the frontier. */
        DCHECK(g_rival == NULL || flow_is_member(g_rival),
               "the value yield cached a rival that is not in the frontier — the pointer is read until the "
               "generation next moves, so a departure that did not raise the generation leaves this hook "
               "ranking against freed memory");
        /* The rival's own terms, frozen with the pointer after the membership assert, since they dereference it.
           The own service notch, not the silence notch: the silence adds the family's half, which advances on
           every step of whoever holds the thread, the one movement this interval permits. */
        if (g_rival) {
            g_rival_own_notch = flow_service_notch(g_rival); g_rival_visits = g_rival->visits;
            g_rival_val = flow_reward(g_rival); g_rival_dist = flow_distance(g_rival);
            g_rival_branch_born = flow_branch_born(g_rival);
            g_rival_w = flow_weight(g_rival);
        }
    }
    /* (0) Blocked beats both rankings: a flow holding an unanswered synchronous host request cannot progress, and
       the answer cannot arrive while it holds the thread (the host is asked only between steps). */
    if (cur && flow_blocked(cur)) return 1;
    /* Both sides are evaluated here, which makes this the question the pick answers: the cache holds which flow
       the rival is, and this computes what both are worth now.
       Nothing but the aging moved under the cached rival, asserted here, where the interval has elapsed: neither
       the generation nor `cur` changed, so the rival is the member the readings were taken of, and each reading
       is a flow_weight term the generation claims to speak for. `flow_service_notch` is the member's own silence,
       charged by flow_age_running only to `g_running`. The family notch and carry bit, the aging's shared half
       (flow.c's flow_silence_phase), are deliberately absent. Dev-only and O(1). The branch bucket's mint count is
       the fifth clause (`sub_born`'s two writers are flow_new and flow_fork_inherit, and the raise in
       flow_fork_inherit follows flow_new's bump with no opcode between); it guards a bucket minted into without
       flow_new, which would re-rank an arm silently.
       The message names which term moved, and whether the rival is still a member: a departure the cache outlived
       reads as a moved term (a freed flow answers 0 through the early returns that ask `family`), and the two
       take opposite diffs. flow_is_member is free on the passing path, since DCHECKF evaluates its arguments only
       when the condition failed. Every writer in solver/flow.c of these terms raises the rank change in the same C
       call, except flow_credit_visit's completed-unit count, whose sole caller credits `cur`, which forces the
       rescan by its second half. */
    DCHECKF(!g_rival ||
            (flow_service_notch(g_rival) == g_rival_own_notch && g_rival->visits == g_rival_visits &&
             flow_reward(g_rival) == g_rival_val && flow_distance(g_rival) == g_rival_dist &&
             flow_branch_born(g_rival) == g_rival_branch_born),
            "a term of the cached RIVAL's weight moved while the frontier generation stood still — cached "
            "against current: own service notch %lld -> %lld, completed units %lld -> %lld, reward %g -> %g, "
            "fitness distance %g -> %g, branch bucket mint count %ld -> %ld, and the rival is still a member "
            "of the frontier: %d. A mint count of -1 is this hook declining to ask a departed flow for its "
            "bucket, and a `still a member` of 0 is a DEPARTURE this cache outlived rather than a writer that "
            "moved a term — the two take opposite diffs. None of these may change without a rank change being "
            "raised. Two things are now false at once: this hook is ranking against a rival the scheduler's "
            "pick would no longer have returned, and the ONE-BIT claim every sub-linear spelling of the order "
            "rests on — that between two generations nothing per-member moves but the aging's carry — is not "
            "true of this tree. Raise frontier_rank_changed() at the writer the pair above names",
            (long long)g_rival_own_notch, (long long)flow_service_notch(g_rival),
            (long long)g_rival_visits, (long long)g_rival->visits,
            g_rival_val, flow_reward(g_rival),
            g_rival_dist, flow_distance(g_rival),
            g_rival_branch_born, flow_is_member(g_rival) ? flow_branch_born(g_rival) : -1L,
            flow_is_member(g_rival));
    /* The same claim over the whole weight, after the five so that a fire here is a movement the five have no
       name for: five matching terms and a weight that rose, which only the omitted family half can do. See the
       declaration for the state and the one-sided bound. */
    DCHECKF(!g_rival || flow_weight(g_rival) <= g_rival_w,
            "the cached RIVAL's weight ROSE while the frontier generation stood still — cached %.17g against "
            "current %.17g. Between two generations a non-running member's member half is fixed (the five "
            "clauses above, and solver/flow.c's walk over every member) and its aging notch is a quotient of "
            "two operands flow_age_running only ADDS to, so the weight may FALL and may not rise. The five "
            "clauses above read this member's OWN silence and deliberately not its FAMILY's, so the movement "
            "they cannot see and this one can is a forgiveness of the family's silence with no rank change "
            "raised behind it — flow_credit_emit sends both operands to zero for a whole family in one "
            "statement and is listed at that walk among the writers that bump, so a fire here is that bump "
            "having been lost or a second forgiver having been added without one. The other two readings are a "
            "summand added to flow_weight that moves UP while the five named terms stand still, and a notch "
            "that can decrease. Every cached maximum, heap and index anybody builds over this frontier is "
            "derived from the direction this line asserts; solver/flow.h's `sil_phases` prices the MAGNITUDE "
            "and this is the SIGN. Raise frontier_rank_changed() at whichever writer sent the silence back",
            g_rival_w, flow_weight(g_rival));
    if (cur && g_rival && flow_weight(g_rival) > flow_weight(cur)) {   /* value yield */
        /* The value yield may fire only on a rank change: a flow picked to run, against which nothing has changed,
           must still be running ("a top-ranked flow runs on at ~zero switch cost"). What can change the answer is
           exactly what is snapshotted at switch-in: the frontier generation (a fork or finish, and an emission, bump
           it), the running flow's silence notch (its own and its family's thread time since emission, in whole
           cooperative quanta), its completed-unit count, its family's reward, and its fitness distance. A parked
           flow burns no CPU and its `val` cannot change without running. The list is re-derived whenever
           flow_weight's formula changes; a term left off makes this fire on a legitimate rank change.
           The eligible set is also an input (the comparison is best-eligible-other > cur), and host-owed marks
           decide membership of it; a mark change calls frontier_rank_changed() (flow.c), so it arrives through the
           generation, clause one, rather than as a separate clause that would silence this while the hook ranked
           against a set the pick never used. What this catches with no published change: a tie handed on by the
           pick's registry order, a rival recomputed against a stale cache, a weight term moving with something not
           listed; each is a COW delta swap bought with nothing.
           The reward clause reads flow_reward, the family's account flow_weight uses, not `cur->val`, one member's
           ledger, which a sibling's emission does not move. */
        DCHECK(flow_frontier_gen() != g_ranked_gen ||
               flow_silence_notch(cur) != g_ranked_silence ||
               cur->visits != g_ranked_visits || flow_reward(cur) != g_ranked_val ||
               flow_distance(cur) != g_ranked_dist,
               "the VALUE YIELD fired on a flow whose rank nothing changed since the scheduler switched it in — "
               "same frontier generation, same silence notch, same completed-unit count, same reward and same "
               "fitness distance on both "
               "sides of the comparison, so the pick and the hook are answering one unchanged state two "
               "different ways and every swap this buys is a COW delta swap for no ranking decision at all");
        return 1;
    }
    /* (2) Cooperative-quantum floor: thread-sharing, not value. The same expiry the scheduler loop returns to the
       host on, so a flow parking on it and a step returning on it are one event seen twice. Nothing is dropped,
       starved or reordered: the same flow resumes byte-identically unless the WFQ says otherwise. */
    return quantum_expired();
}

/* May the running flow go on to the next sub-step of the event-loop turn it is in? The scheduler's granularity
 * input: one call of the policy above, not a second policy or a constant.
 *
 * The unit is the spec's: HTML §8.1.7.3 Processing model step 2's nested list runs 2.1 to 2.8, where 2.6 is
 * "Perform oldestTask's steps" and 2.8 is "Perform a microtask checkpoint", itself a loop "While the event loop's
 * microtask queue is not empty". So one iteration of step 2 is one task plus the checkpoint after it, and a pick
 * that reaches the delivery arm carries the whole turn rather than resting at every rung.
 *
 * This changes the drain rate, not the size of the debt: the backlog is Θ(records × registers naming them), because
 * a fork copies a flow's undelivered backlog (engine_sibling_assemble's `pending_fork` shares the record and copies
 * the naming, solver/pending.h) and §8.1.4.4 Calling scripts' empty-execution-context-stack guard (flow_stack_empty)
 * forbids delivering while a program runs. That counts namings over every live register; the networking-first
 * arm's bound is the length of one register, which a fork duplicates rather than lengthens, so neither bounds or
 * refutes the other.
 *
 * A granularity, not a bound: every declined offer costs one predicate, every rung is re-derived from the flow's
 * state at each entry, and a step ending here leaves the same state as one ending at a return. A policy refusing
 * every continuation reproduces the per-rung engine exactly. It is preempt_hook's question asked from the other
 * level, so the flow yields when outranked (clause 1), when its quantum is spent (clause 2) and when blocked on the
 * host (clause 0). JS_PREEMPT_HOST is the right kind ("no bytecode shape behind it at all"); the seam census counts
 * it as an offered suspend point. */
static int turn_continues(void) {
    return !preempt_hook(JS_PREEMPT_HOST);
}

/* Advance flow `f` by up to one quantum. Returns 1 when the flow has finished all its scripts and lazy chunks, 0
   when it yielded mid-execution (resume it later). Each <script>/chunk is its own program (JS_FlowNew), run in
   document order in the shared context under f's COW delta (set by the caller). */
/* Async-as-flow job-enqueue hook (installed as JS_SetJobEnqueueHook): routes a promise reaction or microtask to the
   enqueuing flow's own queue instead of the global list, so it runs later under that flow's live COW delta. */
/* How many jobs this document queued and ran, published with the other cost numbers: a run whose reactions never
   fire and a page that queued none look the same from outside. */
static long g_jobs_q, g_jobs_run;
long engine_jobs_queued(void) { return g_jobs_q; }
long engine_jobs_run(void) { return g_jobs_run; }
/* Named residual. Not covered: neither total is split by the queue's kind. The run side is split in effect (a
   `run-a-task` step runs exactly one task, a `microtask-checkpoint` step only microtasks, since job_pick_index takes
   the first non-task entry and the checkpoint arm is due only while one exists), so `jobsRun` minus `run-a-task` is
   the microtask count; the queued side has no split although `is_task` is in hand where `g_jobs_q` is raised. Next
   diff builds: `jobsQueuedTask`/`jobsQueuedMicro` beside `_jobsQueued` on the work line, their sum asserted equal.
   Absence shows as: a document's `run-a-task` count that cannot be read as most or a tiny fraction of the tasks the
   page queued. */
/* Completed units of work, the precondition for running a job: every job arm of flow_step is under `frame == NULL`
   (HTML §8.1.4.4 Calling scripts' clean up after running script step 3, "if the JavaScript execution context stack
   is now empty"), so jobs queued and none run reads two ways: flows reach that boundary and hold nothing there, or no
   flow ever reaches it (no program finished). This separates them. It is the quantity the optimism term counts
   (flow_credit_visit's completed unit), counted once per instance at that credit's own site; flow_credit_visit
   asserts the rest of §8.1.4.4's boundary. A boundary reached with a checkpoint still owed is not one: the turn is
   not over, and the next step runs the job (`jobsRun` reads it). */
static long g_units_done;
long engine_units_done(void) { return g_units_done; }

void engine_orphan_claims(long *met, long *unmet) {
    DCHECK(met != NULL && unmet != NULL,
           "the orphan round trip was asked for one of its two numbers — the pair is the statement, because a "
           "session that satisfied many waits and lost one has a nonzero count on both");
    *met = g_orphan_claims_met;
    *unmet = g_orphan_claims_unmet;
}

/* The `jobs` carrier's §8.1.7.1 declaration, so whether one source sits in one queue can be asked of a whole flow:
   engine_queue_into asserts the field for `dyn` and stores nothing; this asserts it for `jobs`. It is not stored on
   the flow's job record, which has no reader for it, but is on the runtime's entry, because `baseline_call_list`
   hands a task to this hook later. The sentinel is the one value the two type systems share (quickjs.h's opaque
   JSTaskSource and this file's enum), so it is checked statically. */
_Static_assert((int)TASK_SOURCE_UNSTATED == JS_TASK_SOURCE_UNSTATED,
               "the fork's never-stated task source and this engine's differ — quickjs carries the scalar "
               "without interpreting it and states only the zero, so a disagreement here makes every microtask "
               "arrive naming whatever this enum puts first");

static int engine_enqueue_job(JSContext *ctx, JSJobFunc *fn, int argc, JSValueConst *argv, bool is_task,
                              JSTaskSource raw_src, JSTaskHandle handle) {
    /* The hook's signature is the fork's: quickjs carries an opaque scalar (quickjs.h's JSTaskSource) and this
       engine's enum names the values, and declaring the hook with the enum would be a different function pointer
       type (an enum's underlying type is the compiler's choice), so the conversion is here. */
    TaskSource src = (TaskSource)raw_src;
    /* The owner is named when the user agent is the queuer (g_enqueue_owner, beside the one bracket that sets
       it); otherwise the callback belongs to the flow whose program queued it. */
    Flow *f = g_enqueue_owner;
    g_jobs_q++;
    if (!f)
        f = flow_running();
    else
        /* Host time is "no slice open", not "no flow running": engine_sched_step returns the quantum yield without
           switching the running flow out, so it can resume byte-identically, and the stamp stays up across the
           host's time by design (a yielded flow's COW delta is still applied). The named owner wins by precedence,
           so the queue reached is the named flow's whatever `flow_running()` says. What would break this is a flow
           executing while a host-time bracket names an owner, two queuers for one callback, which is the slice
           being open: the predicate preempt_hook asks and qjs_step asserts on every exit. */
        DCHECK(!quantum_slice_open(),
               "a host-time bracket named the owner of a queued callback while a scheduler SLICE was open — an "
               "owner is named only between two steps, so an open slice means a flow is EXECUTING and two "
               "queuers claim one callback: the named one wins and the running flow's own reaction is filed "
               "under a timeline that never caused it");
    /* There is no global drain: declining would hand the job to quickjs's global list, which nothing runs, so it
       would be dropped. Every task source comes through here (a window message, a port delivery, a broadcast, a
       timer callback, a custom element reaction), and a dropped one is a handler the page registered and this
       engine never entered. */
    DCHECK(f != NULL, "a job was enqueued with no flow running — there is no global drain, so it would be "
                      "dropped: seed it as a flow on the frontier instead of declining it here");
    if (!f) return 0;
    DCHECK(ctx != NULL, "a job was enqueued with no realm — §7.5.10 step 5 removes a destroyed document's tasks "
                        "by comparing this, so a job without one outlives its document");
    /* The handle arrives with the job and is recorded, never defaulted: it is the only way the host can find this
       callback again, and without it a tracker could not remove the task (a `toggle` that fires twice). Every
       runtime enqueue path allocates one (js_enqueue asserts it). */
    DCHECK(handle != JS_TASK_HANDLE_NONE,
           "a job reached the scheduler under the never-issued handle — quickjs mints one at every enqueue and "
           "carries it across the baseline handover, so a job without a name has come from a path that does "
           "not, and nothing could ever take it back off this flow's queue");
    /* Which task source put it here (HTML §8.1.7.1's `source`), asserted at the one site that takes ownership of a
       queued callback, as engine_queue_into asserts it where rows are made, so each carrier states its source.
       Two-sided: a producer that never stated one reaches a task queue with the sentinel and fires here; a source
       stated for a microtask is caught in js_enqueue, which knows which queue it uses. `task_source_is_task(src)` is
       deliberately not asserted: this engine queues as tasks several fires its standard states as synchronous
       steps of a running algorithm (core/events/event_target.h at `event_target_fire`), and each declares
       TASK_SOURCE_NOT_A_TASK, so that population is greppable at the JS_EnqueueCallTask call sites. */
    DCHECKF(!is_task || task_source_stated(src),
            "a callback was queued as a TASK without saying which task source, if any, put it there — HTML "
            "§8.1.7.1 gives a task a source so that one source is in one queue, and a job that states none "
            "cannot be ordered against the same source's work on this flow's other carrier or shown not to "
            "have any (is_task=%d)", (int)is_task);
    flow_job_push(ctx, f, fn, argc, argv, is_task, handle);
    return 1;   /* host owns it */
}

/* The third part of that ownership (installed as JS_SetJobRemoveHook): HTML §4.11.4 The dialog element's "Remove
   element's dialog toggle task tracker's task from its task queue" (§4.11.1 The details element and §6.12 The
   popover attribute hold the same tracker), for a job this scheduler took. JS_RemoveQueuedTask walks quickjs's
   own two queues and baseline list first. It is the running flow's queue, never a global search: after a fork N
   flows hold copies of one handle, each named by its own arm's tracker, and sweeping every flow would delete a
   sibling's task from a timeline where the removal never happened. Finding nothing is ordinary: the task already
   ran, is running (a `toggle` listener that closes the dialog again removes the task dispatching it), or went
   with its document. */
static int engine_remove_job(JSTaskHandle handle) {
    Flow *f = flow_running();

    DCHECK(f != NULL,
           "a queued task was removed by name with no flow running — a tracker's removal is made by state that "
           "only ever changes inside a flow, so a caller here is a platform edge reaching for a queue that "
           "belongs to a timeline it is not in");
    if (!f) return 0;   /* release path under the assert: nothing this hook can honestly reach */
    return flow_job_remove(f, handle);
}

/* The other half of engine_enqueue_job (installed as JS_SetJobDropHook): HTML §7.5.10 Destroying documents step 5
   for the jobs this scheduler took; without it a destroyed document's reactions would later run in a Document
   whose browsing context is null. The running flow's queue only, for JSJobRemoveHook's reason (quickjs.h): a
   destruction is a fact in the timeline that performed it (an `<iframe>` removed in one arm is still there in the
   other), and §7.5.9 step 6 puts every timeline's unload task on the outgoing document's global, so a sweep would
   let the first flow reaching step 20 take the others' tasks too. A destruction every timeline performs is fanned
   out by engine_unload_document, one per flow. */
static int engine_drop_jobs(JSContext *ctx) {
    Flow *f = flow_running();
    int dropped;

    DCHECK(f != NULL,
           "§7.5.10 step 5 removed a destroyed Document's queued tasks with no flow running — a destruction is "
           "performed by a timeline (§7.5.9's unload task is a job of one), so a caller here is a platform edge "
           "reaching for a queue that belongs to a timeline it is not in");
    if (!f) return 0;   /* release path under the assert: nothing this hook can honestly reach */
    /* Not while this walk holds the flow: the step's allocator refusal edge can page a member out, and the
       registry removes by swapping its last member into the hole, which could move the Flow this walk indexes.
       A refusal here is just a refusal; the next allocation is armed again. */
    int prev_reclaim = engine_reclaim_set(0);
    dropped = flow_job_drop_realm(ctx, f, ctx);
    engine_reclaim_set(prev_reclaim);
    return dropped;
}

/* Runs one of the flow's queued jobs under its applied COW delta, then frees its result. The pick is HTML §8.1.7's
   microtask checkpoint, not a FIFO pop: arrival order within a queue, but no task may begin while this flow holds
   a microtask, so `setTimeout(f, 0)` cannot run in the middle of a promise chain. */
static void flow_run_one_job(JSContext *ctx, Flow *f) {
    JSValue job = flow_job_take(ctx, f);   /* the pick IS the checkpoint rule — see flow.h */
    g_jobs_run++;
    JSValue r = flow_job_run(ctx, job);    /* the reaction runs in this flow's timeline */
    /* A job that threw is a page error, as a script that threw is: a promise reaction, a queueMicrotask callback
       or a delivered message reports through §8.1.4.6 Runtime script errors' "report an exception", the same hook a
       script uses (core/events/report_exception.h). */
    if (JS_IsException(r)) {
        JSValue e = JS_GetException(ctx);
        result_page_error_value(ctx, e);
        JS_FreeValue(ctx, e);
    }
    JS_FreeValue(ctx, r);
    /* The record's reference, not its contents: forked arms name the same records, so the arguments go with the
       last arm's reference; freeing them here would empty a job a sibling has not run. */
    JS_FreeValue(ctx, job);
}

/* flow_stack_empty is solver/flow.h's: it is a question about a flow (its frame and its sequence's position
   column), and cold_census needs it too, since the delivery arm is guarded on it and a census must say whether a
   member can take its answered replies. One exported predicate, not a second spelling in cold.c. */

/* Is a microtask checkpoint due before this flow's next program? The stack is empty and a microtask is queued.
   §8.1.7.3 Processing model performs the checkpoint at the end of each task, so every later task of this flow
   comes after it, including the next program: `<script>Promise.resolve().then(() => fetch('/a'))</script>
   <script>fetch('/b')</script>` issues /a before /b. */
static int flow_checkpoint_due(const Flow *f) {
    if (!flow_job_microtask(f)) return 0;
    return flow_stack_empty(f);
}

/* One unit of work, then return: flow_step is a step, not a drain. Every branch that finishes something returns
   instead of looping on to the next program, chunk, listener or candidate, so a flow holding many short programs
   (none long enough to reach a back-edge preempt) cannot run them back to back without the scheduler interleaving,
   re-ranking or honouring its quantum. It costs only loop iterations (the switch counter moves only when a
   different flow is picked); every returning branch made progress first. */
/* Which unit: set by the branch about to run, so a step that turns out to have no suspend point names itself in
   the scheduler's assertion. */
static StepUnit g_step_unit = STEP_UNIT_NONE;

/* How many times each arm has run over this instance's life, the opposite question to `Flow.step_unit`.
   `Flow.step_unit` is where a member stands, so solver/cold.c's histogram over it is a gauge of the frontier at an
   instant (`run-a-task: 0` there: nobody stands in that arm now); this counts steps (`run-a-task: 0` here: the
   ladder never reached it). Those take opposite work: rungs above that never run out, or a member that reaches
   the rung and cannot get past it. A report, never a gate: nothing may branch on it (no fixpoint over an arm that
   stopped moving, no progress detector, no cap, no seen-set), because ordering by how often something happened is
   not ordering by value of information. Not reset at a session boundary, like `g_finished` and `g_switches`: the
   host wants the instance's total. */
static long g_step_unit_runs[STEP_UNIT_N];
/* The same list restricted to turns that overran the slice (solver/engine.h's `over_arms`): an arm whose two counts
   are equal is a step that cannot be preempted, not merely a hot one. */
static long g_step_unit_over[STEP_UNIT_N];
/* Its own sum, walked rather than carried beside the rows (a second copy of one fact). Pure; used by the asserts. */
static inline long step_unit_over_total(void)
{
    long t = 0;
    int i;

    for (i = 0; i < STEP_UNIT_N; i++) t += g_step_unit_over[i];
    return t;
}
/* The same list restricted further: overrunning turns that offered not one suspend point. `g_step_unit_over` says
   which arm overran and the scalar `g_over_seamless` how many overrunning turns were seamless, and the two cannot be
   joined (an arm at three quarters of all overruns beside a seamless count at three quarters may or may not be the
   same turns). The cases take opposite work (solver/engine.h's `over_arms`): a seamless stretch is a C activation
   declaring no step boundary, a step-machine conversion in its owning component; a stretch that offered points and
   ran anyway is the page choosing a back-edge-free stretch, which nothing may cap. Raised at the scalar's line from
   the same turn's arm and delta, so `sum == g_over_seamless` and containment in `g_step_unit_over` are asserted
   there. Decides and bounds nothing; raised in every build. */
static long g_step_unit_over_seamless[STEP_UNIT_N];
/* Its own sum, walked, for `step_unit_over_total`'s reason. */
static inline long step_unit_over_seamless_total(void)
{
    long t = 0;
    int i;

    for (i = 0; i < STEP_UNIT_N; i++) t += g_step_unit_over_seamless[i];
    return t;
}
/* The points the non-seamless overrunning turns of each arm offered (solver/engine.h's `over_ask_arms`, which says
   why the denominator is not the arm's overrun count). Raised in the `else` of the seamless histogram's branch,
   from the same delta. */
static long g_step_unit_over_asks[STEP_UNIT_N];
/* Its own sum, walked, for the same reason. */
static inline long step_unit_over_asks_total(void)
{
    long t = 0;
    int i;

    for (i = 0; i < STEP_UNIT_N; i++) t += g_step_unit_over_asks[i];
    return t;
}
/* The worst no-suspend-point stretch any non-seamless overrunning turn of each arm contained (solver/engine.h's
   `over_gap_arms`: the density beside it is a mean). Raised in the same `else` as the ask row. Not a partition: a
   sum of maxima is a quantity no turn produced, so its identity is a max against the scalar. */
static long g_step_unit_over_gap[STEP_UNIT_N];
/* Its own maximum, walked; a fold over an empty history answers 0, as every arm and the scalar read before the
   first such turn. */
static inline long step_unit_over_gap_max(void)
{
    long m = 0;
    int i;

    for (i = 0; i < STEP_UNIT_N; i++) if (g_step_unit_over_gap[i] > m) m = g_step_unit_over_gap[i];
    return m;
}
/* The steps themselves, counted where the histogram is not: incremented at flow_step's entry, while the histogram
   is incremented where the scheduler records the arm after flow_step returns. So `sum(g_step_unit_runs) == g_steps`
   says every entry reached the one point that records its arm, which a second call site, a step driven from
   elsewhere, or an early return between the two would break. */
static long g_steps;
/* How many of those steps reached the work ladder (solver/engine.h's `unframed_steps` says what it separates and
   why it is not `unframed_picks_lifetime`). Raised at the line entering flow_step's `if (!f->frame)` block, the
   ladder's precondition: a framed step asks none of the ladder's conditions, the orphan ask included. Per pass, not
   per entry (the turn continuation iterates the loop body), so it is not comparable with `g_steps`; the orphan ask
   is raised per pass in the same block, so their containment is exact. A report only. */
static long g_unframed_steps;
/* Which arm took the step of a member holding a runnable task, answering what `readyPicksLifetime`'s second reading
   hands to the ladder ("flow_step declines the job at an arm above the one that would run it"); `g_step_unit_runs`
   covers every step and cannot say the member held a task. Both raise sites stand below the checkpoint arm, whose
   guard is `flow_job_microtask && flow_stack_empty`, and `flow_stack_empty(f)` holds at both, so a non-empty queue
   there holds no microtask and `flow_job_pending(f) > 0` means a task is runnable (a DCHECK at each site).
     `g_task_held_deliv`: the reply-delivery arm took the step. The entry carries a stamp (PEND_WORK_SEQ) and the arm
       asks flow_task_precedes, so a delivery no longer precedes an older queued callback, but it still precedes
       every row (its arm records that as a deadlock it avoids). So this sizes the arm going ahead of a younger task.
       Comparing it across the stamp's introduction compares two quantities: `git grep -c PEND_WORK_SEQ <stamp> --
       engine/host` says which an artifact measures.
     `g_task_held_seq`: the program-sequence arm took it (`seq_compiles && !job_precedes`): the queued task is younger
       than the row at the cursor. The only record of that answer.
     `g_task_arm_older`: the task arm ran because the comparison said the task is older.
     `g_task_arm_no_row`: the task arm ran with no program to start (`seq_compiles` 0); a member past the end of its
       sequence is here at every step (`programCursors`' top bucket counts them).
   Identity, asserted at the accessor: `g_task_arm_older + g_task_arm_no_row == g_step_unit_runs[STEP_UNIT_RUN_TASK]`;
   the halves are raised at the choice and the total after flow_step returns, so a second writer or an early return
   breaks it. Not covered: the three arms above delivery (link-connected time, a routed delivery, a peer's parked
   operation), visible as `run-a-task` plus these four not accounting for unframed steps while `jobsReadyTask` stays
   above zero; the checkpoint arm (no task is runnable there; `jobsReadyMicro` sizes it); and queue depth
   (job_pick_index takes the oldest, so a late task waits behind earlier ones). Lifetime counts, a report only.
   Named residual. Not covered: rows are still not in the arrival chain above the delivery arm. Next diff builds:
   the oldest runnable row's stamp computed above the delivery arm, putting all three carriers in one arrival
   order, after which `g_task_held_deliv` counts nothing. Absence shows as: `g_task_held_deliv` above zero (today
   every use of `seq_compiles` is below the line `grep -n 'uint64_t deliv_seq = flow_stack_empty'` prints). */
static long g_task_held_deliv, g_task_held_seq, g_task_arm_older, g_task_arm_no_row;
/* Why a turn did not end a unit of work, the three readings behind a low `g_units_done`. The unit boundary in the
   dispatch loop is a conjunction of three clauses, and these count which refused:
     a live frame: the member is inside a program and the turn advanced it (`resume-program`'s state); a frontier
       of forks is framed by construction (engine_sibling_assemble copies the frame), so bulk refusal here is not an
       idle thread;
     a parked continuation: the member is suspended on an await or a host reply;
     a checkpoint still owed: §8.1.4.4 Calling scripts' clean up after running script step 3 checkpoints when the
       stack empties, and the unit ends when the queue does, so the flow's own reactions are eligible and unrun.
   `g_units_done` stays beside `flow_credit_visit`, whose credit is the optimism term's denominator (a completed
   unit), so its discriminators are published beside it rather than moving it. With it they partition `g_steps`
   exactly: the boundary is evaluated once per dispatch on a straight line between flow_step's return and the
   credit, and the identity is asserted at the credit (the document carries `_unitsDone` and `steps` in two
   different objects). Named outside `stepUnit*` and `_unitsDone`, so existing queries measure what they did. */
static long g_unit_mid_program;      /* …the member held a live frame: inside a program, the trial continues */
static long g_unit_parked;           /* …the runtime held a parked continuation: suspended on an await/reply */
static long g_unit_checkpoint_owed;  /* …the flow still owed its microtask checkpoint: the turn is not over */
/* What those steps cost in the slice's own measure (solver/engine.h's EngineStepUnitRuns `step_us`: read as a ratio
   against `g_steps`, never a total). Declared beside its denominator and its one adding line, so numerator and
   denominator live in one file. It is the same microseconds `flow_age_running` is charged, taken at the same line,
   not a second clock reading; the charge asserts the delta non-negative (flow.c), so this only climbs. A report
   only: no watchdog, cap or fixpoint may be built on it. `int64_t`, not `long`: on wasm32 `long` is four bytes, and
   a microsecond accumulator would overflow after 35.8 minutes and invert its reading. */
static int64_t g_step_us;
/* The split `step_us / steps` cannot make: when a turn is slice-bound, which half spent the time. A step
   overshooting the slice is the quantum lacking an asynchronous source to expire it (on single-threaded wasm nothing
   raises the yield bit mid-call); a pick and swap that dominate are the ordering and the delta costing more than the
   work. Two rows rather than a subtraction, so `slice + sched == step` is asserted where all three are read and a
   third phase added later breaks it loudly. `sched_us` is everything in the turn that is not the step: the charge
   telescopes (solver/engine.h), so it carries the previous iteration's tail (checkpoint, finish) with this one's
   pick and swap. One extra clock read per turn (`quantum_thread_us` crosses into the embedder on the shipped host).
   A report only. */
static int64_t g_slice_us, g_sched_us;
/* What all three are a share of (solver/engine.h's `instance_us`): `g_instance_us0` is the first reading of the
   slice's measure this instance took, so `quantum_thread_us() - g_instance_us0` is the thread measure since the
   dispatch loop first ran. A separate flag rather than a sentinel, because 0 is a legitimate thread-clock reading.
   Set once, from the reading engine_sched_step's wrapper takes at its first line (not engine_sched_slice's entry,
   one line later): `g_loop_us` is charged from the wrapper's own two readings, so a later baseline would break the
   partition assert on the first slice. Not reset at a session boundary, for `g_step_us`'s reason. */
static int64_t g_instance_us0;
static int     g_instance_us0_set;
/* The partition of that span (solver/engine.h's `loop_us`): `g_loop_us` is the measure spent inside
   engine_sched_step's bracket over every slice, `g_between_us` the measure between one slice's return and the next
   one's entry (the host's time); with the span since the last return they sum to `instance_us`, asserted at the
   accessor. Both are charged in the wrapper, not at engine_sched_slice's seven returns, for the reason
   engine_sched_step gives for `quantum_end()`: a charge per return is eventually missed. Two clock reads per
   slice. `g_slice_exit_us` carries the previous slice's closing reading; `g_slice_exit_set` is a separate flag, as
   above. `g_slices` is their denominator (`long`: a count, not a clock). A report only. */
static int64_t g_loop_us;
static int64_t g_between_us;
static int64_t g_slice_exit_us;
static int     g_slice_exit_set;
static long    g_slices;
/* The partition of the above (solver/engine.h's `slice_overruns`), counted from the same two readings the slice arm
   is accumulated from, so a turn cannot be charged to one and counted by the other. */
static int64_t g_slice_over;
/* Whether the overrunning turns offered a suspend point at all (solver/engine.h's `slice_overrun_asks`):
   `g_preempt_asked` bracketed across the step from the same `t_slice0`. In every build: one load per turn, and a
   release engine must be able to say whether an overrunning turn reached the interpreter. A report only. */
static uint64_t g_over_asks;       /* suspend points offered, summed over the turns that met the slice */
static long     g_over_seamless;   /* …and how many of those turns offered NOT ONE */
/* The longest stretch of one of those turns during which the policy was not consulted (solver/engine.h's
   `over_gap_arms`: the pair above is a density, which cannot tell nine spread stretches from one unbroken run).
   The lifetime maximum over non-seamless overrunning turns; its per-turn readings, `g_gap_last_us` and
   `g_gap_max_us`, are declared with their writer preempt_hook. The lead and tail count: the closing reading closes
   the stretch after the last offer, and the turn's opening reading the one before the first. A report only. */
static int64_t g_over_gap_us;      /* the worst such interval over every non-seamless overrunning turn */
/* The one phase inside a step whose cost is a length the page chose: the classic compile. `g_slice_over` and
   `g_step_unit_over` say a turn met the slice and in which arm, not which phase: a start step is a compile
   (JS_FlowNew -> JS_Eval over `body_n` bytes) then an execution (JS_FlowResume), and only the second runs bytecode.
   The parse rests: JS_FlowCompileStep polls the budget hook and preempt policy from the production dispatch and
   hands back through `f->compile`, so a stint that still meets the slice is one whose rest points were too far
   apart (the production dispatch's granularity). solver/engine.h's `over_arms` observation (`start-ended-its-frame`
   ran 153 times and overran never) is stratified by source length, since outcome correlates with it, so it speaks
   only for short compiles.
   Readings: `classicCompiles` near the programs reached with `classicCompileOverruns` 0 says the compile is not the
   cost; nonzero overruns name the parse's granularity; repeats are `classicCompileAgain`. A `classicCompiles` many
   times a document's `<script>` count is normal (forks parse later programs themselves, @S and cold replays
   re-parse, and appended rows such as chunks, injected scripts, `javascript:` URLs, operations and dumps count too),
   and no published row is its denominator (`rootPrograms` is narrower on both axes).
   The two rows count different events: `g_classic_compiles` once per program, at the stint that finishes its parse;
   `g_classic_compile_over` once per stint that met the slice, so `over <= compiles` is not an invariant. Every stint
   parks (naming STEP_UNIT_COMPILE_YIELDED) or finishes (raising `g_classic_compiles`), the step name has one site,
   and nothing releases the counters, so classic compile stints == g_classic_compiles +
   g_step_unit_runs[STEP_UNIT_COMPILE_YIELDED], and `g_classic_compile_over` <= that sum is asserted in
   engine_step_unit_runs. A module parse parks under its own row (`module-compile-handed-the-thread-back`) to keep
   this an equality. Two clock reads per compile. A report only.
   Named residual. Not covered: the parse's rest granularity is decided inside the parser, not by the scheduler
   (rest_unit.h declares no JS-production kind). Next diff builds: the parse as a pull whose granularity
   solver/rest_unit.h owns (rest_unit_items), after which an overrunning stint is an ordinary preempted span and these
   rows retire. Absence shows as: nonzero `classicCompileOverruns`. */
static long g_classic_compiles;       /* classic program compiles, ONE PER PROGRAM, at flow_step's start site */
static long g_classic_compile_over;   /* compile STINTS that met the slice — NOT a subset of the row above */
/* Whether a parse that handed the thread back was picked up again: compile stints that continued a parse rather than
   beginning one. No arm of the histogram recovers it, since a resuming stint is named like a first stint. A parse of
   k stints yields k-1 times if it ends (k while in flight) and resumes k-1 times, so
       arms[`compile-handed-the-thread-back`] - classic_compile_resumed
           == parses suspended now + parses dropped while suspended.
   Near zero: parses are carried forward, and a low program count is a budget. Large: parses are handed back and not
   resumed, work this scheduler holds and does not advance. A count, not a maximum: a high-water mark saturates and is
   not comparable across runs of different length. Both zero means the seam never fired. A report only. */
static long g_classic_compile_resumed; /* compile STINTS that CONTINUED a parse rather than beginning one */
/* The repeat, observed (solver/dyn_body.h: the identity is the body, not a hash of its text). Raised at the same
   event as `g_classic_compiles`, the next statements of one straight-line block, so `again` and `own_decode` each
   partition it (complements derived); they answer different questions about one parse and may not be added
   together. `again_bytes` is int64_t for `g_step_us`'s reason: one real bundle ships a 1.4 MB chunk, which would
   overflow a four-byte total after about fifteen hundred repeats. */
static long    g_classic_compile_again;        /* parses of bytes some flow had ALREADY parsed to completion */
static int64_t g_classic_compile_again_bytes;  /* and how many source bytes those re-parses covered */
static long    g_classic_compile_own_decode;   /* parses of a body one flow decoded for its own delivery */
/* Programs started from a parse another flow finished: without it a falling `g_classic_compiles` is unattributable,
   since a shared parse parses no bytes and raises nothing above. `classic_compiles + classic_parse_shared` counts the
   classic programs whose closure was obtained at all, comparable with a pre-sharing revision's `classic_compiles`.
   Raised where the parse does not happen, so it partitions programs started, not parses, and is no subset of a row
   above. The copy-out asserts that a shared parse implies some finished parse and that the gauge of bodies holding
   one does not exceed the lifetime count of finished parses. A report only (solver/dyn_body.h owns the mechanism). */
static long    g_classic_parse_shared;         /* classic programs started from a parse another flow finished */
/* The width of the byte total as a build failure: the runtime check in engine_step_unit_runs is a DCHECKF, compiled
   out in release, the build real pages are measured on. Both the accumulator and the field it is copied into, since
   narrowing either loses the run. */
_Static_assert(sizeof g_classic_compile_again_bytes >= 8,
               "solver/engine.c: the re-parsed-byte accumulator has been narrowed. On wasm32 `long` is 4 "
               "bytes, so a 32-bit total of source bytes re-parsed turns NEGATIVE after about 1500 repeats of "
               "a 1.4 MB chunk — a count a forking frontier reaches on a real bundle — and it reads as a "
               "cheap engine rather than as a broken number; see solver/engine.h's `classic_compile_again_bytes`.");
_Static_assert(sizeof ((EngineStepUnitRuns *)0)->classic_compile_again_bytes
                   >= sizeof g_classic_compile_again_bytes,
               "solver/engine.h: EngineStepUnitRuns' `classic_compile_again_bytes` is narrower than the "
               "accumulator it is copied from, so engine_step_unit_runs truncates the total on its way to "
               "solver/result.c's census — the same lost run as an overflow and with the same inverted "
               "reading behind it.");
/* The same for the dispatch accumulator: a comment does not stop the edit that narrows it to match its neighbours.
   Both objects are asserted here, the one translation unit holding both, above the copy-out and the single add. */
_Static_assert(sizeof g_step_us >= 8,
               "solver/engine.c: the dispatch loop's microsecond accumulator has been narrowed. On wasm32 "
               "`long` is 4 bytes, so a 32-bit accumulator of the slice's own measure overflows after ~35.8 "
               "minutes and hands its reader a NEGATIVE total, which reads as cheap turns rather than as a "
               "broken number — see solver/engine.h's `step_us`.");
_Static_assert(sizeof ((EngineStepUnitRuns *)0)->step_us >= sizeof g_step_us,
               "solver/engine.h: EngineStepUnitRuns' `step_us` is narrower than the accumulator it is copied "
               "from, so engine_step_unit_runs truncates the total on its way to solver/result.c's census — "
               "the same lost run as an overflow and with the same inverted reading behind it.");
/* And for the span's two halves, where narrowing is worse: they measure spans longer than `g_step_us`, so they
   overflow first, and the symptom is the partition assert firing with a negative half, which reads as a clock that
   stopped being monotone. */
_Static_assert(sizeof g_loop_us >= 8 && sizeof g_between_us >= 8 && sizeof g_slice_exit_us >= 8,
               "solver/engine.c: the slice-span accumulators have been narrowed. On wasm32 `long` is 4 bytes, "
               "so a 32-bit accumulator of the slice's own measure overflows after ~35.8 minutes — and these "
               "measure spans STRICTLY LONGER than `g_step_us` does, so they invert first.");
_Static_assert(sizeof ((EngineStepUnitRuns *)0)->loop_us >= sizeof g_loop_us &&
               sizeof ((EngineStepUnitRuns *)0)->between_slices_us >= sizeof g_between_us,
               "solver/engine.h: EngineStepUnitRuns' `loop_us`/`between_slices_us` are narrower than the "
               "accumulators they are copied from, so engine_step_unit_runs truncates them on the way to "
               "solver/result.c's census and the partition it asserts stops being about the run that happened.");
/* The histogram's sum, for the one assertion that uses it. Not under `#if APICLIENT_DEV`: a release DCHECKF leaves
   its condition unevaluated inside `sizeof`, which still needs the identifier to exist; `static inline` emits nothing
   where it is never called and draws no `-Wunused-function`. */
static inline long step_unit_runs_total(void)
{
    long t = 0;
    int i;
    for (i = 0; i < STEP_UNIT_N; i++) t += g_step_unit_runs[i];
    return t;
}

/* No completion crosses a scheduler boundary, asked at every boundary. `rt->current_exception` is per runtime while a
 * completion is per evaluation (ECMA-262 §6.2.4 The Completion Record Specification Type; §5.2.4.3 Shorthands for
 * Unwrapping Completion Records: "?" propagates an abrupt completion to the caller), so under an interleaving host a
 * throw left standing would reach a flow that did not produce it. JS_FlowResume's entry DCHECK catches the victim;
 * these catch the producer. The four boundaries cover all of the scheduler's own time:
 *   slice-entry: the host's time between slices (a reply parsed into a record, a delivery routed, an answer written),
 *                plus on the first slice the session setup;
 *   pre-step:    the pick: solve_seed_candidates, flow_next_to_run, the context switch (flow_switch_out/in, the COW
 *                and DOM delta swap, decide/pins resume) and solve_flow_begin;
 *   post-step:   the arm flow_step ran (every arm takes its own completion);
 *   slice-exit:  the tail: HTML §8.1.7.3's end-of-checkpoint hook, the aging charge, flow_finish.
 * The one that fires names the segment. The throw is taken to describe it, which is sound because DFAIL does not
 * return. Compiled out in release. */
#if APICLIENT_DEV
static void engine_no_stray_completion(JSContext *ctx, const char *where, int detail)
{
    JSValue le;
    char et[320];

    if (ctx == NULL || !JS_HasException(ctx))
        return;
    le = JS_GetException(ctx);
    result_error_text(ctx, le, et, sizeof et);
    /* The park state is printed beside the unit because `JS_CallAsFlow` answers 0 for a settle that parked and for
       one that completed: a park standing here says the work is suspended and the completion was never this
       segment's; no park says the segment finished and dropped it. */
    JS_FreeValue(ctx, le);
    DFAILF("a completion was still live in the runtime at a SCHEDULER BOUNDARY — at `%s`, after the `%s` "
           "step unit. The exception slot is per-RUNTIME and a completion is per-EVALUATION (§6.2.4), so the "
           "next flow resumed would receive a throw §5.2.4.3 says belongs to whoever asked for the "
           "evaluation that produced it. Take it where it is produced: a program's through JS_FlowResume's "
           "`pres`, a job's at the job, a parked continuation's through JS_ResumeParkedFlow's `pres`, a "
           "delivery's at JS_CallAsFlow. parked=%d detail=%d. The throw was: %s",
           where, step_unit_name(g_step_unit), JS_HasParkedFlow(JS_GetRuntime(ctx)) ? 1 : 0, detail,
           *et ? et : "(a throw this engine could not describe)");
}
#define ENGINE_NO_STRAY(ctx, where, detail) engine_no_stray_completion((ctx), (where), (detail))
#else
#define ENGINE_NO_STRAY(ctx, where, detail) ((void)0)
#endif

/* Declared above flow_step, the one function that writes them (it compiles programs and finishes flows); what each
   means is documented at the accessors that publish them. */
static long g_finished;
/* The two populations it sums (EngineFrontierCensus), written at the one line `g_finished` is, from
   `Flow.cand_src`; the identity is asserted where they are read together. Both count members: `g_finished_cands`
   is not a count of candidate sessions (the fork copies the substitution to every sibling), and engine.h names
   solve.c's `_candidates` as the per-session count to read it against. */
static long g_finished_flows, g_finished_cands;
/* The deepest program any flow started: a high-water mark, so it saturates and then reads flat, the shape of a
   ceiling, and the terminal census of a short run makes it look like a gate. Quote it with the length of its census
   series or not at all, never as a claim about what the engine can reach; whether `g_finished` crossed zero is the
   categorical question that does not depend on run length. */
static int  g_deepest = -1;
/* The deepest row any flow's cursor has left, a different fact from `g_deepest` (the deepest program started). They
   come apart when a row is legitimately skipped: HTML §4.12.1.1's "execute the script element" step 4, "If el's
   result is null, then fire an event named error at el, and return", passes a row the compile never sees (a document
   script's row converted in place, or an injected one's row appended by its failed reply). Merging them would either
   report a program started that never was, or charge every correct skip as a defect; the cursor is censused at the
   ask (the flow is leaving the row), not the outcome. Raised only in ENGINE_LEAVE_ROW, the one place a cursor moves.
   A fork does not raise it: the sibling's copied cursor is a position the parent already recorded. */
static int  g_deepest_left = -1;
/* The highest index this document has run to completion, separate from `g_deepest`: `deepest 1, completed 0` says
   a flow started program 1 and no flow ever finished it, so nothing the document loads after program 1 (a lazy
   chunk, an injected <script>, a fired PoC) has been compiled by anything. `deepest` alone proves only that some
   flow completed the program before the deepest one started. */
static int  g_completed = -1;

/* The flow leaves the row at its cursor: the one spelling of a cursor advance. A macro expanded at each of the five
 * sites, because the invariant is over a transition and a message naming one shared line would name no site. It
 * asserts the row being left exists (`script_i < dyn_n` before the move is `script_i <= dyn_n` after it), which
 * lets the message name the row, and it raises `g_deepest_left`, since the ask and the advance are one event. */
#define ENGINE_LEAVE_ROW(f) do {                                                                          \
        Flow *leave_f_ = (f);                                                                             \
        DCHECK(leave_f_->script_i < leave_f_->dyn_n,                                                      \
               "a flow is leaving a row its own program queue does not have — the cursor names the row "   \
               "the flow is AT, so a flow with no row there has nothing to leave. flow.c's "               \
               "flow_programs_unstarted_for_document states the contract this breaks (\"between two "      \
               "programs the cursor stands one past the last started row\"), and a cursor beyond `dyn_n` " \
               "is ahead of the slot every APPEND takes: the next program queued onto this flow lands at "  \
               "`dyn_n`, which this cursor has already passed, and is never run and never counted as "     \
               "unstarted. The completion that moved it was not a row of the sequence");                   \
        if (leave_f_->script_i > g_deepest_left) g_deepest_left = leave_f_->script_i;                      \
        leave_f_->script_i++;                                                                              \
    } while (0)
/* The counts those two maxima cannot carry (one member or every member at program 5 reads the same), written at the
   same line: program starts, partitioned by the started row's kind into the @S search's candidate re-fires and
   everything else. The two take opposite work (a page script starting is coverage; a candidate starting is a PoC's
   one chance to fire), and `progStarts` makes a zero candidate count readable (a run that started nothing said
   nothing about candidates). Three rows rather than two, for `g_finished`'s reason: the total and each arm are
   written at one line, and the identity, checked in engine_frontier_census, catches a second start path added
   later. `progStartsCand` may exceed `progQueuedCand`: a fork copies the whole queue, so one ask can be started once
   in each of N timelines, and no inequality between queue and start sides is asserted. A report only. */
static long g_prog_starts, g_prog_starts_cand, g_prog_starts_other;

/* See engine.h. It reads the total, not either arm: a candidate re-fire and a host's synthesized row are programs,
   and an address minted while one ran was minted after this instance began executing. No assert: the counter is
   `++`-only at the single start line and never reset, and the total-versus-arms identity belongs to
   engine_frontier_census. */
int engine_any_program_started(void)
{
    return g_prog_starts > 0;
}

/* HTML §8.1.4.4 Calling scripts, "run a module script" step 8: "If preventErrorReporting is false, then upon
 * rejection of evaluationPromise with reason, report an exception given by reason for script's settings object's
 * global object." A module completes as a promise (which is why top-level `await` is observable), so its failure is
 * not a throw the compile site can read; this reaction is that step and ends as a thrown script does, in a page
 * error.
 * It is attached with PerformPromiseThen, not `.then`: reading `then` would run whatever the page put on
 * Promise.prototype and construct through @@species, and the attach marks the promise handled, so the page's
 * `unhandledrejection` handler does not see a rejection HTML has claimed for the error report. */
static JSValue module_eval_rejected(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    DCHECK(argc == 1, "the rejection reaction of a module script's evaluation promise ran without a reason — "
                      "a promise reaction job calls its handler with exactly the settled value, so an argc "
                      "other than 1 means this function was reached from something that is not one");
    result_page_error_value(ctx, argv[0]);
    return JS_UNDEFINED;
}

/* Attaches that reaction. `ctx` is the program's realm (the module's settings object's global); a reaction
   function carries the realm it was minted in, so it is minted per evaluation, not held in a static that would
   report every document's rejection in the first document's realm. */
static void module_report_rejection(JSContext *ctx, JSValueConst eval_promise) {
    JSValue on_rejected = JS_NewCFunction(ctx, module_eval_rejected, "reportModuleScriptError", 1);
    JSValue derived;
    CHECK(!JS_IsException(on_rejected),
          "the reaction that reports a module script's evaluation error could not be allocated — without it a "
          "module that threw is silently delivered as an unhandled rejection instead of a page error");
    derived = JS_PerformPromiseThen(ctx, eval_promise, JS_UNDEFINED, on_rejected);
    CHECK(!JS_IsException(derived),
          "HTML §8.1.4.4 \"Calling scripts\" step 8's rejection reaction could not be attached to a module "
          "promise — the module's own failure would then have no reader at all");
    /* The derived promise has no reader by construction (the handler returns undefined, so it fulfils); it is the
       capability PerformPromiseThen must produce, and §8.1.4.4 returns evaluationPromise itself to its caller. */
    JS_FreeValue(ctx, derived);
    JS_FreeValue(ctx, on_rejected);
}

/* The programs are the flow's own rows (`f->dyn`), so this takes no inventory. */
static int flow_step(JSContext *ctx, Flow *f) {
    /* A flow is about to run, so the flow stamp and the DOM capture must both be on. This is the one point a flow
       is driven (every JS_FlowNew / JS_FlowResume is below it). The generation stamps every object the flow
       creates and g_dom_capture is its twin over the Lexbor tree; with either off the flow's creations would be
       recorded as baseline and its DOM writes would land nowhere, silently. */
    DCHECK(JS_FlowGen() != 0 && g_dom_capture,
           "a flow is being stepped with the flow stamp or the DOM capture off — the objects it creates would "
           "be stamped baseline and its DOM writes would be captured into no delta, so its state would leak "
           "into every sibling and its rewind would restore something that never existed");
    /* The step has not named itself yet. Every exit declares its arm (solver/step_unit.h), and starting from a value
       that is not an arm lets the scheduler's convergence assert catch an arm added without a name instead of it
       inheriting the previous step's. Reset per step, because a flow stepped through two arms must read as the
       second. */
    g_step_unit = STEP_UNIT_NONE;
    /* The step is counted where the name is cleared: an entry has happened and no arm is named. The lifetime
       histogram is counted at the convergence point, so the identity between them can fail. A report only. */
    g_steps++;
    /* Whether this step has performed its task (HTML §8.1.7.3 Processing model step 2.6, the one act an iteration of
       step 2 contains). Declared outside the loop and reset per entry, because the body iterates (the turn
       continuation at the reply delivery) and must remember across passes. A step runs at most one task and then
       only step 2.8's checkpoint, whatever the policy answers: the policy decides how much of one turn a step
       completes, never how many turns, which makes it a granularity and not a cap. Only the task arm that
       continues sets it. */
    int turn_task_done = 0;
    /* Whether this call began with a live frame, read once before the loop: the question is about the call, and its
       one consumer is the `!framed_at_entry` assert beside `g_unframed_steps++`. The loop may move `f->frame`, and
       the two disagreeing is what makes that assert a check. In release it is only type-checked inside `sizeof`. */
    const int framed_at_entry = (f->frame != NULL);
    for (;;) {
        /* One arm per distinct answer, before anything else this flow could do: everything else consumes what the
           arm is made of. The arm clones the frame this flow is suspended in, and the first resume below takes the
           answer off the register and leaves that call site; the flow is switched in, so its delta, DOM head and
           decision state are what an arm inherits.
           The three assignments at the top of this loop are guarded by `turn_task_done`, as the checkpoint arm is:
           the loop iterates (the turn continuation at the reply delivery), and a continued pass must not rename a
           step already named for the task it performed, or `deliver-one-reply` would be credited only to
           deliveries whose `turn_continues()` answered no. `turn_task_done` is that settled-name predicate: its only
           writer is the reply-delivery arm, meaning this step performed its §8.1.7.3 step 2.6 task. */
        if (!turn_task_done) g_step_unit = STEP_UNIT_FORK_PEER_ANSWER;
        if (flow_answer_fork(ctx, f)) return 0;
        /* One arm per declined request, beside it for the same reason: a fork over a value that arrived (the zone
           will not ask, two feasible outcomes and no observation, flow_decline_fork), built from the same frame,
           delta and DOM head. */
        if (!turn_task_done) g_step_unit = STEP_UNIT_FORK_DECLINED_REQ;
        if (flow_decline_fork(ctx, f)) return 0;
        /* The parked continuation outranks everything else this flow could do: a forced preempt must be
           transparent to observable ordering, so the flow resumes before any job it has queued. Yielding after
           one resume keeps the scheduler in charge; a park that parks again rides the switch-out. */
        if (!turn_task_done) g_step_unit = STEP_UNIT_RESUME_PARKED;
        /* Nothing else may move the name: the delivery arm writes `turn_task_done` on the line after it names the
           step, so on any pass created by this function's own `continue` the step is already named. A fourth
           assignment added above without the guard crashes here. */
        DCHECK(!turn_task_done || g_step_unit == STEP_UNIT_DELIVER_REPLY,
               "a continued pass of one step carries a unit name that is not the task it performed — "
               "`turn_task_done` is written only by the reply-delivery arm, on the line after it names the "
               "step, so any other name here is an assignment that ran on a pass created by this function's "
               "own `continue` and the step is about to be reported as an arm it never took");
        /* Resuming a parked continuation is progress only if the flow can still get somewhere: a flow blocked on
           the host resumes into the same wait. Its completion is taken here, the one place it can be: a parked
           continuation is the late half of a job (a step machine offers a park at every re-entry, so most jobs
           park), and an uncaught throw from it must be recorded as this page's error (§8.1.4.6 Runtime script
           errors' "report an exception", the hook scripts and jobs use) and taken out of the per-runtime
           `rt->current_exception` before another flow's evaluation finds it. */
        {
            JSValue pcv = JS_UNDEFINED;
            if (JS_ResumeParkedFlow(JS_GetRuntime(ctx), &pcv)) {
                if (JS_IsException(pcv)) {
                    JSValue e = JS_GetException(ctx);
                    result_page_error_value(ctx, e);
                    JS_FreeValue(ctx, e);
                }
                JS_FreeValue(ctx, pcv);
                return flow_blocked(f) ? FLOW_STEP_OWED : 0;
            }
            JS_FreeValue(ctx, pcv);
        }
        /* Which row this flow had started before this step, read before the block that may start another and
           consumed at the resume below. A reading of the one field the compile writes: `last_compiled` is written at
           one line and strictly increasing (the no-replay DCHECK there), so "it moved" means "this step compiled".
           Declared inside the loop body, so it is re-read on every pass. */
        int last_compiled0 = f->last_compiled;
        if (!f->frame) {
            const char *body;
            /* The ladder has been descended, counted at the one line entering it, before any arm is asked. The
               orphan ask below is raised on the same per-pass basis, so `asked` is a subset of this, asserted in
               engine_step_unit_runs (solver/engine.h's `unframed_steps`). */
            g_unframed_steps++;
            /* The call began unframed. A member born with a clone of its parent's frame cannot reach this ladder on
               the dispatch that ends that frame: the four frame-clearing arms (`resume-ended-its-frame`,
               `start-ended-its-frame`, `program-detached-its-base`, `start-detached-its-base`) all return, so the
               ladder is reached on the member's next dispatch. A `continue` at those arms would be the drain this
               function was rewritten out of: the resume that ran a program to its end performed §8.1.7.3 step 2.6's
               task, every arm below is the next iteration of step 2, and the value yield and cooperative quantum are
               consulted only where flow_step returns. Two-sided: `framed_at_entry` is read before the loop and
               `f->frame` is what the loop moves. */
            DCHECKF(!framed_at_entry,
                    "this call of flow_step began with a LIVE FRAME and has descended the unframed ladder "
                    "anyway (step_unit=%d, orphan asks so far %ld) — a frame-clearing exit must RETURN, "
                    "because the resume that cleared it performed HTML §8.1.7.3 \"Processing model\" step "
                    "2.6's task and every arm below this line is the next iteration of step 2. Whatever "
                    "added a `continue` at one of those exits has re-created the drive-to-completion this "
                    "function was rewritten out of, and has taken the scheduler's value yield and its "
                    "cooperative quantum with it, because both are consulted only where this function "
                    "returns",
                    (int)g_step_unit, g_orphan_asks);
            /* And its length, which the body already holds (solver/dyn_body.h), rather than a `strlen` over every
               byte of a program each time one starts. */
            size_t body_n = 0;
            /* Which kind the program about to be compiled is; only a page <script> must parse. */
            DynKind kind = DYN_PAGE_SCRIPT;
            /* Which of §8.1.4.4 Calling scripts' two algorithms runs it: run a classic script or run a module
               script. The row answers for every position (flow.h's `dyn_type`, written by child navigables, joined
               documents and injected elements alike). Classic is the answer for every entry a flow added:
               §7.4.2.3.2's `javascript:` URL, a lazy chunk and a `setTimeout` string are classic scripts, and a
               cross-agent operation's program is this engine's own classic text. */
            ScriptType stype = flow_dyn_type(f);
            /* Which of the orphan step's three outcomes happened, so the branch names its own work. */
            int orphan_step;
            /* What the cursor says this step can do with the flow's own sequence: holding a program and being able
               to run one are different facts. `seq_compiles` means a program starts on this step and selects the
               fall-through to the compile below. `seq_awaits` means the row at the cursor is an external script whose
               bytes have not returned, HTML §4.12.1.1 step 31's parser-blocking wait, which is not a reason for the
               flow to do nothing (see there). Never both; neither means the cursor is past the last row. */
            int seq_compiles = 0;
            int seq_awaits = 0;
            /* The parser's own `<link>` elements, ahead of everything else this flow could do: HTML §4.6.8.20 Link
               type "preload"'s "when the external resource link's link element becomes browsing-context connected",
               for elements a parse produced. A browser connects them during tree construction, before any script
               of the document runs, and a lexbor parse has no per-token seam, so the first thing that has a flow
               performs the trigger; after the programs, a chunk preload would follow the bundle waiting on its
               `load`. One element per call, so seventy links are seventy work items. Asked every step, one integer
               read once drained (core/html/html_link.c keeps a per-flow cursor over documents still owing a
               request), so a child navigable installed later is served by the timeline that created it.
               This arm and the two below each produce a task, so each is guarded by `turn_task_done`: on a pass
               the turn continuation created the task is done and only step 2.8's checkpoint may run. On an entry
               the guards are constant true. */
            if (!turn_task_done && g_link_connected_hook && g_link_connected_hook(ctx)) {
                g_step_unit = STEP_UNIT_LINK_CONNECTED; return 0; }
            /* The routed deliveries this flow was handed, before the arms that find their tasks on the queue. One
               per step, oldest first: each becomes a §9.3.3 task at the receiving Window, so tasks are enqueued in
               post order and the scheduler re-ranks between them. */
            if (!turn_task_done && flow_deliver_pending(f)) {
                g_step_unit = STEP_UNIT_ROUTED_DELIVERY; flow_deliver(ctx, f); return 0; }
            /* The operation a peer is parked on, before this flow's own programs for the delivery's reason: another
               agent's flow is suspended at it, and it becomes one of this flow's programs. */
            if (!turn_task_done && flow_perform_pending(f)) {
                g_step_unit = STEP_UNIT_CROSS_AGENT_OP; flow_perform(ctx, f); return 0; }
            /* The microtask checkpoint, before the next program and not after the last one: HTML §8.1.4.4 Calling
               scripts' clean up after running script step 3, decided by flow_checkpoint_due. The checkpoint belongs
               to the program that just finished, so nothing the sequence holds may run in front of it; everything
               below is a task. A job can park (reaction_flow_step parks a step machine suspended on a synchronous
               host request), so a blocked flow reports OWED rather than progress: reporting progress would resume
               and re-park it for ever without the host being asked. */
            if (flow_checkpoint_due(f)) {
                /* The step is named after its task, and the checkpoint is not one: a step that delivered a reply
                   and then drained the reactions it enqueued did one unit, §8.1.7.3 step 2's iteration, and renaming
                   it `microtask-checkpoint` would leave `deliver-one-reply` counting only deliveries whose turn the
                   policy cut short. A step that runs microtasks and no task is named for them. */
                if (!turn_task_done) g_step_unit = STEP_UNIT_MICROTASK;
                flow_run_one_job(ctx, f);
                if (flow_blocked(f)) return FLOW_STEP_OWED;
                /* The checkpoint is a loop: §8.1.7.3 Processing model's perform a microtask checkpoint runs "While
                   the event loop's microtask queue is not empty", so one job is a sub-step, not a unit the event
                   loop rests at. Offering here and continuing is that `While`; the offer is the scheduler's
                   (turn_continues), so the flow still yields when outranked, out of quantum or blocked, and a
                   refusal leaves the state a return always left, with the checkpoint arm taking the next job on the
                   next pick. */
                if (turn_continues()) continue;
                return 0;
            }
            /* One iteration of §8.1.7.3 Processing model step 2 ends here: the task is done (2.6) and the
               checkpoint above has drained (2.8), so the next task is the next iteration and the next step. This
               return keeps the continuation from becoming a drain: one task per call of flow_step. */
            if (turn_task_done) return 0;
            /* The networking task source: a rung of this ladder, not its floor. Fetch §2 Infrastructure's queue a
             * fetch task ends "Otherwise, queue a global task on the networking task source with taskDestination
             * and algorithm", and both kinds of reply this register holds (a `fetch()`, and an external script's
             * bytes via HTML §8.1.4.2 Fetching scripts) arrive that way, so one delivery serves both. Within one
             * source, HTML §8.1.7.1 Definitions ("For each event loop, every task source must be associated with a
             * specific task queue") and §8.1.7.3 step 2.3 ("Set oldestTask to the first runnable task in taskQueue,
             * and remove it from taskQueue") make order arrival order; which answered entry is taken is
             * flow_deliver_one_reply's residual. Only which queue is implementation-defined (step 2.1, "chosen in
             * an implementation-defined manner"), a preference and not an exclusion: §8.1.7.1's example prefers
             * input "three-quarters of the time, keeping the interface responsive but not starving other task
             * queues".
             *
             * The arm stands above the sequence because the sequence is a set page code extends (chunks, injected
             * scripts, `javascript:` URLs, operations), so "the sequence is exhausted" could stay false for ever
             * and exclude replies with no counter; a fixed order over an extensible set is a permanent exclusion.
             * Its guard is §8.1.4.4's: a task may not begin while the execution context stack is non-empty
             * (flow_stack_empty, also read by the checkpoint arm). The delivery and the reactions it triggers are
             * one turn: the step continues into the checkpoint and ends when it drains, still one reply per call
             * and never two settles without a checkpoint between them.
             *
             * It is ordered against this flow's queued tasks by arrival: `pending_next_deliverable` names the reply
             * the delivery will take and `flow_pending_ready_next_seq` its stamp, and flow_task_precedes compares
             * it against the oldest task on the same clock (solver/flow.c's g_work_seq), declining to a strictly
             * older task. The register can extend itself (a delivery runs page code, which issues the next request,
             * as in every bundler's chunk waterfall), so delivery-first would starve tasks for ever. Declining is
             * an order, not a bound: the tasks older than a given reply are fixed at its birth and finite. It
             * defers to older tasks only, not rows: a task is runnable wherever this comparison is asked, while a
             * row at the cursor may be a DYN_SCRIPT_SRC parked on its own reply, so deferring to it could deadlock;
             * `seq_compiles`, which would separate those, is computed below this arm (the row half is the residual
             * at `g_task_held_deliv`). This is the one delivery in the ladder.
             *
             * The deferral it imposes on the sequence is bounded by this register's length, not the frontier's
             * debt: a fork duplicates a register (asserted in engine_sibling_assemble), so every inherited naming
             * was pushed by the prefix both arms share. That bound is in entries and is spent one per dispatch,
             * while a fork also divides the dispatch budget among arms, so it holds without binding a member's
             * whole life. A per-member rate over `live` measures neither population (framed members never reach
             * this ladder); read solver/flow.h's `deliv_ready`/`deliv_framed`/`deliv_owed`, and `stepUnitRuns`
             * beside @COLD's `live`/`framed`, before blaming this arm for a frozen `deepest`. */
            uint64_t deliv_seq = flow_stack_empty(f) ? flow_pending_ready_next_seq(f) : 0;
            if (deliv_seq != 0 && !flow_task_precedes(f, deliv_seq)) {
                /* Whether this arm took a step the flow's task queue could have had (`g_task_held_deliv`), raised
                   before the delivery, whose settle lengthens the queue. With the stack empty a queued job is a task:
                   the checkpoint arm above is guarded by exactly `flow_job_microtask && flow_stack_empty`, asserted.
                   Since the condition excluded an older task, this counts the arm going ahead of a younger one. */
                DCHECK(!flow_job_microtask(f),
                       "the reply-delivery arm was reached with an empty execution context stack and a "
                       "microtask outstanding — the checkpoint arm's guard is exactly those two facts and it "
                       "stands above this line, so a task source has run in front of the checkpoint HTML "
                       "§8.1.4.4 \"Calling scripts\"' clean up after running script step 3 owes");
                if (flow_job_pending(f) > 0) g_task_held_deliv++;
                g_step_unit = STEP_UNIT_DELIVER_REPLY;
                flow_deliver_one_reply(ctx, f);   /* §8.1.7.3 step 2.6 */
                /* And its checkpoint, in the same turn: the settle enqueued this reply's reactions (step 2.8's), the
                   checkpoint arm stands above every task arm, and the `turn_task_done` return ends the step when they
                   are gone. A settle that parked is taken by the resume arm at the top of the loop, which returns. */
                turn_task_done = 1;
                if (turn_continues()) continue;
                return 0;
            }
            /* The flow's sequence, one table: every row is an HTML §4.12.1.1 position of some document of this agent,
               inline or external, with a type and an address, so §4.12.1.1's immediate interposition has a position
               (engine_queue_into). */
            if (f->script_i < f->dyn_n) {
                body = dyn_body_text(f->dyn[f->script_i]);
                body_n = dyn_body_len(f->dyn[f->script_i]);
                kind = flow_dyn_kind(f);
                /* The sequence holds only executable scripts (engine_queue_into asserts it where rows are made). */
                DCHECK(script_type_executes(stype),
                       "a row of this flow's script sequence carries a type that does not execute — every row "
                       "is CLASSIC or MODULE, so a third answer means the type column was never written");
                if (kind == DYN_SCRIPT_SRC) {
                    /* An external script of some document of this agent, at its position. The row holds its address
                       (§4.12.1.1's encoding-parsed url, never the raw attribute, which the host would resolve
                       against its own base), and the flow runs no program of its own here: §4.12.1.1 fixes this
                       script's position against the scripts around it. The request went out when the row was created
                       (step 33 fetches at prepare; step 35 only decides where the result runs), and an answered reply
                       would already have been delivered by the networking arm above, which the dev block asserts. */
#if APICLIENT_DEV
                    {
                        /* Read outside the condition, under the dev guard: pending_ready reads the register's
                           `length`, which interns an atom. flow_stack_empty is the other half of the networking
                           arm's conjunction; a DYN_SCRIPT_SRC row is queued DYN_POS_APPEND at both sites that
                           create one, so an immediate row cannot be how this line was reached. */
                        int ready = flow_pending_ready(f);
                        DCHECK(flow_stack_empty(f) && !ready,
                               "a flow reached an external script row holding an answered reply — the "
                               "networking task source's arm runs above the whole sequence, so an answered "
                               "entry standing here means that arm's guard and this row's have stopped being "
                               "the same question, and this flow is about to report itself owed a reply the "
                               "host has already paid");
                        /* The request step 33 owed for this row is outstanding: the park is made at the row's
                           creation and names the row by `dyn_id`, so a flow here with no entry naming the row would
                           report itself host-owed over a request nobody can list (a §7.5.10 removal that took the
                           entry without the row, or a row from some route other than engine_queue_into). A dev-only
                           walk. */
                        int parked = 0;
                        for (int j = 0, m = pending_count(f->pending); j < m; j++) {
                            JSValue pe = pending_entry(f->pending, j);
                            if (pending_get_int(pe, PEND_KIND) == FLOW_PENDING_DOCSCRIPT &&
                                (uint64_t)pending_get_int(pe, PEND_SCRIPT_ROW) == f->dyn_id[f->script_i])
                                parked = 1;
                            JS_FreeValue(ctx, pe);
                            if (parked) break;
                        }
                        DCHECK(parked,
                               "a flow is standing on an external script row that owes no request — HTML "
                               "§4.12.1.1 step 33 issues the fetch when the row is created and the entry names "
                               "the row by `dyn_id`, so a row with no entry left is one whose park was removed "
                               "without it, and this flow is about to wait for the rest of the session on a "
                               "reply the host is never asked for");
                    }
#endif
                    /* And the flow does not stop here; it spins the event loop. A parser-blocking script blocks the
                       tokenizer and nothing else: HTML §13.2.6.4.8 The "text" insertion mode, "Block the tokenizer
                       for this instance of the HTML parser, such that the event loop will not run tasks that invoke
                       the tokenizer", reached by spinning the event loop, whose expansion in §8.1.7.3 ends "This
                       causes the event loop's main set of steps or the perform a microtask checkpoint algorithm to
                       continue" (HTML §14.2 Parsing XML documents says the same for XML). Queued callbacks,
                       delivered messages, due timers and rendering opportunities still run. So the row is recorded
                       as a wait (`seq_awaits`), `seq_compiles` stays 0, the cursor stays, and the ladder below is
                       asked; if every source declines, the OWED arm reports the flow host-owed. `seq_awaits` also
                       carries the step's name to that arm, so a member parked on a fetch is not reported under the
                       previous arm's name. */
                    seq_awaits = 1;
                }
                /* HTML §4.12.1.1 Processing model, "execute the script element" step 4: "If el's result is null, then
                   fire an event named error at el, and return". This row is an element whose result is null
                   (flow_deliver_one_reply's null arm), so the cursor advances, step 6's switch is never reached and
                   nothing runs; the failed element keeps its place against the scripts around it.
                   Named residual. Not covered: step 4 fires synchronously, but this queues an element task (the
                   listener list is the page's, so the dispatch needs a flow base, core/html/html_script.h), so the
                   order between this `error` and anything queued in the same turn differs. Next diff builds: a
                   step-machine entry to "execute the script element" that flow_step drives at the row, making step 4
                   and step 8's `load` bare fires. Absence shows as: a page with `setTimeout(f,0)` beside a failing
                   `<script src>` observing `f` before `onerror`, where a browser runs `onerror` first. */
                if (kind == DYN_SCRIPT_FAILED) {
                    DCHECK(f->dyn_el[f->script_i] != NULL,
                           "a failed-load row carries no element — the row is created by the null arm of "
                           "flow_deliver_one_reply, which asserts the element on both of its two paths, and by "
                           "engine_seed_scripts for a refused `src`, which asserts it too; §4.12.1.1 fires "
                           "`error` AT el");
                    /* The element's own document's realm, never the flow's ambient one: the task's callee is a C
                       function running in the realm that defined it, and a child navigable's script element is an
                       ordinary row of this same sequence. */
                    html_script_queue_error(doc_realm(f->dyn_doc[f->script_i]), f->dyn_el[f->script_i]);
                    g_step_unit = STEP_UNIT_SCRIPT_LOAD_FAILED;
                    /* The one arm of the five that leaves a row no compile saw: step 4 ran the row's whole algorithm
                       and started nothing, which is what `g_deepest_left` exists to record. */
                    ENGINE_LEAVE_ROW(f);
                    return 0;
                }
                /* A row that reaches the compile holds a program, not an address: a DYN_SCRIPT_SRC row only sets
                   `seq_awaits`, and a DYN_SCRIPT_FAILED row (whose body is its address, the account of what failed)
                   took step 4 above. The guard also carries the assertion's premise: an address body is non-NULL, so
                   without it the DCHECK would pass on the row it is written about. */
                if (!seq_awaits) {
                    DCHECK(body != NULL,
                           "a row of this flow's sequence reached the compile with no body — a row is created "
                           "with one (engine_queue_into asserts it) and the two kinds whose body is an ADDRESS "
                           "rather than a program are both taken by the branches directly above this line");
                    /* The one state that falls through to the compile, recorded as a value the ladder below reads. */
                    seq_compiles = 1;
                }
            }
            /* The ladder and what it is asked on. The arms are bound to `seq_compiles` ("a program of this flow's
             * own sequence starts on this step"), not to `f->script_i < f->dyn_n`: the sequence is a set page code
             * extends (a lazy chunk, an injected `<script>`, a `javascript:` URL, a peer's operation), so "the
             * cursor is past the last row" could stay false for ever and exclude every source below with no
             * counter. A step that starts a program has done its unit (HTML §8.1.7.3 Processing model step 2 runs
             * one task per iteration), and every arm below returns, so one step cannot both run a task and compile.
             *
             * Which of the two carriers goes first is decided by arrival. §8.1.7.3 step 2.1 chooses a queue "in an
             * implementation-defined manner", and §8.1.7.1 Definitions bounds that freedom in its own example
             * ("keeping the interface responsive but not starving other task queues"). Each carrier extends the
             * other (a task runs page code that appends rows; a program enqueues tasks), so either strict order
             * could exclude one for ever; arrival order cannot, since an item's stamp never changes and the set
             * ahead of it is fixed at its birth and finite. An order, not a bound. The stamps are one clock's
             * (solver/flow.c's g_work_seq: a row's `dyn_run`, a callback's written at flow_job_push), which also
             * discharges §8.1.7.1's one-source-one-queue rule for sources that reach both carriers (NETWORKING,
             * NAVIGATION_AND_TRAVERSAL), since within one source arrival order is queue order;
             * core/timing/task_source.h records the narrower repair.
             *
             * The delivery arm above the sequence joins the order for tasks (PEND_WORK_SEQ, minted at every push,
             * and flow_task_precedes asked with the reply's stamp), so a flow whose register never empties still
             * runs a task that arrived first. It still precedes every row, because a row at the cursor may be a
             * DYN_SCRIPT_SRC parked on its own reply (deferring to it could deadlock) and `seq_compiles` is
             * computed only below that arm; the residual is at `g_task_held_deliv`. Its next diff hoists the kind
             * walk that sets `seq_compiles` rather than spelling "can this row start" twice. Absence shows as: a
             * census with `taskHeldDelivLifetime` near zero while `deliver-one-reply` is most of a member's steps
             * and `programCursors` does not advance. */
            /* Which carrier holds the older work item, asked only when there is a row this step could start
               (`seq_compiles`, which flow_task_precedes cannot establish itself); each refusal inside that predicate
               is a rule of the standard, argued at its definition. The row's arrival as a runnable item, `dyn_run`,
               not its name `dyn_id`: a root `<script src>` is named by the seed before any program runs but becomes
               runnable when the network answers, and ordering by name would put every job behind any seed row at the
               cursor (flow.h's `dyn_run`). Zero is no stamp (the clock starts at 1), so a live row with 0 is a column
               grown, forked, shifted or compacted without this one; asserted where a missed site first decides. */
            DCHECK(!seq_compiles || f->dyn_run[f->script_i] != 0,
                   "the row at this flow's cursor can start and carries NO arrival stamp — `dyn_run` is minted "
                   "with the row and re-minted where a delivery takes it out of DYN_SCRIPT_SRC, so a zero is a "
                   "column that was allocated, forked, grown, shifted or compacted without its nine siblings "
                   "and this step would order a program against this flow's queued callbacks by a number no "
                   "clock issued");
            int job_precedes = seq_compiles && flow_task_precedes(f, f->dyn_run[f->script_i]);
            /* The queue's length, read once for both arms of the choice (`g_task_held_seq`), so a raise and the
               dispatch it is about read one state; nothing between here and the arms touches the queue. */
            int jobs_here = flow_job_pending(f);
            if (seq_compiles && !job_precedes) {
                /* No work here: this arm's work is the compile below, and its body only declines every arm beneath
                   so control reaches that one compile. The one statement is a report (`g_task_held_seq`): the arrival
                   comparison answered no, so a task on the queue here is one the sequence went in front of. */
                if (jobs_here > 0) g_task_held_seq++;
            }
            else if (jobs_here > 0) {
                /* What is left on the queue is a task (HTML §8.1.7.3 Processing model step 2, whose 2.3 takes "the
                 * first runnable task in taskQueue" and 2.6 performs its steps). The checkpoint above consumes every
                 * microtask before any program and declines only for an immediate row (flow_stack_empty's second
                 * half), which would have set `seq_compiles`. A DYN_SCRIPT_SRC row at the cursor (the other row a
                 * flow can stand on without compiling) is queued DYN_POS_APPEND at both creating sites, so it is
                 * never immediate, and the assert below holds.
                 *
                 * The two queues partition by carrier (`dyn` holds programs, `jobs` C callbacks), while HTML §8.1.7.1
                 * Definitions partitions by source ("For each event loop, every task source must be associated with
                 * a specific task queue"; "the user agent would never process events from any one task source out of
                 * order"). Each row producer states its source, and engine_queue_into asserts it, so which sources
                 * reach `dyn` is a grep for the enumerator: NOT_A_TASK, SOLVER_CANDIDATE, NETWORKING and
                 * NAVIGATION_AND_TRAVERSAL (§7.4.2.3.2's `javascript:` URL; §7.4.2.2 step 21 queues that navigation
                 * on the navigation and traversal source while the same section's document load reaches `jobs`, so
                 * one source has two carriers, as `iframe.src =` and `location.href = "javascript:…"` show). The
                 * timer source reaches only `jobs` (§8.7 Timers runs a string handler's script inside step 9's task,
                 * substeps 9.8.7-9.8.8, in core/timing/timer.c). Arrival order discharges both the per-source order
                 * and the starvation; a queue per source would discharge only the first.
                 *
                 * A microtask is taken by the checkpoint arm above the sequence (on the holder's next step), a task
                 * only by this arm (on a step that starts no program); `jobsReadyTask`/`jobsReadyMicro` (flow.h's
                 * `jobs_ready_task`) say which arm declined a backlog, not what the order should be. The DCHECK
                 * below catches the naive repair of hoisting this arm above the sequence, which would reach it with
                 * an immediate row at the cursor holding a microtask; flow_task_precedes answers 0 under a non-empty
                 * stack and under an outstanding microtask, so the arrival comparison keeps those states out of
                 * this arm. */
                DCHECK(!flow_job_microtask(f),
                       "a task was about to begin while this flow still held a microtask — the checkpoint runs "
                       "before every program in the sequence, so reaching a task with one outstanding means a "
                       "program ran in front of the checkpoint it owed");
                /* Which of the arm's two reasons this step took it for (`g_task_arm_older`): `job_precedes` is the
                   arrival comparison answering yes (the remaining rows are younger than a queued task),
                   `seq_compiles` 0 is a member with no row left. Different populations; their sum is asserted at
                   the accessor. */
                if (job_precedes) g_task_arm_older++; else g_task_arm_no_row++;
                g_step_unit = STEP_UNIT_RUN_TASK;
                flow_run_one_job(ctx, f);
                return flow_blocked(f) ? FLOW_STEP_OWED : 0;
            }
            /* There is no third delivery here: the one delivery sits above the sequence, and a second would be two
               implementations of one register's order. Delivery-first is not safe by construction: a delivery
               consumes one entry and then runs page code, which pushes the next (a `then` fetching the next chunk, a
               module importing), so on a real page the answered set does not drain. Arrival order is the answer,
               which the arm above asks the register to join. */
            /* Nothing queued. What follows becomes due when the flow has nothing else, in the order it becomes due:
               the load lifecycle (already due; a finished parse waits on no clock), then the clock-driven sources.
             *
             * The host-owed return sits between "due now" and "finished". Everything above it is work §8.1.7.3
             * would run on this turn whatever is in flight (step 2 asks only whether a task queue has a runnable
             * task, and a `fetch()` is on no queue until it completes); the two arms below it claim the run is
             * finished (no handler will be attached to a rejection, nothing called a function), which an
             * outstanding reply can still falsify, so it gates only them. */
            /* A flow suspended mid-expression runs nothing else of its own, asked once here for every arm below.
               flow_blocked is an unanswered synchronous cross-instance read: the flow is parked at that operand, and
               running a `load` listener, rendering opportunity or timer callback now would interleave two program
               points of one flow. Stated positively so an arm added below inherits it. OWED, because only the host
               can change it (engine_host_answer). */
            else if (flow_blocked(f)) {
                g_step_unit = STEP_UNIT_HOST_BLOCKED;   /* suspended mid-expression on a synchronous request */
                return FLOW_STEP_OWED;
            }
            /* A document finished loading, in this flow's world: DOMContentLoaded across the agent's documents in
             * tree order, then `load` innermost-first, one per turn. Before the clock-driven sources, because a
             * finished parse is already due and everything due runs before the clock may move (a parse-time
             * `setTimeout(f, 0)` runs after DOMContentLoaded). Above the owed return, because HTML §13.2.7 The end
             * step 8 waits until "there is nothing that delays the load event in the Document", not for quiescence:
             * a `fetch()` or a dynamic `import()` delays nothing, and an SPA's bootstrap, router and lazy loaders
             * hang off `load`. What still holds it, per kind:
             *  - A `<script src>`, the document's own (DOCSCRIPT) or an injected one (SCRIPT): §4.12.1.1 "Whenever
             *    a script element el's delaying the load event is true, the user agent must delay the load event of
             *    el's preparation-time document", set by step 33 ("Set el's delaying the load event to true.") as
             *    it fetches, which is when the entry this reads is created (engine_queue_into).
             *  - A HOSTREQ is not a delay source; the flow is suspended, answered by the arm above.
             *  - A `fetch()` (RESOLVE) and a dynamic `import()` (MODULE) delay nothing, and this runs in front of
             *    them.
             * The other step 8 sources are per-element flags (`img`, the media elements, `script`), owned by those
             * components; core/dom/document.c's document_load_event_delayed names them at the gate. */
            else if (!pending_outstanding_kind(f->pending, FLOW_PENDING_SCRIPT) &&
                     !pending_outstanding_kind(f->pending, FLOW_PENDING_DOCSCRIPT) &&
                     g_docdone_hook && g_docdone_hook(ctx)) {
                g_step_unit = STEP_UNIT_LIFECYCLE; return 0; }
            /* The flow's own recorded drive: the half of the orphan surface that stays above the clock. A resume is
               this flow's own identity (recorded in a previous session as the drive of one body, carrying the path
               that made it reachable, and able to do nothing else until handed that body), so it stays where the
               flow's own work is; below an unbounded source it might never build its call. A seed manufactures a
               member from a body nothing asked for, so it sits below the clock-driven sources. The exit's
               `orphan_want` assert depends on this rung standing above every clock-driven source. A referenced
               document is served too (engine_set_referenced stops a last timeline finishing, not driving uncalled
               code). To reach here a member must first be dispatched (a member the pick never reaches asks nothing;
               solver/cold.h's `step_units` row `NONE`, paired with @WFQ's `unrun`, counts those), and then have: no
               live frame; no program of its own sequence it can run; no queued job; no block on a cross-instance
               read; and no document-lifecycle stage left. That last terminates: document_lifecycle_step advances
               readiness 0 -> 1 -> 2, one stage per call, asserting it moved, and answers 0 once every document is
               at 2. */
            else if ((orphan_step = engine_orphan_resume(ctx, f)) != 0) {
                g_step_unit = engine_orphan_unit(orphan_step); return 0; }
            /* §8.1.7.3's in-parallel half, the first clock-driven source because it can defer: it compares the next
               rendering opportunity with the earliest timer expiry and yields to an earlier timer. Without it there
               is no requestAnimationFrame, ResizeObserver or IntersectionObserver delivery, no
               scroll/resize/pagereveal and no Web Animations microtask checkpoint. */
            else if (engine_clock_render_asked(ctx)) {
                g_step_unit = STEP_UNIT_RENDERING; return 0; }
            /* The timer task source: §8.7 Timers' timer initialization steps end by queueing "a global task on the
               timer task source given global to run task", so a due timer is a runnable task (§8.1.7.3 step 2). */
            else if (engine_clock_timer_asked(ctx)) { g_step_unit = STEP_UNIT_TIMER; return 0; }
            /* The idle rung: Cooperative Scheduling of Background Tasks §5.1 Start an idle period algorithm,
               "called by the event loop processing model when it determines that the event loop is otherwise idle".
               Here every source above has answered no, so the loop is otherwise idle by construction. One step per
               visit (a period started or one callback queued), because §5.1 step 6 and §5.2 step 3.4 queue a task
               rather than loop. */
            else if (engine_clock_idle_asked(ctx)) { g_step_unit = STEP_UNIT_IDLE_PERIOD; return 0; }
            /* The forced surface, below every source the page arranged (the project's decision, stated as the
               rule): the three rungs above are work the page arranged (a `setTimeout`, a `requestAnimationFrame`, a
               ResizeObserver), and a seed is work the engine forces from a body nothing asked for. "What the bundle
               CAN do but didn't" does not include a boot-path `setTimeout(…, 0)` the bundle does do, so the page's
               clock runs first. "Drive early, the WFQ starves the unproductive" argues against the ladder's exit,
               not against these rungs, where it is symmetric; what breaks the symmetry is whose work it is. It is a
               priority, not a cap: both the clock and the seed's surface keep refilling (every closure bumps the
               orphan generation), and this rung is reached whenever the three hooks decline (no fully-active
               navigable, or a frame strictly ahead with a reply in the air per `event_loop_may_advance`, and no due
               timer or idle work); the close-request arm below is reached on the solvergate fixtures. Few
               dispatches reach this boundary at all, so the holder of the clock surface is the histogram's head,
               not this rung. The exit's two invariants hold by position: `g_orphan_claims_unmet` needs the seed's
               rung to have answered 0 on the pass that reaches the exit. */
            else if ((orphan_step = engine_orphan_seed(ctx, f)) != 0) {
                g_step_unit = engine_orphan_unit(orphan_step); return 0; }
            /* Only host-owed replies, an awaited script row or a refusal remain: no progress, and not finished.
             *
             * Below the clock-driven sources, as the lifecycle arm is: a debt no task source waits on must not
             * decide when a task source runs (§8.1.7.3 step 2 conditions on runnable tasks only). Whether a task is
             * due is a question about the clock, and this engine substitutes a jump for the wait §8.1.7.3 and §8.7
             * state, so the hooks above ask core/timing/event_loop.h's event_loop_may_advance before moving the
             * clock and answer 0 where it fails; a due task still runs ahead of this return. Placed above them, one
             * request in the air would take the flow out of the pick until a host event, losing every due timer and
             * rendering opportunity (testharness.js sets `all_loaded` in a `setTimeout(…, 0)` from its `load`
             * handler and would never complete). It stays above the two arms below, which claim the run is
             * finished.
             *
             * It asks `pending_outstanding`, not `pending_count`: a register holding one answered entry has a non-
             * zero count with nothing outstanding (pending.h), and a deliverable answered entry was taken by the
             * delivery arm above, so a count would mark a flow waiting for a host event that is not coming. It is
             * one bit looser than the mark's assert (`pending_entry_host_owed`) on purpose: the gap is the declined
             * entry, a flow still waiting (so not finished) that no host owes anything. FLOW_STEP_OWED is the only
             * resting verdict (the three are DONE, OWED, MORE), and the mark is rank-neutral (`flow_set_host_owed`
             * neither removes the flow nor touches its weight), so a refused flow rests under OWED with its own
             * unit name; engine_sched_slice's tail writes such survivors to the cold tier before it closes.
             *
             * `seq_awaits` is a disjunct, not an assertion below: a flow at an external script row has a reply
             * coming that replaces the row, so it must not reach the two finished arms. Part of the selecting
             * predicate, so the state is impossible in release too, where the exit's no-held-work DCHECK is
             * compiled out and reads `jobs` rather than `dyn`. It also decides the name, being a fact about the
             * flow's own sequence. */
            else if (seq_awaits || pending_outstanding(f->pending)) {
                /* Every task source is empty and a row or a reply is not; the three names answer what the flow is
                   waiting for, asked of the cursor and then of the register. */
                g_step_unit = seq_awaits
                            ? STEP_UNIT_AWAIT_FETCH_RECORD
                            : pending_host_outstanding(f->pending)
                            ? STEP_UNIT_AWAIT_OWED_REPLY
                            : STEP_UNIT_AWAIT_DECLINED;
                return FLOW_STEP_OWED;
            }
            /* HTML §8.1.4.7 Unhandled promise rejections' "notify about rejected promises" is not an arm of this
               ladder: perform a microtask checkpoint runs the queue to empty and then notifies for every settings
               object of the event loop, and engine_sched_slice performs that emptying test, so the notify is called
               there. A ladder arm reachable only when everything above is empty would never run on a page that
               keeps queueing work. */
            /* A modelled potential close request (HTML §6.10.1 Close requests), the weakest act of the ladder, work
               nobody did, and the last thing tried before the flow declares itself finished. Both orphan rungs stand
               above it, which is all the ordering needs: a resumed drive has built its call several rungs up, and a
               seeded drive is a sibling whose watcher lands in its own delta. See engine_close_request_fork for why
               this flow takes it, what ends it, and why everything past it is forced. */
            else if (engine_close_request_fork(ctx, f)) {
                g_step_unit = STEP_UNIT_CLOSE_REQUEST; return 0; }
            else {
                /* A flow may not finish holding work. Every arm above has offered its queue a turn, so a job still
                   on it here means one of them returned first, and the job would be dropped with the flow silently:
                   a dropped reaction looks exactly like a page that registered no handler. Asserted at the one
                   place "finished" is decided. */
                DCHECK(flow_job_pending(f) == 0, "a flow finished holding queued jobs — a promise reaction, a timer "
                                     "callback or a delivered message would be dropped with it");
                /* A flow of a referenced document may not finish at all (engine.h's engine_set_referenced). Having
                   nothing left to run is not being done while a peer can still ask this timeline something: it
                   waits on the host for the next operation, which is what OWED means. The flow keeps its snapshot,
                   delta and rank and is out of the pick until the host has something for it. */
                if (g_referenced) { g_step_unit = STEP_UNIT_AWAIT_PEER; return FLOW_STEP_OWED; }
                /* A resumed drive that never got its body back is counted on its way out. The seed is its own rung
                   above this exit, so reaching the exit means the seed's rung answered 0 on this same pass: the heap
                   holds no untaken orphan at this instant. The orphan set is not final (a live flow creates function
                   objects, which is why js_closure2 bumps the generation), so the route is asked at a body's
                   creation (engine_orphan_born), satisfying a claim at the moment its body exists.
                   Named residual. Not covered: a body created by another member after this flow has passed this
                   line. Next diff builds: a resting verdict for an open claim, shaped like the referenced return
                   above, which this arm cannot borrow because FLOW_STEP_OWED is a claim about the host, cleared
                   only by a host event, while a claim is answered by another flow. Absence shows as:
                   `orphanClaimsUnmet` nonzero on a run whose residue's locators all name bodies the page still
                   ships.
                   Counted rather than asserted, because a miss is a legitimate outcome (a resumed flow re-derives
                   from current sources, and the code is one); without the count a residue whose every drive missed
                   looks like one whose every drive landed. A claimant holding a body cannot stand here:
                   engine_orphan_resume builds the call, returns on exactly that pair and stands above every
                   clock-driven source, so only the two reads disagreeing reach the assert. */
                if (f->orphan_want) {
                    DCHECK(JS_IsUndefined(f->fn),
                           "a flow holds an open orphan claim and a body at the same time — "
                           "engine_orphan_resume's rung returns on exactly that pair and stands above every "
                           "clock-driven source, so reaching the exit in that state means the two reads "
                           "disagree and the drive's call would be built by nobody");
                    g_orphan_claims_unmet++;
                }
                /* And it may not finish holding an un-notified rejection, the other half of the job assertion
                   above. HTML §8.1.4.7 Unhandled promise rejections' notify runs at the end of every microtask
                   checkpoint (engine_sched_slice), a seam every step that ends a unit of work reaches, so a non-empty
                   list here means this flow reached the exit that declares its timeline over without ending a unit,
                   and the fires that would have been queued (§8.1.4.7 step 4 tasks, work this flow would still have
                   had) die with it: an error the page reported that this engine never saw. Below the referenced
                   return, because the message is about finishing and a referenced flow does not finish. Read
                   outside the condition under the dev guard: the read interns `length`, a side effect a DCHECK may
                   not have (as engine_host_take's pending_extra_count). */
#if APICLIENT_DEV
                {
                    int unnotified = unhandled_rejection_pending(ctx);
                    DCHECK(unnotified == 0,
                           "a flow finished still holding rejections nobody has been notified about — HTML "
                           "§8.1.4.7 Unhandled promise rejections' notify runs at the end of every microtask "
                           "checkpoint, and that seam is reached by every step that ends a unit of work, so a "
                           "non-empty list at the one exit that declares a timeline OVER means the checkpoint "
                           "seam and this exit have stopped agreeing about when a flow's turn ends");
                }
#endif
                /* No orphan arm here: the seed is its own rung and a seed that returns 0 does not retire, so the
                   last member cannot consume the orphan surface and leave nobody to drive it. */
                g_step_unit = STEP_UNIT_FINISHED;
                return 1;   /* all scripts + chunks + microtask jobs + live fetches + load listeners done */
            }
            /* No step unit is named here: each exit of the compile below names itself, and a classic program that
               started is named where it starts running (`start-program`). The compile is reached only from the
               `seq_compiles` arm, since every other arm of the ladder returns; an arm added without a `return`
               would reach it in silence, reading `body` uninitialised and compiling a stale `kind`. The assert
               compares a local the block above wrote with the control path that got here. */
            DCHECK(seq_compiles,
                   "control reached the compile without the sequence arm having decided a program starts — "
                   "every other arm of the task-source ladder returns, so an arm that falls through instead "
                   "is about to compile whatever `body` and `kind` were left holding");
            /* No replay, asserted at the only place a program can start. A flow compiles each row of its sequence
               once and thereafter resumes the suspended frame, so reaching here again for a row it already started
               means the resume path lost the frame and the flow would re-run side effects against a delta that
               already holds them. The one exception is a compile that handed the thread back: the parse is
               suspended at an exact production, no bytecode has run, and the next stint continues it. `compile` is
               NULL the moment a program starts, so the first assert keeps its force for every other reading, and
               the second keeps a parked parse from resuming against a row the flow has moved off. */
            DCHECK(f->script_i > f->last_compiled || f->compile != NULL,
                   "a flow compiled a program it had already started — the suspended frame was lost and the "
                   "flow is REPLAYING it, re-running side effects against a delta that already holds them");
            DCHECK(f->compile == NULL || f->script_i == f->last_compiled,
                   "a flow is holding a parse that was suspended for a DIFFERENT row than the one its cursor "
                   "now names — the parse would be resumed into a program the flow has already left, so the "
                   "bytes it is half-way through and the bytes the row holds are two different programs");
            f->last_compiled = f->script_i;
            /* The deepest program this document has reached, recorded where a program starts and by whichever flow
               starts it: a coverage fact about the document, not the flow holding the thread (see g_deepest). A row
               whose parse is parked has been reached, and re-assigning a maximum is idempotent across stints. */
            if (f->script_i > g_deepest) g_deepest = f->script_i;
            /* The program-start count is not raised here. This line is reached once per compile stint, not once per
               program, and core/…/endpoint.c stamps each address's `pre_program` from engine_any_program_started,
               so a raise here would credit forced execution with an address composed while the first program's
               parse was still suspended. It is raised where a program has started, below. */
            /* In the realm of the document the program belongs to, asked of the row, never of the session. A
               program compiled here closes over the compiling realm's global (JS_FlowNew): compiled in the root,
               `globalThis[member]` reads the root Window's member, a child document's classic script defines the
               child's globals on its creator, and a peer's operation answers about the wrong document. The queue
               entry answers which document this is a program of, for every kind; a same-origin agent cluster holds
               a realm per document, so the non-session documents are not a fixed set. */
            JSContext *prog_ctx = doc_realm(flow_dyn_doc(f));
            /* An inline program's name is its document's address: HTML §4.12.1.1's "let base URL be el's node
               document's document base URL" for a script with no `src`, which a relative `import('./chunk.js')`
               inside it resolves against. document_base_url is the one component that owns a document's address,
               and the compile is per document. */
            const char *prog_name = document_base_url(prog_ctx);
            /* An external program's name is its own address. §4.12.1.1 hands its resolved `url` to §8.1.4.2
               Fetching scripts, which creates the script from the address the bytes came from: "let script be the
               result of creating a classic script given sourceText, settingsObject, response's URL, options,
               mutedErrors, and url". It decides answers in both of §8.1.4.4's entries: a nested
               `import('./chunk.js')` in a bundle served from `/assets/app.js` is `/assets/chunk.js`, and for a
               module the name is also the module map key, so two `<script type=module src>` named by their
               document's address would be one module and the second would evaluate nothing. Asked of the row,
               because every document of this agent can hold external scripts; flow_deliver_one_reply moves the
               address out of the body column and asserts the column was empty before it did. */
            int src_flags = 0;
            {
                const char *ext = flow_dyn_url(f);
                if (ext) prog_name = ext;
                /* And which half of the document this program is. HTML §4.12.1 The script element splits a
                   document's programs by one attribute: `src` "denotes that instead of using the element's child
                   text content as the script content, the script will be fetched from the specified URL". A
                   page-script row with no address is an inline script, whose source text came in the document's own
                   response, rendered against this visitor's credentials; an external row's bytes are a subresource
                   served identically to everybody, which is why its code holds the logged-in surface a logged-out
                   visit never runs. The kind is asked too: a candidate body, a `javascript:` URL and a driven orphan
                   are address-free and none is a `<script>` the server rendered. The solver reads this where it
                   decides whether a missing member of a present record is unknown input or `undefined`
                   (solver/absent.h). */
                if (!ext && flow_dyn_kind(f) == DYN_PAGE_SCRIPT)
                    src_flags = JS_EVAL_FLAG_INLINE_SCRIPT;
            }
            /* A module is a different algorithm, not a flag: §8.1.4.4 has two entries and they part here. A classic
               script is a program: JS_FlowNew wraps it in a preemptible frame the scheduler resumes, and it
               completes with a value. A module is a graph: it is linked and evaluated, its body is an async function
               on the flow machinery's seam (a top-level `await` or a loop back-edge parks it into the slot
               JS_ResumeParkedFlow drains at the top of this loop), and Evaluate completes with a promise, which is
               why a top-level `await` is a SyntaxError in the classic entry. */
            int started;   /* did the program start: a classic gets a frame, a module has already evaluated */
            if (stype == SCRIPT_TYPE_MODULE) {
                /* Initialised, because the entry takes its address and a suspended answer writes JS_UNDEFINED into
                   it and nothing else, so the park arm's assert reads a value this line put there. */
                JSValue ev = JS_UNDEFINED;
                /* `prog_name` is this module's record identity: it keys the module map, it is `import.meta.url`,
                   and it is the base the loader registers each fetched dependency under, so JS_FlowEvalModule
                   asserts it is non-empty. */
                /* §4.12.1.1 Processing model, "execute the script element", the module arm's first step: "Assert:
                   document's currentScript attribute is null." The module arm has no set/restore bracket (a module
                   reads `import.meta` instead), so a non-null slot here means a classic script's bracket outlived
                   its own completion. */
                DCHECK(document_current_script_is_null(prog_ctx),
                       "§4.12.1.1's module arm asserts this document's currentScript is null and it is not — "
                       "a classic script's §3.1.7 bracket has outlived the program it belonged to, so this "
                       "module would run with some other script element globally exposed");
                /* Passing `&f->compile` lets 16.2.1.7.1 ParseModule give the thread back part way through (the
                   third answer), so a module's length, which the page chooses, never appears in a step's cost
                   (solver/rest_unit.h's bound (1)). Nothing here is bracketed or counted: this entry compiles and
                   evaluates, so a timing bracket would time a parse and an execution together and a `compiles` row
                   would count a program that has already run. Nor is the shared parse consulted:
                   `dyn_body_parse_ref` holds a closure a later flow instantiates, while a module's parse produces a
                   record linked and evaluated once under its name, and the module map (js_find_loaded_module) is
                   that phase's own memo. */
                int mr = JS_FlowEvalModule(prog_ctx, body, body_n, prog_name, src_flags, &ev, &f->compile);
                if (mr == 0) {
                    /* Parked mid-parse. No ENGINE_LEAVE_ROW and no cursor move, as in the classic arm: no bytecode
                       has run, this row is still the flow's row, and the ladder re-enters here when the flow is next
                       picked. The pair of asserts above this block keeps the parked parse from resuming against
                       another row; both read the one field both entries park into. */
                    DCHECK(f->compile != NULL && JS_IsUndefined(ev),
                           "a module compile answered SUSPENDED and left neither a parked parse nor an "
                           "evaluation — the flow would stand at a row whose module is neither evaluating "
                           "nor being parsed");
                    g_step_unit = STEP_UNIT_MODULE_COMPILE_YIELDED;
                    return 0;
                }
                DCHECK(f->compile == NULL,
                       "a module compile that finished left a parked parse behind it — the next step would "
                       "resume a parse for a module that has already been linked and evaluated");
                /* `started` is asked of the entry's one answer: it folds an abrupt evaluation into -1 rather than
                   returning a status beside a value. */
                started = (mr > 0);
                if (started) module_report_rejection(prog_ctx, ev);   /* §8.1.4.4 step 8 */
                JS_FreeValue(prog_ctx, ev);
            } else {
                /* The row's own body, asserted before anything is spent on it: the shared-parse question below is
                   asked of it, so a cursor that moved between the sequence arm's read and here would hand this row
                   a closure compiled from another row's bytes. */
                DCHECK(f->script_i < f->dyn_n && dyn_body_text(f->dyn[f->script_i]) == body,
                       "the row the cursor names is not the row whose bytes this compile is about — the cursor "
                       "moved between the sequence arm's read and here, so this program would be started from "
                       "another row's parse and the repeat census would mark one program's bytes for another's");
                DynBody *pb = f->dyn[f->script_i];
                /* Has some flow of this process already parsed these bytes under this row's key? Asked of every
                   classic row, with no kind or population carved out: a cache consulted only sometimes leaves the
                   other half paying the old cost with nothing saying so. It is not a second way to obtain a program:
                   there is one parse (JS_FlowCompileStep) and one instantiation (JS_FlowInstantiate), and this
                   changes only how many times the parse is spent for one set of bytes, so the false arm is the
                   producer, not a legacy body. The key is the row's, its three parts already computed for the
                   compile: `prog_ctx` (the document's realm; JS_CallInternal takes the running realm from the
                   bytecode, so a frame over a foreign closure runs against the wrong global), `prog_name` (the
                   ScriptOrModule name, the module map key and the base of a relative `import()`) and `src_flags`
                   (strictness and JS_EVAL_FLAG_INLINE_SCRIPT). `f->compile == NULL` is when the question can be
                   asked at all: a non-NULL carrier is this flow's own suspended parse of this row, and taking a
                   shared closure would leave it live under a started program, which the assert below and
                   JS_FlowCompileStep's suspend assert forbid. So the consult is made once per row, at the stint that
                   begins it. */
                JSValue held = JS_UNDEFINED;
                int shared = (f->compile == NULL)
                             && dyn_body_parse_ref(pb, prog_ctx, prog_name, src_flags, &held);
                if (shared) {
                    /* No bracket and no compile row here, which keeps the census's identities exact:
                       `g_classic_compiles` is one per program parsed and this stint parses nothing,
                       `g_classic_compile_over` times a parse and there is none, and the stint identity
                       `compile stints == classic_compiles + arms[compile-handed-the-thread-back]` holds because a
                       stint that never entered the parse is not a compile stint. g_classic_parse_shared rises
                       instead, which makes a falling `classicCompiles` attributable (see its declaration). */
                    f->frame = JS_FlowInstantiate(prog_ctx, held);
                    JS_FreeValue(prog_ctx, held);
                    g_classic_parse_shared++;
                } else {
                    /* The compile, bracketed (see g_classic_compiles). Both readings are in the slice's own measure
                     * (quantum_thread_us), so the comparison is the inequality quantum_expired() asks. Only the
                     * classic arm is bracketed: JS_FlowEvalModule compiles and evaluates, and a bracket around it
                     * would time both together. A failed compile is counted, because it parsed the bytes and spent
                     * the time. Nothing branches on either.
                     *
                     * JS_FlowCompileStep polls the same budget hook and preempt policy as the interpreter's
                     * dispatch, from the parse's production dispatch, and hands a suspended parse back through
                     * `f->compile`; the next stint continues it at the exact production, and nothing below runs.
                     * So the bracket times one stint: a stint that meets the slice is one the seam did not fire in,
                     * which makes `g_classic_compile_over` a measurement of this seam, not of the page's source
                     * length. It is per stint while `g_classic_compiles` is per program; its denominator is derived
                     * at g_classic_compiles's declaration from the two arms every stint ends in. */
                    /* Whether this stint resumes a parse (g_classic_compile_resumed), read before the call because
                       the call takes the carrier out of the slot (`*pcompile = NULL`) when it continues a parse; it
                       reads the storage the call is handed, one statement earlier. Above the timing bracket, so the
                       bracket's span stays the parse. Unconditional, not under an outcome arm: a resume is a fact
                       about how the stint started, true of one that parks again, finishes or ends in a SyntaxError,
                       and raised under an arm it would partition outcomes rather than stints. */
                    if (f->compile != NULL) g_classic_compile_resumed++;
                    int64_t t_comp0 = quantum_thread_us();
                    JSValue fn = JS_UNDEFINED;
                    int cr = JS_FlowCompileStep(prog_ctx, body, body_n, prog_name, src_flags, &fn, &f->compile);
                    if (quantum_thread_us() - t_comp0 >= (int64_t)ENGINE_QUANTUM_MS * 1000)
                        g_classic_compile_over++;
                    if (cr == 0) {
                        /* Parked mid-parse. No ENGINE_LEAVE_ROW and no cursor move: this row is still the row the
                           flow is at, and the ladder re-enters here when the flow is next picked. */
                        DCHECK(f->compile != NULL && JS_IsUndefined(fn),
                               "a compile answered SUSPENDED and left neither a parked parse nor a program — the "
                               "flow would stand at a row whose program is neither running nor being parsed");
                        g_step_unit = STEP_UNIT_COMPILE_YIELDED;
                        return 0;
                    }
                    DCHECK(f->compile == NULL,
                           "a compile that finished left a parked parse behind it — the next step would resume a "
                           "parse for a program that has already started");
                    /* A failed compile is still a compile and a suspended one is not (see g_classic_compiles): the
                       row is about what the span cost, and a stint that handed the thread back has not finished
                       spending it. Counting stints would make the denominator the number of times the scheduler
                       looked at a parse rather than the number of programs parsed. */
                    g_classic_compiles++;
                    /* Whether these bytes had been parsed before, partitioned off that total at this event (see
                       g_classic_compile_again, and solver/dyn_body.h for why the identity is the body). On this
                       line, not at the entry: a stint that handed the thread back has finished no program, so
                       marking at the entry would report the second stint of one parse as a repeat of its first.
                       The row stays nonzero with the parse shared, for two populations only this block can see: a
                       program whose parse failed holds nothing (its exception is one flow's completion, not a fact
                       about the bytes), and a flow already mid-parse of a row another flow has since finished
                       resumes its own parse to the end. Both parse the bytes twice. */
                    if (dyn_body_note_parsed(pb)) {
                        g_classic_compile_again++;
                        g_classic_compile_again_bytes += (int64_t)dyn_body_len(pb);
                    }
                    /* The floor's bound, raised unconditionally because it is a property of the buffer, not of
                       this parse: a reply is decoded per delivery, so two arms parked on one external row adopt two
                       buffers over the same chunk and a repeat between them is unobservable above. Repeats are
                       therefore in [`again`, `again + ownDecode`], the upper end loose by the number of distinct
                       such programs. */
                    if (dyn_body_is_own_decode(pb)) g_classic_compile_own_decode++;
                    if (cr > 0) {
                        /* Held for every later timeline that reaches these bytes, keyed on the identity the repeat
                           census keys on (solver/dyn_body.h). The body takes its own reference, so this flow still
                           owns `fn` and frees it below. A second offer of one body's parse keeps the first closure,
                           for the reason dyn_body_parse_hold states: `entered` is a mutable bit on the bytecode and
                           is the orphan identity. */
                        dyn_body_parse_hold(pb, prog_ctx, prog_name, src_flags, fn);
                        f->frame = JS_FlowInstantiate(prog_ctx, fn);
                        JS_FreeValue(prog_ctx, fn);
                    } else {
                        /* The parse failed and the exception is pending: no program starts (§4.12.1.1's "if el's
                           result is null, then fire an event named error at el, and return"), and nothing is held,
                           since a SyntaxError is not a fact about the bytes a later timeline may inherit. */
                        f->frame = NULL;
                    }
                }
                started = (f->frame != NULL);
                /* §4.12.1.1's classic arm, steps 1-2, here rather than around a call: the arm's third step ("run
                   the classic script") is JS_FlowNew plus every JS_FlowResume after it, spread over any number of
                   scheduler steps with sibling flows running between. A C save/restore bracket would set the slot
                   for whichever flow was running when the next program started; the write instead rides this
                   flow's COW delta (core/dom/document_current_script.c), so a context switch unapplies it, a park
                   carries it and a resume restores it byte-identically. Only if the program started: a failed
                   compile runs no script, so there is nothing to bracket and the restore below is not owed. */
                if (started && flow_dyn_el(f)) {
                    /* Step 1's oldCurrentScript is null for a row of this sequence, asserted here because this
                       caller's premise supports it: a flow holds at most one live program frame (the compile runs
                       only under `!f->frame`), so the previous row's restore has already run. A non-null value is a
                       skipped restore, or two flows' writes leaking across the COW delta. §4.12.1.1 step 36's nested
                       run cannot make this claim and carries the value instead (core/html/html_script.c). */
                    JSValue old = document_current_script_set(prog_ctx, flow_dyn_el(f));

                    DCHECK(JS_IsNull(old),
                           "a classic script element began executing while this document's currentScript was "
                           "still set — a program of the flow's own sequence cannot nest, because the flow "
                           "holds one program frame, so this is two script executions sharing one document's "
                           "slot: either the restore at the previous program's completion was skipped, or the "
                           "write is not riding the running flow's COW delta");
                    JS_FreeValue(prog_ctx, old);
                }
            }
            /* How many programs started, counted where `started` is decided and nowhere else: the module arm
               answers from `mr > 0`, the classic from `f->frame != NULL`, and this is the first line with either
               answer in hand. A failed compile is not a start (a SyntaxError or the OOM floor leaves `started`
               false), as §4.12.1.1's "if el's result is null, then fire an event named error at el, and return"
               says. The three counters are one partition, asserted in engine_frontier_census. The arm is read from
               the row at the cursor, never the flow: a candidate session (`Flow.cand_src`) re-runs the document
               from the baseline and compiles the page's own programs too, and `progQueuedCand` is the ask these
               starts are read against. */
            DCHECK(f->compile == NULL,
                   "a program's start is being counted while this flow still holds a suspended parse — both "
                   "compile arms above assert the carrier is NULL once they have an answer and both park arms "
                   "RETURN, so reaching this line with one live means a park arm now falls through to the "
                   "start count and the count has gone back to measuring compile STINTS rather than programs");
            if (started) {
                g_prog_starts++;
                if (flow_dyn_kind(f) == DYN_CANDIDATE) g_prog_starts_cand++; else g_prog_starts_other++;
            }
            if (!started) {
                /* What failed, read before anything is decided from it. A SyntaxError is the program's; out of
                   memory is the physical floor. Reporting the second as the first sends a reader looking for a
                   parse bug in code that parses. */
                JSValue exc = JS_GetException(ctx);
                bool oom = JS_IsOutOfMemoryError(ctx, exc);

                /* OOM is a CHECK in dev and release alike: a dropped flow corrupts the frontier. This allocation
                   is inside the partial self-park's safepoint, so the refusal behind this NULL already went
                   through the runtime's reclaim edge (quickjs.h's JSMemoryReclaimFunc, answered by
                   engine_reclaim_tail), which paged the lowest-weight member out to the cold tier and retried for
                   as long as it had a member to give. A NULL here means the frontier is down to the running flow,
                   and only the host's Level-1 eviction (a whole-engine park) carries past it. The reclaim is armed
                   only across a flow step (`g_reclaim_allowed`), the one region in which the engine holds no
                   position in its own frontier; an allocation refused at host time (a walk over the flows, the
                   result document, a reply being routed) is refused with the tail unsold and reported by its
                   asker. */
                CHECK(!oom, "the frontier could not hold another flow after the cold tier had already paged out "
                            "every member it could — this is the physical RAM floor with the tail already sold. "
                            "The engine is down to the flow that is running; what carries past this is the "
                            "HOST's Level-1 eviction (engine_request_park), which gives up this whole document's "
                            "residue for a document worth more");
                /* An @S candidate that does not parse is a dead candidate: the search tries several breakouts per
                   sink because most do not fit most contexts. A `javascript:` URL that does not parse is HTML
                   §7.4.2.3.2's abrupt evaluation, which produces no Document and no navigation
                   (`<a href="javascript:{{{">` is a link that does nothing). A page script that does not compile
                   still asserts. */
                /* For a module this is the parse only: §8.1.4.4's module entry loads the graph (16.2.1.6.1.1,
                   asynchronous; the host fetches each specifier) and links it on that load's fulfilment, so a graph
                   that fails to load or link rejects the evaluation promise and is reported by
                   module_report_rejection, as HTML's "set moduleScript's error to rethrow" says. What fails before
                   a module starts is 16.2.1.7.1 ParseModule, the page's own SyntaxError. */
                /* A cross-agent operation's program is the engine's own text (core/frame/remote_op.c), so it parses
                   or this engine wrote it wrong, and skipping it would leave the peer's flow parked on an answer
                   never coming. The asserts carry the SyntaxError, the part a reader can act on: `exc` holds the
                   message and, through the error's stack slot, the position. */
#if APICLIENT_DEV
                /* And a host instrument's program is text the host wrote, so it parses or the instrument is wrong;
                   skipping it would leave the driver an empty register, reading as a document with no geometry. */
                if (kind == DYN_PAGE_SCRIPT || kind == DYN_CROSS_AGENT_OP || kind == DYN_VALUE_DUMP) {
                    char et[320];

                    result_error_text(ctx, exc, et, sizeof et);
                    DFAILF("%s — the compile reported: %s",
                           kind == DYN_PAGE_SCRIPT
                               ? "flow_step: a page <script>/chunk did not start — its source did not COMPILE "
                                 "(a classic script's program, or a module script's 16.2.1.7.1 ParseModule)"
                           : kind == DYN_CROSS_AGENT_OP
                               ? "the program that performs a cross-agent operation did not compile — it is "
                                 "this engine's own text, and skipping it parks the asking flow on an answer "
                                 "nothing will send"
                               : "a host instrument's program did not compile — its text crossed the ABI from "
                                 "the driver that asked for the dump (engine_request_dump), so this is that "
                                 "instrument's own SyntaxError and not the page's",
                           *et ? et : "(a throw this engine could not describe)");
                }
#endif
                JS_FreeValue(ctx, exc);
                /* Step over it. Not advancing would leave the flow at the same unparseable body, compiled again on
                   every step, never finishing and never progressing; a dead candidate is skipped. This is the one
                   arm of the ladder that advances the cursor without running anything, so it has its own row: a
                   frontier standing here is stepping through bodies (a run of dead @S candidates, or a page whose
                   modules do not parse), and the work that answers it is at the compile, never the scheduler. */
                g_step_unit = STEP_UNIT_NO_COMPILE;
                ENGINE_LEAVE_ROW(f);
                return 0;
            }
            /* A module has already run as far as it goes in this unit, so its row is finished here and the flow
               leaves with no frame of its own. A module body is an async activation the flow machinery owns, not a
               program handle: it parked itself into the slot the top of this loop drains (a top-level `await`, or a
               loop back-edge under the forced preempt), and the scheduler resumes it there like every parked
               continuation. Its evaluation promise settles as a reaction on this flow's queue, run by the
               checkpoint above, so nothing here waits for it. It is its own arm, §8.1.4.4's other entry: this exit
               leaves no frame, so the flow's queued reactions are reachable on the next step, while the classic
               exit below leaves a live frame and every job arm is out of reach until it completes. */
            if (stype == SCRIPT_TYPE_MODULE) {
                g_step_unit = STEP_UNIT_EVALUATE_MODULE;
                ENGINE_LEAVE_ROW(f);
                return 0;
            }
        }
        /* HTML §8.1.4.4 Calling scripts, run a classic script step 8's third bullet: built at the completion below
           and installed after that completion's frame has been freed, since the report takes the slot; declared out
           here for that reason. NULL for every completion but a classic script's abrupt one. */
        JSValue *report = NULL;
        /* Whether the frame is about to run a program or a call, asked of the frame before it can complete and be
           freed, since the two completions mean different things below. A driven orphan's flow runs its call and
           then whatever that call queued (a lazy chunk it loaded is an ordinary program of this flow's sequence),
           so `f->orphan` says what the flow is and this says what this completion is. The position in the sequence
           cannot answer it: a row queued by the running call moves the cursor back inside the sequence while the
           call frame is live. Declared out here so the cursor advance at the tail can ask it too. */
        int is_call = JS_FlowIsCall((JSValue *)f->frame);
        /* And whether it is a program, which is not the negation of `is_call`. A base has three kinds, and
           `!is_call` is also true of a clone of an activation this host never created, taken by JS_FlowClone at a
           concolic branch inside a module body, whose row flow_step's module arm left when HTML §8.1.4.4 "Calling
           scripts"' run a module script Evaluate returned. Read as that row's program, such an arm would leave the
           row a second time: ENGINE_LEAVE_ROW aborts where the cursor is at the end, and elsewhere the next row is
           skipped in silence (a row behind the cursor is never compiled, and flow_programs_unstarted_for_document
           does not count it). The consumers that mean "this completion finished the row at the cursor" ask this;
           both are one read of `base_kind` (JS_FlowIsProgram). The third kind is driven by JS_FlowResume's
           FLOW_BASE_ASYNC_CALL arm, which refuses a second driver: "two drivers for one state is how a flow gets
           resumed twice". */
        int is_program = JS_FlowIsProgram((JSValue *)f->frame);
        /* The frame and the cursor pair agree about it. flow.c's flow_programs_unstarted_for_document declares the
           cursor side ("between two programs the cursor stands one past the last started row and during one it
           stands on it"), so `last_compiled == script_i` is "inside a program of its sequence", from two fields
           the compile site maintains. `f->reporting` is the one call frame that is the row's remaining work
           (§8.1.4.4 step 8's report is inside step 8), so it sits on the frame side. Two-sided, so a frame kind
           that stops matching the cursor pair fires here rather than at whichever consumer is reached first. */
        DCHECKF((is_program || f->reporting) == (f->last_compiled == f->script_i),
                "this flow's FRAME and its CURSOR disagree about whether it is inside a program of its own "
                "sequence: the frame says %s and the pair says %s (script_i=%d last_compiled=%d "
                "is_program=%d is_call=%d reporting=%d). flow.c's flow_programs_unstarted_for_document states "
                "the contract — between two programs the cursor stands one past the last started row and "
                "during one it stands ON it — and the compile site is the only writer of `last_compiled`, so "
                "one of the two was moved by something that is not a program starting or a row being left",
                (is_program || f->reporting) ? "inside one" : "between programs",
                (f->last_compiled == f->script_i) ? "inside one" : "between programs",
                f->script_i, f->last_compiled, is_program, is_call, f->reporting);
        {
            /* A <script>'s completion value is not observable to the page (only an eval API surfaces one), so it is
               taken and released here, never discarded by the engine, which would hide a live value from the host. */
            JSValue cv = JS_UNDEFINED;
            /* Whether this step started the frame it is about to resume. The block above is the only writer of
               `last_compiled` in this function and writes it strictly upward, so the field moving across that block
               is the compile itself; a flow that arrived with a live frame never entered the block. The two are
               opposite facts about a frontier: a start advances this flow's HTML §4.12.1.1 position (a program that
               had never run is now running), a resume advances a program counter inside one already running, and a
               census of all starts and one of all resumes are the same census when they share a row. */
            int started_here = (f->last_compiled != last_compiled0);
            /* Named residual. Not covered: a start whose parse spanned several stints. `f->last_compiled =
             * f->script_i` runs at the top of the compile site, above the shared-parse consult and
             * JS_FlowCompileStep, so a parse that hands the thread back parks with the field already at the
             * cursor, and the later stint that finishes it, installs the frame and reaches this line reads it
             * unmoved. `started_here` is therefore "this step is the first stint of this row's parse": the five
             * start rows (`start-a-classic-program`, `start-blocked-on-a-host-answer`, `start-ended-its-frame`,
             * `start-reported-an-exception`, `start-detached-its-base`) count classic starts parsed in one stint
             * plus those taken from a parse another timeline finished, and a multi-stint start is filed under
             * `resume-program` or a sibling. Next diff builds: the question asked of the frame. `framed_at_entry`,
             * read before the loop, is "this call did not install the frame it is about to resume", true on every
             * stint that installs a frame and no other, for a shared parse as for a parsed one. Absence shows as:
             * on a document whose programs are long enough for the parse to rest, `sum(start rows) -
             * classicParseShared` below `classicCompiles` while `compile-handed-the-thread-back` is nonzero, with
             * the missing starts in `resume-program` and its siblings. */
            /* A program this step started is never a call. Three consumers below (`g_completed`, §4.12.1.1 step
               4's restore, and the cursor advance at the tail) are guarded by `is_call` on the premise that a call
               frame holds no row of the sequence, and the frame the block above built holds the row
               `last_compiled` was written from; a true reading would have all three exclude this row's program. */
            DCHECK(!started_here || !is_call,
                   "the frame a step just COMPILED reports itself as a call root — JS_FlowNew builds a program "
                   "base, so this is a compile that produced something else, and the three consumers below "
                   "that exclude calls would each exclude the row this step just started");
            g_step_unit = started_here ? STEP_UNIT_START_PROGRAM : STEP_UNIT_RESUME_PROGRAM;
            int r = JS_FlowResume(ctx, (JSValue *)f->frame, &cv);
            /* HTML §6.10.1 Close requests' step 9, taken where a task's completion value is in hand: "Alternative
               processing: Otherwise, there was nothing watching for a close request. The user agent may instead
               interpret this interaction as some other action, instead of interpreting it as a close request."
               This engine declines the "may" (there is nothing to reinterpret the gesture as), but the fact stops
               the arrival being modelled again in this timeline, and it is the algorithm's own answer. Latched and
               never cleared: a path cannot un-observe it. It is the first arm of this chain, for `is_call`'s
               reason: `close_req` is a fact about the live frame and `flow_dyn_kind` about the row at the cursor,
               which is not the frame completing here. A `close` handler that loads a chunk or is handed a
               cross-agent operation moves `dyn_n` past the cursor while the task runs, so the arm below could
               claim the task: the operation's answer machinery would be handed step 9's boolean, a peer would
               resume on it, and this flow would never latch and would model close requests for ever. The arms
               split on `JS_IsException` because a completion is either abrupt or a value. */
            if (r == 0 && f->close_req && !JS_IsException(cv)) {
                DCHECK(is_call,
                       "a modelled close request's frame does not report itself as a call root — it is built "
                       "by JS_FlowNewCall, so a program base here is a frame this flag was set over that is "
                       "not the task");
                DCHECK(JS_IsBool(cv),
                       "HTML §6.10.1 Close requests' task completed with something that is not a boolean — "
                       "its machine's fini yields step 9's one fact and nothing else, so a value of another "
                       "type is a completion that came from somewhere that is not the task. UNDEFINED HERE "
                       "IS A DIFFERENT FAULT AND NAMES ITS OWN SITE: it is what a call flow answers when "
                       "nobody takes its result, so close_request_flow's JS_FlowNewCall was built with "
                       "take_result false and the boolean was freed one frame below this read");
                if (JS_ToBool(ctx, cv)) f->close_req_none = 1;
            }
            /* A cross-agent operation's completion is an answer, asked ahead of every throw arm below because this
               program is another agent's operation: its throw belongs to the flow that asked, and reported as this
               document's page error it would be lost and the peer would resume with `undefined` where the spec
               propagates a throw. It stands behind the close-request arm, which is about the live frame rather
               than the row at the cursor (see that arm for why the order is not a preference). */
            else if (r == 0 && flow_dyn_kind(f) == DYN_CROSS_AGENT_OP)
                flow_answer_perform(ctx, f, cv);
            /* A host instrument's completion is its answer, taken before every throw arm for the same reason: each
               would report it as this document's page error and hand the driver an empty register. Its throw is
               not handled here: an instrument's program is the host's own text over the page's platform, so a
               throw escaping it is a capability this engine lacks, a crash naming what to build rather than a
               value to report. It falls through to the classic script's report arm below, which records the thrown
               value's own text in the result document's `pageErrors`. */
            else if (r == 0 && flow_dyn_kind(f) == DYN_VALUE_DUMP)
                flow_emit_dump(ctx, f, cv);
            /* The report itself completing abruptly is this engine's defect, asked first because a report frame is
               a call root and so is a driven orphan's: the orphan arm below would read it as the exploration
               surface and swallow an engine bug. Nothing the page writes reaches here: DOM §2.9's inner invoke step
               2.11 catches a listener's throw and reports it (event_target.c declares catches_abrupt for that), and
               HTML §8.1.4.6 step 6's error reporting mode stops the report of a report. So what completed abruptly
               is a step of §8.1.4.6 itself. */
            else if (JS_IsException(cv) && f->reporting) {
                JSValue e = JS_GetException(ctx);
                /* The thrown value's own text, recorded before the abort discards it: `msg` reaches the assertion
                   line unescaped (check.h), so it cannot carry the exception's message (result.h states the same
                   rule). */
                result_page_error_value(ctx, e);
                JS_FreeValue(ctx, e);
                /* The flag is not cleared here. The DFAIL compiles out in release, so control falls through to the
                   tail, where the flag says this frame was the row's remaining work and owes §4.12.1.1 step 4's
                   restore; clearing it would leave `document.currentScript` set for the rest of the session in the
                   build that cannot report why. */
                DFAIL("HTML §8.1.4.4 \"Calling scripts\" step 8's report of a classic script's abrupt "
                      "completion ITSELF completed abruptly. DOM §2.9 inner invoke step 2.11 catches a "
                      "listener's throw and §8.1.4.6 step 6's error reporting mode stops a report of a report, "
                      "so this is a step of §8.1.4.6 throwing rather than the page — the value's own text is "
                      "the last entry of the result document's `pageErrors`, and what it names is built in "
                      "core/events/report_exception.c");
            }
            /* A modelled close request that threw is this engine's defect, asked before the orphan arm because both
               can be true of one flow: a flow that drove an uncalled function carries `orphan` for the rest of its
               life, and the frame completing here is the close request's task, not that drive's call, so the orphan
               arm would swallow the throw. Nothing the page writes reaches here: HTML §6.10.2 "Close watcher
               infrastructure" says a watcher's cancel action and close action each "can never throw an exception",
               and DOM §2.9's inner invoke step 2.11 catches a listener's throw. So what completed abruptly is a step
               of §6.10.1 or §6.10.2 itself. */
            else if (JS_IsException(cv) && f->close_req) {
                JSValue e = JS_GetException(ctx);
                /* The thrown value's own text, recorded before the abort discards it (check.h: the assertion line
                   is unescaped). */
                result_page_error_value(ctx, e);
                JS_FreeValue(ctx, e);
                DFAIL("HTML §6.10.1 \"Close requests\"' close request steps completed ABRUPTLY. §6.10.2 states "
                      "that a close watcher's cancel action and its close action can never throw, and DOM "
                      "§2.9's inner invoke step 2.11 catches a listener's throw, so this is a step of the "
                      "close-request or close-watcher algorithms throwing rather than the page — the value's "
                      "own text is the last entry of the result document's `pageErrors`, and what it names is "
                      "built in core/html/close_request.c and core/html/close_watcher.c");
            }
            /* A driven orphan that threw is the exploration surface, not a page error. The page never called this
               function; this engine did, with unknown input for every argument and the receiver, so
               `this.config.url` throwing a TypeError is forced execution on unknown input working. Recording it as
               the page's would report a document error per uncalled function, attributing to the bundle a throw
               only this engine's invocation could produce. The throw is consumed rather than left live: the next
               thing to ask the context anything would find it. */
            else if (JS_IsException(cv) && f->orphan && is_call) {
                JSValue e = JS_GetException(ctx);
                JS_FreeValue(ctx, e);
            }
            /* HTML §8.1.4.4 "Calling scripts", run a classic script step 8, third bullet: "Otherwise, rethrow
             * errors is false. Perform the following steps: 1. Report an exception given by
             * evaluationStatus.[[Value]] for script's settings object's global object. …" This is an `error` event
             * at the global, not a diagnostic: a page that installs `window.onerror` for routing or telemetry takes
             * a browser's path. The console line is written at §8.1.4.6 step 7.3, behind step 7's notHandled,
             * through the hook registered beside the unhandled-rejection one, so a throw with nothing listening is
             * reported and one whose listener calls `preventDefault()` is not.
             *
             * Step 8's other two bullets are `rethrow errors is true`, and the one caller that wants the throw back
             * is a peer's operation, whose completion the cross-agent arm above hands to the agent that asked; that
             * arm is step 8's first bullet, so this one is unconditional. The realm is the program's: "script's
             * settings object's global object" is the document the row belongs to, the realm the program was
             * compiled in, not the realm the scheduler holds or the session's. */
            else if (JS_IsException(cv)) {
                JSValue e = JS_GetException(ctx);
                report = report_exception_flow(doc_realm(flow_dyn_doc(f)), e);
                JS_FreeValue(ctx, e);
            }
            /* One program's completion value is read, and it decides a navigation. HTML §7.4.2.3.2's evaluate a
               javascript: URL step 9: "if evaluationStatus is a normal completion, and evaluationStatus.[[Value]]
               is a String, then set result to it", and a non-null result becomes a new Document, built from a
               synthesized `text/html` response whose body is that string, which replaces the target navigable's
               active document. Every other completion is step 10's null, the ordinary `javascript:doThing()`,
               finished here. This is the only place the value exists, so the condition is asked here rather than
               at the row that queued the program. */
            if (r == 0 && JS_IsString(cv) && flow_dyn_kind(f) == DYN_JAVASCRIPT_URL)
                DFAIL("a `javascript:` URL evaluated to a STRING — HTML §7.4.2.3.2 step 9 turns that into a new "
                      "Document that REPLACES the target navigable's active document, built from a synthesized "
                      "`text/html;charset=utf-8` response whose body is the string. §7.4's navigate can only "
                      "load an address the host FETCHES (navigable.c's js_nav_load_step asks "
                      "`document.fetch\\t<prov>\\t<url>`), so build the navigate that takes a RESPONSE THE "
                      "ENGINE ALREADY HAS and route this through it");
            JS_FreeValue(ctx, cv);
            /* §8.1.4.4 step 8 is about an evaluationStatus, and a frame that suspended or detached has none: both
               returns below leave without reaching the install, so a report built on those paths would be dropped
               silently, carrying an exception that is not this program's completion. */
            DCHECK(report == NULL || r == 0,
                   "§8.1.4.4 step 8's report was built for a frame that has not COMPLETED — a suspended or "
                   "detached base has no evaluation status to be abrupt, so the exception this report carries "
                   "belongs to something other than this program's run");
            if (r == 1) {
                /* A mid-frame yield, and the one case where it is not more work. A flow that suspended inside a
                   machine holding an unanswered synchronous host request has no work until the host answers, and
                   reporting it runnable would have the scheduler hand it the thread again at once; OWED is waiting,
                   not finished. The two states are two rows: a resume the quantum preempted advanced the program,
                   while one that suspended owing the host an answer advanced nothing however often it is
                   re-dispatched, a fact about the reply door that a different diff repairs. Read once and spent
                   twice (the arm and the return), so the four frame-live rows partition by construction
                   (solver/step_unit.h). */
                int blocked = flow_blocked(f);
                g_step_unit = started_here ? (blocked ? STEP_UNIT_START_BLOCKED  : STEP_UNIT_START_PROGRAM)
                                           : (blocked ? STEP_UNIT_RESUME_BLOCKED : STEP_UNIT_RESUME_PROGRAM);
                return blocked ? FLOW_STEP_OWED : 0;
            }
            if (r == JS_FLOW_DETACHED) {
                /* the base registered itself as a continuation elsewhere (a module body's top-level await): it
                   is no longer this flow's to free, and the awaited promise will drive it from here. */
                /* And that is the one completion path §4.12.1.1's classic restore cannot reach, so it is asserted
                   where it would be born. A detached base is a module body's continuation (a module has no
                   bracket); a classic program that detached would leave this document's §3.1.7 currentScript set
                   for ever, and the next program's set would assert one step later at the wrong site. */
                DCHECK(flow_dyn_el(f) == NULL || document_current_script_is_null(doc_realm(flow_dyn_doc(f))),
                       "a classic script's program DETACHED its base while §4.12.1.1's currentScript bracket "
                       "was open — the restore is at the program's completion and a detached base never "
                       "reaches one, so this document's currentScript would stay set for the rest of the "
                       "session. Route the restore through whatever drives the detached continuation");
                /* This row is over, which the cursor says and the flag must say with it: a frame this flow no
                   longer owns cannot be §8.1.4.4 step 8's report of the row the cursor is about to leave, and a
                   standing flag would make the next program's completion read as a report. The cursor moves only
                   for a frame that is this row's work, the predicate the restore below and the tail use (expanded
                   here, not shared, for the reason the tail gives). A detaching base is reached only through
                   JS_FlowResume's FLOW_BASE_ASYNC_CALL arm, a clone of a module body taken at a concolic branch,
                   whose row flow_step's module arm left before the fork existed; so no state reachable here
                   advances today, and the day a program or a report detaches, its row is finished here and the
                   advance is owed. */
                if (f->reporting || is_program) {
                    ENGINE_LEAVE_ROW(f);
                }
                /* A modelled close request cannot reach this exit: a detaching base is a module body handing its
                   continuation to an awaited promise, and §6.10.1's task is a call of a step machine with no module
                   in it. A set flag here is a frame that registered itself elsewhere while this flow still expects
                   its completion value, and step 9's answer would be lost silently. */
                DCHECK(!f->close_req,
                       "a modelled close request's task DETACHED its base — HTML §6.10.1 Close requests' task "
                       "is a call of a step machine, which has no continuation to hand to an awaited promise, "
                       "so this flow is about to drop the completion value step 9's answer rides on");
                f->close_req = 0;
                f->reporting = 0;
                /* This exit is its own arm, named here because the assignment before the resume runs before any of
                   JS_FlowResume's four outcomes exist. Every job, delivery, task, lifecycle and retirement arm of
                   flow_step is under `frame == NULL`, so a step that clears the frame makes that ladder reachable
                   and one that leaves it live does not. `started_here` selects the arm: a start is forward progress
                   on either outcome and may not be filed under a resume's row, nor under `start-a-classic-program`,
                   whose frame is still live. A module that starts returns at EVALUATE_MODULE above, so the start
                   arm is unreachable today; it is named so that the day a started frame detaches, the step stays in
                   the row HTML §4.12.1.1 position advances are counted in. */
                g_step_unit = started_here ? STEP_UNIT_START_DETACHED : STEP_UNIT_DETACH_PROGRAM;
                f->frame = NULL;
                return 0;
            }
            /* The program ran to its end: the one event that moves this document's completed depth (g_completed),
               recorded at the only line where a program's own completion is the fact in hand. The other sites that
               advance `script_i` are not completions: a module evaluated to a promise rather than a value, a
               detached base handed its continuation to an awaited promise, and a failed compile never started.
               Two frames that complete here are not programs either, which is why the guard is `is_program`: a
               driven orphan's call holds no row (its cursor is one past the last program), and an arm forked
               inside a module body settles the module's evaluation promise rather than finishing a row, a row
               flow_step's module arm already left. Counting either would report a program this document never
               ran to completion, in the number engine.h argues answers "has anything this document loads after
               program i ever been compiled". */
            if (is_program && f->script_i > g_completed) g_completed = f->script_i;
            /* The step is renamed here, at the only line that knows the resume's outcome: the assignment before
             * JS_FlowResume can say which of §8.1.4.4 "Calling scripts"'s two entries this step took but not what
             * became of the frame, and the frame is what the row is read for. The mid-frame yield above returns
             * with the frame live; this arm frees it (solver/step_unit.h has the readings a merged row loses).
             *
             * The report is the other arm, which makes the predicate the frame and not the program. Run a classic
             * script step 8's third bullet is "Otherwise, rethrow errors is false. Perform the following steps:",
             * whose step 1 is "Report an exception given by evaluationStatus.[[Value]] for script's settings
             * object's global object"; that bullet holds one list, so `8.3.1` is a step. The report takes the slot
             * this program vacated (the tail installs it as `f->frame` and returns), so a completion that owes one
             * ends a program and leaves the member framed: named as an end it would put a framed member in the
             * population that can reach the ladder, named as a resume it would put an ended program among
             * programs that did not end. A start's outcomes split the same way: `start-a-classic-program` returned
             * with its frame live, `start-ended-its-frame` and `start-reported-an-exception` cleared it. One
             * predicate evaluated once at this line, so the rows partition by construction. */
            g_step_unit = started_here
                ? (report ? STEP_UNIT_START_REPORTED : STEP_UNIT_START_ENDED_FRAME)
                : (report ? STEP_UNIT_REPORT_EXCEPTION : STEP_UNIT_END_FRAME);
            /* §4.12.1.1's classic arm, step 4: "Set document's currentScript attribute to oldCurrentScript.", the
             * other end of the bracket the compile opened, at the line where a program's own completion is in hand
             * (as for `g_completed`). It covers a program that ran to its end and one that threw, since §8.1.4.4's
             * run a classic script reports the exception rather than propagating it, but not one that owes a
             * report: step 8's report is a step inside step 3's run, so a page's `error` listener reads the
             * throwing `<script>` out of `document.currentScript`, as in a browser, and the restore travels to the
             * report frame's own completion. A parked program never gets here: a mid-frame yield returned above
             * with the frame live, and the slot went with the flow's delta.
             *
             * The guard is the frame's kind, because the cursor is not one: a driven orphan's call frame sits past
             * the end of the sequence (where flow_dyn_el answers NULL), but a row the running call queued moves the
             * cursor back inside it. `f->reporting` tells the one call frame that is this row's remaining work
             * (flow.h). It is `is_program`, not `!is_call`, because an arm forked inside a module body has nothing
             * to restore: §4.12.1.1's module arm has no bracket (the compile site asserts the standard's "Assert:
             * document's currentScript attribute is null"). */
            if (!report && (f->reporting || is_program) && flow_dyn_el(f))
                document_current_script_restore(doc_realm(flow_dyn_doc(f)), flow_dyn_el(f), JS_NULL);
        }
        JS_FlowFree(ctx, (JSValue *)f->frame);
        /* The modelled close request's flag dies with the frame it names, at the free rather than at the tail: the
           report install below returns, so a flag cleared only at the bottom would stand over the report's frame,
           whose completion would be read as §6.10.1's step 9 answer. In dev an abruptly-completing task aborts
           above; in release that DFAIL is compiled out, the build where a standing flag has nothing to say so. Its
           value was already read by the completion arm above. */
        f->close_req = 0;
        /* §8.1.4.4 step 8.3.1 takes the slot step 8's program vacated, and the cursor does not move: the report is
           the rest of this row's run a classic script, so the row is advanced once, at the report's completion.
           Everything the `if (!f->frame)` block above would do, step 8.3.2's microtask checkpoint first, is skipped
           while this frame lives, the order §8.1.4.4 states and the reason the report is a frame. */
        if (report) { f->frame = report; f->reporting = 1; return 0; }
        /* This script done, next: only when the frame that finished is this row's work, by the restore's predicate,
           read before `f->reporting` is cleared. The cursor names a row and a call is not one. A driven orphan's
           drive is assembled with its parent's cursor at `dyn_n` (the parent ran out of rows, which is why the
           orphan arm was reached), so advancing for its call would put the cursor at `dyn_n + 1`, one slot ahead
           of every append: the next row anything queues (a lazy chunk the driven function loads, a `<script>` it
           appends, a cross-agent operation engine_perform appends to every live timeline) is written behind the
           cursor and never compiled, and flow_programs_unstarted_for_document (which selects `k >= script_i`)
           answers 0 for it, so HTML §7.5.10 "Destroying documents" step 5 reads zero for an unstarted row. An arm
           forked inside a module body (FLOW_BASE_ASYNC_CALL) is excluded for the same reason: the module arm
           already left its row, so advancing would leave it a second time, which ENGINE_LEAVE_ROW's
           `script_i < dyn_n` catches where the cursor is at the end and which skips the next row in silence
           where it is not. */
        if (f->reporting || is_program) {
            ENGINE_LEAVE_ROW(f);
        }
        f->reporting = 0;
        f->frame = NULL;
        return 0;
    }
}

/* Context switches performed by the dispatch loop, for the result document (result.h). Cumulative for the
   life of this engine — one wasm instance is one document, so that is the document's count. */
static int g_switches = 0;
int engine_switch_count(void) { return g_switches; }

/* The four ladder rows in one read (see their declaration). One call rather than four accessors, because the assert
   below is between three counters and the histogram, and four entries would be four instants. Nothing is reset with
   a session, for `g_step_unit_runs`' reason: the host wants the instance's ladder. */
void engine_ladder_task_census(EngineLadderTaskCensus *out)
{
    DCHECK(out != NULL, "the ladder's task-arm census was asked for into nothing — a reading that lands nowhere "
                        "is a reading whose caller cannot have taken it");
    out->task_held_deliv = g_task_held_deliv;
    out->task_held_seq   = g_task_held_seq;
    out->task_arm_older  = g_task_arm_older;
    out->task_arm_no_row = g_task_arm_no_row;
    /* The task arm's step count, partitioned by the reason the arm was reached. Not a tautology: the halves are
       raised inside the arm at the choice and the total at the convergence point after flow_step returns, so a
       second assignment of STEP_UNIT_RUN_TASK, or a return added between the choice and that point, breaks it. */
    DCHECKF(out->task_arm_older + out->task_arm_no_row == g_step_unit_runs[STEP_UNIT_RUN_TASK],
            "the task arm ran %ld times and its two reasons account for %ld — the arm is reached either because "
            "the queued task is strictly OLDER than the row at this flow's cursor or because there is NO row to "
            "compare against, so a step the histogram counted and neither reason claims is a second writer of "
            "STEP_UNIT_RUN_TASK or a return added between the choice that raises these and the point that "
            "records the arm",
            g_step_unit_runs[STEP_UNIT_RUN_TASK], out->task_arm_older + out->task_arm_no_row);
}

/* The ladder's own traffic, copied out whole (solver/engine.h has what the rows mean and how they differ from the
   `@COLD` histogram). Copied rather than pointed at, as EngineFrontierCensus is filled: a consumer composes a reading
   of an instant, and every row is taken in this one call so each quotient and identity below compares one instant.
   The step conservation is not asserted here: it is exact only between steps, and a host may call this wherever its
   thread is; it is asserted at engine_sched_slice's convergence point, where the step that broke it has just
   returned. */
void engine_step_unit_runs(EngineStepUnitRuns *out)
{
    int i;
    /* The one clock reading this accessor takes: `instance_us` and the open tail of `between_slices_us` are two
       uses of one instant, and two readings would make the halves exceed the total by whatever ran between them. */
    int64_t now = 0;

    DCHECK(out != NULL, "the lifetime step histogram was asked for into nothing — a reading that lands nowhere "
                        "is a reading whose caller cannot have taken it");
    out->steps = g_steps;
    out->unframed_steps = g_unframed_steps;
    /* And what they cost, in the same reading as the count they are divided by. */
    out->step_us = g_step_us;
    out->slice_us = g_slice_us;
    out->sched_us = g_sched_us;
    out->slice_overruns = g_slice_over;
    /* The span those four are a share of, read here and nowhere else. The clock is read rather than accumulated,
       because the quantity is a span, not a sum of charges; `g_instance_us0` is its opening. Zero before the first
       slice is a real answer, with `steps` 0 beside it. One clock read per census, not per turn:
       `quantum_thread_us` crosses into the embedder on the shipped host, so frequency is the price. */
    if (g_instance_us0_set) now = quantum_thread_us();
    out->instance_us = g_instance_us0_set ? now - g_instance_us0 : 0;
    /* And its two halves (solver/engine.h's `loop_us`). `g_loop_us` is closed at each slice's return and copied;
       the host's half has an open interval now (the span since the last slice returned), closed here from the
       reading `instance_us` closes on, which makes the two a partition. It is closed into the copy, never into
       `g_between_us`, which would charge that span again at the next census. */
    /* The open tail is the host's only while no slice is open: composed from inside the dispatch loop, this census
       would charge the running slice's elapsed time to the host's row, a wrong attribution no arithmetic check can
       catch (the halves still telescope). It holds by construction (solver/quantum.h: the shipped ABI never returns
       to the host holding a slice, asserted at main.c's qjs_step, and both callers of engine_census_emit stand
       after engine_sched_step returns) and is asserted for the next caller. `quantum_slice_open()` is for
       assertions only; a branch on it would choose between a scheduled and an unscheduled path. */
    DCHECK(!quantum_slice_open(),
           "the step census was composed from INSIDE an open slice — `betweenSlicesUs` closes its open tail "
           "from the last slice's RETURN, so the running slice's own elapsed time is about to be charged to "
           "the HOST's half of the partition. The sum still equals `instanceUs`, which is why nothing else "
           "here can catch it, and the row would report thread the engine was holding as thread it was "
           "waiting for — the exact reading the split exists to make");
    out->loop_us           = g_loop_us;
    out->between_slices_us = g_between_us + (g_slice_exit_set ? now - g_slice_exit_us : 0);
    out->slices            = g_slices;
    /* The containment, an identity rather than an expectation: `g_instance_us0` is opened at engine_sched_slice's
       entry from the reading the first turn's `t0` is carried from, and every charge in `g_step_us` is a
       sub-interval closed before this line reads the clock. A total above its span is a baseline taken after a
       turn was charged, a second writer, or a measure no longer monotone, and would publish `stepUs/instanceUs`
       above 1. */
    DCHECKF(out->step_us <= out->instance_us,
            "solver/engine.c: the dispatch loop's total %lld exceeds the %lld the instance has measured since "
            "its first slice — every turn's charge is a sub-interval of that span, so this is a baseline taken "
            "after a turn was charged or a measure that is no longer monotone, and `stepUs/instanceUs` is "
            "about to be published as a share above 1",
            (long long)out->step_us, (long long)out->instance_us);
    /* The partition of that span (solver/engine.h's `loop_us` has why these are two rows). The readings telescope:
       the wrapper's entry reading opens the baseline and each slice's closing reading is the next gap's opening, so
       their sum plus the open tail is `now - g_instance_us0` by construction. A difference is a slice whose span
       was never charged (an exit that escaped the wrapper), a second writer of an accumulator, or a non-monotone
       measure. */
    DCHECKF(out->loop_us + out->between_slices_us == out->instance_us,
            "solver/engine.c: the instance's span is not partitioned by its two halves (%lld inside the slice "
            "bracket + %lld between slices against %lld measured since the first slice) — the readings "
            "telescope from engine_sched_step's own pair, so a difference is a slice whose span was never "
            "charged, a second writer of one accumulator, or a clock that is no longer monotone",
            (long long)out->loop_us, (long long)out->between_slices_us, (long long)out->instance_us);
    /* Every turn's charge is taken inside the dispatch loop, inside the wrapper's bracket, so the turns are a
       sub-interval of their slices; a violation is a turn charged outside a slice (a second driver of flow_step, or
       a charge surviving `quantum_end()`). */
    DCHECKF(out->step_us <= out->loop_us,
            "solver/engine.c: the dispatch turns cost %lld against %lld spent inside the slice bracket they "
            "run in — every turn is charged between engine_sched_step's own two readings, so a total that "
            "exceeds them is a turn charged outside a slice entirely",
            (long long)out->step_us, (long long)out->loop_us);
    /* A turn implies a slice: flow_step's only caller is the dispatch loop and the loop's only caller is the body
       this wrapper brackets, so steps without slices is a step driven from outside the bracket, and every
       per-slice quotient would be a fraction of the wrong population. */
    DCHECKF(out->steps == 0 || out->slices > 0,
            "solver/engine.c: %ld dispatch turn(s) were taken across ZERO slices — the count is raised in "
            "engine_sched_step before the body runs and a turn can only be taken inside that body, so this is "
            "a step driven from outside the slice bracket and every per-slice reading on this census is a "
            "fraction of a population that did not produce it",
            out->steps);
    /* The compile phase, read with the overrun total it is contained in. */
    out->classic_compiles         = g_classic_compiles;
    out->classic_compile_overruns = g_classic_compile_over;
    /* The repeat partition, read with the total it partitions; all four are raised on one line. */
    out->classic_compile_again       = g_classic_compile_again;
    out->classic_compile_again_bytes = g_classic_compile_again_bytes;
    out->classic_compile_own_decode  = g_classic_compile_own_decode;
    /* The shared-parse row: `classic_compiles + classic_parse_shared` is what a reader compares against a
       pre-sharing revision's compile count. */
    out->classic_parse_shared        = g_classic_parse_shared;
    /* The stint partition. A reader wants `arms[STEP_UNIT_COMPILE_YIELDED] - classic_compile_resumed`, the parses
       currently suspended (see g_classic_compile_resumed), a small number, so a skew of one stint between the two
       copies would be the whole of it; both are taken here, before the loop that copies the histogram. */
    out->classic_compile_resumed     = g_classic_compile_resumed;
    for (i = 0; i < STEP_UNIT_N; i++) out->over_arms[i] = g_step_unit_over[i];
    for (i = 0; i < STEP_UNIT_N; i++) out->over_seamless_arms[i] = g_step_unit_over_seamless[i];
    for (i = 0; i < STEP_UNIT_N; i++) out->over_ask_arms[i] = g_step_unit_over_asks[i];
    for (i = 0; i < STEP_UNIT_N; i++) out->over_gap_arms[i] = g_step_unit_over_gap[i];
    /* The arm histogram, copied here rather than at the tail because the compile containment below reads
       `arms[STEP_UNIT_COMPILE_YIELDED]`, the other half of the stint population `classic_compile_overruns` is
       drawn from. */
    for (i = 0; i < STEP_UNIT_N; i++) out->arms[i] = g_step_unit_runs[i];
    /* The clock boundary's three arrival counts, read with the histogram the identities below compare them to. */
    out->clock_render_asks = g_clock_render_asks;
    out->clock_timer_asks  = g_clock_timer_asks;
    out->clock_idle_asks   = g_clock_idle_asks;
    /* Whether the overrunning turns offered a suspend point; both are raised inside the overrun branch. */
    out->slice_overrun_asks     = g_over_asks;
    out->slice_overrun_seamless = g_over_seamless;
    /* The worst no-offer stretch. It is a maximum, not a partition, so its only identity (`max(arms) == scalar`)
       is exact at the raise and is not asked again here. */
    out->slice_overrun_gap_us   = g_over_gap_us;
    /* The unit boundary's refusal arms, read with the `steps` they partition. */
    out->unit_mid_program     = g_unit_mid_program;
    out->unit_parked          = g_unit_parked;
    out->unit_checkpoint_owed = g_unit_checkpoint_owed;
    /* A count of turns cannot exceed the turns: a subset larger than its population is the free arithmetic tell,
       and a fraction whose numerator and denominator advance at different events is unbounded and reads as a rate. */
    DCHECKF(out->slice_overruns <= out->steps,
            "solver/engine.c: slice_overruns %lld exceeds steps %lld — the overrun count is raised once per "
            "dispatch turn from the same two clock readings the slice arm is accumulated from, so it can only "
            "outrun its denominator if one of the two moved without the other",
            (long long)out->slice_overruns, (long long)out->steps);
    /* The compile phase's containment, against the population the numerator is drawn from. The overrun is raised
       per stint, and every stint ends in exactly one of two arms of one straight-line block: parked (naming the
       step `compile-handed-the-thread-back`) or finished (raising `classic_compiles`), so their sum is the stint
       population for the life of the process (see g_classic_compiles). A violation is a stint that raised the
       overrun and ended in neither: a third exit from the block, or a yielded stint whose step name was
       overwritten before flow_step returned. Against `classic_compiles` alone it would fail by construction,
       since a program parsed over several overrunning stints outruns its own count. */
    DCHECKF(out->classic_compile_overruns
                <= out->classic_compiles + out->arms[STEP_UNIT_COMPILE_YIELDED],
            "solver/engine.c: classic_compile_overruns %ld exceeds the %ld compile stint(s) it is drawn from "
            "(%ld program(s) whose parse finished + %ld stint(s) that handed the thread back) — the overrun "
            "is raised once per STINT and every stint ends in exactly one of those two arms of one "
            "straight-line block, so a subset larger than their sum is a third exit from the compile block, "
            "or a yielded stint whose step name was overwritten before flow_step returned",
            out->classic_compile_overruns,
            out->classic_compiles + out->arms[STEP_UNIT_COMPILE_YIELDED],
            out->classic_compiles, out->arms[STEP_UNIT_COMPILE_YIELDED]);
    /* The stint partition's containment, an exact accounting identity. A parse's stints are one begin and k
       resumes; every resume follows a park of that parse and every park is followed by at most one resume, so
       `resumed <= yielded`, the slack being exactly the parses begun and not ended (g_classic_compile_resumed
       derives it). A violation is a stint continuing a parse this process never parked: `f->compile` made
       non-NULL by a route other than a park arm, the third-site defect JS_FlowCompileStep's suspend assert names
       from the other side. Both rows are copied at one instant above. It stays exact with the module arm parking
       into the same field, because the two park sites name different rows (`compile-handed-the-thread-back`,
       `module-compile-handed-the-thread-back`); one row for both would keep this inequality true and make
       g_classic_compiles's stint equality false (solver/step_unit.h). */
    DCHECKF(out->classic_compile_resumed <= out->arms[STEP_UNIT_COMPILE_YIELDED],
            "solver/engine.c: classic_compile_resumed %ld exceeds the %ld stint(s) that handed a CLASSIC parse "
            "back — a stint may only CONTINUE a parse this process PARKED, and the classic block's park arm is "
            "the only site that leaves a carrier in `f->compile` under THIS row (the module arm parks into the "
            "same field under its own row), so more resumes than parks is a carrier installed by a third site "
            "and a parse being continued that nothing here suspended",
            out->classic_compile_resumed, out->arms[STEP_UNIT_COMPILE_YIELDED]);
    /* The cross-row containment, tying the phase to its turn. A compile stint that met the slice sits inside a step
       at least as long, and the step's reading brackets flow_step from outside (one caller; the slice accounting
       after it is unconditional, with no `continue`, `break`, `return` or `goto` between), so every compile
       overrun is a slice overrun. The numerator is per stint, so this also needs a turn to hold at most one stint,
       which it does: flow_step's `continue`s are the microtask and task arms, both above the script row, and every
       exit at or past the compile returns. A `continue` below the script row would break this with nothing else
       changing. */
    DCHECKF(out->classic_compile_overruns <= out->slice_overruns,
            "solver/engine.c: classic_compile_overruns %ld exceeds slice_overruns %lld — a compile stint that "
            "alone met the slice is inside a step that therefore met it too, and a turn reaches the compile "
            "at most once, so this says the step containing it was not counted: flow_step has gained a caller "
            "outside the dispatch loop's clock bracket, an arm that returns before the slice accounting, or a "
            "`continue` below the script row that lets one turn take two compile stints",
            out->classic_compile_overruns, (long long)out->slice_overruns);
    /* The repeat partition's two containments: two independent partitions of one population (a parse can be in
       both), so two checks rather than one sum. Both subsets are raised in the same straight-line block as their
       total, with no branch between, so a violation is a raise moved out from under `g_classic_compiles` or behind
       a condition. */
    DCHECKF(out->classic_compile_again <= out->classic_compiles,
            "solver/engine.c: classic_compile_again %ld exceeds classic_compiles %ld — the repeat count is "
            "raised in the same straight-line block as the total it is a subset of, with no branch between, so "
            "a subset larger than its population means one of the two has been moved: a parse marked at the "
            "stint entry rather than at the stint that FINISHES it would count the second stint of one program "
            "as a repeat of its own first",
            out->classic_compile_again, out->classic_compiles);
    DCHECKF(out->classic_compile_own_decode <= out->classic_compiles,
            "solver/engine.c: classic_compile_own_decode %ld exceeds classic_compiles %ld — the provenance "
            "count is raised in the same straight-line block as the total it is a subset of, so this is that "
            "raise having been moved out from under it or behind a condition",
            out->classic_compile_own_decode, out->classic_compiles);
    /* The shared-parse row's implication. A closure is held only by a body whose parse finished
       (solver/dyn_body.c asserts the mark and the hold are one event), so a share before any parse is a program
       obtained from a parse this process never performed. An implication, not a bound: one parse can be shared
       any number of times. */
    DCHECKF(out->classic_parse_shared == 0 || out->classic_compiles > 0,
            "solver/engine.c: %ld classic program(s) were started from a shared parse and NOT ONE parse "
            "finished — a body holds a closure only at the stint that finished parsing its bytes, so a share "
            "with no compile behind it is a program obtained from a parse this process never performed",
            out->classic_parse_shared);
    /* The gauge against the lifetime event that raises it: a body holds at most one parse, and only one it was
       marked for, so the bodies holding one now cannot exceed the parses ever finished. Comparing a gauge with a
       lifetime total is sound in this direction only (solver/dyn_body.h's `dyn_body_parses_held` states its
       kind). */
    DCHECKF(dyn_body_parses_held() <= out->classic_compiles,
            "solver/engine.c: %ld program text(s) are holding a parse against %ld parse(s) ever finished — a "
            "body is marked by the stint that finishes its parse and holds at most one closure, so more held "
            "parses than finished ones is a closure held by a body nothing here compiled",
            dyn_body_parses_held(), out->classic_compiles);
    /* The byte total against its count, which catches the overflow its type is chosen against
       (g_classic_compile_again_bytes): a body is at least one byte and the accumulator only climbs, so a total
       below its count, or negative, is that narrowing. */
    DCHECKF(out->classic_compile_again_bytes >= (int64_t)out->classic_compile_again,
            "solver/engine.c: classic_compile_again_bytes %lld is below the %ld re-parse(s) it sums the "
            "lengths of — every body is at least one byte and this accumulator only climbs, so the total has "
            "either overflowed a narrowed type or is being raised for parses the count does not cover",
            (long long)out->classic_compile_again_bytes, out->classic_compile_again);
    /* The two overrun-path rows' equivalence (solver/engine.h's `slice_overrun_asks`): a turn adds to the sum
       exactly when it is not counted seamless, by the order of two statements in one branch, so an empty sum and a
       wholly seamless population are one fact. */
    DCHECKF((out->slice_overrun_asks == 0) == (out->slice_overrun_seamless == out->slice_overruns),
            "solver/engine.c: the overrunning turns offered %llu suspend point(s) with %ld of %lld of those "
            "turns offering none — the sum is raised on exactly the turns the seamless count is not, so these "
            "two can only disagree if one of them has a second raise site or the seamless count is being "
            "raised for a turn the overrun total never counted",
            (unsigned long long)out->slice_overrun_asks, out->slice_overrun_seamless,
            (long long)out->slice_overruns);
    /* And its containment in the lifetime total it is a subset of (the @WFQ line's `preemptAsksLifetime`): every
       consultation charged here happened inside a step. A violation is the sum accumulated across a bracket wider
       than the turn. */
    DCHECKF(out->slice_overrun_asks <= engine_preempt_asks(),
            "solver/engine.c: the overrunning turns alone account for %llu consultations of the preempt policy "
            "against %llu in the whole instance — the sum is a per-step delta of that same counter, so a "
            "subset larger than its population is a bracket that spans more than one step",
            (unsigned long long)out->slice_overrun_asks, (unsigned long long)engine_preempt_asks());
    /* The turn's phases partition its cost (see g_slice_us). Both arms are added in the iteration the charge is
       taken in, from the same two readings, so a difference is a third phase with no arm. */
    DCHECKF(g_slice_us + g_sched_us == g_step_us,
            "the dispatch turn's phases do not partition its cost (%lld slice + %lld scheduler against %lld "
            "step) — both arms are written inside the iteration the charge is taken in and from the same two "
            "clock readings, so a difference is a phase of the turn that has no arm and a reading that has "
            "stopped being about the whole turn",
            (long long)g_slice_us, (long long)g_sched_us, (long long)g_step_us);
    /* The orphan ask is a subset of the ladder descents. `g_orphan_asks` is raised inside engine_orphan_seed,
       which flow_step reaches only from inside the `if (!f->frame)` block the descents are counted at, both per
       pass, so an ask with no descent is the seed reached from elsewhere (a second caller, or the rung hoisted out
       of the block). A floor across a restart, not an equality: engine_session_close releases `g_orphan_asks` with
       the agent and nothing here, so a second session starts the ask at 0 against a climbing descent count. */
    DCHECKF(g_orphan_asks <= g_unframed_steps,
            "solver/engine.c: the orphan surface was asked %ld time(s) against %ld descent(s) of the ladder "
            "it is a rung of — the ask is raised inside engine_orphan_seed, which is reached only from within "
            "flow_step's `if (!f->frame)` block, and both are counted per pass through that block, so an ask "
            "outside that containment is a second path to the seed rather than a fact about the frontier",
            g_orphan_asks, g_unframed_steps);
    /* And the ask's exits partition it (see the rows' declaration). engine_orphan_seed raises the total at its
       entry past the forking gate and exactly one of the three at each exit, so an ask outside the sum is an exit
       that records nothing: an early return between the ask and the take, a second caller reaching the walk, or a
       new memo arm. That is how the residue a reader computes against solver/step_unit.h's two orphan arms could
       widen silently. An equality, unlike the floor above, because all four are released together at
       engine_session_close. */
    DCHECKF(g_orphan_asks == g_orphan_asks_memo + g_orphan_asks_empty + g_orphan_asks_took,
            "solver/engine.c: the orphan surface was asked %ld time(s) and its exits account for %ld (%ld "
            "memo, %ld empty walk, %ld take) — the total is raised at engine_orphan_seed's entry past the "
            "forking gate and each of the three at exactly one of its exits, so a difference is an exit that "
            "records nothing, and the residue a reader computes against `seed-one-orphan-flow` and "
            "`hand-a-parked-drive-its-function` has silently stopped being the memo and the empty walk",
            g_orphan_asks, g_orphan_asks_memo + g_orphan_asks_empty + g_orphan_asks_took,
            g_orphan_asks_memo, g_orphan_asks_empty, g_orphan_asks_took);
    /* The clock boundary's chain, which makes "how many dispatches reached the clock" a reading off this
     * histogram. The three rungs are consecutive arms of one `else if` chain, so a dispatch reaching the timer arm
     * is one that reached the rendering arm and was not taken there, and likewise one rung down. The asks are
     * raised inside flow_step at the arm and the outcome totals at the convergence point after it returns, so it
     * is a real check, broken by an arm or a `return` inserted between two rungs, a reorder, or a second writer of
     * STEP_UNIT_RENDERING or STEP_UNIT_TIMER, each of which changes what a zero on `queue-rendering-opportunity`
     * means.
     *
     * The eleven units at or below the boundary (RENDERING, TIMER, IDLE_PERIOD, ORPHAN_SEED, ORPHAN_ROUTE,
     * AWAIT_FETCH_RECORD, AWAIT_OWED_REPLY, AWAIT_DECLINED, CLOSE_REQUEST, AWAIT_PEER, FINISHED) are each written at
     * one site at or below it, so their sum is the dispatches that descended this far, which `g_clock_render_asks`
     * counts at the arm; the third assert below is that agreement. `finished` at zero with the sum at zero says
     * the retirement arm was never asked (a fact about the arms above); with the sum large, it was asked and
     * something below declined.
     * Named residual. Not covered: the eleven-unit list is written here by hand, not derived from the ladder.
     * Next diff builds: a marker on solver/step_unit.h's list naming which side of the boundary each arm is written
     * on, from which the sum is derived. Absence shows as: the boundary-sum assert firing after a rung moves across
     * the clock on purpose, until this list is edited to match. */
    DCHECKF(g_clock_timer_asks == g_clock_render_asks - g_step_unit_runs[STEP_UNIT_RENDERING],
            "the rendering rung was reached %ld times and took %ld, so the timer rung one arm below it must "
            "have been reached %ld times and was reached %ld — the two rungs are consecutive arms of one chain, "
            "so a difference is an arm or a return added between them, a reorder, or a second writer of "
            "STEP_UNIT_RENDERING",
            g_clock_render_asks, g_step_unit_runs[STEP_UNIT_RENDERING],
            g_clock_render_asks - g_step_unit_runs[STEP_UNIT_RENDERING], g_clock_timer_asks);
    DCHECKF(g_clock_idle_asks == g_clock_timer_asks - g_step_unit_runs[STEP_UNIT_TIMER],
            "the timer rung was reached %ld times and took %ld, so the idle rung one arm below it must have "
            "been reached %ld times and was reached %ld — the two rungs are consecutive arms of one chain, so a "
            "difference is an arm or a return added between them, a reorder, or a second writer of "
            "STEP_UNIT_TIMER",
            g_clock_timer_asks, g_step_unit_runs[STEP_UNIT_TIMER],
            g_clock_timer_asks - g_step_unit_runs[STEP_UNIT_TIMER], g_clock_idle_asks);
    /* The sum the banner licenses, the one check that sees a rung cross the boundary; the two asserts above pin
       the triple's internal adjacency and are blind to anything moving past all three. A unit that descends
       without joining the list reads the sum too small, one that rises without leaving it too large, a unit
       written at a second site above the boundary too large, a rung asked twice per step too small. One sum of
       eleven rows rather than eleven containments, because the claim is the partition. ORPHAN_RESUME is absent:
       engine_orphan_seed returns SEEDED or ROUTED, never RESUMED, so the third outcome of `engine_orphan_unit`
       belongs to the rung above the boundary. */
    {
        long below = g_step_unit_runs[STEP_UNIT_RENDERING] + g_step_unit_runs[STEP_UNIT_TIMER] +
                     g_step_unit_runs[STEP_UNIT_IDLE_PERIOD] + g_step_unit_runs[STEP_UNIT_ORPHAN_SEED] +
                     g_step_unit_runs[STEP_UNIT_ORPHAN_ROUTE] + g_step_unit_runs[STEP_UNIT_AWAIT_FETCH_RECORD] +
                     g_step_unit_runs[STEP_UNIT_AWAIT_OWED_REPLY] + g_step_unit_runs[STEP_UNIT_AWAIT_DECLINED] +
                     g_step_unit_runs[STEP_UNIT_CLOSE_REQUEST] + g_step_unit_runs[STEP_UNIT_AWAIT_PEER] +
                     g_step_unit_runs[STEP_UNIT_FINISHED];
        DCHECKF(below == g_clock_render_asks,
                "the clock boundary was reached %ld times and the eleven arms at or below it account for %ld — "
                "every dispatch that arrives at the rendering rung leaves through exactly one of them, so a "
                "difference is a rung that crossed this boundary without joining or leaving solver/engine.c's "
                "eleven-unit list, a unit written at a second site on the other side of it, or the rendering "
                "rung being asked more than once per step. The two asserts above cannot see any of those: they "
                "pin RENDERING/TIMER/IDLE_PERIOD's adjacency to each other and say nothing about what stands "
                "below all three",
                g_clock_render_asks, below);
    }
    /* The clock chain's containment in the ladder descents, which a reader comparing
       `stepReachedRenderingLife` with `unframedStepsLifetime` on one census line depends on. The same argument as
       `g_orphan_asks <= g_unframed_steps`, one rung down: both are raised per pass through the `if (!f->frame)`
       block, the rendering rung an `else if` inside it. A floor, and the gap is the reading: every arm above the
       rendering rung that takes a descent never reaches the chain, so descents with all three asks at 0 say the
       ladder stopped above the clock (read off `arms`). Both are per-instance and released by nothing, so a
       restart does not weaken it. */
    DCHECKF(g_clock_render_asks <= g_unframed_steps,
            "solver/engine.c: the clock boundary was reached %ld time(s) against %ld descent(s) of the work "
            "ladder — the rendering rung is an `else if` inside the `if (!f->frame)` block the descents are "
            "counted at and both are raised per PASS through it, so an ask outside that population is the "
            "chain having been reached from outside the block: a second caller, or the triple hoisted out of "
            "it. Every reading that compares `stepReachedRenderingLife` with `unframedStepsLifetime` rests on "
            "this containment",
            g_clock_render_asks, g_unframed_steps);
}

/* Two facts the scheduler publishes in the frontier census.
 *
 * `g_finished`: how many flows have ever reached flow_step's finished exit. `live == flows` cannot answer it: the
 * frontier's current size and the number ever created are equal whenever creations and finishes balance, and also
 * when nothing has finished. Its companion `framed / live` is a gauge that decays: it reads 1.000 at the first census
 * of every run, healthy or broken, because a flow that has just started holds a frame, so it is compared only at a
 * matched census index between runs that both reached it, as an indicator rather than a test one run passes. Read
 * `framed`, `live` (older logs spell it `flows`) and `finished` from one `@COLD` line. `domHeadEntries` does not
 * separate the populations: it is a sum over every live flow (cold.c's `out->dom_head_entries += f->dom_n`), not
 * the running flow's delta, and its quotient by `live` separates only through the denominator.
 *
 * The counter is a lifetime total of one instance, reset by nothing (the only writes are the `++` pair in
 * flow_finish), so it restarts at 0 in the next process. A build log concatenates several processes' censuses with
 * nothing on the `@COLD` line saying so: every lifetime row decreases at a process boundary, and `tail -1` selects
 * the stage least likely to have finished a flow (a stage killed at its rlimit runs last and longest; a stage that
 * drains ends early). Split the file at every decrease before reading any row of it.
 * Named residual. Not covered: the `@COLD` line does not name the instance that composed it. Next diff builds: an
 * instance ordinal on the line. Absence shows as: a lifetime row of `@COLD` decreasing within one log file.
 *
 * `g_deepest`: the highest program index this document has compiled, across every flow. The progress line's
 * `script` is the current flow's cursor and says nothing about coverage: `script: 1` for ever is either no flow
 * reaching the second <script> or thousands having done so while the flow holding the thread is at its first. */

/* The work this engine has performed: forks taken, flows created, jobs run, context switches. One definition for
 * three consumers: the seam verdict below measures one step's share of it (being descheduled cannot inflate any of
 * the four, since a thread that is not running performs none of them); the progress stream's cadence keys on it (a
 * cadence keyed on `g_switches` alone goes silent while one flow holds the thread, because nothing outranks it);
 * and a host reporting on its own run decides when to report from it (engine.h).
 *
 * The four are not one currency, so this total over a clock measures composition, never a rate: a turn that ends
 * a frame or drains microtasks costs orders of magnitude less than one that runs a program, so a run that mints
 * jobs scores far more work for the same thread time. Read the four addends, which the verdict line and `@HWORK`
 * print separately; the sum is right for a cadence, which wants a quantity that always moves.
 * Named residual. Not covered: what a turn of each kind costs. Next diff builds: a census row per turn kind with
 * its cost. Absence shows as: `workDone` per CPU-second splitting runs at one rlimit into two bands while the
 * count of program turns overlaps between them. */
long engine_work_done(void) {
    return decide_fork_total() + flow_created_count() + g_jobs_run + g_switches;
}

static void flow_switch_out(JSContext *ctx, Flow *f) {   /* pause f: snapshot its solver state, restore baseline */
    /* The parked continuations travel with the flow, as the delta does: each resumes a suspended async activation
       of this flow under this flow's heap, and left in the runtime it would be resumed by whichever flow ran next,
       against the wrong delta. The whole set moves, since a step reaches as many bases as it reaches. */
    f->parked = JS_TakeParkedFlows(JS_GetRuntime(ctx));
    f->dec_blob = decide_suspend();
    f->pin_blob = concolic_pins_suspend();
    cow_unapply(ctx, (CowDelta *)f->delta);
    cow_set_current(NULL);
    dom_unapply();                                  /* DOM twin of cow_unapply: restore the baseline document */
    f->dom = dom_buf_take(&f->dom_n, &f->dom_cap);  /* detach this flow's DOM head so the global is empty for the next flow */
    f->dom_base = dom_base_take();                  /* ...and its shared base chain (NULL until a DOM fork) */
    flow_set_running(NULL);
}

static void flow_switch_in(JSContext *ctx, Flow *f) {   /* resume/start f: apply its delta + solver state */
    JS_PutParkedFlows(JS_GetRuntime(ctx), f->parked);
    f->parked = NULL;
    /* Nothing the interpreter has counted so far is this flow's: the retired-opcode count moves the event loop's
       clock (quickjs.h's JSFlowControlHooks.work), and what stands in it here is host time between two flows or the
       residue of a flow that finished without parking, never `f`'s. Banked, it would put another timeline's
       duration into this flow's first `performance.now()`. */
    JS_FlowDiscardRetiredWork();
    if (!f->delta) f->delta = cow_delta_new();
    cow_set_current((CowDelta *)f->delta);
    cow_apply(ctx, (CowDelta *)f->delta);
    dom_buf_load(f->dom, f->dom_n, f->dom_cap);   /* attach this flow's DOM head (NULL/0 for a fresh flow = empty) */
    dom_base_load(f->dom_base);                   /* ...and its base chain, BEFORE dom_apply walks it */
    dom_apply();                                  /* DOM twin of cow_apply: replay this flow's document writes */
    /* A fresh flow carries no recorded path, asserted at the line that would drop one. The branches below are told
       apart by `started` alone, so a flow whose installer set `dec_blob` and left the bit clear takes decide_enter
       and never reads its path, and at its first suspend `f->dec_blob = decide_suspend()` overwrites and leaks the
       blob with its reference on the frozen segment, keeping the prefix under it alive. A flow on a recorded path
       is installed as a triple (`started`, `dec_blob`, `pin_blob`) by cold.c's 'f' and 'c' arms and by solve.c's
       @S re-injection. */
    DCHECK(f->started || f->dec_blob == NULL,
           "a flow that has never run carries a recorded decision path — decide_enter is about to ignore it "
           "and the flow's first suspend will overwrite the pointer, so the path is dropped AND its segment "
           "reference leaked. Whatever installed it set `dec_blob` without `started`");
    /* A fresh flow replays nothing: decide_enter sets `g_dec_n = 0`, emptying the vector rather than rewinding it,
       and the assert above forbids a never-run flow a recorded path. The other arm installs a recorded path, and a
       forked sibling takes it, because engine_sibling_assemble sets `started`. Installing the path does not
       re-execute the parent's prefix: engine_sibling_assemble refuses a fork inside a program without a frame
       snapshot, so a sibling resumes where its parent stands over an O(1) shared base segment (solver/flow.c
       states the same). */
    if (!f->started) { f->started = 1; decide_enter(ctx, f); }   /* fresh flow: an empty vector, nothing recorded */
    else {                                                        /* paused flow OR forked sibling: install its path */
        decide_resume(f->dec_blob, f->fn);   decide_blob_free(f->dec_blob); f->dec_blob = NULL;
        concolic_pins_resume(f->pin_blob);   concolic_pins_blob_free(f->pin_blob); f->pin_blob = NULL;
    }
    flow_set_running(f);
    /* What this flow was ranked on when it took the thread: the pick compared it against every runnable member and
       found none strictly better, so from here the value yield may fire only if one of these moves. The hook's
       assertion reads them; nothing decides from them. */
    g_ranked_gen = flow_frontier_gen(); g_ranked_val = flow_reward(f);   /* the reward term, the family's */
    g_ranked_silence = flow_silence_notch(f);  /* the aging term — see the assertion in preempt_hook */
    g_ranked_visits = f->visits;               /* …and the optimism term's, which is a count and not a clock */
    g_ranked_dist = flow_distance(f);          /* …and the fitness term's, which is a reading and not a payment */
}

static void flow_finish(JSContext *ctx, Flow *f) {   /* f completed: tear down its interleaving state + remove */
    g_finished++;   /* the one place a flow completes; see g_finished */
    /* Which of the frontier's two populations retired, read from the label at the instant of retirement:
       `cand_src` is live here (flow_release below frees it) and is the whole question (solver/engine.h). An
       exploration flow finishing is coverage; a candidate member finishing is search spent on a payload that did
       not fire. Counted per member, never per session: engine_sibling_assemble copies `cand_src` to every
       sibling, so one seed's N arms each arrive here. The emitted key reads `finishedCands`; engine.h says why,
       what to read it against, and why no inequality against a seed count may be asserted. */
    if (f->cand_src) g_finished_cands++; else g_finished_flows++;
    /* "all scripts, chunks, jobs and fetches are done" cannot be true with a continuation still parked — the
       loop above resumes one before it can answer that. Asserting it here is what keeps the park inside the
       no-work-item-is-ever-dropped rule rather than merely intending to. */
    DCHECK(!JS_HasParkedFlow(JS_GetRuntime(ctx)) && f->parked == NULL,
           "a flow finished with a continuation still parked — that flow's async activation is dropped");
    /* A finished flow has no live frame and no program still being parsed. `frame` is the JS_FlowNew handle
       holding this flow's heap frame chain (every activation, closure and local it is suspended across), so one
       left at finish retains the whole execution graph, which the runtime's leak walk reports as thousands of
       anonymous Functions. A suspended compile holds no frame, so the frame assert alone would pass while the flow
       walked off with a half-parsed program, its descent chunks and its JSFunctionDef chain; the compile arm
       returns the moment a parse parks, so a flow here holding one means a completion arm ran while a parse was in
       flight. Asserted rather than freed, since freeing would hide that the finish path ran on a suspended flow. */
    DCHECK(f->compile == NULL,
           "a flow finished with a program still being PARSED — the row it is standing at never started, so "
           "either a completion arm ran for a row the flow had not reached or the compile handle outlived the "
           "program it belongs to");
    DCHECK(f->frame == NULL, "a flow finished with a live preemptible frame — its whole activation chain, and "
                             "everything those frames close over, is retained by a handle nothing will free");
    DCHECK(flow_deliver_pending(f) == 0,
           "a flow finished still holding routed deliveries — each is a message a peer sent and this document "
           "never received, and a page that never receives one is indistinguishable from a page that "
           "registered no handler");
    /* The same for an operation, at both ends: a record never turned into a program is a question nobody
       performed, and a token never spent is a peer's flow parked at the line that asked, for ever. */
    DCHECK(!flow_owes_answer(f),
           "a flow finished holding a cross-agent operation — either the record was never performed, or its "
           "program's completion was never sent, and either way the flow that ASKED is suspended at the line "
           "that asked it with nothing coming. Asked of the QUEUE as well as the arrival slot, because a "
           "started operation's token is on the row of the program that answers it");
    /* The job queue and the pending register are empty here, asserted rather than cleaned up: a walk that frees
       them here could run only when a work item is being dropped, and would hide the drop. flow_step decides
       "finished" only after offering the job queue a turn (the checkpoint before every program, the task arm for
       the rest) and only with the register empty (an entry with a value drains, one without reports host-owed).
       The release below frees both because an evicted flow legitimately holds them (the cold tier's recipe
       re-enqueues the reactions and re-issues the requests as it replays), so the assertion stands here, where
       "finished" is claimed. */
    DCHECK(flow_job_pending(f) == 0, "a finishing flow still held queued jobs — its promise reactions and timer callbacks "
                         "are being dropped, and the release below is what would hide that");
    DCHECK(pending_count(f->pending) == 0,
           "a finishing flow still held pending host replies — a flow awaiting one is parked, not finished, "
           "and the release below is what would hide that");
    /* A finish is a switch-out followed by a release, not its own teardown: the release (solver/flow.h) also
       serves an evicted flow, so every flow that ends exercises the eviction path. decide_leave rather than
       decide_suspend: a finishing flow's chain reference ends here and there is no blob for the release to free.
       The rest is the switch-out in its order: the heap delta unapplied and un-currented, then the document,
       whose head and base chain come back onto the flow so the release frees the live buffers rather than stale
       pointers the run may have realloc'd away from. */
    decide_leave(ctx);
    cow_unapply(ctx, (CowDelta *)f->delta); cow_set_current(NULL);
    dom_unapply();
    f->dom = dom_buf_take(&f->dom_n, &f->dom_cap);
    f->dom_base = dom_base_take();
    flow_set_running(NULL);
    flow_release(ctx, f);
}

/* One BFS scheduler: explore and @S candidate verify are the same loop, differing only in whether a concolic
   branch forks. A separate verify executor (a `while(JS_FlowResume){}` driving one candidate to completion with
   preemption off) would be a second scheduler and a drive to completion, and an unbounded candidate loop would hang
   unparked. So verify is this loop with forking off (FC_VERIFY): one concrete path, every candidate flow preemptible
   and parkable. */
/* The two asks that come with a resume point. An interpreter at an OP_if and a step driver holding a machine that
   yielded JS_STEP_FORK both clone their activation just after they ask; a C builtin calling the same symbol by name
   does not, and nothing at the seam could tell them apart. These two function pointers are the interpreter's way
   in, so `g_fork_snapshot_owed` is raised here. Not a nesting counter: neither seam runs page code, so nothing can
   ask again underneath one, which the assertion states. */
static int engine_branch_hook(JSContext *ctx, JSValueConst cond) {
    int r;
    DCHECK(!g_fork_snapshot_owed,
           "the interpreter asked for a branch arm while another snapshot-owning ask was still open — the "
           "decision seam runs no page code, so a second one means something re-entered it and the inner "
           "fork would be built against the outer ask's activation");
    g_fork_snapshot_owed = 1;
    /* No non-forking arm: this wrapper is FC_EXPLORE's `branch` and nothing else installs it, so a session that
       does not fork never reaches this line; the interpreter finds no hook, reads -1, and the ordinary ToBool
       decides the `if`. SOLVER_NO_NONFORKING_ARM states that, so a session here with forking off crashes at the
       seam naming the predicate. */
    r = solver_decide(ctx, cond, SOLVER_NO_NONFORKING_ARM);
    g_fork_snapshot_owed = 0;
    return r;
}
/* The site is forwarded, not stamped here: `solver_outcome` is the macro that captures `SOLVER_SITE_HERE`, so
   stamping in this one forwarding function would make every outcome abort name this line. The ask's own pair
   arrives as a parameter (quickjs.h's `JS_FLOW_SITE_HERE`) and goes to `solver_outcome_at`. */
static int engine_outcome_hook(JSContext *ctx, JSValueConst over, const char *op, int n, int real,
                               const char *site) {
    int r;
    DCHECK(!g_fork_snapshot_owed,
           "the step driver asked for an outcome arm while another snapshot-owning ask was still open — see "
           "engine_branch_hook");
    DCHECK(site != NULL,
           "an outcome ask arrived with no site — quickjs.h's contract says the pair is captured AT THE ASK "
           "with JS_FLOW_SITE_HERE, so a NULL here is a caller that threaded nothing and would put this "
           "engine's outcome aborts back to naming one forwarding function for every operation");
    g_fork_snapshot_owed = 1;
    r = solver_outcome_at(ctx, over, op, n, real, site);
    g_fork_snapshot_owed = 0;
    return r;
}
/* HTML §8.1.7.3 Processing model's second mover of the clock, adapted. The interpreter reports the opcodes the
   running flow retired, and the event loop's clock is what a moment on this agent's timeline means, so this is one
   line with no policy (quickjs.h's `work` and core/timing/event_loop.h's accumulate-exactly rule decide the unit,
   batching and exactness). Not guarded on a running flow: event_loop_work_advance asserts one at its origin,
   because an advance with none writes the shared baseline and moves every sibling's moment, and a guard here would
   skip that advance silently. The interpreter asks only under a live flow base; a case where that is not a running
   flow is fixed at its route. */
static void engine_work_hook(JSContext *ctx, uint64_t units) { event_loop_work_advance(ctx, units); }
/* JSFlowControlHooks.budget: is this slice spent, asked from the interpreter's dispatch every
   ENGINE_QUANTUM_ASK_EVERY dispatches so the budget can expire inside bytecode that raises nothing. It is the same
   call clause (2) of preempt_hook makes: one budget and one edge with three consumers (a flow that parks on it, a
   step that returns the thread on it, a dispatch that asks for the park to be requested). It holds no policy: the
   interpreter decides only whether to raise, and the raise is answered by preempt_hook as a back-edge's is, with
   the rank comparison in front of it, so asking earlier drops, skips or reorders nothing. Its precondition is the
   callee's: quantum_expired() aborts outside an open slice, and the gate asks only under a live flow base, so a new
   host-time entry into the interpreter reaches that abort and names itself. */
static int engine_budget_hook(void) { return quantum_expired(); }
static const JSFlowControlHooks FC_EXPLORE = { .branch = engine_branch_hook, .outcome = engine_outcome_hook,
                                               .fork = engine_fork_finalize, .preempt = preempt_hook,
                                               .budget = engine_budget_hook,
                                               .budget_period = ENGINE_QUANTUM_ASK_EVERY,
                                               .work = engine_work_hook };
static const JSFlowControlHooks FC_VERIFY  = { .preempt = preempt_hook,      /* candidate re-fire: no fork, still preemptible */
                                               .budget = engine_budget_hook, /* and the same slice: a candidate
                                                                                holds the thread as any flow does */
                                               .budget_period = ENGINE_QUANTUM_ASK_EVERY,
                                               .work = engine_work_hook };   /* and a verify's clock still runs: a
                                                                                candidate re-fires as a flow, and a
                                                                                sink reading a duration must see the
                                                                                one the explore run saw */
static const JSFlowControlHooks FC_OFF     = { 0 };

/* The wall clock, which times nothing the scheduler decides on: the cooperative slice and the WFQ's aging charge
   are both in the quantum's own measure (solver/quantum.h), CPU actually consumed, in one currency. What is left
   for elapsed time is the seam message's reported numbers, the gap between two suspend points and a step's wall
   time, printed and never a verdict. CLOCK_MONOTONIC, so a wall-clock adjustment cannot make a reported gap
   negative. Microseconds is the primitive: milliseconds would quantise a sub-millisecond step to zero, and the gap
   census is built of those. */
static int64_t engine_now_us(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}
static int64_t engine_now_ms(void) { return engine_now_us() / 1000; }

/* The session's hooks come back off in one place, because a session ends two ways (its frontier drained, or was
   paged out) and both must leave the runtime in the same state; a second copy of this list would leave a fork hook
   installed over a runtime whose flows are gone. */
static void engine_session_close(void) {
    /* The frontier a closed session leaves is a set of snapshots: every flow's decision state in its own blob, its
       COW delta unapplied and its DOM head detached. That holds for every member but the one the scheduler is
       holding, whose state is live in decide.c's globals and applied to the heap and document, so the last act of
       a session is the ordinary suspend (the park path, which takes it before it writes, has already left
       g_sess_cur NULL). Otherwise flow_registry_free would be handed a flow with its delta applied, leaving its
       writes in the shared baseline with nothing able to unapply them; flow_release asserts it. */
    if (g_sess_cur) { flow_switch_out(g_sess_ctx, g_sess_cur); g_sess_cur = NULL; }
    /* And the frontier is no longer for sale: past this line there is no scheduler to page for, and the teardown
       allocates (the result document, the leak walk), so an allocation there must fail as an allocation. */
    reclaim_uninstall(JS_GetRuntime(g_sess_ctx));
    JS_SetJobEnqueueHook(NULL);
    JS_SetJobDropHook(NULL);
    JS_SetJobRemoveHook(NULL);
    /* And the orphan-born hook: past this point there is no frontier to route to, and a host that opens a second
       session in this process still compiles and builds closures, so a standing hook would walk a registry that
       has been given back. */
    JS_SetOrphanBornHook(JS_GetRuntime(g_sess_ctx), NULL, NULL);
    JS_SetFlowControlHooks(&FC_OFF);
    g_sess_forking = 0;   /* …and the same bit for the callers that ask the seam by symbol — see engine_sched_begin */
    /* The session's generation ends here. The live stamp is cleared rather than the scheduler's saved copy,
       because this runs inside the slice and the bracket saves whatever the slice leaves, so the saved copy becomes
       zero too: a session that ended and a host between two slices say "no flow is running" with the same number. */
    JS_SetFlowGen(0);
    /* No flow is running, so no candidate substitution may be installed — the same mirror the switch-in keeps,
       completed at the one point where the answer is "none". Without it the LAST flow to run leaves its
       payload and its endpoint suppression standing over everything that reads the frontier afterwards. */
    solve_flow_begin(NULL);
    /* g_dom_capture is not cleared here: it is the flow stamp's twin and the slice owns both, turning it off the
       instant this slice returns, which is when this session's last flow stopped. */
    /* And no flow may be seeded from a document this session no longer has: the hook comes down before the table
       it reads is freed, so a flow created after this line fails flow_add's seeding assert. */
    flow_set_seed_hook(NULL);
    /* And the root document's seed table, this engine's own: the host's inventory is three borrowed columns freed
       by whoever built them (doc_scripts_free), and this table is the copy plus §4.12.1.1's encoding-parsed
       addresses engine_sched_begin computed about it. Freed where a session ends. */
    if (g_root_scripts) {
        /* The table holds a reference on each of its programs and every flow seeded from it holds its own, so the
           buffer lives as long as some flow holds the row. */
        for (int i = 0; i < g_root_n; i++) {
            if (g_root_scripts[i].body) dyn_body_unref(g_root_scripts[i].body);
            free(g_root_scripts[i].url);
        }
        free(g_root_scripts);
        g_root_scripts = NULL;
    }
    g_root_n = 0;
    g_root_n_held = g_root_n_awaited = g_root_n_refused = 0;   /* the arms go back with the total they partition */
    g_sess_live = 0;
}

/* The RAM-to-disk floor and its two flags. The host decides there is pressure (only it sees the other documents'
   engines and the summed working set) and asks this engine to give up its residue; the engine decides when, at a
   scheduler step boundary with no flow switched in, the only moment every flow's decision state is in its own blob.
   `g_parked` then says for the rest of the instance's life that the frontier was written out rather than dropped,
   which the teardown asserts read: a paged flow's continuation, owed replies and queued jobs are regenerated by
   the replay its recipe drives. */
static int g_park_req, g_parked;

/* The generation the scheduler holds across the host's own time. Not a second source of truth: the live stamp is
   quickjs's g_flow_gen, engine_sched_step puts it down here while the host has the thread, and JS_FlowGen() is
   read back at the end of every slice so the forks taken inside it carry forward. Monotonic across slices. */
static uint32_t g_slice_flow_gen;
/* And the delta the scheduler holds across it, beside it because the two are one fact about the running flow (its
   generation, and where its captures land); a boundary that put one down and left the other up would record the
   host's construction of a fresh object into a stranger's head (see engine_sched_step). NULL between slices means
   the host's time captures into nothing, as `!g_current` means (cow.h). */
static CowDelta *g_slice_delta;

void engine_request_park(void) {
    DCHECK(g_sess_live, "a park was requested of an engine with no live session — there is no frontier to "
                        "write out, and the host would then store an empty residue over a real one");
    g_park_req = 1;
}

int engine_frontier_paged(void) { return g_parked; }

/* Write the whole frontier down and state that it has been written: the one park, reached from the two exits that
 * end a session holding live members (the host asking for RAM back, and the engine having nothing left to convert
 * into work while members still hold snapshots). One function, since they are one act.
 *
 * The running flow is switched out first, which is the mechanism: cold_park's walk reads every flow's decision
 * state from its own blob, and the flow the scheduler holds keeps its evolving vector in decide.c's globals. From
 * then on the frontier is a set of snapshots, which is what a parked flow is.
 *
 * Not a drop and not a bound: every member is written in full, and the host stores the document under this
 * bundle's key. The teardown that follows releases the flows, which frees the RAM, and a residue coming back is
 * the same admission step, ranked by the same order. */
static void engine_park_frontier(JSContext *ctx, Flow *cur)
{
    if (cur) flow_switch_out(ctx, cur);
    /* The questions this instance was asked and did not start are handed back first; they are not this instance's
       work to save. A token is the zone's name for a flow suspended in another instance, carries no generation and
       does not outlive the zone session, so it may never enter a recipe (engine.h). Returning them lets cold_park's
       assert name the one shape that has no recipe yet: an operation already started. */
    engine_retract_operations(ctx);
    cold_park();
    /* And the foreign segments the residue carries, announced before the deaths below because the zone acts on
       them in that order: these worlds belong to peers and outlive this session in the park document, so the zone
       holds their deaths for the instance that resumes this document; the ones below are ours and end here. */
    {
        const char *const *carried;
        int n_carried = world_segments_park(&carried);
        engine_notify_worlds_parked(ctx, carried, n_carried);
    }
    /* And every world this session put on the wire is dead to its peers, announced between the park and the
       close, the last point at which a notice of ours is still drained. The frontier is recipes from the line
       above and a resumed session mints in a disjoint generation (world.h), so no name this session sent is used
       again, while a peer still holds a segment for each. The flows released by the host's teardown after this
       returns find nothing left to announce. */
    {
        const char *const *gone;
        int n_gone = world_session_gone(&gone);
        engine_notify_worlds_gone(ctx, gone, n_gone);
    }
    g_parked = 1;
}

/* ─────────────────────────────────────────────────────────────────────────────────────────────────────────
 * The partial self-park: the engine selling its own tail at the RAM floor. Level-1 eviction is the host giving up
 * a whole document for one worth more; this is a lone engine, which no host will ask to park, meeting the floor and
 * staying alive by paging its least valuable flows.
 *
 * Driven by the allocator's refusal and nothing else. A watermark, budget or high-water mark is a number someone
 * picked: one that fires early truncates work while memory remains, one that fires late is the OOM. The runtime
 * asks (quickjs.h's JSMemoryReclaimFunc) when an allocation cannot be satisfied and again after each flow given
 * up, so the stopping condition is the allocation succeeding and the floor is "no member left but the running
 * flow".
 *
 * The safepoint: this runs inside a failing allocation, and removing a member is unsafe wherever the engine holds a
 * position in the frontier (a walk over flow_at that allocates would have a member swapped into the hole behind it
 * and skip one, a dropped work item). So the reclaim is armed only across the flow step, the region with no such
 * position where nearly every allocation happens, and the one frontier walk inside a step disarms it around
 * itself. It is quickjs's version of V8's `DisallowGarbageCollection`: a declaration of where the operation is
 * legal, outside which an allocation fails as an allocation. */
static int g_reclaim_allowed;

static int engine_reclaim_set(int v) { int prev = g_reclaim_allowed; g_reclaim_allowed = v; return prev; }

static int engine_reclaim_tail(JSRuntime *rt, void *opaque, size_t wanted) {
    Flow *tail;

    (void)opaque; (void)wanted;
    /* The ask is counted before it is judged: a refusal counted only where it is honoured would report the same 0
       as no pressure. */
    g_reclaim_asks++;
    if (!g_reclaim_allowed) {
        g_reclaim_unarmed++;
        return 0;
    }
    /* The hook is installed with the session and removed with it, so reaching here without one is a reclaim
       against a frontier that is gone. */
    DCHECK(g_sess_live, "the runtime asked this engine to page out a flow with no live session — the hook is "
                        "installed by engine_sched_begin and removed by engine_session_close, so this is a "
                        "reclaim against a frontier that is gone");
    DCHECK(rt == JS_GetRuntime(g_sess_ctx),
           "a reclaim was asked of this engine by a runtime that is not its session's — one instance is one "
           "runtime, so paging this frontier would not free the memory the other one is short of");
    /* The running flow cannot go: its decision state is live in decide.c rather than in its blob and its delta is
       applied to the heap, which cold_park_flow and flow_release both refuse. Every other member is a snapshot. */
    tail = flow_worst(flow_running());
    if (!tail) {
        /* The floor, a reading rather than a failure: the frontier held nothing but the flow this allocation is
           for, so the allocator's NULL is the physical answer. Counted apart from the unarmed refusal because the
           two take opposite work: here the pager was consulted and had nothing, there it was consulted where it
           may not act. */
        g_reclaim_floor++;
        return 0;
    }
    /* The sharper statement: flow_worst is constructed to exclude the flow the scheduler is switched into, and
       what must hold is that the flow sold is not the one this allocation is for. The engine's own allocations
       ask here (solver/reclaim.h), often while growing the running flow's state (its delta's entry array, its DOM
       undo log) with the old buffer in a local, so selling that flow would free the buffer the caller is about to
       write through, a corruption that would look like a COW bug. */
    DCHECK(tail != flow_running(),
           "the pager chose the flow the scheduler is switched into — the allocation that asked for this "
           "memory is usually growing that flow's own delta or undo log, so selling it frees the buffer the "
           "caller is holding and is about to write through");
    /* And it is still in the registry, a separate fact with a separate failure: a non-member is a stale Flow* the
       frontier already removed, for which cold_park_flow would write a recipe and flow_release free a second time.
       Two asserts, since a conjunction reports either failure under one message. */
    DCHECK(flow_is_member(tail),
           "the pager chose a flow that is not in the frontier — flow_worst walks the registry, so this is a "
           "stale pointer, and both the park below it and the release after it would act on freed memory");
    /* Write it down, then give its RAM back, in that order, because the release makes the recipe the only copy.
       cold_park_flow appends one record to the session's park document (stored under this bundle's key and merged
       back by the next session); flow_release frees the flow's suspended frame chain, heap delta, DOM head and the
       nodes it created, and walks the shared heap and document chains down only as far as the segments this flow
       held the last reference on. A tail flow's dying prefix cannot be a segment the running flow stands on (a
       shared segment has a second reference), so the release moves nothing in the live heap or document. */
    /* The debt leaves with the flow, as two debts counted by kind (see g_paged_owed): a synchronous request is
       answered by id at a call site and a fetch reply is paired by (method, url), so a register counted whole
       would hand the fetch side a credit belonging to the request side and excuse the next reply the host
       mispairs. The reply side counts what is still owed, since the two doors differ in arity (pending.h,
       pending_owed_replies): the reply door answers a request once and drops it from the join when it carries a
       value, so an answered entry sold here was already delivered, and crediting it would quiet qjs_provide's
       pairing assert. The request door answers one id once per peer timeline, so an answered HOSTREQ entry still
       awaits answers and stays counted. */
    g_paged_reqs += pending_count_kind(tail->pending, FLOW_PENDING_HOSTREQ);
    g_paged_owed += pending_owed_replies(tail->pending);
    /* The debt that leaves by being given back: a recipe cannot carry a rendezvous token (engine.h: it is the
       zone's name, has no generation, and does not outlive the zone's session), so the question is returned rather
       than parked. Per flow, because this is a partial park: the instance keeps running and the operation is
       attached to every other timeline, so the notice belongs to the last holder leaving. Selling one of many
       holders costs the peer one timeline's answer, as a timeline that never existed would; the rest still answer
       under the zone's token. */
    /* Which population is leaving, read before the release frees the label: `cand_src` is the whole question
       (solver/engine.h) and flow_release frees it, so a read beside the counter would read freed memory. Per
       member, as at the retirement arm (engine.h). */
    { const int was_cand = tail->cand_src != NULL;
      engine_retract_flow(g_sess_ctx, tail);
      cold_park_flow(tail);
      flow_release(g_sess_ctx, tail);
      g_flows_sold++;
      if (was_cand) g_flows_sold_cands++; else g_flows_sold_flows++; }
    return 1;
}

/* The seam between two vocabularies for one fact. core/html/unhandled_rejection.h names the two spec steps it
   reports from (§8.1.4.7 step 4.1.3 and §8.1.6.4 step 7.4); solver/result.h names what its console does with them
   (a report stands, a report is taken back). A browser component may not include a solver header and a solver
   console does not speak in section numbers, so the translation lives here, in the file that installs the edges.
   Exhaustive and crashing: a third edge is a fact this console has not been told how to render. */
static void engine_rejection_console(JSContext *ctx, JSValueConst reason, RejectionReportEdge edge) {
    switch (edge) {
    case REJECTION_REPORTED:  result_page_error_value(ctx, reason); return;
    case REJECTION_RETRACTED: result_page_error_value_retract(ctx, reason); return;
    }
    DFAIL("§8.1.4.7/§8.1.6.4 reported a rejection edge this console has no answer for — every edge either "
          "records an occurrence or takes one back, so a third one is a step of those algorithms that nothing "
          "here has been taught to render");
}

void engine_sched_begin(JSContext *ctx, char **bodies, char **srcs, const ScriptType *types,
                        const ScriptSchedule *sched, lxb_dom_element_t **els, int n, int forking,
                        const char *recipes) {
    DCHECK(!g_sess_live, "engine_sched_begin while a session is already running — one scheduler, one session");
    /* The document install's own work is the first flow's. §4.8.5's insertion steps run for an <iframe src> in
       the initial markup when the host installs the document, before this session exists, and queue §7.4 step
       14's navigation through JS_EnqueueCallTask; with no enqueue hook installed that is quickjs's baseline list,
       which the first executing flow adopts (js_adopt_baseline_calls), so the frame loads in the boot timeline.
       What this asserts is the other population: a job on quickjs's own job or task list, which only a raw
       JS_EnqueueJob before the hook can put there and nothing in this engine drains. */
    DCHECK(!JS_IsJobPending(JS_GetRuntime(ctx)),
           "a job was enqueued on quickjs's own job or task list BEFORE this session began — the user agent's "
           "own callbacks wait on the baseline list for the first flow, so this is a raw JS_EnqueueJob with no "
           "owner, a work item the frontier will never run");
    g_sess_ctx = ctx;
    g_sess_cur = NULL; g_sess_live = 1;
    /* The three arrays are one inventory, so a session opened with a length and two of them would have the seed
       read the third off the end. Asserted where the borrow is taken, while its builder is still on the stack. */
    DCHECK(n == 0 || (bodies != NULL && srcs != NULL && types != NULL),
           "a session was opened over a script inventory missing one of its three columns — bodies, srcs and "
           "types are one table with one length, and the seed below reads all three");
    /* A list of `<script>` elements carries their §4.12.1.1 schedules (core/loader/document_scripts.h's
       `sched`); only a host's synthesized program list, which names no element, has none and runs in its own
       order. */
    DCHECK(n == 0 || els == NULL || sched != NULL,
           "a session was opened over a document's `<script>` elements without their §4.12.1.1 schedules — "
           "document_exec_scripts computes the column, and without it an `async` external would hold a "
           "position the as-soon-as-possible set does not have");
    /* The document this session is of: the instance's root document, whose script sequence the session runs.
       Asserted against the realm, because everything below compiles by asking the document for its realm, and a
       ctx of another document's realm would run this document's scripts somewhere else. */
    g_sess_doc = world_local_doc();
    DCHECK(doc_realm(g_sess_doc) == ctx,
           "a session was opened with a realm that is not the root document's — the session's scripts are that "
           "document's programs, so they would be compiled in one realm and belong to another");
    /* HTML §4.12.1.1 "Processing model": "Let url be the result of encoding-parsing a URL given src, relative to
       el's node document." Resolved once, here, because the standard computes this url once per element and uses
       it for both things this engine asks of it, §8.1.4.2's fetch and, for a module, the record's [[HostDefined]]
       identity; two derivations would ask the host for one address and key the module map by another. The
       document's realm answers (the assert above makes it `ctx`); every other document's scripts arrive through
       engine_join_document, which resolves against its own realm. Once per session, not per flow: flows are
       created throughout the session (a cold-resumed replay, an @S candidate), and resolving at the seed would
       re-ask under whichever delta was applied, so a `<base href>` one flow wrote would re-point another flow's
       bundle. */
    DCHECK(g_root_scripts == NULL && g_root_n == 0,
           "a session opened while the previous session's root script table was still allocated — the table is "
           "the ENGINE's, so the build below would leak every string in it; engine_session_close is the one "
           "place it is given back and it is the one place a session ends");
    g_root_scripts = n ? (RootScript *)calloc((size_t)n, sizeof(RootScript)) : NULL;
    CHECK(n == 0 || g_root_scripts != NULL,
          "engine: OOM building the root document's script table — every flow of this document is seeded from "
          "it, so without it the document's own bundle would silently not run in any timeline");
    for (int i = 0; i < n; i++) {
        /* A row is something, asserted at the row. The two columns are independent facts about one script (see
           RootScript), so the impossible row is one that is neither, which would park the flow on nothing for the
           rest of the session. Both is the ordinary shape of an external script whose response the host already
           holds: document_scripts.h says of an external entry that `bodies[i]` is "NULL until the host supplies
           it". */
        DCHECK(bodies[i] != NULL || srcs[i] != NULL,
               "a row of the root document's script inventory holds neither source text nor an address — "
               "§4.12.1.1's \"if url is failure … fire an event named error at el, and return\" means such an "
               "element runs no script and takes no position, so a row that is nothing is a row the seed would "
               "park a flow on and never fill");
        DCHECK(script_type_executes(types[i]),
               "a row of the root document's script inventory holds a type that executes nothing — an import "
               "map and a set of speculation rules are registered rather than run and §4.12.1.1's null type "
               "runs nothing at all, so document_exec_scripts drops all three before they reach a session");
        /* And the element the row is the program of: §4.12.1.1's "execute the script element" switches on el, and
           its classic arm sets this document's §3.1.7 `currentScript` to it. Borrowed: it names a node of the tree
           this session was opened over, which outlives the session. An absent column is a positive statement, "no
           program in this sequence came from a `<script>`", which a host driving a synthesized list makes
           (wpt_runner.c builds its own prologue and epilogue); §3.1.7's answer while such a program runs is null,
           its own answer for a document not executing a script element. */
        g_root_scripts[g_root_n].el = els ? els[i] : NULL;
        g_root_scripts[g_root_n].type = types[i];
        g_root_scripts[g_root_n].sched = sched ? sched[i]
                                         : (bodies[i] ? SCRIPT_SCHED_IMMEDIATE : SCRIPT_SCHED_PARSER_BLOCKING);
        /* The address first, for every row that has one, including a row that also has its source text.
           §4.12.1.1's next step for a `src` that does not encoding-parse is "If url is failure, then queue an
           element task on the DOM manipulation task source given el to fire an event named error at el, and
           return", so that element runs no script and takes no position, whatever bytes were handed over (a host
           could hold them only by fetching the address this parse refuses). Resolved before the body is copied,
           so the refusal costs nothing. A refused row keeps the attribute's text as its body and the seed makes
           it the row that fires `error`. */
        if (srcs[i]) {
            g_root_scripts[g_root_n].url = script_src_absolute(ctx, srcs[i], strlen(srcs[i]));
            if (!g_root_scripts[g_root_n].url) {
                DCHECK(g_root_scripts[g_root_n].el != NULL,
                       "a synthesized program list carries an address the URL parser refuses — the host built "
                       "that address itself, and there is no element to fire §4.12.1.1's `error` at");
                g_root_scripts[g_root_n].refused = 1;
                g_root_scripts[g_root_n].body = dyn_body_new(srcs[i], strlen(srcs[i]));
                CHECK(g_root_scripts[g_root_n].body,
                      "engine: OOM copying a refused script's `src` into its seed row");
                g_root_n_refused++;
                g_root_n++;
                continue;
            }
        }
        if (bodies[i]) {
            /* Copied rather than borrowed: this table outlives the call and is read whenever a flow of this document
               is created, throughout the session, while the host's columns are freed by whoever built them. Once,
               not per flow: the copy is the shared body every timeline of this document references
               (solver/dyn_body.h), so the bundle is in this instance once however many flows the frontier grows.
               Its length is its `strlen`, by the tokenizer's guarantee: this column is an inline `<script>`'s text
               as document_exec_scripts scanned it out of a parsed tree, and HTML §13.2.5.4 "Script data state"'s
               U+0000 NULL row reads "This is an unexpected-null-character parse error. Emit a U+FFFD REPLACEMENT
               CHARACTER character token", with `&#0;` the same answer one state later (§13.2.5.84 "Numeric
               character reference end state": "If the number is 0x00 … Set the character reference code to
               0xFFFD"). An inline script cannot carry a NUL, which is why this seam crosses as `char **` while
               every seam that can carry one crosses as a pair. */
            g_root_scripts[g_root_n].body = dyn_body_new(bodies[i], strlen(bodies[i]));
            CHECK(g_root_scripts[g_root_n].body,
                  "engine: OOM copying a program of the root document into its seed table");
        }
        if (g_root_scripts[g_root_n].body) g_root_n_held++; else g_root_n_awaited++;
        g_root_n++;
    }
    /* Every flow of this document is seeded from it, installed before the frontier is seeded below, because the
       boot flow and every cold-resumed one are created by that seeding and need their programs at creation. */
    flow_set_seed_hook(engine_seed_root_flow);
    g_parked = 0;
    /* What a sale left behind belongs to the instance, so it is asserted rather than zeroed. The reply is owed by
       the host that was shown the request, the sold flow is on the cold tier re-issuing next session, and a reply
       in flight across a session boundary must find its credit there; a zero would hand the first such reply to
       qjs_provide's pairing DFAIL. Every host opens one session per instance (main.c, wpt_runner.c's two entries,
       test_forced.c's engine_run), so a second session on one instance crashes here rather than dropping the debt
       it must carry. */
    DCHECK(g_paged_owed == 0 && g_paged_reqs == 0 && g_flows_sold == 0,
           "a session began on an instance that had already sold flows — the debt those sales left is owed by "
           "the HOST that was shown the requests, so it crosses a session boundary with them; it must be "
           "carried into this session, never zeroed, or the first reply that arrives for a sold flow is "
           "reported as the host's pairing being off");
    /* Not reset, because they are the instance's totals rather than a session's, and a rate that restarted would
       be a level again (see g_host_asked): g_host_asked/g_host_answered/g_host_answers_extra/g_host_answers_late/
       g_host_terminated; g_reclaim_asks/g_reclaim_unarmed/g_reclaim_floor, which partition against g_flows_sold
       (carried by the assert above); g_prog_starts/g_prog_starts_cand/g_prog_starts_other, which partition
       against each other, and g_prog_queued_cand, the ask the candidate arm is read against; and the five
       g_net_prog_* counters, three of which stand in two relations asserted at engine_frontier_census and whose
       doors are the host's. Resetting some terms of an identity and not others would break it at the next census
       rather than at the line that did it. Since all of them carry together, a total carried into a second
       session preserves every inequality's order, so a survival cannot explain `answered` overtaking `asked`: a
       reader at that abort is looking at a mint or a credit. */
    /* What an uncancelled rejection means is this half's answer: the browser half fires the event and honours
       preventDefault, and a reason that survives it is a page error, like a script that threw. */
    unhandled_rejection_set_report_hook(engine_rejection_console);
    /* And the same sentence one section up: HTML §8.1.4.6 step 7.3, "the user agent may report exception to a
       developer console", reached only when step 7's notHandled is true. `pageErrors` is that console, one line
       per distinct message carried out of the run. Registered rather than performed at the throw, so every caller
       of report an exception writes one (a §2.9 listener that threw, a custom element reaction, an animation-frame
       or IntersectionObserver callback) and a page that cancels the `error` event stops it. */
    report_exception_set_console_hook(result_page_error_value);
    /* The frontier is seeded once, from one of two alternatives. A document nobody has explored starts with one
       flow: the page's scripts over an empty decision vector. A document with a parked residue starts with that
       residue, every flow the last session could not finish, each on its own path; a boot flow beside them would
       explore the un-forked path again and re-fork every branch the residue stands on, duplicating the frontier on
       every visit, and a boot flow live at the park is one of the records (an empty chain). A new page appends to
       the one continuous cross-session frontier rather than starting a scheduler. Emptiness is a property of
       starting a session, not of rebuilding a residue, so it is asserted here and cold_resume is free to merge,
       which a partial park's read-back needs (its tail returns to the flows that were not paged out). */
    DCHECK(flow_count() == 0,
           "a session began over a frontier that already has members — the boot flow and a parked residue are "
           "ALTERNATIVES, so whichever is chosen below would be a second seeding of the same document: the "
           "un-forked path explored twice, and every branch the existing members already stand on re-forked");
    if (recipes && *recipes) cold_resume(ctx, recipes);
    else flow_add(ctx, JS_UNDEFINED, WORLD_NONE);   /* the first flow: the page's scripts, empty decision vector */
    /* The routing latch is answered here, where the answer is known: only the resume above opens a claim on an
       orphan's body, so the frontier holds one now or none for the session. Closing it here rather than on the
       first closure a page builds makes engine_orphan_born free for every session that inherited no drive, and
       that hook is reached by every function object in this runtime. */
    if (!engine_orphan_claim_open()) g_orphan_claims_closed = 1;
    /* The generation the first slice opens at, written to the scheduler's saved copy, not the live stamp: the
       live one set here would stamp everything created before the first step (the host reads qjs_bundle_id, pulls
       the pending list, provides replies it already had) as belonging to a flow that has not run. This call and
       cold_resume above create at baseline. */
    g_slice_flow_gen = 1;
    /* And no flow is running yet, so the first slice opens with captures routed nowhere. Stated rather than left
       to zero-initialisation, because a second session in one process would otherwise start on the delta the
       previous session's last slice left, a pointer into a released frontier. */
    g_slice_delta = NULL;
    dom_cow_set_ctx(ctx);                   /* the DOM delta needs ctx for the attribute taint-shadow dup/free */
    cow_set_ctx(ctx);                       /* …and the heap delta needs one for the component records it captures */
    JS_SetFlowControlHooks(forking ? &FC_EXPLORE : &FC_VERIFY);   /* preempt always on; fork only when exploring */
    /* The same bit, written where callers that bypass that table can be held to it. The hook table carries the
       explore/verify policy into the interpreter; a C builtin asking the decision seam by symbol
       (core/dom/abort.c, core/timing/timer.c) never consults it, so a candidate re-fire, one concrete path, could
       mint frontier members during its own verification. Asserted rather than answered, because a non-forking
       answer means something different per site (the interpreter falls through to ToBool, the step driver takes
       outcome 0, the timer order has no third answer), and one answer for all would choose a page's timer order
       at random. */
    g_sess_forking = forking;
    JS_SetJobEnqueueHook(engine_enqueue_job);   /* async as flow: reactions route to the enqueuing flow's queue */
    JS_SetJobDropHook(engine_drop_jobs);        /* …and §7.5.10 step 5 takes them back off it */
    JS_SetJobRemoveHook(engine_remove_job);     /* …and a toggle task tracker takes one back off, by name */
    /* And an inherited drive is handed its body the moment that body exists: engine_orphan_born is a second
       reader of the routing walk, not a second door onto the frontier (it routes, never mints). Installed below
       the seeding, because the latch it reads is answered by the line after cold_resume; installed earlier, it
       would be asked about an unrebuilt frontier and engine_orphan_route would close the latch on the compile's
       first closure, before any claim existed. */
    JS_SetOrphanBornHook(JS_GetRuntime(ctx), engine_orphan_born, NULL);
    /* The frontier is the engine's reserve, and this lets an allocator spend it: installed with the session, when
       there is a frontier to page, so a refusal asks engine_reclaim_tail, which sells the lowest-weight member to
       the cold tier and answers "retry". Every allocator, not only the runtime's (solver/reclaim.h): most of this
       engine's memory is the HTML parser's, so most refusals are Lexbor's. */
    reclaim_install(JS_GetRuntime(ctx), engine_reclaim_tail, NULL);
}

/* A second document of this agent, appended to the running frontier (engine.h).
 *
 * One flow, not a session: a new document appends its flows to the one continuous frontier and starts no
 * scheduler, run, attention or counter. engine_sched_begin is the root document's seeding and asserts an empty
 * frontier because a boot flow and a parked residue are alternatives for one document; this is a different
 * document, so it adds to whatever is there.
 *
 * The flow is added unseeded, which makes it this document's boot rather than a second boot of the root's: a
 * flow's cursor indexes one table, its own rows, and flow_add seeds that table with the root document's programs.
 * This member takes none of them, so everything queued below is its whole program order, at cursor 0. The order
 * is the document's, and engine_seed_scripts lays it down from the same rows the root's seed reads, schedule and
 * refusals included. */
void engine_join_document(JSContext *cctx, uint32_t doc, char **bodies, char **srcs,
                          const ScriptType *types, const ScriptSchedule *sched, lxb_dom_element_t **els,
                          int n) {
    Flow *f;
    RootScript *rows;
    int i, rown = 0;

    DCHECK(g_sess_live, "a document was joined to an instance with no live session — the flow its scripts are "
                        "the programs of would have no scheduler to run it, so the document would be parsed, "
                        "given a realm, and never execute a line");
    DCHECK(cctx != NULL, "a document was joined with no realm to compile its programs in");
    DCHECK(doc != 0, "a document was joined naming no document — zero is the world registry's NONE, so the "
                     "programs below would be compiled in whichever realm the session happens to be rooted at");
    DCHECK(doc != g_sess_doc,
           "the document this session was opened over was joined to its own agent a second time — its scripts "
           "are already the frontier's, and a second boot flow over them explores the un-forked path twice");
    DCHECK(doc_realm(doc) == cctx,
           "a document was joined naming a realm that is not the one it answers with — the queue carries the "
           "NAME and the compile resolves it, so the two disagreeing compiles this document's code in another "
           "document's Window");
    DCHECK(n == 0 || (bodies != NULL && srcs != NULL && types != NULL && sched != NULL && els != NULL),
           "a document was joined with a script inventory missing one of its five columns — bodies, srcs, "
           "types, sched and els are one table with one length, and the seeding below reads all five");

    /* The joined document's boot flow: an empty decision vector over this agent's baseline, minted with a root
       world of its own because it stands on no other flow's decisions. Every timeline this document has is a fork
       of it, as every timeline of the root is a fork of engine_sched_begin's flow. */
    f = flow_add_unseeded(g_sess_ctx, JS_UNDEFINED, WORLD_NONE);
    /* The same table shape as the root document's seed, built here because §4.12.1.1's encoding-parse belongs to
       this document: §8.1.3.2 "Environment settings objects"' API base URL is the joined document's, so
       `<script src=app.js>` in a document at `/app/child.html` is `/app/app.js`. `cctx`, the joined document's
       realm, is what the parse is asked of; the session's root realm would fetch a joined document's bundle from
       another document's directory. One row shape, one seeding function, one set of asserts. */
    rows = n ? (RootScript *)calloc((size_t)n, sizeof(RootScript)) : NULL;
    CHECK(n == 0 || rows != NULL,
          "engine: OOM building a joined document's script table — without it the document is parsed, given a "
          "realm and never executes a line, which is indistinguishable from a document that had no scripts");
    for (i = 0; i < n; i++) {
        DCHECK(script_type_executes(types[i]),
               "a joined document's script inventory holds a row whose type executes nothing — an import map "
               "and a set of speculation rules are registered rather than run and §4.12.1.1's null type runs "
               "nothing at all, so document_exec_scripts drops all three before they become rows and a fourth "
               "answer here means the types column was never written for this row");
        DCHECK(bodies[i] != NULL || srcs[i] != NULL,
               "a joined document's script inventory holds an entry with neither source text nor an address — "
               "§4.12.1.1's \"if url is failure … fire an event named error at el, and return\" means such an "
               "element runs no script and takes no position, so a row that is nothing is one this document's "
               "boot flow would park on and never fill");
        /* And the element, borrowed from the tree the host handed over: §4.12.1.1's "execute the script element"
           switches on el, and a joined document's `currentScript` is as observable as the root's. */
        DCHECK(els[i] != NULL,
               "a joined document's script inventory holds a row with no `script` element — every row is one "
               "element of one parsed tree");
        rows[rown].el = els[i];
        rows[rown].type = types[i];
        rows[rown].sched = sched[i];
        /* The address first, for every row that has one, as in the root document's build: source text and base
           URL are two independent items of one script (§8.1.4.1 "Scripts"), so a row handed over with its response
           still carries the address §8.1.4.2 makes its base URL. A `src` that does not parse is a refused row,
           which the seed makes the row that fires `error` at el. */
        if (srcs[i]) {
            rows[rown].url = script_src_absolute(cctx, srcs[i], strlen(srcs[i]));
            if (!rows[rown].url) {
                rows[rown].refused = 1;
                rows[rown].body = dyn_body_new(srcs[i], strlen(srcs[i]));
                CHECK(rows[rown].body, "engine: OOM copying a joined document's refused `src` into its row");
                rown++;
                continue;
            }
        }
        /* The copy is made once, here, and every timeline of this document references it: the row's body is the
           shared program text (solver/dyn_body.h), so this document's bundle is in the instance once however many
           arms its boot flow forks. The length is the `strlen` for the root document's reason: an inline
           `<script>`'s text off a parsed tree, in which HTML §13.2.5.4 "Script data state" has turned a U+0000
           into U+FFFD (engine_sched_begin). */
        if (bodies[i]) {
            rows[rown].body = dyn_body_new(bodies[i], strlen(bodies[i]));
            CHECK(rows[rown].body,
                  "engine: OOM copying a joined document's program into its script table");
        }
        rown++;
    }
    engine_seed_scripts(f, doc, rows, rown);
    /* The table is this call's, not the flow's: engine_queue_into copies the address into the flow's own row and
       takes a reference on the body, so both are given back here, each body surviving as long as some flow holds
       the row that names it. The root document's table outlives its call because it seeds a flow every time one
       is created; this one seeds exactly one. */
    for (i = 0; i < rown; i++) {
        free(rows[i].url);
        if (rows[i].body) dyn_body_unref(rows[i].body);
    }
    free(rows);
}

void engine_set_referenced(int referenced) {
    g_referenced = referenced;
    /* And every member is askable again, because this flag is one of the answers: a flow with nothing left to run
       reports host-owed instead of finishing while it is set (flow_step's referenced return), so clearing it lets
       those flows finish, a change the host made naming no flow, which is the whole-frontier clear (flow.h).
       Unconditional rather than on the 1→0 edge: a mark cleared a step early costs one step, a mark kept after its
       fact has gone costs the flow its ending. */
    flow_clear_host_owed_all();
}

static double g_yield_floor = -1.0 / 0.0;
void engine_set_yield_floor(double w) { g_yield_floor = w; }

/* The host's one input (engine.h). The pick is the scheduler's own: a weight the host ranks an engine by must be one
   the engine could convert into work, and flow_best answers over members that have told the scheduler they cannot
   run. A frontier whose every member is host-owed burns no CPU, so its weight never ages and the evictee picker
   would never reach it. -Infinity is what an empty frontier already publishes, so a stalled engine sorts last
   through the host's existing order. */
double engine_top_weight(void) {
    Flow *b = flow_next_to_run(NULL, FLOW_SCAN_OTHER);
    /* Why the pick came back empty, asserted where the host is told. -inf from a non-empty frontier is a claim on
       the host ("I can convert no slice into work until you act"), so there must be something for the host to act
       on: an unanswered entry on some register, or a peer holding a reference into one of this agent's documents,
       the two the stall path answers STALLED for. The failure worth catching is a mark that outlived what it
       rested on, a register whose every entry has been answered, which no host event can clear: flow_set_host_owed
       asserts that per flow when a mark is made, and this asserts it of the frontier when the mark becomes an
       answer the host ranks on. (`flow_host_owed_count() == flow_count()` would be a tautology, since the pick's
       only filter is the mark.) */
    /* A third thing a -inf can rest on, which is no claim on the host: a frontier every member of which is parked
       on a request the trusted zone refused, a park this engine has recorded and will write to the cold tier. It
       is invisible to the bill by design (the joins skip a refused entry so it is not re-asked and re-refused), so
       `engine_host_owes` answers 0, and the -inf reads "rank me last and let me write my residue down". Asked
       second, not folded into the bill, which would put a refused entry back on a bill the host cannot pay. A
       frontier whose marks rest on answered entries satisfies neither, because `pend_owed` is false for an
       answered entry. */
    DCHECK(b != NULL || flow_count() == 0 || engine_host_owes() || engine_frontier_settled() || g_referenced,
           "this engine reported -inf — it can hand the thread to nobody — while the host owes it NOTHING, no "
           "peer holds a reference into it, and its members are not parked on a REFUSAL either: every member's "
           "mark rests on a register that has already been ANSWERED, so no host event can clear one, the "
           "Level-1 order will rank this document last forever and every timeline it holds is unexplored with "
           "nothing naming it");
    return b ? flow_weight(b) : -1.0 / 0.0;
}

#if APICLIENT_DEV
/* A printable, bounded excerpt of page bytes for a diagnostic. The source is whatever the page shipped, so a message
   quotes an excerpt, and one that stopped says so (bytes that simply end read as the whole). Walked by length
   rather than to a terminator, because ECMAScript §11.1 "Source Text" permits a U+0000 in a program, which would
   otherwise quote as the empty string. */
static void excerpt(char *out, size_t cap, const char *src, size_t n)
{
    size_t i;

    for (i = 0; i + 1 < cap && i < n && src != NULL; i++)
        out[i] = (src[i] >= 0x20 && src[i] < 0x7f) ? src[i] : '.';
    out[i] = '\0';
    if (src != NULL && i < n) {
        const char more[] = "…";
        size_t at = i + sizeof more <= cap ? i : cap - sizeof more;

        memcpy(out + at, more, sizeof more);
    }
}
#endif

static int engine_sched_slice(void) {
    JSContext *ctx = g_sess_ctx;
    Flow *cur = g_sess_cur;
    /* One reading of the slice's own measure per iteration, carried: it closes the step just run (the WFQ's aging
       charge) and opens the next, so the charges telescope across the quantum and no quantisation is lost between
       steps. It is the quantum's measure (quantum_thread_us), because the aging term is CPU aging ("a monopolizer
       that burns CPU without emitting") and must be in the slice's currency; where a host has no CPU clock,
       quantum_measure() says so. Telescoping charges the context switch and finish between two readings to the
       flow stepped next, not the one they were for: one COW delta swap of misattribution, bounded by the swaps
       this scheduler counts, which a second reading per iteration would buy back for an order decision a notch of
       quantisation already absorbs. */
    int64_t now = quantum_thread_us();
    /* The span every cost this loop reports is a share of is opened at engine_sched_step's first statement, not
       here: the partition `loop_us`/`between_slices_us` is charged from engine_sched_step's own two readings, and
       the wrapper's first reading precedes this one (and `quantum_begin()`, the flow-stamp restore and the capture
       route), so a baseline opened here would sit inside the first slice's span. */
    /* The session the host stepped is still open. Two exits close it, both answering ENGINE_STEP_DONE (the
       frontier draining, and the park that writes the residue to the cold tier), so this fires on a caller that
       transformed the previous answer: a wrapper folding DONE into YIELD would send the host back in with nothing
       live, and the abort would read as a bug in the park. Every layer propagates the code unchanged
       (engine_sched_step brackets the slice and returns it verbatim; qjs_step latches it in g_done and answers
       DONE to every later step). */
    DCHECK(g_sess_live,
           "a slice was opened on a session that is no longer live — the previous step answered "
           "ENGINE_STEP_DONE and the host stepped again anyway");
    /* The flow carried across a quantum boundary is still a member of the frontier. The cooperative yield resumes
       "the same top flow on the byte-identical frontier", and what makes that true here is a raw `Flow *` held in
       a static across a return to the host, during which the host routes records, provides replies and answers
       requests. If any of those removed a flow, this pointer would be read (`best != cur`, then flow_switch_out)
       after its free, a lifetime bug looking like a scheduling one. Asserted where the pointer is picked up. */
    DCHECK(cur == NULL || flow_is_member(cur),
           "the flow this quantum resumes is no longer in the frontier — it was removed while the scheduler was "
           "returned to the host, so the resume is neither the same flow nor a byte-identical frontier");
    /* The finish the previous slice deferred so the host could photograph this member's last moment (@PERWORLD;
       the argument is at `g_sess_finish_owed`). Above the park, the pick and every engine_session_close here, so
       the deferral is a split call rather than a second teardown path: whatever this slice does, the member that
       answered FLOW_STEP_DONE last time is finished first, by the two lines that would have finished it then. The
       member is still switched in, as the return left it: the host does not switch flows between the two (no ABI
       entry does), so the delta this unapplies is the one photographed. */
    if (g_sess_finish_owed) {
        DCHECK(cur != NULL,
               "a finish was deferred for a picture and the member it was deferred for is gone — the yield "
               "hands the thread back with that flow standing and this is the one line that completes it, so "
               "a NULL here is a session that lost its own cursor across the return and a flow whose COW and "
               "DOM deltas are still applied to the shared baseline with nothing left to unapply them");
        DCHECK(!flow_paint_owed(cur),
               "a finish deferred for a picture belongs to a member that still owes one — the discharge and "
               "the deferral are written at one line, so this is a mark laid down again while the thread was "
               "with the host, which would photograph a world that has already ended");
        g_sess_finish_owed = 0;
        solve_flow_end(cur);
        flow_finish(ctx, cur);
        cur = NULL;
        g_sess_cur = NULL;
    }
    /* The park is taken here, before the first pick: engine_park_frontier switches the running flow out first,
       since the walk reads every flow's decision state from its own blob. Not a drop and not a bound: every member
       is written in full, and the instance teardown that follows frees the RAM the host asked for. Level-1
       eviction is a whole document's engine leaving memory; its residue coming back is the same admission step,
       ranked by the same order. */
    if (g_park_req) {
        engine_park_frontier(ctx, cur);
        cur = NULL;
        g_park_req = 0;
        g_sess_cur = NULL;
        engine_session_close();
        return ENGINE_STEP_DONE;
    }
    /* Nothing is re-admitted here. A slice that ended on its CPU quantum handed the thread to a host with nothing
       to answer, so clearing host-owed marks at every slice would launder them through the exit a busy slice
       always takes, and a frontier blocked on the host could never be swept to its end, making the stall
       unreachable. The marks live until the host does something that could have answered them, and each such
       event clears the flow it reached (engine_provide, engine_host_answer, engine_deliver, engine_perform, and
       the shared document-script slot inside flow_deliver_one_reply). */
    for (;;) {
        /* The search's own work items, created when they come into existence: an @S candidate re-fire is a flow on
           this one frontier, so a detected sink becomes members the way a fork does. Asked at every pick, not only
           when the frontier drained, which a frontier need never do. Idempotent by cursor (solve.h): one walk of
           the detected-sink list, adding only what detection appended since. At the pick, so a sink detected by
           the last runnable flow is seeded before the frontier can be declared empty, and no other seeding site
           exists to keep in step. A blocked flow's reply is not deferred by this: every driver pays the host at
           every slice (run_scheduler below, main.c's qjs_step at the ABI), so a flow suspended inside a read is
           answered at the next slice. */
        solve_seed_candidates(ctx);
        /* WFQ: highest value of information among the members that can still make progress; a fresh fork (UCB)
           can preempt. The members waiting on the host are taken out of the pick only; they keep their weight,
           place and every work item (flow.h). The flow holding the thread is passed in, which makes this a
           schedule rather than a ranking: the incumbent is displaced only by a strictly better flow, the
           comparison the preempt hook makes, so the two ends of the decision cannot disagree and a field of
           equal-weight members (most frontiers) does not pay a COW delta swap per iteration. */
        Flow *best = flow_next_to_run(cur, FLOW_SCAN_NEXT);
        /* Nothing left to run: the frontier is empty, or every member is waiting on the host (the stall), decided
           by asking each member rather than by counting unproductive picks. */
        if (!best) break;
        if (best != cur) {                  /* context switch: swap COW delta + decision + pins */
            if (cur) flow_switch_out(ctx, cur);
            flow_switch_in(ctx, best);
            solve_flow_begin(best);   /* the substitution is live only while its own flow runs */
            /* The member's own record that it was chosen. The switch count below is a fact about the scheduler;
               this is a fact about the member, and tells a flow the ordering never reached from one it reached
               that made no progress, which take opposite work (flow.h's `picks`); every other candidate row is a
               term of the weight, and flow_credit_emit resets them. Credited here rather than in flow_switch_in,
               whose job is making the flow's state live. */
            flow_credit_pick(best);
            cur = best;
            /* Counted, and reported in the result document: a scheduler that interleaves and one that runs flows
               FIFO produce the same endpoints on an easy page and diverge on every hard one, so whether it switches
               has to be reported. The host's WFQ reads it. Cumulative across steps. */
            g_switches++;
        }
        /* The image this member owes the host is not taken here (@PERWORLD). At the opening of a member's first
           turn its deltas are empty, so a picture here is the document no flow has written, byte-identical to the
           `baseline` image engine_session_close writes, under a world's name; and the frontier walk the ask is
           made of (`paint_mark_standing_members`) can run before the first `qjs_step`, when the one member is the
           boot flow and has run nothing. A turn closes as well as opens with the member switched in and its deltas
           applied, and only the closing moment holds what the member did, so the discharge is where a turn closes:
           the FLOW_STEP_DONE arm below (deferred across the return, because flow_finish unapplies the deltas) and
           the slice-end yields beside it. It is a yield, not a hook: the host that asked renders, and nothing
           crosses the seam but the turn. At most one image is un-drained, since each mark is cleared by the one
           discharge that spends it. It changes no rank and truncates nothing; the host's render burns this
           thread, the misattribution a slice already carries for every record it routes, paid only by a host that
           asked for pictures. */
        {
            /* Aging is charged in thread time, not in steps: a step is one unit of work, so a per-step charge would
             * bill twelve milliseconds of execution the same as advancing one script index. The aging term demotes
             * "a monopolizer that burns CPU without emitting", so it measures time in the reward's currency
             * (flow.c's FLOW_AGE_RATE: one emitted finding per second held without emitting). The reading is
             * unconditional, since a release build needs the aging policy as much as a dev one, and costs nothing,
             * being carried in `now`.
             *
             * It is the quantum's measure (solver/quantum.h): CPU actually consumed where the host has a CPU clock,
             * natively CLOCK_THREAD_CPUTIME_ID, the clock the slice's timer is armed on. On the extension's wasm
             * instance every WASI clock is emscripten_get_now(), so the measure is wall time and quantum_measure()
             * names it; there a descheduled step is charged for time it did not burn and a flow is demoted one
             * notch early, which reorders nothing out of the frontier. The seam assertion below cannot take that
             * trade, since it aborts, so it decides on work.
             *
             * Work done is the wrong quantity here: a tight compute loop over opaque input forks, queues and emits
             * nothing, so its share of engine_work_done() stays at zero and aging by work would never demote it.
             * The interpreter's retired-dispatch count moves in that loop but has one increment site, inside
             * quickjs.c's DISPATCH, so a C activation that declares no step boundary retires none of it however
             * long it runs, and a charge in it would leave a C-builtin-heavy flow undemotable. That is the
             * population solver/quantum.h names as one no raise source reaches: one blind spot in the raise and the
             * charge, closed by the step-machine conversion rather than by a choice of unit. */
            int64_t t0 = now;   /* this step's start: the previous iteration's reading, carried */
#if APICLIENT_DEV
            uint64_t pq0 = 0, pf0 = 0, pa0 = g_preempt_asked;
            /* The work this step performs, sampled beside the clocks (see the verdict below): forks, flows and jobs
               are things the engine did, which no descheduling can inflate. */
            long w0 = engine_work_done();
            /* And the wall clock, read separately because it measures something else: `now` is the slice's measure
               (CPU where the host has one), while the gap census and the step duration below are wall quantities
               the seam message reports and never decides on. */
            int64_t wall0_ms = engine_now_ms();
            JS_FlowPreemptStats(&pq0, &pf0);
            g_max_gap = 0; g_last_ask = wall0_ms;   /* this step's gaps, measured from the moment it starts */
            idl_slowest_reset();              /* ...and this step's slowest single Web API member step */
#endif
            /* The reclaim safepoint, this one region for a structural reason: across the step the engine holds no
               position in its own frontier, so a member removed by the allocator's refusal edge cannot be removed
               from under a walk. It is also where nearly every allocation of a run happens (the page's objects,
               frames, deltas). Restored rather than cleared, because the step can reach this loop again through a
               nested driver. */
            int prev_reclaim = engine_reclaim_set(1);
            /* The scheduler's pick is behind this line (solve_seed_candidates, flow_next_to_run, the context switch,
               solve_flow_begin), and the flow is switched in but has executed nothing. `detail` is the switch count:
               a stray completion with it unchanged is the seeding or the pick, one with it just incremented is the
               swap. */
            ENGINE_NO_STRAY(ctx, "pre-step: the scheduler's pick, the context switch and the delta swap",
                            g_switches);
            /* Where the turn's two halves meet (see g_slice_us): everything the scheduler does for this turn is
               behind this line and the flow has executed nothing, the boundary the stray check above is declared
               at. */
            int64_t t_slice0 = quantum_thread_us();
            g_sched_us += t_slice0 - now;
            /* The same bracket in the other currency, opened on the clock's line so the two readings are of one
               span: the overrun test below compares `now - t_slice0`, and this counts the suspend points offered
               across that interval. From here rather than the turn's `t0`, because the pick, switch and delta swap
               consult no policy. A load of a static in every build; see g_over_asks. */
            uint64_t pa_slice0 = g_preempt_asked;
            /* The third reading of the same bracket, needing no clock call: `t_slice0` is the turn's opening
               reading in the slice's measure, so the gap census opens from the number the overrun test subtracts
               from. Reset here rather than at `t0`, as `pa_slice0` is: a gap measured from `t0` would include a
               span in which no offer was possible and read as a missing seam (solver/engine.h's `over_gap_arms`). */
            g_gap_max_us = 0; g_gap_last_us = t_slice0;
            int r = flow_step(ctx, cur);
            /* Where the step's answer goes onto the flow: the one point every arm of flow_step converges on, so a
               new arm cannot forget to be recorded. The arm declares its name; it is kept here, read by the `@COLD`
               histogram and the seamless-stretch aborts below, and decides nothing. flow_step resets to NONE at
               entry, so a step returning without naming its arm crashes here with the flow in hand rather than
               being attributed to the previous step (solver/step_unit.h). */
            DCHECK(g_step_unit != STEP_UNIT_NONE,
                   "a scheduler step returned without naming the arm it took — flow_step resets to NONE at its "
                   "entry and every exit declares itself, so this is an exit added without a name and the "
                   "member's step-unit row would report whatever the PREVIOUS step did");
            cur->step_unit = g_step_unit;
            /* And the arm's lifetime count, at this one point for the same reason: an increment per exit is two
               dozen places to forget one, and a forgotten one undercounts silently. The line above is where this
               member stands, overwritten at its next step; this is how many times the scheduler has run the arm,
               never overwritten. The pair separates "the ladder never reaches this rung" from "it reaches it
               constantly and no member is sitting there when a census is taken", two readings of one zero that
               take opposite work. It decides nothing. */
            g_step_unit_runs[g_step_unit]++;
            /* The conservation, asserted at the one instant it is exact: flow_step has returned, so every entry
               counted at its reset is recorded here, and the two are counted at different places so that this can
               fail. Here rather than at the publishing accessor, because the step that failed to record itself has
               just returned with its flow in hand, and between an entry and its record the identity is off by one
               by design. */
            DCHECKF(step_unit_runs_total() == g_steps,
                    "the lifetime step histogram does not account for every scheduler step (%ld arm counts "
                    "against %ld entries into flow_step) — the entry is counted where flow_step resets the "
                    "arm name and the arm is counted here, so a difference is a step that entered and did NOT "
                    "reach this line: a second caller of flow_step, a return added between the call and this "
                    "record, or a step driven from outside engine_sched_slice. Every reading composed from "
                    "the histogram is then about a ladder that is not the one this document climbed",
                    step_unit_runs_total(), g_steps);
            engine_reclaim_set(prev_reclaim);
            ENGINE_NO_STRAY(ctx, "post-step: the arm flow_step just ran", r);
            /* HTML §8.1.7.3's end of a microtask checkpoint. The flow has run a unit of work (a script, a
             * microtask, a task), and if it holds no microtask the queue has drained, when HTML runs the steps
             * other standards register there. Asked here rather than in flow_run_one_job, because a script that
             * queued nothing still ends in a checkpoint and never reaches the job pump. Running it more often than
             * a browser costs nothing: every consumer's steps are idempotent (Indexed Database §2.7.1 empties its
             * set and then answers false), as HTML's own re-entry from clean up after running script relies on. A
             * finished flow is not asked: the registered steps queue work (a deactivated transaction with an empty
             * request list commits, a database task), and a task enqueued on a finished flow is dropped; the
             * flow's COW delta carries every transaction it created and unapplies with it.
             *
             * Nor is a flow suspended inside a program: a preempt is not the end of a unit, and HTML §8.1.4.4
             * Calling scripts performs the checkpoint only "if the JavaScript execution context stack is now
             * empty". That stack has two representations here: `Flow::frame`, a page script's suspended frame
             * chain (solver/flow.h), and the runtime's parked slot, where a flow that preempts inside job-driven
             * code parks with `frame` NULL (quickjs.h's pump). Every Indexed Database member that matters runs
             * inside a job (`upgradeneeded`, `success`, `abort` are event handlers), and the cleanup step's state
             * transitions (deactivate an active transaction, commit it when its request list is empty), run early,
             * would deactivate a transaction under a member suspended between §4.5's "is the transaction active"
             * check and the step that places its request, which core/indexeddb/idb_object_store.c's
             * os_active_across_suspension aborts on. Running these steps more often is free; running them earlier
             * is not. `JS_HasParkedFlow` is asked of the runtime because the flow is switched in here and the queue
             * is its own (JS_TakeParkedFlows/JS_PutParkedFlows carry it across a switch). */
            /* The emptying itself, read once: this checkpoint, the notify below and the visit credit further down
               ask the identical three-term question, and what may differ between them is what they do, never what
               "this flow's turn ended" means. */
            /* Whose turn this is, asserted before the first line that reads the answer as `cur`'s. Two of the three
             * terms below are per-flow fields; the middle one, `JS_HasParkedFlow`, asks the runtime, which under an
             * interleaving host holds only the switched-in flow's parked continuations (flow_switch_out's
             * JS_TakeParkedFlows moves the set onto `Flow::parked`; flow_switch_in's JS_PutParkedFlows puts it
             * back, asserting the runtime was empty). So it is a fact about `cur` only while `cur` holds the
             * thread; asked of another member it answers about a stranger, and the §8.1.4.7 notify, the §8.1.7.3
             * checkpoint and the visit credit below all act on it. quickjs.h says who may park at JS_HasParkedFlow
             * but whose parks the runtime holds only at JS_TakeParkedFlows, and the name alone reads as a
             * global-quiescence gate rather than §8.1.4.4's "the JavaScript execution context stack is now empty"
             * (solver/flow.c's flow_between_units says the same from the other side).
             *
             * One assert covers both readers because the register cannot move between them: it is `static` to
             * solver/flow.c, written only by its initialiser, flow_registry_init's reset at a session boundary and
             * flow_set_running (called only by flow_switch_out, flow_switch_in and flow_finish), and the two
             * consumers between here and the charge are browser components with no scheduler entry. If something
             * that can write the register comes to stand between them, the charge needs its own assert again. */
            DCHECK(flow_running() == cur,
                   "the flow the scheduler stepped is not the one holding the thread — the unit-of-work "
                   "boundary on the next line reads `JS_HasParkedFlow` of the RUNTIME, whose parked set "
                   "belongs to the switched-in flow alone, so the microtask checkpoint and the rejection "
                   "notify below would run on another flow's suspended activations and the visit credit "
                   "would be spent on the wrong member; and the aging charge further down would demote a "
                   "flow that did not burn the time and leave the one that did holding its rank");
            /* And which clause answered, recorded where it is decided: the same conjunction in the same order (a
               live frame, the runtime's parked slot, a checkpoint still owed), written as an if/else so the refusing
               clause has a name. Short-circuiting is preserved, so `JS_HasParkedFlow` is not asked of a member
               holding a frame, and `unit_ended` is derived from the ladder. One arm per turn, so a partition (see
               g_unit_mid_program). */
            int unit_ended;
            if (cur->frame)                                { g_unit_mid_program++;     unit_ended = 0; }
            else if (JS_HasParkedFlow(JS_GetRuntime(ctx))) { g_unit_parked++;          unit_ended = 0; }
            else if (flow_job_microtask(cur))              { g_unit_checkpoint_owed++; unit_ended = 0; }
            else                                             unit_ended = 1;

            /* And the step of "perform a microtask checkpoint" that comes first. Having run the microtask queue to
               empty, the algorithm says "For each environment settings object settingsObject whose responsible
               event loop is this event loop, notify about rejected promises given settingsObject's global object",
               and the Cleanup Indexed Database transactions step the hook below serves is the step directly after
               it, so the two tails are written in the standard's order. Idempotent for a stronger reason than the
               hook: the notify clears the list as it queues (unhandled_rejection.h), so a second checkpoint queues
               nothing; and the position is not earlier than a browser's, because this predicate is the emptying
               HTML §8.1.4.4 Calling scripts' clean up after running script step 3 triggers the checkpoint on. A
               finished flow is not asked, for the hook's reason: what this queues are §8.1.4.7 step 4 tasks on the
               flow's own queue. flow_step's finished exit refuses to end a timeline whose list is non-empty, so a
               flow that never reaches this seam crashes by name rather than dropping the page's error report. */
            if (r != FLOW_STEP_DONE && unit_ended)
                unhandled_rejection_notify(ctx);
            if (g_checkpoint_hook && r != FLOW_STEP_DONE && unit_ended)
                g_checkpoint_hook(ctx);
            /* The charge, billed to the flow that ran: `flow_age_running` bills whoever the registry says is
               running, which is the flow this step advanced only while nothing since the switch-in changed it
               (asserted at the unit boundary above, before the register's first reader); otherwise a monopolizer
               would never age and a sibling would be demoted for time it never spent. The finish path clears the
               register, so this stands before the r == DONE branch. */
            /* What the order said about this flow before it was charged, held for the resolution assertion below
               and guarded with it: check.h's release DCHECK still type-checks its condition (`(void)sizeof(cond)`),
               so an unguarded reading would be a per-step call in a build that reads nothing. */
#if APICLIENT_DEV
            int64_t age_notch0 = flow_silence_notch(cur);
#endif
            now = quantum_thread_us();
            /* The other half, from the same two readings the charge uses, never a third clock reading: `t_slice0`
               was taken just before the step and `now` just after, so this arm is the step alone, and the two arms
               sum to the charge by construction. */
            g_slice_us += now - t_slice0;
            /* Whether this turn overran, by the inequality solver/quantum.c's quantum_expired() uses: a step that
               met the slice never reached a raise point, which a mean of turn costs cannot report. Counted and
               nothing else; no branch depends on it, since a per-turn overrun test is what a watchdog is built of. */
            if (now - t_slice0 >= (int64_t)ENGINE_QUANTUM_MS * 1000) {
                g_slice_over++;
                /* Keyed on the arm this turn declared, still in hand: `g_step_unit` is written at flow_step's
                   convergence point above and not overwritten until the next step, so arm, readings and budget are
                   one turn's. */
                g_step_unit_over[g_step_unit]++;
                /* The conservation, at the one instant it is exact: both are raised here, so a disagreement learned
                   at the accessor would arrive with the turn that broke it long gone. */
                DCHECKF(step_unit_over_total() == g_slice_over,
                        "the slice-overrun histogram does not account for every overrunning turn (%ld arm "
                        "counts against %lld overruns) — both are raised on this line from one turn's arm and "
                        "one turn's clock readings, so a difference is a second raise site for one of them",
                        step_unit_over_total(), (long long)g_slice_over);
                /* And the per-arm containment neither sum can see: both totals hold with one arm's overrun count
                   above its run count and another's below. This turn raised `g_step_unit_runs[g_step_unit]`
                   unconditionally at the convergence point, same arm and no branch between, so it is true by
                   construction here, which makes `arms[i] - over_arms[i]` a non-negative reading of one
                   population. */
                DCHECKF(g_step_unit_over[g_step_unit] <= g_step_unit_runs[g_step_unit],
                        "arm %d has overrun the slice %ld time(s) against %ld run(s) of that arm — the "
                        "run is counted at the convergence point above and the overrun on the line above "
                        "this one, from ONE turn's arm, so a subset larger than its own population is "
                        "`g_step_unit` having been rewritten between the two lines or an overrun counted "
                        "for a turn whose arm was never recorded",
                        (int)g_step_unit, g_step_unit_over[g_step_unit], g_step_unit_runs[g_step_unit]);
                /* And whether the page's code offered a point in this turn, raised here where the turn is known to
                   be one of the population (solver/engine.h's `slice_overrun_asks`); `pa_slice0` was read on
                   `t_slice0`'s line, so both operands bracket the span judged above. A turn adds to the sum exactly
                   when it does not add to the seamless count, by the order of these two statements, which the
                   accessor asserts as an equivalence. The delta cannot be negative, since `g_preempt_asked`'s only
                   writer is preempt_hook's increment; a negative delta is a reset or a wrap. */
                DCHECKF(g_preempt_asked >= pa_slice0,
                        "the preempt policy reports FEWER lifetime consultations after a step than before it "
                        "(%llu against %llu) — the only writer is preempt_hook's own increment, so this is a "
                        "second writer that resets it, and every `sliceOverrunAsks` reading taken since is a "
                        "difference of two unrelated instants",
                        (unsigned long long)g_preempt_asked, (unsigned long long)pa_slice0);
                if (g_preempt_asked == pa_slice0) {
                    g_over_seamless++;
                    /* And in which arm, which decides whether the answer is a step-machine conversion or nothing
                       (see g_step_unit_over_seamless). Same arm, turn and delta, one statement apart, so both
                       identities below are exact here. */
                    g_step_unit_over_seamless[g_step_unit]++;
                    DCHECKF(step_unit_over_seamless_total() == g_over_seamless,
                            "the seamless-overrun histogram does not account for every seamless turn (%ld arm "
                            "counts against %ld seamless) — both are raised on these two lines from one "
                            "turn's arm, so a difference is a second raise site for one of them",
                            step_unit_over_seamless_total(), g_over_seamless);
                    /* And the per-arm containment the sum is blind to, for the runs/overruns pair's reason;
                       `g_step_unit_over[g_step_unit]` was raised a few lines up, same arm and no branch between. */
                    DCHECKF(g_step_unit_over_seamless[g_step_unit] <= g_step_unit_over[g_step_unit],
                            "arm %d offered no suspend point in %ld overrunning turn(s) against %ld "
                            "overrunning turn(s) of that arm — the overrun is counted a few lines above this "
                            "one from the SAME `g_step_unit`, so a subset larger than its own population is "
                            "that name having been rewritten between the two lines",
                            (int)g_step_unit, g_step_unit_over_seamless[g_step_unit],
                            g_step_unit_over[g_step_unit]);
                } else {
                    g_over_asks += g_preempt_asked - pa_slice0;
                    /* And in which arm those points were offered, where the two readings of an overrunning turn
                       differ (solver/engine.h's `over_ask_arms`). Same turn, arm and delta, one statement from the
                       scalar, so the identity below is exact here. */
                    /* The headroom is asked before the add, never after: a sum of non-negative deltas (the sign is
                       asserted above) can mislead only by overflowing, and a check after the addition reads an
                       undefined value. The row is a `long` because solver/result.c has one step-unit composer and
                       it prints `%ld` (solver/engine.h's `over_ask_arms`). */
                    DCHECK((long)(g_preempt_asked - pa_slice0) <= LONG_MAX - g_step_unit_over_asks[g_step_unit],
                           "one arm's count of suspend points offered by overrunning turns would overflow a "
                           "`long` — the deltas are non-negative and the row only climbs, so this is a "
                           "frontier that has run for about two billion consultations in one arm or a "
                           "consultation counter that has stopped being monotone; either way every "
                           "`stepUnitOverrunAsks` reading taken after it is a wrapped total");
                    g_step_unit_over_asks[g_step_unit] += (long)(g_preempt_asked - pa_slice0);
                    DCHECKF((uint64_t)step_unit_over_asks_total() == g_over_asks,
                            "the per-arm ask histogram does not account for every point offered by an "
                            "overrunning turn (%llu arm counts against %llu in the scalar) — both are raised "
                            "on these two lines from ONE turn's arm and ONE consultation delta, so a "
                            "difference is a second raise site for one of them",
                            (unsigned long long)step_unit_over_asks_total(),
                            (unsigned long long)g_over_asks);
                    /* And the widest interval inside this turn with no point offered, which the sum above is a
                       proxy for (solver/engine.h's `over_gap_arms`). The tail is closed here: `now` is the turn's
                       closing reading in `g_gap_last_us`'s measure, so `now - g_gap_last_us` is the stretch from the
                       last offer to the turn's end, which for a turn that consulted nine times in its first
                       millisecond and then ran for seconds is the finding; `g_gap_max_us` alone would report
                       milliseconds for it. Only for a non-seamless turn: a turn that offered no point has no
                       interval between offers, its whole duration is already its share of `g_slice_us`, counted by
                       `g_step_unit_over_seamless`, and one row holding a turn's worst gap and its length could not
                       be read for either. */
                    {
                        int64_t gap = now - g_gap_last_us > g_gap_max_us
                                      ? now - g_gap_last_us : g_gap_max_us;
                        /* The headroom is asked before the narrowing, for the ask row's reason against a
                           different horizon: this row is a maximum, so the `long` it must be overflows only for one
                           turn whose worst no-offer stretch exceeds about 2147 seconds on a 32-bit host, an engine
                           hung for half an hour. */
                        DCHECK(gap <= (int64_t)LONG_MAX,
                               "one turn's widest no-suspend-point stretch does not fit the `long` the "
                               "step-unit row composer prints — that is about 2147 seconds in the slice's "
                               "own measure inside a SINGLE turn, so either this engine has held the thread "
                               "for half an hour without offering a point or the two readings the interval "
                               "is taken between belong to different clocks");
                        if (gap > g_over_gap_us) g_over_gap_us = gap;
                        if ((long)gap > g_step_unit_over_gap[g_step_unit])
                            g_step_unit_over_gap[g_step_unit] = (long)gap;
                        /* The identity is a max, not a sum: a sum of maxima is a quantity no turn produced, and the
                           arm row and its scalar are folds of one per-turn number over one population, so the
                           scalar is the arms' maximum. Asserted here, with the arm, the turn's readings and its
                           delta in hand. */
                        DCHECKF(step_unit_over_gap_max() == g_over_gap_us,
                                "the per-arm worst-gap row and its scalar disagree (%ld against %lld) — both "
                                "are folded from ONE turn's interval on the two lines above this one, and the "
                                "fold is a MAXIMUM rather than a sum because a sum of maxima is a quantity no "
                                "turn produced, so a difference is a second raise site for one of them or a "
                                "row reset while its scalar was not",
                                step_unit_over_gap_max(), (long long)g_over_gap_us);
                    }
                }
                DCHECKF(g_over_seamless <= g_slice_over,
                        "more overrunning turns offered no suspend point (%ld) than overran at all (%lld) — "
                        "both are raised inside this one branch from one turn, the total unconditionally and "
                        "the subset under a test of that turn's own consultation delta, so a subset larger "
                        "than its population is one of them being raised somewhere else",
                        g_over_seamless, (long long)g_slice_over);
            }
            /* What this turn of the dispatch loop cost, from the delta the charge bills, never a second reading
               (see g_step_us). Before the charge, because `flow_age_running` is the WFQ's policy and this is a
               census, which may not decide whether the policy ran. In every build: without it `steps` has no
               denominator. The headroom is asked before the add: a sum of non-negative charges (flow_age_running
               asserts the sign; flow.c: a negative charge promotes a monopolizer) can mislead only by overflowing.
               At int64_t the horizon is hundreds of thousands of years of slice time, so this cannot fire today;
               the _Static_assert catches a narrowing at build time and this catches a clock that stopped being
               what the reading assumes, which together make solver/engine.h's paragraph a checked claim. */
            DCHECK(now - t0 <= INT64_MAX - g_step_us,
                   "the dispatch loop's microsecond total would overflow — its charges are non-negative and it "
                   "only climbs, so this is either a slice measure that has stopped being microseconds or an "
                   "accumulator that has been narrowed under solver/engine.h's `step_us`; either way the "
                   "census's `stepUs / steps` would read as CHEAP turns rather than as a broken number, which "
                   "is the one reading that row exists to make and the direction it inverts to");
            g_step_us += now - t0;
            flow_age_running(now - t0);
#if APICLIENT_DEV
            /* The WFQ's monopolizer half, asserted where the charge meets the pick. Its twin, "a UCB optimism bonus
               ∝ 1/(visits+1) so a never-run flow is never starved", is a floor on the weight flow_pick returns;
               flow_pick's bound on the aging half is derived from flow_weight and catches only an edit to the
               formula. This is observability: whether consuming a whole quantum moves the flow's rank at the
               granularity the thread is handed out in, since the next statement of this loop is the pick. Under
               today's unit it is an identity: `flow_silence_notch` divides by the cooperative quantum and the
               charge adds the same microseconds to both of its summands, so a full quantum moves it at least two
               notches. A unit coarser than a quantum would leave the optimism bonus as the only finer term, whose
               sole mover is completing a unit of work, the precondition for reaching a queued job
               (`frame == NULL`), so the one flow able to run a promise reaction would be the only one paying. A
               charge under one quantum is exempt: it is below anything this scheduler decides, and the charges
               telescope. */
            DCHECK(now - t0 < (int64_t)ENGINE_QUANTUM_MS * 1000 || flow_silence_notch(cur) > age_notch0,
                   "a flow consumed a whole COOPERATIVE QUANTUM of the thread and its rank did not move — the "
                   "aging term is quantised coarser than the granularity at which the thread is handed over, "
                   "so the pick that immediately follows this charge is made against a weight this charge did "
                   "not touch, and a monopolizer is invisible to the ordering for as many picks as that unit "
                   "is quanta wide");
#endif
            /* And the other term's charge, a count and not a clock: the optimism bonus is "∝ 1/(visits+1)", and a
             * visit is a completed unit of work (a program that ended, a job, a delivery, a lifecycle stage), never
             * a slice of thread time. The predicate is HTML §8.1.4.4 "Calling scripts"'s, step 3 of clean up after
             * running script, "if the JavaScript execution context stack is now empty", read from the one
             * `unit_ended` the checkpoint and the notify read. A flow preempted mid-program is not credited: it is
             * the same trial, still running, and crediting per quantum would leave a flow strictly outranked by
             * every arm it forked, never reaching the end of a program or its queued jobs. The unit ends when the
             * microtask queue does, since the checkpoint the emptying triggers runs "While the event loop's
             * microtask queue is not empty"; crediting at the return would demote the flow (the bonus is
             * 1/(1+visits)) the moment its queued reactions became eligible, and flow_credit_visit asserts this at
             * the origin. A finished flow is credited too: `r == FLOW_STEP_DONE` is the largest completed unit,
             * read only by the census.
             *
             * `g_units_done` is the instance-wide count of the same event, taken here so it cannot drift from the
             * credit: `_unitsDone` beside `_jobsRun` tells "flows reach the boundary and have no jobs" from "no
             * flow reaches it" (see g_units_done). */
            if (unit_ended) { flow_credit_visit(cur); g_units_done++; }
            /* The four arms of the unit boundary partition the dispatches, asserted on the line the fourth arm is
               written on: `g_steps` counted the turn at flow_step's entry and the ladder above put it on exactly one
               arm, with no `continue`, `break` or `return` between. It catches a reason to decline a unit added
               without an arm, and a nested dispatch (a hook between the boundary and here that stepped a flow would
               move `g_steps` under a turn already charged, which `sum(arms) == steps` also fires on). */
            DCHECKF(g_units_done + g_unit_mid_program + g_unit_parked + g_unit_checkpoint_owed == g_steps,
                    "the unit-of-work boundary's arms do not partition the dispatches (%ld ended + %ld "
                    "mid-program + %ld parked + %ld checkpoint-owed against %ld steps) — the boundary is "
                    "evaluated once per turn on a straight line from flow_step's return to this credit and "
                    "`g_steps` is raised at that step's entry, so a difference is a clause added to the "
                    "conjunction with no arm, or a dispatch that happened between the two",
                    g_units_done, g_unit_mid_program, g_unit_parked, g_unit_checkpoint_owed, g_steps);
            /* The cooperative-quantum contract, asserted at its site. A flow_step must reach a suspend point (a
               bytecode back-edge where the preempt hook runs, a step machine's boundary) within the quantum, which
               is what makes the frontier parkable. A path with no suspend point stops the run: the slice's expiry
               is never observed and the engine spins with the switch count frozen. The preempt fired/requested
               ratio cannot catch it, since a pure C loop reaches no back-edge and requests nothing. Not a bound: it
               truncates nothing, drops no flow and is compiled out of release; it asserts the path has the seam the
               design requires and names the flow, with an enormous margin (400x the 12 ms quantum) so anything
               under it is merely slow. */
#if APICLIENT_DEV
            {
                /* THE WALL CLOCK'S OTHER END, taken once, for the two REPORTED numbers below. It is deliberately
                   a different reading from the aging charge above: that one is the slice's measure and this one
                   is elapsed time, and a message that printed one of them under the other's name is exactly the
                   confusion §Testing's four false reds came from. */
                int64_t done = engine_now_ms();
                int64_t spent = done - wall0_ms;
                (void)spent;
                /* the TAIL closes the last gap: a step that offers a point and then runs for seconds before
                   returning has that silence between its last offer and its end, and nothing else records it. */
                int64_t gap = done - g_last_ask > g_max_gap ? done - g_last_ask : g_max_gap;
                /* THE VERDICT IS WORK, NOT TIME — and the clock is now only something the message PRINTS.
                   `asked == 0` says the path offered the scheduler no suspend point, which is the missing seam
                   this exists to catch. It was ANDed with a wall-clock gap, and a wall clock cannot tell the two
                   states apart that matter here:
                     - a stretch of C that genuinely has no seam, which spins forever with the switch count
                       frozen and is the real defect, and
                     - a stretch that was simply DESCHEDULED because something else on the box had the CPU.
                   A descheduled thread executes no instructions, so it reaches no preempt check either: it shows
                   `asked=0` for exactly the same reason, and the wall gap it accumulates is not its own. That is
                   not hypothetical — three concurrent builds made this fire at 14486 ms with `points asked=0` on
                   a tree whose seam was intact, and the same commit run alone passed; the resulting diagnosis
                   was published and had to be retracted. A false failure is worse than a missing one, because it
                   sends the next reader hunting a phantom in whichever component happened to be under test.
                   CPU TIME IS THE RIGHT MEASURE AND ONE OF THE TWO HOSTS HAS IT — see the second verdict below.
                   This paragraph used to say flatly that "this host does not have one", which was true of the
                   extension's wasm instance (emscripten's WASI clock_time_get answers CLOCK_MONOTONIC and both
                   CPUTIME clocks from emscripten_get_now(), i.e. wall time) and false of the native build, which
                   this same file compiles for. solver/quantum.h answers it per host instead of a comment
                   claiming it for both, and quantum_measure_is_cpu() is what the CPU verdict is gated on so
                   there is never a wall clock inside something calling itself CPU.
                   WHAT IS MEASURED HERE IS WHAT THE STEP DID. Forks, flows created and jobs run are work the
                   engine PERFORMED; being descheduled cannot inflate any of them, because a thread that is not
                   running performs none. A step that mints thousands of siblings without once consulting the
                   preempt hook has no seam on that path whatever the machine's load — that is the invariant,
                   stated in the quantity it is actually about. It truncates nothing and drops no flow: the
                   engine that reaches this margin is already the one that never returns.
                   THE SHAPE THAT REACHES IT is real and is in the tree today: an unknown-length walk over
                   opaque input asks one outcome-fork question per position and the driver re-enters the same
                   step immediately after each fork, so one scheduler step mints an unbounded chain of siblings
                   and never comes back to be ranked.
                   THE ONE CASE THIS CANNOT SEE is a seamless stretch that also does no observable work — a bare
                   `for(;;);` inside C, which forks nothing, queues nothing and emits nothing. That is what the
                   SECOND verdict below is for, and it exists only because the native host now has a real thread
                   CPU clock: consumed CPU cannot be inflated by load, so it separates a seamless stretch from a
                   descheduled one where a wall clock never could. It is a SEPARATE verdict with its own message
                   deliberately — §Testing: two measures must never collapse into one answer, or a reader cannot
                   tell which one decided. */
                long work = engine_work_done() - w0;
                /* THE CPU VERDICT — the seamless stretch that does no observable work, which the work verdict
                   below is blind to by construction. Gated on the host actually having a CPU clock rather than
                   assumed: on emscripten this quantity is wall time, a loaded machine would falsify it, and a
                   false red is worse than a missing one (§Testing). Its own message, its own margin, so the two
                   answers are never confused for each other. */
                if (quantum_measure_is_cpu() && (now - t0) > ENGINE_SEAMLESS_CPU_US && g_preempt_asked == pa0) {
                    /* SIZED FOR THE WHOLE LINE, and 256 was not: the fixed text alone is over 200 bytes before
                       quantum_measure()'s 54, so `unit=` and `script_i=` — the two fields that say WHERE — were
                       the first things off the end. */
                    DFAILF("%d ms of THREAD CPU consumed in one step with NO suspend point offered (measure: "
                           "%s; %ld units of work, which is why the work verdict cannot see this one) — this "
                           "stretch has no suspend/resume seam; unit=%s script_i=%d",
                           (int)((now - t0) / 1000), quantum_measure(), work, step_unit_name(g_step_unit),
                           cur ? cur->script_i : -1);
                }
                if (work > ENGINE_SEAMLESS_WORK && g_preempt_asked == pa0) {
                    /* THE TWO EXCERPTS ARE THE ONLY BOUNDED THINGS HERE, and they are bounded because their
                       SOURCES are unbounded — a payload and a script body are page bytes of any length, so a
                       diagnostic quotes an excerpt and says it is one. Everything else on this line is the
                       assert's message and is sized by check.h, which is what deleted the clamp that used to
                       stand here: `wi` took snprintf's return — the length it WOULD have written — so a
                       truncated prefix left an index past the end and the body append computed `why + 900`
                       with `sizeof why - 900` underflowing to ~2^64, writing the terminator outside a 640-byte
                       stack buffer. The diagnostic that names a missing seam smashed the stack at the moment
                       it named it. There is no index to get wrong now. */
                    char pl_txt[512], body_txt[1024];
                    const char *sk = cur && cur->cand_sink ? cur->cand_sink : "(exploration flow)";
                    const char *pl = cur ? cur->cand_payload : NULL;
                    /* WHICH PROGRAM. "a resume ran 5s" is still a symptom until the JS it was running is named,
                       and the flow already knows: script_i indexes the ONE sequence — the document's scripts and
                       the flow's own dynamic bodies (a lazy chunk, an injected <script>, a fired PoC) are rows of
                       the same table (solver/flow.h). Without this the only way
                       left to find the code is bisecting the fixture by hand, which is the thing this assertion
                       exists to replace. */
                    const char *bodytxt = NULL;
                    size_t bodyn = 0;
                    int si = cur ? cur->script_i : -1;
                    if (cur && si >= 0 && si < cur->dyn_n) {
                        bodytxt = dyn_body_text(cur->dyn[si]);
                        bodyn = dyn_body_len(cur->dyn[si]);
                    }
                    uint64_t pq = 0, pf = 0;
                    const char *slow_name = "(none)";
                    long wrap_n = 0, wrap_cap = 0;
                    long steps_n = 0;
                    int64_t slow_ms = idl_slowest_step(&slow_name);
                    JSMemoryUsage mem;
                    int64_t steps_ms = idl_step_total(&steps_n);
                    JS_FlowPreemptStats(&pq, &pf);
                    if (g_wrap_stats) g_wrap_stats(&wrap_n, &wrap_cap);
                    /* THE LIVE HEAP, because a garbage collection is the one thing reachable from an ordinary
                       Web API call whose cost is the size of everything ELSE. createElement allocates — a
                       wrapper, a shape — so it can trigger a collection, and a collection marks the whole live
                       set. That makes ONE call arbitrarily slow while every other call of the same member is
                       fast. The wrapper map's size sits beside it deliberately: the two together say whether
                       the live set is the DOM the flows built or something else entirely, which is the
                       difference between a leak this fixes and a leak still to find. */
                    JS_ComputeMemoryUsage(JS_GetRuntime(ctx), &mem);
                    /* THREE numbers, because two of them cannot separate the roots. `asked` is how many suspend
                       points the path OFFERED (every consultation of the hook); `requested` is how many of those
                       the WFQ wanted to take; `fired` is how many actually parked. asked==0 means the path has
                       no suspend point on it at all — the seam is missing and must be built. asked>0 with
                       requested==0 means the points were there and the scheduler declined every one, which is a
                       ranking question, not a missing seam. requested>fired means a point was reached, the
                       preempt was wanted, and it was DROPPED because no driver at that depth adopts the seam. */
                    /* BOTH MEASURES ARE PRINTED, the work one that decided it and the wall clock that no
                       longer does, so the next reader can see at a glance which state they are in: a big work
                       count with a small gap is a real seamless stretch on an idle box, and a big gap with the
                       same work count says the box was also loaded — neither changes the verdict, and that is
                       the point. */
                    /* BOTH EXCERPTS ARE WALKED BY LENGTH, NOT TO A TERMINATOR — this diagnostic exists to NAME
                       the program a stuck flow is running, and a bundle carrying a U+0000 (ECMAScript §11.1
                       "Source Text" permits one) would have named the empty string or a prefix of itself. The
                       byte filter replaces every non-printable with '.', so a NUL prints as one character like
                       every other control. It is NOT for the record's sake — check.h escapes what it emits —
                       it is so a reader gets one line of source text instead of a run of escapes. */
                    excerpt(pl_txt, sizeof pl_txt, pl, pl ? strlen(pl) : 0);
                    excerpt(body_txt, sizeof body_txt, bodytxt, bodyn);
                    DFAILF("%ld units of work (forks+flows+jobs) with NO suspend point offered "
                           "(wall gap %d ms, step ran %d ms — reported, NOT the verdict; the slice's own "
                           "measure here is %s; points asked=%llu, preempts wanted=%llu fired=%llu; slowest "
                           "Web API member step: %s %dms of %dms over %ld member steps; wrapper map "
                           "%ld/%ld; live objects %lld, heap %lld KiB) — this stretch has no "
                           "suspend/resume seam; unit=%s script_i=%d "
                           "flow=%s payload=%s body=%s",
                           work, (int)gap, (int)spent, quantum_measure(),
                           (unsigned long long)(g_preempt_asked - pa0),
                           (unsigned long long)(pq - pq0),
                           (unsigned long long)(pf - pf0), slow_name, (int)slow_ms,
                           (int)steps_ms, steps_n, wrap_n, wrap_cap,
                           (long long)mem.obj_count, (long long)(mem.malloc_size / 1024),
                           step_unit_name(g_step_unit), si, sk, pl_txt, body_txt);
                }
            }
#endif
            if (r == FLOW_STEP_DONE) {
                /* THE IMAGE THIS MEMBER OWES THE HOST, TAKEN AT THE ONE MOMENT ITS WORLD IS COMPLETE —
                   @PERWORLD. This is the closing edge of the member's last turn: it has run everything it
                   will ever run, `flow_running()` still names it, and its COW and DOM deltas are still
                   applied, because the two lines below are what unapply them. A member ends exactly ONCE, so
                   a mark spent here costs exactly one return and one picture per world — the same price the
                   discharge above this loop used to charge for a picture of nothing.
                   THE YIELD IS TAKEN BEFORE THE FINISH AND THE FINISH IS OWED ACROSS IT, which is the whole
                   of `g_sess_finish_owed` and is not a second teardown path: the SAME two calls run, at this
                   function's own entry, before anything else this session does. Yielding after them would
                   render a document whose delta had just been reverted — the baseline again, one turn later.
                   NOTHING IS DEFERRED FOR A MEMBER THAT OWES NO PICTURE, so a run made without the ask —
                   `engine_request_paint_every_world` — reaches none of this and finishes on the line it
                   always did. */
                if (flow_paint_owed(cur)) {
                    flow_clear_paint_owed(cur);
                    g_sess_finish_owed = 1;
                    g_sess_cur = cur;
                    return ENGINE_STEP_YIELD;
                }
                solve_flow_end(cur); flow_finish(ctx, cur); cur = NULL;
            }
            else if (r == FLOW_STEP_OWED) {
                /* THIS FLOW CAN MAKE NO PROGRESS UNTIL THE HOST ANSWERS, and the scheduler records that ON THE
                   FLOW. It is NOT skipped and NOT removed — it stays in the WFQ at its own weight with every
                   work item it holds, and it is picked again at the top of the next slice — which is the first
                   moment anything CAN have answered it, since only the host can.
                   IT USED TO BE A COUNT of consecutive owed answers, broken at `flow_count()` on the claim that
                   "every member has answered that in a row". The claim was false: this loop re-picks the SAME
                   top-ranked flow, because an owed step burns microseconds and so moves neither its service
                   notch nor its weight. N owed answers were therefore N answers from ONE flow, and the break
                   declared the whole frontier stalled while runnable siblings had never been asked — which in
                   the smoke host ends the run over them (the provider answers nothing and run_scheduler
                   breaks). A no-progress count is in §NO-BOUNDS' own list, and this is why. */
                /* AND THE MARK IS A CLAIM ABOUT THE HOST, ASSERTED WHERE IT IS MADE. A marked flow leaves the
                   pick until a HOST EVENT clears it, so the mark is only ever true if there is something the
                   host has actually been shown and can still answer: an entry on this flow's register the JOINS
                   will list — or the one case with no entry at all, a document a peer holds a reference into.
                   IT ASKED `pending_outstanding` AND JUSTIFIED ITSELF BY A PROPERTY THOSE JOINS DO NOT HAVE.
                   The clause here read "which is in engine_pending_fetches or engine_host_requests by
                   construction, since both walk every flow's register and select exactly the unanswered", and
                   both joins select the unanswered AND NOT DECLINED — deliberately, so that a refusal is a fork
                   rather than a spin. So the predicate was one bit looser than the sentence defending it, and
                   the one state it let through is the one this assert was written to catch: the WAITING arm of
                   a decline is marked host-owed, leaves the pick for good, and appears on neither join, so the
                   host is billed for a reply nobody will ever send. Anything else is a flow that has left the
                   run queue for good, and NOTHING would say so: `live` still counts it, `blocked` and `owed`
                   still report it as waiting, and the whole timeline behind it simply never runs again.
                   THE FRONTIER-WIDE FORM OF THIS EXISTS ALREADY AND CANNOT SEE IT (the stall claim below).
                   That one is checked only when the pick finds NO runnable member, so a single permanently
                   marked flow is invisible for as long as its siblings keep the loop busy — which is exactly
                   the state in which a document stops getting deeper while every number about it looks
                   healthy. It is also the STRICTER predicate: `pending_count > 0` is satisfied by a register
                   whose every entry has already been ANSWERED, and a flow stuck on one of those is precisely
                   the shape no host event can ever clear. Asked per flow, at the instant the mark is made. */
                /* …AND THE THIRD REASON A MARK MAY REST ON SOMETHING, WHICH IS WHY THIS IS THREE CLAUSES AND
                   NOT TWO. The paragraph above is right that the assert and the selecting arm ask different
                   questions and WRONG about what follows from it: it concluded that the gap "is the FORCING
                   FUNCTION" and that a refused flow needed a resting verdict of its own. A mark is a bit
                   meaning INELIGIBLE, and a refused flow is exactly that — it can convert no slice into work,
                   and taking it out of the pick is the right and only action. What the assert is really about
                   is whether the mark RESTS ON ANYTHING, and there are three ways it can: a reply the host can
                   still send, a peer that can still ask this timeline something, and a refusal this engine has
                   recorded and can account for. The third was missing, so the one state the design REQUIRES
                   aborted the dev build.
                   THE DEFECT IT WAS WRITTEN TO CATCH IS UNTOUCHED, WHICH IS THE WHOLE TEST OF A WIDENING. A
                   register whose every entry has been ANSWERED satisfies none of the three — `pend_owed` is
                   false for an answered entry, so both register walks answer 0 — and it fires exactly as
                   before. The widening admits the refusal and nothing else, which is what the partition assert
                   in pending.c makes a property rather than a hope.
                   AND IT MAY ONLY BE WIDENED BECAUSE THE STATE IS NOW CORRECT AT THE OTHER END. Until
                   engine_sched_slice's tail wrote its survivors to the cold tier, admitting a refused flow
                   here would have converted a loud dev abort into a silent DROP — a green gate over a
                   timeline that ends with the process, which is choosing the result over the forcing
                   function. The park and this clause are one diff for that reason. */
                DCHECK(pending_host_outstanding(cur->pending) ||
                       pending_declined_outstanding(cur->pending) || g_referenced,
                       "a flow was marked host-owed while its mark rests on NOTHING — every entry on its "
                       "register has already been ANSWERED, so no host event can clear this mark and the flow "
                       "is out of the pick for the rest of the session with its whole timeline unexplored. A "
                       "deliverable answered entry is taken by the delivery arm above this one, so what "
                       "reached here is an entry the host settled and nothing will settle again: find why that "
                       "delivery did not happen. A REFUSED entry is NOT this state — it is owed to the flow "
                       "and owed by nobody, it is a park the session writes to the cold tier before it closes, "
                       "and it satisfies the clause beside this one");
                flow_set_host_owed(cur);
            }
            /* AND NOTHING IS CLEARED BY PROGRESS, which is a statement about what a slice can do rather than an
               omission. No step of any flow can answer another flow's wait: every kind on the register — a
               fetch reply, an external script's text, a synchronous request's answer, a routed record, a
               cross-agent operation — is filled by the HOST, and the host does not run until this slice
               returns. Clearing here instead would re-admit every marked flow after every step, and with a
               marked flow outranking a working one that is a wasted step and two COW swaps per unit of real
               progress. */
        }
        /* THREAD-SHARING, not value: the slice's budget is gone, so hand the thread back and keep the frontier.
           It is the SAME question preempt_hook asks — one budget, one edge, asked at the two levels it acts on:
           there the running flow parks, here the step returns. Asking it twice from one source is what makes a
           flow that parked on the quantum and a step that ends on it the same event rather than two policies
           that have to be kept in step. */
        /* VALUE, in the two shapes below it: this engine's best is now worth less than the runner-up ENGINE's,
           or a better DOCUMENT is waiting. The flow keeps its snapshot and resumes where it stands — an order
           decision, never a drop. The document arm takes a FINITE weight only, and that is the whole of the
           difference between the two answers this engine can give a host. engine_top_weight is the RUNNABLE
           pick, so -inf means this frontier can hand the thread to nobody at all — which is not "the other
           document is worth more", it is the STALL, and the host has to be told which because they ask for
           opposite things: a yield asks to be outranked, a stall asks to be PAID. The stall is one iteration
           away (the pick at the top of the loop returns NULL and breaks to it), so the guard costs a
           comparison and keeps the two verdicts from collapsing into the weaker one.
           THE THREE ARE ONE DECISION READ INTO ONE LOCAL, AND THAT IS WHAT MAKES THE DISCHARGE BELOW SOUND
           RATHER THAN A SECOND COPY OF THEM. They used to be three `if`s with three returns; a picture owed at
           a slice boundary has to be spent on exactly the turns that RETURN, and a second spelling of "does
           this turn return" beside the first is two right answers to one question, which is the shape that
           drifts the day a fourth yield is added. One evaluation, one answer, both consumers. `engine_top_weight`
           is also asked exactly once here, where the old shape would have asked it twice. */
        {
            int yielding = 0;

            if (quantum_expired()) yielding = 1;
            else if (cur && flow_weight(cur) < g_yield_floor) yielding = 1;
            /* AND THE THIRD ARM IS ASKED ONLY WHERE IT CAN DISAGREE WITH THE SECOND — A COST AND NEVER A
               DECISION, BECAUSE THE ANSWER IS IDENTICAL AT EVERY INPUT. The two arms read two POPULATIONS:
               arm 2 is the INCUMBENT'S weight, arm 3 the maximum over the pick's RUNNABLE candidates.
               flow_pick's only filter on a runnable entry is flow_host_owed — this call passes no seed and
               no exclusion, so nothing else skips anything — hence an incumbent that is a live unmarked
               member IS one of the candidates arm 3 maximises over, and `top >= flow_weight(cur)`. Reaching
               arm 3 at all says arm 2 did not fire, i.e. `flow_weight(cur) >= g_yield_floor`. The two compose
               to `top >= g_yield_floor`, so `top < g_yield_floor` is unreachable there and the scan cannot
               move `yielding` off 0 — which is where the skip leaves it. Where the incumbent is NOT a
               candidate that implication is gone and the scan is the only thing that can answer: `cur` is
               NULL because the arm above FINISHED it, or `cur` is MARKED because the arm above MARKED it,
               and in both cases arm 2 asked about a flow the pick no longer weighs.
               THE FILTER IS READ AND NOT RE-DERIVED. flow_host_owed is flow_pick's own predicate, published
               for this one consumer; inferring the mark from the step code that laid it down would be a
               second writer's worth of knowledge about a mark with exactly one writer, which is the shape
               the paragraph above this block already names for "does this turn return".
               WHAT IS CONTINGENT IS MEMBERSHIP, AND IT IS THE HALF ASSERTED. Once the incumbent is in the
               scanned set the arithmetic is an identity, so all that is left to be wrong is `cur` naming a
               flow the registry no longer holds — O(1) through its registry handle, side-effect-free.
               THE HANDLE IS READ HERE THOUGH flow_is_member REFUSES TO, and the difference is whose pointer
               it is. That predicate answers for a pointer that MAY BE DANGLING, so it can only compare
               addresses; `cur` is the member this iteration just stepped and ARM 2 ONE LINE ABOVE ALREADY
               DEREFERENCED IT through flow_weight, so the read adds no exposure this decision did not
               already have. `flow_running()` is not the witness to reach for instead: flow_remove does not
               clear it, so it goes on naming a departed flow and would answer TRUE for exactly the state
               this assert exists to catch.
               ASSERTING THE CONCLUSION IS NOT AVAILABLE AT ANY PRICE: flow_pick raises `g_scan_runs` on
               every call, so a dev-only re-scan would corrupt the very counters this decision is measured
               with and make the dev build's census a different program from the one that ships.
               NO ASSERTION COVERAGE GOES WITH THE CALL. engine_top_weight's own DCHECK fires only on a -inf
               — a pick that came back empty — and in the skipped case the incumbent is a candidate, so the
               pick returns at least it and that assert's first disjunct holds for every input reaching here.
               IT IS A PRECONDITION AND NOT A FALLBACK BY §C-stack'S TEST: delete engine_top_weight and the
               question still has to be asked, because "is the incumbent in the population the other arm
               read" is what decides whether arm 2's answer already covers arm 3's. Nothing is selected
               against and no second implementation waits behind a shape it fails to answer for.
               RETIREMENT: this argument goes when the two arms become ONE reading over ONE population — an
               arm 2 that asks the pick's maximum rather than the incumbent's own weight — because there is
               then no second population for the implication to be about. */
            else if (cur == NULL || flow_host_owed(cur)) {
                double top = engine_top_weight();
                if (top > -1.0 / 0.0 && top < g_yield_floor) yielding = 1;
            }
            else {
                DCHECK(flow_at(cur->reg_i) == cur,
                       "the yield decision skipped its own third arm on an incumbent the registry does not "
                       "hold — the skip is sound because a live unmarked member is weighed by the runnable "
                       "pick, so `top` is at least this flow's weight and the arm could not have fired; a "
                       "flow that has LEFT the frontier is weighed by nobody, the bound is gone, and this "
                       "engine will hold the thread past the host's floor with nothing saying so. Either "
                       "`cur` outlived a flow_remove that should have cleared it, or its registry handle "
                       "and the registry have come apart");
            }
            /* AND THE OTHER CLOSING EDGE OF A TURN, DISCHARGED WITHOUT BUYING A RETURN — @PERWORLD. A yield
               hands the host this exact member, standing, with its COW and DOM deltas applied and a step of
               real work behind it: that IS the turn a picture is owed of, so a member whose mark is still up
               when one fires has been paid, and clearing it is the discharge rather than a shortcut past one.
               No extra return is taken because the return is already happening.
               IT IS WHAT KEEPS A MEMBER THAT NEVER FINISHES FROM NEVER BEING PHOTOGRAPHED, which is the one
               thing moving the discharge off the pre-step line could otherwise have cost: a flow that loops
               for the rest of the session reaches the DONE arm never and reaches a slice boundary constantly.
               The guard on `cur` is the same one the weight arm above carries — a slice can reach this line
               with no member standing at all. */
            if (yielding) {
                if (cur != NULL && flow_paint_owed(cur)) flow_clear_paint_owed(cur);
                g_sess_cur = cur;
                return ENGINE_STEP_YIELD;
            }
        }
    }
    g_sess_cur = cur;
    /* THE STALL CLAIM, CHECKED OF EVERY MEMBER — the other half of the host-owed mark, and the half that would
       catch the OPPOSITE failure from the one just fixed. flow_set_host_owed asserts that a mark is never
       cleared by something that cannot have answered it; nothing asserted that a mark is never LAID DOWN on a
       flow that could still have progressed. That failure is silent in the other direction: the engine stops
       exploring early and reports a frontier "waiting on the host" that the host owes nothing, which from
       outside is indistinguishable from a document that really is waiting.
       Reaching this line means the pick found no runnable member, so every member is marked; each of them must
       therefore hold something ONLY THE HOST can supply. That is an entry on its register — a fetch reply, a
       document script's text, an answer to a synchronous request — or the one case with no register entry at
       all: a document a peer still holds a reference into, whose last timeline reports host-owed INSTEAD of
       finishing (engine_set_referenced), and which is waiting for an operation rather than for a reply.
       IT IS ALSO WHERE "how many requests may one stall answer" IS DECIDED, and the answer is ALL of them:
       engine_host_requests joins every outstanding record into a buffer that grows (no truncation, no cap) and
       every host walks the whole list. So a frontier that stays blocked across a stall is not a seam that
       answers one at a time — it is this set being REGENERATED by forking, since a fork re-issues its parent's
       unanswered synchronous request under the sibling's own world (engine_fork_finalize). */
    for (int i = 0; i < flow_count(); i++) {
        /* OUTSTANDING, NOT MERELY PRESENT — the same correction the per-flow assert at the mark carries, and
           the reason it is a correction rather than a tightening: `pending_count > 0` is true of a register
           whose every entry has already been ANSWERED, and that is not a member waiting on the host, it is a
           member nothing can wake. Counted, it read as a healthy stall; asked this way it names itself. */
        /* AND THE MESSAGE ASKS THE PREDICATE'S OWN QUESTION NOW, WHICH IT DID NOT, AND THE GAP WAS A TRAP FOR
           THE NEXT READER RATHER THAN A WORDING SLIP. It read "owed the host NOTHING" over
           `pending_outstanding`, which is the FLOW question — and a reader who took the message at its word
           would have "corrected" this to `pending_host_outstanding`, at which point it fires on every frontier
           parked entirely on REFUSALS: a state that is legitimate, that reaches this walk on the one route
           that gets past the two STALLED exits above, and whose members are written to the cold tier six lines
           below. That is the mirror of the two corrections in pending.h, and the predicate here was right
           while its sentence pointed at the wrong one. What this walk is actually about is a member holding
           NOTHING AT ALL — a mark laid on a flow that could still have progressed — which is the failure
           direction the mark's own assert cannot see. */
        DCHECK(pending_outstanding(flow_at(i)->pending) || g_referenced,
               "the frontier reported a STALL while one of its members was waiting on NOTHING — every entry on "
               "its register has been answered and delivered, so its mark was laid down while it still had "
               "work to do and the exploration of that timeline stops here for no reason at all. A member "
               "parked on a REFUSAL is not this state: a refused entry is still outstanding to its own flow, "
               "so it satisfies this and is carried out to the cold tier rather than caught here");
    }
    /* STALLED, not exhausted: the run-queue is empty but flows are parked on something only the host can
       supply. Asked BEFORE closing — the session and every parked snapshot stay live, and the host steps again
       once it has provided. It is the ENGINE's own question over the ENGINE's own registers now; it used to be
       a host callback, and the shipped host answered half of it. See engine_host_owes. */
    if (engine_host_owes())
        return ENGINE_STEP_STALLED;
    /* NO SEEDING HERE, AND THE FRONTIER IS NOT ASKED WHETHER IT IS BLOCKED. Both used to stand at this line —
       the @S search was seeded exactly where the frontier had drained, guarded by an assert that no member was
       parked on the host — and the whole of that is now one ask at the PICK above, where a candidate becomes a
       member the moment its sink exists. What stood here could only ever have run for a document that DRAINS,
       which §NO BOUNDS says need never happen. */
    /* AND THE OTHER REASON A FRONTIER IS NOT EXHAUSTED, which is not a reply the host owes but a QUESTION it
       has not yet been asked: a document another instance holds a reference into can be asked something at any
       moment, so its timelines are parked (flow_step returns OWED for them) rather than finished, and the
       session stays live with every snapshot intact. Closing here instead would leave the next operation with
       no timeline to answer in — which engine_perform's own assert names, from the other end. */
    if (g_referenced)
        return ENGINE_STEP_STALLED;
    /* ASYNC-AS-FLOW forcing function: every flow has run to completion, so NO microtask/promise reaction may
       still be queued. If one is, the scheduler DROPPED it — the not-yet-built async-as-flow capability (a
       reaction must become a first-class scheduler flow carrying the queuing flow's COW, which needs a fork
       job-enqueue hook). Crash LOUD here rather than silently drop it, so the gap cannot hide. */
    DCHECK(!JS_IsJobPending(JS_GetRuntime(ctx)),
           "async: a job reached the global list (enqueued outside a flow) but was never drained");
    /* THE SAME RULE ONE LEVEL UP, over the FLOWS' OWN queues. flow_step asserts that a flow may not FINISH
       holding work, and that covers the flow that runs out of work — but the loop above can also LEAVE with a
       flow still alive: every member has reported itself host-owed, so no member can be picked, and this line
       closes the session over the survivors. A reaction still on one of their queues is dropped exactly as it
       would be at finish, and only the finish case was being checked, so the wider one was invisible. */
    for (int i = 0; i < flow_count(); i++) {
        DCHECK(flow_job_pending(flow_at(i)) == 0,
               "the frontier was declared exhausted while a live flow still held queued jobs — its promise "
               "reactions, timer callbacks and delivered messages die with the session");
        /* THE OWED-ANSWER ASSERT THAT STOOD HERE IS GONE, AND WHAT RETIRED IT IS THE PARK AT THE FOOT OF THIS
           FUNCTION RATHER THAN ANYBODY DISAGREEING WITH IT. Its text read "the frontier was declared exhausted
           while a live flow still owed a peer the answer to a cross-agent operation … this session is about to
           end without ever telling it anything", and every word of that was true while this exit DROPPED its
           survivors — which is exactly what engine_park_frontier's own banner below says of the state it ended
           ("A SESSION THAT CLOSES OVER LIVE MEMBERS WRITES THEM DOWN FIRST, and until this line it did not").
           IT IS NOT TRUE IN ANY STATE THIS LOOP CAN RUN IN, which is stronger than saying it went stale. The
           loop iterates flow_count() times and the park below is gated on flow_count() > 0, so a body that
           executes at all is a body whose park runs — and engine_retract_operations is the FIRST thing that
           park does, unconditionally and ahead of cold_park, clearing BOTH disjuncts of flow_owes_answer for
           every flow and emitting `remoteop.retracted` for every token that no staying member still holds. The
           peer IS told, so the assert's own stated harm was false in one hundred per cent of the states it
           could fire in — the concession tell (§Offensive-programming) arriving as a claim about the
           continuation rather than about the case.
           AND THE OBLIGATION IS ASSERTED TWICE MORE, both times AFTER the repair rather than in front of it:
           engine_retract_span's own postcondition ("disjuncts of flow_owes_answer are addressed above"), and
           cold_park_flow's pair over the arrival slot and the program's row. This line was the only one of the
           three standing BEFORE the hand-back, which is why it was the only one that could abort a correct
           session; deleting it loses no coverage and removes the copy that cannot be right.
           WHAT WAS PROPOSED INSTEAD, RECORDED BECAUSE THE NEXT READER WILL RE-DERIVE IT AND BOTH ARMS ARE
           WRONG: move the assert below engine_park_frontier — which is a fourth copy of a predicate already
           asserted at its producer AND at its consumer — or scope it to the flow_count() == 0 arm, where this
           loop iterates ZERO times and the assert becomes one whose two sides cannot disagree. The remedy half
           of that reasoning was a hypothesis and its order half was evidence; only the order half held.
           RESIDUAL — THE TWO ASSERTS THIS LOOP STILL CARRIES REST ON THE SAME RETIRED PREMISE AND ARE NOT SWEPT
           WITH IT, because what stands downstream of them is not the same thing. NOT COVERED: `flow_job_pending
           == 0` forbids a state cold_park_flow's own banner argues at length is CORRECT (a job the replay
           re-causes rides the recipe; only flow_job_external is refused there), and `flow_deliver_pending == 0`
           forbids a state the park REPAIRS by writing an 'm' record per delivery — but only when the park is
           TAKEN, and cold_park has a declining arm. NEXT DIFF: settle whether the `zero == 0` conservation
           assert further down still holds without this walk, since its banner cites "the walk above asserts no
           live flow holds one" as its premise, then move both below the park gated on the decline
           engine_frontier_paged() already names. ABSENCE SHOWS: a session whose members hold replay-caused
           jobs, or a peer's message the park would have written, aborts at this loop instead of parking.
           AND THE NOTE DIRECTLY BELOW IS NOW ADJACENT TO ITS OWN SUBJECT, which it was not before: it is about
           a ROUTED RECORD and it sat above the owed-answer assert, one declaration too high, so a reader
           scanning the loop bound it to the wrong line. A comment binds by POSITION and by nothing else, and
           an assert inserted between a note and the thing it describes re-points it silently. */
        /* AND THE SAME RULE FOR A ROUTED RECORD, which is a work item exactly as a job is. flow_finish asserts
           it for the flow that RUNS OUT of work; a flow that leaves this loop alive (every member host-owed, so
           none is pickable) had nothing checking it, and the record then dies in flow_registry_free — the
           peer's message, dropped, indistinguishable from a page that registered no handler. A flow suspended
           inside a live frame is the shape that reaches here holding one: the delivery is made only where
           flow_step has no frame, so if this fires, the enqueue belongs earlier than that branch. */
        DCHECK(flow_deliver_pending(flow_at(i)) == 0,
               "the frontier was declared exhausted while a live flow still held routed records — a peer's "
               "message this document never received, dropped with the session");
    }
    /* AND THE CONSERVATION LAW OVER THE DELIVERIES THAT DID BECOME TASKS. The THREE asserts above are each about
       ONE queue at ONE moment — it read FOUR until the owed-answer assert above was retired, and the count is
       stated here rather than silently renumbered so a reader re-deriving it does not put the fourth back;
       this is the only line that can say the queued §9.3.3 tasks all reached an end,
       because a task's end is a fact about a run and not about a queue. flow_deliver asserts the delivery
       became exactly one task (§9.3.3 step 8 queues one global task, singular); nothing asserted that the task
       RUNS, and a task that never ran leaves the page unable to tell a message it did not get from one that was
       never sent. The four ends are engine.h's; the inequality is its too — a fork gives the arm its own Array
       naming the parent's job records, so a timeline that branches between the enqueue and the run delivers in
       both arms and the sum legitimately exceeds the count of tasks queued. Below it, a delivery vanished. */
    {
        long ends[ROUTED_TASK_END_N], sum = 0;

        engine_routed_task_census(ends);
        for (int i = 0; i < ROUTED_TASK_END_N; i++) sum += ends[i];
        DCHECK(sum >= g_routed_delivered,
               "the frontier was declared exhausted with FEWER §9.3.3 delivery-task ends than deliveries — a "
               "routed record became a task on a receiving timeline's queue (flow_deliver asserts exactly one) "
               "and that task never ran, so a peer's message was dropped by the SCHEDULER rather than reaching "
               "any of the three ends on which delivering nothing is correct (§9.3.3 step 8.1's targetOrigin "
               "check, a target Document §7.5.10 destroyed, and the task's own abrupt completion). It is the "
               "one outcome that is a defect: find the queue it is standing on and why that flow was never "
               "picked");
        (void)sum;
    }
    /* …AND THE CONSERVATION LAW ONE LEVEL UP, WHICH IS ABOUT RECORDS AND NOT ABOUT TASKS. Everything above is
       per (record, TIMELINE) attachment: the three asserts are each about one queue, and the inequality beside
       them compares two counts of attachments. NONE of them can say whether some ONE record was admitted by no
       timeline at all — one record admitted by N timelines pays for N-1 records admitted by none, so a
       shortfall in any of those sums names a loss when it fires and establishes nothing when it does not.
       THIS IS THE LINE THAT CAN, AND ONLY THIS LINE. `zero_delivery` is a GAUGE: while the frontier is live it
       counts records still in flight as indistinguishable from records that are lost, and it FALLS as
       timelines admit them. Three lines above, every live flow has been asserted to hold no pending delivery
       and every finished flow asserted the same at flow_finish — so at THIS instant every attachment this
       instance ever made has been consumed, the gauge cannot fall again, and its value is the number of
       peers' messages this document never received. That is the one number a page cannot distinguish from a
       message that was never sent, which is why it is asserted rather than reported: a count that names a loss
       and lets the session close is the concealment §Offensive-programming forbids, not the measurement.
       WHAT FIRES HERE IS A REFUSAL WITH NO ARM BEHIND IT, and deliver_admits asserts the CONTRADICT half of
       that at the refusal itself, where the reader is standing at the decision. This is the FORECLOSED half's
       only witness and the backstop for both: an arm that was minted and then never picked, a record attached
       to a flow that left the frontier through a path flow_finish does not cover, or a refusal whose covering
       timeline exists and refuses it for a second reason of its own.
       AND IT IS THE ONLY WITNESS FOR A THIRD REFUSAL, WHICH IS WHY THAT ONE DOES NOT ABORT WHERE IT IS MADE.
       A commitment whose arm is OWED (flow.h's FLOW_COMMIT_ARM_ANSWER_OWED) cannot be asserted against at the
       refusal: the arm is minted only if the answering peer answers from a SECOND timeline, so an abort there
       would fire on a peer that answered once, which is a peer behaving correctly. The obligation is recorded
       at the push and the only moment at which it can be known to have gone undischarged is here, with the
       frontier exhausted and the gauge unable to fall again. That is the same reason this line exists at all
       — a loss is a property of the whole session and never of one decision — and it is what makes an
       obligation a sound thing to record rather than a deferral in a field. */
    {
        long d = 0, r = 0, zero = 0;

        engine_routed_census(&d, &r, &zero);
        DCHECK(zero == 0,
               "the frontier was declared exhausted holding routed records that NO TIMELINE of this document "
               "ever admitted — every attachment has been consumed (the walk above asserts no live flow holds "
               "one and flow_finish asserts it of every flow that ended), so each of these was refused by "
               "every timeline it was offered to and the peer's message is simply gone, which the page cannot "
               "tell from a message that was never sent. Each refusal happened at deliver_admits, on one of "
               "three rows, and the census says which: a RECEIVED world CONTRADICTS it and the arm that "
               "should take it was never minted; a FORECLOSED world covers it and the parent that took that "
               "world refused it for a reason of its own; or a cross-instance ANSWER world contradicts it and "
               "the arm for that peer's other timeline is OWED by flow_answer_fork and was never minted, "
               "because the peer never answered again. The third is the one this line is the ONLY witness "
               "for — `routed-delivery-an-arm-is-owed` says at the refusal that the record COULD be lost and "
               "this says that it WAS. Find which by reading the step-unit histogram first and then "
               "re-running the routed world vectors through deliver_admits; for the first two the answer is a "
               "property of the vectors alone, and for the third it is a property of how many timelines the "
               "peer answered from");
        (void)zero;
    }
    /* A SESSION THAT CLOSES OVER LIVE MEMBERS WRITES THEM DOWN FIRST, and until this line it did not — which
       is the §NO BOUNDS drop, reached by the one route that had nothing else standing in front of it. The
       walks above are all about work a member HOLDS (a job, a routed record, an owed answer); none of them is
       about the member ITSELF, and the banner over the per-member walk just above already says the state
       plainly: "the loop
       above can also LEAVE with a flow still alive: every member has reported itself host-owed, so no member
       can be picked, and this line closes the session over the survivors." The survivors then went to
       flow_registry_free with their snapshots, their deltas and their whole unexplored timelines.
       IT IS NOT THE SAME EXIT AS AN EXHAUSTED FRONTIER AND THE GUARD IS WHAT SAYS SO. A frontier that DRAINED
       has no member left to write — every flow ran to its end and flow_finish tore it down — so an empty park
       there is a different statement (cold_park's own banner: it deletes the origin's entry, which is the
       honest report for a residue that was resumed and consumed). Routing by `flow_count()` keeps the two
       answers apart; it is routing and not a fallback by §C-stack's test, because deleting the survivors case
       leaves the question still asked.
       WHICH ROUTE REACHES IT, AND WHY NOTHING DID BEFORE. Three exits stand in front of this line and two of
       them return STALLED — a member the host can still be asked about, and a document a peer holds a
       reference into. What passes all three is a frontier every one of whose members is parked on a request
       the trusted zone REFUSED: it is outstanding to the flow (so the timeline is not torn down) and owed by
       nobody (so `engine_host_owes` correctly answers 0 and there is no bill to hand back). That is §@S's
       search-not-yet-solved — waiting on a per-origin widening that happens in a LATER SESSION — and a
       residue is exactly what carries it there. Before `engine_host_owes` asked the host question, this
       frontier returned STALLED with an empty bill for the rest of the session, which is the livelock that
       fix removed; what it left behind was this drop, which is better and is still a cap. */
    if (flow_count() > 0) {
        engine_park_frontier(ctx, g_sess_cur);
        g_sess_cur = NULL;
    }
    /* AND THE CLAIM IS ASSERTED WHERE IT IS MADE RATHER THAN LEFT TO THE CALL ABOVE BEING REMEMBERED. This is
       the property §NO BOUNDS actually demands of a resting member — STARVE means deprioritize-and-PAGE,
       resumable and cross-session, never terminate — and it is a statement about the FRONTIER at the one
       instant the session stops existing. A member still standing here whose recipe was never written is a
       timeline that ends with the process, and nothing anywhere would say so: `live` counted it, the census
       reported it as merely waiting, and the result document named no residue at all. */
    DCHECK(flow_count() == 0 || engine_frontier_paged(),
           "the session closed over LIVE members whose recipes were never written — every one of them holds a "
           "snapshot, a COW delta and an unexplored timeline, and the instance teardown is about to free them "
           "with nothing anywhere naming the loss. A resting member is deprioritized and PAGED, never dropped; "
           "if a new exit reaches this line it takes the park above it, and if it must not, it is not an exit "
           "that may close a session holding members");
    engine_session_close();
    return ENGINE_STEP_DONE;
}

/* The other end of the `begin`/`step` pair (engine.h has why every host calls it unconditionally when it stops
   stepping, and what is still live in a session left open). The whole of "the session is over" is one function,
   and this is the seam a host outside this file reaches it by. */
void engine_sched_end(void) {
    /* A session that already closed is already ended: engine_sched_step closes the session itself when it answers
       DONE, and a host that stopped stepping because of that DONE is in the state this call establishes. */
    if (!g_sess_live) return;
    engine_session_close();
}

/* One quantum. Returns ENGINE_STEP_DONE when the frontier is empty (the session is closed and its hooks are
 * uninstalled) and ENGINE_STEP_YIELD when the slice expired with the frontier intact. The slice is a budget of CPU
 * actually consumed (solver/quantum.h says what each host can measure), not a cap: nothing is dropped, starved,
 * reordered or forgotten across it, and the next call resumes the same top flow on the same frontier.
 *
 * The slice's bracket is a wrapper because the body has many exits. Opening the budget arms an asynchronous edge
 * (solver/quantum.h) and closing it disarms the edge, so a return that forgot one would leave a CPU timer running
 * over the host's time between two steps: the signal would land while the host pumps its port, raise a request
 * nothing answers, and expire the next slice at its first opcode. A `quantum_end()` before every return is the
 * shape where one is eventually missing; one bracket around one call cannot be, which is also why the slice-span
 * accumulators are here and not at those returns. */
int engine_sched_step(void) {
    int r;
    /* THE EARLIEST READING THIS ENGINE TAKES ON ITS OWN THREAD, and it is the FIRST statement of the function
       rather than a line inside the body for a reason that is arithmetic and not tidiness: everything between
       here and `engine_sched_slice`'s own entry — arming the quantum, restoring the flow stamp and the
       capture route, the stray check — is thread the engine spent, so a span opened past it would report that
       work as the HOST's and the partition below would not sum. See solver/engine.h's `loop_us`. */
    int64_t t_in = quantum_thread_us();

    /* AND THE SPAN EVERY COST THIS ENGINE REPORTS IS A SHARE OF, OPENED FROM THAT SAME READING — see
       `g_instance_us0`, which used to be opened one call deeper and is opened here now so that `g_loop_us`
       below cannot start before it. */
    if (!g_instance_us0_set) { g_instance_us0 = t_in; g_instance_us0_set = 1; }
    /* THE HOST'S OWN THREAD BETWEEN TWO SLICES, CHARGED AT THE READING THAT CLOSES IT — never from a third
       clock read, which would be a different quantity wearing this one's name. `g_slice_exit_us` is the
       PREVIOUS slice's closing reading, carried, so this gap and that slice's own span meet at one number and
       the two accumulators telescope to the whole of `instance_us`. Nothing is charged before the first slice
       has returned, which is what the flag says and is why it is a flag rather than a reserved reading. */
    if (g_slice_exit_set) {
        DCHECKF(t_in >= g_slice_exit_us,
                "solver/engine.c: this slice opened at %lld, BEFORE the previous one returned at %lld — the "
                "two are readings of one thread clock taken in program order, so this is a measure that has "
                "stopped being monotone and `betweenSlicesUs` is about to be charged a negative gap, which "
                "reads as a host that gave the engine more thread than there was",
                (long long)t_in, (long long)g_slice_exit_us);
        DCHECK(t_in - g_slice_exit_us <= INT64_MAX - g_between_us,
               "the host's between-slice total would overflow — its charges are non-negative and it only "
               "climbs, so this is a slice measure that has stopped being microseconds or an accumulator "
               "narrowed under solver/engine.h's `between_slices_us`; either way the partition at "
               "engine_step_unit_runs would fail with a negative half and read as a broken clock");
        g_between_us += t_in - g_slice_exit_us;
    }
    /* THE SLICE ITSELF, COUNTED WHERE ITS BUDGET IS ARMED — solver/engine.h's `slices`. It is the denominator
       `loop_us` and `between_slices_us` would otherwise not have, and the bound the `over_arms` residual
       names: the quantum arms once here and the dispatch loop ends the slice on that same expiry, so at most
       one turn per slice can end on the cooperative clause. Raised BEFORE the body rather than after it
       because the count is of slices ENTERED, which is the population both spans are over and which a body
       that aborts cannot retroactively un-enter. */
    g_slices++;
    quantum_begin();
    /* THE FLOW STAMP AND THE DOM CAPTURE ARE THE SLICE'S, NOT THE SESSION'S — and that is one correction, not
       two, because they are twins over the two halves of the same state (the JS heap and the Lexbor tree).
       They were set in engine_sched_begin and cleared in engine_session_close, which reads like a session-
       scoped flag and IS one — but engine_sched_slice returns YIELD without switching the running flow out, so
       "the session is live" and "a flow is running" are not the same statement and the gap between them is the
       host's own time. Everything the host created there (the ABI's parse of a fetch reply, and then a dup of
       it onto EVERY parked flow's pending register) was stamped with the live generation, so
       `JS_ObjFlowGen(obj) > d->fork_gen` skipped it in every delta forked earlier: a write to that shared
       object by any of those flows was recorded nowhere and survived that flow's unapply. Silent, and
       refcounting kept it from ever crashing.
       IT IS THE WRAPPER THAT MAKES THIS SYMMETRIC, which is the same reason the wrapper exists for the
       quantum: the body has SEVERAL exits and a mark cleared at all but one of them would look correct
       forever. One bracket around one call cannot be got wrong. THE ENUMERATION THAT STOOD HERE IS GONE AND
       THE REASON IS THE SAME ONE IT GAVE FOR ITSELF. It listed "the park's DONE, three yields, STALLED twice
       … and the exhausted path that closes the session" — SEVEN — and said it was "spelled out because it
       was WRONG while the count was right", having named a seeded-candidate yield that had gone and missed
       two exits added since. Measured at this revision, with comments and string literals stripped from the
       body, there are SIX `return` statements: two DONE, two YIELD, two STALLED. So the count has now been
       wrong in both directions, the list has been wrong once, and NOTHING here depends on either — the
       argument is that there is more than one exit. A list of exits in a comment is a census of a function
       that changes, which is what this file's own banner says a comment may not be.
       AND IT IS SUSPEND/RESTORE, NEVER CLEAR/RE-ENTER. The generation is monotonic — every delta's fork_gen is
       a point on that one line — so the next slice resumes at the number this one left, and a restart at 1
       would make a later object compare as older than an earlier fork. */
    /* AND THE CAPTURE ROUTE IS THE THIRD, WHICH THE PARAGRAPH ABOVE LEFT OUT — the one that made the stamp line
       below not merely incomplete but the thing that ARMED the corruption.
       The three marks are one statement in three registers: the STAMP says what generation an object is born
       at, `g_dom_capture` says whether the Lexbor tree is being recorded, and the DELTA says WHOSE head a
       capture lands in. The DOM half turned its recording OFF for the host's time; the JS half turned its STAMP
       to 0 and left the route pointed at whichever flow the slice last switched in — and `cow_set_current` is
       never called between slices, because §scheduler requires the yield to keep the running flow switched in.
       So across the host's own time the capture hook was LIVE and aimed at a stranger's delta, and stamping the
       host's objects BASELINE is exactly what removed the only thing that had been skipping them: a baseline
       object is `flow_gen 0 <= d->fork_gen`, which is the SHARED arm of cow_capture_hook's test.
       WHAT THAT DID, and it is the whole of the `urlList` abort: every property the host wrote on an object it
       had just created was recorded as a CREATION (existed=0) in that flow's head, and the next context switch
       away from it ran cow_unapply_entries, which DELETES a creation. A reply record built by
       fetch_reply_new — or parsed by qjs_provide — therefore arrived at its delivery as `{}`: no urlList, no
       status, no headers, no body, with its refcount intact and nothing to say what had happened. The same
       sentence covers every other object the host builds between slices (qjs_host_answer's parsed answer,
       qjs_route's and qjs_perform's delivery records), which is the list the DCHECK in qjs_step already names.
       SUSPEND/RESTORE for the same reason the generation is: the slice left the route pointed at the flow it is
       still holding across the yield, and the next slice must resume with that flow's captures going where they
       went — clearing it would be a flow running with its writes recorded nowhere. */
    JS_SetFlowGen(g_slice_flow_gen);
    g_dom_capture = 1;
    cow_set_current(g_slice_delta);
    /* THE HOST'S OWN TIME IS BEHIND THIS LINE, and on the first slice so is everything the session setup did
       before any flow existed — the two cases the other three boundaries cannot see. Between two slices the
       host parses a fetch reply into a record, routes a delivery and writes a synchronous answer, all of which
       build JS values; a throw any of them leaves standing belongs to no flow at all, and the slice below would
       hand it to whichever flow it resumes first. `detail` is the session's live flag, which distinguishes
       "before the session was opened" from "between two of its slices". */
    ENGINE_NO_STRAY(g_sess_ctx, "slice-entry: the host's own time between slices (and the session setup before "
                                "the first one)", g_sess_live);
    r = engine_sched_slice();
    /* …AND THE SCHEDULER'S TAIL, which is the fourth segment: HTML §8.1.7.3's end-of-checkpoint hook, the aging
       charge and flow_finish all run after the post-step boundary inside the loop, so this is the only line
       that watches them. Asked BEFORE the marks below are put back, so what it reports is the state the slice
       actually ended in. */
    ENGINE_NO_STRAY(g_sess_ctx, "slice-exit: the scheduler's tail after the last step "
                                "(§8.1.7.3's end-of-checkpoint hook, the aging charge, flow_finish)", r);
    g_slice_flow_gen = JS_FlowGen();   /* the forks this slice took moved it; carry them, never restart */
    JS_SetFlowGen(0);                  /* the host's own time is baseline: what it creates is shared by construction */
    g_dom_capture = 0;
    /* …and it is recorded into NO delta, which is what makes the line above safe rather than merely tidy. The
       save reads whatever the slice left: the flow it is still holding across a yield, or NULL after a close —
       engine_session_close switches its last flow out, and flow_switch_out un-currents as it unapplies, which
       is the same reason that function's own clear of the live stamp is enough for the line above. */
    g_slice_delta = cow_current();
    cow_set_current(NULL);
    quantum_end();
    /* WHAT THIS SLICE COST THE ENGINE'S OWN THREAD, CLOSED AFTER `quantum_end()` BECAUSE DISARMING THE EDGE
       IS THE SLICE'S WORK AND NOT THE HOST'S — the same boundary the marks above are restored at. One
       accumulate at one line, past every one of the body's returns, which is the property the wrapper is for:
       a charge written at those returns would be missing from one of them eventually and the symptom would be
       a slice's whole duration in neither row rather than a crash. */
    {
        int64_t t_out = quantum_thread_us();

        DCHECKF(t_out >= t_in,
                "solver/engine.c: this slice closed at %lld, BEFORE it opened at %lld — two readings of one "
                "thread clock taken in program order, so this is a measure that has stopped being monotone "
                "and `loopUs` is about to be charged a negative span",
                (long long)t_out, (long long)t_in);
        DCHECK(t_out - t_in <= INT64_MAX - g_loop_us,
               "the dispatch loop's slice-span total would overflow — its charges are non-negative and it "
               "only climbs, so this is a slice measure that has stopped being microseconds or an accumulator "
               "narrowed under solver/engine.h's `loop_us`; the reading it feeds does not go absent, it "
               "INVERTS, and a negative `loopUs` reads as an engine that was never given the thread");
        g_loop_us += t_out - t_in;
        /* CARRIED FOR THE NEXT SLICE'S GAP, so the host's time between two slices is measured from a reading
           that was already taken rather than from a third. */
        g_slice_exit_us  = t_out;
        g_slice_exit_set = 1;
    }
    return r;
}

/* A host that has nothing else to do between quanta — the node smoke test — drives the SAME steps in a loop.
   That is a HOST's loop over the one scheduler, not a second scheduler: the state machine is unchanged and a
   quantum boundary is invisible to it. */
/* THE HOST STREAMS WHAT THE RUN IS COSTING, because a run that does not finish reports nothing at all.
   The three cost numbers are published in the result document, which is built when the frontier drains — so a
   run that takes twenty minutes instead of three says exactly nothing about why, which is the state the last
   attempt to measure one ended in. Emitting them as the run goes is the host's own job: between returns from
   the scheduler it pumps messages, interleaves engines, streams findings and snapshots, and this is a finding.
   THE CADENCE IS A COUNT, NOT A CLOCK. A wall-clock cadence would make the output differ run to run for a
   reason that has nothing to do with the engine; a switch count is what the engine actually did, so two runs of
   the same page emit the same lines. It is a reporting interval and not a bound: nothing is dropped, skipped or
   reordered by it, and the loop it sits in is unchanged. */
/* Sized against what a run actually costs rather than guessed: this fixture's whole exploration is under six
   thousand switches, so a cadence in the hundreds of thousands emits nothing and tells nobody anything. */
#define ENGINE_PROGRESS_EVERY 1000

/* THE C ALLOCATOR'S OWN TWO NUMBERS — see engine.h for what each says. `live` is what it has handed
   out and not been given back (quickjs's bytes included, since js_malloc routes to malloc); `arena` is the
   total space it has taken from the system, which in wasm is LINEAR MEMORY AND ONLY EVER GROWS. It is one
   allocator interface with two spellings: emscripten's dlmalloc exports the classic `mallinfo` with size_t
   fields, glibc's `mallinfo` is int-wide and deprecated in favour of the size_t `mallinfo2`. Picking the
   spelling is an ABI question about the platform's malloc, not a choice about what to measure — both answer
   the same two fields, and the int-wide one would silently wrap on a run this size. */
#if defined(__EMSCRIPTEN__)
#define ENGINE_MALLINFO_T struct mallinfo
#define ENGINE_MALLINFO() mallinfo()
#else
#define ENGINE_MALLINFO_T struct mallinfo2
#define ENGINE_MALLINFO() mallinfo2()
#endif

size_t engine_c_alloc_live(void)
{
    ENGINE_MALLINFO_T m = ENGINE_MALLINFO();
    return (size_t)m.uordblks;
}

size_t engine_c_alloc_arena(void)
{
    ENGINE_MALLINFO_T m = ENGINE_MALLINFO();
    return (size_t)m.arena + (size_t)m.hblkhd;   /* brk'd space plus whatever came from mmap */
}

/* THE FRONTIER'S OWN READING — see engine.h for what each row is and for why this is one struct rather than
   twelve getters. It DECIDES NOTHING and it reads no other component: every row is a static of this file, and
   the things a reader wants BESIDE them (the delta chains, the cold tier's byte census, the WFQ's order) are
   their own components' to answer and are composed beside this one in solver/result.c. */
void engine_frontier_census(EngineFrontierCensus *out)
{
    DCHECK(out != NULL, "the frontier census was asked for into no record — a reading of an instant that lands "
                        "nowhere is a census whose caller cannot have taken it at one instant");
    out->finished          = g_finished;
    out->finished_flows    = g_finished_flows;
    out->finished_cands    = g_finished_cands;
    out->sold              = g_flows_sold;
    out->sold_flows        = g_flows_sold_flows;
    out->sold_cands        = g_flows_sold_cands;
    out->forks             = decide_fork_total();
    out->deepest           = g_deepest;
    out->completed         = g_completed;
    out->deepest_left      = g_deepest_left;
    /* THE SEED LENGTH `deepest` AND `completed` ARE READ AGAINST — see EngineFrontierCensus. It is read from
       the SESSION'S table rather than from any flow's `dyn_n`, because a flow's sequence is the seed followed
       by whatever that timeline queued, so per-flow it is a different number per arm and none of them is the
       document's. A closed session reads 0: engine_session_close gives the table back. */
    out->root_programs     = g_root_n;
    out->root_programs_held_at_seed    = g_root_n_held;
    out->root_programs_awaited_at_seed = g_root_n_awaited;
    out->root_programs_refused_at_seed = g_root_n_refused;
    out->prog_starts       = g_prog_starts;
    out->prog_starts_cand  = g_prog_starts_cand;
    out->prog_starts_other = g_prog_starts_other;
    out->prog_queued_cand  = g_prog_queued_cand;
    out->net_prog_queued       = g_net_prog_queued;
    out->net_prog_fetch_asks   = g_net_prog_fetch_asks;
    out->net_prog_fetch_queued = g_net_prog_fetch_queued;
    out->net_prog_xhr_asks     = g_net_prog_xhr_asks;
    out->net_prog_xhr_queued   = g_net_prog_xhr_queued;
    out->claims_met        = g_orphan_claims_met;
    out->claims_unmet      = g_orphan_claims_unmet;
    out->host_asked        = g_host_asked;
    out->host_answered      = g_host_answered;
    out->host_answers_extra = g_host_answers_extra;
    out->host_answers_late  = g_host_answers_late;
    out->host_terminated    = g_host_terminated;
    out->paged_reqs         = g_paged_reqs;
    out->paged_asks         = g_reclaim_asks;
    out->paged_unarmed      = g_reclaim_unarmed;
    out->paged_floor        = g_reclaim_floor;
    /* THE PAYMENT PAIR IS AN INEQUALITY AND IT IS ASSERTED HERE, at the one place both halves are read
       together. `answered` counts the asks this session SETTLED; `asked` counts the ids it minted. A settled
       count above the minted count is not a large number, it is the two counters having stopped describing one
       population, and only their ORDER says so — both read as a healthy run on their own.
       WHAT IT NAMES IS A MINT OR A CREDIT, AND NOT A LIFETIME. This message used to offer "a counter that
       survived a session boundary" as the first of two hypotheses, and that hypothesis is unreachable: all
       four host counters are on engine_session_close's NOT-RESET list together, so a carried total carries
       every term and preserves the order. It cost a reader the wrong search once; a cause that cannot produce
       the symptom is the stale-DFAIL failure with a number attached, so it is gone rather than demoted.
       THE TWO REAL PRODUCERS ARE BOTH CLOSED AND BOTH ARE STRUCTURAL, which is what makes this an invariant
       rather than an expectation: every id is minted by `mint_req` (so a re-issue at a fork is an ask like any
       other), and `g_host_answered` is credited at exactly ONE line — the delivery that finds PEND_HAVE_VALUE
       clear, writes the value and returns — so an id can be settled at most once and a second peer TIMELINE's
       answer goes to `g_host_answers_extra` instead. A fire is therefore one of those two facts having stopped
       being true: an id minted somewhere other than `mint_req`, or a second credit for one entry. */
    /* THE TWO RETIREMENT PARTITIONS, ASSERTED AT THE ONE PLACE ALL SIX ROWS ARE IN ONE HAND — the same
       identity solver/cold.c asserts between its `step_units` histogram and `flows`, and asserted for the same
       reason: a split whose parts do not provably sum to the whole is two numbers that can drift, and drifted
       they are worse than the one number they replaced, because each half then looks like a measurement.
       WHAT IT ACTUALLY CATCHES is a retirement or a sale added LATER that moves the total without stating
       which population it moved. Both totals are written at exactly one line each today and the arm is written
       on the line beside it, so the identity holds by inspection right now — that is the point: the moment a
       second site appears, the reading composed from these rows would silently start describing a frontier
       whose members are not all accounted for, and this is what stops it. `finished` and `sold` are lifetime
       totals of the INSTANCE (neither is on engine_sched_begin's reset path), so no session boundary can
       separate a total from its arms. */
    DCHECKF(out->finished_flows + out->finished_cands == out->finished,
            "the frontier census's retirement rows do not partition its retirement total (%ld exploration + "
            "%ld candidate against %ld finished) — every member is on exactly one side of `Flow.cand_src` for "
            "the whole of its life, so a difference is a flow_finish that moved the total without saying which "
            "population retired. Both halves are written at one line in flow_finish; a second retirement path "
            "is what this fires on",
            out->finished_flows, out->finished_cands, out->finished);
    DCHECKF(out->sold_flows + out->sold_cands == out->sold,
            "the frontier census's sale rows do not partition its sale total (%ld exploration + %ld candidate "
            "against %ld sold) — see the retirement assert above; the label is read BEFORE flow_release frees "
            "it, so a sale path that reads it afterwards gets freed memory rather than a wrong count and this "
            "is where that shows",
            out->sold_flows, out->sold_cands, out->sold);
    /* AND THE TWO TOTALS THEMSELVES PARTITION THE FRONTIER'S DEPARTURES, asserted here for the two above's
       reason and ONE LEVEL UP FROM THEM. Each of those says what its own total is made of; neither says what
       the totals are made OF, so the frontier's departure count and this file's two retirement totals were
       three numbers living in two censuses with no identity anywhere between them — and "this engine retires
       nothing" is exactly the sentence a reader composes by reading two of them at zero and subtracting.
       A subtraction across two censuses that share no identity is not a measurement.

       THE PARTITION IS EXACT BY CONSTRUCTION AND THAT IS WHY IT CAN BE ASSERTED RATHER THAN ESTIMATED.
       solver/flow.c's `flow_remove` is the one line a member ever leaves the frontier on, `flow_release` is
       its one caller, and it has exactly three on a live instance: flow_finish (which raises `g_finished` on
       the line beside it), engine_reclaim_tail (which raises `g_flows_sold`), and flow_registry_free's drain
       (which now raises the third arm). None of the three is reset by a session boundary — the sale totals are
       asserted to CARRY at engine_sched_begin and the arrival/departure pair is not reset by
       flow_registry_init — so no boundary can separate this total from its arms.

       THE THIRD TERM IS NOT DECORATION EVEN THOUGH IT READS 0 AT EVERY CENSUS A HOST TAKES TODAY. Every host
       opens one session per instance and the teardown drain runs after the report is composed, so what the
       term buys is that the identity is true at every instant of the instance's life rather than only at the
       instants the census happens to be called — a host that censuses after a partial teardown gets an answer
       instead of a false fire, and the arm a reader most wants to name is the one that is named.

       WHAT IT CATCHES is a departure path added later that moves the frontier's count without saying which
       population left, and that is not a hypothetical here: reclamation gated on anything other than
       completion is a FOURTH arm by construction, so the next diff in this area is precisely the diff this
       fires on if it forgets to declare itself. */
    DCHECKF(out->finished + out->sold + (long)flow_departures_teardown() == (long)flow_departures(),
            "the frontier's departure causes do not partition its departure count (%ld finished + %ld sold + "
            "%lld gone with the registry against %lld departures) — flow_remove is the one line a member ever "
            "leaves the frontier on and flow_release is its one caller, so a difference is a FOURTH exit that "
            "moved the total without crediting an arm, and every reading composed from these rows is then "
            "about a frontier whose members are not all accounted for",
            out->finished, out->sold,
            (long long)flow_departures_teardown(), (long long)flow_departures());
    /* AND THE SEED TABLE'S PARTITION, asserted here for the same reason and about a table built ONCE per
       session rather than per flow: `held` is a row whose source text this instance already has and `awaited`
       is one whose bytes the reply door still owes, which is the whole content of the pair. A difference means
       a row was added to the table by a path that did not say which side it was on, and the side that matters
       is the one `replyAsked` is read against — an unlabelled row there silently lowers the bundle's own share
       of the reply door and makes a run that learned nothing read as one that reached a request. */
    DCHECKF(out->root_programs_held_at_seed + out->root_programs_awaited_at_seed +
                out->root_programs_refused_at_seed == out->root_programs,
            "the root document's seed rows do not partition its own count (%d held + %d awaited + %d refused "
            "against %d rows) — the arms are written beside the total, on the columns engine_seed_scripts "
            "branches on, so a difference is a second path into that table and `replyAsked` is about to be read "
            "against a bundle share that is not the bundle's",
            out->root_programs_held_at_seed, out->root_programs_awaited_at_seed,
            out->root_programs_refused_at_seed, out->root_programs);
    /* AND THE PROGRAM-START PARTITION, asserted for the reason the two above are and at the same place. It is
       the ONLY identity these four rows have, which is stated here because the missing one is the tempting
       one: `prog_queued_cand` is an ASK and `prog_starts_cand` counts STARTS ACROSS TIMELINES, and a fork
       copies an unstarted row into the arm it makes — so a started count above the asked count is that copy
       and not a broken number, and an inequality between them would fire on a healthy branch. */
    DCHECKF(out->prog_starts_cand + out->prog_starts_other == out->prog_starts,
            "the frontier census's program-start rows do not partition its start total (%ld candidate + %ld "
            "other against %ld starts) — a start is of exactly ONE row and that row's kind is read on the line "
            "beside the total, so a difference is a second place a program can start that moved the total "
            "without saying which population it moved",
            out->prog_starts_cand, out->prog_starts_other, out->prog_starts);
    /* AND THE TWO REPLY DOORS' OWN PAIRS, ASSERTED HERE BECAUSE THIS IS THE ONE PLACE ALL FIVE ROWS ARE READ
       TOGETHER. Each door's queue is a SUBSET of the replies it examined — both raises happen in one
       straight-line block for one reply, and nothing forks between them — so a queued count above its own ask
       count is not a large number, it is the two counters having stopped describing one population, and it is
       exactly what a door that raises its queued arm and forgot its ask looks like. That forgetting is the
       one thing four separate entries cannot prevent by construction, so it is asserted instead. */
    DCHECKF(out->net_prog_fetch_queued <= out->net_prog_fetch_asks,
            "the fetch() reply door queued more programs than it was shown replies (%ld queued against %ld "
            "asks) — both are raised in one straight-line block for one reply record, so the queue side is a "
            "SUBSET by construction and a door that raises engine_note_net_prog_fetch_queued without having "
            "raised the ask is about to publish a numerator with no denominator under it",
            out->net_prog_fetch_queued, out->net_prog_fetch_asks);
    DCHECKF(out->net_prog_xhr_queued <= out->net_prog_xhr_asks,
            "the XMLHttpRequest reply door queued more programs than it was shown replies (%ld queued against "
            "%ld asks) — both are raised in one straight-line block for one reply record, so the queue side "
            "is a SUBSET by construction and a door that raises engine_note_net_prog_xhr_queued without "
            "having raised the ask is about to publish a numerator with no denominator under it",
            out->net_prog_xhr_queued, out->net_prog_xhr_asks);
    /* AND THE TWO ARMS AGAINST THE ONE ENTRY, WHICH IS AN INEQUALITY AND SAYS SO. `engine_queue_fetched_script`
       raises the total and has a THIRD caller on purpose — test_forced.c's `loadScript` host edge, which
       stands in for a `<script src>`-shaped door — so an equality would fire on that fixture and the residue
       is a quantity a reader READS rather than one this line requires to be zero. What it catches is the
       direction that cannot be innocent: an arm above the total is a door that queued a program WITHOUT going
       through the one compile entry, which is the second compile door §A-superseded-system-is-DELETED forbids
       arriving as an observation rather than as a mechanism. */
    DCHECKF(out->net_prog_fetch_queued + out->net_prog_xhr_queued <= out->net_prog_queued,
            "the reply doors' queued arms exceed the programs the one compile entry built (%ld fetch + %ld "
            "xhr against %ld at engine_queue_fetched_script) — a door crediting itself with a program that "
            "entry never saw has queued it somewhere else, which is a SECOND compile door beside the working "
            "one and is what these rows exist to make visible",
            out->net_prog_fetch_queued, out->net_prog_xhr_queued, out->net_prog_queued);
    /* AND THE REFUSAL EDGE'S OWN PARTITION, which is the row that makes a zero `sold` READABLE. Every call to
       engine_reclaim_tail leaves by exactly one of three doors — declined because the safepoint is not armed,
       answered at the frontier's floor, or a sale that gives back exactly one flow — so the three arms are the
       whole of the asks by construction. WHAT IT CATCHES is a fourth door added to that hook without a
       counter: the reading composed from these rows would then attribute its refusals to one of the three that
       did not happen, which is worse than the single ambiguous zero they replaced, because each arm would
       still look like a measurement. `asks` is also the only row here that can distinguish "the allocator
       never refused" from "the pager was consulted and could do nothing", and those are the two states a
       reader of `sold: 0` has been unable to tell apart. */
    DCHECKF(out->paged_unarmed + out->paged_floor + out->sold == out->paged_asks,
            "the frontier census's reclaim rows do not partition the asks the allocator made (%ld unarmed + "
            "%ld at the floor + %ld sold against %ld asked) — engine_reclaim_tail returns through exactly "
            "three doors and each counts itself on the line it leaves by, so a difference is a fourth exit "
            "added without a counter and every reading of the pager's behaviour is now attributing those "
            "refusals to a door they did not go through",
            out->paged_unarmed, out->paged_floor, out->sold, out->paged_asks);
    DCHECKF(out->host_answered <= out->host_asked,
            "the frontier census was taken with more host answers (%ld) than host asks (%ld) — the two count "
            "one population (an id this instance minted, and the ONE delivery that settled it), so "
            "`answered > asked` means either an id was minted outside mint_req and never counted as an ask, "
            "or one request was credited as settled twice — a second peer TIMELINE's answer is not a second "
            "payment and belongs in hostAnswersExtra (%ld), which is where the multi-timeline case goes",
            out->host_answered, out->host_asked, out->host_answers_extra);
}

static int (*g_provider)(JSContext *ctx);
void engine_set_provider(int (*provide)(JSContext *ctx)) { g_provider = provide; }

/* …AND WHETHER THIS HOST WANTS THE FRONTIER'S RESIDUE — see engine.h. A seam, not a policy: the answer is the
   host's and the moment the park is TAKEN at is the engine's. */
static int (*g_park_hook)(void);
void engine_set_park_hook(int (*want_park)(void)) { g_park_hook = want_park; }

/* THE CENSUS, FOR EVERY HOST WHOSE OUTPUT IS A STREAM OF LINES — one cadence, one order, one emitter.
 *
 * WHY THIS IS A FUNCTION AND NOT A BLOCK IN A LOOP, AND IT IS A CORRECTION RATHER THAN A TIDY-UP. The five
 * lines below stood inside `run_scheduler`, which the smoke fixture reaches through `engine_run` and NOTHING
 * else does — the ABI's hosts drive `engine_sched_step` through `qjs_step` and never enter that loop. So the
 * five censuses were COMPOSED on every sample of every run and PRINTED only by the fixture, which is
 * §Testing's "measure what the shipped path writes, not what a harness prints" with the writer on the wrong
 * side of it for the SECOND time in one file: the block's own banner records the first, where these rows were
 * computed for the product and printed for the smoke, and moving the numbers into this block fixed the
 * COMPOSITION and left the EMISSION exactly where it was.
 *
 * WHAT THAT COST, MEASURED ON THE SHIPPED PATH RATHER THAN ARGUED. `test_forced.c --abi`, driven with one
 * `document` record over its own channel, ran a page to a DRAIN — 9808 context switches, 774 flows, 4644 jobs
 * and a 14-row fork table — and wrote `@QUANTUM` once, its bills, and `@RESULT`. Not one census line. The
 * same host on a page whose frontier does NOT drain inside a budget writes `@QUANTUM` and nothing else at all,
 * so a run exploring thousands of worlds and a run wedged on one member are the SAME SILENCE from outside.
 * That is the only thing this entry changes: both are still the same run; only one of them is now legible.
 *
 * WHO CALLS IT AND WHO DELIBERATELY DOES NOT. The block's own banner already states the rule — "a host that
 * publishes a DOCUMENT reads the census out of it, and a host whose output IS lines must print it when it is
 * TAKEN or it is not in the output at all" — and it names exactly two hosts in this tree: `run_scheduler`
 * (the smoke) and `test_forced.c`'s `--abi` arm (the native ABI, whose trusted zone reads its stdout as a
 * record stream). The wasm ABI hosts — `route.mjs`, `solvergate.mjs`, `pagecensus.mjs`, the extension's
 * bridge — read the RESULT DOCUMENT, which carries these same bytes from these same composers, so a line for
 * them would be a second spelling of a number they already hold.
 *
 * WHERE IT RUNS AND HOW OFTEN, STATED HERE BECAUSE AN OBSERVER RUNS AT THE FREQUENCY OF WHAT IT OBSERVES.
 * On the HOST's own time, between two steps, never inside a slice — so it creates no JS value and the flow
 * stamp and the capture route are down, which is what makes `result_heap_json`'s runtime walk legal here. The
 * cadence is `ENGINE_PROGRESS_EVERY` units of `engine_work_done()` (forks + flows + jobs + switches) or a NEW
 * CANDIDATE, whichever comes first, and a sample is O(frontier members): solver/flow.h's own note says it
 * weighs every member twice, and prices ITSELF on the line it prints — read `scanCensusWeights` against
 * `scanNextWeights` on the @WFQ line of the very run you are reading for the share of all frontier-weighing
 * that went to the REPORT rather than to the run. An instrument that cannot be priced from its own output is
 * the one §Testing says is not a measurement; this one can be, on every sample, by its reader.
 *
 * THE FIRST SAMPLE IS UNCONDITIONAL AND THAT IS LOAD-BEARING, not an artifact of `last_cands` starting at -1.
 * It is what separates THREE states a reader otherwise cannot tell apart from a killed run's output: NO census
 * line at all is a host that never reached its first round (or a binary without this entry); EXACTLY ONE is a
 * frontier that has not moved `ENGINE_PROGRESS_EVERY` units nor minted a candidate since — a wedged run, said
 * POSITIVELY, with the numbers it wedged at; MANY is a run exploring, and the series says how. An absent line
 * and a zero line are different facts (solver/result.c says so about every row of @COLD), and this is that
 * rule applied to the sample itself.
 *
 * AND EACH OF THOSE THREE STATES IS READABLE OFF ONE LINE RATHER THAN OFF THE WHOLE STREAM, which is what the
 * sample ordinal is for. The paragraph above is a statement about a COUNT OF LINES, so only a reader holding
 * the log entire can make it — and CLAUDE.md's standing instruction is to read a run's TERMINAL census, which
 * a series of length ONE satisfies with its OPENING sample and says nothing whatever about having done so. A
 * reader who greps one line out of a build log, or reads one echoed by the trusted zone, gets the state before
 * anything happened and has nothing on the line to tell them. So every line states the ordinal of the sample it
 * belongs to: the greatest ordinal in a log IS the series length, and a last line reading 1 is the wedged state
 * this paragraph describes rather than an end state. It is spliced by the emission below and is NOT a
 * composer's row — see the comment there for why a fact about a POSITION IN A STREAM may not ride bytes that
 * also go on a document, and why one of the five lines does not carry it.
 *
 * THE CADENCE IS STATIC AND NOT A CALLER'S LOCAL, which is the one behaviour change here beyond the caller
 * set. `engine_work_done` is a LIFETIME counter that this file refuses to reset at a session boundary, so a
 * cadence restarting per call would be a reporting interval keyed on a clock that does not restart with it.
 * `long` rather than the `int` the local carried, for the same reason: the counter is a long and a run that
 * outlives an `int` would wrap its own reporting interval.
 *
 * AND IT ANSWERS WHETHER IT SAMPLED, WHICH IS THE ONE FACT A LINE-STREAM HOST CANNOT RE-DERIVE WITHOUT
 * KEEPING A SECOND COPY OF THIS CADENCE. solver/engine.h states what that answer is for and why a caller may
 * not compute it for itself; what it is used for TODAY is the other half of the sentence this banner already
 * makes — the result DOCUMENT is composed when the frontier drains or stalls, and a real page's does neither,
 * so a host that streams its five lines and not its FINDINGS has made the run legible and left the product's
 * own answer on the floor. `test_forced.c`'s `--abi` arm spends this return on `qjs_emit_partial`. */
int engine_census_emit(void)
{
    static long next = ENGINE_PROGRESS_EVERY;
    static int  last_cands = -1;
    /* THE SAMPLE ORDINAL, STATIC FOR THE CADENCE'S OWN REASON. The emission below states what it is and why no
       composer publishes it; what makes it a LIFETIME count rather than a per-call one is the paragraph above
       about `next` — `engine_work_done()` is not reset at a session boundary, so an ordinal that restarted
       while the cadence it numbers did not would give two samples of one process one number. `long` for the
       same reason that counter is one. */
    static long seq = 0;

    /* THE PRECONDITION THAT MAKES EVERY LINE BELOW LEGAL, NAMED AT THE CALLER'S MISTAKE. Each of the five
       composers walks state the SESSION owns — the frontier, the pager's tables, the runtime behind
       `g_sess_ctx` — so a host that asks for a census outside a live session is asking about nothing, and
       `result_heap_json`'s own null check would report it as a missing runtime one component further on. */
    DCHECK(g_sess_live && g_sess_ctx != NULL,
           "a census was asked for with no live session — every row of these five lines is a walk of the "
           "frontier, the pager and the runtime this session holds, so a host asking here is reporting on "
           "state that has been torn down or was never seeded");

    /* Either enough work has happened to be worth a line, or the SEARCH grew — a new candidate is the event
       that changes what the rest of the run will cost, so it is worth saying when it happens. */
    if (engine_work_done() < next && solve_candidate_count() == last_cands)
        return 0;
    while (engine_work_done() >= next) next += ENGINE_PROGRESS_EVERY;
    last_cands = solve_candidate_count();
    /* THE SAMPLE IS NUMBERED BEFORE IT IS COMPOSED, so the four emissions below carry ONE ordinal and the lines
       of a block are joinable by it rather than by the order they happen to arrive in. Raised on its own
       statement and never inside a condition: an increment is a side effect and §Offensive-programming forbids
       one in a DCHECK's condition, which is exactly the mistake a counter asserted at its own read invites. */
    seq += 1;

    /* THE FOUR CENSUSES, EACH PRINTED FROM THE ONE COMPOSER THAT ALSO PUTS IT ON THE RESULT DOCUMENT
       — solver/result.c's `result_swap_json`/`result_cold_json`/`result_heap_json`/`result_wfq_json`
       and solver/decide.c's `decide_fork_json`. Not four renderings of four structs: one composer per
       census, two emission sites, and the bytes are the SAME BYTES, so the two can never disagree.

       WHY THEY MOVED HERE, AND IT IS THE SAME SENTENCE FOR ALL OF THEM. Every row below was a static of
       this file with exactly one consumer — a printf inside `run_scheduler`, which `engine_run` reaches
       and nothing else does — so the pager's own accounting, the runtime's heap decomposition, the cost
       of a context switch and the frontier's provenance were computed on every census of every
       production run and printed on NONE of them: §Testing's "measure what the shipped path writes, not
       what a harness prints" with the writer on the wrong side of it. Sixty-nine numbers, not one of
       them ever read off a real page.

       AND THAT REPAIR FIXED THE COMPOSITION AND LEFT THE EMISSION, WHICH IS WHY THE SENTENCE ABOVE IS
       REWRITTEN RATHER THAN DELETED — a reader who re-derives it will re-introduce the printf inside one
       host's loop. The clause it used to carry ("this loop runs for exactly one host … the extension's
       ABI drives `engine_sched_step` directly and never enters this function") was TRUE of the block and
       is what made these five lines a fixture-only stream: the numbers reached the product's result
       DOCUMENT and the lines reached only the smoke, so a run that never drains — which is every real
       page inside any budget anyone has given one — printed nothing at all. This is a FUNCTION for
       exactly that reason, and the banner above it names both of its callers and the two hosts that
       deliberately do not call it.

       A HOST WHOSE OUTPUT IS A STREAM OF LINES STILL PRINTS ITS OWN LINE, and that is not a second
       reporting mechanism: it is the rule result.h states for a page error and test_forced.c states
       for the @S array — a host that publishes a DOCUMENT reads the census out of it, and a host whose
       output IS lines must print it when it is TAKEN or it is not in the output at all.

       THE @PROGRESS LINE IS GONE AND ITS ROWS ARE NOT. `switches`, `flows` and `candidates` were the
       result document's `_switches`/`_flows`/`_candidates` under a second spelling; `objects` and
       `heapKiB` were the @HEAP line's own rows; `live` and `sold` are the frontier's and are in the
       cold census, where `finished` beside them is what tells a frontier that RETIRED its flows from
       one that PAGED them; `forks` and the fork table are `_forkAt` and its total. What is deleted
       outright is the three that were a SAMPLE of one flow rather than a census of anything — the
       running flow's script cursor, and the sink and payload of whichever candidate happened to be
       switched in. `deepest` and `completed` answer the cursor's question as facts about the DOCUMENT,
       and the @S array already carries every candidate's sink and payload; a sample of one flow at one
       instant is not a measurement the next reader can compare against anything. */
    {
        char *swap = result_swap_json(), *cold = result_cold_json();
        char *heap = result_heap_json(g_sess_ctx), *wfq = result_wfq_json(), *forks = decide_fork_json();

        CHECK(swap && cold && heap && wfq && forks,
              "a census could not be composed — these five lines are the whole of what this host says "
              "about the frontier it is draining, and a run that continues without one reports its "
              "cost, its heap and its order as an absence that reads exactly like a zero");
        /* ONE PRINT PER MARKER, because a marker is what a reader MATCHES on: build.mjs greps
           `^@COLD (\{.*\})$` and `^@WFQ (\{.*\})$` to drive its stuck-run discriminators, and a
           single format string carrying five of them puts four marker names in the middle of an
           emission where nothing looking for an emission's marker can see them. The bytes on the
           stream are identical; what differs is whether the stream is READABLE as five records.
           AND THE MARKER STAYS A LITERAL AT THE HEAD OF ITS OWN FORMAT STRING, which is why the ordinal below
           is spliced at each line rather than once inside a helper taking the marker as an argument:
           engine/fieldgate.mjs reads a stream marker as `@TAG` at the very START of an emission, so a marker
           handed to a helper is a marker that namespace no longer sees WRITTEN — and a written marker read as
           unwritten is a finding in the one gate that pairs this stream's producers with its readers.

           EACH LINE STATES THE ORDINAL OF THE SAMPLE IT BELONGS TO, SPLICED IN FRONT OF THE COMPOSER'S FIRST
           ROW. It is written HERE and is a row of no composer, because those bytes are read at TWO emission
           sites — this stream and solver/result.c's own document — and an ordinal is a fact about a POSITION IN
           A STREAM, which a document is not. A composer row would answer a count of census LINES on a document
           whose host emits none, and that zero is a REGIME rather than a count: one field answering two
           questions, which is the shape §A-FIELD-A-CONSUMER-DEFAULTS calls a plausible datum. A composer does
           not know it is being streamed; the thing that streams it does, and owns the number.
           IT IS NOT A SECOND SPELLING OF `workDone`, WHICH result_wfq_json ALREADY PUBLISHES. That row says
           WHEN a sample was composed and this says WHICH sample it is, and only the second is UNIQUE: the gate
           above has a second arm, so a NEW CANDIDATE emits a sample without `engine_work_done()` having
           advanced at all and two samples can carry one `workDone`. That is what makes the ordinal the key the
           lines of one block are JOINED by — result_wfq_json's own note is that two censuses share no identity
           unless a row holds one — and it is why emitting `workDone` here as well would put a DUPLICATE KEY on
           the @WFQ line, which a JSON parser resolves silently to whichever of the two came last.
           THE SPLICE STEPS ONE BYTE PAST THE COMPOSER'S OPENING BRACE, so the composer's bytes go out VERBATIM
           as a suffix and the payload is still ONE object for `^@COLD (\{.*\})$` to match whole. The comma is
           CONDITIONAL because a composer may legitimately answer an EMPTY object — result_wfq_json does so for
           an empty frontier, as a positive statement that nothing was standing — and an ordinal followed by
           `,}` is not a smaller answer but a line no reader can parse: engine/trusted.mjs takes an unparseable
           payload as the two grammars having parted rather than as a line to drop.

           @FORKAT IS DELIBERATELY NOT ONE OF THEM, AND THAT IS A FINDING ABOUT THAT LINE'S OBJECT RATHER THAN A
           NARROWING HERE. Its payload is not a record of named rows: it is a TABLE whose keys are PARTITIONED
           BY THEIR FIRST BYTE — solver/decide.c writes a constraint key opening on concolic_ident_compose's
           decimal length prefix, a mechanism row on `(`, an unnamed fork site on `~` and its own two bounds on
           `_` — and engine/build.mjs's `forkReading` classifies every key exhaustively and THROWS on a fourth
           spelling, deliberately, because a key it cannot name is mass filed under whichever population it
           resembles. `{}` there is also the positive statement that nothing forked, which a spliced row would
           end. So that census is placed in its block by the @COLD line it is printed beside, which is what that
           reader's own landed residual on this pairing already asks for.
           RESIDUAL: a reader who greps `^@FORKAT` alone still has no ordinal. What the next diff builds is a
           fourth namespace in `forkReading` — a census-owned member admitted BY NAME, as its two bounds are —
           and its empty-table sentence rewritten to mean "the ordinal and no rows". Its absence shows as a
           @FORKAT line that cannot be placed in a series without counting the @COLD lines above it. */
        DCHECK(swap[0] == '{' && swap[1] != '\0' && cold[0] == '{' && cold[1] != '\0'
                   && heap[0] == '{' && heap[1] != '\0' && wfq[0] == '{' && wfq[1] != '\0',
               "a census composer handed this host bytes that are not a closed object — the four emissions "
               "below splice the sample ordinal in front of the composer's first row by stepping one byte past "
               "its opening brace, so a body that does not open on `{` goes out with its first character eaten "
               "and a body one byte long has no closing brace to splice in front of. Either way the line "
               "parses into rows nobody wrote, which is worse than an absent census: build.mjs and "
               "engine/trusted.mjs each read an unparseable payload as the two grammars having parted. A "
               "conjunction over four bodies for the reason the CHECK above it is one — they are composed on "
               "the two lines above this comment and emitted on the four below it, so which composer to open "
               "is named by which of those four objects is malformed and a reader is standing in the block "
               "either way");
        printf("@SWAP {\"censusSeq\":%ld%s%s\n", seq, swap[1] == '}' ? "" : ",", swap + 1);
        printf("@COLD {\"censusSeq\":%ld%s%s\n", seq, cold[1] == '}' ? "" : ",", cold + 1);
        printf("@HEAP {\"censusSeq\":%ld%s%s\n", seq, heap[1] == '}' ? "" : ",", heap + 1);
        printf("@WFQ {\"censusSeq\":%ld%s%s\n", seq, wfq[1] == '}' ? "" : ",", wfq + 1);
        printf("@FORKAT %s\n", forks);
        free(swap); free(cold); free(heap); free(wfq); free(forks);
    }
    fflush(stdout);
    /* THE SAMPLE HAPPENED — said AFTER the flush rather than at the gate above, because what a caller does
       with this answer is write MORE of its own stream at the same instant, and a `1` handed out before these
       five lines were on the wire would order a host's output by whichever `printf` ran first. */
    return 1;
}

static void run_scheduler(JSContext *ctx, char **bodies, char **srcs, const ScriptType *types,
                          const ScriptSchedule *sched, lxb_dom_element_t **els, int n, int forking,
                          const char *recipes) {
    int r;
    /* THE RESIDUE THIS HOST WAS HANDED, or NULL. This line used to say the cold tier's resume "belongs to the
       host that has an IndexedDB", and that was a claim about STORAGE standing in for a claim about the
       SCHEDULER: seeding from a residue is engine_sched_begin's own alternative to seeding a boot flow, and a
       host with a file is as much a store as a host with an object store. Passing NULL from here made the
       resume path unreachable from every host but the extension's, which is exactly the shape §SECURITY.md
       names one level up — a mechanism that can be written, reviewed and self-tested with no process able to
       run it. The store is the caller's business; the choice is the scheduler's, and it is made in one place. */
    engine_sched_begin(ctx, bodies, srcs, types, sched, els, n, forking, recipes);
    for (;;) {
        /* DOES THIS HOST WANT THE RESIDUE? Level-1 eviction is the HOST's decision — it is the only zone that
           can see the other documents' engines and the summed working set — so it is a SEAM here exactly as
           the stall payment is, and for the same reason: the scheduler holds no idea of what memory pressure
           is, and the host holds no idea of what a flow is. A host that never evicts installs nothing, which
           is every existing caller.
           IT TRUNCATES NOTHING, which is what keeps it out of §NO BOUNDS. The park WRITES every member of the
           frontier and the caller stores it, so what this seam decides is WHEN the residue leaves memory and
           never how much of it survives — which is precisely the difference between paging and a cap.
           ASKED AT THE TOP OF THE LOOP, so a host that answers yes gets the park taken at the next step
           boundary with no flow switched in, which is the only moment every flow's decision state is in its
           own blob rather than in decide.c's globals. */
        if (g_park_hook && g_park_hook())
            engine_request_park();
        r = engine_sched_step();
        if (r == ENGINE_STEP_DONE)
            break;
        /* THE HOST PAYS EVERY SLICE. It used to pay only at a STALL, and that was recorded here as a known
           defect with the fix written out in order; this is that fix, and the reason it could not be taken
           before is one line in solver/pending.h that has now been corrected.
           WHAT THE OLD SCHEDULE COST, and it is the answer to why this document never got deeper than its
           second <script>. A stall is reached only when the pick finds NO runnable member, so under a stall-only
           seam a flow's reply is conditional on EVERY OTHER FLOW in the document also becoming blocked. One
           flow's progress is then a function of 511 siblings' states — a cross-flow coupling this scheduler
           forbids everywhere else — and the coupling gets WORSE as exploration succeeds, because every fork adds
           a member that must also block before anybody is paid, and a fork RE-ISSUES its parent's unanswered
           synchronous request under a fresh id (engine_sibling_assemble), so the set to be blocked on is
           regenerated by the very thing that makes progress. The leading flow — the one furthest into the
           document, the only one that can reach a lazily-loaded chunk — is the flow this hurts most: it is the
           first to block on the host and the last to be answered.
           THE PRODUCT NEVER HAD IT, which is why this host was the odd one out rather than the canary. The
           extension's bridge pulls qjs_pending and qjs_host_requests after EVERY non-DONE return — a stall and
           an ordinary quantum are paid on the same schedule there, which is why main.c carrying the stall code
           separately changes what the host KNOWS and not when it pays — so in the extension a blocked flow is
           answered at the next quantum. This driver now speaks the same schedule.
           AND THE CLAUSE THAT STOOD HERE — "which is also what makes it a fair oracle for the product: a
           difference in findings between the two hosts should be a difference in the ENGINE, and a payment
           schedule is not one" — IS TRUE OF `run_scheduler` AND FALSE OF THE NATIVE BINARY, because that
           binary has TWO drivers and only this one was fixed. Its `--abi` arm is a host of the production
           `qjs_*` entries and paid inside `if (step == ENGINE_STEP_STALLED)`, which is the schedule this
           paragraph's own first half names as the defect. It is rewritten rather than deleted because the
           reasoning is right and is what a reader re-derives: a driver repaired at ONE of two call sites
           reads as a host repaired, and nothing named the second.
           THE `--abi` ARM NOW POLLS, IN SOURCE, AND THE COST OF THAT REPAIR WAS NOT WHAT THIS RECORD'S OWN
           RETIREMENT CLAUSE IMPLIED. `pays on the same schedule` names an OUTCOME and reads as a one-line move
           of `abi_pay` out of its stall branch, and that move alone HANGS: the channel is half-duplex at BOTH
           ends, so `engine/trusted.mjs` writes to an instance only while it is `stalled` and the child blocks
           in `getdelim` until `go`. Moving the read without the write would have had the child block forever
           on a zone that had already decided not to speak to it. The repair is therefore a second VERB on the
           one channel — `poll`, answered in the same turn with whatever is READY and never awaited — which
           keeps the deadlock rule exactly as written (the zone writes only in answer to a line the child wrote
           asking to read) and changes only which condition makes the child ask. Two files, two ends, one
           commit; the JS half is additive, so a child that never writes `poll` is answered as it always was.
           WHAT IS NOT ESTABLISHED IS THAT IT WORKS, and this record stays until somebody observes it. The
           claim above is about the INSTALLED artifact, a build retires it and no amount of reading does, so
           the observation is named with the actor who can make it: a COORDINATOR builds, drives one real SPA
           through the `--abi` arm, and reads the census for `replyAnswered` equal to `replyAsked` with
           `rowsAwaitingBytes` 0 — the numbers the WASM host already answers for the same document.
           OBSERVED, AND THE PAYMENT HALF IS DISCHARGED. A coordinator built the native binary and drove
           `https://gitlab.com/explore` through this `--abi` arm over engine/trusted.mjs under a 900 s
           RLIMIT_CPU, thread-cpu quantum 12 ms: `replyAsked 45 == replyAnswered 45` with `replyOutstanding 0`
           at EVERY ONE of the run's 15 censuses, against the 1-of-43 with 42 frozen across 35 consecutive
           censuses this record was written about. The livelock this paragraph describes is gone from the
           built artifact and not merely from the source.
           AND THE OTHER HALF OF THE CONDITION ABOVE WAS NEVER A REACHABLE ONE, WHICH IS THIS RECORD'S OWN
           DEFECT AND NOT THE ENGINE'S. `rowsAwaitingBytes` 0 reads as a fact about the DOCUMENT and is a SUM
           OVER LIVE MEMBERS: engine_rows_awaiting_bytes walks every flow and counts that flow's own
           DYN_SCRIPT_SRC rows, and solver/engine.h says so at the accessor in its own words — "IT IS SUMMED
           PER MEMBER AND THE FAN-OUT IS THE ANSWER … one document row awaited by N members counts N times",
           with no inequality holding in either direction. So on any forking frontier the row is the
           document's outstanding programs MULTIPLIED by the member count, and demanding 0 of it demands that
           no member anywhere still holds an unfetched program — which is a condition about the SEARCH's size
           and not about the reply door this record is about. Measured on the same run, and it is the
           accessor's paragraph confirmed rather than a new finding: the quotient held at 26.98 rows per
           member at EVERY census while the frontier grew 4034 → 4993, a 24% growth with the ratio flat to
           two decimals, so the document had ~27 outstanding programs throughout and every member carried its
           own copy. A retirement condition that names a per-member sum is the §READ-THE-ACCESSOR defect
           written into the one clause a reader acts on, and the check that catches it costs one read of the
           header one file over — which is where the correct reasoning already was.
           RETIREMENT: the payment claim is MET and this record is REWRITTEN RATHER THAN DELETED, because a
           reader who re-derives the stall-only schedule from ENGINE_STEP_STALLED's own definition will
           re-introduce it. What remains open is NOT this record: the run aborted at solver/flow.c's
           comparator surrogate before it could ask for those ~27 programs, so whether the engine REQUESTS
           them is a question about the order and is recorded where that abort fires.
           AND THE STALL-ONLY SCHEDULE IS NOT MERELY SLOWER THERE, IT IS UNREACHABLE. ENGINE_STEP_STALLED is
           returned only where the run queue is EMPTY, so a frontier forking on unknowns never produces it and
           the payment is never reached at all — the coupling this paragraph describes is not a delay but a
           LIVELOCK, since the one reply that did arrive is what starts the forking that prevents the next.
           MEASURED over the archived censuses, partitioned by HOST rather than by revision: one real SPA (26
           of its programs awaiting bytes at seed, 43 reply-door asks) read `replyAnswered 43` with
           `rowsAwaitingBytes 0` in 3 of 3 runs under the WASM artifact, and `replyAnswered 1` with
           `replyOutstanding 42` held across 37, 35, 35 and 18 CONSECUTIVE censuses in 4 of 4 runs under the
           `--abi` arm, forks climbing 3 -> 6242 in the same series while the count stayed at 1. A second real
           page read 27 of 27 under the WASM artifact. Those runs were relayed as a page whose bytes never
           arrived and a fetch path that was broken; the bytes arrive, and what does not is the host's payment.
           SO A NATIVE `--abi` CENSUS AND A WASM ONE ARE NOT COMPARABLE ON ANY REPLY-CONSUMING ROW until
           `replyOutstanding` is read: a GAUGE standing at a positive value across a whole census series is
           this schedule, and it is not a delivery defect however much it reads like one.
           RETIREMENT: this record goes when a census taken from the `--abi` arm of a BUILT artifact reads
           `replyAnswered == replyAsked` on a document the WASM host answers in full, because the sentence
           above is then true as originally written and no second driver exists for it to be false of. The
           SOURCE half of that condition is met and is deliberately not enough: every host of the `qjs_*`
           entries now pays at a slice boundary rather than only at a stall, and this record is about what the
           shipped binary does.
           WHY IT ABORTED WHEN IT WAS TRIED, and it was neither the provider nor the reply record. `pending_ready`
           answered YES for an ANSWERED HOSTREQ, so a synchronous answer arriving between two slices made the
           register look deliverable, flow_step called the reply delivery, and it swap-removed the rendezvous
           record and pushed it through a `resolve` capability it does not have. At a stall that state cannot
           exist, because the asking machine consumes its answer on the very next step; at a quantum boundary
           nothing guarantees the asking machine runs next. Both halves are fixed at the root — the predicate
           asks the KIND (solver/pending.h) and the delivery LEAVES a synchronous answer where its machine will
           take it (flow_deliver_one_reply) — so the shape this branch was avoiding no longer exists to be
           avoided. */
        {
            /* THE HOST'S OWN TIME, ASSERTED WHERE IT BEGINS — the same claim qjs_step makes at the ABI boundary,
               made here because this driver has no ABI and pays the provider directly. A provider BUILDS
               objects (a reply record, a peer's answer) and the three marks that decide what happens to those
               objects belong to the flow that just ran, not to the host: with the stamp up they are stamped as
               a flow's and no later write to them is ever captured, and with the capture ROUTE up every field
               the provider writes is recorded as a creation in that flow's head and DELETED by its next
               unapply — which is how a reply record reached its delivery as `{}`. Two registers, one fact, and
               the failure mode of each is silent in the opposite direction. */
            DCHECK(JS_FlowGen() == 0 && cow_current() == NULL,
                   "the host is being paid with the last flow's generation or its capture route still up — "
                   "every object the provider now builds either belongs to that flow (so nobody else's write "
                   "to it is ever captured) or has its every field recorded as a CREATION in that flow's head, "
                   "which the next context switch deletes: the record arrives at its delivery empty");
            int filled = g_provider ? g_provider(ctx) : 0;
            /* AND THE PROVIDER IS ITS OWN SEGMENT, which is the split the slice-entry boundary's own report
               asked for. That boundary watches the WHOLE of the host's time as one region, and the host's time
               has two halves that fail in different places: the PROVIDER, which builds JS values (a reply
               record, a peer's answer, a cross-agent operation's text), and everything else this loop does
               between two steps — the census lines, the progress emission, the cold-tier preview — which
               reads. A completion standing here belongs to the first; one that reaches slice-entry with this
               silent belongs to the second, and there is no third half. `detail` is what the payment filled,
               so a leak on a payment that answered nothing reads differently from one that answered records. */
            ENGINE_NO_STRAY(ctx, "host-provider: the host's reply/answer/operation payment", filled);
            /* AND THE PAYMENT WAS COMPLETE, asserted at the seam instead of inferred from a census line six
               minutes later. This provider answers out of its OWN tables — a reply record it builds, a peer's
               answer it stands in for — so unlike the extension's it has no asynchronous half and nothing it
               may legitimately still owe once it has run. A record outstanding here is therefore one it was
               never handed or one it walked past. `blocked` and `owed` cannot say which: they report the LEVEL
               of unanswered work, and a frontier that keeps issuing requests reads identically whether the host
               is paying promptly or has silently skipped a record. This says which, at the moment it happens,
               and it names the two registers separately because they are two questions.
               UNCONDITIONAL NOW, rather than inside the stall branch: the claim is about what the provider
               leaves behind and is equally true of a slice at which nothing was outstanding (both registers are
               empty either way), so guarding it by the stall would only have narrowed where it can fire. */
            if (g_provider) {
                DCHECK(*engine_host_requests() == '\0',
                       "the smoke host paid and a SYNCHRONOUS request is still outstanding — this provider "
                       "answers every record it is handed out of its own tables, so the flow blocked on this "
                       "one is parked at a call site nothing is going to resume, and its whole timeline is lost "
                       "with nothing but a `blocked` count to say which record it was");
                DCHECK(*engine_pending_fetches() == '\0',
                       "the smoke host paid and a reply is still owed — the same silence one register over: the "
                       "flow that issued this fetch keeps its snapshot and its continuation and is never handed "
                       "the body, so everything the page does behind that reply is missing from the run");
            }
            /* NOBODY CAN SUPPLY IT, so this driver stops driving — and STOPPING IS NOT THE SAME AS THE SESSION
               ENDING, which is why the close is below rather than here. The frontier that remains is real
               (every flow parked on a reply that never came), and it is handed to the teardown as SNAPSHOTS.
               The condition is unchanged: a STALL the payment could not move. A stall with nothing filled is a
               frontier waiting on something outside this host's tables — in the smoke driver, the referenced-
               document case, which no reply can answer. */
            if (r == ENGINE_STEP_STALLED && filled == 0)
                break;
        }
        engine_census_emit();
    }
    /* THE SESSION IS CLOSED ON EVERY EXIT, not only on the one that drained. `engine_sched_step` closes it when
       it answers DONE; the OTHER way out of this loop — a stall nobody can supply — left the session LIVE, with
       the scheduler's hooks still installed over a frontier the host was about to tear down and one flow still
       switched in. That is exactly what engine_session_close's own comment says must not differ between the two
       ways a session ends, and the way it ended here was the one nothing said.
       THE `r != ENGINE_STEP_DONE` THAT STOOD HERE IS GONE, and its absence is the point: it made "did this exit
       already close the session?" a question every host answers for itself, which is one hand-copied condition
       per driver and a §NO-STUBS-grade hole in whichever one gets it wrong. The wpt runner got it wrong the
       first time it grew a second exit, and 26 files aborted on flow_release. `engine_sched_end` answers it
       once, so a host's rule is now "call it when you stop stepping" with nothing to get right. */
    engine_sched_end();
}

/* EXPLORE: seed boot OR a parked residue, then drain the frontier, forking at every concolic branch. */
void engine_run(JSContext *ctx, char **bodies, char **srcs, const ScriptType *types,
                lxb_dom_element_t **els, int n, const char *recipes) {
    /* Named residual. Not covered: this entry's caller (test_forced.c) passes no schedule column, so it is
       re-derived here with document_exec_scripts' own call, every row being a parse product (parser-inserted,
       force async false). Next diff builds: engine_run takes DocScripts.sched and this derivation is deleted.
       Absence shows as: two derivations of one column, which agree only while every row is a parse product. */
    ScriptSchedule *sched = NULL;

    if (n > 0 && els) {
        sched = (ScriptSchedule *)calloc((size_t)n, sizeof *sched);
        CHECK(sched != NULL, "engine: OOM deriving the schedule column for engine_run's inventory");
        for (int i = 0; i < n; i++)
            sched[i] = els[i] ? script_block_schedule(els[i], types[i], /*parser_inserted*/true,
                                                      /*force_async*/false)
                              : (bodies[i] ? SCRIPT_SCHED_IMMEDIATE : SCRIPT_SCHED_PARSER_BLOCKING);
    }
    run_scheduler(ctx, bodies, srcs, types, sched, els, n, 1, recipes);
    free(sched);
}

/* WHETHER THE FRONTIER HAS ALREADY GONE DOWN IN THIS TEARDOWN. It is not agent state and is deliberately not
   declared as any: it is zero everywhere outside the window between the two calls below, and what it exists for
   is that the window is the one place a host can put the browser half in the wrong half of it. */
static int g_frontier_released;

/* See solver/engine.h — THE FRONTIER, AND IT GOES DOWN WHILE THE BROWSER IS STILL STANDING. */
void solver_frontier_free(JSContext *ctx)
{
    DCHECK(ctx != NULL, "the frontier was released against no realm — its flows hold JSValues and its deltas "
                        "hold the writes those flows made, so there is a realm they belong to and a release "
                        "with none would drop them against nothing");
    DCHECK(!g_frontier_released,
           "the frontier was released twice in one agent — the second call walks a registry the first already "
           "freed, and every flow it would have named is one the first call already tore down");
    /* THE @S SEARCHES GO BACK BEFORE THE FRONTIER, AND THE ORDER IS THE WHOLE OF WHY. A search takes a
       reference on the DECISION CHAIN at the moment its sink is detected — add_pending's decide_freeze_path,
       the re-injection point every later candidate is seeded from — and that reference is neither a flow's nor
       the running flow's globals'. flow_registry_free ends in decide_free, whose `g_dec_seg_live == 0` is a
       statement that every such reference has already been given back, and solve_free is the give-back: with
       it AFTER, any session that detected a sink while standing on a non-empty decision path aborted the
       renderer at teardown. That is every real page, because a page that has taken one branch before its first
       tainted sink is the ordinary case rather than the corner — measured on testing/corpus/control, which
       forks on `window.__FLAGS.admin` before its first markup sink and hit that assert on every run, while the
       same sink with no branch in front of it froze an EMPTY path, held no segment, and completed.
       IT IS THE SHAPE core/platform.h STATES AND THE ONE concolic_free's OWN ASSERT RELIES ON: the claimant
       releases at its own release, and the holder asserts that it did — so the fix is the position of the
       give-back, never a softer assert. Nothing in the frontier's teardown reads this file's store (flow_release
       does not call solve_flow_end; the scheduler does), so the claim can be given back before it. */
    solve_free();
    flow_registry_free(ctx);
    g_frontier_released = 1;
}

/* See solver/engine.h — the solver half's release column, in reverse dependency order. */
void solver_agent_free(JSContext *ctx)
{
    DCHECK(ctx != NULL, "the solver's agent state was released against no realm — the frontier's flows hold "
                        "JSValues and its deltas hold the writes those flows made, so there is a realm they "
                        "belong to and a release with none would drop them against nothing");
    /* THE FRONTIER IS ALREADY GONE, AND IT WENT WHILE THE BROWSER WAS STILL STANDING — see solver_frontier_free
       in solver/engine.h for the whole of why, and core/platform.c's own census assert for the other end of
       it. Reaching here with the frontier live means a host ran this column where it used to be one call: the
       lines below release the emission tables and the source registry, and the flows that would have been torn
       down after them are suspended INSIDE browser components this agent's platform release has already
       finished with. */
    DCHECK(g_frontier_released,
           "the solver's agent state was released with the FRONTIER STILL LIVE — a suspended flow is a live "
           "activation of the browser, so its teardown re-enters the very components platform_agent_free has "
           "already given back; call solver_frontier_free BEFORE platform_agent_free and this after it");
    /* NOTHING OF THE BROWSER HALF IS STILL REGISTERED HERE, AND THAT IS AN ORDERING STATEMENT RATHER THAN A
       TIDINESS ONE. Each of these slots holds a C function pointer INTO a browser component, and every browser
       component was released by platform_agent_free, which runs BEFORE this call — so a slot still set is the
       scheduler holding a callback into state that has already been given back. That is exactly the defect
       core/agent_state.h records for Indexed Database §2.7.1's cleanup: found by reading, invisible to both of
       JS_FreeRuntime's censuses, because a stale handle gave its reference back and then kept the number.
       The claimant releases; this asserts that it did. */
    DCHECK(g_timer_hook == NULL,
           "§8.1.7's timer step was still registered when the solver's agent state was released — "
           "core/timing/timer.c claimed it and gives it back at timer_free, which is a row on "
           "core/platform.h's release column and therefore runs first");
    DCHECK(g_rendering_hook == NULL,
           "§8.1.7.3's in-parallel half was still registered when the solver's agent state was released — "
           "core/rendering/rendering.c claimed it and gives it back at rendering_free");
    DCHECK(g_idle_hook == NULL,
           "the event loop's idle rung was still registered when the solver's agent state was released — "
           "core/scheduling/idle_callback.c claimed it and gives it back at idle_callback_free");
    DCHECK(g_checkpoint_hook == NULL,
           "the end-of-microtask-checkpoint step was still registered when the solver's agent state was "
           "released — core/indexeddb/idb_transaction.c claimed it and gives it back at idb_transaction_free");
    DCHECK(g_wrap_stats == NULL,
           "the wrapper-census hook was still registered when the solver's agent state was released — "
           "core/dom/node.c claimed it and gives it back at node_free, under the `element` row's cascade");
    DCHECK(g_docdone_hook == NULL,
           "§13.2.7's document-load lifecycle step was still registered when the solver's agent state was "
           "released — core/dom/document.c claimed it at document_init and gives it back at "
           "document_agent_free, which is a row on core/platform.h's release column and therefore runs first");
    DCHECK(g_link_connected_hook == NULL,
           "§4.6.8.20's connected time for parsed link elements was still registered when the solver's agent "
           "state was released — core/html/html_link.c claimed it at html_link_declare and gives it back at "
           "html_link_free, which html_element_free calls and core/dom/element.c drives from platform.h's "
           "release column, and that column runs in full before this one");
    /* THE ORPHAN COUNTERS AND THE GENERATION THEY LAST WALKED AT, given back with the frontier they describe.
       THE ARGUMENT THAT USED TO JUSTIFY `g_orphans_driven`'S RESET IS RETIRED AND ITS REPLACEMENT IS NOT THE
       SAME SENTENCE. It read: the ordinal names an argument's source identity ({orphan7.arg0}), so a second
       session on top of the first would mint identities that collide with the previous session's constraints.
       Every clause of that is now false in the direction that MATTERS — engine_orphan_call names each unknown
       `{orphan<locator>.argN}` from JS_OrphanHash, which is a fact about the BUNDLE, so this counter names
       nothing at all, and §ONE-global-frontier is explicit that two drives of one body SHARING the name is
       correct rather than the aliasing an ordinal was avoiding. A reader who re-derived the retired reason
       would conclude the opposite of the design, which is why it is rewritten here rather than deleted.
       WHAT SURVIVES IS THE SENTENCE `g_orphan_asks` ALREADY HAD, and it covers both: they are per-SESSION
       numbers, and the result document is the AGENT's rather than any one document's — engine_sched_begin
       asserts that a host opens exactly ONE session per instance, so the two boundaries coincide and this
       release is what keeps them coinciding. The GENERATION is a different fact and keeps its
       own reason — it is a memo about ONE runtime's heap, so a second agent that kept it would skip a walk
       over a heap it never walked. All are single words: the release is what makes them a session's rather
       than a process's.
       `g_orphan_asks` IS ON THIS LINE AND WAS NOT, which was a defect in the commit that introduced it rather
       than an omission with no consequence. The hosts that take a runtime down and bring another up per file
       run many sessions in one process, so a counter left standing reports the PROCESS's total under a name
       the result document spells per AGENT — and this one in particular: the whole of what `asked` is for
       is that `asked == 0` means NO FLOW IN THIS SESSION ever reached the end of its own work. A carried-over
       count makes that read `asked > 0` for a session that never asked at all, which is the exact reading the
       pair exists to distinguish, inverted, in the direction that looks healthy. */
    g_orphans_driven = 0; g_orphan_asks = 0;
    /* AND THE ASK'S OWN PARTITION WITH IT, for the sentence directly above: they sum to `g_orphan_asks` and a
       summand a second session kept would report the previous session's asks under this one's total and break
       the equality at the first census of a host that runs many sessions in one process. */
    g_orphan_asks_memo = 0; g_orphan_asks_empty = 0; g_orphan_asks_took = 0;
    g_orphan_gen_seen = 0; g_orphan_gen_valid = 0;
    /* …AND THE ROUND TRIP'S THREE NUMBERS BESIDE THEM, for the same sentence: what an inherited drive did with
       its recipe is a fact about ONE session's residue, and the latch is a fact about ONE frontier's members.
       A second session that kept any of them would report the first session's round trip as its own and would
       skip the routing walk on a frontier that has just been rebuilt with claims in it. */
    g_orphan_claims_met = 0; g_orphan_claims_unmet = 0; g_orphan_claims_closed = 0;
    attr_shadow_free(ctx);
    endpoint_free();
    /* THE PAGE-ERROR CONSOLE, BESIDE THE OTHER EMISSION TABLE AND AFTER THE WHOLE PLATFORM. It had no release
       at all, so both its tables leaked their rows on every run of every host and its route LATCH would have
       handed a second agent an answer no host of that agent ever gave. WHAT MAKES THIS POSITION SAFE is that
       every writer of that console is already gone, and there are only two kinds: a FLOW (`flow_step`,
       `flow_run_one_job`, a module's rejection reaction), all released by `solver_frontier_free` above this
       column; and a BROWSER HOST EDGE — HTML §8.1.4.6 "Runtime script errors"'s console hook in
       core/events/report_exception.c, §8.1.4.7/§8.1.6.4's report hook in core/html/unhandled_rejection.c —
       each a slot its component declares to core/agent_state.h and nulls on core/platform.c's release column,
       which `platform_agent_free` has already run in full. So nothing left in this agent can report a page
       error into a table this line is about to free. */
    result_free();
    /* The metrics block, after every composer above has run its last composition and before the next agent's
       concolic_init asserts it zero. */
    metrics_agent_release();
    /* THE CONCOLIC VALUE COMPONENT IS LAST, and the position is the argument. Its SOURCE REGISTRY is what a
       report asks for a source's browser delivery — the encode set, the address component, the reproduction
       mechanism — so every line above may still read it while it renders and releases what it holds. It is
       also the one registry in this half that BROWSER components write into: each row is a claim given back by
       its claimant on core/platform.h's release column, which platform_agent_free ran before this call, so the
       assert at its first line is a checked statement about ITS CLAIMANTS rather than about this component —
       and it names the one that did not finish out of the row, which is why nothing here has to list them. */
    concolic_free();
    /* AND THE TEARDOWN WINDOW CLOSES. A host that takes a runtime down and brings another up per file runs many
       agents in one process, so a latch left standing would make the NEXT agent's solver_agent_free read a
       frontier release that belonged to the previous one — which is the exact question this pair exists to
       ask, answered with a fact about a program that has already ended. */
    g_frontier_released = 0;
}

