/* The dispatch loop — see engine.h. */
#include "core/fetch/fetch.h"
#include "core/events/listener_act.h"   /* the user act a driven listener is reached by, stated by a PoC */
#include "core/html/input_value.h"      /* the person typing into a text control, a drive's typing act */
#include "core/clipboard/data_transfer.h"   /* a drop act's drag data store */
#include "core/events/drag_event.h"        /* a drop act's DragEvent */
#include "core/file/file_system.h"         /* a drop act's user file */
#include "core/dom/user_scroll.h"           /* a scroll act's task */
#include "core/intersection_observer/intersection_observer.h"   /* a scroll act's target */
#if defined(__EMSCRIPTEN__)
#include <emscripten/heap.h>   /* emscripten_get_heap_max: the linear memory's ceiling, RAM pressure's limit */
#else
#include <sys/resource.h>      /* RLIMIT_AS: the native process's address-space ceiling, RAM pressure's limit */
#include <unistd.h>            /* sysconf(_SC_PAGESIZE): the unit of /proc/self/statm's mapped size */
#endif
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
#include "solver/c_alloc.h"   /* the C heap's running total, RAM pressure's reading */
#include "solver/solve.h"
#include "solver/witness_verify.h"
#include "core/frame/open_again.h"   /* the attacker opening the page again */
#include "solver/request_observe.h"
#include "solver/post_index.h"
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
#include "core/html/referrer_policy_attribute.h"   /* HTML §2.5.5: a script element's referrer policy */
#include "core/html/nonce_attribute.h"   /* §4.12.1.1 reads el's [[CryptographicNonce]] slot, not its attribute */
#include "core/loader/script_fetch.h"   /* HTML §8.1.4.2: where a fetched body becomes a script's source text */
#include "core/loader/import_map.h"   /* HTML §8.1.4.2's resolve a module integrity metadata */
#include "core/loader/document_scripts.h"   /* §4.12.1.1's schedule, for engine_run's caller that states none */
#include "core/frame/window_message.h"   /* the receiving half of a routed `windowproxy.post` */
#include "core/events/message_event.h"   /* the event an attacker post hands a driven `message` listener */
#include "core/frame/remote_op.h"        /* the receiving half of a cross-agent operation: what performs it */
#include "core/frame/remote_object.h"    /* …and the grammar its completion crosses back in */
#include "core/frame/navigable.h"   /* HTML §7.3.1 Navigables: tree order over this agent's navigables */
#include "core/frame/window_proxy.h" /* which realm of this agent a document name is, in the asking timeline */
#include "core/frame/document_lifecycle.h"   /* HTML §7.4.6.1: what a navigation does to the Document it replaces */
#include "core/timing/thread_work.h"   /* the census of thread work the clock does not see */
#include "core/timing/event_loop.h"   /* HTML §8.1.7.3 Processing model: the clock's second mover — the work the
                                         running flow retired, which the interpreter reports and this file relays */
#include "solver/dom_cow.h"   /* the DOM half of time-travel — swapped per-flow alongside the heap COW delta */
#include "solver/cold.h"      /* the park and the resume themselves; their census is composed in result.c */
#include "solver/quantum.h"   /* the slice's bracket, the host's claim on the thread, and the pricing clock */
#include "solver/rest_unit.h" /* the collector's step, a rest unit of its work */
#include "solver/reclaim.h"   /* …and its RAM edge: which allocators can sell a flow rather than fail */
#include "solver/reply_decode.h" /* …and what the reply body itself teaches — see engine_provide */
#include "solver/pending_index.h" /* which records a reply answers, without walking the frontier to find out */
#include "solver/endpoint.h"    /* the @H surface, and one of the three tables no leak walk in this runtime can see */
#include "solver/novelty.h"     /* the identity set novelty_free gives back beside the @H surface */
#include "solver/attr_shadow.h" /* …and the taint shadow, whose entries are GC objects and are leaked by two hosts */
#include "solver/metrics.h"     /* the agent's metrics block, released with the agent */
#include "check.h"
#include <time.h>
#include <stdlib.h>
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
 * empty string. Its integrity is step 2's resolve a module integrity metadata, which
 * engine_pending_module_url writes over the element's. */
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
           "§4.12.1.1's script metadata was composed for a node that is not an element — every park reaching this "
           "line holds an element of a live tree, so the pointer names freed or foreign memory");
    if (el) {
        h.nonce_slot = nonce_attribute_current(ctx, el);
        /* An unknown nonce is an undecided predicate: §2.5.6's slot holds a JS value so that
           `s.nonce = location.hash.slice(1)` survives as a source, and CSP §6.7.2.3 step 3.1 asks whether it is
           identical to a policy's base64-value, which an unconstrained domain answers neither way. The arm must
           fork; a coerced example would report one world's verdict as both (core/html/html_link.c, same read). */
        DCHECK(!concolic_is(h.nonce_slot),
               "a `script` element's [[CryptographicNonce]] is an unknown external value, and CSP §6.7.2.3 step 3.1 "
               "compares it for identity, which an unconstrained domain cannot answer. Fork the request here into "
               "the matching and non-matching arms, each carrying the narrowed domain, as an opaque operand forks "
               "every other predicate");
        /* The slot read can refuse, and a refusal is not OOM. engine/qjs/quickjs.c's string constructor bounds
           the length it is handed and throws a RangeError for an oversized one; that arrives here as an
           exception in the slot, and `JS_ToCString` of an exception is NULL, the same NULL an allocation failure
           gives. The two are separated so the message names a corrupt length rather than memory. */
        if (JS_IsException(h.nonce_slot)) {
            /* Fatal by design: a dev build has already aborted inside the string constructor with the caller's
               stack above it, and in release no narrower record exists, since §4.12.1.1's metadata grades the
               request and a nonce nobody could read would report a load a browser refuses. */
            CHECK_FAIL("§4.12.1.1: a `script` element's [[CryptographicNonce]] could not be read — the slot held "
                       "bytes whose length the engine's string constructor refused, a corrupt or stale attribute "
                       "length rather than an allocation failure");
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
    DCHECKF(0, "a park whose reply becomes a program states the kind %d, which `pending_kind_is_program` admits and "
               "this mapping has no door for — a program kind added to that predicate must say which mechanism "
               "composes its address", kind);
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
/* The requests the engine declined for having no address bytes (engine.h): pending_park_request's, and the
   synchronous rendezvous engine_host_request_unaddressed opens refused. */
static long g_reply_unaddressed;
long engine_unaddressed_total(void) { return g_reply_unaddressed; }

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
    DCHECK(req->address != FETCH_ADDRESS_UNPLACED,
           "a request parked without stating whether its URL is bytes it may be sent to — the producer holds "
           "the address value (core/fetch/fetch.h's FetchAddressClass)");
    /* The asking document travels with the address: engine_pending_fetches names it to the trusted zone, which
       judges the request against that document's record. */
    DCHECK(pending_get_int(e, PEND_DOC) != 0,
           "a request parked without naming its asking document — every park records it before this door "
           "(document_doc of the asking realm, or a document script row's `dyn_doc`), and the trusted zone "
           "judges the request against that document's record");
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
       rather than turned into it. And the mode, read by §4.1 step 7's Integrity Policy disjunct below and by
       the trusted zone, which decides from it what the page may read of the reply (`fetch_mode_token` is
       fatal on the zero, so release cannot send one). Both are the consumer's half of the guards `fetch_owe` makes
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
        int driven = (int)pending_get_int(e, PEND_DRIVEN);
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
        endpoint_record(ctx, req->method, uv, NULL, 0, NULL, prov, driven, door);
        JS_FreeValue(ctx, uv);
    }
    /* Fetch §4.1 Main fetch step 6, run before the key is composed. The reply seam is keyed on (method, mode,
       url) and step 6 rewrites the url, so a PEND_URL composed from the pre-upgrade address would park the
       request under an address the host is never asked for, and the entry would silently never settle. It is
       asked at this consumer as well as at the producers, because the parks in this file pass no producer's
       door; it is idempotent (core/fetch/fetch.h), so a producer having run it changes nothing. */
    /* Fetch §4.1 step 4 runs here only for the program loads, whose park is the first point that judges them;
       every other kind reaching this door was built by a component that ran step 4 itself (core/fetch/fetch.h). */
    if (pending_kind_is_program(pkind))
        fetch_main_report_csp(ctx, req->url, req->destination, req->metadata);
    up_url = fetch_main_upgrade(ctx, req->url, req->destination, req->initiator);
    use_url = up_url ? up_url : req->url;
    /* The mode is written FIRST because it is part of the identity the outstanding set keys on when the
       address and method arrive (solver/pending.c). Fetch §4.1 Main fetch decides from it what the page may
       read of the reply, so a `fetch(u)` and a `<script src=u>` are two requests with two answers.
       `fetch_mode_token` is fatal on the unplaced zero, the release backstop under the DCHECK above. */
    pending_set(e, PEND_MODE, JS_NewString(ctx, fetch_mode_token(req->mode)));
    /* The referrer policy and the referrer, also before the address because both are in the key: Fetch §4.1
       "Main fetch" gives an empty policy the policy container's, and a "client" referrer is resolved against
       this realm's Document (core/fetch/fetch.h says why that is this engine's). */
    {
        char *referrer = fetch_referrer_for_wire(ctx, req->referrer);
        pending_set(e, PEND_REFERRER_POLICY,
                    JS_NewString(ctx, fetch_referrer_policy_for_wire(ctx, req->referrer_policy)));
        pending_set(e, PEND_REFERRER, JS_NewString(ctx, referrer));
        free(referrer);
    }
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
    /* What the URL is, on the record, so the join can assert that only bytes are ever listed. */
    pending_set_int(e, PEND_ADDRESS, (int)req->address);
    /* AN ADDRESS WITH NO BYTES IS NOT FETCHABLE, and nothing asks a host for it: its URL is the display shape of
       an unknown, this engine's annotation, never a URL. It is declined here as the trusted zone declines a
       request it will not make, and flow_decline_fork asks §4.1 step 12's same-origin test over the unknown
       first: both origins keep an arm parked on the request (a later session whose run has an example grounds
       the address and fires it) and one taking §5.6's network error, and the other origin's arm also explores
       the success flow_success_arm builds. The test is computed here, in the asking realm, whose base URL and
       origin it reads. The record keeps the address as its key and the reason as its account; the join never
       lists a declined record. */
    if (req->address == FETCH_ADDRESS_UNKNOWN) {
        DCHECK(concolic_is(req->address_value),
               "a request whose address is unknown reached the park without the unknown it stands for — the "
               "producer states both from the one value the page passed (core/fetch/fetch.h)");
        pending_set(e, PEND_ADDRESS_VALUE, JS_DupValue(ctx, req->address_value));
        pending_set(e, PEND_SAME_ORIGIN, fetch_main_same_origin_unknown(ctx, req->address_value, req->address_url));
        pending_set(e, PEND_DECLINED, JS_NewString(ctx, PENDING_DECLINED_UNADDRESSED));
        g_reply_unaddressed++;
        free(up_url);
        return;
    }

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
       the host is shown. The entry stays outstanding until engine_provide answers it. The request leaves the
       engine here, past §4.1's upgrade and blocking steps, which is the point a request fire reads. */
    case SCHEME_FETCH_NETWORK:
        request_observe_park(&sf);
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
    DCHECK(f != NULL, "engine_pending_fetch_url: a live fetch issued outside a running flow — the flowless caller is "
                      "the pre-boot baseline tree walk; issue the fetch from the boot flow, not from the walk");
    e = pending_push(&f->pending, FLOW_PENDING_RESOLVE, flow_path_forced(f), flow_path_pinned(f), flow_path_driven(f),
                     /*parser_inserted*/0, flow_work_seq_next());   /* a `fetch()` or an XHR — page code composed it */
    pending_set(e, PEND_RESOLVE, JS_DupValue(ctx, resolve));
    pending_set(e, PEND_VALUE, JS_DupValue(ctx, value));
    /* Which document asked: the realm this chokepoint was entered with. `flow_step`'s realm is the session's,
       so a delivery re-deriving it would compile the reply in whichever realm the session is rooted at. It is
       carried rather than defaulted because `PEND_DOC`'s default 0 names no document. */
    pending_set_int(e, PEND_DOC, (int)document_environment_doc(ctx));
    /* The address and what travels with it go through the one door, which also asks §4.3. Requests reaching
       here always get NETWORK: `fetch_owe` and `fetch()` run §4.3 with their own `deliver` closure and return
       before touching this register when it answers locally, so a local answer here means one of those seams
       stopped asking. With no address, the caller supplied the value up front and owes the host nothing. */
    if (url) pending_park_request(ctx, e, req);
    else pending_set(e, PEND_HAVE_VALUE, JS_TRUE);
    JS_FreeValue(ctx, e);
}

/* Fetch §2 Infrastructure's "abort a fetch controller", for §5.6 step 11's abort steps: the running flow stops
   waiting on the park `deliver` names, and the trusted zone is told to end the send once no live flow still
   owes that request. Records are shared across forks, so a sibling that did not abort keeps the record in the
   outstanding set and the send goes on for it. A park that already holds its reply is left to deliver, since
   processResponse returns at locallyAborted; a park the zone declined was never sent. The notice is
   `fetch.cancel<TAB>METHOD<TAB>MODE<TAB>REFERRER_POLICY<TAB>REFERRER<TAB>URL`, the key engine_provide reads. */
void engine_pending_fetch_cancel(JSContext *ctx, JSValueConst deliver) {
    Flow *f = flow_running();
    int i, n;

    DCHECK(f != NULL, "a fetch's abort steps ran outside a running flow — an abort algorithm runs inside the "
                      "signal-abort steps of the flow that aborted, and the park it ends is on that flow's register");
    for (i = 0, n = pending_count(f->pending); i < n; i++) {
        JSValue e = pending_entry(f->pending, i), r = pending_get(e, PEND_RESOLVE);
        int mine = pending_get_int(e, PEND_KIND) == FLOW_PENDING_RESOLVE && JS_IsObject(r) &&
                   JS_VALUE_GET_PTR(r) == JS_VALUE_GET_PTR(deliver);
        int answered = pending_get_int(e, PEND_HAVE_VALUE) != 0;

        JS_FreeValue(ctx, r);
        JS_FreeValue(ctx, e);
        if (!mine) continue;
        if (!answered) engine_cancel_entry(ctx, f, i);
        return;
    }
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
    /* An address resolved from a string: an unknown `src` records its shape and parks nothing. */
    req.address = FETCH_ADDRESS_CONCRETE;
    /* Fetch §2.2.5's metadata, which for a dynamic import is the referrer's. HostLoadImportedModule gets "the
       descendant script fetch options given originalFetchOptions, url, and settingsObject", an algorithm of
       §8.1.4.2 Fetching scripts: its step 1 copies the options, so the cryptographic nonce is the importing
       script's (the element whose program this flow runs), and its step 2 replaces the integrity metadata with
       resolve a module integrity metadata's answer from the global's import map. flow_dyn_el is NULL for a
       program no `<script>` produced (a lazy chunk, an @S candidate, a `javascript:` URL), whose options carry
       the empty string. */
    ScriptCspMeta meta;
    JSValue e;
    char *integrity;
    DCHECK(f != NULL, "a dynamic import was issued outside a running flow");
    meta = script_csp_meta(ctx, flow_dyn_el(f));
    req.metadata = meta.m;
    integrity = import_map_module_integrity(ctx, url);
    req.metadata.integrity = integrity;
    req.metadata.integrity_len = strlen(integrity);
    /* The credentials mode is the referring script's, copied by the same step (see
       `script_credentials_mode`); a row no `<script>` produced takes the default script fetch options. */
    req.credentials = script_credentials_mode(flow_dyn_el(f));
    /* The mode: HTML §8.1.4.2's fetch a single module script creates the request with mode "cors" outright,
       so this is the module arm and never the element's. */
    req.mode = script_request_mode(SCRIPT_TYPE_MODULE, flow_dyn_el(f));
    /* The referrer is Fetch §2.2.5's "client"; the referrer policy is the referring script's options', copied by
       §8.1.4.2's descendant script fetch options, and a program no `<script>` produced has the empty string. */
    req.referrer = "client";
    req.referrer_policy = flow_dyn_el(f) ? referrer_policy_attribute_keyword(flow_dyn_el(f)) : "";
    DCHECK(url != NULL && *url, "a dynamic import parked with no module URL for the host to fetch");
    e = pending_push(&f->pending, FLOW_PENDING_MODULE, flow_path_forced(f), flow_path_pinned(f), flow_path_driven(f),
                     /*parser_inserted*/0, flow_work_seq_next());   /* a dynamic `import()` — §16.2.1.8 is reached by RUNNING code */
    pending_set(e, PEND_RESOLVE, JS_DupValue(ctx, resolve));
    /* The reject half, which carries the load's failure: this is the only park whose failure is a rejection
       rather than an event at an element. HTML §8.1.6.7.3 HostLoadImportedModule's onSingleFetchComplete makes
       a null moduleScript "ThrowCompletion(a new TypeError)", and ECMAScript §13.3.10.3 ContinueDynamicImport
       calls "promiseCapability.[[Reject]]" with it. Without this the delivery could only settle `resolve` with
       empty source text, evaluating an empty module, so `import(u).catch(h)` would never run `h`. */
    DCHECK(JS_IsFunction(ctx, reject),
           "a dynamic import parked with no reject capability — JS_NewPromiseCapability mints both halves, and the "
           "failure arm settles the import through this one");
    pending_set(e, PEND_REJECT, JS_DupValue(ctx, reject));
    /* And which document asked, as every other fetch park records it: the importing realm is the asking
       document's, and HTML §7.5.11 step 2 cancels this load with that document's other fetches. */
    pending_set_int(e, PEND_DOC, (int)document_environment_doc(ctx));
    /* And §4.3 Scheme fetch with it: `import("data:text/javascript,export default 1")` is answered from the
       specifier's own bytes, settling the promise JS_ModuleLoadPending is about to be handed. */
    pending_park_request(ctx, e, &req);
    script_csp_meta_free(ctx, &meta);
    free(integrity);
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
    DCHECK(f != NULL, "a browser algorithm's subresource fetch was issued outside a running flow — a fetch parks on "
                      "the flow's own register (core/html/html_link.c's trigger names the four entries HTML §4.2.4.3 "
                      "reaches it from)");
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
           "a browser algorithm's subresource fetch stated a destination Fetch §2.2.5 \"Requests\" does not "
           "enumerate — run Fetch §2.2.7's translate a potential destination over the `as` keyword first, as HTML "
           "§4.6.8.20 step 3 does");
    /* A browser algorithm's subresource: no `script` element, so no §4.12.1.1 `parser document`. */
    e = pending_push(&f->pending, FLOW_PENDING_RESOURCE, flow_path_forced(f), flow_path_pinned(f), flow_path_driven(f),
                     /*parser_inserted*/0, flow_work_seq_next());
    pending_set(e, PEND_RESOLVE, JS_DupValue(ctx, deliver));
    /* Which document asked, as in the parks above. */
    pending_set_int(e, PEND_DOC, (int)document_environment_doc(ctx));
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
    /* An address resolved from a string: an unknown `src` records its shape and parks nothing. */
    req.address = FETCH_ADDRESS_CONCRETE;
    /* §4.12.1.1's three metadata members, off the element, so `<script nonce=x src=…>` under
       `script-src 'nonce-x'` is checked by step 7 at the park. */
    ScriptCspMeta meta;
    JSValue e;
    DCHECK(f != NULL, "a <script src> was parked on outside a running flow");
    DCHECK(url != NULL && *url, "a <script src> was parked on with no URL");
    DCHECK(script_type_executes(stype),
           "a <script src> was parked on for an element whose type executes nothing — §4.12.1.1 fires error at an "
           "`importmap` or `speculationrules` element with a `src` and returns without fetching");
    meta = script_csp_meta(ctx, el);
    req.metadata = meta.m;
    /* The credentials mode, off the same element by §4.12.1.1's own step (see `script_credentials_mode`), so
       `<script crossorigin=use-credentials src>` is fetched with the mode it names. */
    req.credentials = script_credentials_mode(el);
    /* The mode, which `stype` decides besides the decode: §2.5.1's mode off the element for a classic script,
       `cors` for a module (see script_request_mode). */
    req.mode = script_request_mode(stype, el);
    /* Fetch §2.2.5's "client", and HTML §4.12.1.1's script fetch options' referrer policy: "the current state of
       el's referrerpolicy content attribute". */
    req.referrer = "client";
    req.referrer_policy = el ? referrer_policy_attribute_keyword(el) : "";
    e = pending_push(&f->pending, FLOW_PENDING_SCRIPT, flow_path_forced(f), flow_path_pinned(f), flow_path_driven(f),
                     parser_inserted, flow_work_seq_next());
    pending_set_int(e, PEND_SCRIPT_TYPE, (int)stype);
    /* Which document's program the reply will be: the realm this chokepoint was entered with is the document
       the element was inserted into, not whichever realm the session is rooted at. */
    pending_set_int(e, PEND_DOC, (int)document_environment_doc(ctx));
    /* Whose element it is. Every caller reaches this from §4.12.1.1's steps over an element, so NULL is
       asserted rather than defaulted past. */
    DCHECK(el != NULL, "a <script src> was parked on with no element — §4.12.1.1 reaches this destination only from "
                       "prepare the script element, and a row without one would run the program with currentScript "
                       "null");
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
   not a session: raised by the host that provisioned the instance or by the document creating a navigable another
   agent hosts, it survives every session boundary. It is read by flow_step (a timeline that may not finish), by
   the slice (a frontier that may not be declared exhausted) and by the ABI (engine_referenced). */
static int g_referenced;

/* The browser layer's document-load lifecycle, asked when a flow has run everything the document gave it. */
static int (*g_docdone_hook)(JSContext *ctx);
void engine_set_document_done_hook(int (*fn)(JSContext *ctx))
{
    /* One claimant, and NULL is the release: document_init claims the slot once per agent and
       document_agent_free gives it back. A second claim on a one-entry slot would silently stop the first from
       running. */
    DCHECK(fn == NULL || g_docdone_hook == NULL,
           "a second component claimed §13.2.7's document-load lifecycle step — one call answers for every document "
           "of this agent (it walks navigable_tree_order), so there is one claim per agent");
    g_docdone_hook = fn;
}

/* The browser layer's §13.2.7 step 3, see engine.h. */
static int (*g_parse_end_hook)(JSContext *ctx);
void engine_set_parse_end_hook(int (*fn)(JSContext *ctx))
{
    DCHECK(fn == NULL || g_parse_end_hook == NULL,
           "a second component claimed §13.2.7 step 3 — one call answers for every document of this agent");
    g_parse_end_hook = fn;
}

/* The event loop's timer step, registered by the timer component so the scheduler does not depend on the browser
   half. Asked only where this flow has nothing else to run, the one moment virtual time may move (timer.h). */
static int (*g_timer_hook)(JSContext *ctx);
void engine_set_timer_hook(int (*fn)(JSContext *ctx))
{
    DCHECK(fn == NULL || g_timer_hook == NULL,
           "a second component claimed §8.1.7's timer step — there is one event loop per agent, so there is one "
           "claim");
    g_timer_hook = fn;
}

static uint64_t g_task_arrival;   /* engine_task_arrival's stamp, until the enqueue it names takes it */
void engine_task_arrival(uint64_t seq)
{
    DCHECK(g_task_arrival == 0, "a task's arrival was named while an earlier one was unconsumed — the enqueue "
                                "it was named for never reached engine_enqueue_job");
    g_task_arrival = seq;
}

static int (*g_timer_race_hook)(JSContext *ctx);
void engine_set_timer_race_hook(int (*fn)(JSContext *ctx))
{
    DCHECK(fn == NULL || g_timer_race_hook == NULL,
           "a second component claimed the delivery arm's deadline race — there is one timer source per event loop");
    g_timer_race_hook = fn;
}

static int (*g_timer_active_hook)(JSContext *ctx);
void engine_set_timer_active_hook(int (*fn)(JSContext *ctx))
{
    DCHECK(fn == NULL || g_timer_active_hook == NULL,
           "a second component claimed the count of active timers — there is one timer source per event loop");
    g_timer_active_hook = fn;
}

static uint64_t (*g_timer_due_hook)(JSContext *ctx);
void engine_set_timer_due_hook(uint64_t (*fn)(JSContext *ctx))
{
    DCHECK(fn == NULL || g_timer_due_hook == NULL,
           "a second component claimed the timer source's arrival stamp — there is one timer source per event loop");
    g_timer_due_hook = fn;
}

/* The timer source's due task takes the turn of a younger reply or callback (HTML §8.1.7.1 Definitions: within
   §8.1.7.3 step 2.1's implementation-defined choice of queue, arrival order starves no queue). It is due, so the
   step fires it without moving the clock. */
static int engine_timer_takes_turn(JSContext *ctx, Flow *f, uint64_t timer_seq, uint64_t other_seq) {
    int fired;

    if (timer_seq == 0 || timer_seq >= other_seq || flow_blocked(f)) return 0;
    fired = g_timer_hook(ctx);
    DCHECK(fired, "a timer the arrival stamp found due did not fire — timer_due_stamp and timer_run_due "
                  "disagree about which entry is the timer source's next task");
    return fired;
}

/* The event loop's other clock-driven source: §8.1.7.3's in-parallel half, which queues an update-the-rendering
   task on the rendering task source when a navigable has a rendering opportunity. Registered by the rendering
   component, like the timer step. It is asked immediately before the timer step and yields to a timer that
   expires first, so the one virtual clock runs its two sources in the order their moments fall. */
static int (*g_rendering_hook)(JSContext *ctx, bool due_only);
void engine_set_rendering_hook(int (*fn)(JSContext *ctx, bool due_only))
{
    DCHECK(fn == NULL || g_rendering_hook == NULL,
           "a second component claimed §8.1.7.3's in-parallel half — there is one rendering task source per event "
           "loop, and a second claim would silently replace the first");
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
           "a second component claimed the event loop's idle rung — there is one event loop per agent, and a second "
           "claim would stop the first from ever being asked");
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
    return g_rendering_hook != NULL && g_rendering_hook(ctx, false);
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
           "a second component claimed §4.6.8.20's connected time for parsed link elements — the inventory it drains "
           "is one list per agent, and a second claim would stop the first from draining");
    g_link_connected_hook = fn;
}

/* The end of a microtask checkpoint (HTML §8.1.7.3), registered by the browser component that owns what HTML
   invokes there; see engine.h. Its one caller is Indexed Database §2.7.1's transaction cleanup. */
static void (*g_checkpoint_hook)(JSContext *ctx);
void engine_set_checkpoint_hook(void (*fn)(JSContext *ctx))
{
    DCHECK(fn == NULL || g_checkpoint_hook == NULL,
           "a second component claimed the end-of-microtask-checkpoint step — the slot holds one entry, and a second "
           "claim would stop the first from running");
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
/* Standbys left before a peer answer was taken, and those the peer's exhaustion retired. */
static long g_answer_standbys, g_answer_standbys_retired;
/* Exhaustions this instance sent as the answering side, and those it was delivered as the asking side. */
static long g_perform_exhausted, g_host_exhausted;
/* Answers that arrived when nothing was left to consume them. A peer answers by running a program on its own
 * timeline, relayed by a zone that cannot know what became of the asker, and Level-1 eviction makes a peer
 * outliving the asker's park the ordinary case. The asking flow is a recipe whose replay re-issues the request
 * and gets today's value, so a late answer is refused (engine_host_answer) and counted here: `hostAnswersLate`
 * climbing while `hostAnswered` does not is a host paying an instance that stopped listening. */
static long g_host_answers_late;
/* Peer answers no holder admitted: every entry naming the request belongs to a flow whose commitment record
 * refuses the answering timeline (engine_host_answer's state 0). */
static long g_host_answers_refused;
/* Holders that refused a peer answer under a foreclosed row: their own record names the answering timeline as
 * one a family member took (deliver_admits' STEP_UNIT_ROUTED_PARENT_TOOK). Counted per holder, at both of
 * engine_host_answer's refusals, so a resumed standby meeting the rows its park carried is a number the asking
 * instance states rather than one a driver infers from which answers were taken. */
static long g_host_answers_foreclosed;
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
/* RAM pressure's policy (engine.h's engine_set_ram_share): the share in permille, the limit it is a share of (0:
   none known, so pressure sells nothing), and the sales it made, a subset of `g_flows_sold`. */
static unsigned g_ram_share = ENGINE_RAM_SHARE_DEFAULT;
static uint64_t g_ram_limit;
static const char *g_ram_limit_source = "{\"none\":1}";
/* The part of that limit no heap block can use, read once with it (engine_ram_limit_read), and the allocator's
   footprint less its live bytes at the last instant pressure started selling, 0 before the first. */
static uint64_t g_ram_nonheap;
static uint64_t g_pressure_frag;
/* The footprint at which pressure last made room: footprint up to it is address space the instance has already
   held without meeting the limit, so only growth past it (or past the line) asks for a sale. */
static uint64_t g_ram_fp_mark;
static long g_pressure_sold;
/* The cycle collections pressure owed, and the count of finished collections it last sold at (UINT64_MAX before the
   first sale of the session). */
static long g_pressure_collects;
/* Compositions between steps (engine_note_rest_composition), the attempts they took, and the most one took. */
static long g_rest_compositions, g_rest_compose_attempts, g_rest_compose_attempts_max;
static uint64_t g_pressure_sold_at_collections = UINT64_MAX;
/* The members pressure's next batch may sell before it owes a collection, and whether the last batch sold all it
   could (engine_relocate_under_pressure). */
static long g_pressure_batch = 1;
static bool g_pressure_batch_full;

int engine_take_paged_owed(void) {
    /* The credit counts entries the host was shown and has not answered (engine_reclaim_tail), and this is its
       only spender, so below zero means a second spender or one reply spending two credits; after that the
       number no longer means "replies owed for flows this instance sold". */
    DCHECK(g_paged_owed >= 0,
           "the paged-reply debt went negative — it counts unanswered entries that left with sold flows and this is "
           "its only spender, so more replies were excused as sales than were sold");
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
    e = pending_push(&f->pending, FLOW_PENDING_HOSTREQ, flow_path_forced(f), flow_path_pinned(f), flow_path_driven(f),
                     /*parser_inserted*/0, flow_work_seq_next());
    pending_set(e, PEND_OP, JS_NewString(ctx, op));
    /* Which document asked: the realm the operation was entered with. The trusted zone judges the request
       against that document's record (engine_host_requests). */
    pending_set_int(e, PEND_DOC, (int)document_environment_doc(ctx));
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
        DCHECKF(dups == 1, "minting host request %u left %d entr(y/ies) on this flow's register under that machine "
                           "name — engine_host_answered returns the first match, so the name must be "
                           "unique", id, dups);
    }
#endif
    /* The request is what makes the flow blocked. Every caller returns JS_STEP_YIELD, which the driver re-enters
       immediately and asks nothing of (quickjs.c's arm for it), so the yield is a park only because
       preempt_hook's clause (0) sees this entry on the register. A pending kind without pending_blocked's
       agreement would turn every cross-instance read into a flow that burns the thread until the host answers,
       and the host is asked only between steps, so the answer could never arrive. Asserted here, where the
       blocking is claimed. */
    DCHECK(flow_blocked(f), "a synchronous host request left its flow runnable — the caller returns JS_STEP_YIELD "
                            "and re-enters at once, so an unblocked flow would spin on a question answered only "
                            "between steps");
    return id;
}

/* The running flow's rendezvous under machine name `req`, owned; JS_UNDEFINED when it holds none. A flow's
   machine names are unique (asserted at engine_host_request). */
static JSValue host_request_entry_of(JSContext *ctx, Flow *f, uint32_t req, int *slot) {
    for (int i = 0, n = pending_count(f->pending); i < n; i++) {
        JSValue e = pending_entry(f->pending, i);
        if (pending_get_int(e, PEND_KIND) == FLOW_PENDING_HOSTREQ && (uint32_t)pending_get_int(e, PEND_REQ) == req) {
            if (slot) *slot = i;
            return e;
        }
        JS_FreeValue(ctx, e);
    }
    return JS_UNDEFINED;
}

/* See engine.h. The push counts every rendezvous as blocking; pending_mark_async gives that count back before
   the asker returns, so the flow is runnable on the step that asked. */
static uint32_t host_request_make_async(JSContext *ctx, uint32_t id, JSValueConst deliver) {
    Flow *f = flow_running();
    JSValue e = host_request_entry_of(ctx, f, id, NULL);

    DCHECK(JS_IsFunction(ctx, deliver), "an async host rendezvous was asked with no delivery — its answer would "
                                        "be owed to nobody and its flow could never finish");
    DCHECK(!JS_IsUndefined(e), "an async host rendezvous is not on its flow's register after the ask");
    pending_set(e, PEND_RESOLVE, JS_DupValue(ctx, deliver));
    pending_mark_async(f->pending, e);
    JS_FreeValue(ctx, e);
    DCHECK(!flow_blocked(f), "an async host rendezvous left its flow blocked — the flow is meant to run on while "
                             "the zone fetches");
    return id;
}

uint32_t engine_host_request_async(JSContext *ctx, const char *op, JSValueConst deliver) {
    return host_request_make_async(ctx, engine_host_request(ctx, op), deliver);
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
        DFAILF("a machine asked about host request %u, which is not on its flow's register — it never issued that "
               "request, or it inherited the id across a fork (which re-issues under the sibling's own world). The "
               "register holds %d entr(y/ies), %d listed as MACHINE~HOST/kKIND (+v answered, KIND a solver/pending.h "
               "FLOW_PENDING_* value): %s; a HOSTREQ (k3) at another id is the fork reading",
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
 * The entry carries no answer nobody has forked over: flow_step takes one arm per recorded extra before it
 * resumes this frame (flow_answer_fork), so `extra` is empty at every healthy take. A later peer answer lands on
 * the standby flow_answer_standby left before this resume, not on this entry. */
/* The row a taken cross-instance answer owes, defined with the other commitment writer because what a receiving
   timeline has heard from is one question with one set of rules. */
static void answer_commit_taken(JSContext *ctx, Flow *f, const char *world);
static void deliver_take_marks(Flow *f, EngineArrival m);   /* defined with the delivery, below */

JSValue engine_host_take(JSContext *ctx, uint32_t req, int *pcompletion) {
    Flow *f = flow_running();
    JSValue v;
    DCHECK(f != NULL, "a host answer was taken outside a flow");
    DCHECK(pcompletion != NULL,
           "a host answer was taken without reading its completion type — an answer is a completion record, and "
           "reading only its value would deliver a peer's throw as `undefined`");
    for (int i = 0, n = pending_count(f->pending); i < n; i++) {
        JSValue e = pending_entry(f->pending, i);
        if ((uint32_t)pending_get_int(e, PEND_REQ) == req) {
            JSValue c;
            DCHECK(pending_get_int(e, PEND_HAVE_VALUE), "a host answer was taken before it arrived");
            DCHECK(!pending_get_int(e, PEND_ASYNC), "an async rendezvous's answer was taken by its asker — it is "
                                                   "delivered to its RESOLVE as a task (flow_deliver_one_reply)");
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
                JSValue wv = pending_get(e, PEND_ANSWER_WORLD), mv = pending_get(e, PEND_ANSWER_MARKS);

                DCHECK(JS_IsString(wv) == JS_IsString(mv),
                       "a taken answer names its timeline without that timeline's marks, or the reverse — "
                       "engine_host_answer and flow_answer_fork write the two together");
                if (JS_IsString(wv)) {
                    const char *w = JS_ToCString(ctx, wv), *m = JS_ToCString(ctx, mv);

                    CHECK(w != NULL && m != NULL,
                          "engine: OOM recording which peer timeline answered a cross-instance request — "
                          "without the commitment this timeline would accept a message from a peer arm it "
                          "never took");
                    answer_commit_taken(ctx, f, w);
                    /* The answer rests on the answering timeline's path, as a delivery rests on its sender's. */
                    deliver_take_marks(f, engine_arrival_of_token(m));
                    JS_FreeCString(ctx, w);
                    JS_FreeCString(ctx, m);
                }
                JS_FreeValue(ctx, wv);
                JS_FreeValue(ctx, mv);
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
    DCHECK(completion != ENGINE_COMPLETION_DECLINED,
           "a declined cross-agent operation's answer was taken as a value — no peer computed one, so the asking "
           "machine asks engine_host_declined first and builds an absent value of its member's type");
    if (completion == ENGINE_COMPLETION_THROW) {
        /* Re-raised at the call site that parked on it, so the page's own `try`/`catch` around the read, write
           or call handles it as it would a local throw. JS_Throw consumes the value. */
        *presult = JS_UNDEFINED;
        JS_Throw(ctx, v);
        return JS_STEP_ABRUPT;
    }
    DCHECK(completion == ENGINE_COMPLETION_NORMAL,
           "a host answer arrived under a completion type other than normal or throw — ECMA-262 6.2.4's `return`, "
           "`break` and `continue` do not cross a call site");
    *presult = v;
    return JS_STEP_DONE;
}

int engine_host_declined(uint32_t req) {
    Flow *f = flow_running();
    int declined = 0;

    DCHECK(f != NULL, "a declined answer was asked about outside a flow");
    for (int i = 0, n = pending_count(f->pending); i < n; i++) {
        JSValue e = pending_entry(f->pending, i);
        if (pending_get_int(e, PEND_KIND) == FLOW_PENDING_HOSTREQ && (uint32_t)pending_get_int(e, PEND_REQ) == req) {
            DCHECK(pending_get_int(e, PEND_HAVE_VALUE), "a declined answer was asked of a request not yet answered");
            declined = pending_get_int(e, PEND_COMPLETION) == ENGINE_COMPLETION_DECLINED;
            JS_FreeValue(pending_ctx(), e);
            return declined;
        }
        JS_FreeValue(pending_ctx(), e);
    }
    DFAIL("a declined answer was asked of a request id this flow's register does not hold");
    return 0;
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
/* A reply ARRIVES: its value is on the entry and it is answered, through the register for a rendezvous
   (pending_answer_rendezvous) and the generic door otherwise. A reply the delivery takes is a task of its flow
   from this instant (Fetch §4.1 "Main fetch" queues processResponse when the response arrives), so its arrival
   stamp is minted here and a task the page queued after the request is older and runs first. A blocking
   rendezvous is taken at its asker's park and is no task. `f` is the register's flow, needed only for a
   rendezvous; a shared park record's arrival is one instant for every flow naming it. A reply answered inside
   the park that asked (a blocked request, a local scheme, a supplied value) arrives at its push, whose stamp
   is that instant already. */
static void reply_arrived(Flow *f, JSValueConst e) {
    if (pending_get_int(e, PEND_KIND) == FLOW_PENDING_HOSTREQ) {
        DCHECK(f != NULL, "a rendezvous's answer arrived with no register to lower its blocked count on");
        pending_answer_rendezvous(f->pending, e);
        if (!pending_get_int(e, PEND_ASYNC)) return;
    } else {
        pending_set(e, PEND_HAVE_VALUE, JS_TRUE);
    }
    pending_set_int(e, PEND_WORK_SEQ, (int64_t)flow_work_seq_next());
}

/* `fetch.cancel<TAB><host name>`: the zone is asked to stop the transfer of the rendezvous it knows as `req`. */
static void host_cancel_notify(JSContext *ctx, uint32_t req) {
    char id[16];
    const char *fields[1] = { id };
    char *rec;

    snprintf(id, sizeof id, "%u", req);
    rec = engine_notice_build("fetch.cancel", fields, 1);
    engine_host_notify(ctx, rec);
    free(rec);
}

/* See engine.h. */
int engine_host_withdraw(JSContext *ctx, uint32_t req) {
    Flow *f = flow_running();
    int slot = -1;
    JSValue e;

    DCHECK(f != NULL && req != 0, "a fetch controller was withdrawn with no flow or no rendezvous to withdraw");
    e = host_request_entry_of(ctx, f, req, &slot);
    if (JS_IsUndefined(e)) return 0;
    JS_FreeValue(ctx, e);
    engine_cancel_entry(ctx, f, slot);
    return 1;
}

/* See engine.h. */
void engine_cancel_entry(JSContext *ctx, Flow *f, int slot) {
    static const int field[5] = { PEND_METHOD, PEND_MODE, PEND_REFERRER_POLICY, PEND_REFERRER, PEND_URL };
    const char *fv[5] = { NULL, NULL, NULL, NULL, NULL };
    JSValue e = pending_entry(f->pending, slot);
    int rendezvous = pending_get_int(e, PEND_KIND) == FLOW_PENDING_HOSTREQ;
    int owed = !pending_get_int(e, PEND_HAVE_VALUE);
    int keyed = !rendezvous && owed && pending_index_keyed(e);
    uint32_t host = rendezvous ? (uint32_t)pending_get_int(e, PEND_REQ_HOST) : 0;

    DCHECK(!rendezvous || pending_get_int(e, PEND_ASYNC),
           "a blocking rendezvous was cancelled — its flow is blocked and runs no code that could cancel it; "
           "its teardown is engine_host_terminate");
    for (int k = 0; keyed && k < 5; k++) {
        JSValue s = pending_get(e, field[k]);
        fv[k] = JS_ToCString(ctx, s);
        JS_FreeValue(ctx, s);
        CHECK(fv[k] != NULL, "engine: OOM naming a cancelled request");
    }
    JS_FreeValue(ctx, e);
    pending_remove(&f->pending, slot);
    if (rendezvous) {
        g_host_terminated++;
        if (owed && g_sess_live) host_cancel_notify(ctx, host);
        return;
    }
    if (!keyed) return;
    {
        PendRequestKey key = { fv[0], fv[1], fv[2], fv[3], fv[4] };
        PendIndexNode *node = pending_index_find(&key);

        if ((node == NULL || pending_index_node_count(node) == 0) && g_sess_live) {
            char *rec = engine_notice_build("fetch.cancel", fv, 5);
            engine_host_notify(ctx, rec);
            free(rec);
        }
        for (int k = 0; k < 5; k++) JS_FreeCString(ctx, fv[k]);
    }
}

/* A flow leaving the frontier unfinished (the pager's sale, a retirement) takes its async sends with it, and
   no machine of theirs is suspended to say so at its fini: the zone is told to end each one still owed. */
static void flow_withdraw_async_sends(JSContext *ctx, Flow *f) {
    for (int i = 0, n = pending_count(f->pending); i < n; i++) {
        JSValue e = pending_entry(f->pending, i);
        if (pending_get_int(e, PEND_KIND) == FLOW_PENDING_HOSTREQ && pending_get_int(e, PEND_ASYNC) &&
            !pending_get_int(e, PEND_HAVE_VALUE)) {
            g_host_terminated++;
            if (g_sess_live) host_cancel_notify(ctx, (uint32_t)pending_get_int(e, PEND_REQ_HOST));
        }
        JS_FreeValue(ctx, e);
    }
}

void engine_host_terminate(JSContext *ctx, uint32_t req) {
    int held = 0;

    DCHECK(ctx != NULL, "a host request was withdrawn with no context to free its register entry through");
    DCHECK(req != 0, "a host request with no id was withdrawn — the id names which rendezvous is given back");
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
                   "a rendezvous was withdrawn through an arm's register rather than the issuer's — an arm carries "
                   "the issuer's id with its answer already fixed");
#if APICLIENT_DEV
            {
                /* The notice below is `fetch.cancel`, so what it withdraws is a network send: the two
                   rendezvous the zone answers out of the network. A cross-agent operation is given back by
                   engine_retract_operations, never through here. */
                JSValue ov = pending_get(p, PEND_OP);
                const char *op = JS_ToCString(ctx, ov);
                DCHECK(op != NULL && (!strncmp(op, "xhr.send\t", 9) || !strncmp(op, "document.fetch\t", 15)),
                       "a rendezvous that is not a network send was withdrawn as one — engine_host_terminate tells "
                       "the zone `fetch.cancel`; give a cross-agent operation back through "
                       "engine_retract_operations");
                JS_FreeCString(ctx, op);
                JS_FreeValue(ctx, ov);
            }
#endif
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
    /* The zone is asked to stop the transfer, which only it can do: `fetch.cancel<TAB><id>`, the notice an
       aborted park sends (engine_pending_fetch_cancel), naming the request by the key its answer would come
       back under. Fetch §2 Infrastructure makes a terminated and an aborted controller both canceled, and the
       error abort records is the page's, never the zone's, so the zone owes both one act. A one-way notice:
       the engine's withdrawal is already complete, and a transfer that lands anyway is refused at
       engine_host_answer's ENGINE_ANSWER_HOST arm. Sent whether or not a register held the id, since the
       transfer is the zone's state. */
    host_cancel_notify(ctx, req);
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
static StepUnit deliver_admits(JSContext *ctx, const Flow *f, const char *vec);
/* Defined with engine_route, the first edge that reads a peer's marks; counts every record refused for them. */
static int peer_marks_word_ok(const char *word, size_t len);
static long g_peer_marks_refused;

int engine_host_answer(JSContext *ctx, uint32_t req, const char *world, const char *marks, JSValueConst value,
                       int completion, int source) {
    int extra = 0, fixed = 0, refused = 0;
    StepUnit verdict;

    DCHECK(req != 0, "the host answered a request with no id");
    DCHECK(completion == ENGINE_COMPLETION_NORMAL || completion == ENGINE_COMPLETION_THROW,
           "the host answered a request with a completion type that is neither normal nor a throw");
    DCHECK(source == ENGINE_ANSWER_HOST || source == ENGINE_ANSWER_PEER,
           "an answer arrived without saying who computed it — a value this zone computed has exactly one "
           "answer and a peer's has one per timeline, and only the deliverer knows which this is");
    /* The two sources differ exactly in whether a timeline produced the answer, so the name is required of one
       and forbidden of the other. An anonymous peer answer could not be told from a duplicate. */
    DCHECK((source == ENGINE_ANSWER_PEER) == (world != NULL && *world),
           "a cross-instance answer disagrees with its source about whether a peer timeline computed it — a peer's "
           "answer names the world of the flow that ran it, and a trusted-zone answer names none");
    DCHECK((world != NULL) == (marks != NULL && *marks),
           "a cross-instance answer names its timeline without that timeline's marks, or the reverse — "
           "perform_answer_notify writes both on every answer");
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
               "a cross-instance answer was computed in a world of a document this agent holds — the operation was "
               "routed back to the asking instance, so the value came from this heap");
    }
#endif
    /* The answering engine's marks word, refused as engine_route refuses a delivery's: the asker stays parked,
       as it would on a peer that never answered. */
    if (source == ENGINE_ANSWER_PEER && !peer_marks_word_ok(marks, strlen(marks))) {
        g_peer_marks_refused++;
        return 0;
    }
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
                       "a request whose answer was fixed by an answer fork is unanswered — the arm is built from the "
                       "answer it took, so the two are written together");
                /* A peer answer meets the rule a routed message does (deliver_admits): a flow committed to a
                   contradicting timeline, or foreclosing this one because a family member took it
                   (flow_answer_fork, flow_answer_standby), is not its taker. The request's addressee
                   (engine_flow_addressee) narrows to the received rows on the peer's side; a foreclosed row
                   cannot be addressed, so a standby's refusal is made only here. */
                if (source == ENGINE_ANSWER_PEER &&
                    (verdict = deliver_admits(ctx, f, world)) != STEP_UNIT_ROUTED_DELIVERY) {
                    if (verdict == STEP_UNIT_ROUTED_PARENT_TOOK) g_host_answers_foreclosed++;
                    refused++;
                    JS_FreeValue(ctx, p);
                    continue;
                }
                /* A standby takes a later peer timeline's answer as its own delivery, as a routed post is
                   delivered to a timeline still able to receive it. */
                DCHECK(!pending_get_int(p, PEND_ANSWER_STANDBY) ||
                       (source == ENGINE_ANSWER_PEER && !pending_get_int(p, PEND_ANSWER_EXHAUSTED)),
                       "a standby took an answer that is not a peer timeline's — a zone answer, or one arriving "
                       "after the peer said every timeline had answered");
                pending_set(p, PEND_VALUE, JS_DupValue(ctx, value));
                pending_set(p, PEND_COMPLETION, JS_NewInt32(ctx, completion));
                /* The timeline is written with the value, so a flow that took an answer can address its next
                   operation to that timeline and refuse the same one arriving again. */
                pending_set(p, PEND_ANSWER_WORLD, world ? JS_NewString(ctx, world) : JS_NULL);
                pending_set(p, PEND_ANSWER_MARKS, marks ? JS_NewString(ctx, marks) : JS_NULL);
                /* `haveValue` last and through the register, because it is the write that unblocks the flow.
                   The register carries the count `pending_blocked` answers from, and a record cannot find the
                   registers naming it, so the walk that found the register states it. The generic `pending_set`
                   refuses this field on a synchronous record (solver/pending.h), so a path settling one without
                   a register crashes instead of leaving a flow blocked with its answer on it. */
                reply_arrived(f, p);
                /* The flow is askable again: this is the event a flow parked on a synchronous request waits
                   for, so the mark comes off here, on the flow the answer reached (flow.h). The first answer
                   stops at the first flow because an unanswered synchronous request is not shared by a fork
                   (engine_sibling_assemble unshares it and mints a fresh id), so exactly one register names it.
                   Sharing is never a reason for a per-flow clear to stop early: an arm that shared a record
                   would still need its own mark cleared. */
                flow_clear_host_owed(f);
                /* The pay half of the rate above. A standby's answer is another peer timeline answering an ask
                   its family's first answer already settled, so it is counted with the extras. */
                if (pending_get_int(p, PEND_ANSWER_STANDBY)) g_host_answers_extra++;
                else g_host_answered++;
                JS_FreeValue(ctx, p);
                return 1;
            }
            /* A CHECK, not a DCHECK: everything below records another timeline's answer, so a second host
               answer reaching it would be recorded as an unnamed peer timeline and fork an arm into a timeline
               that does not exist. Not proceeding is required in release too. */
            CHECK(source == ENGINE_ANSWER_PEER,
                  "the trusted zone answered one request twice — a value this zone computes (§7.4 step 14's load, "
                  "XHR §3.5.6's fetch) has exactly one answer");
            /* An arm's own answer is fixed. The arm holds the same request id (in the step state of the frame it
               cloned), so without this skip a third answer would fork from the arm as well as the issuer, giving
               a timeline that answered B and then C at one call site. */
            if (pending_get_int(p, PEND_ANSWER_FIXED)) { fixed++; JS_FreeValue(ctx, p); continue; }
            /* An entry that has left a standby hands every later answer to it: recording one here as well would
               deliver one peer timeline to two asking timelines. */
            if (pending_get_int(p, PEND_STANDBY_LEFT)) { fixed++; JS_FreeValue(ctx, p); continue; }
            if ((verdict = deliver_admits(ctx, f, world)) != STEP_UNIT_ROUTED_DELIVERY) {
                if (verdict == STEP_UNIT_ROUTED_PARENT_TOOK) g_host_answers_foreclosed++;
                refused++;
                JS_FreeValue(ctx, p);
                continue;
            }
            /* The same timeline may not answer one question twice: a peer performs the operation once per live
               flow (engine_perform) and spends the token off the row as it answers (flow_answer_perform), so a
               second completion under one name is a relay that duplicated. Forking over it would grow a twin of
               an existing arm. It is asserted, never dropped, because dropping hides the relay defect. */
            DCHECK(!pending_answer_world_seen(p, world),
                   "one peer timeline answered one cross-instance request twice — the answering flow spends the "
                   "rendezvous token as it answers, so the routing zone delivered one completion twice");
            /* An answer beyond the first does not stop at the first flow, and is not written onto a shared
               record: the asker and each branch sibling still holding the request must fork over it, each by
               draining its own list, so the record is unshared here per flow, as a branch fork does. */
            JS_FreeValue(ctx, p);
            p = pending_unshare(f->pending, i);
            pending_extra_add(p, completion, JS_DupValue(ctx, value), world, marks);
            JS_FreeValue(ctx, p);
            flow_clear_host_owed(f);
            /* Not a payment: the first answer settled the ask, and this is another peer timeline answering. */
            g_host_answers_extra++;
            extra++;
        }
    }
    if (extra) return 1;
    /* The session is live and no live register took this answer. Four states reach here; each has its own
       message because they are observed differently. */

    /* (0) Every entry able to take it refused it: each asking timeline holding the request is committed to a
       contradicting peer timeline or forecloses this one, so the answer is another family member's (under that
       member's own request when a park re-issued them apart). */
    if (refused) {
        g_host_answers_refused++;
        return 0;
    }

    /* (1) Every entry naming the request has its answer or has left a standby, and no standby waits: a family
       keeps one standby until the peer says it is exhausted (engine_host_exhausted), so this is a standby lost. */
    if (fixed)
        DFAIL("a peer's answer arrived for a request whose live entries all hold an answer and no standby waits — "
              "flow_answer_standby leaves one before every peer answer is taken and only exhaustion retires it");

    /* (2) Nothing names it, which for a value this zone computed is innocent: the asker is gone. */
    if (source == ENGINE_ANSWER_HOST)
        return 0;

    /* (3) For a peer's answer: no register names the request. A family's standby lives until exhaustion, so the
       asking family left by a sale (a resumed recipe re-issues the request), or the zone relayed an answer after
       the exhaustion that retired the standby. The abort states the sale count, the door this engine can see. */
    DFAILF("a peer's answer arrived for a request no register holds — the asking family's standby was sold or "
           "already retired by the peer's exhaustion; this session sold %ld flow(s) owing %ld synchronous "
           "request(s)", g_flows_sold, g_paged_reqs);
    return 0;
}

int engine_host_exhausted(JSContext *ctx, uint32_t req) {
    int n = 0;

    DCHECK(ctx != NULL && req != 0, "a peer's exhaustion was delivered with no context or no request id");
    g_host_exhausted++;
    if (!g_sess_live) {
        g_host_answers_late++;
        return 0;
    }
    for (int k = 0; ; k++) { Flow *f = flow_at(k); if (!f) break;
        for (int i = 0, m = pending_count(f->pending); i < m; i++) {
            JSValue p = pending_entry(f->pending, i);

            if (pending_get_int(p, PEND_KIND) == FLOW_PENDING_HOSTREQ &&
                (uint32_t)pending_get_int(p, PEND_REQ_HOST) == req) {
                DCHECK(!pending_get_int(p, PEND_ANSWER_EXHAUSTED),
                       "a peer's exhaustion arrived twice for one request — the answering side sends it once, "
                       "when the last timeline holding the rendezvous lets it go");
                pending_set(p, PEND_ANSWER_EXHAUSTED, JS_TRUE);
                /* An unanswered entry now waits for nothing: its flow is picked again and retires at its step. */
                if (!pending_get_int(p, PEND_HAVE_VALUE)) { flow_clear_host_owed(f); n++; }
            }
            JS_FreeValue(ctx, p);
        }
    }
    return n;
}

/* The name of the document a register entry asks for (PEND_DOC), as the trusted zone knows it: the token a
   provisioning record gave it, or the "<creator>.<k>" world_mint_doc composed. A name crosses inside a TAB- and
   newline-separated line and, on the fetch bill, inside a comma-joined set, so it holds none of the three. */
static const char *engine_asker_name(JSValueConst p) {
    uint32_t doc = (uint32_t)pending_get_int(p, PEND_DOC);
    const char *name;

    DCHECK(doc != 0, "an outstanding request names no asking document — every park records it at its push "
                     "(pending_park_request and engine_host_request assert it), so this entry bypassed both");
    name = world_doc_name(doc);
    DCHECK(!strpbrk(name, "\t\n,"),
           "a document name holds a TAB, a newline or a comma — the bill and the request line carry it as a "
           "token, and the bill's asker set is comma-joined, so the line would split into names nobody holds");
    return name;
}

/* What the host still owes, as `id\tasker\top\n` records, `asker` being the asking document's name
   (engine_asker_name). Pulled each step like the pending URLs and not deduplicated: two identical questions
   from two flows are two questions, each answered under its own world. */
const char *engine_host_requests(void) {
    static char *join;
    static size_t cap;
    size_t n_out = 0;

    /* One pass with a growing buffer: converting a JS string's text allocates, so a measure-then-write pair
       would convert every record twice. */
    if (!join) {
        cap = 256; join = reclaim_malloc(cap);
        CHECK(join, "engine: OOM joining the outstanding host requests");
    }
    join[0] = 0;
    /* Only the registers owed a rendezvous of either class (pending_owed_register): a register owing none lists
       nothing below, and an async one leaves its flow runnable, so the blocking count alone would never show the
       host an asynchronous send. Nothing below writes a register, so the set holds still while it is read. */
    for (int k = 0; k < pending_owed_register_count(); k++) {
        JSValueConst reg = pending_owed_register(k);
        for (int i = 0, n = pending_count(reg); i < n; i++) {
            JSValue p = pending_entry(reg, i);
            JSValue o;
            const char *s, *asker;
            size_t ol = 0, al;
            char idbuf[24];
            int idlen;
            /* "Can the host still be asked", not "is this flow still waiting", via the one exported predicate.
               `PEND_HAVE_VALUE` alone is also true of an entry the trusted zone refused (a refusal answers "will
               you make this request"), so a declined rendezvous would be re-joined, declined again, and abort
               at engine_host_decline's `!pending_entry_declined`. The address-keyed join applies the same filter,
               which keeps a decline a fork rather than a spin. */
            if (pending_get_int(p, PEND_KIND) != FLOW_PENDING_HOSTREQ ||
                !pending_entry_host_owed(p) || pending_get_int(p, PEND_ANSWER_STANDBY)) {
                /* A standby's rendezvous is the one its family already asked; the zone holds it in flight. */
                JS_FreeValue(pending_ctx(), p);
                continue;
            }
            DCHECK(pending_get_int(p, PEND_ADDRESS) != FETCH_ADDRESS_UNKNOWN,
                   "a rendezvous whose address has no bytes is about to be shown to a host — the scheduler refuses "
                   "it after the step that opened it (host_requests_decline_unaddressed), before the slice returns");
            /* The host's name is what the host is shown. This buffer is the only place an id leaves the engine,
               so every id the zone can quote back comes from here; the machine's name would give the host two
               arms' identical names and land one answer in two contradictory worlds. */
            idlen = snprintf(idbuf, sizeof idbuf, "%u\t", (uint32_t)pending_get_int(p, PEND_REQ_HOST));
            asker = engine_asker_name(p);
            al = strlen(asker);
            o = pending_get(p, PEND_OP);
            s = JS_ToCStringLen(pending_ctx(), &ol, o);
            DCHECK(s != NULL, "an outstanding host request has no text — the host routes on it");
            while (n_out + (size_t)idlen + al + ol + 3 > cap) {
                cap *= 2;
                join = realloc(join, cap);
                CHECK(join, "engine: OOM growing the outstanding-request join");
            }
            memcpy(join + n_out, idbuf, (size_t)idlen); n_out += (size_t)idlen;
            memcpy(join + n_out, asker, al); n_out += al;
            join[n_out++] = '\t';
            memcpy(join + n_out, s, ol); n_out += ol;
            join[n_out++] = '\n';
            join[n_out] = 0;
            JS_FreeCString(pending_ctx(), s);
            JS_FreeValue(pending_ctx(), o);
            JS_FreeValue(pending_ctx(), p);
        }
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

    DCHECK(op != NULL && *op, "a host notice was built under no operation name — the host routes on the operation");
    DCHECK(nfields == 0 || fields != NULL,
           "a host notice was built from a field count with no fields behind it");
    DCHECK(strchr(op, '\t') == NULL && strchr(op, '\n') == NULL,
           "a host notice's operation name contains one of the record's delimiters (tab or newline)");
    len = strlen(op);
    for (i = 0; i < nfields; i++) {
        DCHECK(fields[i] != NULL,
               "a host notice field is NULL — every field of a record is present or it is a different record, "
               "and a value that is absent is stated in that field's own vocabulary (`u`, `null`, `none`) "
               "rather than left out, because an empty field is a hole a reader can default");
        DCHECK(strchr(fields[i], '\t') == NULL,
               "a host notice field contains a tab, the record's field delimiter — a value that may hold one "
               "crosses encoded, as navigable_notice.h's BASE64 fields do");
        DCHECK(strchr(fields[i], '\n') == NULL,
               "a host notice field contains a newline, the record separator");
        len += 1 + strlen(fields[i]);   /* the TAB that precedes this field, then the field itself */
    }
    rec = reclaim_malloc(len + 1);
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
    DCHECK(at == len, "a host notice was assembled to a length its sizing walk did not predict — the two walks read "
                      "different bytes");
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
        char *rec = reclaim_malloc(cap);
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
        char *rec = reclaim_malloc(cap);
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

/* The answering side's other last-holder edge: a timeline let `token` go by answering under it or by refusing it,
   and when no timeline holds it any more no further answer follows, which the asker hears as `remoteop.exhausted`
   (engine_host_exhausted). A retraction is the last-holder edge that re-asks, so it never reaches here. */
static void perform_token_spent(JSContext *ctx, const char *token)
{
    size_t cap;
    char *rec;

    DCHECK(token != NULL && *token, "a rendezvous was spent with no token");
    if (perform_token_held(ctx, token)) return;
    cap = strlen(token) + 20;   /* "remoteop.exhausted" + TAB + the token + NUL */
    rec = reclaim_malloc(cap);
    CHECK(rec != NULL, "engine: OOM announcing that every timeline answered a cross-agent operation");
    snprintf(rec, cap, "remoteop.exhausted\t%s", token);
    engine_host_notify(ctx, rec);
    free(rec);
    g_perform_exhausted++;
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
    (*seen)[*n] = reclaim_strdup(token);
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

    /* `only` is visited alone rather than found by a walk: the pager hands back one member per sale, and a walk of
       the registry to reach it would make every sale O(members). */
    for (i = 0; (f = only ? (i == 0 ? only : NULL) : flow_at(i)) != NULL; i++) {
        int n, held = 0;

        /* The flow may not be the one holding the thread. Both callers switch out first (the park before it
           walks, the pager by construction); asserted because a switched-in flow could still run its program to
           completion against a row whose token was just returned to the zone. */
        DCHECK(f != flow_running(),
               "a cross-agent operation was handed back on the flow the scheduler is switched into — that flow can "
               "still complete its program and would find a row whose token was given back");
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
               "a flow still owes a peer an answer after every question it held was handed back — a rendezvous token "
               "survives outside the arrival slot and the program rows");
    }
    for (k = 0; k < seen_n; k++) {
        /* The last holder, asked after the strip: every flow this call was given now holds nothing, so a hit
           is a member that stays and will answer under this token. */
        if (!perform_token_held(ctx, seen[k])) {
            size_t cap = strlen(seen[k]) + 20;   /* "remoteop.retracted" + TAB + the token + NUL */
            char *rec = reclaim_malloc(cap);

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
           "a world-gone notice carried an ancestry — a death names one world, and its ancestors live while any "
           "descendant does");
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
    out = reclaim_malloc((size_t)(tail - worlds) + 1);
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
               "a delivery-world commitment carried no small-integer received/foreclosed flag");
        /* The minting mechanism is asked of every row, not only the one the refusal reads: a row without one
           would read JS_UNDEFINED as 0, FLOW_COMMIT_ARM_NONE, and fire the refusal's assert naming the delivery
           seam for a defect that is the pusher's. */
        DCHECK(JS_VALUE_GET_TAG(av) == JS_TAG_INT,
               "a delivery-world commitment carried no small-integer minting mechanism (flow.h's FlowCommitArm) — "
               "the flow_world_commit_push that wrote this row omitted it");
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
                   `flow_add*(…, WORLD_NONE)`: this file's boot flow, solve.c's @S candidate session, and each member a cold resume rebuilds; read which minted the
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
                           "a routed delivery contradicts a world this timeline already received, and that world's "
                           "row states no mechanism minted a flow on the other side of its branch, so no timeline of "
                           "this document will receive the record. The only producer of such a row is the "
                           "delivery-time fork declining a root sending world; find which root mint (a boot flow, a "
                           "@S candidate session, a cold-resume rebuild) made the "
                           "pair");
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

/* Is a commitment to receiving `vec` already implied by this timeline's record? The arm a delivery forecloses
 * and the commitment it records share this answer, so they are minted together or not at all. A world's
 * ancestors are totally ordered, so a received commitment at or above `vec` refuses all `vec` would, and its arm
 * forecloses a subtree holding `vec`; foreclosed rows are not read. The test is not "any commitment below the
 * nearest named ancestor": world_ancestry keeps only worlds that crossed as a head, so that ancestor is the
 * nearest sender, not a fork point, and suppressing on it retires an arm on the receiver's own record.
 *
 * Named residual. Not covered: k mutually contradicting worlds under one named ancestor yield k+1 arms, one a
 * timeline no sender was in. Next diff builds: a vector naming a retired fork point once its other subtree has
 * sent (world.h: an unfiltered chain grows with branches taken), so world_vec_fork_point names a real branch.
 * Absence shows as: live flows tracking the peer's distinct sent worlds, refusals growing as their product.
 *
 * `covering`, when non-NULL and the answer is 1, gets the weakest covering row's arm (answer commitments too),
 * never the first match. Under `need_arm` an arm-free row (a root world heard while unmarked) covers nothing:
 * that flow can take a mark without forking, and its marked delivery owes the timeline that never received it. */
static int deliver_commit_implied(JSContext *ctx, const Flow *f, const char *vec, bool need_arm,
                                  FlowCommitArm *covering)
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
               "a delivery-world commitment carried no small-integer received/foreclosed flag — a foreclosed row "
               "read as received would suppress the arm for a world this timeline never heard from");
        rel = world_vec_relate(vec, c);
        if (JS_VALUE_GET_INT(tv) && (rel == WORLD_REL_SAME || rel == WORLD_REL_ANCESTOR)) {
            JSValue av = JS_GetPropertyUint32(ctx, e, 2);
            FlowCommitArm row_arm;

            DCHECK(JS_VALUE_GET_TAG(av) == JS_TAG_INT,
                   "a delivery-world commitment covering an arriving world carried no small-integer minting "
                   "mechanism — the covering row says whether an arm for this world already exists");
            row_arm = (FlowCommitArm)JS_VALUE_GET_INT(av);
            JS_FreeValue(ctx, av);
            /* A row with no arm is a root world's, and only that same root can arrive at or above it. */
            DCHECK(row_arm != FLOW_COMMIT_ARM_NONE || rel == WORLD_REL_SAME,
                   "a received commitment with no arm is covered by a world above it — an arm-free row is pushed "
                   "only for a root sending world, which has no ancestor");
            if (!(need_arm && row_arm == FLOW_COMMIT_ARM_NONE)) {
                if (row_arm == FLOW_COMMIT_ARM_ANSWER_OWED) arm = FLOW_COMMIT_ARM_ANSWER_OWED;
                implied = 1;
            }
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

/* A peer record's marks word, read as input: another instance wrote it, and the trusted zone relays it unread
   because only the sending engine knows its flow's path. Answers 1 when the first `len` bytes of `word` are a
   word engine_record_head writes, `<derived|forced>,<driven|undriven>,<pinned|unpinned>`, and 0 otherwise.
   The edges that take a peer's marks (engine_route, engine_perform, engine_host_answer) refuse the record on 0,
   so every later parse reads a word this instance accepted. A word that does parse is not trusted further: a
   receiving timeline ORs the marks into its own (flow_take_peer_marks), so a peer's marks can only narrow what
   this instance sends, and a peer stating fewer than it holds gains no more than its page could by posting
   other bytes. */
static int peer_marks_word_ok(const char *word, size_t len)
{
    static const char *const words[3][2] = {
        { PENDING_PROVENANCE_DERIVED, PENDING_PROVENANCE_FORCED },
        { PENDING_DRIVEN_YES, PENDING_DRIVEN_NO },
        { PENDING_PINNED_YES, PENDING_PINNED_NO },
    };
    size_t at = 0;

    for (int k = 0; k < 3; k++) {
        size_t end = at;
        int hit = 0;

        while (end < len && word[end] != ',') end++;
        for (int j = 0; j < 2; j++)
            hit |= end - at == strlen(words[k][j]) && !memcmp(word + at, words[k][j], end - at);
        if (!hit || (k < 2) != (end < len)) return 0;
        at = end + 1;
    }
    return 1;
}

long engine_peer_marks_refused(void) { return g_peer_marks_refused; }

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
           "a record was routed into an instance whose frontier is parked — its flows are recipes and a message is "
           "not re-sent on replay. Build the inbound half of the cross-instance park (the record and the stamped "
           "sender origin cross as text), or have the trusted zone hold the record until the instance resumes");
    /* Drained: every timeline finished, so the document can no longer receive. engine_set_referenced exists to
       prevent that (a document a peer holds a WindowProxy for keeps its last timeline waiting), so reaching here
       means nothing told this instance it was still referenced. */
    DCHECK(g_sess_live,
           "a record was routed into an instance whose frontier has drained — engine_set_referenced holds a "
           "referenced document's last timeline open, so the provisioning zone did not report that a peer holds a "
           "WindowProxy for this document");

    /* Every routed record's first three fields are the transport's: the target document (which instance), the
       sending flow's world (whose timeline) and its path marks (engine_record_head). The rest belongs to the
       component the op names, so a second op is routed without a second router. */
    dup = reclaim_strdup(record);
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
    {
        const char *mk = tail + 1, *mk_end = strchr(mk, '\t');

        /* The marks field is the sending engine's word, refused here when it is not one engine_record_head
           writes, before the record reaches any timeline (see peer_marks_word_ok). */
        if (!peer_marks_word_ok(mk, mk_end ? (size_t)(mk_end - mk) : strlen(mk))) {
            g_peer_marks_refused++;
            free(dup);
            return;
        }
    }

    /* Routed to the wrong instance is the trusted zone's bug, and delivering anyway would fire a message the
       page never received. Which of this agent's documents it names is not decided here: an instance is an
       origin-keyed agent cluster with several documents, and the delivering realm is selected where the
       delivery runs (flow_deliver), since a hosted navigable's realm is materialized by whichever flow first
       reads through it (navigable.h). */
    DCHECK(world_doc_hosted(world_doc_intern(doc)),
           "a record was routed to an instance that does not hold the document it names — the offscreen, which alone "
           "knows the placement, routed it wrongly");
    {
        WorldId w;
        const WorldId *anc;
        int n_anc = world_parse(worlds, &w, &anc);
        /* Against every document this agent holds, not its root: a message from a same-origin child is
           delivered in this heap and never reaches a transport. */
        DCHECK(!world_doc_hosted(w.doc), "a record was routed back to the instance whose flow sent it — a message to "
                                         "one's own document is delivered locally");
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
               "the sending world has written in this instance, so the delivery's world is a join of two deltas over "
               "one baseline, which the linear delta chain cannot express. Build the join: merge the two entry lists "
               "where they touch disjoint slots, and treat a slot both touch as a contradiction with no delivery in "
               "that world");
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
    DCHECK(n > 0, "a message was routed to a document whose every timeline has finished — keep the receiving flows "
                  "live while a peer holds a WindowProxy for them");
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
    DCHECK(f != NULL, "a parked routed delivery was rebuilt onto no flow — an 'm' record belongs to the flow written "
                      "immediately before it");
    DCHECK(record != NULL && *record && sender_origin != NULL && *sender_origin,
           "a parked routed delivery was rebuilt missing its record or its stamped sender origin — both cross the "
           "tier as text, and the reader may not invent either");
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
           "doc_realm_at was reached with no caller site — the `doc_realm` macro passes __FILE__ and __LINE__, so "
           "this is a hand-written call that bypassed it");
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
            DFAILF("a cross-agent operation or queued program named a document whose active Document this timeline "
                   "destroyed, asked at %s:%d — §7.5.10 nulled the realm and proxy_realm does not rebuild a "
                   "destroyed navigable. A §7.2.1 member returning an object of that realm and an `object.*` verb on "
                   "a lent object need different answers; read the asking site and the residual above before "
                   "building either", file, line);
            break;
        case WP_DOC_INITIAL:
            DFAILF("a navigable this agent holds was asked for its active document's realm, which this timeline has "
                   "never materialized, asked at %s:%d — only a read through the navigable's own WindowProxy builds "
                   "it; a caller that must materialize resolves the name with window_proxy_of_document and calls "
                   "window_proxy_realm", file, line);
            break;
        case WP_DOC_REMOTE:
            DFAILF("the realm of a document a peer instance holds was asked for at %s:%d — callers must pass "
                   "`world_doc_hosted` first", file, line);
            break;
        case WP_DOC_NO_NAVIGABLE:
            DFAILF("a document name resolved to no navigable of this timeline, asked at %s:%d — no timeline here "
                   "opened it, this timeline navigated away from it, or the caller is at host time with no owner "
                   "named (engine_unload_document's residual)", file, line);
            break;
        case WP_DOC_ACTIVE:
            DFAILF("the realm lookup answered NULL for a navigable whose record says its active document is "
                   "materialized, asked at %s:%d — the two reads walk one table in one applied delta", file, line);
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
           "a Document was reported replaced to an instance with no live session — its unload is a task of every "
           "timeline of that document, and there is no scheduler to run one");
    DCHECK(world_doc_hosted(doc),
           "a Document this agent does not hold was reported replaced — the trusted zone named the wrong instance");
    n = flow_count();
    DCHECK(n > 0,
           "a Document was replaced while every timeline of this instance had finished — its unload would run in no "
           "timeline; keep a replaceable document's flows live, as engine_set_referenced does for a referenced one");
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

const PolicyContainer *engine_document_policy(uint32_t doc)
{
    /* Not gated on a live session: a frontier that has drained still holds its documents until qjs_teardown,
       and the zone may ask in the round between the last step and the finish. */
    DCHECK(world_doc_hosted(doc),
           "a document's policy container was asked of an instance that does not host it — the zone routes the "
           "question to the instance holding the document, and answering here would judge a request against "
           "another document's policy");
    return document_policy(doc_realm(doc));
}

/* A park that was a suspension (ECMA-262 §25.4.3.11 SuspendThisAgent, JS_TakeSuspension). Only an agent that can
   block suspends, and a flow here runs its window agent alone, which cannot. */
static void engine_refuse_suspension(JSContext *ctx)
{
    JSValue susp = JS_TakeSuspension(JS_GetRuntime(ctx));

    DCHECK(JS_IsUndefined(susp),
           "an agent of this flow suspended in Atomics.wait: its base is resumed only once JS_SuspensionWoken answers, "
           "which is the agent selection flow_select_agent's residual names, built with the flow's second agent");
    JS_FreeValue(ctx, susp);
}

/* Install `base`, a preemptible base built over `realm`, as the selected agent's execution context stack. A
   realm runs on the stack of its own agent (HTML §8.1.2.1, realm.[[AgentSignifier]]). */
static void flow_install_base(Flow *f, JSContext *realm, JSValue *base)
{
    DCHECK(JS_GetRealmAgent(realm) == flow_agent_running(f)->agent,
           "a preemptible base was installed on the execution context stack of another agent than its realm's");
    flow_frame(f) = base;
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
 * `f` makes the delivery in this same step; at most one arm per delivery. A cross-agent operation is received by
 * the same rule (flow_perform): its arm is the timeline never asked. */
static Flow *engine_sibling_assemble(JSContext *ctx, Flow *parent, JSValue *clone,
                                     void *dec_blob, void *pin_blob, FlowPathMarks marks);
/* What the step names itself (defined with the scheduler below). A refusal is a step that consumed a record and
   delivered nothing, and it says so. */
static StepUnit g_step_unit;

/* Returns which mechanism minted the other side, because this is the one line that knows: the caller writes it
   on the commitment row and deliver_admits reads the row rather than re-deriving it from the vector. "Did this
   fork run" and "does this vector name a branch" agree for this producer but are different questions. */
static FlowCommitArm deliver_fork_arm(JSContext *ctx, Flow *f, const char *vec, EngineArrival m, const char *why)
{
    char *fork_point = world_vec_fork_point(vec);
    FlowCommitArm covering;
    Flow *sib;

    /* A root world from a sender that carries no mark: no branch, so no other side to be on. A marked sender has
       one even at a root: the timeline in which it never ran (a drive, a forced arm) and so never posted, which
       is this timeline as it stands, kept unmarked beside the arm that takes the sender's marks. */
    if (fork_point == NULL && m.prov != PROV_FORCED && !m.driven && !m.pinned) return FLOW_COMMIT_ARM_NONE;
    free(fork_point);
    /* This timeline has not already minted it. The arm for a (flow, world) pair is minted at the flow's first
       delivery from that world, together with its commitment, decided by one predicate (deliver_commit_implied)
       over a list neither call changes (the arm's own row goes on the sibling), so an arm cannot exist without
       its row. The converse holds only where the vector names an ancestor: a root world is pushed as a
       commitment with no arm, which is the state deliver_admits' refusal asserts against.
       An implied commitment is covered by the deeper one's arm, whose foreclosure is a subtree containing this
       world, and which mechanism minted that arm is the covering row's statement, not a constant: a
       cross-instance answer also pushes received rows (answer_commit_taken), and its arm is owed, not minted. */
    if (deliver_commit_implied(ctx, f, vec, true, &covering)) return covering;
    DCHECK(flow_frame(f) == NULL,
           "a delivery-time fork was taken by a flow inside a program — flow_step makes deliveries only with no "
           "frame");
    DCHECK(!JS_HasActivation(JS_GetRuntime(ctx)),
           "a delivery-time fork was taken with page code on the stack — the arm re-enters its scheduler step, which "
           "is sound only between tasks");
    sib = engine_sibling_assemble(ctx, f, NULL, decide_fork_same_path(why), concolic_pins_suspend(),
                                  flow_path_marks(f));
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
    if (!deliver_commit_implied(ctx, f, vec, arm != FLOW_COMMIT_ARM_NONE, NULL))
        flow_world_commit_push(ctx, f, vec, 1, arm);
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
           "a cross-instance answer was recorded as a commitment with no answering timeline — a peer's answer names "
           "the world of the flow that ran it");
    if (!deliver_commit_implied(ctx, f, world, false, NULL))
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
        r->record = reclaim_strdup(record);
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
           "a routed record was delivered with no arrival on this instance's ledger. Either the delivery queue was "
           "rebuilt from the cold tier without the arrival (build the inbound half of the cross-instance park), "
           "something outside engine_route pushes deliveries, or the record did not survive its round trip through "
           "JS (key the ledger by a per-attachment ticket)");
    if (r != NULL) r->admitted++;
}

void engine_routed_census(long *delivered, long *refused, long *zero_delivery) {
    int i;

    DCHECK(delivered != NULL && refused != NULL && zero_delivery != NULL,
           "the routed-delivery census was asked for fewer than its three numbers — they are returned together");
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
           "the routed-task census was asked for nothing to write into — its four ends are returned together");
    for (i = 0; i < ROUTED_TASK_END_N; i++) ends[i] = g_routed_task_end[i];
}

/* The marks word of a record `f` sends: a question's head and an answer both carry it. */
static void flow_marks_token(const Flow *f, char *buf, size_t cap)
{
    EngineArrival m;

    m.prov = flow_path_forced(f) ? PROV_FORCED : PROV_DERIVED;
    m.driven = flow_path_driven(f);
    m.pinned = flow_path_pinned(f);
    engine_arrival_token(m, buf, cap);
}

char *engine_record_head(const char *op, const char *doc, const Flow *f)
{
    char world[1024], marks[64], *out;
    size_t cap;

    DCHECK(op != NULL && doc != NULL && f != NULL, "a cross-instance record's head was composed with no verb, "
                                                   "no target document or no sending flow");
    /* world_serialize crashes on truncation rather than sending a prefix. */
    world_serialize(f->world, world, sizeof world);
    flow_marks_token(f, marks, sizeof marks);
    cap = strlen(op) + strlen(doc) + strlen(world) + strlen(marks) + 4;
    out = reclaim_malloc(cap);
    CHECK(out != NULL, "engine: OOM composing a cross-instance record's head");
    snprintf(out, cap, "%s\t%s\t%s\t%s", op, doc, world, marks);
    return out;
}

/* A record's marks field (its fourth for a routed delivery), in the arrival word's grammar and parsed by its
   one parser: `field` runs to the next tab or the end. */
static EngineArrival record_marks_field(const char *field)
{
    const char *end = strchr(field, '\t');
    size_t len = end ? (size_t)(end - field) : strlen(field);
    char word[64];

    CHECK(len < sizeof word, "engine: a record's marks field is longer than any engine_arrival_token writes");
    memcpy(word, field, len);
    word[len] = 0;
    return engine_arrival_of_token(word);
}

/* A routed record's fourth field: `record` is the whole routed text. */
static EngineArrival record_marks(const char *record)
{
    const char *p = record;
    int i;

    for (i = 0; i < 3; i++) {
        p = strchr(p, '\t');
        CHECK(p != NULL, "engine: a routed record ends before its marks field");
        p++;
    }
    return record_marks_field(p);
}

/* The receiving timeline takes the sending flow's marks (flow.h's flow_take_peer_marks), on the arm that receives
   the record, after the arm that does not has been minted with this timeline's own marks: in a real browser that
   other timeline never got the message. The word is one the receiving edge accepted (peer_marks_word_ok), so it
   is never `observed`. */
static FlowPeerMarks arrival_marks(EngineArrival a);   /* defined with the arrival, below */
static void deliver_take_marks(Flow *f, EngineArrival m)
{
    DCHECK(f == flow_running(), "a sender's marks were taken onto a timeline that is not the one receiving");
    DCHECK(m.prov != PROV_OBSERVED, "a peer's marks word reached a timeline stating `observed` — engine_route, "
                                    "engine_perform and engine_host_answer refuse every word but "
                                    "`derived` or `forced` with two marks");
    flow_take_peer_marks(f, arrival_marks(m));
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
    EngineArrival marks;

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
        FlowCommitArm arm;

        entry = flow_deliver_entry(f, 0);
        rv = JS_GetPropertyUint32(ctx, entry, 0);
        record = JS_ToCString(ctx, rv);
        CHECK(record != NULL, "engine: OOM reading a routed delivery's path marks");
        marks = record_marks(record);
        JS_FreeCString(ctx, record);
        JS_FreeValue(ctx, rv);
        JS_FreeValue(ctx, entry);
        arm = deliver_fork_arm(ctx, f, vec, marks,
                               "(a cross-document message arrived from one arm of a sender's branch — no "
                               "predicate was asked)");
        deliver_commit_taken(ctx, f, vec, arm);
        deliver_take_marks(f, marks);
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
    dup = reclaim_strdup(record);
    CHECK(dup != NULL, "engine: OOM splitting a routed delivery's transport fields");
    doc = strchr(dup, '\t');    *doc++ = 0;
    worlds = strchr(doc, '\t'); *worlds++ = 0;
    tail = strchr(worlds, '\t'); *tail++ = 0;
    tail = strchr(tail, '\t');   /* past the path marks, taken above */
    DCHECK(tail != NULL, "a routed delivery has no fields after its path marks");
    *tail++ = 0;
    /* The receiving document's realm is what "delivered to that document" means: the event and `event.source`
       are made in it and the task is enqueued at its Window. It must be a document this agent holds, asked
       again here because a delivery off the cold tier never passed engine_route in this session: a resumed flow
       replays from its first script, so a record naming a child navigable can be taken before the replay has
       re-created it. This assert names that ordering (a parked delivery must be made where the replay reached
       its arrival); doc_realm's DFAIL would instead name a navigable without a realm. */
    doc_id = world_doc_intern(doc);
    DCHECK(world_doc_hosted(doc_id),
           "a routed delivery names a document this session does not hold — a resumed flow replays from its first "
           "script and this delivery is taken before the navigable is rebuilt; make a parked delivery at the replay "
           "position where its message arrived");
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
               "a navigable whose record says this timeline destroyed its active Document still answered with a "
               "realm — the two reads walk one table in one applied delta");
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
        int jobs0 = flow_jobs_held(f);
        flow_job_external_begin(entry);
        if (!strcmp(dup, "windowproxy.post"))
            window_message_route(rctx, record, tail, world_doc_name(w.doc), origin);
        else
            DFAIL("a record was routed with an op no component receives — the sending half emits it, so the "
                  "receiving half is the unbuilt one; build it rather than dropping the delivery");
        flow_job_external_end();
        DCHECK(flow_jobs_held(f) == jobs0 + 1,
               "a routed delivery did not become exactly one task on the receiving timeline's queue — HTML §9.3.3 "
               "\"Posting messages\" step 8 queues one global task on the posted message task source");
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

    DCHECK(f != NULL, "the addressee of a cross-instance operation was asked of no flow");
    DCHECK(doc_name != NULL && *doc_name,
           "the addressee of a cross-instance operation was asked without naming the document it addresses — a flow "
           "may be committed to timelines of several peers at once");
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
                    best = reclaim_strdup(c);
                    CHECK(best != NULL, "engine: OOM naming the peer timeline a cross-instance operation is "
                                        "addressed to");
                } else {
                    WorldRel rel = world_vec_relate(c, best);

                    DCHECK(rel != WORLD_REL_CONTRADICT,
                           "two received commitments to one sending document contradict on a flow addressing its "
                           "next operation — flow_world_commit_push refuses that pair at the append, so a row "
                           "reached the list another way (the cold tier's replay can)");
                    /* Deepest wins: world_vec_relate(a, b) answers DESCENDANT when `b` is an ancestor of `a`, so
                       the candidate speaks for a strictly narrower subtree. SAME and ANCESTOR keep the
                       incumbent. */
                    if (rel == WORLD_REL_DESCENDANT) {
                        char *d = reclaim_strdup(c);
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
   release); (iii) one was attached and every holder left the frontier before the park walked. `inherited` is the
   third count: the arms a fork made that hold a question their parent held (engine_sibling_assemble copies the
   queue and the row tokens), which holders that engine_perform never attached to. */
static long g_perform_asks, g_perform_attached, g_perform_inherited;

void engine_perform_census(long *asks, long *attached, long *inherited)
{
    DCHECK(asks != NULL && attached != NULL && inherited != NULL,
           "the cross-agent ask census was asked to fill nothing");
    *asks = g_perform_asks; *attached = g_perform_attached; *inherited = g_perform_inherited;
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
    /* The asking engine's marks word, refused as engine_route refuses a delivery's. */
    if (!peer_marks_word_ok(remote_op_marks_field(op), strlen(remote_op_marks_field(op)))) {
        g_peer_marks_refused++;
        remote_op_free(op);
        return;
    }
    /* Routed to the wrong instance is the trusted zone's bug, and answering would return this document's frame
       count as the other's. Which of this agent's documents it names is the operation's business: the realm is
       selected where the program is queued (flow_perform), with the answering flow switched in, after other
       flows have had their turn to materialize a navigable this one may reach through. */
    DCHECK(world_doc_hosted(world_doc_intern(remote_op_doc(op))),
           "a cross-agent operation was routed to an instance that does not hold the document it names — the "
           "offscreen, which alone knows the placement, routed it wrongly");
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
               "the asking world has written in this instance, so the operation must run in a join of the answering "
               "flow's timeline with those writes, which the linear delta chain cannot express. Build the join "
               "engine_route names");
    }
    /* Which of this instance's timelines the asker is in, copied off the record before it is freed (`addr`
       points into the parse's text). ENGINE_ADDRESSEE_NONE is a real answer and the common one (engine.h): a
       flow that has taken no cross-instance answer is in none of our timelines, so every one may answer, and the
       fan-out is required, since `otherW.length` differs in each and all are true. */
    {
        const char *addr = remote_op_addressee(op);

        DCHECK(addr != NULL && *addr,
               "a cross-agent operation arrived with an empty addressee field — the grammar spells the unaddressed "
               "case as the ENGINE_ADDRESSEE_NONE token (solver/engine.h), so the two ends of the wire disagree");
        if (strcmp(addr, ENGINE_ADDRESSEE_NONE) != 0) {
            addressee = reclaim_strdup(addr);
            CHECK(addressee != NULL, "engine: OOM copying the timeline a peer addressed a cross-agent operation to — "
                                     "without it every timeline here would perform the operation, including ones the "
                                     "peer refuses to hear from");
        }
    }
    remote_op_free(op);

    n = flow_count();
    DCHECK(n > 0, "a cross-agent operation was asked of a document whose every timeline has finished — keep the "
                  "flows of a document a peer still references live");
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
           "a cross-agent operation named a timeline of this document that every live flow contradicts — every "
           "continuation of the addressed world has left the frontier, so the asking flow is parked on a question no "
           "timeline here will answer");
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

int engine_provenance_of_token(const char *token)
{
    CHECK(token != NULL, "engine: a host stated a provenance and handed no word");
    if (!strcmp(token, PENDING_PROVENANCE_OBSERVED)) return PROV_OBSERVED;
    if (!strcmp(token, PENDING_PROVENANCE_DERIVED)) return PROV_DERIVED;
    if (!strcmp(token, PENDING_PROVENANCE_FORCED)) return PROV_FORCED;
    CHECK_FAILF("engine: a host stated the provenance `%s`, which is none of the three engine.h spells", token);
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

/* See engine.h. The flow-less release arm answers `pinned`, the word that fires nothing at the default. */
int engine_pinned_of_running_path(void)
{
    const Flow *f = flow_running();

    DCHECK(f != NULL,
           "a request built by the page's code asked whether its address rests on a witness this engine chose, with "
           "no flow standing — the witness is a fact about one path, so the caller has lost the flow it acts for");
    return f == NULL || flow_path_pinned(f);
}

/* See engine.h. */
const char *engine_navigation_pinned_token(void)
{
    const Flow *f = flow_running();

    return engine_pinned_token(f != NULL && flow_path_pinned(f));
}

/* See engine.h. */
void engine_note_persistent_write(void)
{
    Flow *f = flow_running();

    if (f) f->persist_wrote = 1;
}

/* See engine.h. */
int engine_driven_of_running_path(void)
{
    const Flow *f = flow_running();

    return f != NULL && flow_path_driven(f);
}

/* See engine.h. A CHECK for engine_provenance_token's reason: the firing decision reads this field. */
const char *engine_driven_token(int driven)
{
    switch (driven) {
    case 0: return PENDING_DRIVEN_NO;
    case 1: return PENDING_DRIVEN_YES;
    }
    CHECK_FAILF("engine: a request's drive mark is %d, which is neither bit engine.h defines — the trusted "
                "zone's firing decision reads this field", driven);
}

/* Fetch §2.2.5's credentials mode, from the token a record carries: length-delimited because the record's string
   is read with its length, and derived from core/fetch/fetch.h's own wire spelling (`fetch_credentials_token`'s
   and nobody else's) rather than a second list of the three words. */
static FetchCredentialsMode cred_of_token(const char *tok, size_t n) {
    FetchCredentialsMode m;
    for (m = FETCH_CREDENTIALS_OMIT; m <= FETCH_CREDENTIALS_INCLUDE; m++) {
        const char *w = fetch_credentials_token(m);
        if (n == strlen(w) && !memcmp(tok, w, n)) return m;
    }
    CHECK_FAILF("engine: a line this join wrote carries the credentials token `%.*s`, which it cannot have "
                "written — the buffer is this function's alone", (int)n, tok);
}

/* The set's credentials mode is its least credentialed member. The dedup is over the request's identity
   (engine_pending_fetches), so one reply satisfies every park in the set; the initiator and provenance fold
   toward the most observed (a claim about evidence), but this is a claim about whose session pays, and the
   errors are asymmetric: fetching credentialed for a set containing an `omit` park hands that park a
   personalised body its algorithm forbade, while fetching uncredentialed for an `include` park only narrows. The
   fold is written over the enumerators, not their numbers, because core/fetch/fetch.h does not declare its order
   a rank.
   Named residual. Not covered: two parks with one identity and different credentials modes are two requests a
   browser makes twice, and this folds them into one. Next diff builds: the credentials mode in the key beside
   the mode (solver/pending_index.h's PendRequestKey), with engine_provide pairing on it. Absence shows as: a page
   whose `<img crossorigin=use-credentials src=u>` and `fetch(u)` name one address learns the logged-out body for
   the image, and the census counts one request where a browser's log shows two. */
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

/* Is `name` a member of the comma-joined asker set `set` (`n` bytes)? */
static int asker_set_has(const char *set, size_t n, const char *name, size_t nl) {
    const char *at = set, *end = set + n;

    while (at < end) {
        const char *comma = memchr(at, ',', (size_t)(end - at));
        const char *stop = comma ? comma : end;
        if ((size_t)(stop - at) == nl && !memcmp(at, name, nl)) return 1;
        at = stop + 1;
    }
    return 0;
}

/* Append `n` bytes to the join, growing it by doubling. */
static void join_put(char **join, size_t *n_out, size_t *cap, const char *s, size_t n) {
    while (*n_out + n + 1 > *cap) {
        *cap *= 2;
        *join = realloc(*join, *cap);
        CHECK(*join, "engine: OOM growing the pending-request join");
    }
    memcpy(*join + *n_out, s, n);
    *n_out += n;
    (*join)[*n_out] = 0;
}

/* The URLs the host owes, newline-joined and deduplicated over the request's identity, or "". Several flows park
   on one request (a candidate re-fire re-runs the exploring flow's fetches) and engine_provide fills every entry
   that names it, so the list is a set of requests, not of waiters; deduplicating over the URL alone would drop a
   POST behind a GET, and without the mode (Fetch §2.2.5's request mode) a `<script src=u>` and a `fetch(u)` would
   share a reply that Fetch §4.1 Main fetch gives one as a body and the other behind a CORS check.
   The set is solver/pending_index.h's: one node per identity, holding exactly the records the host can still be
   shown (keyed, unanswered, not refused, named by a live register), walked in creation order. So the work is the
   outstanding requests and not every register entry of every flow, and the identity's five strings are the
   node's own copies, parsed once at the park. The buffer is this function's, valid until the next call. Each
   line is `METHOD<TAB>DESTINATION<TAB>INITIATOR<TAB>PROVENANCE<TAB>PINNED<TAB>DRIVEN<TAB>CREDENTIALS<TAB>MODE<TAB>
   REFERRER_POLICY<TAB>REFERRER<TAB>ASKERS<TAB>URL`, ASKERS being the comma-joined names of the documents whose
   parks the line folds (engine_asker_name), each once. */
const char *engine_pending_fetches(void) {
    static char *join, *askers;
    static size_t cap, acap;
    size_t n_out = 0;
    long listed = 0;
    PendIndexNode *node;

    if (!join) { cap = 256; join = reclaim_malloc(cap); CHECK(join, "engine: OOM joining the pending requests"); }
    if (!askers) { acap = 64; askers = reclaim_malloc(acap); CHECK(askers, "engine: OOM joining a request's askers"); }
    join[0] = 0;
    for (node = pending_index_first(); node; node = pending_index_next(node)) {
        int n_mem = pending_index_node_count(node);
        const char *m = pending_index_node_method(node), *o = pending_index_node_mode(node);
        const char *rp = pending_index_node_referrer_policy(node), *r = pending_index_node_referrer(node);
        const char *u = pending_index_node_url(node);
        /* The fold's state: the line's destination, initiator, (provenance, witness, drive) vector, credentials
           mode and asker set, each stating what survives every member, independent of the members' order. */
        char dst[32];
        size_t dl = 0, a_n = 0;
        int parser = 0, prov = 0, pinned = 0, driven = 0;
        FetchCredentialsMode cred = FETCH_CREDENTIALS_INCLUDE;

        if (n_mem == 0) continue;   /* every record answered, refused or dropped; the node keeps its history */
        /* The line grammar, asked once of the identity every member shares: URL Standard §4.4 URL parsing removes
           all ASCII tab and newline, and a method is an RFC 9110 §5.6.2 token, so a line breaking either never
           went through the parser (a concolic shape carried as an address) and would split into records the
           host fetches. */
        DCHECK(!strpbrk(u, "\t\n"),
               "an outstanding request's URL holds a TAB or a NEWLINE — URL Standard §4.4 URL parsing removes both "
               "from its input, so this string never went through the parser and the joined line splits into "
               "records nobody parked on");
        DCHECK(method_is_token(m),
               "an outstanding request's METHOD is not a token — Fetch §2.2.1 Methods, and RFC 9110 §5.6.2 Tokens "
               "is the production; the joined line would not split back into the pair");
        (void)fetch_mode_of_token(o);   /* fatal on a word §2.2.5 does not define */
        DCHECK(fetch_is_referrer_policy(rp) && *r && !strpbrk(r, "\t\n"),
               "an outstanding request carries no Referrer Policy §3 referrer policy, or no referrer, or one a line "
               "cannot hold — pending_park_request writes both before the address, the policy through "
               "fetch_referrer_policy_for_wire and the referrer as \"no-referrer\" or a parsed URL, so this "
               "record bypassed that door");
        /* Fetch §4.3 Scheme fetch's closure, at the consumer. The trusted zone fetches only HTTP(S) (Fetch §2.1
           URL) and its refusal looks like a network failure, so a URL §4.3 answers inside this agent reaching
           this line is a park that never ran §4.3, owed an answer that can only be `blocked`.
           `pending_park_request` places the local answer; core/fetch/fetch.c's `fetch_owe` handles a request
           with a `deliver` closure. */
        DCHECK(scheme_fetch_is_network(pending_ctx(), u),
               "a request whose URL's scheme Fetch §4.3 Scheme fetch answers INSIDE this agent is about to be "
               "handed to the HOST'S network. The trusted zone fetches nothing but an HTTP(S) scheme (Fetch §2.1 "
               "URL) and its refusal is indistinguishable from a network failure, so this flow would park for the "
               "rest of the session on an answer that can only be `blocked`. The park that created this entry did "
               "not run §4.3 — route it through pending_park_request (this file)");
        a_n = 0;
        askers[0] = 0;
        for (int i = 0; i < n_mem; i++) {
            JSValue pe = pending_index_node_member(node, i);
            JSValue dv = pending_get(pe, PEND_DESTINATION), cv = pending_get(pe, PEND_CREDENTIALS);
            size_t mdl = 0, cl = 0, al;
            const char *md = JS_IsString(dv) ? JS_ToCStringLen(pending_ctx(), &mdl, dv) : NULL;
            const char *c = JS_IsString(cv) ? JS_ToCStringLen(pending_ctx(), &cl, cv) : NULL;
            /* What each member is evidence of, read off its record and never recomputed: it was true at the
               instant the request was built, and this walk runs later, over a flow that may since have taken a
               contradicted arm. HTML §4.12.1.1's `parser document` likewise (pending.h's `parserIns`). */
            int m_parser = (int)pending_get_int(pe, PEND_PARSER_INS) != 0;
            int m_prov = (int)pending_get_int(pe, PEND_PROV);
            int m_pinned = (int)pending_get_int(pe, PEND_PINNED);
            int m_driven = (int)pending_get_int(pe, PEND_DRIVEN);
            const char *asker = engine_asker_name(pe);
            FetchCredentialsMode m_cred;

            /* The member's half of the park's contract, asserted where the record is read. Only bytes are
               listed (pending_park_request declines an address with none), a synchronous host request has no
               address and belongs to engine_host_requests, and the destination and credentials mode are the
               record's own, written unconditionally by the park. */
            DCHECKF(pending_get_int(pe, PEND_ADDRESS) == FETCH_ADDRESS_CONCRETE ||
                    pending_get_int(pe, PEND_ADDRESS) == FETCH_ADDRESS_EXAMPLE,
                    "an outstanding request is about to be listed with the address class %d — only an address this "
                    "run has bytes for may reach a host, and pending_park_request declines the rest",
                    (int)pending_get_int(pe, PEND_ADDRESS));
            DCHECK(pending_get_int(pe, PEND_KIND) != FLOW_PENDING_HOSTREQ,
                   "a synchronous host request stands in the outstanding set — it is answered by "
                   "engine_host_answer against a request id, and pending_push never tracks one");
            DCHECK(md != NULL && fetch_is_destination_type(md),
                   "an outstanding request carries no DESTINATION Fetch §2.2.5 Requests enumerates — the trusted "
                   "zone reads it to decide whether the reply may be ingested as CODE, so a value outside the "
                   "enumeration would be read as `not code` and a script's reply ingested as data");
            DCHECK(c != NULL,
                   "an outstanding request carries no CREDENTIALS MODE — pending_park_request writes it "
                   "unconditionally after refusing the unplaced zero, so this record bypassed that door");
            /* The line's destination is copied into a fixed buffer, so its width is a memory bound in every
               build; §2.2.5's longest destination is far inside it. */
            CHECK(md == NULL || mdl < sizeof dst,
                  "engine: an outstanding request's destination is wider than any Fetch §2.2.5 destination");
            m_cred = cred_of_token(c, cl);   /* fatal on a word §2.2.5 does not define */
            if (i == 0) {
                memcpy(dst, md, mdl); dl = mdl;
                parser = m_parser; prov = m_prov; pinned = m_pinned; driven = m_driven; cred = m_cred;
            } else {
                /* The (provenance, witness, drive) vector folds as one, taking one member's whole, so the line
                   never states a vector no member held: folding each by its own extremum gave (derived,
                   unpinned, undriven) from a (forced, pinned, undriven) member and a (derived, unpinned,
                   driven) one, each refused alone. The order is whose act first: drive, then witness, then
                   provenance, the egress default's own discriminator, so a line fires by default exactly when one
                   member alone would; ties fold toward most observed. */
                int take = m_driven < driven ||
                           (m_driven == driven && (m_pinned < pinned || (m_pinned == pinned && m_prov < prov)));
                int f_prov = take ? m_prov : prov, f_pinned = take ? m_pinned : pinned;
                int f_driven = take ? m_driven : driven;
                DCHECK((f_prov == prov && f_pinned == pinned && f_driven == driven) ||
                       (f_prov == m_prov && f_pinned == m_pinned && f_driven == m_driven),
                       "a folded request line states a (provenance, witness, drive) vector neither the line nor the "
                       "member it folds holds — the zone judges the line as one request, so the fold would fire an "
                       "act no member was permitted as the page's own");
                prov = f_prov; pinned = f_pinned; driven = f_driven;
                /* The initiator folds to parser-inserted if any member is. The destination folds to the
                   strictest ingestion: if any member compiles the reply the bytes must pass the code rule, so
                   script-like (§2.2.5) wins; otherwise the line keeps what it has, since no consumer
                   distinguishes, say, `image` from `font`. The credentials mode folds the other way (see
                   `cred_narrower` for the rule and its residual). */
                parser = parser || m_parser;
                if (destination_is_script_like(md, mdl) && !destination_is_script_like(dst, dl)) {
                    memcpy(dst, md, mdl); dl = mdl;
                }
                cred = cred_narrower(cred, m_cred);
            }
            /* The asker set is the union of the members' documents, so the zone judges the one reply against
               every document it fills. */
            al = strlen(asker);
            if (!asker_set_has(askers, a_n, asker, al)) {
                if (a_n) join_put(&askers, &a_n, &acap, ",", 1);
                join_put(&askers, &a_n, &acap, asker, al);
            }
            if (md) JS_FreeCString(pending_ctx(), md);
            if (c) JS_FreeCString(pending_ctx(), c);
            JS_FreeValue(pending_ctx(), cv);
            JS_FreeValue(pending_ctx(), dv);
            JS_FreeValue(pending_ctx(), pe);
        }
        listed += n_mem;
        {
            const char *ini = parser ? PENDING_INITIATOR_PARSER : PENDING_INITIATOR_SCRIPT;
            const char *f[11];
            f[0] = m; f[1] = NULL; f[2] = ini; f[3] = engine_provenance_token(prov);
            f[4] = engine_pinned_token(pinned); f[5] = engine_driven_token(driven);
            f[6] = fetch_credentials_token(cred); f[7] = o; f[8] = rp; f[9] = r; f[10] = askers;
            for (int k = 0; k < 11; k++) {
                if (k == 1) join_put(&join, &n_out, &cap, dst, dl);
                else        join_put(&join, &n_out, &cap, f[k], strlen(f[k]));
                join_put(&join, &n_out, &cap, "\t", 1);
            }
            join_put(&join, &n_out, &cap, u, strlen(u));
            join_put(&join, &n_out, &cap, "\n", 1);
        }
    }
    /* Every keyed record was listed exactly once: the gauge rises where a record joins a node and falls where it
       leaves one (solver/pending_index.c), so a difference is a member this walk skipped or counted twice. */
    DCHECKF(listed == pending_index_keyed_now(),
            "the pending-request join listed %ld outstanding record(s) and the outstanding set holds %ld — the "
            "walk and the set's own gauge read one population", listed, pending_index_keyed_now());
    return join;
}

void engine_pending_split(char *line, const char **method, const char **destination, const char **initiator,
                          const char **provenance, const char **pinned, const char **driven,
                          const char **credentials, const char **mode, const char **referrer_policy,
                          const char **referrer, const char **askers, const char **url) {
    const char **field[] = { method, destination, initiator, provenance, pinned, driven, credentials, mode,
                             referrer_policy, referrer, askers, url };
    enum { NFIELDS = sizeof field / sizeof field[0] };
    char *at = line;

    DCHECK(line && method && destination && initiator && provenance && pinned && driven && credentials && mode &&
           referrer_policy && referrer && askers && url, "a pending line was split with nowhere to put its fields");
    /* A CHECK per TAB, not a DCHECK: release has no defined answer, because the fields key the delivery and
       decide what fires, and a line not in this engine's grammar would decide them from the wrong field. A URL
       cannot contain a TAB (URL Standard §4.4), so the URL is the remainder of the line. */
    for (int k = 0; k < NFIELDS - 1; k++) {
        char *tab = strchr(at, '\t');
        CHECKF(tab != NULL,
               "engine: a host split a pending line carrying %d TABs where engine_pending_fetches writes %d "
               "(`METHOD<TAB>DESTINATION<TAB>INITIATOR<TAB>PROVENANCE<TAB>PINNED<TAB>DRIVEN<TAB>CREDENTIALS<TAB>"
               "MODE<TAB>REFERRER_POLICY<TAB>REFERRER<TAB>ASKERS<TAB>URL`) — the host is reading another grammar, "
               "and field %d would be decided from the wrong bytes", k, NFIELDS - 1, k + 1);
        *tab = 0;
        *field[k] = at;
        at = tab + 1;
    }
    *field[NFIELDS - 1] = at;
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
    DCHECK(!strcmp(*driven, PENDING_DRIVEN_YES) || !strcmp(*driven, PENDING_DRIVEN_NO),
           "a pending line's DRIVE MARK is neither of the two tokens engine.h declares — the trusted zone's "
           "`actor` row reads it, and an unknown word would let a drive's request fire as the page's own");
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
    /* The request mode, through `fetch_mode_of_token`, which is fatal on an undefined word. */
    (void)fetch_mode_of_token(*mode);
    DCHECK(fetch_is_referrer_policy(*referrer_policy) && **referrer,
           "a pending line's REFERRER POLICY is not a Referrer Policy §3 referrer policy, or its REFERRER is "
           "empty — the join writes the policy through fetch_referrer_policy_for_wire and the referrer as "
           "\"no-referrer\" or a URL, so this is the writer and the reader having parted");
    DCHECK(**askers && (*askers)[strlen(*askers) - 1] != ',' && **askers != ',' && !strstr(*askers, ",,"),
           "a pending line's ASKERS is not a comma-joined set of document names — the join writes the asking "
           "document of every park it folds, so an empty member is the writer and the reader having parted");
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
                   "a flow is waiting on a register entry the host is never shown — engine_pending_fetches lists "
                   "unanswered entries with an address and engine_host_requests unanswered synchronous ones, so an "
                   "entry that is neither stalls the frontier");
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

/* Delivers a body for the request `(method, mode, referrer policy, referrer, url)` into every flow parked on
   it. The value lands on each flow's own pending entry, so the reaction the resolve enqueues belongs to that flow
   and its COW delta. The method is part of the key because matching on the URL alone would settle a POST's
   promise with a GET's reply, the mode (Fetch §2.2.5's request mode) because the trusted zone answers a `cors`
   request and a `no-cors` one to the same address differently, and the referrer pair because two referrers are
   two requests to the server. Returns how many entries it filled. */
int engine_provide(JSContext *ctx, const char *method, const char *mode, const char *referrer_policy,
                   const char *referrer, const char *url, JSValueConst value) {
    PendRequestKey key = { method, mode, referrer_policy, referrer, url };
    PendIndexNode *node;
    int n = 0, matched = 0, i, m;
    /* The reply's own grade, joined from the records it answers (see the fill loop). `-1` is "no record read
       yet", which the tail refuses to hand on. */
    int reply_prov = -1;
    int reply_driven = -1;   /* the same join for the drive mark: `undriven` if any record's flow was not a drive */
    DCHECK(url != NULL, "a body was provided for no URL");
    /* What the host sends back must be what the join emitted: an absent method is a host not converted to the
       pair (the JS bridge's `Provide` carries one), and an address in the method's place is that host with its
       operands shifted. Both would match nothing, and the parked flow would silently wait for the session. */
    DCHECK(method != NULL,
           "a reply was provided with no method — the reply seam is keyed on (method, url), and "
           "engine_pending_fetches hands the host both halves");
    DCHECK(method_is_token(method),
           "a reply was provided with a method that is not a token (Fetch §2.2.1, RFC 9110 §5.6.2) — the host's "
           "operands are shifted by one");
    /* The mode the line stated, handed back; the mapping is fatal on a word Fetch §2.2.5 does not define. */
    DCHECK(mode != NULL && referrer_policy != NULL && referrer != NULL,
           "a reply was provided with no MODE, REFERRER POLICY or REFERRER — the reply seam is keyed on all "
           "five fields the pending line states");
    (void)fetch_mode_of_token(mode);
    /* One lookup in the frontier's outstanding set (solver/pending_index.h) rather than a walk of every flow's
       register: the walked set does not shrink (a flow keeps an answered entry until it delivers, and @S
       re-fires re-issue their fetches), so a walk costs every request the session ever made, per reply.
       Membership is the predicate the walk used (an entry with an address, not a synchronous request, no value
       yet), and a record leaves only when answered or when the last register naming it is gone, so nothing is
       dropped. */
    node = pending_index_find(&key);
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
               "the frontier's outstanding set handed a reply an already-answered record — a record leaves that set "
               "on the write that answers it");
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
                    "a reply with no `urlList` is being written onto a fetch or subresource entry — the trusted "
                    "zone's reply path built this record, not this file's register. request=%s %s", method, url);
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
        {
            int rd = (int)pending_get_int(p, PEND_DRIVEN);

            (void)engine_driven_token(rd);
            reply_driven = reply_driven < 0 ? rd : (reply_driven && rd);
        }
        pending_set(p, PEND_VALUE, JS_DupValue(ctx, value));
        reply_arrived(NULL, p);
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
    if (n) reply_decode_learn(ctx, method, url, value, reply_prov, reply_driven);
    /* A request answered twice, told apart from one answered for nobody. Every entry naming the request already
       has a reply, so this call wrote none. `n == 0` with nothing matched is the host's pairing being off, the
       caller's assert (it owns the paged-sale credit that can explain it); this case no credit excuses, since a
       request leaves the join once answered. It catches a host with two overlapping lists, which is why facts
       about a request travel on the request itself. */
    if (matched && !n)
        DFAILF("a reply was provided for a request whose every parked entry is already answered — the join drops a "
               "request once it carries a value, so the host answered it twice. request=%s %s", method, url);
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
int engine_decline(JSContext *ctx, const char *method, const char *mode, const char *referrer_policy,
                   const char *referrer, const char *url, const char *reason) {
    PendRequestKey key = { method, mode, referrer_policy, referrer, url };
    PendIndexNode *node;
    int matched, marked = 0, i;
    JSValue rv;

    DCHECK(method != NULL && mode != NULL && referrer_policy != NULL && referrer != NULL && url != NULL,
           "a refusal was recorded for no request — a decline answers the same five-field identity a reply "
           "answers, because it is an answer to the same question, and one that names part of it refuses "
           "nothing and leaves every flow parked");
    DCHECK(method_is_token(method),
           "a refusal names a method that is not a token (Fetch §2.2.1, RFC 9110 §5.6.2) — the host's operands are "
           "shifted by one");
    /* The reason is required and non-empty: it is the only account of a request nobody made, whose flow will
       not drain this session, and it decides what happens next (`blocked-signal` names the signal and value of
       the person's per-origin egress control that holds it, `blocked-destructive` a refusal nothing reopens);
       only the zone that applied the rule knows which. Nothing here matches on the string (trusted.mjs asks the
       policy for the grade, never a `statusText`). */
    DCHECK(reason != NULL && *reason != '\0',
           "a request was refused with no reason — the refusing party states which rule holds the frontier");
    (void)fetch_mode_of_token(mode);
    node = pending_index_find(&key);
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
               "the frontier's outstanding set holds an already-refused record — a record leaves its pair on the "
               "write that refuses it");
        DCHECK(!pending_get_int(p, PEND_HAVE_VALUE),
               "the frontier's outstanding set holds an answered record — a record leaves that set on the write that "
               "answers it");
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
               "pending/decline pairing is off. request=%s %s reason=%s",
               method, url, reason);
    return marked;
}

/* The same refusal for a synchronous request; engine.h says why it is keyed on the rendezvous and is a second
   entry rather than a third completion. The walk is engine_host_answer's, the one other writer that reaches a
   HOSTREQ record from outside the asking flow, so it is the statement of where such a record can be. It stops at
   the first match: exactly one register names an unanswered rendezvous, because a fork unshares it and mints a
   fresh id. */
/* Unaddressed rendezvous opened by the step running now, refused when it returns. */
static int g_unaddressed_opened;

/* See engine.h. The rendezvous is opened and marked; the scheduler refuses it once the step returns. A refusal
   written while the step runs lands before the step's own host-owed report, which would then hold the flow out
   of the pick with nothing left to clear it. */
uint32_t engine_host_request_unaddressed_async(JSContext *ctx, const char *op, JSValueConst deliver) {
    return host_request_make_async(ctx, engine_host_request_unaddressed(ctx, op), deliver);
}

uint32_t engine_host_request_unaddressed(JSContext *ctx, const char *op) {
    Flow *f = flow_running();
    uint32_t req = engine_host_request(ctx, op);
    int marked = 0;

    for (int i = 0, n = pending_count(f->pending); i < n; i++) {
        JSValue p = pending_entry(f->pending, i);
        if (pending_get_int(p, PEND_KIND) == FLOW_PENDING_HOSTREQ && (uint32_t)pending_get_int(p, PEND_REQ) == req) {
            pending_set_int(p, PEND_ADDRESS, FETCH_ADDRESS_UNKNOWN);
            marked++;
        }
        JS_FreeValue(ctx, p);
    }
    DCHECK(marked == 1, "a rendezvous opened for an unaddressed request is not on its flow's register once — "
                        "engine_host_request parks exactly one entry under the id it returns");
    g_unaddressed_opened++;
    return req;
}

/* Refuses every unaddressed rendezvous the step that just returned opened, as the trusted zone refuses one it
   will not make: engine_host_decline marks it and clears its flow's host-owed mark, so flow_decline_fork builds
   its two arms at that flow's next pick. Runs before the slice can return, so no host is shown one. Ids are
   collected first because the refusal walks the same registers. */
static void host_requests_decline_unaddressed(void)
{
    uint32_t *ids = NULL;
    int n_ids = 0, cap = 0;
    Flow *f;

    if (!g_unaddressed_opened) return;

    for (int k = 0; (f = flow_at(k)) != NULL; k++)
        for (int i = 0, n = pending_count(f->pending); i < n; i++) {
            JSValue p = pending_entry(f->pending, i);
            if (pending_get_int(p, PEND_KIND) == FLOW_PENDING_HOSTREQ &&
                pending_get_int(p, PEND_ADDRESS) == FETCH_ADDRESS_UNKNOWN && pending_entry_host_owed(p)) {
                if (n_ids == cap) {
                    cap = cap ? cap * 2 : 4;
                    ids = realloc(ids, (size_t)cap * sizeof *ids);
                    CHECK(ids, "engine: OOM collecting unaddressed rendezvous");
                }
                ids[n_ids++] = (uint32_t)pending_get_int(p, PEND_REQ_HOST);
            }
            JS_FreeValue(pending_ctx(), p);
        }
    DCHECKF(n_ids >= 1, "%d unaddressed rendezvous were opened in the step that just returned and no register "
            "holds one — the flow parked on it left the frontier within its own step", g_unaddressed_opened);
    for (int i = 0; i < n_ids; i++) {
        int marked = engine_host_decline(pending_ctx(), ids[i], PENDING_DECLINED_UNADDRESSED);
        DCHECK(marked == 1, "an unaddressed rendezvous collected a moment ago was not found to refuse");
        (void)marked;
        g_reply_unaddressed++;
    }
    g_unaddressed_opened = 0;
    free(ids);
}

int engine_host_decline(JSContext *ctx, uint32_t req, const char *reason) {
    DCHECK(req != 0, "a synchronous request with no id was refused — the id is this kind's only identity");
    /* The reason is required, for the address-keyed refusal's reason. */
    DCHECK(reason != NULL && *reason != '\0',
           "a synchronous request was refused with no reason — the refusing party states which rule holds the "
           "machine");
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
                   "a synchronous request carries both an answer and a refusal — the trusted zone answers or refuses "
                   "a rendezvous, not both");
            DCHECK(!pending_entry_declined(p),
                   "a synchronous request was refused twice — the waiting arm was built from the first reason");
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

/* HTML §13.2.7 "The end": the parse's own scripts run before step 5's list, and step 5 runs before the
   DOMContentLoaded task is queued, so a parser-inserted script row's bytes reach a document whose
   DOMContentLoaded has not run. The readiness cannot say so: step 3 makes it "interactive" before step 5. A
   failed load counts: §4.12.1.1 step 4's `error` is that script's execution. Asserted at the arrival, where both
   its document and its park are in hand. A macro so each arm reports its own line; under the dev guard because
   the read interns an atom. */
#if APICLIENT_DEV
#define ENGINE_DCHECK_PARSER_SCRIPT_BEFORE_DCL(p, doc) do {                                                    \
        uint32_t pdoc_ = (doc);                                                                          \
        DCHECKF(!pending_get_int((p), PEND_PARSER_INS) || !document_dcl_queued(doc_realm(pdoc_)),        \
                "a parser-inserted script's bytes arrived for document %u after its DOMContentLoaded: the " \
                "parser's scripts hold DOMContentLoaded, so it fired while this one was owed", pdoc_);   \
    } while (0)
#else
#define ENGINE_DCHECK_PARSER_SCRIPT_BEFORE_DCL(p, doc) do { } while (0)
#endif

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
 * `pending_answer_rendezvous`) meet and pend_index_sync dispatches, and the selector choosing the smallest. Absence
 * shows as: engine/solvergate.mjs's `lastreply` schedule disagreeing with `direct` on a corpus document whose
 * two concurrently answered fetches' `.then` handlers observe each other. */
static void flow_deliver_one_reply(JSContext *ctx, Flow *f) {
    int i = pending_next_deliverable(f->pending);
    if (i >= 0) {
        JSValue p = pending_entry(f->pending, i);
        JSValue pv;
        int kind;

        f->replies_taken++;
        /* No entry is delivered while a settle of this register is still parked: its reactions are not queued
           yet (27.5.1.4 FulfillPromise step 7), and anything delivered first would enqueue ahead of them. Both
           homes of a park are asked (on the Flow while switched out, in the runtime's FIFO while switched in), as
           flow_answer_fork and flow_finish do. It catches a caller arriving with a continuation outstanding,
           which flow_step's resume arm exists to prevent. */
        DCHECK(!JS_HasParkedFlow(JS_GetRuntime(ctx)) && f->parked == NULL,
               "a reply is about to be delivered while a settle from this flow's register is still parked — its "
               "reactions must be enqueued first; resume the parked continuations first (flow_step's resume arm)");
        /* Under this flow's own delta, because the delivery runs the page's code; flow_deliver asserts the same
           for a routed message. */
        DCHECK(flow_running() == f,
               "a reply was delivered while another flow was switched in — the resolving function would run against "
               "that flow's delta");
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
        if (kind != FLOW_PENDING_RESOLVE && kind != FLOW_PENDING_RESOURCE && kind != FLOW_PENDING_HOSTREQ &&
            (JS_IsNull(pv) || !script_fetch_status_ok(fetch_reply_status(ctx, pv)) ||
             (kind == FLOW_PENDING_MODULE && !module_reply_is_javascript(ctx, pv)))) {
            DCHECK(kind == FLOW_PENDING_SCRIPT || kind == FLOW_PENDING_DOCSCRIPT ||
                   kind == FLOW_PENDING_MODULE,
                   "a failed answer reached the program kinds' failure arm for another kind — a park of a new kind "
                   "is being delivered with no arm of its own");
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
                       "a dynamic import's park carries no reject capability — engine_pending_module_url asserts "
                       "both halves at the push, so another park pushed this record");
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
                           "an external document script's failure names a row this flow does not hold — the entry "
                           "was delivered into another flow, or a destroy took its row and not the entry");
                    DCHECK(f->dyn_cand[di] == DYN_SCRIPT_SRC,
                           "an external document script's failure named a row that is not awaiting a program "
                           "— the row already holds one, so this is a second answer to one request");
                    ENGINE_DCHECK_PARSER_SCRIPT_BEFORE_DCL(p, f->dyn_doc[di]);
                    DCHECK(f->dyn_el[di] != NULL,
                           "an external document script's row carries no element — §4.12.1.1 fires `error` at el, "
                           "and both seams that queue such rows (engine_queue_docscript_url, the seed's address arm) "
                           "take the element");
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
                           "an injected <script src> failed for a park carrying no element — the park writes the "
                           "element's wrapper and §4.12.1.1 step 4 fires `error` at it");
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
                   "an external document script replied for a row this flow does not hold — the entry was delivered "
                   "into another flow, or a destroy took its row and not the entry");
            DCHECK(f->dyn_cand[di] == DYN_SCRIPT_SRC,
                   "an external document script replied for a sequence position that is not awaiting one — the "
                   "slot holds a program already, so this reply is a second answer to one request and the "
                   "program it overwrites would never run");
            ENGINE_DCHECK_PARSER_SCRIPT_BEFORE_DCL(p, f->dyn_doc[di]);
            /* Which of §8.1.4.2 Fetching scripts' two decodes runs is the row's own type: a module's bytes are
               UTF-8 whatever the response says, a classic script's go through the response's charset label. The
               encoding realm is the entry's document's. */
            DCHECK(script_type_executes((ScriptType)f->dyn_type[di]),
                   "an external document script replied for a row whose type executes nothing — only an executing "
                   "`<script>` becomes a DYN_SCRIPT_SRC row");
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
                   "an external document script's row already held an address before its reply arrived — the row's "
                   "body is its URL until this delivery takes it, so the reply was delivered twice or the row was "
                   "queued wrongly");
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
                       "an external document script's row holds an address containing U+0000 — the URL body is read "
                       "as a NUL-terminated string, so the row was filled by something other than "
                       "engine_queue_docscript_url or the seed");
                f->dyn_url[di] = reclaim_strdup(dyn_body_text(f->dyn[di]));
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
                   "an injected <script src> replied for a park whose type executes nothing — §4.12.1.1 fetches only "
                   "for the two executing types");
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
        } else if (kind == FLOW_PENDING_HOSTREQ) {
            /* An async rendezvous (engine_host_request_async): the answer goes to the asker's processResponse
               as a task of this flow. A zone answer is a normal completion, a network error included. */
            JSValue deliver = pending_get(p, PEND_RESOLVE);
            DCHECK(pending_get_int(p, PEND_ASYNC) && JS_IsFunction(ctx, deliver),
                   "a blocking rendezvous reached the reply delivery — its asking machine takes it at its park");
            DCHECK(pending_get_int(p, PEND_COMPLETION) == ENGINE_COMPLETION_NORMAL,
                   "an async rendezvous was answered with a throw — the zone answers a fetch out of the network, "
                   "and a network error is a value");
            if (JS_CallAsFlow(ctx, deliver, pv) < 0) {
                JSValue exc = JS_GetException(ctx);
                JS_FreeValue(ctx, exc);   /* a listener's throw is the page's to observe, not this step's */
            }
            JS_FreeValue(ctx, deliver);
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
            /* A reply no server sent (fetch_reply_unknown, flow_decline_fork's success arm) is an outcome nothing
               observed, so from its delivery on this flow stands on a forced arm: what the page builds from it
               is graded forced and kept out of the derived pool, as the same decline's failure arm is. Marked
               here rather than at the fork because a sibling is born with no mark its parent lacks. */
            if (fetch_reply_unobserved(ctx, pv)) {
                flow_mark_forced_arm();
                DCHECK(flow_path_forced(f), "a flow took a reply no server sent and its path is not forced");
            }
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
    DFAIL("a reply delivery found nothing to deliver — pending_ready reported an answered non-HOSTREQ entry that the "
          "scan of the same register did not find");
}

/* Snapshot-fork handoff: solver_decide stashes the sibling's hot decision and pins here at a forking branch; the
   interpreter then clones the frame and calls engine_fork_finalize, which assembles the sibling. */
static void *g_fork_dec = NULL, *g_fork_pins = NULL;
/* …and the parent's path marks at that instant, since the branch marks its own arm before the clone. */
static FlowPathMarks g_fork_marks;
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
                        const char *site, int assumed_slot) {
    Flow *f = flow_running();
    FlowPathMarks marks;

    DCHECK(f != NULL, "a sibling was prepared with no flow running — the blobs would describe no timeline");
    DCHECK(g_sess_forking,
           "a fork was prepared inside a non-forking session — an @S candidate re-fire is one concrete path, and "
           "decide_arm's non-forking branch answers with the arm the site declared");
    if (!g_fork_snapshot_owed) {
        /* Nobody will clone a frame, so the sibling's resume point must be the flow's own state, which holds for
           engine code re-reached by re-running the flow's scheduler step: HTML §8.1.7.3 Processing model step
           2.1 chooses "in an implementation-defined manner", between tasks, with nothing on the stack. The
           sibling has no frame and its step re-runs the same walk, where the arm recorded here replays.
           Assembled now, because no later moment has the parent still switched in. */
        if (restartable) {
            DCHECK(flow_frame(f) == NULL,
                   "a restartable fork was asked for by a flow holding a live program frame — the sibling would "
                   "re-enter that frame instead of re-reaching the ask");
            DCHECK(!JS_HasActivation(JS_GetRuntime(ctx)),
                   "a restartable fork was asked for with page code on the stack (a queued job's callback, a "
                   "listener in a rendering step) — the sibling would resume at a step that does not re-reach the "
                   "ask");
            marks = flow_path_marks(f);
            marks.assumed_slot = assumed_slot;
            engine_sibling_assemble(ctx, f, NULL, dec_blob, pin_blob, marks);
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
            DFAILF("a C builtin forked over unknown input from inside its own activation, so the sibling has nowhere "
                   "to resume. Declare the builtin a step machine (JS_CFUNC_STEP_DEF) and move this question into "
                   "its branch seam, step_tobool_run, so the driver snapshots the machine at the ask. Ask site: %s. "
                   "Question: %s",
                   site ? site : "(no ask site)", asked ? name : "(no source identity)");
        }
#endif
        (void)asked;
    }
    DCHECK(g_fork_dec == NULL && g_fork_pins == NULL,
           "a sibling's snapshot state was prepared while a previous one was unconsumed — the branch that prepared "
           "it never reached its fork hook");
    g_fork_dec = dec_blob; g_fork_pins = pin_blob; g_fork_marks = flow_path_marks(f);
    g_fork_marks.assumed_slot = assumed_slot;
    return 1;
}

uint64_t engine_flow_timeline(void) {
    /* A named enqueue owner is the timeline the queued work belongs to (engine_enqueue_job reads it first). */
    Flow *f = g_enqueue_owner ? g_enqueue_owner : flow_running();

    if (!f) return 0;
    return ((uint64_t)f->world.session << 48) ^ ((uint64_t)f->world.doc << 32) ^ (uint64_t)f->world.serial;
}

/* The world a timeline handle names: this instance's document and session, since a flow's world is minted here. */
static WorldId engine_timeline_world(uint32_t handle) {
    WorldId w = { world_local_doc(), handle, world_session() };

    DCHECK(handle != 0, "a timeline handle of 0 names the baseline, which is no world");
    return w;
}

uint32_t engine_flow_timeline_handle(void) {
    Flow *f = g_enqueue_owner ? g_enqueue_owner : flow_running();

    if (!f) return 0;
    DCHECK(f->world.doc == world_local_doc() && f->world.session == world_session() && f->world.serial != 0,
           "a flow runs under a world this instance did not mint in this session, so its serial alone does not "
           "name it and a function object stamped with it could be matched against another document's timeline");
    return f->world.serial;
}

int engine_flow_timeline_continues(uint32_t handle) {
    Flow *f = g_enqueue_owner ? g_enqueue_owner : flow_running();

    return f != NULL && world_descends_from(f->world, engine_timeline_world(handle));
}

/* Whether a flow whose world is `w` can reach the closure `fn`: it was built outside every flow, or in `w`'s
   timeline or one `w` descends from. A closure another timeline built holds that timeline's environment. */
static int engine_world_reaches_fn(WorldId w, JSValueConst fn) {
    uint32_t h = JS_FunctionTimeline(fn);

    return h == 0 || world_descends_from(w, engine_timeline_world(h));
}

/* Generator-state fork stash: clone_deep_flow calls gen_fork for each generator body frame it clones (during
   JS_FlowClone), before engine_fork_finalize has the sibling's delta. Appended here; the finalize drains them onto
   the new sibling's delta and resets. Filled and fully drained within one fork. */
typedef struct { JSValueConst genobj; JSForkRecordKind kind; void *g0, *g1; } GenForkRec;
static GenForkRec *g_genforks = NULL; static int g_genfork_n = 0, g_genfork_cap = 0;
void engine_gen_fork(JSContext *ctx, JSValueConst genobj, JSForkRecordKind kind, void *base_gd, void *cur_gd) {
    (void)ctx;
    if (g_genfork_n >= g_genfork_cap) {
        g_genfork_cap = g_genfork_cap ? g_genfork_cap * 2 : 8;
        g_genforks = realloc(g_genforks, (size_t)g_genfork_cap * sizeof(GenForkRec));
        CHECK(g_genforks, "engine: OOM generator-fork stash");
    }
    g_genforks[g_genfork_n].genobj = genobj; g_genforks[g_genfork_n].kind = kind;
    g_genforks[g_genfork_n].g0 = base_gd; g_genforks[g_genfork_n].g1 = cur_gd;
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
 * `dec_blob` and `pin_blob` are consumed; they become the sibling's. `marks` is the parent's path marks read
 * where the blobs were taken. */
static Flow *engine_sibling_assemble(JSContext *ctx, Flow *parent, JSValue *clone,
                                     void *dec_blob, void *pin_blob, FlowPathMarks marks) {
    DCHECK(parent != NULL, "a sibling was assembled with no parent flow — every field below is copied from one");
    DCHECK(dec_blob != NULL, "a sibling was assembled with no decision state — every branch its parent took would be "
                             "re-asked");
    DCHECK(!parent->typing_due,
           "a flow forked before performing the typing act its drive was assembled with — the act is performed at "
           "its first switch-in, before any opcode that could fork");
    /* A frameless sibling is safe only over a frameless parent. A parent inside a program would give a hot
       sibling (`started` is set below) with nothing to resume, and flow_step would compile the row at
       `script_i` again and replay its side effects. A parent with no frame is between programs, and so is its
       sibling, which re-enters its scheduler step where the parent was: a fork asked between tasks
       (engine_prepare_fork), the cold tier's rebuilds (flow.h), and the orphan drive's fresh call frame over a
       frameless parent all fit. */
    DCHECK(clone != NULL || flow_frame(parent) == NULL,
           "a fork of a flow inside a program arrived with no frame snapshot — the sibling would recompile the row "
           "and replay the parent's side effects");
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
    /* The decision this fork is taken at is where both arms stand (solver/flow.c's FlowSite): the page frame
       standing at the branch names it, so the parent is moved there before the arm copies its site. A fork asked
       with no page frame standing (between tasks, at a park) or for a flow not holding the thread leaves the
       parent where it stood. */
    {
        uint64_t site;
        if (parent == flow_running() && JS_RunningSiteHash(ctx, &site)) flow_stand_at(parent, FLOW_SITE_CODE, site);
    }
    /* Unseeded: an arm inherits its parent's sequence (copied below), so seeding would allocate the root
       document's rows onto it and leak them under the copy. */
    Flow *sib = flow_add_unseeded(ctx, parent->fn, fork_point);
    parent->world = world_mint_child(fork_point);
    /* Its account is a child of the parent's too: the WFQ's two terms, taken over at the branch. First, because
       it decides where the flow enters the queue. */
    flow_fork_inherit(sib, parent, marks);
    sib->started = 1;                 /* hot: resume from the blobs (and the cloned frame, when there is one) */
    /* The clone is of the parent's selected agent's base, so the arm selects that agent; every other agent's
       base and parks are the parent's state too, cloned onto the arm. */
    flow_agents_fork(ctx, sib, parent);
    flow_frame(sib) = clone;               /* the frame snapshot taken at the branch; NULL for a fork between tasks */
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
    sib->persist_wrote = parent->persist_wrote;
    /* The modelled user acts the parent's world has had performed: the arm stands in them. */
    flow_user_acts_copy(sib, parent);
    /* How far the sequence has been run, so the no-replay assert at the compile site holds for an arm, which has
       run what its parent ran; a frameless sibling is the shape that can reach that compile. */
    sib->last_compiled = parent->last_compiled;
    /* No parse in flight: a branch is taken by running bytecode, and a compiling flow runs none, so a parent
       holding one means a fork from inside the compiler. Asserted rather than zeroed. */
    DCHECK(parent->compile == NULL,
           "a flow was forked while its program was still being parsed — branches are taken by running bytecode, so "
           "the fork came from inside the parser or the compile handle outlived its program");
    DCHECK(sib->compile == NULL, "a freshly built sibling already held a suspended compile");
    /* Where the program it stands in has already interposed (flow.h): the arm inherits the parent's `dyn` rows
       and so their interpositions; without this pair it would compute its next immediate slot as if none had
       happened. A pair, because `imm_next` means nothing without the cursor it was computed against. */
    sib->imm_at = parent->imm_at; sib->imm_next = parent->imm_next;
    sib->spin_row = parent->spin_row;
    sib->replies_taken = parent->replies_taken;
    sib->delta = cow_delta_fork(ctx, (CowDelta *)parent->delta);   /* O(1) shared base segment, then diverges */
    /* Generator-state swaps built by clone_deep_flow for this fork, recorded on the sibling's delta so the shared
       generator object resolves to the sibling's own cloned execution state while it runs. */
    for (int i = 0; i < g_genfork_n; i++)
        cow_delta_add_gendata(ctx, (CowDelta *)sib->delta, g_genforks[i].genobj, g_genforks[i].kind,
                              g_genforks[i].g0, g_genforks[i].g1);
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
        DCHECK(parent->cand_payload && parent->fire.kind != FIRE_NONE,
               "a candidate session forked holding only part of its substitution — the source, the payload and "
               "exactly one of a sink class or a witness verification key are one identity, and the arm would "
               "inject nothing or be unable to say what it was trying");
        sib->cand_src = reclaim_strdup(parent->cand_src);
        sib->cand_payload = reclaim_strdup(parent->cand_payload);
        CHECK(sib->cand_src && sib->cand_payload, "engine: OOM forking an @S candidate session's substitution");
        fire_target_copy(&sib->fire, &parent->fire);
        flow_deliveries_copy(sib, parent);
        sib->cand_fired = parent->cand_fired;
        /* Where the payload came from is a fact about the bytes, so both arms have it; an arm without it would
           claim to be seeded from this session's search list with no row there, which solve.c's arrival assert
           refuses at the first branch a resumed candidate takes. */
        sib->cand_resumed = parent->cand_resumed;
    }
    /* Being a driven orphan travels: an arm of an orphan drive is that drive continued, no more reproducible from
       a decision vector than its parent, so it carries the drives its parent has called (their chain, the body
       running now in `fn` and its name) and the cold tier writes them for it. */
    sib->orphan_want = 0;
    /* The arm drives the same body, so a re-drive from it builds the same call shape. */
    sib->orphan_argc = parent->orphan ? parent->orphan_argc : 0;
    /* The wait does not travel. A parked drive is one flow, so its claim is held by one: the claimant, whose
       replay builds each call where its work runs out. An arm the replay forks before a call is built is an arm
       of the drives already called (none, before the first), so it carries only those; were it to inherit the
       claim, every arm of the replay would build the call and park as the same drive of the same body. Staying
       with the parent keeps the claim on the drive's recorded side: a branch or completion arm is a slot the
       replay consumes, and a peer's answer or a delivery is a commitment row ('r') that deliver_admits applies
       when the replay re-asks, so the timeline a fork hands the sibling is one the drive's record refuses. A peer
       whose own session ended is the exception flow_answer_fork's residual names. */
    {
        int called = parent->orphan_want ? parent->orphan_claim_at : parent->orphan_chain_n;

        DCHECK(!parent->orphan || called > 0 || JS_IsUndefined(parent->fn),
               "a drive that has called nothing holds a function — `fn` is written when a call is built");
        sib->orphan = parent->orphan && called > 0;
        sib->orphan_hash = sib->orphan ? parent->orphan_hash : 0;
        if (sib->orphan) flow_orphan_chain_copy(sib, parent, called);
        sib->orphan_claim_at = sib->orphan_chain_n;
    }
    /* A field added to the candidate identity is an obligation here: the fields are copied one by one so that a
       new one is visible, and this checks they arrived. */
    DCHECK(!!sib->cand_src == !!parent->cand_src && !!sib->cand_payload == !!parent->cand_payload &&
           sib->fire.kind == parent->fire.kind && sib->cand_dn == parent->cand_dn &&
           sib->cand_resumed == parent->cand_resumed,
           "an arm of an @S candidate session left the fork without the substitution or its provenance — both are "
           "copied with the arm, which is the payload's flow continued");
    /* The rank that substitution is the denominator of: flow.h's "every non-candidate stands at exactly 0",
       asserted here, where it is transiently false and made true again. flow_fork_inherit runs first and copies
       the parent's ladder position, so between that line and the copy above the sibling holds a distance and no
       payload. It catches a candidate identity that stops being copied while the rank still is, which would carry
       up to a full point of fitness (the comparator's whole range) into ordinary exploration. Written over
       flow_distance rather than a list of rungs, so a rung added to the comparator is covered the day it is
       added. */
    DCHECK(sib->cand_payload != NULL || flow_distance(sib) == 0.0,
           "an arm stands on the @S ladder carrying no payload — the fitness reads bytes this flow injected");
    /* The answer token travels with the queue: the sibling resumes the operation's program and owes the same peer
       an answer of its own (engine_host_answer records the extras). The token and the document asked are on the
       program's row (`dyn_token`, `dyn_doc`, flow.h), so the queue copy below carries them. */
    if (parent->dyn_n) {              /* inherit the lazy chunks loaded up to the branch */
        sib->dyn = reclaim_malloc((size_t)parent->dyn_n * sizeof(DynBody *)); CHECK(sib->dyn, "engine: OOM fork dyn");
        /* The table's columns come with the bodies; a column added to the queue is an obligation at every clone,
           free and finish site. The kinds, so an inherited dead breakout is not re-armed as a page script. */
        sib->dyn_cand = reclaim_malloc((size_t)parent->dyn_n); CHECK(sib->dyn_cand, "engine: OOM fork dyn flags");
        /* Which of §8.1.4.4 Calling scripts' two algorithms runs each, so a module row is not handed to the
           classic entry on one arm. */
        sib->dyn_type = reclaim_malloc((size_t)parent->dyn_n);
        CHECK(sib->dyn_type, "engine: OOM fork dyn script types");
        /* The address each program's bytes came from: §8.1.4.2's created-script base URL and a module row's
           module map key. */
        sib->dyn_url = reclaim_malloc((size_t)parent->dyn_n * sizeof(char *));
        CHECK(sib->dyn_url, "engine: OOM fork dyn script addresses");
        /* The `script` element each row is the program of (§4.12.1.1's "execute the script element" switches on
           el; §3.1.7 `currentScript`). Copied, not duplicated: the element is the same node in the same tree,
           reachable through the DOM base segment the parent's fork froze. */
        sib->dyn_el = reclaim_malloc((size_t)parent->dyn_n * sizeof(lxb_dom_element_t *));
        CHECK(sib->dyn_el, "engine: OOM fork dyn script elements");
        /* The document each row belongs to, so a child navigable's script is not compiled in the session's
           realm. */
        sib->dyn_doc = reclaim_malloc((size_t)parent->dyn_n * sizeof(uint32_t));
        CHECK(sib->dyn_doc, "engine: OOM fork dyn documents");
        /* Each row's name, copied, never re-minted: a register entry outstanding at the fork is shared by both
           arms (solver/pending.h) and names its row, so a fresh name would leave it resolving to nothing in one
           arm. */
        sib->dyn_id = reclaim_malloc((size_t)parent->dyn_n * sizeof(uint64_t));
        CHECK(sib->dyn_id, "engine: OOM fork dyn row names");
        /* When each row became runnable, copied for the name's reason: an inherited row arrived when the
           parent's did, and a fresh stamp would let an arm change its place in the arrival order by being forked.
           The re-mint belongs to readiness, owned by flow_deliver_one_reply. */
        sib->dyn_run = reclaim_malloc((size_t)parent->dyn_n * sizeof(uint64_t));
        CHECK(sib->dyn_run, "engine: OOM fork dyn row arrival stamps");
        /* The rendezvous token of any row still owing an answer: the arm answers the same peer under the same
           token. */
        sib->dyn_token = reclaim_malloc((size_t)parent->dyn_n * sizeof(char *));
        CHECK(sib->dyn_token, "engine: OOM fork dyn tokens");
        /* Whether each row is a task or the synchronous tail of the program that caused it: the arm inherits the
           parent's cursor and owed microtask checkpoint, so an inherited row must keep its order against that
           checkpoint. */
        sib->dyn_pos = reclaim_malloc((size_t)parent->dyn_n);
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
                sib->dyn_url[i] = reclaim_strdup(parent->dyn_url[i]);
                CHECK(sib->dyn_url[i], "engine: OOM forking an external script's address");
            }
            sib->dyn_token[i] = NULL;
            if (parent->dyn_token[i]) {
                sib->dyn_token[i] = reclaim_strdup(parent->dyn_token[i]);
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
    flow_job_fork(ctx, sib, parent);
    /* The array is per flow because the host walks every flow's register from outside any delta and each arm
       removes an entry when it delivers; the records are shared because a record never changes after its push
       except for the answer, which both arms wait on and observe. */
    sib->pending = pending_fork(parent->pending);
    /* The unstarted operations by the same split: a question a peer asked of this document before the branch
       was asked of the arm too, and the arm answers under its own delta (§7.2.1's multiplicity when a document's
       state is its flows). Each arm starts its own copy; the [record, token] entries are shared. */
    sib->perform_q = perform_q_fork(ctx, parent);
    /* The arm now holds every question its parent held, queued or started, without engine_perform attaching it
       there; counted, so the park's holder count is bounded by attaches plus inheritances. Asked after both
       copies (the row tokens above, the queue on the line before). */
    if (flow_owes_answer(sib)) g_perform_inherited++;
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
                DCHECKF(sm == pm, "a fork moved the arm's machine name for host request %u to %u — the name lives in "
                                  "the asking machine's step state inside the cloned frame, so nothing out here may "
                                  "rewrite it", pm, sm);
                DCHECKF(answered || sh != ph,
                        "a fork left an unanswered host request sharing its parent's host name %u — the arm's world "
                        "differs from the parent's from this instant, so one answer would land in two contradictory "
                        "worlds", ph);
            }
            JS_FreeValue(ctx, pp);
            JS_FreeValue(ctx, sp);
        }
    }
#endif
    engine_reclaim_set(prev_reclaim);   /* the sibling is fully assembled: it may be paged like any other member */
    DCHECK(g_genfork_n == 0,
           "a fork finished with generator-state swaps still stashed — the sibling's cloned gen_data was never "
           "recorded on its delta");
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
       only under `!flow_frame(f)`), so the clone is an activation that is not a row's program. JS_FlowResume's
       FLOW_BASE_ASYNC_CALL arm is that clone's home, and consumers ask JS_FlowIsProgram rather than
       `!JS_FlowIsCall`, since `base_kind` has three values.
       Unmade deliveries and unstarted operations are inherited, not refused: engine_route attaches to every
       live flow, including one suspended mid-frame, and the arm is that timeline continued, so it holds the
       message and the question too and answers under its own delta (engine_host_answer records the extra
       answers under one token). A started operation's token travels too: a branch inside the answering program
       is a real peer timeline. */
    dec = g_fork_dec; g_fork_dec = NULL;
    pins = g_fork_pins; g_fork_pins = NULL;
    engine_sibling_assemble(ctx, parent, clone, dec, pins, g_fork_marks);
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
        JSValue e = pending_entry(f->pending, i), av = JS_UNDEFINED, aw = JS_UNDEFINED, am = JS_UNDEFINED, se;
        JSValue *clone;
        void *parked;
        int in_program, in_job;
        Flow *sib;
        int completion;

        if (pending_extra_count(e) == 0) { JS_FreeValue(ctx, e); continue; }
        DCHECK(flow_running() == f, "an answer fork was taken while another flow was switched in — the arm would "
                                    "clone that flow's delta and DOM head");
        /* Where this flow's suspension is, asked of both homes. A flow inside a program holds its call site in
           `frame`; a flow inside a job holds no frame (flow_step runs a job only under `!flow_frame(f)`) and its call
           site is the parked continuation of the activation the job entered, as for a `.then` handler making a
           cross-instance read. The park rides the Flow while switched out and sits in the runtime's queue while
           switched in, which this is, so the runtime answers and `f->parked` must be empty. */
        in_program = flow_frame(f) != NULL;
        in_job = JS_HasParkedFlow(JS_GetRuntime(ctx));
        DCHECK(f->parked == NULL,
               "a flow holds parked continuations on its own slot while switched in — the switch hands the set to "
               "the runtime and clears the slot");
        DCHECK(in_program || in_job,
               "a flow holding a peer's second answer is suspended in neither home (no program frame, no parked "
               "continuation) — the call site the arm would resume is gone");
        /* And not both: a job runs only while the flow holds no frame, and flow_step's resume arm drains a parked
           continuation before any program is compiled. If both ever hold, the arm needs both cloned, in the order
           flow_step resumes them. */
        DCHECK(!(in_program && in_job),
               "a flow is suspended in both homes at once (a program frame and a parked continuation) — clone both "
               "onto the arm, in the order flow_step resumes them");
        /* A routed delivery is not refused: engine_sibling_assemble forks the delivery queue, so the arm, this
           timeline continued, has its own copy. */
        /* Taken from the parent first, so the parent cannot fork over this answer again; the arm's inherited
           copy of the list is cleared below. */
        completion = pending_extra_pop(e, &av, &aw, &am);
        JS_FreeValue(ctx, e);

        /* The arm is a clone of whichever home holds the suspension (the asserts above established exactly one).
           Each is a CHECK: a clone this engine cannot build is a peer timeline dropped off the frontier. */
        clone = in_program ? JS_FlowClone(ctx, (JSValue *)flow_frame(f)) : NULL;
        parked = in_job ? JS_CloneParkedFlows(ctx) : NULL;
        CHECK(!in_program || clone != NULL,
              "engine: a flow suspended on a cross-instance read could not be cloned — the peer's other "
              "timeline has an answer and no arm to carry it");
        CHECK(!in_job || parked != NULL,
              "engine: a flow parked on a cross-instance read inside a job could not have its continuation cloned");
        sib = engine_sibling_assemble(ctx, f, clone,
                                      decide_fork_same_path("(a peer's answer arrived — no predicate was "
                                                            "asked)"),
                                      concolic_pins_suspend(), flow_path_marks(f));
        /* The arm's own continuation goes on the slot a switched-out flow keeps it in: its first switch-in hands
           it to the pump, and flow_step's resume arm re-enters it before anything else, where it reads the answer
           written below. The assembly takes a frame but not a park; a third caller needing a park would be the
           reason to give it that parameter (orphan drives and branch arms carry none). */
        DCHECK(sib->parked == NULL,
               "a freshly assembled arm already holds parked continuations — the assembly builds a flow that "
               "has never run, so a set on it came from somewhere that is not this fork and the write below "
               "would drop it");
        sib->parked = parked;
        /* The arm took this timeline, so this flow forecloses it. The standby this entry leaves inherits the row,
           and the row crosses the cold tier, so neither refuses less after a park. Pushed after the assembly, so
           the arm's inherited record does not foreclose its own answer. */
        {
            const char *w = JS_ToCString(ctx, aw);

            CHECK(w != NULL, "engine: OOM recording the peer timeline an answer arm takes");
            flow_world_commit_push(ctx, f, w, 0, FLOW_COMMIT_ARM_ANSWER_TAKEN);
            JS_FreeCString(ctx, w);
        }
        /* The arm's own answer, the one record it cannot share: pending_fork shares an answered entry (an answer
           from before the fork was computed in a world both arms were in), but here the arms hold different
           answers to one question. */
        se = pending_unshare(sib->pending, i);
        pending_set(se, PEND_VALUE, av);
        pending_set_int(se, PEND_COMPLETION, completion);
        /* Whose answer it is, from the same triple: the arm belongs to a named pair of timelines, and its next
           operation is addressed with this (engine_flow_addressee). */
        pending_set(se, PEND_ANSWER_WORLD, aw);
        pending_set(se, PEND_ANSWER_MARKS, am);
        /* No answers beyond its own, both ways: the inherited remaining list would make the arm fork over answers
           the parent will fork over, and its answer is fixed, so a later answer belongs to the issuer. Together
           these make a peer with N timelines cost exactly N-1 arms. */
        pending_set(se, PEND_EXTRA, JS_NULL);
        pending_set(se, PEND_ANSWER_FIXED, JS_TRUE);
        DCHECK(pending_get_int(se, PEND_HAVE_VALUE) != 0,
               "an arm was forked over a peer's answer onto a request still unanswered — the assembly re-issues "
               "unanswered requests under a fresh id");
        JS_FreeValue(ctx, se);
        return 1;
    }
    return 0;
}

/* The standby a peer answer leaves behind: before a flow resumes on an answer one of a peer's timelines computed,
 * a sibling is forked at the same call site holding the request unanswered, so a later timeline's answer is its own
 * delivery to a timeline still at the read, as a routed post is delivered to every timeline able to receive it.
 * The standby takes that answer and leaves the next standby in turn; engine_host_exhausted retires the last one when
 * the peer says no timeline will answer again. Runs where flow_answer_fork does, after it, one per step.
 * What the family took is the standby's commitment record: it forecloses its parent's world here and inherits a
 * foreclosed row for each arm's (flow_answer_fork), and engine_host_answer admits an answer by that record, as
 * deliver_admits admits a routed message. The rows cross the cold tier, so a resumed member re-asking a peer still
 * in its session is refused the timelines its family took.
 * Named residual. Not covered: a peer whose session ended too. Its new timelines are independent of every carried
 * row, so each resumed member re-forks over all of them. Next diff builds: the meaning for `sent` across a
 * generation that flow_answer_fork's residual names, so a carried row relates to the timeline's continuation.
 * Absence shows as: after a park and resume of both instances, arms over one read numbering the resumed family's
 * members times the peer's timelines. */
static int flow_answer_standby(JSContext *ctx, Flow *f) {
    int n = pending_count(f->pending), i;

    for (i = 0; i < n; i++) {
        JSValue e = pending_entry(f->pending, i), wv, se;
        JSValue *clone;
        void *parked;
        int in_program, in_job;
        const char *w;
        Flow *sib;

        if (pending_get_int(e, PEND_KIND) != FLOW_PENDING_HOSTREQ || !pending_get_int(e, PEND_HAVE_VALUE) ||
            pending_get_int(e, PEND_ANSWER_FIXED) || pending_get_int(e, PEND_STANDBY_LEFT) ||
            pending_get_int(e, PEND_ANSWER_EXHAUSTED)) { JS_FreeValue(ctx, e); continue; }
        wv = pending_get(e, PEND_ANSWER_WORLD);
        if (!JS_IsString(wv)) { JS_FreeValue(ctx, wv); JS_FreeValue(ctx, e); continue; }   /* a zone answer */
        DCHECK(pending_extra_count(e) == 0,
               "a standby was about to be left while answers beyond the first wait for arms — flow_answer_fork runs "
               "first in flow_step, so the arms and the standby would split one timeline between them");
        DCHECK(flow_running() == f && f->parked == NULL,
               "a standby was forked with another flow switched in, or with parked continuations off the runtime");
        in_program = flow_frame(f) != NULL;
        in_job = JS_HasParkedFlow(JS_GetRuntime(ctx));
        DCHECK(in_program != in_job,
               "a flow holding a peer's answer is suspended in neither home or in both — the standby clones the one "
               "call site the answer is read at");
        w = JS_ToCString(ctx, wv);
        CHECK(w != NULL, "engine: OOM reading the peer timeline a standby's parent takes");
        clone = in_program ? JS_FlowClone(ctx, (JSValue *)flow_frame(f)) : NULL;
        parked = in_job ? JS_CloneParkedFlows(ctx) : NULL;
        CHECK(!in_program || clone != NULL,
              "engine: a flow at a cross-instance read could not be cloned — a later peer timeline would have "
              "no timeline to answer");
        CHECK(!in_job || parked != NULL,
              "engine: a flow parked on a cross-instance read inside a job could not have its continuation cloned");
        sib = engine_sibling_assemble(ctx, f, clone,
                                      decide_fork_same_path("(a peer's answer is taken; its later timelines wait "
                                                            "— no predicate was asked)"),
                                      concolic_pins_suspend(), flow_path_marks(f));
        DCHECK(sib->parked == NULL, "a freshly assembled standby already holds parked continuations");
        sib->parked = parked;
        se = pending_answer_standby(sib->pending, i);
        JS_FreeValue(ctx, se);
        /* The parent takes `w`, so the standby is the far side of it. */
        flow_world_commit_push(ctx, sib, w, 0, FLOW_COMMIT_ARM_ANSWER_TAKEN);
        /* It waits for the zone, as its parent did before the first answer. */
        flow_set_host_owed(sib);
        pending_set(e, PEND_STANDBY_LEFT, JS_TRUE);
        g_answer_standbys++;
        JS_FreeCString(ctx, w);
        JS_FreeValue(ctx, wv);
        JS_FreeValue(ctx, e);
        return 1;
    }
    return 0;
}

/* Whether this flow waits on a peer request the peer has exhausted: no timeline will answer it, so the timeline
   this flow would continue does not exist. */
static int flow_answer_exhausted(const Flow *f) {
    int n = pending_count(f->pending), i, hit = 0;

    for (i = 0; i < n && !hit; i++) {
        JSValue e = pending_entry(f->pending, i);

        hit = pending_get_int(e, PEND_KIND) == FLOW_PENDING_HOSTREQ && !pending_get_int(e, PEND_HAVE_VALUE) &&
              pending_get_int(e, PEND_ANSWER_EXHAUSTED);
        JS_FreeValue(pending_ctx(), e);
    }
    return hit;
}

/* The success arm of a `fetch()` whose reply no observation gave: declined by the zone, or answered with a status
 * that is not ok. The server decides the status, headers and body (for the second, under credentials or state
 * this visitor lacks), so where the domain permits the success path that path runs, on a reply whose body is
 * unknown (fetch_reply_unknown). The arm's copy of record `i` is answered with that reply and marked `taken`; the
 * arm is a sibling assembled from its own clone of the flow, or `f` itself when its recorded path chose it, and
 * the return says which (1: `f` took the success). Its path is marked forced when the reply is delivered
 * (flow_deliver_one_reply), because a sibling is born with no mark its parent lacks. A `same-origin` request to
 * another origin has no success to explore. `cross` is this flow's answer to §4.1 step 12's same-origin test for
 * an unknown address (flow_unaddressed_same_origin): 1 another origin, whose server the address's author chose,
 * and 0 this one, whose bytes stay owed to a grounded address and get no arm here; -1 for an address of bytes.
 * Named residual. Not covered: an XMLHttpRequest's declined send, whose answer is a host request's
 * (FLOW_PENDING_HOSTREQ) and has no reply record to be unknown. Next diff builds: the same arm through
 * engine_host_answer's record. How its absence shows: an address built from `xhr.response` behind a declined
 * send is reached on no flow while the same address behind a declined `fetch()` is. */
static int flow_success_arm(JSContext *ctx, Flow *f, int i, int in_program, int in_job, const char *site,
                            int taken, int cross) {
    JSValue e = pending_entry(f->pending, i), uv, mv;
    const char *u, *m;
    JSValue reply, se;
    JSValue *clone;
    void *parked, *dblob;
    Flow *sib, *arm_flow;
    int arm, unaddressed = pending_get_int(e, PEND_ADDRESS) == FETCH_ADDRESS_UNKNOWN;

    DCHECK(unaddressed ? (cross == 0 || cross == 1) : cross == -1,
           "a success arm was asked with a same-origin answer that does not match its address — the test is "
           "asked over an unknown address only, and every unknown address has one");
    if (cross == 0) {
        JS_FreeValue(ctx, e);
        return 0;
    }
    uv = pending_get(e, PEND_URL);
    mv = pending_get(e, PEND_MODE);
    u = JS_ToCString(ctx, uv);
    m = JS_ToCString(ctx, mv);
    CHECK(u != NULL && m != NULL, "engine: OOM reading a declined fetch's address and mode");
    if (unaddressed) {
        JSValue av = pending_get(e, PEND_ADDRESS_VALUE);

        reply = fetch_reply_unaddressed(ctx, u, m, av);
        JS_FreeValue(ctx, av);
    } else {
        reply = fetch_reply_unknown(ctx, u, m);
    }
    JS_FreeCString(ctx, u);
    JS_FreeCString(ctx, m);
    JS_FreeValue(ctx, uv);
    JS_FreeValue(ctx, mv);
    JS_FreeValue(ctx, e);
    if (JS_IsUninitialized(reply))
        return 0;
    /* The success arm is completion 1 of a recorded two-armed choice, so a replay that recorded it makes this
       flow the success arm, and one that recorded 0 makes it the other; only a new choice mints the sibling. */
    arm = decide_completion(site, &dblob);
    if (arm == 1) {
        arm_flow = f;
    } else if (dblob != NULL) {
        clone = in_program ? JS_FlowClone(ctx, (JSValue *)flow_frame(f)) : NULL;
        parked = in_job ? JS_CloneParkedFlows(ctx) : NULL;
        CHECK(!in_program || clone != NULL,
              "engine: a flow parked on a declined fetch could not be cloned for the arm that explores its success");
        CHECK(!in_job || parked != NULL,
              "engine: a flow parked on a declined fetch inside a job could not have its continuation cloned");
        sib = engine_sibling_assemble(ctx, f, clone, dblob, concolic_pins_suspend(), flow_path_marks(f));
        DCHECK(sib->parked == NULL, "a freshly assembled arm already holds parked continuations");
        sib->parked = parked;
        arm_flow = sib;
    } else {
        JS_FreeValue(ctx, reply);
        return 0;
    }
    se = pending_unshare(arm_flow->pending, i);
    pending_set_int(se, taken, 1);
    pending_set(se, PEND_VALUE, reply);
    pending_set_int(se, PEND_HAVE_VALUE, 1);
    JS_FreeValue(ctx, se);
    return arm_flow == f;
}

/* A `fetch()` answered with a status that is not ok (Fetch §2.2.3 Statuses: "An ok status is a status in the range
 * 200 to 299, inclusive"), a 401 to a logged-out visitor being the population this exists for: the observation is
 * this visitor's, and the success path behind it is the surface another visitor reaches, so it gets the same arm as
 * a declined request (flow_success_arm). Asked before the reply is delivered, so the arm clones the flow still
 * waiting on it; this flow keeps the observed reply unless its recorded path chose the success arm. A reply no
 * server sent is never asked again. One per step. */
static int flow_reply_success_fork(JSContext *ctx, Flow *f) {
    int n = pending_count(f->pending), i;

    for (i = 0; i < n; i++) {
        JSValue e = pending_entry(f->pending, i), v, pe;
        int in_program, in_job, ask;

        ask = pending_get_int(e, PEND_KIND) == FLOW_PENDING_RESOLVE && pending_get_int(e, PEND_HAVE_VALUE) &&
              !pending_get_int(e, PEND_SUCCESS_TAKEN) && !pending_get_int(e, PEND_DECLINE_TAKEN);
        v = ask ? pending_get(e, PEND_VALUE) : JS_UNDEFINED;
        ask = ask && JS_IsObject(v) && !fetch_reply_unobserved(ctx, v) &&
              fetch_reply_type(ctx, v) != FETCH_REPLY_OPAQUE && !script_fetch_status_ok(fetch_reply_status(ctx, v));
        JS_FreeValue(ctx, v);
        JS_FreeValue(ctx, e);
        if (!ask) continue;
        DCHECK(flow_running() == f && f->parked == NULL,
               "a success arm was forked with another flow switched in, or with parked continuations off the "
               "runtime");
        in_program = flow_frame(f) != NULL;
        in_job = JS_HasParkedFlow(JS_GetRuntime(ctx));
        /* flow_decline_fork's three refusals, for its reasons: two homes at once, and an atomic operation. */
        if (f->compile != NULL || (in_program && in_job) ||
            (in_program && JS_FlowUnforkable(ctx, (JSValue *)flow_frame(f))) ||
            (in_job && JS_ParkedFlowsUnforkable(ctx)))
            continue;
        flow_success_arm(ctx, f, i, in_program, in_job,
                         "(a reply's status was not ok — the arm takes a reply no server sent)", PEND_SUCCESS_TAKEN,
                         /*cross*/ -1);
        /* This flow's own copy, so the register's arm is never forked twice, whichever arm this flow took. */
        pe = pending_unshare(f->pending, i);
        pending_set_int(pe, PEND_SUCCESS_TAKEN, 1);
        JS_FreeValue(ctx, pe);
        return 1;
    }
    return 0;
}

/* Fetch §4.1 Main fetch step 12's same-origin test for the declined request `e` to an unknown address, asked by
 * `f` with its continuation cloned onto the sibling (`in_program`/`in_job` as flow_decline_fork read them). The
 * test is the predicate pending_park_request stored, the one a page's own `new URL(addr, base).origin` comparison
 * mints, so a page that checked first has decided it and the ask replays that arm. A page that checked some
 * other way (`addr.startsWith('/')`) asked a different question, and both answers stay: `//host/x` passes that
 * guard and is another origin. The sibling takes the other answer and re-asks at its first step, where its
 * recorded arm replays. A session that does not fork takes this origin, whose arms take no reply no server sent.
 * Returns 1 for same origin. */
static int flow_unaddressed_same_origin(JSContext *ctx, Flow *f, JSValueConst e, int in_program, int in_job) {
    JSValue cond = pending_get(e, PEND_SAME_ORIGIN);
    JSValue *clone;
    void *parked, *dec, *pins;
    Flow *sib;
    int r;

    if (JS_IsBool(cond))
        return JS_ToBool(ctx, cond);
    DCHECK(concolic_is(cond), "an unknown address's park stored a same-origin test that is neither a Boolean nor "
                              "an unknown one — pending_park_request writes fetch_main_same_origin_unknown's "
                              "answer on every such record");
    DCHECK(!g_fork_snapshot_owed, "the same-origin ask was made while another snapshot-owning ask was open");
    g_fork_snapshot_owed = 1;
    r = solver_decide(ctx, cond, /*nonforking*/ 1);
    g_fork_snapshot_owed = 0;
    JS_FreeValue(ctx, cond);
    DCHECK(r >= 0, "the same-origin ask over an unknown found no running flow's decision state");
    if (SOLVER_FORKED(r)) {
        clone = in_program ? JS_FlowClone(ctx, (JSValue *)flow_frame(f)) : NULL;
        parked = in_job ? JS_CloneParkedFlows(ctx) : NULL;
        CHECK(!in_program || clone != NULL,
              "engine: a flow parked on an unaddressed request could not be cloned for the other origin's arm");
        CHECK(!in_job || parked != NULL,
              "engine: a flow parked on an unaddressed request inside a job could not have its continuation "
              "cloned");
        dec = g_fork_dec; g_fork_dec = NULL;
        pins = g_fork_pins; g_fork_pins = NULL;
        sib = engine_sibling_assemble(ctx, f, clone, dec, pins, g_fork_marks);
        DCHECK(sib->parked == NULL, "a freshly assembled arm already holds parked continuations");
        sib->parked = parked;
    }
    return SOLVER_ARM(r);
}

/* One arm per declined request: the fork a refusal owes. The zone grades each refusal on one axis: would a real
 * browser making this request also get Fetch §5.6 Fetch methods' network error? Where it would (a blocked scheme,
 * a CORS failure, a CORB-blocked body) the reply arrives as that error. Where it would not (the firing policy at an
 * unwidened origin, the destructive deny list, a method the chokepoint cannot issue) the outcome is unobserved and
 * every arm runs, each a completion asked in this order (decide_completion), so each parks on its own recipe:
 *   - the waiting arm keeps the unanswered record and stays parked at the line that asked, a parked search that
 *     fires the day the origin is widened. It holds no reply, status or example.
 *   - for a `fetch()`, the success arm (flow_success_arm): a reply no server sent, graded forced.
 *   - the failure arm takes §5.6's network error (JS_NULL) and runs the page's `catch`, where bundles keep fallback
 *     endpoints, retry hosts and degraded-mode configuration. Its path is marked forced in the same operation
 *     (flow_mark_forced_arm), so nothing it builds is graded derived; the other arms stand on the path before it.
 * It runs where flow_answer_fork does, one decline per step. Every kind takes the same JS_NULL, and
 * flow_deliver_one_reply turns it into each kind's own failure (a rejected `fetch()`, §4.12.1.1's null result,
 * HostLoadImportedModule's TypeError, a `<link>`'s error branch), so this holds no per-kind knowledge of the
 * outcome beyond the synchronous kind's completion below. */
static int flow_decline_fork(JSContext *ctx, Flow *f) {
    int n = pending_count(f->pending), i;

    for (i = 0; i < n; i++) {
        JSValue e = pending_entry(f->pending, i), se, pe, reason;
        JSValue *clone;
        void *parked, *dblob;
        int in_program, in_job, kind, cross;
        Flow *sib, *arm_flow;

        if (!pending_entry_declined(e) || pending_get_int(e, PEND_DECLINE_TAKEN)) {
            JS_FreeValue(ctx, e);
            continue;
        }
        DCHECK(flow_running() == f,
               "a decline fork was taken while another flow was switched in — the arm would clone that flow's delta "
               "and DOM head");
        /* A refusal and a reply cannot both be true of one request: `haveValue` says the server answered,
           `declined` that nobody asked it. */
        DCHECK(!pending_get_int(e, PEND_HAVE_VALUE),
               "a request carries both a reply and a refusal — engine_decline refuses only records the host still "
               "owes");
        kind = (int)pending_get_int(e, PEND_KIND);
        reason = pending_get(e, PEND_DECLINED);
        /* A synchronous request can be here: engine_host_decline reaches it by rendezvous id, since it has no
           address for `pending_index_find`. The failure side is kind-aware in exactly two writes below: the
           completion engine_host_take requires (type and value are one write), and `pending_answer_rendezvous` instead
           of a bare `haveValue`, since the register carries `pending_blocked`'s count (solver/pending.h). For a
           §7.4 navigation the waiting arm holds the record with no value, so `engine_host_answered` keeps
           answering no, NAV_LOAD_CREATE never runs, and the navigable keeps the initial `about:blank` §7.3.1.3
           Child navigables created; the failure arm gets the error page a browser shows. Both names are asserted
           because a fork moves one of them. */
        DCHECK(kind != FLOW_PENDING_HOSTREQ ||
                   (pending_get_int(e, PEND_REQ) != 0 && pending_get_int(e, PEND_REQ_HOST) != 0),
               "a refused synchronous host request carries no machine or host rendezvous id — engine_host_decline "
               "found the record by those ids, and an arm needs both to be polled and shown");
        /* Both homes of the suspension are asked and neither is required, unlike flow_answer_fork: a flow waiting
           on a reply usually holds neither, since a `fetch()` hands back a pending promise and the program runs
           on, leaving a flow between tasks whose continuation is the reaction the delivery will enqueue.
           engine_sibling_assemble takes that shape, and a frameless arm resumes its scheduler step. An injected
           `<script src>` parks inside the inserting program (a frame); a document's external script parks at
           its row (no frame). Both at once is refused, since the arm would carry one suspension and abandon the
           other. */
        in_program = flow_frame(f) != NULL;
        in_job = JS_HasParkedFlow(JS_GetRuntime(ctx));
        DCHECK(f->parked == NULL,
               "a flow holds parked continuations on its own slot while switched in — the switch hands the set to "
               "the runtime and clears the slot");
        DCHECK(!(in_program && in_job),
               "a flow is suspended in both homes at once (a program frame and a parked continuation) — clone both "
               "onto the arm, in the order flow_step resumes them");
        /* A step machine inside an operation the spec makes atomic (HTML §8.5.4's fragment parsing and
           placement, §14.4's XML fragment parse) answers why it cannot be forked yet, and this fork waits for a
           later pick. No page code runs inside such an operation, and this request's outcome reaches the page
           only through a job or a parked call site that resumes after it, so forking at the operation's end
           is the same pair of arms. A parse parked mid-row (`f->compile`) is the same case: a parse runs no page
           code, and the parked parse is a third home of the suspension no arm could clone. The answer fork
           cannot wait: its arm resumes the call site this one leaves. */
        if (f->compile != NULL ||
            (in_program && JS_FlowUnforkable(ctx, (JSValue *)flow_frame(f))) ||
            (in_job && JS_ParkedFlowsUnforkable(ctx))) {
            JS_FreeValue(ctx, reason);
            JS_FreeValue(ctx, e);
            continue;
        }
        /* An unknown address's origin first: it decides whether the success arm below exists. */
        cross = pending_get_int(e, PEND_ADDRESS) != FETCH_ADDRESS_UNKNOWN ? -1
              : !flow_unaddressed_same_origin(ctx, f, e, in_program, in_job);
        /* The waiting arm is completion 1 of a recorded choice, asked first; a flow whose recorded path chose
           it is that arm, and only a new choice mints it as a sibling. */
        if (decide_completion("(the trusted zone declined this request — the arm waits for the real reply)",
                              &dblob) == 1) {
            arm_flow = f;
        } else if (dblob != NULL) {
            clone = in_program ? JS_FlowClone(ctx, (JSValue *)flow_frame(f)) : NULL;
            parked = in_job ? JS_CloneParkedFlows(ctx) : NULL;
            CHECK(!in_program || clone != NULL,
                  "engine: a flow parked on a declined request could not be cloned — the waiting arm is what makes "
                  "a per-origin widening possible");
            CHECK(!in_job || parked != NULL,
                  "engine: a flow parked on a declined request inside a job could not have its continuation "
                  "cloned");
            sib = engine_sibling_assemble(ctx, f, clone, dblob, concolic_pins_suspend(), flow_path_marks(f));
            DCHECK(sib->parked == NULL,
                   "a freshly assembled arm already holds parked continuations — the assembly builds a flow that "
                   "has never run, so a set on it came from somewhere that is not this fork");
            sib->parked = parked;
            arm_flow = sib;
        } else {
            arm_flow = NULL;
        }
        if (arm_flow != NULL) {
            /* The waiting arm's own copy of the record, marked as having had its fork. The record is shared by
               pending_fork, so both sides take a private copy; otherwise the first flow to step would consume a
               decline every parked timeline is owed. The copy leaves the outstanding set (no reply is coming)
               and keeps the reason, the account of a park that will not drain. */
            se = pending_unshare(arm_flow->pending, i);
            pending_set_int(se, PEND_DECLINE_TAKEN, 1);
            DCHECK(!pending_get_int(se, PEND_HAVE_VALUE) && pending_entry_declined(se),
                   "the waiting arm of a decline came out of the unshare holding an answer, or holding no refusal "
                   "— it is the arm that goes on waiting for the real one, so an answered copy would resume it "
                   "over a reply nobody sent and an unrefused copy would be re-listed to the host and re-declined");
            JS_FreeValue(ctx, se);
        }
        if (arm_flow == f ||
            (kind == FLOW_PENDING_RESOLVE &&
             flow_success_arm(ctx, f, i, in_program, in_job,
                              "(the trusted zone declined this request — the arm takes a reply no server sent)",
                              PEND_DECLINE_TAKEN, cross))) {
            JS_FreeValue(ctx, reason);
            JS_FreeValue(ctx, e);
            return 1;
        }
        /* This flow's copy, answered with §5.6's network error (JS_NULL, core/fetch/fetch.h). The delivery owns
           what each kind does with it; this states the outcome. */
        pe = pending_unshare(f->pending, i);
        pending_set_int(pe, PEND_DECLINE_TAKEN, 1);
        pending_set(pe, PEND_VALUE, JS_NULL);
        if (kind == FLOW_PENDING_HOSTREQ) {
            /* For this kind the completion and value are one write (engine_host_take asserts it). The network
               error is a normal completion carrying null, never a throw, which would re-raise at the parked call
               site and make a declined navigation look like a document that threw. And through the register
               (pending_answer_rendezvous), since `haveValue` here is the write that unblocks the flow.
               A cross-agent operation has no network error: HTML §7.2.1.3.4 CrossOriginGetOwnPropertyHelper's
               getter, §7.3.1.6's close and a lent object's internal methods complete in the agent holding the
               object. Its answer is unknown because nobody asked, so it is DECLINED and the asking machine
               builds an absent value of the member's type (engine_host_declined). */
            JSValue ov = pending_get(e, PEND_OP);
            const char *op = JS_ToCString(ctx, ov);
            int is_op;

            DCHECK(JS_IsString(ov), "a synchronous request was parked with no operation text — engine_host_request "
                                    "writes it at the push");
            CHECK(op != NULL, "engine: a refused synchronous request's operation text could not be read");
            is_op = remote_op_names_operation(op);
            JS_FreeCString(ctx, op);
            JS_FreeValue(ctx, ov);
            if (is_op) pending_set(pe, PEND_VALUE, JS_UNDEFINED);
            pending_set(pe, PEND_COMPLETION,
                        JS_NewInt32(ctx, is_op ? ENGINE_COMPLETION_DECLINED : ENGINE_COMPLETION_NORMAL));
        }
        reply_arrived(f, pe);
        JS_FreeValue(ctx, pe);
        /* What this path is evidence of from now: everything after stands on an unobserved outcome, so every
           request it builds is forced (pending_prov_compose reads the mark at the park) and every endpoint a
           forced sighting endpoint.c keeps out of the derived pool. Marked after the assembly, so the waiting arm
           inherits the path from before the refusal. */
        flow_mark_forced_arm();
        DCHECK(flow_path_forced(f),
               "the failure arm of a decline stands on an unobserved outcome and its path is not marked forced — its "
               "requests would be graded derived");
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
           "the orphan take handed over a second function in one call — each becomes a flow, so the seeding step "
           "must yield between them");
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
           "the orphan census was asked for one of its two numbers — the drive and ask counts are returned together");
    *driven = g_orphans_driven; *asked = g_orphan_asks;
}

/* The arms of the orphan rungs' gate (engine.h's EngineOrphanGateCensus), counted per ask. */
enum { GATE_NONE, GATE_PROGRAM, GATE_TIMER, GATE_REACTION, GATE_UNINSPECTED, GATE_ARMS };
static long g_gate_asks, g_gate_arm[GATE_ARMS];

/* Can outstanding work still run this flow's page code? A drive claims that nothing called its body, and a call
 * built while something can still call it puts the drive's questions and that work's in an order a replay need
 * not reproduce. Asked at the moment of the ask, in one place, for both orphan rungs. A program row or a program
 * reply always can; so can an active timer; a fetch() reply can exactly when its promise holds a reaction, since
 * without one nothing holds the Response or can read its body. `seq_awaits` is the ladder's own row fact.
 * Named residual. Not covered: a reply that has a reaction and never settles (a long poll) blocks drives for as
 * long as it is owed. Next diff builds: the drive and the reply's arrival as two orders, as security-cors's
 * deadline race does, since the reply's arrival is unknown. How its absence would show: `orphanGate.reaction`
 * climbing with `_orphansAsked` flat on a page that holds one request open.
 * Named residual. Not covered: a resource, XHR, module or async rendezvous reply counts as able to run page
 * code, because whether its object has a listener is not inspected. Next diff builds: the listener test on the delivery's target. How
 * its absence would show: `orphanGate.uninspected` nonzero on a run whose drives stay at zero. */
static int engine_outstanding_runs_page_code(JSContext *ctx, Flow *f, int seq_awaits) {
    int arm = GATE_NONE, n, i;

    g_gate_asks++;
    if (seq_awaits) arm = GATE_PROGRAM;
    else if (g_timer_active_hook && g_timer_active_hook(ctx) > 0) arm = GATE_TIMER;
    n = arm == GATE_NONE ? pending_count(f->pending) : 0;
    for (i = 0; i < n && arm == GATE_NONE; i++) {
        JSValue e = pending_entry(f->pending, i);
        int kind = (int)pending_get_int(e, PEND_KIND);

        if (pending_entry_owed(e)) {
            DCHECK(kind != FLOW_PENDING_HOSTREQ || pending_get_int(e, PEND_ASYNC),
                   "the orphan gate met an unanswered blocking rendezvous — such a flow is blocked and the ladder "
                   "returns above both orphan rungs");
            if (kind == FLOW_PENDING_SCRIPT || kind == FLOW_PENDING_DOCSCRIPT) {
                arm = GATE_PROGRAM;
            } else if (kind == FLOW_PENDING_RESOLVE) {
                JSValue d = pending_get(e, PEND_RESOLVE);

                if (!fetch_is_delivery(ctx, d)) arm = GATE_UNINSPECTED;
                else if (fetch_delivery_has_reaction(ctx, d)) arm = GATE_REACTION;
                JS_FreeValue(ctx, d);
            } else {
                arm = GATE_UNINSPECTED;
            }
        }
        JS_FreeValue(ctx, e);
    }
    g_gate_arm[arm]++;
    return arm;
}

EngineOrphanGateCensus engine_orphan_gate_census(void) {
    EngineOrphanGateCensus c = { g_gate_asks, g_gate_arm[GATE_NONE], g_gate_arm[GATE_PROGRAM],
                                 g_gate_arm[GATE_TIMER], g_gate_arm[GATE_REACTION], g_gate_arm[GATE_UNINSPECTED] };

    DCHECK(c.none + c.program + c.timer + c.reaction + c.uninspected == c.asks,
           "the orphan gate's arms do not account for its asks — each ask is counted at entry and its deciding "
           "arm once at exit, so a difference is an exit that skipped the arm count");
    return c;
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

/* The generation at which this document last walked the heap empty, and the world that walked it. Creating a
   function object is the only event that can add to the orphan set (JS_OrphanGen), so a walk at an unchanged
   generation can only find what the previous one took; the take answers per timeline, so the memo holds for the
   world that walked and no other, and a fork gives both arms new worlds. A walk is O(live objects). It is not a
   memo about any orphan, and its only error (a 2^32 wrap) costs one redundant walk. */
static uint32_t g_orphan_gen_seen;
static WorldId  g_orphan_gen_world;
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
   third, how many were rebuilt, is the cold tier's (cold_resumed().orphans), and it bounds both, because a claim
   is held by the one flow its record rebuilt (a fork does not copy it) and is satisfied at most once. The unmet
   count is counted at the finish: a drive that finished still waiting is the loss exactly (the bundle no longer
   holds the body, or the locator does not name it), and on unchanged bytes between two sessions it is zero. */
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
        if (fl->orphan_want && JS_IsUndefined(fl->orphan_claim_fn)) return 1;
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
        if (!fl->orphan_want || !JS_IsUndefined(fl->orphan_claim_fn)) continue;
        open++;
        /* The claim is for the next member of the chain only: the calls are built in the order they were seeded. */
        DCHECK(fl->orphan_claim_at < fl->orphan_chain_n,
               "a flow holds an open orphan claim past the end of its chain of drives — `orphan_want` clears when "
               "the last member's call is built");
        if (fl->orphan_chain[fl->orphan_claim_at] != hash) continue;
        /* A claimant in another timeline than the one that built this closure cannot reach it; a compile of the
           same bytes in the claimant's own timeline is the body it waits for. */
        if (!engine_world_reaches_fn(fl->world, fn)) continue;
        DCHECK(fl->orphan,
               "a flow waits for an orphan's body without being marked as a drive — the mark, the locator and the "
               "wait are written together by the cold tier's rebuild");
        /* Only the function crosses here; the claimant builds its call later in its own timeline, because the
           receiver and arguments are concolic objects that, minted under this flow's stamp, would be this flow's
           private state, outside every delta. */
        fl->orphan_claim_fn = JS_DupValue(ctx, fn);
        fl->orphan_argc = argc;
        n++;
    }
    /* The latch, set by the walk itself. `open` is counted before this pass satisfies anything, so a pass that
       satisfies the last claim latches on the next one. */
    if (!open) g_orphan_claims_closed = 1;
    g_orphan_claims_met += n;
    {
        ColdResumed cr;

        cold_resumed(&cr);
        DCHECK(g_orphan_claims_met <= cr.orphans,
               "more orphan claims were satisfied than the cold tier rebuilt — a claim belongs to the one flow its "
               "'o' record named, so a second holder is a fork that copied the wait (engine_sibling_assemble)");
    }
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
/* The position engine_orphan_call names chain member `k`'s unknowns by: `k` where an earlier member is the same
   body (a listener driven again), and 0 for a body's first call, whose names predate re-drives. */
static int orphan_chain_position(const Flow *f, int k) {
    for (int j = 0; j < k; j++) if (f->orphan_chain[j] == f->orphan_chain[k]) return k;
    return 0;
}

/* The sender every driven message is posted from: the attacker's one document, so a drive's chain is a
   one-window PoC and all its messages read one origin source (window_message_delivery_sources). */
#define ENGINE_ATTACKER_SENDER "attacker-document"

/* The delivery record a driven message listener's call at chain position `pos` names its event after. */
static void engine_attacker_post_record(uint64_t hash, int pos, char *out, size_t cap) {
    int n = snprintf(out, cap, "attacker-post:%016llx#%d", (unsigned long long)hash, pos);

    CHECK(n > 0 && (size_t)n < cap, "engine: an attacker post's record name did not fit its room");
}

void engine_orphan_message_sources(const Flow *f, int k, char *data, char *origin, size_t cap) {
    char record[64];

    DCHECK(f != NULL && f->orphan && k >= 0 && k < f->orphan_chain_n && f->orphan_acts[k] &&
           strstr(f->orphan_acts[k], "\"kind\":\"message\"") != NULL,
           "a message delivery's source was asked of a chain member that is not a driven message listener");
    engine_attacker_post_record(f->orphan_chain[k], orphan_chain_position(f, k), record, sizeof record);
    window_message_delivery_sources(record, ENGINE_ATTACKER_SENDER, data, origin, cap);
}

/* The event an attacker document's postMessage hands a driven Window `message` listener (§9.3.3 step 8.4):
   a MessageEvent whose data and origin are the delivery's attacker sources, named after the drive's chain
   position so each event is its own delivery, with no lastEventId, no source and no ports. Owned. */
static JSValue engine_attacker_message_event(JSContext *ctx, uint64_t hash, int pos) {
    char record[64], dsrc[64], osrc[64], dshape[68], oshape[68];
    JSValue data, origin, ports, noid, ev;

    engine_attacker_post_record(hash, pos, record, sizeof record);
    window_message_delivery_sources(record, ENGINE_ATTACKER_SENDER, dsrc, osrc, sizeof dsrc);
    snprintf(dshape, sizeof dshape, "{%s}", dsrc);
    snprintf(oshape, sizeof oshape, "{%s}", osrc);
    data = concolic_new(ctx, dshape, dsrc, CONCOLIC_WHOSE_WORLD, JS_UNDEFINED);
    origin = concolic_new(ctx, oshape, osrc, CONCOLIC_WHOSE_WORLD, JS_UNDEFINED);
    ports = JS_NewArray(ctx);
    CHECK(!JS_IsException(ports), "engine: OOM building an attacker message's ports");
    noid = JS_NewString(ctx, "");
    CHECK(!JS_IsException(noid), "engine: OOM building an attacker message's lastEventId");
    ev = message_event_new(ctx, "message", data, origin, noid, JS_NULL, ports);
    JS_FreeValue(ctx, noid);
    JS_FreeValue(ctx, data);
    JS_FreeValue(ctx, origin);
    JS_FreeValue(ctx, ports);
    CHECK(!JS_IsException(ev), "engine: OOM building an attacker message's event");
    return ev;
}

/* The person types into the text control whose listener `fn` is (core/events/listener_act.h's `input` act), in
   the running flow's timeline, before the drive's first opcode. The typed text is the attacker source
   `user-input:<locator>`, with the chain position past a body's first call, as engine_orphan_call names the
   call's other unknowns. */
static bool act_is_typing(const char *act) { return act && strstr(act, "\"kind\":\"input\"") != NULL; }

static void engine_orphan_type(JSContext *ctx, JSValueConst fn, uint64_t hash, int pos) {
    JSValueConst control = listener_act_typed_control(ctx, fn);
    char src[64];

    DCHECK(JS_IsObject(control),
           "a drive whose act is typing found no text control holding its listener — the act is answered in the "
           "world the call is built in, and that world's state is the one the drive types into");
    if (pos > 0) snprintf(src, sizeof src, "user-input:%016llx#%d", (unsigned long long)hash, pos);
    else snprintf(src, sizeof src, "user-input:%016llx", (unsigned long long)hash);
    input_value_user_edit(ctx, control, src);
}

/* The event a driven `drop` listener is handed when the person drops a file dragged from another application
   (HTML §6.11.5): fire a DND event's DragEvent named drop (§6.11.4), whose DataTransfer is associated with a drag
   data store in the read-only mode holding one File item. The File's bytes are the attacker source
   `file:drop-<locator>[-pos].bin` (core/file/file_system.h's user file). No page dragstart ran, so the store's
   allowed effects state is still "uninitialized", and the drop's dropEffect is the current drag operation that
   state and a "copy" dropEffect give in §6.11.5's table. Owned.
   Named residual. Not covered: the dragenter and dragover the browser fires at the target first, the drop
   firing only when one was canceled, and the association broken after dispatch. Next diff builds: the drop
   act as §6.11.5's sequence dispatched through DOM §2.9 at the act's target, with this drive's call replaced
   by that dispatch. Absence shows as: a fire-verified drop PoC on a target whose dragover listener never
   cancels, or a page reading a stashed dataTransfer's files after its listener returned. */
static JSValue engine_attacker_drop_event(JSContext *ctx, uint64_t hash, int pos) {
    char name[64];
    JSValue file, files, dt, global, ev;

    if (pos > 0) snprintf(name, sizeof name, "drop-%016llx-%d.bin", (unsigned long long)hash, pos);
    else snprintf(name, sizeof name, "drop-%016llx.bin", (unsigned long long)hash);
    file = file_system_user_file_new(ctx, name);
    files = JS_NewArray(ctx);
    CHECK(!JS_IsException(files), "engine: OOM building a dropped file's list");
    JS_DefinePropertyValueUint32(ctx, files, 0, file, JS_PROP_C_W_E);
    dt = data_transfer_new_for_drag(ctx, files, DND_MODE_READ_ONLY, "copy", "uninitialized");
    JS_FreeValue(ctx, files);
    global = JS_GetGlobalObject(ctx);
    ev = drag_event_new_for_dnd(ctx, "drop", global, dt);
    JS_FreeValue(ctx, global);
    JS_FreeValue(ctx, dt);
    return ev;
}

static JSValue *engine_orphan_call(JSContext *ctx, JSValueConst fn, int argc, uint64_t hash, int pos,
                                   const char *act) {
    JSValue *args = NULL, self, *base;
    char id[64];
    int k;

    DCHECK(hash != 0, "a driven orphan's call was minted with no locator — the unknowns it supplies are named after "
                      "the function's identity");
    DCHECK(flow_running() != NULL && engine_world_reaches_fn(flow_running()->world, fn),
           "a driven orphan's call is being built in a timeline that cannot reach the closure — another timeline "
           "built it, so its environment is state this flow's delta never captured; the take and the route both "
           "filter by the closure's timeline");
    if (argc > 0) {
        args = (JSValue *)reclaim_malloc((size_t)argc * sizeof(JSValue));
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

    /* A body driven again is another call with other arguments (a listener's next event), so a member past
       the chain's first occurrence of the body names its unknowns by its chain position: one name per call, and
       a pin taken over the first event never answers for the second. */
    /* A driven Window `message` listener is handed the event an attacker post delivers, so its data and origin
       are that delivery's attacker sources and not this engine's instrument. */
    k = 0;
    if (act && strstr(act, "\"kind\":\"message\"") && argc > 0) {
        args[0] = engine_attacker_message_event(ctx, hash, pos);
        for (k = 1; k < argc; k++) args[k] = JS_UNDEFINED;
    }
    /* A driven `drop` listener is handed the DragEvent the person's drop fires. */
    if (act && strstr(act, "\"kind\":\"drop\"") && argc > 0) {
        args[0] = engine_attacker_drop_event(ctx, hash, pos);
        for (k = 1; k < argc; k++) args[k] = JS_UNDEFINED;
    }
    for (; k < argc; k++) {
        if (pos > 0) snprintf(id, sizeof id, "{orphan%016llx#%d.arg%d}", (unsigned long long)hash, pos, k);
        else snprintf(id, sizeof id, "{orphan%016llx.arg%d}", (unsigned long long)hash, k);
        /* This engine's own instrument: the argument stood in no byte anywhere, so an address composed from it is
           runtime-only but nothing was learned from it; the provenance says so, as the assert says it of the
           example. */
        args[k] = concolic_new(ctx, id, id, CONCOLIC_WHOSE_INSTRUMENT, JS_UNDEFINED);
        ORPHAN_BINDS_OPEN(args[k], id);
    }
    if (pos > 0) snprintf(id, sizeof id, "{orphan%016llx#%d.this}", (unsigned long long)hash, pos);
    else snprintf(id, sizeof id, "{orphan%016llx.this}", (unsigned long long)hash);
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
   called (or a listener's next event, engine_orphan_redrive), hand a body back to the drives a park recorded for
   it, or build the call of a drive that has one.
   0 is "nothing to take", which lets the caller finish. */
#define ORPHAN_STEP_SEEDED   1
#define ORPHAN_STEP_ROUTED   2
#define ORPHAN_STEP_RESUMED  3

/* Two functions because they answer two questions. `engine_orphan_resume` is this flow's own work: a drive it is,
 * whose body was handed back, and whose call it must build before it can run anything. `engine_orphan_seed` is the
 * frontier's work: it consumes a bit on shared bytecode (JSFunctionBytecode.entered, one per body per timeline), it
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
               "a flow waiting to adopt an orphan's body is a member of a non-forking session — its recipe needs a "
               "session that forks");
        return 0;
    }

    /* A resumed drive that has its function back builds its own call here, where the flow has nothing left to
     * run, which is where the drive belonged in the recording session: the recorded arms the replay consumed run
     * out exactly here and the call's arms are the next in the same chain. Adopting earlier would put the call in
     * front of programs still to replay. It is this flow, not a sibling: the recipe named one flow, whose reward
     * and decision chain are the drive's. */
    if (f->orphan_want && !JS_IsUndefined(f->orphan_claim_fn)) {
        uint64_t hash;

        DCHECK(f->orphan && f->orphan_claim_at < f->orphan_chain_n,
               "a flow waits to adopt an orphan without being one, or past its chain — `orphan_want` is written only "
               "by the cold tier's rebuild, beside the mark and the chain, and clears at the chain's end");
        DCHECK(flow_running() == f,
               "a resumed orphan drive built its call frame while another flow was switched in — the receiver and "
               "arguments would carry that flow's generation");
        hash = f->orphan_chain[f->orphan_claim_at];
        DCHECK(f->orphan_acts[f->orphan_claim_at] == NULL,
               "a resumed drive's member already states an act before its call was built — the rebuild records "
               "none, and the act is read from the body handed over");
        f->orphan_acts[f->orphan_claim_at] = listener_act_of(ctx, f->orphan_claim_fn);
        g_orphans_driven++;
        base = engine_orphan_call(ctx, f->orphan_claim_fn, f->orphan_argc, hash,
                                  orphan_chain_position(f, f->orphan_claim_at), f->orphan_acts[f->orphan_claim_at]);
        /* This flow is the drive, so the person types in its own timeline now. */
        if (act_is_typing(f->orphan_acts[f->orphan_claim_at]))
            engine_orphan_type(ctx, f->orphan_claim_fn, hash, orphan_chain_position(f, f->orphan_claim_at));
        flow_install_base(f, ctx, base);
        /* The body handed over is now the one running: it moves into `fn` under its own name, and the claim moves
           to the next member, which the walks may route as soon as it exists. */
        JS_FreeValue(ctx, f->fn);
        f->fn = f->orphan_claim_fn;
        f->orphan_claim_fn = JS_UNDEFINED;
        f->orphan_hash = hash;
        f->orphan_claim_at++;
        f->orphan_want = f->orphan_claim_at < f->orphan_chain_n;
        if (f->orphan_want) g_orphan_claims_closed = 0;
        /* Not counted again here: `orphanClaimsMet` counts hand-overs by the routing walk (from a take or from a
           body's creation, engine_orphan_born), and each hand-over reaches this line once. */
        return ORPHAN_STEP_RESUMED;
    }
    return 0;
}

/* The frontier's orphan surface, served by whichever member has run out of its own work (see the note above
 * engine_orphan_resume). Its gate is the minting half: the line below adds a member to the frontier
 * (engine_sibling_assemble over decide_fork_same_path), and a non-forking session must gain none from inside itself,
 * which the fork seam asserts for every other door. */
/* A LISTENER RUNS ONCE PER EVENT, so a drive whose body is a listener (its member states an act) is followed by
   another drive of the same body in the world it left: the next event, with arguments of its own
   (engine_orphan_call's position names). A take cannot do it, because a body is taken once per session. It is
   seeded once per flow, when the drive has run out of work and the take found no new body, so a sequence grows
   one event at a time as the frontier serves its tip, and a tip that emits nothing new sinks in the order. */
static int engine_orphan_redrive(JSContext *ctx, Flow *f) {
    int called = f->orphan_chain_n;
    uint64_t hash = f->orphan_hash;
    JSValue *base;
    Flow *sib;
    char *act;

    if (!f->orphan || f->orphan_want || f->redrive_seeded || called == 0 || !f->orphan_acts[called - 1])
        return 0;
    DCHECK(JS_IsFunction(ctx, f->fn) && hash == f->orphan_chain[called - 1],
           "a drive with a called listener member holds no function, or names another body — `fn` and the hash "
           "are written with the member's call");
    f->redrive_seeded = 1;
    /* The next event's act is asked in the world the last call left, which may have removed the listener or
       changed its target. */
    if (!(act = listener_act_of(ctx, f->fn))) return 0;
    g_orphans_driven++;
    base = engine_orphan_call(ctx, f->fn, f->orphan_argc, hash, called, act);
    sib = engine_sibling_assemble(ctx, f, base,
                                  decide_fork_same_path("(a listener the page never fired was driven again — no "
                                                        "predicate was asked)"),
                                  concolic_pins_suspend(), flow_path_marks(f));
    DCHECK(sib->orphan && sib->orphan_chain_n == called,
           "a re-drive's assembly did not carry the drives its flow has called — the next event follows them");
    JS_FreeValue(ctx, sib->fn);
    sib->fn = JS_DupValue(ctx, f->fn);
    sib->orphan_hash = hash;
    sib->typing_due = act_is_typing(act);
    sib->typing_pos = called;
    flow_orphan_chain_push(sib, hash, act, 1);
    sib->orphan_claim_at = sib->orphan_chain_n;
    {
        char loc[17];
        const char *fields[1] = { loc };
        snprintf(loc, sizeof loc, "%016llx", (unsigned long long)hash);
        flow_stand_at(sib, FLOW_SITE_DRIVE, hash);
        flow_set_orphan_seen(sib, (int64_t)novelty_drive_note(novelty_id(NOVELTY_DRIVE, fields, 1), sib->visits));
    }
    return ORPHAN_STEP_SEEDED;
}

static int engine_orphan_seed(JSContext *ctx, Flow *f) {
    OrphanTake t = { JS_UNDEFINED, 0, 0 };
    char *act;
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
    if (g_orphan_gen_valid && gen == g_orphan_gen_seen && world_eq(f->world, g_orphan_gen_world)) {
        g_orphan_asks_memo++;
        return engine_orphan_redrive(ctx, f);
    }
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
    /* The walk is priced by the heap entries it visited, the one input its cost grows with. */
    uint64_t walked0 = JS_OrphanWalkCost(JS_GetRuntime(ctx)).entries;
    int took = JS_OrphanTakeOne(ctx, engine_orphan_record, &t);
    thread_work_charge(THREAD_WORK_ORPHAN_WALK, JS_OrphanWalkCost(JS_GetRuntime(ctx)).entries - walked0);
    if (!took) {
        g_orphan_gen_seen = gen; g_orphan_gen_world = f->world; g_orphan_gen_valid = 1;
        g_orphan_asks_empty++;
        DCHECK(JS_IsUndefined(t.fn), "an orphan take that reported nothing still left a function on the record");
        return engine_orphan_redrive(ctx, f);
    }
    /* The take's own row, raised here rather than at either exit below: the two exits (route, seed) are already
       named by solver/step_unit.h, and the partition needs the take they share as a summand of the ask. */
    g_orphan_asks_took++;
    DCHECK(t.n == 1 && !JS_IsUndefined(t.fn),
           "an orphan take reported success without handing over a function — the bit that made it an orphan "
           "is already consumed, so it can never be driven again in this session");

    DCHECK(flow_running() == f,
           "orphans were taken while another flow was switched in — the arms fork this flow's delta, DOM head and "
           "decision state");
    DCHECK(flow_frame(f) == NULL,
           "a flow took an orphan with a suspended frame live — the take happens where the flow has run out of "
           "arranged work, and a sibling forked mid-call would carry a half-finished expression");

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
    act = listener_act_of(ctx, t.fn);
    base = engine_orphan_call(ctx, t.fn, t.argc, hash, 0, act);
    /* The other side of the gate at this function's top, asserted where the member is minted, since a take, a
       hash and a route separate the two. */
    DCHECK(g_sess_forking,
           "a driven orphan was about to join the frontier of a non-forking session — the walk's gate returns before "
           "the take, so a second entry into the drive bypassed it");
    sib = engine_sibling_assemble(ctx, f, base,
                                  decide_fork_same_path("(a function the page never called was driven — no "
                                                        "predicate was asked)"),
                                  concolic_pins_suspend(), flow_path_marks(f));
    sib->orphan = 1;
    /* The function it drives, flow.h's `fn`, so a walk of the frontier can say which functions are being driven.
       The assembly dup'd the parent's, which is given back here. */
    JS_FreeValue(ctx, sib->fn);
    sib->fn = JS_DupValue(ctx, t.fn);
    /* The name that outlives it, written by the park and used by a later session to find the body. Stamped at
       the one place a drive is created; its arms inherit it with the mark (engine_sibling_assemble). A drive seeded
       by a flow that is itself driving is called in the state that drive left, so it is named after the drives
       the seeding flow has called: the assembly copied those, and this one is appended in seeding order. */
    sib->orphan_hash = hash;
    sib->orphan_argc = t.argc;
    DCHECK(sib->orphan_chain_n == (f->orphan ? (f->orphan_want ? f->orphan_claim_at : f->orphan_chain_n) : 0),
           "a seeded drive's chain does not begin with the drives its seeding flow has called — the assembly copies "
           "exactly those");
    sib->typing_due = act_is_typing(act);
    sib->typing_pos = 0;
    flow_orphan_chain_push(sib, hash, act, 0);
    sib->orphan_claim_at = sib->orphan_chain_n;
    /* And how much of this body the product had already explored, read at the seed: the drive's key is the
       body's locator (solver/novelty.h's NOVELTY_DRIVE), which is its script's content and its position there,
       so one bundle at two addresses is one key. The count lowers this drive's optimism and never removes it.
       Written after the assembly's rank-neutral equality, because the drive is an arrival at the discoverer's
       coordinate and not a branch of its work. */
    {
        char loc[17];
        const char *fields[1] = { loc };
        snprintf(loc, sizeof loc, "%016llx", (unsigned long long)hash);
        flow_stand_at(sib, FLOW_SITE_DRIVE, hash);
        flow_set_orphan_seen(sib, (int64_t)novelty_drive_note(novelty_id(NOVELTY_DRIVE, fields, 1), sib->visits));
    }
    /* Where it enters the queue, asserted at the one entry that manufactures its own work: a drive entering at
       virtual time zero would carry the full optimism bonus ahead of the backlog, and a bundle has thousands of
       uncalled functions. It is assembled as a sibling, so flow_fork_inherit hands it the discovering flow's
       account, which equals SFQ's v(t) because this flow is the one in service. Every coordinate is checked: its
       service with the window mark and its family (the aging term's halves). Its site is the driven body's, set
       above, which is what makes it an arrival rather than a branch of this flow's decision. */
    DCHECK(sib->work == f->work && sib->work_gen == f->work_gen && sib->family == f->family,
           "a driven orphan entered the frontier at a virtual time that is not the running flow's — the orphan's arm "
           "inherits the discovering flow's account");

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
    DFAIL("the orphan step reported an outcome it does not have");
    return STEP_UNIT_NONE;
}

/* Skips a target this flow has already asked the scroll question about: the question is asked once per target
   per world, and an arm forked after the ask stands in its answer. */
typedef struct { JSContext *ctx; const Flow *f; } ScrollSkip;
static bool scroll_already_asked(void *arg, lxb_dom_element_t *t) {
    const ScrollSkip *k = arg;
    char *loc = listener_act_target_locator(k->ctx, lxb_dom_interface_node(t));
    bool asked = false;

    if (!loc) return true;   /* a target no locator names is one no PoC could tell a person to scroll to */
    for (int i = 0; i < k->f->scroll_asked_n && !asked; i++) asked = strcmp(k->f->scroll_asked[i], loc) == 0;
    free(loc);
    return asked;
}

/* A modelled user scroll: the person may scroll an observed target into view (core/dom/user_scroll.h), so the
 * observer's next Intersection Observer §3.2.10 update sees it intersect. A person may or may not scroll, so both
 * arms run: a recorded two-armed choice (decide_completion) whose arm 0 does not scroll and whose arm 1 performs
 * the scroll as its next frame and states it as a scroll act (which a PoC states). Like a drive of a listener,
 * the scrolling arm is this tool's act, so flow_path_driven reads its user acts. A replay re-asks the choice
 * at this rung and takes the recorded arm, so nothing is cold-tier state.
 * Returns 1 when the question was asked; 0 when no observed target is out of view or every one was asked.
 * Named residual. Not covered: the other members of what a scroll changes — `scroll` and `scrollend` listeners
 * on a Window or an element, and a branch over scrollX/scrollY/scrollTop — and a scroll at any task boundary
 * other than quiescence. Next diff builds: the act offered for a scroll listener's target and for a document whose
 * program read a scroll position, at each task boundary. Absence shows as: a page whose scroll listener holds a
 * sink, or whose branch on scrollY does, never reaching it. */
static int engine_user_scroll(JSContext *ctx, Flow *f) {
    ScrollSkip skip = { ctx, f };
    lxb_dom_element_t *target;
    char *loc, *act;
    size_t n;
    void *dblob;
    int arm, at;
    JSValue *base;

    if (!g_sess_forking) return 0;   /* an @S re-fire and a conformance run send no scroll */
    DCHECK(flow_running() == f && flow_frame(f) == NULL,
           "a user scroll was modelled for a flow that is not running, or with a frame live — it is modelled only "
           "where the running flow has nothing left to run");
    if (!document_fully_active(ctx)) return 0;
    target = intersection_observer_target_out_of_view(ctx, scroll_already_asked, &skip);
    if (!target) return 0;
    loc = listener_act_target_locator(ctx, lxb_dom_interface_node(target));
    DCHECK(loc != NULL, "a target scroll_already_asked admitted has no locator — it refuses exactly those");
    n = strlen(loc) + 64;
    act = reclaim_malloc(n);
    CHECK(act, "engine: OOM spelling a scroll act");
    snprintf(act, n, "{\"kind\":\"scroll\",\"target\":\"%s\",\"type\":\"scroll\"}", loc);
    flow_scroll_asked_push(f, loc);   /* before the fork, so both arms stand in the answer */
    at = f->orphan ? (f->orphan_want ? f->orphan_claim_at : f->orphan_chain_n) : 0;
    arm = decide_completion("(the person scrolls an observed target into view)", &dblob);
    if (arm == 1) {   /* a replay whose recorded path scrolled: this flow is the scrolling arm */
        DCHECK(dblob == NULL, "a recorded scroll choice handed back a sibling's decision state");
        flow_user_act_push(f, act, at);
        flow_install_base(f, ctx, user_scroll_flow(ctx, target));
    } else if (dblob != NULL) {   /* a new choice: this flow does not scroll, its sibling does */
        Flow *sib;

        base = user_scroll_flow(ctx, target);
        sib = engine_sibling_assemble(ctx, f, base, dblob, concolic_pins_suspend(), flow_path_marks(f));
        flow_user_act_push(sib, act, at);
    } else {
        free(act);   /* a replay whose recorded path did not scroll */
    }
    return 1;
}

/* A modelled potential close request: HTML §6.10.1 Close requests' preamble, performed by an agent with no user.
 * §6.10.1's examples (Esc, a back gesture, an assistive technology's dismiss) are user acts, so without this a run
 * never reaches `cancel`/`close` handlers, a dialog's dismissal or a popover's light-dismiss; modelling the arrival
 * is the same kind of act as driving an uncalled function. The arrival is one frame on this flow (the task the
 * preamble queues), preemptible, parkable and forkable at step 7's history-action activation.
 * This flow, not a sibling: there is one arrival and this flow has nothing left to run. No counter or cap:
 * §6.10.2 takes the last group, so three groups are modelled three times, and step 9's "nothing watching for a
 * close request" (core/html/close_request.h) is latched on the flow. A flow that closed something is asked again
 * next step, so a page that adds a watcher in its own `close` handler keeps being modelled.
 * The request is the victim's act performed by this tool, as a drive of a listener is, so it is recorded as a
 * modelled user act before one opcode runs: flow_path_driven reads it, every park past it is graded driven, and
 * a PoC verified past it states it.
 * Returns 1 if it installed the task as this flow's frame; 0 when nothing is left to model or no fully active
 * document is there to target, which lets the caller finish. */
static int engine_close_request_fork(JSContext *ctx, Flow *f) {
    lxb_dom_node_t *document;

    /* The orphan seed's gate, here about the fork: step 7 asks §6.4.1 Data model's history-action activation,
       answered by a fork, so a non-forking session cannot run these steps to their end (an @S re-fire and a
       conformance run send no gesture). */
    if (!g_sess_forking) return 0;
    /* §6.10.1 step 9, as this timeline already answered it (see the header). */
    if (f->close_req_none) return 0;
    DCHECK(flow_running() == f,
           "a modelled close request was built while another flow was switched in — the task reads the close watcher "
           "manager from the running flow's delta");
    DCHECK(flow_frame(f) == NULL,
           "a flow modelled a close request with a suspended frame live — the request is modelled only where the "
           "flow has nothing left to run");
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
    /* A potential close request is a user act on a document the user can reach, and §6.10.1 step 6's note
       gives the only way that document stops being fully active inside the steps: a step 3 listener. A
       document whose traversable was closed (its own close(), a peer's definitely close) is no target, and
       modelling the arrival there would return at step 6 on every ask with nothing latched. */
    if (!document_fully_active(ctx)) return 0;
    flow_install_base(f, ctx, close_request_flow(ctx, document));
    f->close_req = 1;
    {
        char *act = reclaim_strdup("{\"kind\":\"close-request\",\"target\":\"document\",\"type\":\"close-request\"}");

        CHECK(act, "engine: OOM spelling a close-request act");
        flow_user_act_push(f, act, f->orphan ? (f->orphan_want ? f->orphan_claim_at : f->orphan_chain_n) : 0);
    }
    DCHECK(flow_path_driven(f),
           "a flow inside a modelled close request is not graded driven — its requests would be graded the page's "
           "although no user sent the gesture");
    return 1;
}


/* The attacker opens this page's address again in a new window (core/frame/open_again.h): a second load of the
 * page that reads what the first load left in the user agent's local shed, cookie jar and IndexedDB, which is
 * how a value the first load stored from an attacker source reaches code the second load runs. Whether it
 * happens is the attacker's choice, so both arms run: a recorded two-armed choice (decide_completion) whose arm 1
 * performs the open as its next frame and states it as the attacker's act. That act is the tool's at egress
 * (flow_path_driven) and gates nothing a victim asked for (flow_path_victim_driven). A replay re-asks the choice
 * at this rung and takes the recorded arm.
 * Named residual. Not covered: the open is hosted in this instance rather than in a new browsing context
 * group's instance. Next diff builds: the user agent's local shed as cross-instance state in the trusted zone,
 * after which the open is a real new agent cluster. Absence shows as: a same-origin document in another group
 * reads none of this group's writes.
 * Returns 1 when the question was asked. */
static int engine_open_again(JSContext *ctx, Flow *f) {
    char *act;
    void *dblob;
    int arm, at;

    if (!g_sess_forking) return 0;   /* an @S re-fire and a conformance run open nothing */
    /* The act's precondition, not a dedup: without a write of user-agent-wide state since the question was
       last asked, a second load is the first load again. */
    if (!f->persist_wrote) return 0;
    DCHECK(flow_running() == f && flow_frame(f) == NULL,
           "the page was opened again for a flow that is not running, or with a frame live — it is modelled only "
           "where the running flow has nothing left to run");
    if (!document_fully_active(ctx) || !open_again_available(ctx)) return 0;
    f->persist_wrote = 0;   /* before the fork, so both arms stand in the answer */
    /* The act carries its precondition: HTML §7.3.1.7 "Navigable target names"' rules for choosing a navigable
       create the new top-level traversable only past a popup blocker when the attacker page's active window has
       transient activation, and consume it. So the PoC states the victim's click on the attacker page, which the
       live verify performs as a real click there. */
    act = strdup("{\"kind\":\"open\",\"target\":\"address\",\"type\":\"open\",\"activation\":\"attacker-page\"}");
    CHECK(act, "engine: OOM spelling an open act");
    at = f->orphan ? (f->orphan_want ? f->orphan_claim_at : f->orphan_chain_n) : 0;
    arm = decide_completion("(the attacker opens this page's address again in a new window)", &dblob);
    if (arm == 1) {   /* a replay whose recorded path opened the page again: this flow is that arm */
        DCHECK(dblob == NULL, "a recorded open choice handed back a sibling's decision state");
        flow_attacker_act_push(f, act, at);
        flow_install_base(f, ctx, open_again_flow(ctx));
    } else if (dblob != NULL) {   /* a new choice: this flow does not open it, its sibling does */
        Flow *sib = engine_sibling_assemble(ctx, f, open_again_flow(ctx), dblob, concolic_pins_suspend(),
                                            flow_path_marks(f));

        flow_attacker_act_push(sib, act, at);
    } else {
        free(act);   /* a replay whose recorded path did not open it */
    }
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
   an @S candidate flow (created at the pick with the outgoing flow still switched in) is not the new flow's own.
   engine_join_document's rows are created at baseline (qjs_join asks engine_host_baseline first). The policy
   half is unaffected: a policy container is written once at creation and not COW-captured. Next diff builds:
   creation at baseline for the candidate site, as the session's seeding and cold_resume have. Absence shows as: a page that writes
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
    /* An address resolved from a string: an unknown `src` records its shape and parks nothing. */
    req.address = FETCH_ADDRESS_CONCRETE;
    /* §4.12.1.1's three metadata members, off the row's own element (engine_queue_docscript_url and the seed's
       address arm both take it; flow_deliver_one_reply asserts it on the failure path). NULL is a stated answer:
       a host driving a synthesized program list has rows no `<script>` produced, and script_csp_meta answers the
       empty string. */
    ScriptCspMeta meta;
    JSValue e;

    DCHECK(url != NULL && *url, "an external document script entry carries no URL");
    DCHECK(strlen(url) == dyn_body_len(f->dyn[at]),
           "an external document script's row holds an address containing U+0000 — this park reads the URL body as a "
           "NUL-terminated string, so the request shown would be a prefix of the address");
    meta = script_csp_meta(ctx, f->dyn_el[at]);
    req.metadata = meta.m;
    /* The credentials mode, off the same element by the same step as the injected script's (see
       `script_credentials_mode`); a synthesized row takes §8.1.4.2's default script fetch options. */
    req.credentials = script_credentials_mode(f->dyn_el[at]);
    /* The mode, off the element: fetch a classic script creates a potential-CORS request, so a document's own
       `<script src>` without `crossorigin` is `no-cors`, which is what an integrity policy refuses. */
    req.mode = script_request_mode(SCRIPT_TYPE_CLASSIC, f->dyn_el[at]);
    req.referrer = "client";
    req.referrer_policy = f->dyn_el[at] ? referrer_policy_attribute_keyword(f->dyn_el[at]) : "";
    e = pending_push(&f->pending, FLOW_PENDING_DOCSCRIPT, flow_path_forced(f), flow_path_pinned(f), flow_path_driven(f),
                     parser_inserted, flow_work_seq_next());
    /* Which row this park is for, by name: the row's `dyn_id` rather than its position, which holds only while
       the set is fixed (solver/flow.h), and this entry outlives §4.12.1.1's immediate interposition and
       §7.5.10's removal. */
    pending_set_int(e, PEND_SCRIPT_ROW, (int64_t)f->dyn_id[at]);
    /* Which document asked: the row's, whose realm the program compiles in. */
    pending_set_int(e, PEND_DOC, (int)f->dyn_doc[at]);
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
     scripts to every flow standing when it joined;
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
    DCHECK(f != NULL, "a program was queued naming no flow");
    DCHECK(body != NULL, "a program was queued with no body — the caller has nothing to run and the queue "
                         "entry would be a slot the compile below dereferences");
    DCHECK(doc != 0, "a program was queued naming no document — the realm it compiles in is its document's");
    /* The kind and the token are one statement, asserted both ways at the one site that creates a row: a
       cross-agent operation's row carries the token of the peer parked on its completion, and no other kind's
       completion is read as an answer. */
    DCHECK((kind == DYN_CROSS_AGENT_OP) == (token != NULL),
           "a queued program's kind and its rendezvous token disagree — a cross-agent operation's row must "
           "carry the token of the peer waiting on its completion, and no other kind may carry one, because "
           "only that kind's completion is ever read as an answer");
    /* The sequence holds programs and inline import maps: §4.12.1.1's null type and speculation rules take no
       position, and document_exec_scripts and html_script_prepare drop them before here. */
    DCHECK(script_type_is_sequenced(stype),
           "a row was queued with a script type no sequence holds — §4.12.1.1's null type and speculation rules "
           "take no position");
    /* An import map is an element's inline text: §4.12.1.1 fires `error` at an external one. */
    DCHECK(stype != SCRIPT_TYPE_IMPORTMAP || (kind == DYN_PAGE_SCRIPT && url == NULL && el != NULL),
           "an import map row was queued that is not an element's inline text — only an inline `importmap` "
           "script takes a position");
    /* An address belongs to a row whose bytes came from one (§8.1.4.2 bases the script on the response's URL);
       an inline row has none and §4.12.1.1 answers from the document ("let base URL be el's node document's
       document base URL"). A DYN_SCRIPT_SRC row's address is its own body until the reply arrives. */
    DCHECK(kind != DYN_SCRIPT_SRC || url == NULL,
           "an entry holding a script's address was queued with a second address beside it — the row is the URL "
           "until flow_deliver_one_reply replaces it with the source");
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
           "a program was queued without stating its task source, or that none put it there — HTML §8.1.7.1 orders "
           "one source's work across carriers");
    /* A task takes the tail of its source's queue (§8.1.7.1). The one position that is not the tail is reached by
       queuing nothing, §4.12.1.1's "immediately execute the script element el, even if other scripts are already
       executing", so an immediate row naming a task source confuses running a program in place with queuing one. */
    DCHECKF(!task_source_is_task(src) || pos == DYN_POS_APPEND,
            "a queued program named the %s task source and asked to run immediately — a task takes the tail of its "
            "source's queue, and only a program run in place by its causing algorithm takes another position",
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
            "a cross-agent operation's program named the %s task source — it is the continuation of a peer's "
            "synchronous read, which no task queue holds",
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
               "a program was queued to run immediately into a flow that is not running — `immediately` names the "
               "slot after the causing program, which is in the running flow");
        /* The cursor itself when no program is running, the slot after it when one is. A script element inserted
           from a `.then` reaction is at the cursor; one inserted from a running <script> is after it. A live frame
           is not a live program: `frame` holds three kinds of base (JS_FlowIsProgram), and a call (a driven
           orphan, a §8.1.4.4 step 8 report, a §6.10.1 task) or a clone of a module body is not a row, so for both
           the slot is the cursor, matching the frameless parent of such an arm (an arm queuing at a different
           slot from the timeline it continues would be a reorder). Those bases stand with the cursor at the end
           of the sequence, so `script_i + 1` would be `dyn_n + 1` and fail the CHECK below on an ordinary page
           (an SSR hydration shim's inline <script> from a module body). `flow_frame(f) &&` stays: JS_FlowIsProgram needs
           a base to read. */
        at = (flow_frame(f) && JS_FlowIsProgram((JSValue *)flow_frame(f))) ? f->script_i + 1 : f->script_i;
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
              "a program's immediate slot is past the end of the flow's queue — the slot after the cursor is at most "
              "one past the last row");
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
            "a program was queued into slot %d of a flow whose cursor stands at %d — the queue walks forward from "
            "the cursor, so this row would never run or be counted; find what advanced the cursor past `dyn_n` "
            "(ENGINE_LEAVE_ROW is the one place that can)",
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
    if (url) { f->dyn_url[at] = reclaim_strdup(url); CHECK(f->dyn_url[at], "engine: OOM queued script address"); }
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
            "a row of kind %d states that a parser inserted its element while owing no fetch — only the address park "
            "reads HTML §4.12.1.1's `parser document` (solver/pending.h's pending_prov_compose)",
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
    DCHECK(f != NULL, "a program was queued with no flow running");
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

/* A document's script inventory appended as rows of one flow's sequence: the root document's seeding
   (flow_set_seed_hook) and a joined document's arrival in each flow are the same operation on two documents. HTML
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
    int i, queued = 0, base;

    DCHECK(f != NULL, "a document's scripts were seeded onto no flow");
    DCHECK(n == 0 || rows != NULL,
           "a document was seeded with a script count and no table to read it from");
    base = f->dyn_n;
    for (i = 0; i < n; i++) {
        DCHECK(script_type_is_sequenced(rows[i].type),
               "a script table holds a row of a type no sequence holds — §4.12.1.1's null type and a set of "
               "speculation rules are ignored or registered elsewhere, so document_exec_scripts drops both "
               "before they become rows");
        DCHECK(rows[i].body != NULL || rows[i].url != NULL,
               "a script table row holds neither source text nor an address — a refused row keeps its `src` "
               "text as its body, so a row that is nothing would park the flow on nothing for the rest of the "
               "session");
        DCHECK(!rows[i].refused || (rows[i].url == NULL && rows[i].el != NULL),
               "a refused script row carries an address or no element — §4.12.1.1's \"If url is failure\" leaves no "
               "URL, and the `error` event is fired at el");
        /* The position a row is queued at is the position it is addressed at: `queued` counts the rows this
           seed has given a slot, in §4.12.1.1's order, and the cursor indexes the same table. Asserted per
           row. */
        DCHECK(f->dyn_n == base + queued,
               "a document's scripts left the seed out of document order — the row about to be queued is not the "
               "one the sequence is at, so the flow's cursor would reach this element at some other element's "
               "position");
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
/* How the root document was reached (engine.h's engine_set_arrival); `prov` is -1 until a host states it. */
static EngineArrival g_arrival = { -1, 0, 0 };

/* The marks a timeline takes from another instance's flow whose act is `a`. */
static FlowPeerMarks arrival_marks(EngineArrival a) {
    FlowPeerMarks m = { a.prov == PROV_FORCED, a.pinned, a.driven };
    return m;
}

static void arrival_check(EngineArrival a, const char *what) {
    DCHECKF((a.prov == PROV_OBSERVED || a.prov == PROV_DERIVED || a.prov == PROV_FORCED) &&
            (a.driven == 0 || a.driven == 1) && (a.pinned == 0 || a.pinned == 1),
            "%s was stated with the grade %d, drive mark %d and witness mark %d — engine_arrival_of_token "
            "composes the three from their words", what, a.prov, a.driven, a.pinned);
    /* A person names a document by no fork and no drive, so `observed` carries neither mark. */
    DCHECKF(a.prov != PROV_OBSERVED || (!a.driven && !a.pinned),
            "%s was stated `observed` with a drive or witness mark — an observed document is one a person named, "
            "which no drive navigated to and no pin addressed", what);
}

EngineArrival engine_arrival_of_token(const char *token)
{
    EngineArrival a;
    const char *c1, *c2;
    char prov[16];

    CHECK(token != NULL, "engine: a host stated an arrival and handed no word");
    c1 = strchr(token, ',');
    c2 = c1 ? strchr(c1 + 1, ',') : NULL;
    CHECKF(c1 != NULL && c2 != NULL && strchr(c2 + 1, ',') == NULL && (size_t)(c1 - token) < sizeof prov,
           "engine: a host stated the arrival `%s`, which is not `<provenance>,<drive mark>,<witness mark>`", token);
    memcpy(prov, token, (size_t)(c1 - token));
    prov[c1 - token] = 0;
    a.prov = engine_provenance_of_token(prov);
    CHECKF((c2 - c1 - 1 == (ptrdiff_t)strlen(PENDING_DRIVEN_YES) && !strncmp(c1 + 1, PENDING_DRIVEN_YES, c2 - c1 - 1)) ||
           (c2 - c1 - 1 == (ptrdiff_t)strlen(PENDING_DRIVEN_NO) && !strncmp(c1 + 1, PENDING_DRIVEN_NO, c2 - c1 - 1)),
           "engine: the arrival `%s` states a drive mark that is neither word engine.h spells", token);
    a.driven = c2 - c1 - 1 == (ptrdiff_t)strlen(PENDING_DRIVEN_YES) && !strncmp(c1 + 1, PENDING_DRIVEN_YES, c2 - c1 - 1);
    CHECKF(!strcmp(c2 + 1, PENDING_PINNED_YES) || !strcmp(c2 + 1, PENDING_PINNED_NO),
           "engine: the arrival `%s` states a witness mark that is neither word engine.h spells", token);
    a.pinned = !strcmp(c2 + 1, PENDING_PINNED_YES);
    return a;
}

void engine_arrival_token(EngineArrival a, char *buf, size_t cap)
{
    int n;

    arrival_check(a, "a marks word");
    n = snprintf(buf, cap, "%s,%s,%s", engine_provenance_token(a.prov), engine_driven_token(a.driven),
                 engine_pinned_token(a.pinned));
    CHECK(n > 0 && (size_t)n < cap, "engine: a marks word did not fit its room");
}

void engine_set_arrival(EngineArrival a) {
    arrival_check(a, "the instance's document arrival");
    DCHECKF(g_arrival.prov == -1 ||
            (g_arrival.prov == a.prov && g_arrival.driven == a.driven && g_arrival.pinned == a.pinned),
            "this instance's arrival was restated from (%d,%d,%d) to (%d,%d,%d) — it is a fact about the "
            "navigation that provisioned the instance, and one instance has one",
            g_arrival.prov, g_arrival.driven, g_arrival.pinned, a.prov, a.driven, a.pinned);
    g_arrival = a;
}

/* Every fresh timeline of the root document comes through here (flow.c's flow_seed), so a document reached past
   a forced gate has every timeline marked before its seed parks a request — the boot flow, a cold-resumed one
   and an @S candidate alike. */
static void engine_seed_root_flow(Flow *f) {
    DCHECK(g_sess_live, "a flow was seeded with the root document's programs outside a live session — the table "
                        "exists only while a session is open");
    DCHECKF(f != flow_running() && f->dyn_n == 0 && f->script_i == 0,
            "a document's arrival marks were written onto the running flow or onto one already holding %d program "
            "row(s) — the seed parks those rows' requests off these bits, and a flow is seeded once, at creation",
            f->dyn_n);
    flow_take_peer_marks(f, arrival_marks(g_arrival));
    engine_seed_scripts(f, g_sess_doc, g_root_scripts, g_root_n);
}

/* Whether unstarted row `k` of `f`, still owed its bytes, was filed by a parser. HTML §4.12.1.1 files a
   parser-inserted external script as the pending parsing-blocking script or in the list of scripts that will
   execute when the document has finished parsing, and a script-inserted one with a position in the list of
   scripts that will execute in order as soon as possible. The row cannot say which; the park made with it can. */
static int flow_src_row_parser_inserted(const Flow *f, int k)
{
    int j, m = pending_count(f->pending), found = -1;

    for (j = 0; j < m && found < 0; j++) {
        JSValue pe = pending_entry(f->pending, j);
        if (pending_get_int(pe, PEND_KIND) == FLOW_PENDING_DOCSCRIPT &&
            (uint64_t)pending_get_int(pe, PEND_SCRIPT_ROW) == f->dyn_id[k])
            found = (int)pending_get_int(pe, PEND_PARSER_INS);
        JS_FreeValue(pending_ctx(), pe);
    }
    DCHECKF(found >= 0,
            "row %d (id %llu) is owed its bytes and no park names it — §4.12.1.1 step 33 fetches when the row is "
            "made, so the entry left the register without the row", k, (unsigned long long)f->dyn_id[k]);
    return found == 1;
}

/* See engine.h. Step 5 runs the document's list, then queues the DOMContentLoaded task, so every program of the
   document already in the sequence precedes it. The one row that does not is an in-order-ASAP script still owed
   its bytes: step 7 waits for that list, not step 5. */
int engine_document_dcl_scripts_held(uint32_t doc)
{
    const Flow *f = flow_running();

    DCHECK(f != NULL, "§13.2.7's script lists were asked of a document with no running flow to hold them");
    for (int k = f->script_i; k < f->dyn_n; k++) {
        if (f->dyn_doc[k] != doc || !flow_row_unstarted(f, k)) continue;
        if (f->dyn_cand[k] != DYN_SCRIPT_SRC || flow_src_row_parser_inserted(f, k)) return 1;
    }
    return 0;
}

/* §13.2.7's run rank of row k's element (core/loader/script_type.h's script_sched_run_rank), or -1 for a row no
   element the parser placed. A row's element is the parser's unless §4.12.1.1 recorded it as not
   parser-inserted: an inventory element answers unstated, because the parse never ran preparation over it
   (html_script.h's html_script_parser_inserted residual). Rows at or past the cursor are running or unstarted. */
static int flow_row_parse_rank(const Flow *f, int k)
{
    lxb_dom_element_t *el = f->dyn_el[k];

    if (el == NULL || html_script_parser_metadata(el) == HTML_SCRIPT_PARSER_METADATA_NOT_PARSER_INSERTED)
        return -1;
    return script_sched_run_rank(script_block_schedule(el, (ScriptType)f->dyn_type[k], /*parser_inserted*/true,
                                                       /*force_async*/false));
}

lxb_dom_element_t *engine_document_parse_position(uint32_t doc)
{
    const Flow *f = flow_running();

    DCHECK(f != NULL, "a document's parse position was asked with no running flow to hold its rows");
    for (int k = f->script_i; k < f->dyn_n; k++)
        if (f->dyn_doc[k] == doc && flow_row_parse_rank(f, k) == 0)
            return f->dyn_el[k];
    return NULL;
}

/* See engine.h. A member of the set of scripts that will execute as soon as possible has no row until its reply
   arrives (engine_pending_script_url), so the register answers for it. */
int engine_document_load_scripts_held(uint32_t doc)
{
    const Flow *f = flow_running();
    int j, m;

    DCHECK(f != NULL, "§13.2.7's script lists were asked of a document with no running flow to hold them");
    if (flow_programs_unstarted_for_document(f, doc) > 0) return 1;
    m = pending_count(f->pending);
    for (j = 0; j < m; j++) {
        JSValue pe = pending_entry(f->pending, j);
        int hit = pending_get_int(pe, PEND_KIND) == FLOW_PENDING_SCRIPT &&
                   (uint32_t)pending_get_int(pe, PEND_DOC) == doc;
        JS_FreeValue(pending_ctx(), pe);
        if (hit) return 1;
    }
    return 0;
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
           "a program whose bytes came from a response was queued with no address — §8.1.4.2 creates the script with "
           "the response's URL and §8.1.4.1 keeps it as the base URL");
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
    DCHECK(el != NULL, "a `<script>` element's program was queued with no element — the element-less entry is "
                       "engine_queue_fetched_script");
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
void engine_queue_script_immediate(uint32_t doc, const char *body, size_t body_n, ScriptType stype,
                                   lxb_dom_element_t *el) {
    DCHECK(el != NULL, "an inline classic script was queued to run immediately with no element — §4.12.1.1's prepare "
                       "the script element is about el");
    DCHECK(stype == SCRIPT_TYPE_CLASSIC || stype == SCRIPT_TYPE_IMPORTMAP,
           "a script was queued to run immediately as a module — §4.12.1.1 sends every module to one of its lists "
           "before \"immediately execute the script element\"");
    /* No fetch is owed (see engine_queue_element_script). */
    engine_queue_el(doc, body, body_n, DYN_PAGE_SCRIPT, stype, NULL, TASK_SOURCE_NOT_A_TASK,
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
                       "program from the moment it takes the position");
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
           "a dump was asked of a document whose every timeline has finished — there is no flow to run the program");
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

    DCHECK(g_sess_live, "an image of every world was asked of an instance with no live session — no mark could ever "
                        "be discharged");
    n = flow_count();
    DCHECK(n > 0,
           "an image of every world was asked of a document whose every timeline has finished — only the baseline "
           "remains to render");
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
                                "operands and program would use that flow's delta and document");
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
               "a §7.2.1 member answered from the §7.2.3 record is neither a number nor a boolean — this route "
               "converts in this instance's root realm, which is sound only for values that read no realm; route an "
               "object-valued member through the program route below");
        perform_answer_notify(ctx, ctx, f, token, ENGINE_COMPLETION_NORMAL, answer);
        JS_FreeValue(ctx, answer);
        perform_token_spent(ctx, token);
    } else if (deliver_admits(ctx, f, remote_op_worlds(op)) != STEP_UNIT_ROUTED_DELIVERY) {
        /* The program route receives the asking world as a delivery does, so a timeline committed against it
           neither runs the program nor answers; the timeline on the other side of that commitment holds its own
           copy of the entry (perform_q_fork) and answers. */
        g_step_unit = STEP_UNIT_CROSS_AGENT_OP_NOT_MINE;
        perform_token_spent(ctx, token);
    } else {
        /* The entry is spent and the token survives it: the record has become a program, and the token is copied
           onto the program's row, because the answer is the program's completion. Copied, not moved: a forked arm
           may still name the queue entry, so the row owns its own string. */
        char *own;
        JSContext *rctx = doc_realm(doc);
        EngineArrival marks = record_marks_field(remote_op_marks_field(op));
        FlowCommitArm arm;

        /* The program runs the peer page's code (a close runs its unload handlers) on the asking flow's path, so
           the asked timeline takes the asker's marks; the arm forked first is the timeline never asked, which
           holds no program row and so answers nothing under the asker's rendezvous. */
        arm = deliver_fork_arm(ctx, f, remote_op_worlds(op), marks,
                               "(a cross-agent operation arrived from a marked or branched asker — no predicate "
                               "was asked)");
        deliver_commit_taken(ctx, f, remote_op_worlds(op), arm);
        deliver_take_marks(f, marks);
        own = reclaim_strdup(token);
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
        CHECK(g != NULL, "engine: OOM recording a host instrument's dump");
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
    char world[1024], marks[64];
    char *enc, *rec;
    size_t cap;

    DCHECK(token != NULL && *token,
           "a cross-agent operation's answer was written under no rendezvous token — the completion names no "
           "question");
    enc = remote_completion_encode(ectx, completion, cv);
    /* world_serialize crashes on truncation rather than sending a prefix, so this notice carries the same name as
       every other record of this world. */
    world_serialize(f->world, world, sizeof world);
    /* The answering timeline's marks, which the asking arm takes with the answer (engine_host_take). */
    flow_marks_token(f, marks, sizeof marks);
    cap = strlen(token) + strlen(world) + strlen(marks) + strlen(enc) + 24;
    rec = reclaim_malloc(cap);
    CHECK(rec != NULL, "engine: OOM writing a cross-agent operation's answer — a dropped answer parks the "
                       "asking flow on a question nothing will answer again");
    /* The world sits before the completion because the completion is the remainder: a value grammar may contain a
       tab, a world vector may not (its fields are ':' and ','). */
    snprintf(rec, cap, "remoteop.answer\t%s\t%s\t%s\t%s", token, world, marks, enc);
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
           "a cross-agent operation was answered from a cursor that is not on the queue — the kind that selected "
           "this call was read from the same row");
    token = f->dyn_token[row];
    DCHECK(token != NULL,
           "a cross-agent operation's program completed with no rendezvous token on its row — the token is the only "
           "thing naming the asking world's question");
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
    f->dyn_token[row] = NULL;
    perform_token_spent(ctx, token);
    free(token);
}

/* Preempt hook: two orthogonal yield decisions at the one per-opcode suspend point.
   (1) Value yield: suspend the running flow the moment a parked flow outranks it (the WFQ, not a clock, decides).
       The rival is found by an O(flows) scan only when the frontier generation or the running flow changes; its
       weight is recomputed at every consultation.
   (2) Cooperative-quantum yield: a thread-sharing floor, so even a top-ranked flow breathes after a slice and the
       host loop can interleave, pump and snapshot. Not a step cap: no flow is dropped or reordered, and the flow
       resumes byte-identically. The slice is engine work (engine_quantum_spent), counted at dispatch, so where a
       flow parks is a function of its path; solver/quantum.h's host yield only hands the thread to the host. */
static int64_t engine_now_ms(void);   /* the wall clock, for the gap census below: reported, never a verdict */

/* The engine's one currency: the work its flows retired (quickjs.h's JSFlowControlHooks.work), banked here and
   nowhere else. A lifetime count of this instance, never reset: the slice, a step's aging charge and Level 1's
   service are each a difference of this one counter, so they cannot disagree about what a unit is. */
static int64_t g_service_work = 0;
/* Where the open slice started on that counter, set at engine_sched_step's quantum_begin(). */
static int64_t g_slice_work0 = 0;

/* Bank `units` the running flow retired: its event-loop clock moves (HTML §8.1.7.3's second mover, which
   asserts a running flow at its origin) and the instance's counter with it. */
static void engine_work_bank(JSContext *ctx, uint64_t units) {
    DCHECKF(units <= (uint64_t)(INT64_MAX - g_service_work),
            "banking %llu units would overflow the instance's work counter (%lld) — the units are counted per "
            "dispatch, so this is a count that stopped being one", (unsigned long long)units,
            (long long)g_service_work);
    event_loop_work_advance(ctx, units);
    g_service_work += (int64_t)units;
    /* …and the selected agent's charge, which flow_select_agent compares between the flow's agents. */
    flow_agent_running(flow_running())->work += units;
}

/* The work this slice has banked. The scheduler loop asks it between steps, where the running flow's standing
   count has been banked by the step's close. */
static int64_t engine_slice_work(void) { return g_service_work - g_slice_work0; }

/* Is the open slice's budget spent, asked while a flow executes: the banked work plus the running flow's standing
   count (JS_FlowPeekRetiredWork), so the answer is exact at any dispatch. Monotone within a slice. */
static int engine_quantum_spent(void) {
    DCHECK(quantum_slice_open(), "the slice's budget was asked outside an open slice — whoever is running this "
                                 "flow is not the scheduler");
    return engine_slice_work() + (int64_t)JS_FlowPeekRetiredWork() >= ENGINE_QUANTUM_WORK;
}
/* The rival is cached; the comparison is not. What is cached is the rival's identity, found by the O(flows) scan,
   and the scan reruns only when the generation or running flow changes. Both weights are recomputed at every
   consultation, because a parked flow's weight moves without a generation bump: `flow_silence_notch` reads the
   flow's own silence plus its family's `fam_work`, which `flow_age_running` writes after every step, and on a real
   page the whole frontier is one family (every flow descends from the boot flow). The notch is a floor of
   `(cpu_i + fam_work) / S`, so a step can carry the rival across a boundary while the running flow's stands still; a
   cached weight would then yield to a flow no longer better. A fitness write cannot cause that: it is made only on
   the running flow, through flow_observe_survival or flow_observe_rung, which both raise the generation (a fire
   with the generation equal to the ranked one is a notch straddle, not a distance write). The cached pointer is
   safe across the key: a flow leaving the frontier changes membership and raises the generation, asserted where
   the pointer is taken. */
static unsigned g_seen_gen = 0; static Flow *g_seen_cur = NULL; static Flow *g_rival = NULL;
/* The tenure `g_seen_cur` was read in (flow_running_switches): the cached rival may itself have held the thread,
   and been charged, between two consultations that both see the same incumbent. */
static uint64_t g_seen_switch = 0;
/* Which half of that key moved each time (solver/engine.h explains the reading and why the third row is not a
   rounding row). Raised in the rescan branch after the key compare and before the walk, so they partition
   `scanRivalRuns` by construction. Lifetime counts, raised in every build; only the census reads them. */
static uint64_t g_rival_miss_gen = 0, g_rival_miss_cur = 0, g_rival_miss_both = 0;
/* What the rival was worth in every term the frontier generation is supposed to speak for, recorded where the
   pointer is taken and checked at every consultation until the generation moves. The mirror of `g_ranked_*`
   below: that block holds the incumbent, whose terms may move while it runs; this holds a member that is not
   running and must not move except through the one quantity that legitimately does (flow_silence_phase: the
   aging's carry bit, whose threshold is the family's). If a reward, completed-unit count, fitness distance or own
   service notch could move with the generation still, the order's index (flow.c's flow_ix_*, which holds each
   member's key exact only because every such writer re-keys) would already be wrong; this catches it cheaply on a
   member the hook holds anyway, and also checks the pointer's safety argument for more than membership. Written
   in every build and read only in dev, like `g_ranked_*`: four stores after an O(members) walk, and guarding them
   would put the assertion's operands behind a second condition. */
static int64_t g_rival_own_notch = 0, g_rival_opt_den = 0;
static double g_rival_val = 0.0, g_rival_dist = 0.0;
/* The integer the rival's family coordinate prices (solver/flow.c's acct_family_coord): its family's notch plus its
   carry bit, which is its total notch less its own. flow_age_running charges the family without a rank change, so
   this moves between two generations, and only up: the own notch stands still and the total is a floor of a sum
   that only grows. The fifth term, and the one-sided one. */
static int64_t g_rival_fam_coord_notch = 0;
/* The weight itself, the one clause that survives a summand being added: the five above are fields of a computed
   value, and an assert over the accessor cannot be short a summand. It is one-sided by design: between two
   generations four stand still and the fifth only rises, so the computed weight is non-increasing (rounding to
   nearest is monotone in each operand of flow_order_key's sums); equality would fire on the one movement the
   interval permits. It fires on what the five cannot see: a forgiveness of the family's silence with the generation
   unchanged (flow_credit_emit is that writer and bumps today), a summand that moves up, or a notch that decreases.
   The direction is the precondition for any cached maximum, heap or index over flow_index_key; flow.h's
   `sil_phases` prices the magnitude. Written in every build, read only in dev (in release the condition is inside
   a `sizeof`).
   Named residual. Not covered: every member but the cached rival. solver/flow.c's walk asserts each member's own
   half equal across a generation, but nothing asserts the direction of the whole weight for the rest. Next diff
   builds: a per-member weight stamp beside `ikey_last`, compared with `<=` at that walk, dev-only. Absence shows
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
/* The optimism denominator, the UCB term's operand (solver/flow.c's FlowSite): it moves when the flow's site is
   served across a quantum or the flow stands at another site, and a flow can be re-picked without a switch-in, so it
   can change while the silence notch stands still. */
static int64_t g_ranked_opt_den = 0;
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
/* The same two in work, the pair a verdict could decide on (solver/engine.h's `over_gap_arms`). The wall pair is
   dev-only and reports a seamless turn's whole duration; these are raised in every build and report the widest
   stretch of work between two consecutive offers of a turn that offered some, comparable with
   ENGINE_QUANTUM_WORK. Declared here because preempt_hook writes them; their lifetime scalar is declared with
   `g_over_asks`, where it is read. Published as `sliceOverrunGapWork` and `stepUnitOverrunGapArms`. */
static int64_t g_gap_last_work = 0, g_gap_max_work = 0;

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
       scheduler is running, and between two engine_sched_step calls there is none: the thread is the host's. An
       entry reaching this hook on the host's time would be asking whether to park a flow nobody drives. It is
       asserted here because clauses (0) and (1) can return before the budget (and its own precondition) is
       reached. A hit is a new host-time entry into the interpreter:
       give it a driver that can act on the answer, or stop it consulting the policy; never widen the slice. */
    DCHECK(quantum_slice_open(),
           "the preempt policy was consulted with no slice open — something reached the interpreter on the host's "
           "time between two steps");
    /* This consultation, counted (`preemptAsksLifetime`), before the rescan below so the containment its quotient
       rests on is an order of statements. */
    g_preempt_asked++;
    /* The work since the last offer (solver/engine.h's `over_gap_arms`), raised beside the count and above every
       clause, so an offer answered by clause (1) or the value yield still counts and both numbers bracket the
       same population. The max is taken before the reading is stored, so it is the stretch between consecutive
       offers; `g_gap_last_work` is opened at the turn's bracket, so the first stretch is the one before the first
       offer. */
    {
        int64_t gap_now = g_service_work + (int64_t)JS_FlowPeekRetiredWork();
        if (gap_now - g_gap_last_work > g_gap_max_work) g_gap_max_work = gap_now - g_gap_last_work;
        g_gap_last_work = gap_now;
    }
    /* The wall gap census belongs to the seam message and is compiled out with it; the count and the
       work gap above are not, because solver/result.c publishes them in every build. `engine_now_ms()`
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
    const int rival_cur_moved = (cur != g_seen_cur || flow_running_switches() != g_seen_switch);
    if (rival_gen_moved || rival_cur_moved) {   /* (1) rescan for the rival only on change */
        g_seen_gen = flow_frontier_gen(); g_seen_cur = cur; g_seen_switch = flow_running_switches();
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
               "the value yield cached a rival that is not in the frontier — a departure must raise the generation "
               "that invalidates the cache");
        /* The rival's own terms, frozen with the pointer after the membership assert, since they dereference it.
           The own service notch, not the silence notch: the silence adds the family's half, which advances on
           every step of whoever holds the thread, the one movement this interval permits. */
        if (g_rival) {
            g_rival_own_notch = flow_service_notch(g_rival); g_rival_opt_den = flow_optimism_den(g_rival);
            g_rival_val = flow_reward(g_rival); g_rival_dist = flow_distance(g_rival);
            g_rival_fam_coord_notch = flow_family_notch(g_rival) + flow_silence_carry(g_rival);
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
       charged by flow_age_running only to `g_running`; the same charge moves the family's notch and carry with
       no rank change, and their sum may only rise. Dev-only and O(1).
       The message names which term moved, and whether the rival is still a member: a departure the cache outlived
       reads as a moved term (a freed flow answers 0 through the early returns that ask `family`), and the two
       take opposite diffs. flow_is_member is free on the passing path, since DCHECKF evaluates its arguments only
       when the condition failed. Every writer in solver/flow.c of these terms raises the rank change in the same C
       call, except flow_credit_visit's completed-unit count, whose sole caller credits `cur`, which forces the
       rescan by its second half. */
    DCHECKF(!g_rival ||
            (flow_service_notch(g_rival) == g_rival_own_notch && flow_optimism_den(g_rival) == g_rival_opt_den &&
             flow_reward(g_rival) == g_rival_val && flow_distance(g_rival) == g_rival_dist &&
             flow_family_notch(g_rival) + flow_silence_carry(g_rival) >= g_rival_fam_coord_notch),
            "a term of the cached rival's weight moved while the frontier generation stood still (own notch %lld -> "
            "%lld, optimism denominator %lld -> %lld, reward %g -> %g, distance %g -> %g, "
            "family notch plus carry %lld -> %lld, still a member: %d; -1 and 0 mean it departed) — raise "
            "frontier_rank_changed() at the writer that moved it",
            (long long)g_rival_own_notch, (long long)flow_service_notch(g_rival),
            (long long)g_rival_opt_den, (long long)(flow_is_member(g_rival) ? flow_optimism_den(g_rival) : -1),
            g_rival_val, flow_reward(g_rival),
            g_rival_dist, flow_distance(g_rival),
            (long long)g_rival_fam_coord_notch, (long long)(flow_family_notch(g_rival) + flow_silence_carry(g_rival)),
            flow_is_member(g_rival));
    /* The same claim over the whole weight, after the five so that a fire here is a movement the five have no
       name for: five terms in bounds and a weight that rose, which no term of today's weight can do. See the
       declaration for the state and the one-sided bound. */
    DCHECKF(!g_rival || flow_weight(g_rival) <= g_rival_w,
            "the cached rival's weight rose while the frontier generation stood still (cached %.17g, current %.17g; "
            "family notch plus carry %lld -> %lld) — between generations a non-running member's weight may only "
            "fall, so a forgiveness of its family's silence (flow_credit_emit) or another writer lost its rank-change "
            "raise, or flow_order_key composes a term that moves without a write",
            g_rival_w, flow_weight(g_rival), (long long)g_rival_fam_coord_notch,
            (long long)(flow_family_notch(g_rival) + flow_silence_carry(g_rival)));
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
               flow_optimism_den(cur) != g_ranked_opt_den || flow_reward(cur) != g_ranked_val ||
               flow_distance(cur) != g_ranked_dist,
               "the value yield fired on a flow whose rank nothing changed since it was switched in — generation, "
               "silence notch, optimism denominator, reward and distance are unchanged, so the pick and the hook are "
               "answering one state two ways");
        return 1;
    }
    /* (2) Cooperative-quantum floor: thread-sharing, not value. The same budget the scheduler loop returns to the
       host on, so a flow parking on it and a step returning on it are one event seen twice. Nothing is dropped,
       starved or reordered: the same flow resumes byte-identically unless the WFQ says otherwise. */
    return engine_quantum_spent() || quantum_host_wants_thread();
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
           "the orphan round trip was asked for one of its two numbers — the met and unmet counts are returned "
           "together");
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
               "a host-time bracket named a queued callback's owner while a slice was open — owners are named only "
               "between steps, so two queuers claim one callback");
    /* There is no global drain: declining would hand the job to quickjs's global list, which nothing runs, so it
       would be dropped. Every task source comes through here (a window message, a port delivery, a broadcast, a
       timer callback, a custom element reaction), and a dropped one is a handler the page registered and this
       engine never entered. */
    DCHECK(f != NULL, "a job was enqueued with no flow running — there is no global drain; seed it as a flow on the "
                      "frontier");
    if (!f) return 0;
    DCHECK(ctx != NULL, "a job was enqueued with no realm — §7.5.10 step 5 removes a destroyed document's tasks by "
                        "comparing it");
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
            "a callback was queued as a task without stating its task source, or that none put it there — HTML "
            "§8.1.7.1 orders one source's work across carriers (is_task=%d)", (int)is_task);
    if (g_task_arrival) {
        uint64_t seq = g_task_arrival;

        DCHECK(is_task, "a microtask was queued under a task's arrival stamp");
        g_task_arrival = 0;
        flow_job_push_arrived(ctx, f, fn, argc, argv, handle, seq);
    } else {
        flow_job_push(ctx, f, fn, argc, argv, is_task, handle);
    }
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
           "a queued task was removed by name with no flow running — tracker state changes only inside a flow");
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
    DCHECK(f->compile == NULL,
           "a job ran while this flow holds a parse parked part way — the parse is the task in progress (HTML §8.1.4.3 "
           "Creating scripts), and flow_stack_empty, which every job arm is guarded on, answers no while it is held");
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
   why it is not `unframed_picks_lifetime`). Raised at the line entering flow_step's `if (!flow_frame(f))` block, the
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
     `g_task_held_seq`: the program-sequence arm took it (`seq_compiles && !job_precedes`): the queued task is younger
       than the row at the cursor. The only record of that answer.
     `g_task_arm_older`: the task arm ran because the comparison said the task is older.
     `g_task_arm_row_wait`: the task arm ran while the row at the cursor is an external script still to start
       (`seq_awaits`): the event loop spinning while that row waits, its reply owed or answered behind older tasks.
     `g_task_arm_no_row`: the task arm ran with no row at the cursor; a member past the end of its sequence is here
       at every step (`programCursors`' top bucket counts them).
   Identity, asserted at the accessor: the three task-arm reasons sum to g_step_unit_runs[STEP_UNIT_RUN_TASK]; the
   parts are raised at the choice and the total after flow_step returns, so a second writer or an early return
   breaks it. Not covered: the three arms above delivery (link-connected time, a routed delivery, a peer's parked
   operation), visible as `run-a-task` plus these five not accounting for unframed steps while `jobsReadyTask` stays
   above zero; the checkpoint arm (no task is runnable there; `jobsReadyMicro` sizes it); and queue depth
   (job_pick_index takes the oldest, so a late task waits behind earlier ones). Lifetime counts, a report only.
   Named residual. Not covered: rows are still not in the arrival chain above the delivery arm. Next diff builds:
   the oldest runnable row's stamp computed above the delivery arm, putting all three carriers in one arrival
   order, after which `g_task_held_deliv` counts nothing. Absence shows as: `g_task_held_deliv` above zero (today
   every use of `seq_compiles` is below the line `grep -n 'uint64_t deliv_seq = flow_stack_empty'` prints). */
static long g_task_held_deliv, g_task_held_seq, g_task_arm_older, g_task_arm_row_wait, g_task_arm_no_row;
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
/* What those steps cost on the pricing clock (solver/engine.h's EngineStepUnitRuns `step_us`: read as a ratio
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
/* Turns whose step retired a slice's work (solver/engine.h's `slice_overruns`), counted from the same reading the
   aging charge is, so a turn cannot be charged one amount and judged on another. */
static int64_t g_slice_over;
/* Whether the overrunning turns offered a suspend point at all (solver/engine.h's `slice_overrun_asks`):
   `g_preempt_asked` bracketed across the step from the same `t_slice0`. In every build: one load per turn, and a
   release engine must be able to say whether an overrunning turn reached the interpreter. A report only. */
static uint64_t g_over_asks;       /* suspend points offered, summed over the turns that met the slice */
static long     g_over_seamless;   /* …and how many of those turns offered NOT ONE */
/* The longest stretch of one of those turns during which the policy was not consulted (solver/engine.h's
   `over_gap_arms`: the pair above is a density, which cannot tell nine spread stretches from one unbroken run).
   The lifetime maximum over non-seamless overrunning turns; its per-turn readings, `g_gap_last_work` and
   `g_gap_max_work`, are declared with their writer preempt_hook. The lead and tail count: the closing reading closes
   the stretch after the last offer, and the turn's opening reading the one before the first. A report only. */
static int64_t g_over_gap_work;    /* the worst such stretch over every non-seamless overrunning turn */
/* The work every turn that stepped a flow retired, the sum of the aging charges (solver/engine.h's
   `step_work`). */
static int64_t g_step_work;
/* The pick sequence's digest (solver/engine.h's `pick_digest`), folded once per turn where the turn's member is
   switched in and has executed nothing. */
static uint64_t g_pick_digest = 14695981039346656037ULL;
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
               "`long` is 4 bytes, so a 32-bit accumulator of the pricing clock overflows after ~35.8 "
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
               "so a 32-bit accumulator of the pricing clock overflows after ~35.8 minutes — and these "
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
    DFAILF("a completion was still live in the runtime at a scheduler boundary, at `%s` after the `%s` step unit — "
           "the exception slot is per runtime and the next flow would receive this throw. Take it where it is "
           "produced (JS_FlowResume's `pres`, the job, JS_ResumeParkedFlow's `pres`, JS_CallAsFlow). parked=%d "
           "detail=%d. The throw was: %s",
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
    DCHECK(argc == 1, "a module script's evaluation-promise rejection reaction ran without exactly one argument — a "
                      "promise reaction job calls its handler with the settled value");
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

static void (*g_agent_program_done_hook)(JSContext *realm, bool ran, JSValueConst evaluation);

void engine_set_agent_program_done_hook(void (*hook)(JSContext *realm, bool ran, JSValueConst evaluation))
{
    DCHECK(hook == NULL || g_agent_program_done_hook == NULL,
           "a second component claimed the agent-program-done hook while the first still holds it");
    g_agent_program_done_hook = hook;
}

/* One step of the selected agent when it is not the window's: HTML §8.1.7.3 Processing model for a worker event
   loop, which has no parser, no rendering and no document lifecycle. A parked continuation resumes first, as in
   flow_step, then the agent's program (a module root not yet evaluated, or a classic script's base), then one of
   its jobs (flow_job_take applies the microtask checkpoint). flow_select_agent picks this agent only when it has
   one of these.
   A classic program that completes abruptly is reported by HTML §8.1.4.4 run a classic script step 8's third
   bullet: the report is installed as the agent's frame, its next work, and the program is done when the report is.
   Named residual. Not covered: an abrupt completion surfacing from a parked continuation's resume, which is
   written to the console without HTML §8.1.4.6's report. Next diff builds: the report for that completion, keyed
   by whose continuation it was. Absence shows as: a worker's throw after a park is a page error with no `error`
   event at the Worker. */
static int flow_step_agent(JSContext *ctx, Flow *f) {
    FlowAgent *a = flow_agent_running(f);
    JSRuntime *rt = JS_GetRuntime(ctx);
    JSValue cv = JS_UNDEFINED, evaluation = JS_UNDEFINED;
    int program_done = 0, program_ran = 1;

    DCHECK(f->cur_agent != 0 && a->realm != NULL, "the agent step ran for the window agent or an agent with no realm");
    if (JS_HasParkedFlow(rt)) {
        g_step_unit = STEP_UNIT_AGENT_RESUME_PARKED;
        JS_ResumeParkedFlow(rt, &cv);
    } else if (!JS_IsUndefined(a->module)) {
        /* A module worker's root: "run the module script", evaluated in the agent's realm on its own turn. The
           evaluation promise settles through the module bodies' own continuations. */
        JSValue m = a->module, url_v = JS_GetPropertyUint32(a->realm, m, 0), src_v = JS_GetPropertyUint32(a->realm, m, 1);
        const char *url = JS_ToCString(a->realm, url_v);
        size_t src_n = 0;
        const char *src = JS_ToCStringLen(a->realm, &src_n, src_v);
        JSValue ev = JS_UNDEFINED;

        g_step_unit = STEP_UNIT_AGENT_EVAL_MODULE;
        a->module = JS_UNDEFINED;
        CHECK(url && src, "engine: OOM reading a module worker's program");
        program_ran = JS_FlowEvalModule(a->realm, src, src_n, url, JS_EVAL_TYPE_MODULE, &ev, NULL) > 0;
        a = flow_agent_running(f);   /* the evaluation may have added an agent, moving the list */
        if (!program_ran) JS_FreeValue(a->realm, JS_GetException(a->realm));
        JS_FreeCString(a->realm, url);
        JS_FreeCString(a->realm, src);
        JS_FreeValue(a->realm, url_v);
        JS_FreeValue(a->realm, src_v);
        JS_FreeValue(a->realm, m);
        evaluation = ev;
        program_done = 1;
    } else if (a->frame) {
        int r;

        g_step_unit = STEP_UNIT_AGENT_RESUME_PROGRAM;
        r = JS_FlowResume(a->realm, (JSValue *)a->frame, &cv);
        a = flow_agent_running(f);   /* the program may have added an agent (a nested worker), moving the list */
        DCHECK(!(r == JS_FLOW_DETACHED && a->reporting),
               "§8.1.4.4 step 8's report detached from a worker agent's frame — it is a step-machine call with "
               "no await of its own, so it ends or parks");
        if (r == 0) { JS_FlowFree(a->realm, (JSValue *)a->frame); a->frame = NULL; }
        else if (r == JS_FLOW_DETACHED) a->frame = NULL;   /* the base is the awaited promise's now */
        if (r == 0 && a->reporting) {
            /* DOM's inner invoke catches a listener's throw and §8.1.4.6 step 6 stops a report of a report, so
               a report that completed abruptly is a step of §8.1.4.6 that threw. */
            DCHECK(!JS_IsException(cv), "§8.1.4.4 step 8's report of a worker program's abrupt completion itself "
                                        "completed abruptly (core/events/report_exception.c)");
            a->reporting = 0;
        } else if (r == 0 && JS_IsException(cv)) {
            /* §8.1.4.4 run a classic script step 8's third bullet: report evaluationStatus.[[Value]] for the
               worker global, as the agent's next work, before the steps after run a worker's script. */
            JSValue e = JS_GetException(a->realm);

            a->frame = report_exception_flow(a->realm, e);
            a->reporting = 1;
            JS_FreeValue(a->realm, e);
            JS_FreeValue(a->realm, cv);
            return 0;
        }
        program_done = r == 0;
    } else {
        DCHECK(flow_job_pending(f) > 0, "an agent with no parks, no program and no job was selected to step");
        g_step_unit = STEP_UNIT_AGENT_RUN_JOB;
        flow_run_one_job(a->realm, f);
        return 0;
    }
    if (JS_IsException(cv)) {
        JSValue e = JS_GetException(a->realm);
        result_page_error_value(a->realm, e);
        JS_FreeValue(a->realm, e);
    }
    JS_FreeValue(a->realm, cv);
    if (program_done) {
        JSContext *pr = a->realm;

        DCHECK(g_agent_program_done_hook != NULL,
               "an agent's program ran to its end with no component to take the steps after it");
        g_agent_program_done_hook(pr, program_ran, evaluation);
        JS_FreeValue(pr, evaluation);
    }
    return 0;
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
       completes, never how many turns, which makes it a granularity and not a cap. Only the task arms that
       continue set it. */
    int turn_task_done = 0;
    /* Whether this call began with a live frame, read once before the loop: the question is about the call, and its
       one consumer is the `!framed_at_entry` assert beside `g_unframed_steps++`. The loop may move `flow_frame(f)`, and
       the two disagreeing is what makes that assert a check. In release it is only type-checked inside `sizeof`. */
    /* The rest point between two steps, where the scheduler names the agent this step runs. */
    flow_select_agent(ctx, f);
    if (f->cur_agent != 0) return flow_step_agent(ctx, f);
    const int framed_at_entry = (flow_frame(f) != NULL);
    for (;;) {
        /* One arm per distinct answer, before anything else this flow could do: everything else consumes what the
           arm is made of. The arm clones the frame this flow is suspended in, and the first resume below takes the
           answer off the register and leaves that call site; the flow is switched in, so its delta, DOM head and
           decision state are what an arm inherits.
           The three assignments at the top of this loop are guarded by `turn_task_done`, as the checkpoint arm is:
           the loop iterates (the turn continuation at the reply delivery), and a continued pass must not rename a
           step already named for the task it performed, or `deliver-one-reply` would be credited only to
           deliveries whose `turn_continues()` answered no. `turn_task_done` is that settled-name predicate: its
           writers are the reply-delivery arm and §13.2.7 step 3's arm, meaning this step performed its task. */
        if (!turn_task_done && flow_answer_exhausted(f)) {
            g_step_unit = STEP_UNIT_RETIRE_EXHAUSTED_ASK;
            return FLOW_STEP_RETIRED;
        }
        if (!turn_task_done) g_step_unit = STEP_UNIT_FORK_PEER_ANSWER;
        if (flow_answer_fork(ctx, f)) return 0;
        if (!turn_task_done) g_step_unit = STEP_UNIT_LEAVE_ANSWER_STANDBY;
        if (flow_answer_standby(ctx, f)) return 0;
        /* One arm per declined request, beside it for the same reason: a fork over a value that arrived (the zone
           will not ask, two feasible outcomes and no observation, flow_decline_fork), built from the same frame,
           delta and DOM head. */
        if (!turn_task_done) g_step_unit = STEP_UNIT_FORK_DECLINED_REQ;
        if (flow_decline_fork(ctx, f)) return 0;
        /* …and the success arm of a reply whose status is not ok, before that reply is delivered. */
        if (!turn_task_done) g_step_unit = STEP_UNIT_FORK_REPLY_SUCCESS;
        if (flow_reply_success_fork(ctx, f)) return 0;
        /* The parked continuation outranks everything else this flow could do: a forced preempt must be
           transparent to observable ordering, so the flow resumes before any job it has queued. Yielding after
           one resume keeps the scheduler in charge; a park that parks again rides the switch-out. */
        if (!turn_task_done) g_step_unit = STEP_UNIT_RESUME_PARKED;
        /* Nothing else may move the name: each writer of `turn_task_done` sets it on the line after it names the
           step, so on any pass created by this function's own `continue` the step is already named. A fourth
           assignment added above without the guard crashes here. */
        DCHECK(!turn_task_done || g_step_unit == STEP_UNIT_DELIVER_REPLY || g_step_unit == STEP_UNIT_PARSE_END,
               "a continued pass of one step carries a unit name that is not the task it performed — "
               "`turn_task_done` is written only by the reply-delivery arm and §13.2.7 step 3's arm, each on the "
               "line after it names the step, so any other name here is an assignment that ran on a pass created "
               "by this function's own `continue` and the step is about to be reported as an arm it never took");
        /* Resuming a parked continuation is progress only if the flow can still get somewhere: a flow blocked on
           the host resumes into the same wait. Its completion is taken here, the one place it can be: a parked
           continuation is the late half of a job (a step machine offers a park at every re-entry, so most jobs
           park), and an uncaught throw from it must be recorded as this page's error (§8.1.4.6 Runtime script
           errors' "report an exception", the hook scripts and jobs use) and taken out of the per-runtime
           `rt->current_exception` before another flow's evaluation finds it. */
        DCHECK(f->compile == NULL || !JS_HasParkedFlow(JS_GetRuntime(ctx)),
               "a flow holds a parked parse and a parked continuation at once — no job runs while a parse is held "
               "(flow_run_one_job), so the continuation parked before the parse began, when the compile arm below "
               "could not have been reached");
        {
            JSValue pcv = JS_UNDEFINED;
            if (JS_ResumeParkedFlow(JS_GetRuntime(ctx), &pcv)) {
                engine_refuse_suspension(ctx);
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
        if (!flow_frame(f)) {
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
               `flow_frame(f)` is what the loop moves. */
            DCHECKF(!framed_at_entry,
                    "this call of flow_step began with a live frame and descended the unframed ladder (step_unit=%d, "
                    "orphan asks %ld) — a frame-clearing exit must return, because the resume performed HTML "
                    "§8.1.7.3 step 2.6's task and the value yield and quantum are consulted only at returns",
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
            /* The orphan gate's answer on this pass, GATE_NONE through GATE_UNINSPECTED once asked. */
            int gate = -1;
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
                /* A job runs the page's code beside, not inside, whatever the parked set holds, so none of it
                   may stand in an operation that forbids script — the resume above takes a parked chain first. */
                DCHECKF(JS_ParkedFlowsScriptForbidden(ctx) == NULL,
                        "a microtask checkpoint ran while a parked continuation forbids script: %s",
                        JS_ParkedFlowsScriptForbidden(ctx));
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
            /* HTML §13.2.7 "The end" step 3, "Update the current document readiness to 'interactive'", in the
               parser's own task: the program that held the last parse position has left the stack and its
               checkpoint has drained, and nothing else of this flow may start first, because step 5's spin is
               where the event loop runs again. The update fires `readystatechange`, whose reactions are this
               turn's checkpoint. */
            if (flow_stack_empty(f) && g_parse_end_hook && g_parse_end_hook(ctx)) {
                g_step_unit = STEP_UNIT_PARSE_END;
                turn_task_done = 1;
                if (turn_continues()) continue;
                return 0;
            }
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
            /* The timer source's due task, stamped at the first task boundary that sees it due. */
            uint64_t timer_seq = flow_stack_empty(f) && g_timer_due_hook ? g_timer_due_hook(ctx) : 0;
            if (deliv_seq != 0 && !flow_task_precedes(f, deliv_seq) &&
                engine_timer_takes_turn(ctx, f, timer_seq, deliv_seq)) {
                g_step_unit = STEP_UNIT_TIMER_ARRIVED;
                return 0;
            }
            /* The deadline race before this arrival: where a timer ahead of the clock comes first, it fires and
               every answered reply arrives after its task (pending_restamp_answered), so the next step runs it. */
            if (deliv_seq != 0 && !flow_task_precedes(f, deliv_seq) && !flow_blocked(f) && g_timer_race_hook &&
                g_timer_race_hook(ctx)) {
                pending_restamp_answered(f->pending, flow_work_seq_next);
                g_step_unit = STEP_UNIT_TIMER_RACED;
                return 0;
            }
            if (deliv_seq != 0 && !flow_task_precedes(f, deliv_seq)) {
                /* Whether this arm took a step the flow's task queue could have had (`g_task_held_deliv`), raised
                   before the delivery, whose settle lengthens the queue. With the stack empty a queued job is a task:
                   the checkpoint arm above is guarded by exactly `flow_job_microtask && flow_stack_empty`, asserted.
                   Since the condition excluded an older task, this counts the arm going ahead of a younger one. */
                DCHECK(!flow_job_microtask(f),
                       "the reply-delivery arm was reached with an empty execution context stack and a microtask "
                       "outstanding — the checkpoint arm above owns that state (HTML §8.1.4.4 \"Calling scripts\" "
                       "step 3 of clean up after running script)");
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
                /* engine_queue_into asserts the row types where rows are made. */
                DCHECK(script_type_is_sequenced(stype),
                       "a row of this flow's script sequence carries a type no sequence holds — every row is "
                       "classic, module or an inline import map");
                if (kind == DYN_SCRIPT_SRC) {
                    /* An external script of some document of this agent, at its position. The row holds its address
                       (§4.12.1.1's encoding-parsed url, never the raw attribute, which the host would resolve
                       against its own base), and the flow runs no program of its own here: §4.12.1.1 fixes this
                       script's position against the scripts around it. The request went out when the row was created
                       (step 33 fetches at prepare; step 35 only decides where the result runs). An answered reply
                       stands here only behind a strictly older task: its stamp is its arrival (Fetch §4.1 "Main
                       fetch"), and the networking arm above declines to an older task, which the task arm below
                       runs while the row waits. The dev block asserts that. */
#if APICLIENT_DEV
                    {
                        /* Read outside the condition, under the dev guard: pending_ready reads the register's
                           `length`, which interns an atom. flow_stack_empty is the other half of the networking
                           arm's conjunction; a DYN_SCRIPT_SRC row is queued DYN_POS_APPEND at both sites that
                           create one, so an immediate row cannot be how this line was reached. */
                        int ready = flow_pending_ready(f);
                        int behind = ready && flow_task_precedes(f, flow_pending_ready_next_seq(f));
                        DCHECK(flow_stack_empty(f) && (!ready || behind),
                               "a flow reached an external script row holding an answered reply with no older "
                               "task queued — the networking arm above delivers every reply no task precedes, so "
                               "that arm's guard and this row's have stopped being the same question, and this "
                               "flow is about to report itself owed a reply the host has already paid");
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
                               "a flow stands on an external script row that owes no request — HTML §4.12.1.1 step "
                               "33 issues the fetch with the row and the entry names it by `dyn_id`, so the park was "
                               "removed without the row");
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
                           "a failed-load row carries no element — both producers (flow_deliver_one_reply's null "
                           "arm, engine_seed_scripts for a refused `src`) assert it, and §4.12.1.1 fires `error` at "
                           "el");
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
                           "a row reached the compile with no body — engine_queue_into asserts one, and the two "
                           "address-bodied kinds are taken by the branches above");
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
             * The delivery arm above the sequence joins the order for tasks (PEND_WORK_SEQ, minted when its answer
             * lands, and flow_task_precedes asked with the reply's stamp), so a flow whose register never empties still
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
                   "the row at this flow's cursor can start and carries no arrival stamp — `dyn_run` is minted with "
                   "the row and re-minted when a delivery takes it out of DYN_SCRIPT_SRC, so a column was copied "
                   "without its siblings");
            int job_precedes = seq_compiles && flow_task_precedes(f, f->dyn_run[f->script_i]);
            /* The queue's length, read once for both arms of the choice (`g_task_held_seq`), so a raise and the
               dispatch it is about read one state; nothing between here and the arms touches the queue. */
            int jobs_here = flow_job_pending(f);
            if (seq_compiles && !job_precedes) {
                /* No work here: this arm's work is the compile below, and its body only declines every arm beneath
                   so control reaches that one compile. The one statement is a report (`g_task_held_seq`): the arrival
                   comparison answered no, so a task on the queue here is one the sequence went in front of. */
                if (jobs_here > 0) g_task_held_seq++;
                /* HTML §13.2.7 "The end" step 5.1, "Spin the event loop until the first script in the list of
                   scripts that will execute when the document has finished parsing has its ready to be
                   parser-executed set to true …", once per such row. §8.1.7.3's spin stops the parser's task and
                   queues a new one to continue, so the row's arrival is re-minted, and a rendering task whose frame
                   is already due on the clock arrived first: it is queued ahead of the re-mint, and the arrival
                   comparison runs it before the row. A frame still ahead belongs after the continuation.
                   Named residual. Not covered: a timer already due at the spin, and §13.2.6.4.8's spin for a
                   parser-blocking script that is ready. Next diff builds: the timer source asked at the spin
                   through the same due-only rule. How its absence would show: a `setTimeout(f, 0)` from a
                   parser-inserted script running after a deferred script. */
                if (f->spin_row != f->dyn_id[f->script_i] && flow_row_parse_rank(f, f->script_i) == 1) {
                    int queued = g_rendering_hook != NULL && g_rendering_hook(ctx, true);

                    f->spin_row = f->dyn_id[f->script_i];
                    f->dyn_run[f->script_i] = flow_work_seq_next();
                    g_step_unit = queued ? STEP_UNIT_PARSE_SPIN_RENDERING : STEP_UNIT_PARSE_SPIN;
                    return 0;
                }
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
                       "before every program in the sequence");
                /* Which of the arm's three reasons this step took it for: `job_precedes` is the arrival comparison
                   answering yes (the row at the cursor is younger than a queued task), `seq_awaits` is a row at the
                   cursor still waiting to start, and otherwise the member has no row left. Different populations;
                   their sum is asserted at the accessor. */
                if (timer_seq != 0 && !flow_task_precedes(f, timer_seq) &&
                    engine_timer_takes_turn(ctx, f, timer_seq, UINT64_MAX)) {
                    g_step_unit = STEP_UNIT_TIMER_ARRIVED;
                    return 0;
                }
                if (job_precedes) g_task_arm_older++;
                else if (seq_awaits) g_task_arm_row_wait++;
                else g_task_arm_no_row++;
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
             * task, and a `fetch()` is on no queue until it completes); the arms below it claim the run is
             * finished (no handler will be attached to a rejection), which an outstanding reply can still
             * falsify, so it gates only them. The seed's claim, that nothing called a function, is gated by
             * engine_outstanding_runs_page_code instead, which asks whether outstanding work can run page code. */
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
             * hang off `load`. An owed `<script src>` holds each stage of its own document only, and only the stage
             * whose list it is in (steps 5 and 7); the hook asks engine_document_dcl_scripts_held and
             * engine_document_load_scripts_held per document. A `fetch()` and a dynamic `import()` delay nothing. */
            else if (g_docdone_hook && g_docdone_hook(ctx)) {
                g_step_unit = STEP_UNIT_LIFECYCLE; return 0; }
            /* Another agent of this flow with work of its own (a dedicated worker's program, parks or jobs): the
               window agent has nothing due, so that agent runs before any clock-driven source may move this one's
               clock, and before the flow may rest or finish. */
            else if (flow_select_other_agent(ctx, f))
                return flow_step_agent(ctx, f);
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
            /* Not while outstanding work can still run page code (engine_outstanding_runs_page_code): that work
               can call the body, and a call built before it puts the drive's questions ahead of the work's on a
               replay or behind them, so a recorded path would diverge at its first slot. */
            else if ((gate = engine_outstanding_runs_page_code(ctx, f, seq_awaits)) == GATE_NONE &&
                     (orphan_step = engine_orphan_resume(ctx, f)) != 0) {
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
               rule): the three rungs above are work the page arranged, and a seed is work the engine forces from a
               body nothing asked for, so the page's clock runs first. It is a priority, not a cap. Above the owed
               return and behind the gate: "nothing called this function" is a fact once no outstanding work can
               still run page code, and a reply nobody reacts to is not such work. The exit's two invariants hold
               by position: `g_orphan_claims_unmet` needs this rung to have answered 0 on the pass that reaches
               the exit, which the exit asserts through `gate`. */
            else if ((gate = engine_outstanding_runs_page_code(ctx, f, seq_awaits)) == GATE_NONE &&
                     (orphan_step = engine_orphan_seed(ctx, f)) != 0) {
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
            /* A modelled user scroll, above the close request: scrolling is ordinary use of the page and a close
               request dismisses what is open. See engine_user_scroll. */
            else if (engine_user_scroll(ctx, f)) {
                g_step_unit = STEP_UNIT_USER_SCROLL; return 0; }
            else if (engine_close_request_fork(ctx, f)) {
                g_step_unit = STEP_UNIT_CLOSE_REQUEST; return 0; }
            /* The attacker loading the page again, after every act inside this document has been offered. See
               engine_open_again. */
            else if (engine_open_again(ctx, f)) {
                g_step_unit = STEP_UNIT_OPEN_AGAIN; return 0; }
            else {
                /* A flow may not finish holding work. Every arm above has offered its queue a turn, so a job still
                   on it here means one of them returned first, and the job would be dropped with the flow silently:
                   a dropped reaction looks exactly like a page that registered no handler. Asserted at the one
                   place "finished" is decided. */
                DCHECK(flow_jobs_held(f) == 0, "a flow finished holding queued jobs — a promise reaction, a timer "
                                     "callback or a delivered message would be dropped with it");
                /* A flow of a referenced document may not finish at all (engine.h's engine_set_referenced). Having
                   nothing left to run is not being done while a peer can still ask this timeline something: it
                   waits on the host for the next operation, which is what OWED means. The flow keeps its snapshot,
                   delta and rank and is out of the pick until the host has something for it. */
                if (g_referenced) { g_step_unit = STEP_UNIT_AWAIT_PEER; return FLOW_STEP_OWED; }
                /* A resumed drive that never got its body back is counted on its way out. The seed is its own rung
                   above this exit and its gate answered GATE_NONE on this pass (asserted above), so reaching the exit
                   means the seed's rung answered 0: the heap holds no untaken orphan at this instant. The orphan set is not final (a live flow creates function
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
                DCHECK(gate == GATE_NONE,
                       "a flow reached the exit while the orphan gate said outstanding work can still run its page "
                       "code — the owed return above takes every owed row and reply, so this is an active timer "
                       "every clock-driven source declined, and the seed's rung was never asked on this pass");
                if (f->orphan_want) {
                    DCHECK(JS_IsUndefined(f->orphan_claim_fn),
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
                           "a flow finished holding rejections nobody was notified about — HTML §8.1.4.7's notify "
                           "runs at every microtask checkpoint, which every unit-ending step reaches");
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
                   "control reached the compile without the sequence arm deciding a program starts — every other arm "
                   "of the task-source ladder returns");
            /* No replay, asserted at the only place a program can start. A flow compiles each row of its sequence
               once and thereafter resumes the suspended frame, so reaching here again for a row it already started
               means the resume path lost the frame and the flow would re-run side effects against a delta that
               already holds them. The one exception is a compile that handed the thread back: the parse is
               suspended at an exact production, no bytecode has run, and the next stint continues it. `compile` is
               NULL the moment a program starts, so the first assert keeps its force for every other reading, and
               the second keeps a parked parse from resuming against a row the flow has moved off. */
            DCHECK(f->script_i > f->last_compiled || f->compile != NULL,
                   "a flow compiled a program it had already started — the suspended frame was lost and the flow is "
                   "replaying its side effects");
            DCHECK(f->compile == NULL || f->script_i == f->last_compiled,
                   "a flow holds a parse suspended for a different row than its cursor names — the parse would "
                   "resume into a program the flow has left");
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
            /* HTML §4.12.1.1 "execute the script element"'s `importmap` arm: "Register an import map given el's
               relevant global object and el's result", the result being HTML §8.1.4.8's create an import map
               parse result over the source text and step 34's base URL, which `prog_name` is for an inline row.
               Nothing runs, so the row is left here; a parse result whose error to rethrow is set is reported
               (§8.1.4.6) as a frame, the shape §8.1.4.4 step 8's report takes, and leaves the row when it ends. */
            if (stype == SCRIPT_TYPE_IMPORTMAP) {
                DCHECK(flow_dyn_url(f) == NULL,
                       "an import map row carries an address — only an inline `importmap` script is a row");
                if (import_map_create_and_register(prog_ctx, body, body_n, prog_name) < 0) {
                    JSValue exc = JS_GetException(prog_ctx);
                    flow_install_base(f, prog_ctx, report_exception_flow(prog_ctx, exc));
                    JS_FreeValue(prog_ctx, exc);
                    f->reporting = 1;
                    g_step_unit = STEP_UNIT_REGISTER_IMPORT_MAP;
                    return 0;
                }
                g_step_unit = STEP_UNIT_REGISTER_IMPORT_MAP;
                ENGINE_LEAVE_ROW(f);
                return 0;
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
                           "a module compile answered suspended and left neither a parked parse nor an evaluation");
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
                    flow_install_base(f, prog_ctx, JS_FlowInstantiate(prog_ctx, held));
                    JS_FreeValue(prog_ctx, held);
                    g_classic_parse_shared++;
                } else {
                    /* The compile, bracketed (see g_classic_compiles): the parse's work is its bytes at the
                     * tokenizer's rate, the units it retired, so the comparison is the inequality
                     * engine_quantum_spent asks. Only the classic arm is bracketed: JS_FlowEvalModule compiles and
                     * evaluates. A failed compile is counted, because it parsed the bytes. Nothing branches on
                     * either.
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
                    JSValue fn = JS_UNDEFINED;
                    ThreadWorkSpan tw;
                    uint64_t parsed0 = JS_FlowParseBytesLife();
                    thread_work_begin(&tw);
                    int cr = JS_FlowCompileStep(prog_ctx, body, body_n, prog_name, src_flags, &fn, &f->compile);
                    thread_work_end(&tw, THREAD_WORK_SCRIPT_PARSE, JS_FlowParseBytesLife() - parsed0);
                    if ((int64_t)((JS_FlowParseBytesLife() - parsed0) * THREAD_WORK_UNITS_PER_BYTE_SCRIPT) >=
                        ENGINE_QUANTUM_WORK)
                        g_classic_compile_over++;
                    if (cr == 0) {
                        /* Parked mid-parse. No ENGINE_LEAVE_ROW and no cursor move: this row is still the row the
                           flow is at, and the ladder re-enters here when the flow is next picked. */
                        DCHECK(f->compile != NULL && JS_IsUndefined(fn),
                               "a compile answered suspended and left neither a parked parse nor a program");
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
                           for the reason dyn_body_parse_hold states. */
                        dyn_body_parse_hold(pb, prog_ctx, prog_name, src_flags, fn);
                        flow_install_base(f, prog_ctx, JS_FlowInstantiate(prog_ctx, fn));
                        JS_FreeValue(prog_ctx, fn);
                    } else {
                        /* The parse failed and the exception is pending: no program starts (§4.12.1.1's "if el's
                           result is null, then fire an event named error at el, and return"), and nothing is held,
                           since a SyntaxError is not a fact about the bytes a later timeline may inherit. */
                        flow_frame(f) = NULL;
                    }
                }
                started = (flow_frame(f) != NULL);
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
                       only under `!flow_frame(f)`), so the previous row's restore has already run. A non-null value is a
                       skipped restore, or two flows' writes leaking across the COW delta. §4.12.1.1 step 36's nested
                       run cannot make this claim and carries the value instead (core/html/html_script.c). */
                    JSValue old = document_current_script_set(prog_ctx, flow_dyn_el(f));

                    DCHECK(JS_IsNull(old),
                           "a classic script began executing while this document's currentScript was set — the flow "
                           "holds one program frame, so the previous restore was skipped or the write is not riding "
                           "the running flow's delta");
                    JS_FreeValue(prog_ctx, old);
                }
            }
            /* How many programs started, counted where `started` is decided and nowhere else: the module arm
               answers from `mr > 0`, the classic from `flow_frame(f) != NULL`, and this is the first line with either
               answer in hand. A failed compile is not a start (a SyntaxError or the OOM floor leaves `started`
               false), as §4.12.1.1's "if el's result is null, then fire an event named error at el, and return"
               says. The three counters are one partition, asserted in engine_frontier_census. The arm is read from
               the row at the cursor, never the flow: a candidate session (`Flow.cand_src`) re-runs the document
               from the baseline and compiles the page's own programs too, and `progQueuedCand` is the ask these
               starts are read against. */
            DCHECK(f->compile == NULL,
                   "a program's start is counted while the flow still holds a suspended parse — both compile arms "
                   "clear the carrier and both park arms return, so a park arm falls through to the start count");
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
        int is_call = JS_FlowIsCall((JSValue *)flow_frame(f));
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
        int is_program = JS_FlowIsProgram((JSValue *)flow_frame(f));
        /* The frame and the cursor pair agree about it. flow.c's flow_programs_unstarted_for_document declares the
           cursor side ("between two programs the cursor stands one past the last started row and during one it
           stands on it"), so `last_compiled == script_i` is "inside a program of its sequence", from two fields
           the compile site maintains. `f->reporting` is the one call frame that is the row's remaining work
           (§8.1.4.4 step 8's report is inside step 8), so it sits on the frame side. Two-sided, so a frame kind
           that stops matching the cursor pair fires here rather than at whichever consumer is reached first. */
        DCHECKF((is_program || f->reporting) == (f->last_compiled == f->script_i),
                "this flow's frame and cursor disagree about whether it is inside a program: the frame says %s and "
                "the pair says %s (script_i=%d last_compiled=%d is_program=%d is_call=%d reporting=%d) — during a "
                "program the cursor stands on the last started row, between programs one past it, and only the "
                "compile site writes `last_compiled`",
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
                   "the frame a step just compiled reports itself as a call root — JS_FlowNew builds a program base");
            g_step_unit = started_here ? STEP_UNIT_START_PROGRAM : STEP_UNIT_RESUME_PROGRAM;
            int r = JS_FlowResume(ctx, (JSValue *)flow_frame(f), &cv);
            engine_refuse_suspension(ctx);
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
                       "a modelled close request's frame does not report itself as a call root — JS_FlowNewCall "
                       "builds it");
                DCHECK(JS_IsBool(cv),
                       "HTML §6.10.1 \"Close requests\"' task completed with something that is not a boolean — its "
                       "machine yields step 9's answer; undefined means close_request_flow's JS_FlowNewCall was "
                       "built with take_result false");
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
                DFAIL("HTML §8.1.4.4 \"Calling scripts\" step 8's report of a classic script's abrupt completion "
                      "itself completed abruptly — DOM's inner invoke catches a listener's throw and §8.1.4.6 stops "
                      "a report of a report, so a step of §8.1.4.6 threw (core/events/report_exception.c); the value "
                      "is the last entry of `pageErrors`");
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
                DFAIL("HTML §6.10.1 \"Close requests\"' close request steps completed abruptly — §6.10.2's cancel "
                      "and close actions cannot throw and DOM's inner invoke catches a listener's throw, so a step "
                      "in core/html/close_request.c or close_watcher.c threw; the value is the last entry of "
                      "`pageErrors`");
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
                DCHECK(JS_GetRealmAgent(doc_realm(flow_dyn_doc(f))) == flow_agent_running(f)->agent,
                       "a §8.1.4.4 step 8 report was built over a realm of another agent than the one whose "
                       "execution context stack it is installed on");
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
                DFAIL("a `javascript:` URL evaluated to a string, which HTML §7.4.2.3.2 step 9 turns into a new "
                      "Document replacing the active one, built from a synthesized `text/html;charset=utf-8` "
                      "response. §7.4's navigate here loads only addresses the host fetches; build a navigate that "
                      "takes a response the engine already has and route this through it");
            JS_FreeValue(ctx, cv);
            /* §8.1.4.4 step 8 is about an evaluationStatus, and a frame that suspended or detached has none: both
               returns below leave without reaching the install, so a report built on those paths would be dropped
               silently, carrying an exception that is not this program's completion. */
            DCHECK(report == NULL || r == 0,
                   "§8.1.4.4 step 8's report was built for a frame that has not completed — a suspended or detached "
                   "base has no abrupt evaluation status");
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
                       "a classic script's program detached its base while §4.12.1.1's currentScript bracket was "
                       "open — the restore runs at the program's completion; route it through whatever drives the "
                       "detached continuation");
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
                       "a modelled close request's task detached its base — the task is a step-machine call with no "
                       "continuation to hand an awaited promise, and step 9's answer rides its completion");
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
                flow_frame(f) = NULL;
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
             * this program vacated (the tail installs it as `flow_frame(f)` and returns), so a completion that owes one
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
        JS_FlowFree(ctx, (JSValue *)flow_frame(f));
        /* The modelled close request's flag dies with the frame it names, at the free rather than at the tail: the
           report install below returns, so a flag cleared only at the bottom would stand over the report's frame,
           whose completion would be read as §6.10.1's step 9 answer. In dev an abruptly-completing task aborts
           above; in release that DFAIL is compiled out, the build where a standing flag has nothing to say so. Its
           value was already read by the completion arm above. */
        f->close_req = 0;
        /* §8.1.4.4 step 8.3.1 takes the slot step 8's program vacated, and the cursor does not move: the report is
           the rest of this row's run a classic script, so the row is advanced once, at the report's completion.
           Everything the `if (!flow_frame(f))` block above would do, step 8.3.2's microtask checkpoint first, is skipped
           while this frame lives, the order §8.1.4.4 states and the reason the report is a frame. */
        if (report) { flow_frame(f) = report; f->reporting = 1; return 0; }
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
        flow_frame(f) = NULL;
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
    DCHECK(out != NULL, "the ladder's task-arm census was asked for into nothing");
    out->task_held_deliv = g_task_held_deliv;
    out->task_held_seq   = g_task_held_seq;
    out->task_arm_older    = g_task_arm_older;
    out->task_arm_row_wait = g_task_arm_row_wait;
    out->task_arm_no_row   = g_task_arm_no_row;
    /* The task arm's step count, partitioned by the reason the arm was reached. Not a tautology: the halves are
       raised inside the arm at the choice and the total at the convergence point after flow_step returns, so a
       second assignment of STEP_UNIT_RUN_TASK, or a return added between the choice and that point, breaks it. */
    DCHECKF(out->task_arm_older + out->task_arm_row_wait + out->task_arm_no_row ==
            g_step_unit_runs[STEP_UNIT_RUN_TASK],
            "the task arm ran %ld times and its three reasons account for %ld — every run is an older queued task, "
            "a row at the cursor still waiting, or no row, so STEP_UNIT_RUN_TASK has a second writer or a return "
            "separates the reason from the record",
            g_step_unit_runs[STEP_UNIT_RUN_TASK],
            out->task_arm_older + out->task_arm_row_wait + out->task_arm_no_row);
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

    DCHECK(out != NULL, "the lifetime step histogram was asked for into nothing");
    out->steps = g_steps;
    out->unframed_steps = g_unframed_steps;
    /* And what they cost, in the same reading as the count they are divided by. */
    out->step_us = g_step_us;
    out->step_work = g_step_work;
    out->service_work = g_service_work;
    DCHECKF(g_step_work <= g_service_work,
            "the turns' aging charges (%lld units) exceed the work the instance banked (%lld) — a charge is a "
            "difference of the banked counter across one step, so a larger sum charged work nobody banked",
            (long long)g_step_work, (long long)g_service_work);
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
           "the step census was composed inside an open slice — `betweenSlicesUs` closes from the last slice's "
           "return, so the running slice would be charged to the host's half");
    out->loop_us           = g_loop_us;
    out->between_slices_us = g_between_us + (g_slice_exit_set ? now - g_slice_exit_us : 0);
    out->slices            = g_slices;
    out->pick_digest       = (int64_t)(g_pick_digest & ((1ULL << 52) - 1));
    /* The containment, an identity rather than an expectation: `g_instance_us0` is opened at engine_sched_slice's
       entry from the reading the first turn's `t0` is carried from, and every charge in `g_step_us` is a
       sub-interval closed before this line reads the clock. A total above its span is a baseline taken after a
       turn was charged, a second writer, or a measure no longer monotone, and would publish `stepUs/instanceUs`
       above 1. */
    DCHECKF(out->step_us <= out->instance_us,
            "solver/engine.c: the dispatch loop's total %lld exceeds the %lld the instance has measured since its "
            "first slice — every turn's charge is a sub-interval of that span",
            (long long)out->step_us, (long long)out->instance_us);
    /* The partition of that span (solver/engine.h's `loop_us` has why these are two rows). The readings telescope:
       the wrapper's entry reading opens the baseline and each slice's closing reading is the next gap's opening, so
       their sum plus the open tail is `now - g_instance_us0` by construction. A difference is a slice whose span
       was never charged (an exit that escaped the wrapper), a second writer of an accumulator, or a non-monotone
       measure. */
    DCHECKF(out->loop_us + out->between_slices_us == out->instance_us,
            "solver/engine.c: the instance's span is not partitioned by its two halves (%lld in slices + %lld "
            "between them against %lld) — the readings telescope from engine_sched_step's own pair",
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
            "solver/engine.c: %ld dispatch turn(s) were taken across zero slices — a turn is taken only inside "
            "engine_sched_step's body",
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
    out->slice_overrun_gap_work = g_over_gap_work;
    /* The unit boundary's refusal arms, read with the `steps` they partition. */
    out->unit_mid_program     = g_unit_mid_program;
    out->unit_parked          = g_unit_parked;
    out->unit_checkpoint_owed = g_unit_checkpoint_owed;
    /* A count of turns cannot exceed the turns: a subset larger than its population is the free arithmetic tell,
       and a fraction whose numerator and denominator advance at different events is unbounded and reads as a rate. */
    DCHECKF(out->slice_overruns <= out->steps,
            "solver/engine.c: slice_overruns %lld exceeds steps %lld — an overrun is counted once per turn from the "
            "slice arm's own clock readings",
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
            "solver/engine.c: classic_compile_overruns %ld exceeds the %ld compile stints it is drawn from (%ld "
            "finished + %ld yielded) — every stint ends in one of those two arms, so the compile block has a third "
            "exit",
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
            "solver/engine.c: classic_compile_resumed %ld exceeds the %ld stints that parked a classic parse — only "
            "the classic park arm leaves a carrier for this row, so a third site installed one",
            out->classic_compile_resumed, out->arms[STEP_UNIT_COMPILE_YIELDED]);
    /* The cross-row containment, tying the phase to its turn. A compile stint that met the slice sits inside a step
       at least as long, and the step's reading brackets flow_step from outside (one caller; the slice accounting
       after it is unconditional, with no `continue`, `break`, `return` or `goto` between), so every compile
       overrun is a slice overrun. The numerator is per stint, so this also needs a turn to hold at most one stint,
       which it does: flow_step's `continue`s are the microtask and task arms, both above the script row, and every
       exit at or past the compile returns. A `continue` below the script row would break this with nothing else
       changing. */
    DCHECKF(out->classic_compile_overruns <= out->slice_overruns,
            "solver/engine.c: classic_compile_overruns %ld exceeds slice_overruns %lld — a stint that met the slice "
            "is inside a step that met it, so flow_step gained a caller outside the clock bracket or a turn took two "
            "compile stints",
            out->classic_compile_overruns, (long long)out->slice_overruns);
    /* The repeat partition's two containments: two independent partitions of one population (a parse can be in
       both), so two checks rather than one sum. Both subsets are raised in the same straight-line block as their
       total, with no branch between, so a violation is a raise moved out from under `g_classic_compiles` or behind
       a condition. */
    DCHECKF(out->classic_compile_again <= out->classic_compiles,
            "solver/engine.c: classic_compile_again %ld exceeds classic_compiles %ld — the repeat count is raised in "
            "the same block as its total",
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
            "solver/engine.c: %ld classic program(s) were started from a shared parse while no parse finished — a "
            "body holds a closure only from a finished parse",
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
       which flow_step reaches only from inside the `if (!flow_frame(f))` block the descents are counted at, both per
       pass, so an ask with no descent is the seed reached from elsewhere (a second caller, or the rung hoisted out
       of the block). A floor across a restart, not an equality: engine_session_close releases `g_orphan_asks` with
       the agent and nothing here, so a second session starts the ask at 0 against a climbing descent count. */
    DCHECKF(g_orphan_asks <= g_unframed_steps,
            "solver/engine.c: the orphan surface was asked %ld time(s) against %ld ladder descent(s) — "
            "engine_orphan_seed is reached only inside flow_step's `if (!flow_frame(f))` block, so a second path reaches "
            "the seed",
            g_orphan_asks, g_unframed_steps);
    /* And the ask's exits partition it (see the rows' declaration). engine_orphan_seed raises the total at its
       entry past the forking gate and exactly one of the three at each exit, so an ask outside the sum is an exit
       that records nothing: an early return between the ask and the take, a second caller reaching the walk, or a
       new memo arm. That is how the residue a reader computes against solver/step_unit.h's two orphan arms could
       widen silently. An equality, unlike the floor above, because all four are released together at
       engine_session_close. */
    DCHECKF(g_orphan_asks == g_orphan_asks_memo + g_orphan_asks_empty + g_orphan_asks_took,
            "solver/engine.c: the orphan surface was asked %ld time(s) and its exits account for %ld (%ld memo, %ld "
            "empty walk, %ld take) — each exit records itself, so one exit records nothing",
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
     * The thirteen units at or below the boundary (RENDERING, TIMER, IDLE_PERIOD, ORPHAN_SEED, ORPHAN_ROUTE,
     * AWAIT_FETCH_RECORD, AWAIT_OWED_REPLY, AWAIT_DECLINED, USER_SCROLL, CLOSE_REQUEST, OPEN_AGAIN, AWAIT_PEER,
     * FINISHED) are each written at one site at or below it, so their sum is the dispatches that descended this
     * far, which `g_clock_render_asks` counts at the arm; the third assert below is that agreement. `finished` at zero with the sum at zero says
     * the retirement arm was never asked (a fact about the arms above); with the sum large, it was asked and
     * something below declined.
     * Named residual. Not covered: the thirteen-unit list is written here by hand, not derived from the ladder.
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
       thirteen rows rather than thirteen containments, because the claim is the partition. ORPHAN_RESUME is absent:
       engine_orphan_seed returns SEEDED or ROUTED, never RESUMED, so the third outcome of `engine_orphan_unit`
       belongs to the rung above the boundary. */
    {
        long below = g_step_unit_runs[STEP_UNIT_RENDERING] + g_step_unit_runs[STEP_UNIT_TIMER] +
                     g_step_unit_runs[STEP_UNIT_IDLE_PERIOD] + g_step_unit_runs[STEP_UNIT_ORPHAN_SEED] +
                     g_step_unit_runs[STEP_UNIT_ORPHAN_ROUTE] + g_step_unit_runs[STEP_UNIT_AWAIT_FETCH_RECORD] +
                     g_step_unit_runs[STEP_UNIT_AWAIT_OWED_REPLY] + g_step_unit_runs[STEP_UNIT_AWAIT_DECLINED] +
                     g_step_unit_runs[STEP_UNIT_USER_SCROLL] +
                     g_step_unit_runs[STEP_UNIT_CLOSE_REQUEST] + g_step_unit_runs[STEP_UNIT_OPEN_AGAIN] +
                     g_step_unit_runs[STEP_UNIT_AWAIT_PEER] +
                     g_step_unit_runs[STEP_UNIT_FINISHED];
        DCHECKF(below == g_clock_render_asks,
                "the clock boundary was reached %ld times and the thirteen arms at or below it account for %ld — "
                "every dispatch reaching the rendering rung leaves through one of them, so a unit crossed the boundary, is "
                "written on both sides of it, or the rung is asked twice per step",
                g_clock_render_asks, below);
    }
    /* The clock chain's containment in the ladder descents, which a reader comparing
       `stepReachedRenderingLife` with `unframedStepsLifetime` on one census line depends on. The same argument as
       `g_orphan_asks <= g_unframed_steps`, one rung down: both are raised per pass through the `if (!flow_frame(f))`
       block, the rendering rung an `else if` inside it. A floor, and the gap is the reading: every arm above the
       rendering rung that takes a descent never reaches the chain, so descents with all three asks at 0 say the
       ladder stopped above the clock (read off `arms`). Both are per-instance and released by nothing, so a
       restart does not weaken it. */
    DCHECKF(g_clock_render_asks <= g_unframed_steps,
            "solver/engine.c: the clock boundary was reached %ld time(s) against %ld ladder descent(s) — the "
            "rendering rung is inside the `if (!flow_frame(f))` block the descents are counted in",
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

/* The scheduler events this engine has performed: forks taken, flows created, jobs run, context switches. It is not
 * engine work (retired opcodes plus charged host work, `stepWork`), and a budget is never stated in it. One
 * definition for
 * three consumers: the seam verdict below measures one step's share of it (being descheduled cannot inflate any of
 * the four, since a thread that is not running performs none of them); the progress stream's cadence keys on it (a
 * cadence keyed on `g_switches` alone goes silent while one flow holds the thread, because nothing outranks it);
 * and a host reporting on its own run decides when to report from it (engine.h).
 *
 * The four are not one currency, so this total over a clock measures composition, never a rate: a turn that ends
 * a frame or drains microtasks costs orders of magnitude less than one that runs a program, so a run that mints
 * jobs scores far more events for the same thread time. Read the four addends, which the verdict line and `@HEVENTS`
 * print separately; the sum is right for a cadence, which wants a quantity that always moves.
 * Named residual. Not covered: what a turn of each kind costs. Next diff builds: a census row per turn kind with
 * its cost. Absence shows as: `schedEvents` per CPU-second splitting runs at one rlimit into two bands while the
 * count of program turns overlaps between them. */
long engine_sched_events(void) {
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
    thread_work_note_discarded(JS_FlowDiscardRetiredWork());
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
           "a flow that never ran carries a recorded decision path — decide_enter would ignore it and leak its "
           "segment; whatever installed it set `dec_blob` without `started`");
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
    if (f->typing_due) {
        f->typing_due = 0;
        engine_orphan_type(ctx, f->fn, f->orphan_hash, f->typing_pos);
    }
    /* What this flow was ranked on when it took the thread: the pick compared it against every runnable member and
       found none strictly better, so from here the value yield may fire only if one of these moves. The hook's
       assertion reads them; nothing decides from them. */
    g_ranked_gen = flow_frontier_gen(); g_ranked_val = flow_reward(f);   /* the reward term, the family's */
    g_ranked_silence = flow_silence_notch(f);  /* the aging term — see the assertion in preempt_hook */
    g_ranked_opt_den = flow_optimism_den(f);   /* …and the optimism term's, which is a count and not a clock */
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
    DCHECK(!JS_HasParkedFlow(JS_GetRuntime(ctx)) && !flow_holds_parks(f),
           "a flow finished with a continuation still parked — that flow's async activation is dropped");
    /* A finished flow has no live frame and no program still being parsed. `frame` is the JS_FlowNew handle
       holding this flow's heap frame chain (every activation, closure and local it is suspended across), so one
       left at finish retains the whole execution graph, which the runtime's leak walk reports as thousands of
       anonymous Functions. A suspended compile holds no frame, so the frame assert alone would pass while the flow
       walked off with a half-parsed program, its descent chunks and its JSFunctionDef chain; the compile arm
       returns the moment a parse parks, so a flow here holding one means a completion arm ran while a parse was in
       flight. Asserted rather than freed, since freeing would hide that the finish path ran on a suspended flow. */
    DCHECK(f->compile == NULL,
           "a flow finished with a program still being parsed — a completion arm ran for an unreached row, or the "
           "compile handle outlived its program");
    DCHECK(!flow_holds_frame(f), "a flow finished with a live preemptible frame — its whole activation chain, and "
                             "everything those frames close over, is retained by a handle nothing will free");
    DCHECK(flow_deliver_pending(f) == 0,
           "a flow finished still holding routed deliveries — each is a message a peer sent and this document "
           "never received, and a page that never receives one is indistinguishable from a page that "
           "registered no handler");
    /* The same for an operation, at both ends: a record never turned into a program is a question nobody
       performed, and a token never spent is a peer's flow parked at the line that asked, for ever. */
    DCHECK(!flow_owes_answer(f),
           "a flow finished holding a cross-agent operation — the record was never performed or its completion never "
           "sent, so the asking flow waits for nothing (the queue rows hold started operations' tokens)");
    /* The job queue and the pending register are empty here, asserted rather than cleaned up: a walk that frees
       them here could run only when a work item is being dropped, and would hide the drop. flow_step decides
       "finished" only after offering the job queue a turn (the checkpoint before every program, the task arm for
       the rest) and only with the register empty (an entry with a value drains, one without reports host-owed).
       The release below frees both because an evicted flow legitimately holds them (the cold tier's recipe
       re-enqueues the reactions and re-issues the requests as it replays), so the assertion stands here, where
       "finished" is claimed. */
    DCHECK(flow_jobs_held(f) == 0, "a finishing flow still held queued jobs — its promise reactions and timer callbacks "
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
           "the interpreter asked for a branch arm while another snapshot-owning ask was open — the decision seam "
           "runs no page code, so something re-entered it");
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
           "the step driver asked for an outcome arm while another snapshot-owning ask was open");
    DCHECK(site != NULL,
           "an outcome ask arrived with no site — quickjs.h requires the pair captured at the ask with "
           "JS_FLOW_SITE_HERE");
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
static void engine_work_hook(JSContext *ctx, uint64_t units) { engine_work_bank(ctx, units); }
/* JSFlowControlHooks.budget: is this slice spent, asked from the interpreter's dispatch every
   ENGINE_QUANTUM_ASK_EVERY dispatches so the budget can expire inside bytecode that raises nothing. It is the same
   question clause (2) of preempt_hook asks: one budget with three consumers (a flow that parks on it, a step that
   returns the thread on it, a dispatch that asks for the park to be requested). The raise is answered by
   preempt_hook as a back-edge's is, with the rank comparison in front of it, so asking earlier drops, skips or
   reorders nothing. engine_quantum_spent asserts an open slice, so a host-time entry into the interpreter names
   itself there. */
static int engine_budget_hook(void) { return engine_quantum_spent() || quantum_host_wants_thread(); }
static const JSFlowControlHooks FC_EXPLORE = { .branch = engine_branch_hook, .outcome = engine_outcome_hook,
                                               .fork = engine_fork_finalize, .preempt = preempt_hook,
                                               .budget = engine_budget_hook,
                                               .budget_period = ENGINE_QUANTUM_ASK_EVERY,
                                               .work = engine_work_hook,
                                               .parse_units_per_byte = THREAD_WORK_UNITS_PER_BYTE_SCRIPT };
static const JSFlowControlHooks FC_VERIFY  = { .preempt = preempt_hook,      /* candidate re-fire: no fork, still preemptible */
                                               .budget = engine_budget_hook, /* and the same slice: a candidate
                                                                                holds the thread as any flow does */
                                               .budget_period = ENGINE_QUANTUM_ASK_EVERY,
                                               .work = engine_work_hook,     /* and a verify's clock still runs: a
                                                                                candidate re-fires as a flow, and a
                                                                                sink reading a duration must see the
                                                                                one the explore run saw */
                                               .parse_units_per_byte = THREAD_WORK_UNITS_PER_BYTE_SCRIPT };
static const JSFlowControlHooks FC_OFF     = { 0 };

/* The wall clock, which times nothing the scheduler decides on: the cooperative slice and the WFQ's aging charge
   are both engine work (engine_quantum_spent, flow_age_running), in one currency. What is left
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
#if APICLIENT_DEV
    /* The heap walk the census leaves out, once, at rest: its breakdown printed and the running counts checked. */
    {
        char *walk = result_heap_walk_json(g_sess_ctx);
        CHECK(walk != NULL, "engine: OOM composing the closing heap walk");
        printf("@HEAPWALK %s\n", walk);
        free(walk);
    }
#endif
    /* And the frontier is no longer for sale: past this line there is no scheduler to page for, and the teardown
       allocates (the result document, the leak walk), so an allocation there must fail as an allocation. */
    reclaim_uninstall(JS_GetRuntime(g_sess_ctx));
    /* And no scheduler pays an owed collection: the allocation that crosses the threshold runs it, and one standing
       now is finished by the next JS_RunGC. */
    JS_SetGCDriven(JS_GetRuntime(g_sess_ctx), false);
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
    DCHECK(g_sess_live, "a park was requested of an engine with no live session — the host would store an empty "
                        "residue over a real one");
    g_park_req = 1;
}

int engine_frontier_paged(void) { return g_parked; }

/* Whether the park at the drained-but-owed exit was the session's only writer (engine_park_why). */
static int g_park_refused;

const char *engine_park_why(void) {
    DCHECK(!g_park_refused || cold_park_records() > 0,
           "the session says its residue was written for refusals and the park document holds no record");
    if (g_park_refused) return "refused";
    return cold_park_records() > 0 ? "requested" : "none";
}

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
    /* And a peer's message already turned into a task goes back onto its delivery queue, the one queue a park
       writes, so the resumed session delivers it by that queue's order (flow_job_retract_external). */
    for (int i = 0; flow_at(i) != NULL; i++) flow_job_retract_external(ctx, flow_at(i));
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

/* The flow being switched in, while its delta is applied: not yet the running flow (flow_set_running comes after
   the apply), and the one member a sale may not take, since its chain is half-installed in the heap. */
static const Flow *g_reclaim_keep;

/* The pager's owed records: flows sold for their bulk (engine_park_fund) while another flow's record was being
   written, each written after it, newest first. Live only inside one sale (`g_sale_open`). */
static Flow *g_sale_owed;
static long g_sale_owed_n;
static int g_sale_open;
static long g_park_funded;   /* lifetime: flows sold for their bulk to fund another's record (engine_park_fund) */

/* The first half of a sale: the flow leaves every query and gives back what its record does not read. */
static void engine_sale_release(Flow *tail) {
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
    engine_retract_flow(g_sess_ctx, tail);
    flow_job_retract_external(g_sess_ctx, tail);
    flow_withdraw_async_sends(g_sess_ctx, tail);
    flow_release_bulk(g_sess_ctx, tail);
}

/* The second half: the record, funded by the bulk already given back, then the rest of the flow. */
static void engine_sale_write(Flow *tail) {
    /* Which population is leaving, read before flow_release frees the label: `cand_src` is the whole question
       (solver/engine.h). Per member, as at the retirement arm (engine.h). */
    const int was_cand = tail->cand_src != NULL;
    cold_park_flow(tail);
    flow_release(g_sess_ctx, tail);
    g_flows_sold++;
    if (was_cand) g_flows_sold_cands++; else g_flows_sold_flows++;
}

int engine_park_fund(void) {
    Flow *t;
    if (!g_sale_open) return 0;   /* a park outside a sale (the whole-frontier park) has no bulk to sell for it */
    t = flow_worst(g_reclaim_keep ? g_reclaim_keep : flow_running());
    if (!t) {
        /* The floor is reached only with no sellable bulk left: every member but the excluded one has given its
           bulk back, as the record being written or as an owed one. */
        DCHECKF(flow_count() <= g_sale_owed_n + 1 + (g_reclaim_keep || flow_running() ? 1 : 0),
                "the park document met the floor with %ld members and only %ld owed records — a member still "
                "holds bulk that no query offered for sale", (long)flow_count(), g_sale_owed_n);
        return 0;
    }
    engine_sale_release(t);
    g_park_funded++;
    t->sale_next = g_sale_owed;
    g_sale_owed = t;
    g_sale_owed_n++;
    return 1;
}

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
           "a reclaim was asked by a runtime that is not this session's — paging this frontier frees nothing for the "
           "other runtime");
    /* The running flow cannot go: its decision state is live in decide.c rather than in its blob and its delta is
       applied to the heap, which cold_park_flow and flow_release both refuse. Every other member is a snapshot. */
    DCHECK(!(g_reclaim_keep && flow_running()),
           "a reclaim found both a flow being switched in and a running flow — a switch-in follows the switch-out, so "
           "one of them is the flow this allocation is for and the other is stale");
    tail = flow_worst(g_reclaim_keep ? g_reclaim_keep : flow_running());
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
    DCHECK(tail != flow_running() && tail != g_reclaim_keep,
           "the pager chose the flow the scheduler is switched into — the allocation that asked for this "
           "memory is usually growing that flow's own delta or undo log, so selling it frees the buffer the "
           "caller is holding and is about to write through");
    /* And it is still in the registry, a separate fact with a separate failure: a non-member is a stale Flow* the
       frontier already removed, for which cold_park_flow would write a recipe and flow_release free a second time.
       Two asserts, since a conjunction reports either failure under one message. */
    DCHECK(flow_is_member(tail),
           "the pager chose a flow that is not in the frontier — flow_worst walks the registry, so this is a "
           "stale pointer, and both the park below it and the release after it would act on freed memory");
    /* Give back the bulk, write it down, then give back the rest: the record is written before the remainder it reads
       is freed, because the release makes the recipe the only copy, and after the bulk, so the bulk's memory funds the
       record at the floor. cold_park_flow appends one record to the session's park document (stored under this bundle's
       key and merged back by the next session); flow_release_bulk frees the suspended frame chain, heap delta, DOM
       head and the nodes it created, and walks the shared heap and document chains down only as far as the segments
       this flow held the last reference on. A tail flow's dying prefix cannot be a segment the running flow stands on
       (a shared segment has a second reference), so the release moves nothing in the live heap or document. */
    g_sale_open = 1;
    engine_sale_release(tail);
    engine_sale_write(tail);
    /* And the records of the flows this one's record sold for their bulk, each of which may sell more. */
    while (g_sale_owed) {
        Flow *o = g_sale_owed;
        g_sale_owed = o->sale_next;
        o->sale_next = NULL;
        g_sale_owed_n--;
        engine_sale_write(o);
    }
    DCHECK(g_sale_owed_n == 0, "the pager's owed-record count disagrees with its list once the list is empty");
    g_sale_open = 0;
    return 1;
}

/* See engine.h. The same edge the allocator's refusal takes (JS_ReclaimMemory's latch and collector guard, then
   engine_reclaim_tail), armed for this one ask: the caller is between steps and holds no position in the
   frontier, which is what the safepoint protects. */
int engine_reclaim_at_rest(void) {
    int prev, sold;
    if (!g_sess_live) return 0;   /* no frontier to sell: the floor, read at teardown */
    DCHECK(!g_reclaim_allowed,
           "a reclaim at rest was asked from inside a flow step — there the step's own allocation asks, armed, "
           "and this would re-arm a safepoint the step may have closed around a frontier walk");
    /* The flow left switched in between steps is the running flow, which flow_worst excludes: its delta is applied
       to the heap, so it is the one member a sale may not write down or release. */
    DCHECK(flow_running() == g_sess_cur,
           "a reclaim at rest found a switched-in flow that is not the running one — the sale would pick it as the "
           "tail and release a delta that is applied to the heap");
    prev = engine_reclaim_set(1);
    sold = JS_ReclaimMemory(JS_GetRuntime(g_sess_ctx), 0);
    engine_reclaim_set(prev);
    return sold;
}

void engine_set_ram_share(unsigned permille) {
    DCHECKF(permille >= 1 && permille <= 1000,
            "RAM pressure's share was set to %u permille — it is a share of the instance's limit, 1 to 1000",
            permille);
    g_ram_share = permille;
}

/* The ceiling an allocation meets, as this host states it, and its source for the census: the wasm build's maximum
   linear memory, or the native process's soft RLIMIT_AS (test_forced.c's native_ceiling_arm sets it to the
   vehicle's address space). A process with no address-space limit has none, where an allocation does not fail and
   pressure has nothing to be a share of. */
/* With it, the part of the limit no heap block can use: in wasm the static data and stack below `__heap_base`; under
   an address-space rlimit the process's mapped size less the allocator's footprint at this instant (the binary, the
   stacks, every other mapping). */
static void engine_ram_limit_read(void) {
#if defined(__EMSCRIPTEN__)
    extern unsigned char __heap_base;
    g_ram_limit = (uint64_t)emscripten_get_heap_max();
    g_ram_limit_source = "{\"wasm-heap-max\":1}";
    g_ram_nonheap = (uint64_t)(uintptr_t)&__heap_base;
#else
    struct rlimit rl;
    unsigned long pages = 0;
    FILE *statm;
    CHECK(getrlimit(RLIMIT_AS, &rl) == 0, "the native host could not read its address-space limit");
    g_ram_limit = rl.rlim_cur == RLIM_INFINITY ? 0 : (uint64_t)rl.rlim_cur;
    g_ram_limit_source = g_ram_limit ? "{\"rlimit-as\":1}" : "{\"none\":1}";
    statm = fopen("/proc/self/statm", "r");
    CHECK(statm && fscanf(statm, "%lu", &pages) == 1,
          "the native host could not read its mapped size from /proc/self/statm, which RAM pressure's line subtracts");
    fclose(statm);
    g_ram_nonheap = (uint64_t)pages * (uint64_t)sysconf(_SC_PAGESIZE) - (uint64_t)c_alloc_footprint();
#endif
    CHECK(g_ram_limit == 0 || g_ram_nonheap < g_ram_limit,
          "the instance's non-heap mappings already fill its memory limit, so no heap block could be allocated");
}

/* The live bytes the current episode of pressure sells down to, UINT64_MAX when no episode is open. An episode
   outlives the step that opened it, because each sale waits for a collection to finish first. */
static uint64_t g_pressure_target = UINT64_MAX;

/* RAM pressure relocates the cold tail (engine.h): sell at rest while the allocator's footprint stands above the
   share of what the limit leaves the heap, so the frontier gives memory back before an allocation that does not
   ask the refusal edge meets the wall. It ends at the share or at the frontier's floor (engine_reclaim_at_rest's
   0), never at a count. A sale frees chunks inside the footprint and the footprint does not give them back
   (dlmalloc never trims it; glibc trims only its top), so it cannot be brought back under a line it crossed: what
   a sale buys is free chunks later allocations reuse. So the footprint's growth past the line, or past the mark
   where pressure last made room, opens an episode that takes live bytes down by that growth, and the mark moves
   to the footprint. If the freed chunks cannot be reused the footprint grows again and the next step sells again.
   A collection comes between batches of sales: what a sale gives back is mostly cycles (a child realm is one, held
   by its own intrinsics), which only the cycle collector frees, and the allocation threshold that would owe one may
   stand above the share. So pressure sells a batch, owes a collection and returns; the steps that follow are the
   collector's (engine_collect_slice), and pressure reads the live bytes again only once it has finished. The batch
   is the scheduler's rest granularity, in members: it doubles each time a full batch and its collection left the
   episode unmet, and it starts again at one member when an episode closes. */
static void engine_relocate_under_pressure(JSRuntime *rt) {
    uint64_t line, ref, fp, live;
    long sold = 0;
    bool at_floor = false;
    JSGCCycleStats st;

    if (g_ram_limit == 0) return;
    line = (g_ram_limit - g_ram_nonheap) / 1000 * g_ram_share;
    fp = (uint64_t)c_alloc_footprint();
    ref = line > g_ram_fp_mark ? line : g_ram_fp_mark;
    if (fp > ref) {
        uint64_t t;
        live = (uint64_t)c_alloc_live();
        t = live > fp - ref ? live - (fp - ref) : 0;
        if (t < g_pressure_target) g_pressure_target = t;
        g_pressure_frag = fp > live ? fp - live : 0;
        g_ram_fp_mark = fp;
    }
    if ((uint64_t)c_alloc_live() <= g_pressure_target) {
        /* No episode, or the open one is met. */
        g_pressure_target = UINT64_MAX;
        g_pressure_batch = 1;
        g_pressure_batch_full = false;
        return;
    }
    JS_GCCycleStats(rt, &st);
    if (st.collections == g_pressure_sold_at_collections || g_pressure_sold_at_collections == UINT64_MAX) {
        /* The session's first pressure, or a batch whose collection has not finished: collect before selling. */
        g_pressure_sold_at_collections = st.collections;
        JS_GCOwe(rt);
        g_pressure_collects++;
        return;
    }
    if (g_pressure_batch_full)
        g_pressure_batch *= 2;
    while (sold < g_pressure_batch && (uint64_t)c_alloc_live() > g_pressure_target) {
        if (!engine_reclaim_at_rest()) { at_floor = true; break; }
        sold++;
    }
    g_pressure_sold += sold;
    g_pressure_batch_full = sold == g_pressure_batch;
    if (at_floor) {
        /* The frontier's floor closes the episode: nothing is left to sell. */
        g_pressure_target = UINT64_MAX;
        g_pressure_batch = 1;
        g_pressure_batch_full = false;
    }
    if (sold == 0) return;
    g_pressure_sold_at_collections = st.collections;
    JS_GCOwe(rt);
    g_pressure_collects++;
}

/* See engine.h. The probe is a plain allocation, freed at once: what it proves is that the address space holds
   `bytes` now, which is what the composition after it spends. */
int engine_headroom_at_rest(size_t bytes) {
    for (;;) {
        void *p;
        if (bytes == 0) return 1;
        p = malloc(bytes);   /* a probe: the sale below is the one this loop makes, at rest */
        if (p) { free(p); return 1; }
        if (!engine_reclaim_at_rest()) {
            /* The floor: the composition's own CHECK answers. Reached only with nothing left to sell, so a frontier
               still holding members other than the running one means the probe asked for what no sale can give. */
            DCHECKF(flow_count() <= 1,
                    "headroom for %zu bytes met the floor with %d members still in the frontier (live %zu, footprint "
                    "%zu) — a sale was refused, or the probe asks for one block the freed memory cannot form",
                    bytes, flow_count(), c_alloc_live(), c_alloc_footprint());
            return 0;
        }
    }
}

void engine_note_rest_composition(long attempts) {
    DCHECK(attempts >= 1, "a composition between steps was noted with no attempt at all");
    g_rest_compositions++;
    g_rest_compose_attempts += attempts;
    if (attempts > g_rest_compose_attempts_max) g_rest_compose_attempts_max = attempts;
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
    DCHECK(g_arrival.prov != -1,
           "a session began on an instance whose host never stated how its document was reached — every "
           "timeline's grade starts from it, so call engine_set_arrival (the ABI's qjs_set_arrival) first, with "
           "the trusted zone's reach grade or `observed` for a document a person named");
    /* The document install's own work is the first flow's. §4.8.5's insertion steps run for an <iframe src> in
       the initial markup when the host installs the document, before this session exists, and queue §7.4 step
       14's navigation through JS_EnqueueCallTask; with no enqueue hook installed that is quickjs's baseline list,
       which the first executing flow adopts (js_adopt_baseline_calls), so the frame loads in the boot timeline.
       What this asserts is the other population: a job on quickjs's own job or task list, which only a raw
       JS_EnqueueJob before the hook can put there and nothing in this engine drains. */
    DCHECK(!JS_IsJobPending(JS_GetRuntime(ctx)),
           "a job was on quickjs's own job or task list before this session began — user agent callbacks wait on the "
           "baseline list, so this is an ownerless JS_EnqueueJob");
    g_sess_ctx = ctx;
    g_sess_cur = NULL; g_sess_live = 1;
    /* The three arrays are one inventory, so a session opened with a length and two of them would have the seed
       read the third off the end. Asserted where the borrow is taken, while its builder is still on the stack. */
    DCHECK(n == 0 || (bodies != NULL && srcs != NULL && types != NULL),
           "a session was opened over a script inventory missing one of its three columns (bodies, srcs, types)");
    /* A list of `<script>` elements carries their §4.12.1.1 schedules (core/loader/document_scripts.h's
       `sched`); only a host's synthesized program list, which names no element, has none and runs in its own
       order. */
    DCHECK(n == 0 || els == NULL || sched != NULL,
           "a session was opened over `<script>` elements without their §4.12.1.1 schedules — document_exec_scripts "
           "computes that column");
    /* The document this session is of: the instance's root document, whose script sequence the session runs.
       Asserted against the realm, because everything below compiles by asking the document for its realm, and a
       ctx of another document's realm would run this document's scripts somewhere else. */
    g_sess_doc = world_local_doc();
    DCHECK(doc_realm(g_sess_doc) == ctx,
           "a session was opened with a realm that is not the root document's");
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
           "a session opened while the previous root script table was still allocated — engine_session_close is the "
           "one place it is freed");
    g_root_scripts = n ? (RootScript *)reclaim_calloc((size_t)n, sizeof(RootScript)) : NULL;
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
        DCHECK(script_type_is_sequenced(types[i]),
               "a row of the root document's script inventory holds a type no sequence holds — a set of "
               "speculation rules is registered rather than run and §4.12.1.1's null type runs nothing at all, "
               "so document_exec_scripts drops both before they reach a session");
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
           "a session began on an instance whose sold-flow debt is not zero — the debt crosses sessions with the "
           "host that was shown the requests, so carry it rather than reset it");
    /* Not reset, because they are the instance's totals rather than a session's, and a rate that restarted would
       be a level again (see g_host_asked): g_host_asked/g_host_answered/g_host_answers_extra/g_host_answers_late/
       g_host_answers_refused/g_host_terminated; g_reclaim_asks/g_reclaim_unarmed/g_reclaim_floor, which partition against g_flows_sold
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
    report_exception_set_console_hook(result_page_error_info);
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
           "alternative seedings");
    /* A residue refused whole for an older name spelling seeds the boot flow, as no residue does. */
    if (!(recipes && *recipes && cold_resume(ctx, recipes)))
        flow_add(ctx, JS_UNDEFINED, WORLD_NONE);   /* the first flow: the page's scripts, empty decision vector */
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
    engine_ram_limit_read();
    /* The cycle collector's steps are this scheduler's from here: the allocation threshold owes a collection and
       engine_sched_step pays it (engine_collect_slice). */
    JS_SetGCDriven(JS_GetRuntime(ctx), true);
    g_pressure_sold_at_collections = UINT64_MAX;   /* this session's first pressure collects before it sells */
    g_pressure_batch = 1;
    g_pressure_batch_full = false;
}

void engine_host_baseline(void) {
    DCHECK(g_sess_live, "the baseline was asked for outside a live session");
    DCHECK(cow_current() == NULL && JS_FlowGen() == 0,
           "the baseline was asked for while a slice is open — the held flow is switched out only across the "
           "host's own time, where the capture route is down and the stamp is zero");
    if (g_sess_cur) { flow_switch_out(g_sess_ctx, g_sess_cur); g_sess_cur = NULL; }
    /* The switch-out leaves the flow's shared base chain applied (cow_unapply moves only the head), so the heap
       still shows that flow's family's history; the baseline is the chain moved to none. The DOM's switch-out
       already restores its baseline document. */
    cow_install_baseline(g_sess_ctx);
    /* The slice re-opens with captures routed nowhere, as the first one does; the saved route was the delta
       just unapplied. */
    g_slice_delta = NULL;
}

/* A second document of this agent, appended to every timeline on the running frontier (engine.h).
 *
 * The agent has one event loop (HTML §8.1.7.1 "Definitions"), so a flow is a timeline of every document the
 * agent holds, and a Document the browser hands it arrives in all of them. A boot flow of its own would be a
 * second root world in this instance's forest, where world_vec_relate reads two roots as the two arms of one
 * branch: a peer that took an answer from one timeline would refuse every post and answer from the other, and
 * neither timeline would ever hold both documents' writes.
 * Named residual. Not covered: two orders the agent's event loop allows. A flow's sequence has one cursor, so
 * these rows run after the flow's remaining rows of other documents, and a parser-blocking script of one
 * document holds another's; and a timeline minted from the baseline after this call (an @S candidate session,
 * witness_verify's run) is seeded with the root's rows only. Next diff builds: a cursor per document in the flow's
 * sequence, and the session's list of joins replayed onto every from-baseline timeline. Absence shows as: a
 * joined document's script running only once an earlier document's pending script has arrived, and a candidate
 * session in an agent holding a joined document never reaching a sink in that document's code. */
void engine_join_document(JSContext *cctx, uint32_t doc, char **bodies, char **srcs,
                          const ScriptType *types, const ScriptSchedule *sched, lxb_dom_element_t **els,
                          int n, EngineArrival arrival) {
    RootScript *rows;
    int i, rown = 0, flows = flow_count();

    arrival_check(arrival, "a joined document's arrival");
    DCHECK(g_sess_live, "a document was joined to an instance with no live session");
    DCHECK(cctx != NULL, "a document was joined with no realm to compile its programs in");
    DCHECK(doc != 0, "a document was joined naming no document (the world registry's NONE)");
    DCHECK(doc != g_sess_doc,
           "the session's own document was joined to its agent a second time — its scripts are already the "
           "frontier's");
    DCHECK(doc_realm(doc) == cctx,
           "a document was joined naming a realm that is not the one it answers with");
    DCHECK(n == 0 || (bodies != NULL && srcs != NULL && types != NULL && sched != NULL && els != NULL),
           "a document was joined with a script inventory missing one of its five columns (bodies, srcs, types, "
           "sched, els)");

    /* A referenced instance keeps its last timeline resting rather than finishing (engine_set_referenced), and
       the zone joins only into an instance a peer references, so an empty frontier is a join with no timeline to
       arrive in. */
    DCHECK(flows > 0,
           "a document was joined to an agent with no timeline on its frontier — the trusted zone joins only into "
           "an instance a peer references, whose last timeline rests instead of finishing");
    /* The same table shape as the root document's seed, built here because §4.12.1.1's encoding-parse belongs to
       this document: §8.1.3.2 "Environment settings objects"' API base URL is the joined document's, so
       `<script src=app.js>` in a document at `/app/child.html` is `/app/app.js`. `cctx`, the joined document's
       realm, is what the parse is asked of; the session's root realm would fetch a joined document's bundle from
       another document's directory. One row shape, one seeding function, one set of asserts. */
    rows = n ? (RootScript *)reclaim_calloc((size_t)n, sizeof(RootScript)) : NULL;
    CHECK(n == 0 || rows != NULL,
          "engine: OOM building a joined document's script table — without it the document is parsed, given a "
          "realm and never executes a line, which is indistinguishable from a document that had no scripts");
    for (i = 0; i < n; i++) {
        DCHECK(script_type_is_sequenced(types[i]),
               "a joined document's script inventory holds a row of a type no sequence holds — "
               "document_exec_scripts drops speculation rules and the null type before they become rows");
        DCHECK(bodies[i] != NULL || srcs[i] != NULL,
               "a joined document's script inventory holds an entry with neither source text nor an address — "
               "§4.12.1.1's \"if url is failure … fire an event named error at el, and return\" means such an "
               "element runs no script and takes no position, so a row that is nothing is one every flow "
               "would park on and never fill");
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
           flows hold it and however many arms they fork. The length is the `strlen` for the root document's reason: an inline
           `<script>`'s text off a parsed tree, in which HTML §13.2.5.4 "Script data state" has turned a U+0000
           into U+FFFD (engine_sched_begin). */
        if (bodies[i]) {
            rows[rown].body = dyn_body_new(bodies[i], strlen(bodies[i]));
            CHECK(rows[rown].body,
                  "engine: OOM copying a joined document's program into its script table");
        }
        rown++;
    }
    /* Each flow takes the navigation's marks, as a timeline holding a peer's answer takes the asker's, and is
       askable again: a resting timeline now has rows to run. engine_seed_scripts lays the document's rows after
       the flow's own, in the document's order. */
    for (i = 0; i < flows; i++) {
        Flow *f = flow_at(i);

        flow_take_peer_marks(f, arrival_marks(arrival));
        engine_seed_scripts(f, doc, rows, rown);
        flow_clear_host_owed(f);
    }
    /* The table is this call's, not the flows': engine_queue_into copies the address into each flow's own row and
       takes a reference on the body, so both are given back here, each body surviving as long as some flow holds
       the row that names it. The root document's table outlives its call because it seeds a flow every time one
       is created; this one is laid onto the flows standing now. */
    for (i = 0; i < rown; i++) {
        free(rows[i].url);
        if (rows[i].body) dyn_body_unref(rows[i].body);
    }
    free(rows);
}

void engine_set_referenced(int referenced) {
    /* A reference is never given back: a peer that holds a WindowProxy for this document holds it for as long as
       the peer exists, and nothing tells this instance when that ends. Raising the flag makes no resting flow
       askable, so there is no host-owed mark to clear. */
    DCHECKF(referenced == 0 || referenced == 1, "a peer reference of %d — the flag has two values", referenced);
    DCHECKF(referenced >= g_referenced,
            "the peer-reference flag was lowered from %d to %d — it is raised by the provisioning host and by "
            "this document creating a navigable another agent hosts, and neither fact can be retracted",
            g_referenced, referenced);
    g_referenced = referenced;
}

int engine_referenced(void) { return g_referenced; }

/* Whether the scheduler could hand this frontier the thread now (engine.h). The pick is the scheduler's own, so the
   answer is "a member can convert a slice into work": flow_best would answer over members that have told the
   scheduler they cannot run, and a frontier whose every member is host-owed burns no CPU. */
int engine_runnable(void) {
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
           "this engine reported that nobody can take the thread while the host owes it nothing, no peer "
           "references it and no member is parked on a refusal — every member's mark rests on an answered register "
           "entry, which no host event can clear");
    return b != NULL;
}

int64_t engine_service_work(void) { return g_service_work; }
int64_t engine_work_retired(void) { return g_service_work + (int64_t)JS_FlowPeekRetiredWork(); }

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
    /* One reading of the pricing clock per iteration, carried: it closes the turn just run and opens the next, so
       the step census (`g_step_us` and its halves) telescopes across the slice. It prices the engine's own
       overhead on this host and decides nothing; the aging charge is the step's work. */
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
           "a slice was opened on a session that is no longer live — the previous step answered ENGINE_STEP_DONE");
    /* The flow carried across a quantum boundary is still a member of the frontier. The cooperative yield resumes
       "the same top flow on the byte-identical frontier", and what makes that true here is a raw `Flow *` held in
       a static across a return to the host, during which the host routes records, provides replies and answers
       requests. If any of those removed a flow, this pointer would be read (`best != cur`, then flow_switch_out)
       after its free, a lifetime bug looking like a scheduling one. Asserted where the pointer is picked up. */
    DCHECK(cur == NULL || flow_is_member(cur),
           "the flow this quantum resumes is no longer in the frontier — it was removed while the host held the "
           "thread");
    /* The finish the previous slice deferred so the host could photograph this member's last moment (@PERWORLD;
       the argument is at `g_sess_finish_owed`). Above the park, the pick and every engine_session_close here, so
       the deferral is a split call rather than a second teardown path: whatever this slice does, the member that
       answered FLOW_STEP_DONE last time is finished first, by the two lines that would have finished it then. The
       member is still switched in, as the return left it: the host does not switch flows between the two (no ABI
       entry does), so the delta this unapplies is the one photographed. */
    if (g_sess_finish_owed) {
        DCHECK(cur != NULL,
               "a finish deferred for a picture has lost its member — this line completes it, and the member's COW "
               "and DOM deltas are still applied");
        DCHECK(!flow_paint_owed(cur),
               "a finish deferred for a picture belongs to a member that owes one again — the discharge and the "
               "deferral are written at one line");
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
    /* Nothing is re-admitted here. A slice that ended on its work quantum handed the thread to a host with nothing
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
        Flow *incumbent = cur;
        /* A new member is seeded with no member switched in (flow_seed): its parks read the baseline document and
           credit no account. The incumbent keeps its standing in the pick and is switched back in if it wins. */
        if (cur && (solve_seeds_owed() || witness_verify_seeds_owed())) {
            flow_switch_out(ctx, cur);
            solve_flow_begin(NULL);
            cur = NULL;
        }
        solve_seed_candidates(ctx);
        witness_verify_seed(ctx);   /* the same for an endpoint witness's verification (witness_verify.h) */
        /* The seeding allocates, and an allocation at the RAM floor sells the frontier's tail, which may be the
           incumbent switched out above. The pick reads its incumbent through its handle, so one that departed is
           not offered: an address walk, paid only on the turn that switched the incumbent out to seed. */
        if (incumbent && !cur && !flow_is_member(incumbent)) incumbent = NULL;
        /* WFQ: highest value of information among the members that can still make progress; a fresh fork (UCB)
           can preempt. The members waiting on the host are taken out of the pick only; they keep their weight,
           place and every work item (flow.h). The flow holding the thread is passed in, which makes this a
           schedule rather than a ranking: the incumbent is displaced only by a strictly better flow, the
           comparison the preempt hook makes, so the two ends of the decision cannot disagree and a field of
           equal-weight members (most frontiers) does not pay a COW delta swap per iteration. */
        Flow *best = flow_next_to_run(incumbent, FLOW_SCAN_NEXT);
        /* Nothing left to run: the frontier is empty, or every member is waiting on the host (the stall), decided
           by asking each member rather than by counting unproductive picks. */
        if (!best) break;
        if (best != cur) {                  /* context switch: swap COW delta + decision + pins */
            if (cur) flow_switch_out(ctx, cur);
            /* The switch-in allocates as it applies `best`'s delta (a slot a sibling deleted is re-created, a DOM
               node re-attached), and it holds no position in the frontier, only `best`: so the refusal edge is
               armed across it with `best` kept out of the sale. */
            {
                int prev_reclaim = engine_reclaim_set(1);
                g_reclaim_keep = best;
                flow_switch_in(ctx, best);
                g_reclaim_keep = NULL;
                engine_reclaim_set(prev_reclaim);
            }
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
            /* Aging is charged in engine work, not in steps: a step is one unit of the ladder, so a per-step
             * charge would bill a slice of execution the same as advancing one script index. The work is the
             * step's retired opcodes and regexp back-edges plus host work charged by its input, banked on the
             * instance's counter (engine_work_bank), so the charge is a function of the flow's path and
             * identical on every host and across a resume. A C activation that declares no step boundary and
             * charges nothing retires none of it; that population is closed by the step-machine conversion and
             * by charging host work by its input (core/timing/thread_work.h), never by a clock. */
            int64_t t0 = now;   /* this step's start: the previous iteration's reading, carried */
#if APICLIENT_DEV
            uint64_t pq0 = 0, pf0 = 0, pa0 = g_preempt_asked;
            /* The work this step performs, sampled beside the clocks (see the verdict below): forks, flows and jobs
               are things the engine did, which no descheduling can inflate. */
            long ev0 = engine_sched_events();
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
               frames, deltas). It closes after the microtask checkpoint's tail below, whose writes are the stepped
               flow's own. Restored rather than cleared, because the step can reach this loop again through a
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
            /* The step's opening on the work counter: the switch-in discarded what stood in the interpreter, so
               everything banked from here to the step's close is this flow's. */
            const int64_t step_work0 = g_service_work;
            /* The same bracket in the other currency, opened on the clock's line so the two readings are of one
               span: the overrun test below compares `now - t_slice0`, and this counts the suspend points offered
               across that interval. From here rather than the turn's `t0`, because the pick, switch and delta swap
               consult no policy. A load of a static in every build; see g_over_asks. */
            uint64_t pa_slice0 = g_preempt_asked;
            /* The gap census opens from the number the overrun test subtracts from, reset here rather than at
               `t0`, as `pa_slice0` is: a gap measured from the pick would include a span in which no offer was
               possible and read as a missing seam (solver/engine.h's `over_gap_arms`). */
            g_gap_max_work = 0; g_gap_last_work = step_work0;
            {
                uint64_t b = flow_birth_seq(cur);
                for (int i = 0; i < 8; i++) { g_pick_digest ^= (b >> (8 * i)) & 0xff; g_pick_digest *= 1099511628211ULL; }
            }
            int r = flow_step(ctx, cur);
            /* The work retired since the step's last poll is the step's own (core/timing/thread_work.h): banked on
               its clock here, while it is still the running flow, so a task's end moment does not depend on
               whether the next pick is this flow again. */
            if (flow_running() == cur) {
                uint64_t left = JS_FlowTakeRetiredWork();

                if (left != 0) engine_work_bank(ctx, left);
            }
            /* What this step retired, the aging charge and the overrun test's operand. */
            const int64_t step_work = g_service_work - step_work0;
            /* Where the step's answer goes onto the flow: the one point every arm of flow_step converges on, so a
               new arm cannot forget to be recorded. The arm declares its name; it is kept here, read by the `@COLD`
               histogram and the seamless-stretch aborts below, and decides nothing. flow_step resets to NONE at
               entry, so a step returning without naming its arm crashes here with the flow in hand rather than
               being attributed to the previous step (solver/step_unit.h). */
            DCHECK(g_step_unit != STEP_UNIT_NONE,
                   "a scheduler step returned without naming the arm it took — flow_step resets to NONE at entry and "
                   "every exit names itself");
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
                    "the lifetime step histogram does not account for every scheduler step (%ld arm counts against "
                    "%ld entries) — a second caller of flow_step, a return before this record, or a step outside "
                    "engine_sched_slice",
                    step_unit_runs_total(), g_steps);
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
                   "the flow the scheduler stepped is not the one holding the thread — the unit boundary, "
                   "checkpoint, notify, visit credit and aging charge below all read the switched-in flow");
            /* And which clause answered, recorded where it is decided: the same conjunction in the same order (a
               live frame, the runtime's parked slot, a checkpoint still owed), written as an if/else so the refusing
               clause has a name. Short-circuiting is preserved, so `JS_HasParkedFlow` is not asked of a member
               holding a frame, and `unit_ended` is derived from the ladder. One arm per turn, so a partition (see
               g_unit_mid_program). */
            int unit_ended;
            if (flow_frame(cur))                                { g_unit_mid_program++;     unit_ended = 0; }
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
            /* The checkpoint's tail is the stepped flow's own work, HTML §8.1.7.3 "perform a microtask checkpoint"
               step 4 and the steps after it: notify about rejected promises (§8.1.4.7) captures the realm's list
               into this flow's delta and queues its task on this flow, so its allocations may sell the tail like
               the step's. Nothing since flow_step holds a position in the frontier, and the sale never takes the
               running flow. */
            engine_reclaim_set(prev_reclaim);
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
            /* The other half, from the same two readings, never a third: `t_slice0` was taken just before the
               step and `now` just after, so this arm is the step alone and the two arms sum to the turn. */
            g_slice_us += now - t_slice0;
            /* Whether this turn overran, by the inequality engine_quantum_spent asks: a step that retired a slice's
               work met the budget between two of its asks, which a mean of turn costs cannot report. Counted and
               nothing else; no branch depends on it, since a per-turn overrun test is what a watchdog is built of. */
            if (step_work >= ENGINE_QUANTUM_WORK) {
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
                        "arm %d overran the slice %ld time(s) against %ld run(s) — run and overrun are counted from "
                        "one turn's arm, so `g_step_unit` was rewritten between them",
                        (int)g_step_unit, g_step_unit_over[g_step_unit], g_step_unit_runs[g_step_unit]);
                /* And whether the page's code offered a point in this turn, raised here where the turn is known to
                   be one of the population (solver/engine.h's `slice_overrun_asks`); `pa_slice0` was read on
                   `t_slice0`'s line, so both operands bracket the span judged above. A turn adds to the sum exactly
                   when it does not add to the seamless count, by the order of these two statements, which the
                   accessor asserts as an equivalence. The delta cannot be negative, since `g_preempt_asked`'s only
                   writer is preempt_hook's increment; a negative delta is a reset or a wrap. */
                DCHECKF(g_preempt_asked >= pa_slice0,
                        "the preempt policy reports fewer lifetime consultations after a step than before (%llu "
                        "against %llu) — preempt_hook's increment is the only writer",
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
                            "arm %d offered no suspend point in %ld overrunning turn(s) against %ld overrunning "
                            "turn(s) — both are counted from one turn's `g_step_unit`",
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
                            "the per-arm ask histogram does not account for every point offered by an overrunning "
                            "turn (%llu against %llu) — both are raised from one turn's arm and consultation delta",
                            (unsigned long long)step_unit_over_asks_total(),
                            (unsigned long long)g_over_asks);
                    /* And the widest stretch of work inside this turn with no point offered, which the sum above is
                       a proxy for (solver/engine.h's `over_gap_arms`). The tail is closed here: `g_service_work` is
                       the turn's closing reading, so its distance from `g_gap_last_work` is the stretch from the
                       last offer to the turn's end, which for a turn that consulted nine times early and then ran
                       on is the finding; `g_gap_max_work` alone would report the early stretches. Only for a non-seamless turn: a turn that offered no point has no
                       interval between offers, its whole duration is already its share of `g_slice_us`, counted by
                       `g_step_unit_over_seamless`, and one row holding a turn's worst gap and its length could not
                       be read for either. */
                    {
                        int64_t gap = g_service_work - g_gap_last_work > g_gap_max_work
                                      ? g_service_work - g_gap_last_work : g_gap_max_work;
                        /* The headroom is asked before the narrowing, for the ask row's reason against a
                           different horizon: this row is a maximum, so the `long` it must be overflows only for one
                           turn whose worst no-offer stretch exceeds two billion units on a 32-bit host, about 40000
                           slices with no point offered. */
                        DCHECK(gap <= (int64_t)LONG_MAX,
                               "one turn's widest no-suspend-point stretch does not fit a `long` (two billion units "
                               "in one turn) — the two readings belong to different counters, or the engine held the "
                               "thread that long without a point");
                        if (gap > g_over_gap_work) g_over_gap_work = gap;
                        if ((long)gap > g_step_unit_over_gap[g_step_unit])
                            g_step_unit_over_gap[g_step_unit] = (long)gap;
                        /* The identity is a max, not a sum: a sum of maxima is a quantity no turn produced, and the
                           arm row and its scalar are folds of one per-turn number over one population, so the
                           scalar is the arms' maximum. Asserted here, with the arm, the turn's readings and its
                           delta in hand. */
                        DCHECKF(step_unit_over_gap_max() == g_over_gap_work,
                                "the per-arm worst-gap row and its scalar disagree (%ld against %lld) — both are "
                                "folded as maxima from one turn's interval",
                                step_unit_over_gap_max(), (long long)g_over_gap_work);
                    }
                }
                DCHECKF(g_over_seamless <= g_slice_over,
                        "more overrunning turns offered no suspend point (%ld) than overran at all (%lld) — "
                        "both are raised inside this one branch from one turn, the total unconditionally and "
                        "the subset under a test of that turn's own consultation delta, so a subset larger "
                        "than its population is one of them being raised somewhere else",
                        g_over_seamless, (long long)g_slice_over);
            }
            /* What this turn cost on the pricing clock (see g_step_us) and what it retired, before the charge,
               because `flow_age_running` is the WFQ's policy and these are a census, which may not decide whether
               the policy ran. The headroom is asked before the add: a sum of non-negative readings can mislead
               only by overflowing, and the _Static_assert catches a narrowing at build time. */
            DCHECK(now - t0 <= INT64_MAX - g_step_us,
                   "the dispatch loop's pricing-clock total would overflow — the clock is no longer microseconds "
                   "or `step_us` was narrowed");
            g_step_us += now - t0;
            g_step_work += step_work;
            flow_age_running(step_work);
#if APICLIENT_DEV
            /* The WFQ's monopolizer half, asserted where the charge meets the pick. Its twin, the UCB bonus that
               keeps a never-served site from starving, is a floor on the weight flow_pick returns; flow_pick_walk's
               bound on the aging half is derived from flow_weight and catches only an edit to the formula. This is
               observability: whether retiring a whole quantum moves the flow's rank at the granularity the thread
               is handed out in, since the next statement of this loop is the pick. Under today's unit it is an
               identity: `flow_silence_notch` divides by the cooperative quantum and the charge adds the same units
               to both of its summands, so a full quantum moves it at least two notches. A charge under one quantum
               is exempt: it is below anything this scheduler decides. */
            DCHECK(step_work < ENGINE_QUANTUM_WORK || flow_silence_notch(cur) > age_notch0,
                   "a flow consumed a whole cooperative quantum and its rank did not move — the aging term is "
                   "quantised coarser than the thread is handed over");
#endif
            /* And the completed unit, an observation the census publishes (`visMin`/`visMax`): a visit is a
             * program that ended, a job, a delivery or a lifecycle stage, never a slice of thread time. The
             * predicate is HTML §8.1.4.4 "Calling scripts"'s, step 3 of clean up after running script, "if the
             * JavaScript execution context stack is now empty", read from the one `unit_ended` the checkpoint and
             * the notify read; the unit ends when the microtask queue does, and flow_credit_visit asserts this at
             * the origin. A finished flow is credited too: `r == FLOW_STEP_DONE` is the largest completed unit.
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
                    "the unit-of-work boundary's arms do not partition the dispatches (%ld ended + %ld mid-program + "
                    "%ld parked + %ld checkpoint-owed against %ld steps) — a clause was added with no arm, or a "
                    "dispatch happened between the two counts",
                    g_units_done, g_unit_mid_program, g_unit_parked, g_unit_checkpoint_owed, g_steps);
            /* The cooperative-quantum contract, asserted at its site. A flow_step must reach a suspend point (a
               bytecode back-edge where the preempt hook runs, a step machine's boundary) within the quantum, which
               is what makes the frontier parkable. A path with no suspend point stops the run: the slice's expiry
               is never observed and the engine spins with the switch count frozen. The preempt fired/requested
               ratio cannot catch it, since a pure C loop reaches no back-edge and requests nothing. Not a bound: it
               truncates nothing, drops no flow and is compiled out of release; it asserts the path has the seam the
               design requires and names the flow, with an enormous margin (400 slices) so anything under it is
               merely slow. */
#if APICLIENT_DEV
            {
                /* The wall clock's other end, taken once for the two reported numbers below; printed, never a
                   verdict. */
                int64_t done = engine_now_ms();
                int64_t spent = done - wall0_ms;
                (void)spent;
                /* The tail closes the last gap: a step that offers a point and then runs for seconds before
                   returning has that silence between its last offer and its end, and nothing else records it. */
                int64_t gap = done - g_last_ask > g_max_gap ? done - g_last_ask : g_max_gap;
                /* The verdict is work, not time; the clock is only printed. `asked == 0` says the path offered
                 * the scheduler no suspend point, but a wall clock cannot separate a stretch of C with no seam
                 * (which spins for ever with the switch count frozen) from a stretch that was descheduled: a
                 * descheduled thread reaches no preempt check either, so it shows `asked=0` too, and its wall gap
                 * is not its own. A false failure sends the next reader hunting a phantom. Forks, flows created and
                 * jobs run are work the engine performed, which descheduling cannot inflate, so a step that mints
                 * thousands of siblings without consulting the preempt hook has no seam on that path whatever the
                 * load. It truncates nothing: the engine reaching this margin is already one that never returns.
                 * The shape that reaches it is in the tree: an unknown-length walk over opaque input asks one
                 * outcome-fork question per position, and the driver re-enters the same step after each fork.
                 *
                 * The second verdict is the step's retired work, which no load can inflate either: an interpreted
                 * stretch cannot reach its margin (the dispatch-counted budget raises within a slice), so a step
                 * past it with no point offered is host work charged by its input in one span with no rest point.
                 * A C stretch that retires and charges nothing (a bare `for(;;);`) is invisible to both, and is
                 * closed by the step-machine conversion rather than by a clock. */
                long events = engine_sched_events() - ev0;
                if (step_work > ENGINE_SEAMLESS_STEP_WORK && g_preempt_asked == pa0) {
                    DFAILF("%lld units of retired work in one step with no suspend point offered (%ld forks+flows+"
                           "jobs) — this stretch has no suspend/resume seam; unit=%s script_i=%d",
                           (long long)step_work, events, step_unit_name(g_step_unit), cur ? cur->script_i : -1);
                }
                if (events > ENGINE_SEAMLESS_EVENTS && g_preempt_asked == pa0) {
                    /* The two excerpts are the only bounded things here, because their sources (a payload, a script
                       body) are page bytes of any length; everything else is the assert's message, sized by
                       check.h. */
                    char pl_txt[512], body_txt[1024];
                    const char *sk = cur && cur->fire.kind == FIRE_SINK ? cur->fire.sink : "(exploration flow)";
                    const char *pl = cur ? cur->cand_payload : NULL;
                    /* Which program: script_i indexes the one sequence, the document's scripts and the flow's own
                       dynamic bodies (a lazy chunk, an injected <script>, a fired PoC) as rows of one table
                       (solver/flow.h), so the message can name the JS that was running. */
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
                    /* The live heap, because a garbage collection is the one thing reachable from an ordinary Web
                       API call whose cost is the size of everything else: createElement allocates (a wrapper, a
                       shape) and can trigger a collection, which marks the whole live set, so one call can be
                       arbitrarily slow while every other call of the member is fast. The wrapper map's size beside
                       it says whether the live set is the DOM the flows built or something else. */
                    JS_ComputeMemoryUsage(JS_GetRuntime(ctx), &mem);
                    /* Three numbers, because two cannot separate the roots. `asked` is how many suspend points the
                       path offered (every consultation of the hook), `requested` how many the WFQ wanted to take,
                       `fired` how many parked. asked==0: the seam is missing and must be built. asked>0 with
                       requested==0: the scheduler declined every point, a ranking question. requested>fired: a
                       wanted preempt was dropped because no driver at that depth adopts the seam. */
                    /* Both measures are printed, the work that decided and the wall clock that does not: a large
                       work count with a small gap is a seamless stretch on an idle box, and a large gap with the
                       same count says the box was loaded too; neither changes the verdict. */
                    /* Both excerpts are walked by length, not to a terminator: a bundle carrying a U+0000
                       (ECMAScript §11.1 "Source Text" permits one) would otherwise name the empty string or a
                       prefix. The byte filter prints every non-printable, NUL included, as '.', so a reader gets one
                       line of source text rather than escapes (check.h escapes what it emits regardless). */
                    excerpt(pl_txt, sizeof pl_txt, pl, pl ? strlen(pl) : 0);
                    excerpt(body_txt, sizeof body_txt, bodytxt, bodyn);
                    DFAILF("%ld events (forks+flows+jobs) with no suspend point offered (wall gap %d ms, step "
                           "%d ms; pricing clock %s; points asked=%llu, preempts wanted=%llu fired=%llu; slowest Web "
                           "API member step %s %dms of %dms over %ld steps; wrapper map %ld/%ld; live objects %lld, "
                           "heap %lld KiB) — this stretch has no suspend/resume seam; unit=%s script_i=%d flow=%s "
                           "payload=%s body=%s",
                           events, (int)gap, (int)spent, quantum_measure(),
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
                /* The image this member owes the host, taken at the one moment its world is complete
                   (@PERWORLD): the closing edge of its last turn, with `flow_running()` naming it and its COW and
                   DOM deltas still applied, since the two lines below unapply them. A member ends once, so a mark
                   spent here costs one return and one picture per world. The yield is taken before the finish and
                   the finish is owed across it (`g_sess_finish_owed`): the same two calls run at this function's
                   entry before anything else; yielding after them would render the baseline again. A member that
                   owes no picture defers nothing, so a run without `engine_request_paint_every_world` finishes on
                   this line. */
                if (flow_paint_owed(cur)) {
                    flow_clear_paint_owed(cur);
                    g_sess_finish_owed = 1;
                    g_sess_cur = cur;
                    return ENGINE_STEP_YIELD;
                }
                solve_flow_end(cur); flow_finish(ctx, cur); cur = NULL;
            }
            else if (r == FLOW_STEP_RETIRED) {
                /* A timeline that does not exist leaves without finishing: nothing it did is an outcome. Switched
                   out first, as the pager's sale, since the release frees what a parked flow holds. */
                if (flow_paint_owed(cur)) flow_clear_paint_owed(cur);
                flow_withdraw_async_sends(ctx, cur);
                flow_switch_out(ctx, cur);
                cur->retired = 1;
                flow_release(ctx, cur);
                cur = NULL;
                g_answer_standbys_retired++;
            }
            else if (r == FLOW_STEP_OWED) {
                /* This flow can make no progress until the host answers, and the scheduler records that on the
                 * flow. It is not skipped or removed: it keeps its WFQ weight and every work item, and is picked
                 * again at the top of the next slice, the first moment anything can have answered it. Not a count
                 * of consecutive owed answers: this loop re-picks the same top-ranked flow (an owed step burns
                 * microseconds and moves neither its service notch nor its weight), so N owed answers come from
                 * one flow, and a no-progress count would declare the frontier stalled with runnable siblings
                 * never asked.
                 *
                 * The mark is a claim about the host, asserted where it is made: a marked flow leaves the pick
                 * until a host event clears it, so it must rest on something, and there are three things it can
                 * rest on. An entry the host can still answer (pending_host_outstanding; the joins select the
                 * unanswered and not declined); a peer that can still ask this timeline something (g_referenced);
                 * a refusal this engine has recorded and accounts for (pending_declined_outstanding), a flow that
                 * can convert no slice into work and that the session's tail writes to the cold tier before it
                 * closes. A register whose every entry has been answered satisfies none of them (`pend_owed` is
                 * false for an answered entry): no host event can clear that mark, and the flow would leave the
                 * run queue for good while `live`, `blocked` and `owed` still report it waiting. The
                 * frontier-wide stall check below cannot see one such flow while its siblings keep the loop busy,
                 * so this is asked per flow, when the mark is made. */
                DCHECK(pending_host_outstanding(cur->pending) ||
                       pending_declined_outstanding(cur->pending) || g_referenced,
                       "a flow was marked host-owed while every entry on its register is answered — no host event "
                       "can clear the mark; find why the delivery arm above did not take the answered entry (a "
                       "refused entry is a different, legal state)");
                flow_set_host_owed(cur);
            }
            /* The engine answers as host for an address with no bytes, after the mark it clears. */
            host_requests_decline_unaddressed();
            /* Nothing is cleared by progress: no step can answer another flow's wait, since every kind on the
               register (a fetch reply, an external script's text, a synchronous request's answer, a routed record,
               a cross-agent operation) is filled by the host, which does not run until this slice returns.
               Clearing here would re-admit every marked flow after every step, wasting a step and two COW swaps per
               unit of real progress. */
        }
        /* Thread-sharing: the slice's budget is gone, so hand the thread back and keep the frontier. It is the
         * question preempt_hook asks, at the two levels it acts on (there the running flow parks, here the step
         * returns), so a flow parked on the quantum and a step ended on it are one event; the step's close banked
         * the running flow's standing count, so the banked work is the whole of it here. Level 1 ranks documents
         * in the trusted zone by their novel value per unit of work served, and no flow weight of this frontier
         * is compared with another document's.
         *   Named residual. Not covered: the host yield (quantum_host_wants_thread, emscripten only) ends the
         * slice as the work budget does, so Level 1 re-ranks and Level 2 charges at a point elapsed time chose.
         * Next diff builds: a fourth step code the renderer answers by re-entering the same engine with no pick,
         * charge or service round, so the slice resumes where the host took the thread. Absence shows as: two
         * runs of one fixture on the WASM host giving different pick sequences while native runs agree. */
        if (engine_slice_work() >= ENGINE_QUANTUM_WORK || quantum_host_wants_thread()) {
            /* The other closing edge of a turn, discharged without buying a return (@PERWORLD): a yield hands the
               host this member standing, with its deltas applied and a step of real work behind it, which is the
               turn a picture is owed of, so a mark still up is paid by the return already happening. This is what
               photographs a member that never finishes (a flow looping for the rest of the session never reaches
               the DONE arm but reaches slice boundaries constantly). A slice can reach this line with no member
               standing, hence the guard on `cur`. */
            if (cur != NULL && flow_paint_owed(cur)) flow_clear_paint_owed(cur);
            g_sess_cur = cur;
            return ENGINE_STEP_YIELD;
        }
    }
    g_sess_cur = cur;
    /* The stall claim, checked of every member: the other half of the host-owed mark. The mark's own assert says a
       mark rests on something; nothing else says a mark was never laid on a flow that could still progress, a
       failure that stops exploration early and reports a frontier "waiting on the host" the host owes nothing.
       Reaching here means the pick found no runnable member, so every member is marked and must hold something
       only the host can supply: an entry on its register (a fetch reply, a document script's text, an answer to a
       synchronous request), or, with no entry, a document a peer still holds a reference into, whose last
       timeline reports host-owed instead of finishing (engine_set_referenced). One stall answers every request:
       engine_host_requests joins every outstanding record into a growing buffer (no truncation, no cap) and every
       host walks the whole list; a frontier that stays blocked across stalls is the set being regenerated by
       forking, since a fork re-issues its parent's unanswered synchronous request under the sibling's world
       (engine_fork_finalize). */
    for (int i = 0; i < flow_count(); i++) {
        /* Outstanding, not merely present: `pending_count > 0` is true of a register whose every entry has been
           answered, which is a member nothing can wake. The predicate is the flow question
           (`pending_outstanding`), not the host question: a frontier parked entirely on refusals is legitimate,
           reaches this walk past the two STALLED exits, and is written to the cold tier below, and its refused
           entries are still outstanding to their flows. What this walk catches is a member holding nothing at all,
           a mark laid on a flow that could still progress, which the mark's own assert cannot see. */
        DCHECK(pending_outstanding(flow_at(i)->pending) || g_referenced,
               "the frontier reported a stall while a member waits on nothing — every entry on its register is "
               "answered and delivered, so its mark was laid while it still had work");
    }
    /* STALLED, not exhausted: the run queue is empty but flows are parked on something only the host can supply.
       Asked before closing, so the session and every parked snapshot stay live and the host steps again once it
       has provided. The engine's own question over its own registers (engine_host_owes). */
    if (engine_host_owes())
        return ENGINE_STEP_STALLED;
    /* No seeding here: the @S search is seeded at the pick above, where a candidate becomes a member the moment its
       sink exists; seeding where the frontier drained would serve only documents that drain. */
    /* And the other reason a frontier is not exhausted, a question the host has not yet been asked: a document
       another instance holds a reference into can be asked something at any moment, so its timelines are parked
       (flow_step returns OWED for them) and the session stays live. Closing here would leave the next operation
       no timeline to answer in, which engine_perform's assert names from the other end. */
    if (g_referenced)
        return ENGINE_STEP_STALLED;
    /* Every reaction routes to its enqueuing flow's queue through the enqueue hook, so a job on quickjs's global
       list was enqueued outside a flow and nothing will run it. */
    DCHECK(!JS_IsJobPending(JS_GetRuntime(ctx)),
           "async: a job reached the global list (enqueued outside a flow) and was never drained");
    /* A member's queued jobs are asserted after the park below, not here: a member resting on a refusal may hold
       work its timeline has not reached, which its recipe carries. */
    for (int i = 0; i < flow_count(); i++) {
        /* Named residual. Not covered: `flow_deliver_pending == 0` below forbids a state the park repairs by
           writing an 'm' record per delivery. Next diff builds: confirm the `zero == 0` conservation assert
           below holds without this walk (its premise cites the walk), then move this assert below the park
           beside the jobs assert. Absence shows as: a peer's message the park would have written aborting at
           this loop instead of parking. A peer owed an operation's answer is not asserted here:
           engine_park_frontier's first act, engine_retract_operations, clears flow_owes_answer for every flow
           and tells the peer, and engine_retract_span's postcondition and cold_park_flow assert it after that
           repair. */
        /* The same rule for a routed record, a work item as a job is: flow_finish asserts it for a flow that
           runs out of work, and a flow that leaves this loop alive would drop the record in flow_registry_free,
           the peer's message indistinguishable from a page with no handler. A flow suspended inside a live
           frame is the shape that reaches here holding one (deliveries are made only where flow_step has no
           frame), so if this fires, the enqueue belongs earlier than that branch. */
        DCHECK(flow_deliver_pending(flow_at(i)) == 0,
               "the frontier was declared exhausted while a live flow still holds routed records");
    }
    /* The conservation law over the deliveries that became tasks. The asserts above are each about one queue at
       one moment; this is the only line that can say every queued §9.3.3 task reached an end, a fact about a run.
       flow_deliver asserts a delivery became exactly one task (§9.3.3 step 8 queues one global task); nothing else
       asserts the task runs, and a task that never ran leaves the page unable to tell a message it did not get
       from one never sent. The ends and the inequality are engine.h's: a fork gives the arm its own Array naming
       the parent's job records, so a timeline branching between enqueue and run delivers in both arms and the sum
       may exceed the tasks queued. Below it, a delivery vanished. */
    {
        long ends[ROUTED_TASK_END_N], sum = 0;

        engine_routed_task_census(ends);
        for (int i = 0; i < ROUTED_TASK_END_N; i++) sum += ends[i];
        DCHECK(sum >= g_routed_delivered,
               "the frontier was declared exhausted with fewer §9.3.3 delivery-task ends than deliveries — a "
               "delivery task never ran; find the queue it stands on and why that flow was never picked");
        (void)sum;
    }
    /* And the conservation law one level up, over records rather than tasks. Everything above is per (record,
     * timeline) attachment, and one record admitted by N timelines pays for N-1 admitted by none, so none of those
     * sums can say whether some record was admitted by no timeline. This line can: `zero_delivery` is a gauge that,
     * while the frontier is live, counts records in flight with records lost and falls as timelines admit them.
     * Here every live flow has been asserted to hold no pending delivery and every finished flow asserted it at
     * flow_finish, so every attachment has been consumed, the gauge cannot fall again, and its value is the number
     * of peers' messages this document never received: asserted rather than reported, because the page cannot
     * tell such a message from one never sent.
     *
     * What fires here is a refusal with no arm behind it. deliver_admits asserts the contradict half at the
     * refusal; this is the foreclosed half's only witness and the backstop for both (an arm minted and never
     * picked, a record attached to a flow that left by a path flow_finish does not cover, a covering timeline
     * that refuses for a reason of its own). It is also the only witness for a refusal whose arm is owed (flow.h's
     * FLOW_COMMIT_ARM_ANSWER_OWED): the arm is minted only if the answering peer answers from a second timeline,
     * so an abort at the refusal would fire on a peer that answered once, correctly; the obligation is recorded at
     * the push and can be known undischarged only here, with the frontier exhausted. */
    {
        long d = 0, r = 0, zero = 0;

        engine_routed_census(&d, &r, &zero);
        DCHECK(zero == 0,
               "the frontier was declared exhausted holding routed records no timeline of this document admitted — "
               "each was refused at deliver_admits by a received world that contradicts it, a foreclosed world that "
               "covers it, or an answer world whose arm flow_answer_fork owes; re-run the routed world vectors "
               "through deliver_admits to see which");
        (void)zero;
    }
    /* A session that closes over live members writes them down first: the walks above are about work a member
     * holds, none about the member itself, and the loop above can leave with flows alive (every member host-owed).
     * Not the same exit as an exhausted frontier, and the guard says so: a frontier that drained has no member to
     * write (every flow ran to its end through flow_finish), and an empty park there is a different statement
     * (cold_park deletes the origin's entry, the honest report for a residue resumed and consumed). Routing on
     * `flow_count()` keeps the two apart; deleting the survivors case would leave the question still asked, so it
     * is routing, not a fallback. The route that reaches it passes the two STALLED exits above: a frontier every
     * member of which is parked on a request the trusted zone refused, outstanding to the flow and owed by nobody
     * (`engine_host_owes` answers 0). That is @S search not yet solved, waiting on a per-origin widening in a later
     * session, and a residue is what carries it there. */
    if (flow_count() > 0) {
        /* Refused only if nothing was written before: a tail sold at the RAM floor is runnable work. */
        g_park_refused = cold_park_records() == 0;
        engine_park_frontier(ctx, g_sess_cur);
        g_sess_cur = NULL;
    }
    /* No queued job dies with the session. A member that rests holding jobs is a legal state: the waiting arm of a
       refused synchronous request stays suspended inside the task that asked (an asynchronous XMLHttpRequest
       places its rendezvous from its send task), so a task queued behind that one is work its timeline has not
       reached. The replay its recipe drives re-causes it (cold.h), and cold_park_flow refuses the one kind a replay
       does not re-cause (flow_job_external). So each holder must have had its recipe written. */
    for (int i = 0; i < flow_count(); i++)
        DCHECKF(flow_jobs_held(flow_at(i)) == 0 || flow_at(i)->paged,
                "the session closed while member %d of %d, resting at `%s`, holds %d queued job(s) and has no "
                "recipe — the park above writes every member, so its queue would die with the session unwritten",
                i, flow_count(), step_unit_name(flow_at(i)->step_unit), flow_jobs_held(flow_at(i)));
    /* And the claim is asserted where it is made: a resting member is deprioritized and paged, resumable across
       sessions, never terminated, so at the instant the session stops existing every standing member must have
       its recipe written; otherwise its timeline ends with the process while `live` counted it and the result
       document names no residue. */
    DCHECK(flow_count() == 0 || engine_frontier_paged(),
           "the session closed over live members whose recipes were never written — a resting member is paged, never "
           "dropped, so a new exit must take the park above");
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

/* The cycle collector's slices (solver/rest_unit.c's REST_UNIT_GC_WORK states the policy): how many the scheduler
   gave it, and their thread time on the pricing clock, which prices the collector's work rate and decides nothing.
   Lifetime counts, read by result.c's @HEAP. */
static long long g_gc_slices;
static int64_t   g_gc_us;
long long engine_gc_slices(void) { return g_gc_slices; }
long long engine_gc_us(void) { return (long long)g_gc_us; }

/* A slice the collector holds: it begins an owed collection, then takes steps of rest_unit_items(REST_UNIT_GC_WORK)
   units until the collection finishes, the slice's work reaches the quantum or the host claims its thread. No
   flow runs in it, so nothing separates two steps but the host's own time between slices. Each step's work is
   banked in the instance's currency at THREAD_WORK_UNITS_PER_KI_GC, so the slice ends at the quantum like any
   other and Level 1 sees the instance's collection as service. It answers YIELD: every flow is where it was. */
static int engine_collect_slice(JSRuntime *rt) {
    const uint64_t budget = rest_unit_items(REST_UNIT_GC_WORK);
    int64_t t0 = quantum_thread_us();
    JSGCCycleStats st;
    uint64_t w0;
    bool done;

    DCHECK(JS_GCOwed(rt), "a collector slice was opened with no collection owed or standing");
    g_gc_slices++;
    JS_GCCycleStats(rt, &st);
    if (st.step == 0)
        JS_GCBegin(rt);
    w0 = st.work;
    do {
        uint64_t units;

        done = JS_GCStep(rt, budget);
        JS_GCCycleStats(rt, &st);
        DCHECKF(st.work >= w0, "the collector's lifetime work fell from %llu to %llu across one step",
                (unsigned long long)w0, (unsigned long long)st.work);
        units = ((st.work - w0) * THREAD_WORK_UNITS_PER_KI_GC + 1023) / 1024;
        DCHECKF(units <= (uint64_t)(INT64_MAX - g_service_work),
                "banking %llu collector units would overflow the instance's work counter (%lld)",
                (unsigned long long)units, (long long)g_service_work);
        g_service_work += (int64_t)units;
        w0 = st.work;
    } while (!done && engine_slice_work() < ENGINE_QUANTUM_WORK && !quantum_host_wants_thread());
    g_gc_us += quantum_thread_us() - t0;
    return ENGINE_STEP_YIELD;
}

/* One quantum. Returns ENGINE_STEP_DONE when the frontier is empty (the session is closed and its hooks are
 * uninstalled) and ENGINE_STEP_YIELD when the slice's work was spent with the frontier intact. The slice is a
 * budget of engine work (ENGINE_QUANTUM_WORK), not a cap: nothing is dropped, starved, reordered or forgotten
 * across it, and the next call resumes the same top flow on the same frontier.
 *
 * The slice's bracket is a wrapper because the body has many exits: the slice open state (solver/quantum.h) is
 * asserted by every entry that may run only inside or only outside one, so a return that forgot to close it would
 * fail the next slice's open. A `quantum_end()` before every return is the shape where one is eventually
 * missing; one bracket around one call cannot be, which is also why the slice-span
 * accumulators are here and not at those returns. */
int engine_sched_step(void) {
    int r;
    JSRuntime *gc_rt;
    bool collecting;
    /* The earliest reading this engine takes on its own thread, the function's first statement: everything between
       here and engine_sched_slice's entry (arming the quantum, restoring the flow stamp and capture route, the stray
       check) is thread the engine spent, so a span opened past it would report that work as the host's and the
       partition would not sum (solver/engine.h's `loop_us`). */
    int64_t t_in = quantum_thread_us();

    /* And the span every cost this engine reports is a share of, opened from that same reading (see
       `g_instance_us0`), so `g_loop_us` below cannot start before it. */
    if (!g_instance_us0_set) { g_instance_us0 = t_in; g_instance_us0_set = 1; }
    /* The host's own thread between two slices, charged at the reading that closes it, never from a third clock
       read. `g_slice_exit_us` is the previous slice's closing reading, carried, so this gap and that slice's span
       meet at one number and the two accumulators telescope to the whole of `instance_us`. Nothing is charged
       before the first slice returns, which is what the flag says. */
    if (g_slice_exit_set) {
        DCHECKF(t_in >= g_slice_exit_us,
                "solver/engine.c: this slice opened at %lld, before the previous one returned at %lld — the thread "
                "clock is no longer monotone",
                (long long)t_in, (long long)g_slice_exit_us);
        DCHECK(t_in - g_slice_exit_us <= INT64_MAX - g_between_us,
               "the host's between-slice total would overflow — its charges are non-negative and it only "
               "climbs, so this is a slice measure that has stopped being microseconds or an accumulator "
               "narrowed under solver/engine.h's `between_slices_us`; either way the partition at "
               "engine_step_unit_runs would fail with a negative half and read as a broken clock");
        g_between_us += t_in - g_slice_exit_us;
    }
    /* The slice itself, counted where its budget is armed (solver/engine.h's `slices`): the denominator `loop_us`
       and `between_slices_us` would otherwise lack, and the bound the `over_arms` residual names, since the quantum
       arms once here and the loop ends the slice on that expiry, so at most one turn per slice ends on the
       cooperative clause. Raised before the body because it counts slices entered, the population both spans are
       over. */
    g_slices++;
    /* A collection owed or standing takes the slice (engine_collect_slice). RAM pressure waits for it: a sale frees
       objects, which would restart the trial at every step, and the collection is itself the cheapest relief. */
    DCHECK(g_sess_ctx != NULL, "a step was taken with no session context");
    gc_rt = JS_GetRuntime(g_sess_ctx);
    collecting = JS_GCOwed(gc_rt);
    /* The host's time, with no flow stepping and no frontier walk open: where RAM pressure sells, as
       engine_reclaim_at_rest requires. */
    if (!collecting)
        engine_relocate_under_pressure(gc_rt);
    quantum_begin();
    /* The slice's budget is counted from here: its work on the instance's counter, and the dispatch countdown
       that decides where the budget is asked, both of which a session resumed from the cold tier also starts
       from. */
    g_slice_work0 = g_service_work;
    JS_FlowBudgetArm();
    /* The flow stamp, the DOM capture and the capture route belong to the slice, not the session:
     * engine_sched_slice returns YIELD without switching the running flow out, so "the session is live" and "a
     * flow is running" differ by the host's own time. The three marks are one statement in three registers: the
     * stamp says what generation an object is born at, `g_dom_capture` whether the Lexbor tree is being recorded,
     * and the delta whose head a capture lands in. Across the host's time all three are off. The stamp is 0, so
     * what the host creates (the ABI's parse of a fetch reply and its dup onto every parked flow's register,
     * qjs_host_answer's parsed answer, qjs_route's and qjs_perform's delivery records) is baseline and shared by
     * construction; a live stamp would hide those objects from every delta forked earlier
     * (`JS_ObjFlowGen(obj) > d->fork_gen` skips them) and lose a flow's writes to them. The route is NULL, so no
     * host write lands in a flow's head; a live route with a baseline stamp would record the host's property
     * writes as creations in a stranger's head (a baseline object takes cow_capture_hook's shared arm), and the
     * next switch away would delete them, delivering a reply record as `{}`. qjs_step's DCHECK names the
     * host-side list.
     *
     * The wrapper makes this symmetric: the body has several exits, and one bracket around one call cannot miss
     * one. Suspend and restore, never clear and re-enter: the generation is monotonic (every delta's fork_gen is a
     * point on one line), so the next slice resumes at the number this one left, and the route resumes pointed at
     * the flow held across the yield, whose captures must keep going where they went. */
    JS_SetFlowGen(g_slice_flow_gen);
    g_dom_capture = 1;
    cow_set_current(g_slice_delta);
    /* The host's own time is behind this line, and on the first slice so is the session setup before any flow
       existed, two cases the other boundaries cannot see. Between slices the host parses a fetch reply into a
       record, routes a delivery and writes a synchronous answer, all building JS values; a throw any of them leaves
       standing belongs to no flow, and the slice below would hand it to the first flow it resumes. `detail` is the
       session's live flag, separating before the session opened from between two slices. */
    ENGINE_NO_STRAY(g_sess_ctx, "slice-entry: the host's own time between slices (and the session setup before "
                                "the first one)", g_sess_live);
    r = collecting ? engine_collect_slice(gc_rt) : engine_sched_slice();
    /* And the scheduler's tail: HTML §8.1.7.3's end-of-checkpoint hook, the aging charge and flow_finish run after
       the loop's post-step boundary, so only this line watches them. Asked before the marks are put back, so it
       reports the state the slice ended in. */
    ENGINE_NO_STRAY(g_sess_ctx, "slice-exit: the scheduler's tail after the last step "
                                "(§8.1.7.3's end-of-checkpoint hook, the aging charge, flow_finish)", r);
    g_slice_flow_gen = JS_FlowGen();   /* the forks this slice took moved it; carry them, never restart */
    JS_SetFlowGen(0);                  /* the host's own time is baseline: what it creates is shared by construction */
    g_dom_capture = 0;
    /* And recorded into no delta, which makes the line above safe: the save reads whatever the slice left, the
       flow it still holds across a yield or NULL after a close (engine_session_close switches its last flow out,
       and flow_switch_out un-currents as it unapplies, which is also why that function's clear of the live stamp
       suffices). */
    g_slice_delta = cow_current();
    cow_set_current(NULL);
    quantum_end();
    /* What this slice cost the engine's own thread on the pricing clock, closed after `quantum_end()`, at the
       boundary the marks are restored at. One accumulate past every return of the body, which
       is what the wrapper is for. */
    {
        int64_t t_out = quantum_thread_us();

        DCHECKF(t_out >= t_in,
                "solver/engine.c: this slice closed at %lld, before it opened at %lld — the thread clock is no "
                "longer monotone",
                (long long)t_out, (long long)t_in);
        DCHECK(t_out - t_in <= INT64_MAX - g_loop_us,
               "the dispatch loop's slice-span total would overflow — the slice measure is no longer microseconds or "
               "`loop_us` was narrowed");
        g_loop_us += t_out - t_in;
        /* Carried for the next slice's gap, so the host's time between slices is measured from a reading already
           taken. */
        g_slice_exit_us  = t_out;
        g_slice_exit_set = 1;
    }
    return r;
}

/* A host with nothing else to do between quanta (the node smoke test) drives the same steps in a loop: a host's
   loop over the one scheduler, to which a quantum boundary is invisible. */
/* The host streams what the run is costing, because the result document is built when the frontier drains and a
   run that does not finish would otherwise say nothing about why. The cadence is engine work (`stepWork`), the
   scheduler's one currency: a function of the path, so two runs of one page emit the same lines on every host, and
   a census costs a fixed share of the work it reports rather than one per so many scheduler events, which multiply
   per unit of work once the RAM floor sells and switches every step. As a reporting interval it drops, skips and
   reorders nothing. One hundred quanta, stated as a literal for engine/build.mjs's reader. */
#define ENGINE_PROGRESS_WORK ((int64_t)5400000)
_Static_assert(ENGINE_PROGRESS_WORK == 100 * ENGINE_QUANTUM_WORK, "the census cadence is one hundred quanta");

/* The frontier's own reading (engine.h has each row and why it is one struct). It decides nothing and reads no other
   component: every row is a static of this file, and what a reader wants beside them (the delta chains, the cold
   tier's byte census, the WFQ's order) is composed beside it in solver/result.c. */
void engine_frontier_census(EngineFrontierCensus *out)
{
    DCHECK(out != NULL, "the frontier census was asked for into no record");
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
    /* The seed length `deepest` and `completed` are read against (EngineFrontierCensus), from the session's table
       rather than any flow's `dyn_n`, which is the seed plus whatever that timeline queued and differs per arm. A
       closed session reads 0: engine_session_close gives the table back. */
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
    out->answer_standbys    = g_answer_standbys;
    out->answer_standbys_retired = g_answer_standbys_retired;
    out->perform_exhausted = g_perform_exhausted;
    out->host_exhausted = g_host_exhausted;
    out->host_answers_late  = g_host_answers_late;
    out->host_answers_refused = g_host_answers_refused;
    out->host_answers_foreclosed = g_host_answers_foreclosed;
    out->host_terminated    = g_host_terminated;
    out->paged_reqs         = g_paged_reqs;
    out->paged_asks         = g_reclaim_asks;
    out->paged_unarmed      = g_reclaim_unarmed;
    out->paged_floor        = g_reclaim_floor;
    out->pressure_sold      = g_pressure_sold;
    out->pressure_collects  = g_pressure_collects;
    out->park_funded        = g_park_funded;
    out->rest_compositions  = g_rest_compositions;
    out->rest_compose_attempts = g_rest_compose_attempts;
    out->rest_compose_attempts_max = g_rest_compose_attempts_max;
    out->ram_limit_kib      = (long)(g_ram_limit / 1024);
    out->ram_limit_source   = g_ram_limit_source;
    out->ram_share          = (long)g_ram_share;
    out->ram_nonheap_kib    = (long)(g_ram_nonheap / 1024);
    out->pressure_frag_kib  = (long)(g_pressure_frag / 1024);
    /* The payment pair is an inequality, asserted where both halves are read together: `answered` counts the asks
       this session settled, `asked` the ids it minted, and only their order says the two describe one population.
       A fire names a mint or a credit, not a lifetime: all four host counters carry across a session boundary
       together (engine_sched_begin's not-reset list), preserving the order. Both producers are structural: every
       id is minted by `mint_req` (a re-issue at a fork is an ask like any other), and `g_host_answered` is credited
       at one line, the delivery that finds PEND_HAVE_VALUE clear, so an id is settled at most once and a second
       peer timeline's answer goes to `g_host_answers_extra`. A fire is an id minted elsewhere or a second credit
       for one entry. */
    /* The two retirement partitions, asserted where all six rows are in hand, as solver/cold.c asserts its
       `step_units` histogram against `flows`: a split whose parts do not provably sum to the whole is two numbers
       that drift, each looking like a measurement. Each total and its arm are written on adjacent lines today;
       this catches a retirement or sale added later without stating which population it moved. `finished` and
       `sold` are instance lifetime totals, so no session boundary separates a total from its arms. */
    DCHECKF(out->finished_flows + out->finished_cands == out->finished,
            "the census's retirement rows do not partition its retirement total (%ld exploration + %ld candidate "
            "against %ld finished) — flow_finish writes both halves at one line, so a second retirement path exists",
            out->finished_flows, out->finished_cands, out->finished);
    DCHECKF(out->sold_flows + out->sold_cands == out->sold,
            "the census's sale rows do not partition its sale total (%ld exploration + %ld candidate against %ld "
            "sold) — the label is read before flow_release frees it",
            out->sold_flows, out->sold_cands, out->sold);
    /* And the two totals partition the frontier's departures, one level up: without this, the frontier's departure
       count and this file's retirement totals are three numbers in two censuses with no identity between them, and
       "this engine retires nothing" is a subtraction across them. Exact by construction: solver/flow.c's
       `flow_remove` is the one line a member leaves the frontier on, `flow_release` its one caller, and that has
       four callers on a live instance: flow_finish (raising `g_finished`), engine_reclaim_tail (raising
       `g_flows_sold`), the FLOW_STEP_RETIRED arm (raising `g_answer_standbys_retired`) and flow_registry_free's
       drain (raising the fourth arm). None is reset by a session boundary
       (the sale totals carry at engine_sched_begin, and flow_registry_init does not reset arrivals and
       departures). The third term reads 0 at every census hosts take today, since the drain runs after the report,
       but keeps the identity true at every instant. It catches a departure path added later without an arm, which
       any reclamation gated on something other than completion would be. */
    DCHECKF(out->finished + out->sold + out->answer_standbys_retired + (long)flow_departures_teardown() ==
            (long)flow_departures(),
            "the frontier's departure causes do not partition its departures (%ld finished + %ld sold + %ld retired "
            "+ %lld with the registry against %lld) — flow_release is flow_remove's one caller, so a fifth exit moved "
            "the total",
            out->finished, out->sold, out->answer_standbys_retired,
            (long long)flow_departures_teardown(), (long long)flow_departures());
    /* And the seed table's partition, about a table built once per session: `held` is a row whose source text this
       instance has, `awaited` one whose bytes the reply door still owes, `refused` one whose `src` did not
       encoding-parse. An unlabelled row would lower the bundle's share of the reply door that `replyAsked` is read
       against, making a run that learned nothing read as one that reached a request. */
    DCHECKF(out->root_programs_held_at_seed + out->root_programs_awaited_at_seed +
                out->root_programs_refused_at_seed == out->root_programs,
            "the root document's seed rows do not partition its own count (%d held + %d awaited + %d refused "
            "against %d rows) — the arms are written beside the total, on the columns engine_seed_scripts "
            "branches on, so a difference is a second path into that table and `replyAsked` is about to be read "
            "against a bundle share that is not the bundle's",
            out->root_programs_held_at_seed, out->root_programs_awaited_at_seed,
            out->root_programs_refused_at_seed, out->root_programs);
    /* And the program-start partition, the only identity these four rows have. `prog_queued_cand` is an ask and
       `prog_starts_cand` counts starts across timelines, and a fork copies an unstarted row into its arm, so
       starts above asks is that copy; an inequality between them would fire on a healthy branch. */
    DCHECKF(out->prog_starts_cand + out->prog_starts_other == out->prog_starts,
            "the census's program-start rows do not partition its start total (%ld candidate + %ld other against %ld "
            "starts) — each start is one row's, classified beside the total",
            out->prog_starts_cand, out->prog_starts_other, out->prog_starts);
    /* And the two reply doors' own pairs, asserted where all five rows are read together: each door's queue is a
       subset of the replies it examined (both raises happen in one straight-line block for one reply, with no fork
       between), so a queued count above its ask count is a door that raised its queued arm and not its ask. */
    DCHECKF(out->net_prog_fetch_queued <= out->net_prog_fetch_asks,
            "the fetch() reply door queued more programs than it was shown replies (%ld against %ld) — both are "
            "raised in one block per reply record",
            out->net_prog_fetch_queued, out->net_prog_fetch_asks);
    DCHECKF(out->net_prog_xhr_queued <= out->net_prog_xhr_asks,
            "the XMLHttpRequest reply door queued more programs than it was shown replies (%ld against %ld) — both "
            "are raised in one block per reply record",
            out->net_prog_xhr_queued, out->net_prog_xhr_asks);
    /* And the two arms against the one entry, an inequality: `engine_queue_fetched_script` raises the total and
       has a third caller, test_forced.c's `loadScript` host edge standing in for a `<script src>`-shaped door, so
       the residue is read, not required to be zero. An arm above the total is a door that queued a program without
       going through the one compile entry, a second compile door. */
    DCHECKF(out->net_prog_fetch_queued + out->net_prog_xhr_queued <= out->net_prog_queued,
            "the reply doors' queued programs exceed those the one compile entry built (%ld fetch + %ld xhr against "
            "%ld at engine_queue_fetched_script) — a door queued a program through a second compile door",
            out->net_prog_fetch_queued, out->net_prog_xhr_queued, out->net_prog_queued);
    /* And the refusal edge's own partition, which makes a zero `sold` readable: every engine_reclaim_tail call
       leaves by one of three doors (declined because the safepoint is unarmed, answered at the floor, a sale of one
       flow), so the arms are the whole of the asks, and this catches a fourth door added without a counter. `asks`
       alone separates "the allocator never refused" from "the pager was consulted and could do nothing". */
    DCHECKF(out->paged_unarmed + out->paged_floor + out->sold == out->paged_asks,
            "the census's reclaim rows do not partition the allocator's asks (%ld unarmed + %ld at the floor + %ld "
            "sold against %ld) — engine_reclaim_tail has three exits that each count themselves",
            out->paged_unarmed, out->paged_floor, out->sold, out->paged_asks);
    DCHECKF(out->pressure_sold <= out->sold,
            "RAM pressure counts %ld sales against %ld flows sold — every pressure sale is an engine_reclaim_tail "
            "sale, counted there first", out->pressure_sold, out->sold);
    DCHECKF(out->host_answered <= out->host_asked,
            "the census counts more host answers (%ld) than host asks (%ld) — an id was minted outside mint_req, or "
            "a request was credited as settled twice (a second peer timeline's answer belongs in hostAnswersExtra, "
            "%ld)",
            out->host_answered, out->host_asked, out->host_answers_extra);
}

static int (*g_provider)(JSContext *ctx);
void engine_set_provider(int (*provide)(JSContext *ctx)) { g_provider = provide; }

/* And whether this host wants the frontier's residue (engine.h): a seam, not a policy; the answer is the host's and
   the moment the park is taken is the engine's. */
static int (*g_park_hook)(void);
void engine_set_park_hook(int (*want_park)(void)) { g_park_hook = want_park; }

/* The census, for every host whose output is a stream of lines: one cadence, one order, one emitter.
 *
 * A function rather than a block in run_scheduler, because the ABI's hosts drive engine_sched_step through qjs_step
 * and never enter that loop, and the census must be printed on the shipped path, not only by the fixture. Two hosts
 * call it: run_scheduler (the smoke) and test_forced.c's `--abi` arm (the native ABI, whose trusted zone reads its
 * stdout as a record stream). The wasm ABI hosts (route.mjs, solvergate.mjs, pagecensus.mjs, the extension's
 * bridge) read the result document, which carries the same bytes from the same composers.
 *
 * It runs on the host's own time between two steps, never inside a slice, so with the flow stamp and capture route
 * down it creates no flow's JS value, which makes `result_heap_json`'s runtime walk legal here. The cadence is
 * ENGINE_PROGRESS_WORK units of engine work (`stepWork`) or a new candidate, whichever comes first; a sample is O(frontier members) and prices itself on its own output (`scanCensusWeights` against
 * `scanNextWeights` on the @WFQ line).
 *
 * The first sample is unconditional, which separates three states: no census line is a host that never reached its
 * first round; exactly one is a frontier that has neither retired ENGINE_PROGRESS_WORK units of work nor minted a
 * candidate since, a wedged run stated with its numbers; many is a run exploring. Each line carries its sample ordinal, so the
 * state is readable off one line: the greatest ordinal in a log is the series length, and a last line reading 1 is
 * the wedged state. The cadence and the ordinal are statics, because `stepWork` is a lifetime counter not reset at a
 * session boundary, and the cadence is 64-bit like that counter. It answers whether it sampled (solver/engine.h),
 * which a line-stream host cannot re-derive without a second copy of the cadence: the result document is composed
 * only when the frontier drains or stalls, which a real page's never does, so test_forced.c's `--abi` arm spends
 * this return on `qjs_emit_partial` to stream its findings too. */
int engine_census_emit(ResultCensus *keep)
{
    static int64_t next = ENGINE_PROGRESS_WORK;
    static int     last_cands = -1;
    /* The sample ordinal, static for the cadence's reason: an ordinal that restarted while the cadence it numbers
       did not would give two samples of one process one number. */
    static long seq = 0;

    /* The precondition that makes every line below legal: each composer walks state the session owns (the
       frontier, the pager's tables, the runtime behind `g_sess_ctx`), so a census outside a live session is about
       nothing, and `result_heap_json`'s null check would report it one component later as a missing runtime. */
    DCHECK(g_sess_live && g_sess_ctx != NULL,
           "a census was asked for with no live session — every row is a walk of state the session holds");

    /* Either enough work has happened to be worth a line, or the search grew: a new candidate changes what the
       rest of the run will cost. */
    if (g_step_work < next && solve_candidate_count() == last_cands)
        return 0;
    while (g_step_work >= next) next += ENGINE_PROGRESS_WORK;
    last_cands = solve_candidate_count();
    /* The sample is numbered before it is composed, so the emissions carry one ordinal and a block's lines join by
       it rather than by arrival order. On its own statement, since an increment is a side effect a DCHECK
       condition may not have. */
    seq += 1;

    /* The censuses, each printed from the one composer that also puts it on the result document (solver/result.c's
       `result_swap_json`/`result_cold_json`/`result_heap_json`/`result_wfq_json`, solver/decide.c's
       `decide_fork_json`): one composer per census, two emission sites, the same bytes, so the two cannot
       disagree. A host whose output is lines prints its own line, the rule result.h states for a page error and
       test_forced.c for the @S array: a host that publishes a document reads the census out of it, and a host
       whose output is lines must print it when taken or it is not in the output. No line samples one flow (the
       running flow's cursor, the switched-in candidate's sink and payload): `deepest` and `completed` answer the
       cursor's question about the document, and the @S array carries every candidate's sink and payload. */
    {
        char *swap, *cold, *heap, *wfq, *forks;
        int sold;

        /* The RAM floor's refusal loop, as for the result document (main.c's result_json_at_rest): the composers
           walk the frontier with the safepoint closed, so an allocation they are refused cannot sell a flow and
           answers an absent census instead (composef's NULL, JSON_BUF_AT_REST). The whole census is then dropped,
           one flow is sold at rest, and it is composed again, until it fits or nothing is left to sell. */
        for (;;) {
            swap = result_swap_json();
            cold = result_cold_json();
            heap = result_heap_json(g_sess_ctx);
            wfq = result_wfq_json();
            forks = decide_fork_json();
            if (swap && cold && heap && wfq && forks) break;
            free(swap); free(cold); free(heap); free(wfq); free(forks);
            sold = engine_reclaim_at_rest();
            CHECK(sold,
                  "a census could not be composed and the frontier has nothing left to sell — these five lines "
                  "are the whole of what this host says about the frontier it is draining, and a run that "
                  "continues without one reports its cost, its heap and its order as an absence that reads "
                  "exactly like a zero");
        }
        /* One print per marker, because a marker is what a reader matches on: build.mjs greps
         * `^@COLD (\{.*\})$` and `^@WFQ (\{.*\})$`, and one format string carrying five would bury four markers
         * mid-emission. The marker stays a literal at the head of its own format string, which is why the ordinal
         * is spliced per line rather than in a helper taking the marker: engine/fieldgate.mjs reads a stream
         * marker as `@TAG` at the start of an emission.
         *
         * Each line states its sample ordinal, spliced before the composer's first row. It is no composer's row,
         * because those bytes also go on solver/result.c's document, and an ordinal is a fact about a position in a
         * stream, which a document is not: a composer row would read 0 on a document whose host emits no lines,
         * one field answering two questions. It is not `schedEvents` (which result_wfq_json publishes): that says when
         * a sample was composed and this which sample it is, and only this is unique, since a new candidate emits a
         * sample without events advancing; emitting `schedEvents` here too would put a duplicate key on the @WFQ line.
         * The splice steps one byte past the composer's opening brace, so the composer's bytes go out verbatim and
         * the payload stays one object. The comma is conditional because a composer may answer an empty object
         * (result_wfq_json does for an empty frontier), and `,}` would be unparseable, which engine/trusted.mjs
         * reads as the two grammars having parted.
         *
         * @FORKAT is not spliced: its payload is a table whose keys are partitioned by their first byte
         * (solver/decide.c writes a constraint key on concolic_ident_compose's decimal length prefix, a mechanism
         * row on `(`, an unnamed fork site on `~` and its two bounds on `_`), engine/build.mjs's `forkReading`
         * throws on a fourth spelling, and `{}` there states that nothing forked. It is placed in its block by the
         * @COLD line printed beside it.
         * Named residual. Not covered: a reader grepping `^@FORKAT` alone has no ordinal. Next diff builds: a
         * fourth namespace in `forkReading`, a census-owned member admitted by name as its two bounds are, with its
         * empty-table sentence rewritten to mean "the ordinal and no rows". Absence shows as: a @FORKAT line that
         * cannot be placed in a series without counting the @COLD lines above it. */
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
        if (keep) {
            DCHECK(!keep->swap && !keep->cold && !keep->heap && !keep->wfq && !keep->fork_at,
                   "a census was handed to a holder that still owns a previous one — the host frees each sample's "
                   "objects after the document that borrows them, so a held sample would leak");
            keep->swap = swap; keep->cold = cold; keep->heap = heap; keep->wfq = wfq; keep->fork_at = forks;
        } else {
            free(swap); free(cold); free(heap); free(wfq); free(forks);
        }
    }
    fflush(stdout);
    /* The sample happened, said after the flush, because a caller writes more of its own stream on this answer,
       and a `1` before these lines were on the wire would order the host's output by whichever printf ran first. */
    return 1;
}

static void run_scheduler(JSContext *ctx, char **bodies, char **srcs, const ScriptType *types,
                          const ScriptSchedule *sched, lxb_dom_element_t **els, int n, int forking,
                          const char *recipes) {
    int r;
    /* The residue this host was handed, or NULL. Seeding from a residue is engine_sched_begin's own alternative to
       seeding a boot flow, and a host with a file is as much a store as one with an object store; the store is the
       caller's business and the choice the scheduler's, made in one place. */
    engine_sched_begin(ctx, bodies, srcs, types, sched, els, n, forking, recipes);
    for (;;) {
        /* Does this host want the residue? Level-1 eviction is the host's decision (only it sees the other
           documents' engines and the summed working set), so it is a seam, as the stall payment is: the scheduler
           has no idea of memory pressure and the host none of a flow. A host that never evicts installs nothing. It
           truncates nothing: the park writes every member and the caller stores it, so the seam decides when the
           residue leaves memory, never how much survives. Asked at the top of the loop, so the park is taken at the
           next step boundary with no flow switched in. */
        if (g_park_hook && g_park_hook())
            engine_request_park();
        r = engine_sched_step();
        if (r == ENGINE_STEP_DONE)
            break;
        /* The host pays every slice, not only at a stall. ENGINE_STEP_STALLED is returned only when the run queue
           is empty, so under a stall-only payment one flow's reply would wait on every other flow also blocking, a
           cross-flow coupling that grows as exploration succeeds (every fork adds a member that must block, and a
           fork re-issues its parent's unanswered synchronous request under a fresh id, engine_sibling_assemble); a
           frontier forking on unknowns would never stall and never be paid, a livelock that hurts most the leading
           flow, the one that can reach a lazily-loaded chunk. Every host pays on this schedule: the extension's
           bridge pulls qjs_pending and qjs_host_requests after every non-DONE return, and test_forced.c's `--abi`
           arm asks with a `poll` verb answered in the same turn with whatever is ready (the channel is half-duplex
           at both ends: engine/trusted.mjs writes to an instance only in answer to a line asking to read). Paying
           between slices is sound because pending_ready asks the entry's kind (solver/pending.h) and
           flow_deliver_one_reply leaves a synchronous answer where its machine will take it: at a quantum boundary
           the asking machine need not run next. */
        {
            /* The host's own time, asserted where it begins: the claim qjs_step makes at the ABI boundary, made
               here because this driver pays the provider directly. A provider builds objects (a reply record, a
               peer's answer). With the last flow's stamp up they would be stamped as that flow's and no later
               write to them captured; with its capture route up every field the provider writes would be recorded
               as a creation in that flow's head and deleted by its next unapply, delivering an empty record. */
            DCHECK(JS_FlowGen() == 0 && cow_current() == NULL,
                   "the host is being paid with the last flow's generation or capture route still up — every object "
                   "the provider builds would belong to that flow");
            int filled = g_provider ? g_provider(ctx) : 0;
            /* And the provider is its own segment: the host's time has two halves that fail in different places,
               the provider, which builds JS values (a reply record, a peer's answer, a cross-agent operation's
               text), and the rest of this loop between two steps (the census lines, the progress emission, the
               cold-tier preview), which reads. A completion standing here belongs to the first; one reaching
               slice-entry with this silent belongs to the second. `detail` is what the payment filled. */
            ENGINE_NO_STRAY(ctx, "host-provider: the host's reply/answer/operation payment", filled);
            /* And the payment was complete, asserted at the seam. This provider answers out of its own tables, so
               unlike the extension's it has no asynchronous half and owes nothing once it has run: a record still
               outstanding is one it was never handed or walked past, which `blocked` and `owed` (levels of
               unanswered work) cannot distinguish from prompt payment on a frontier that keeps issuing requests.
               The two registers are named separately because they are two questions, and the check is
               unconditional, since both are empty on a slice with nothing outstanding too. */
            if (g_provider) {
                DCHECK(*engine_host_requests() == '\0',
                       "the smoke host paid and a synchronous request is still outstanding — this provider answers "
                       "every record it is handed, so the blocked flow will never resume");
                DCHECK(*engine_pending_fetches() == '\0',
                       "the smoke host paid and a reply is still owed — the same silence one register over: the "
                       "flow that issued this fetch keeps its snapshot and its continuation and is never handed "
                       "the body, so everything the page does behind that reply is missing from the run");
            }
            /* A stall the payment could not move, with nothing filled, is a frontier waiting on something outside
               this host's tables: a document another agent holds a reference into, which no reply answers. Its
               members are real work, so this driver parks them (engine.h's park hook: there is no "stop driving"
               exit) and the next step answers DONE through the park. */
            if (r == ENGINE_STEP_STALLED && filled == 0) {
                engine_request_park();
                continue;
            }
        }
        engine_census_emit(NULL);
    }
    /* The loop leaves only on DONE, and both DONE exits (a drained frontier, a park) close the session. */
    DCHECK(!g_sess_live, "the one-call driver stopped stepping over a live session");
}

/* Explore: seed the boot flow or a parked residue, then drain the frontier, forking at every concolic branch. */
void engine_run(JSContext *ctx, char **bodies, char **srcs, const ScriptType *types,
                lxb_dom_element_t **els, int n, const char *recipes) {
    /* Named residual. Not covered: this entry's caller (test_forced.c) passes no schedule column, so it is
       re-derived here with document_exec_scripts' own call, every row being a parse product (parser-inserted,
       force async false). Next diff builds: engine_run takes DocScripts.sched and this derivation is deleted.
       Absence shows as: two derivations of one column, which agree only while every row is a parse product. */
    ScriptSchedule *sched = NULL;

    if (n > 0 && els) {
        sched = (ScriptSchedule *)reclaim_calloc((size_t)n, sizeof *sched);
        CHECK(sched != NULL, "engine: OOM deriving the schedule column for engine_run's inventory");
        for (int i = 0; i < n; i++)
            sched[i] = els[i] ? script_block_schedule(els[i], types[i], /*parser_inserted*/true,
                                                      /*force_async*/false)
                              : (bodies[i] ? SCRIPT_SCHED_IMMEDIATE : SCRIPT_SCHED_PARSER_BLOCKING);
    }
    run_scheduler(ctx, bodies, srcs, types, sched, els, n, 1, recipes);
    free(sched);
}

/* Whether the frontier has already gone down in this teardown. Not agent state: it is zero outside the window
   between the two calls below, and the window is the one place a host can put the browser half on the wrong side. */
static int g_frontier_released;

/* See solver/engine.h: the frontier goes down while the browser is still standing. */
void solver_frontier_free(JSContext *ctx)
{
    DCHECK(ctx != NULL, "the frontier was released against no realm");
    DCHECK(!g_frontier_released,
           "the frontier was released twice in one agent");
    DCHECK(!g_sess_live && g_sess_cur == NULL,
           "the frontier was released under a live session; call engine_sched_end where the host stops stepping");
    /* The @S searches go back before the frontier, and the order is the reason. A search takes a reference on the
       decision chain when its sink is detected (add_pending's decide_freeze_path, the re-injection point later
       candidates are seeded from), held neither by a flow nor by the running flow's globals. flow_registry_free
       ends in decide_free, whose `g_dec_seg_live == 0` says every such reference has been given back, and
       solve_free is the give-back; after it, any session that detected a sink while standing on a non-empty
       decision path would abort at teardown, which is the ordinary case (a page that takes one branch before its
       first tainted sink). The shape is core/platform.h's, which concolic_free's assert relies on: the claimant
       releases at its own release and the holder asserts it did. Nothing in the frontier's teardown reads the
       search store (flow_release does not call solve_flow_end; the scheduler does). */
    solve_free();
    witness_verify_free();
    post_index_free();
    flow_registry_free(ctx);
    g_frontier_released = 1;
}

/* See solver/engine.h — the solver half's release column, in reverse dependency order. */
void solver_agent_free(JSContext *ctx)
{
    DCHECK(ctx != NULL, "the solver's agent state was released against no realm");
    /* The frontier is already gone, and went while the browser was still standing (solver_frontier_free in
       solver/engine.h; core/platform.c's census assert is the other end). Reaching here with it live means the
       flows torn down after the lines below would be suspended inside browser components platform_agent_free has
       already finished with. */
    DCHECK(g_frontier_released,
           "the solver's agent state was released with the frontier still live — call solver_frontier_free before "
           "platform_agent_free, and this after it");
    /* Nothing of the browser half is still registered here, an ordering statement: each slot holds a C function
       pointer into a browser component, every one released by platform_agent_free before this call, so a slot
       still set is a callback into released state, invisible to both of JS_FreeRuntime's censuses
       (core/agent_state.h records the same defect for Indexed Database §2.7.1's cleanup). The claimant releases;
       this asserts that it did. */
    DCHECK(g_timer_hook == NULL,
           "§8.1.7's timer step was still registered when the solver's agent state was released — "
           "core/timing/timer.c gives it back at timer_free, which runs first");
    DCHECK(g_timer_active_hook == NULL,
           "the count of active timers was still registered when the solver's agent state was released — "
           "core/timing/timer.c gives it back at timer_free, which runs first");
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
           "§13.2.7's document-load lifecycle step was still registered when the solver's agent state was released — "
           "core/dom/document.c gives it back at document_agent_free, which runs first");
    DCHECK(g_parse_end_hook == NULL,
           "§13.2.7 step 3 was still registered when the solver's agent state was released — document_agent_free "
           "gives it back");
    DCHECK(g_link_connected_hook == NULL,
           "§4.6.8.20's connected time for parsed link elements was still registered when the solver's agent state "
           "was released — html_link_free gives it back, and platform.h's release column runs first");
    /* The orphan counters and the generation they last walked at, given back with the frontier they describe.
       `g_orphans_driven` names nothing (engine_orphan_call names each unknown `{orphan<locator>.argN}` from
       JS_OrphanHash, a fact about the bundle, and two drives of one body sharing the name is correct), so like
       `g_orphan_asks` these are per-session numbers, while the result document is the agent's: engine_sched_begin
       asserts one session per instance, so the two boundaries coincide and this release keeps them coinciding.
       Hosts that take a runtime down and bring another up per file run many sessions in one process, and a carried
       `g_orphan_asks` would read `asked > 0` for a session that never asked, inverting the reading the pair exists
       for. The generation is a memo about one runtime's heap, so a second agent keeping it would skip a walk over a
       heap it never walked. */
    g_orphans_driven = 0; g_orphan_asks = 0;
    g_gate_asks = 0; memset(g_gate_arm, 0, sizeof g_gate_arm);
    /* And the ask's partition with it: the parts sum to `g_orphan_asks`, so a summand a second session kept would
       break the equality at its first census. */
    g_orphan_asks_memo = 0; g_orphan_asks_empty = 0; g_orphan_asks_took = 0;
    g_orphan_gen_seen = 0; g_orphan_gen_world = WORLD_NONE; g_orphan_gen_valid = 0;
    /* And the round trip's three numbers, for the same reason: what an inherited drive did with its recipe is a
       fact about one session's residue, and the latch about one frontier's members; kept, they would report the
       first session's round trip and skip the routing walk on a frontier just rebuilt with claims in it. */
    g_orphan_claims_met = 0; g_orphan_claims_unmet = 0; g_orphan_claims_closed = 0;
    g_park_refused = 0;
    attr_shadow_free(ctx);
    endpoint_free();
    /* And the identity set with the tables whose credit sites filed it: an identity left standing would assert
       against the next agent's first mint of the same record. */
    novelty_free();
    /* The page-error console, beside the other emission table and after the whole platform, so its tables' rows
       and its route latch do not outlive the agent. Safe here because every writer is gone, and there are two
       kinds: a flow (`flow_step`, `flow_run_one_job`, a module's rejection reaction), all released by
       solver_frontier_free above; and a browser host edge (HTML §8.1.4.6 "Runtime script errors"'s console hook in
       core/events/report_exception.c, §8.1.4.7/§8.1.6.4's report hook in core/html/unhandled_rejection.c), each a
       slot its component declares to core/agent_state.h and nulls on core/platform.c's release column, which
       platform_agent_free has run in full. */
    result_free();
    /* The metrics block, after every composer above has run its last composition and before the next agent's
       concolic_init asserts it zero. */
    metrics_agent_release();
    /* The concolic value component is last, and the position is the argument: its source registry is what a report
       asks for a source's browser delivery (the encode set, the address component, the reproduction mechanism), so
       every line above may still read it. It is also the one registry here that browser components write into,
       each row a claim given back on core/platform.h's release column before this call, so its first assert is a
       checked statement about its claimants and names the one that did not give its row back. */
    concolic_free();
    /* And the teardown window closes: a host running many agents in one process would otherwise have the next
       agent's solver_agent_free read a frontier release that belonged to the previous one. */
    g_frontier_released = 0;
}

