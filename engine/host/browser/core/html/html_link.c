/* HTML §4.2.4 "The link element"'s processing model, and §4.6.8.20 Link type "preload" — see html_link.h for
 * what this component exists for, which link types it processes, and what is honestly absent. */
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>
#include <lexbor/html/html.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "solver/endpoint.h"      /* every request host-edge funnels one endpoint into the @H surface */
#include "core/idl_slots.h"
#include "core/url/url.h"
#include "core/dom/node.h"
#include "core/dom/element.h"
#include "core/dom/document.h"
#include "core/dom/shadow_root.h"
#include "core/events/event.h"
#include "core/events/event_target.h"
#include "core/fetch/fetch.h"
#include "core/fetch/port_blocking.h"
#include "core/frame/policy_container.h"
#include "core/css/media_query.h"   /* HTML §2.3.10 "Media queries"' "matches the environment", which
                                       §4.2.4.1 "Processing the media attribute" makes prescriptive */
#include "solver/dom_cow.h"      /* the taint an `href` composed out of unknown input carries */
#include "solver/flow.h"         /* §4.2.4.3 ends in a FETCH, and a fetch parks on a flow — see link_trigger */
#include "solver/engine.h"       /* …so the PARSER's own elements are served as a work item — see below */
#include "core/mime/mime_type.h"    /* §4.6.8.20's "a string type matches a preload destination" */
#include "solver/concolic.h"        /* §2.5.6's slot holds a JS value, so a nonce can be unknown input */
#include "core/html/nonce_attribute.h" /* §4.2.4.3 takes the request's nonce from [[CryptographicNonce]] */
#include "core/html/html_link.h"

static int      g_ready;
static JSValue  g_state_key;
static JSAtom   g_atom_state = JS_ATOM_NULL;
static int      g_task_stepid = -1;
/* THE DOCUMENTS WHOSE PARSED `<link>` ELEMENTS ARE STILL OWED §4.6.8.20's BROWSING-CONTEXT-CONNECTED TIME —
   see html_link_parsed, which appends to it, and html_link_connected_step, which drains it inside a flow.
   IT IS A JS RECORD AND NOT A C LIST, which is CLAUDE.md §State-isolation's rule for exactly this shape: the
   entries are written by flows (a child navigable's Document is installed inside the flow that created its
   container, so ONLY that timeline has one), and a JS property write is what the COW delta already captures.
   A malloc'd list would be one list for every timeline at once, and its cursor would rewind on a context
   switch while the nodes it named stayed reachable from nothing.
   NULL-PROTOTYPE, for core/idl_slots.h's reason: `n` and `i` are fields this file reads back, and a page that
   assigns `Object.prototype.n` a setter would otherwise swallow the write and answer the read. */
static JSValue  g_parsed;

/* ---- the element, and its `rel` ---------------------------------------------------------------------------- */

static bool link_is_node(const lxb_dom_node_t *n)
{
    return n != NULL && n->type == LXB_DOM_NODE_TYPE_ELEMENT && lxb_html_tree_node_is(n, LXB_TAG_LINK);
}

static const char *link_attr(lxb_dom_element_t *el, const char *name, size_t *out_n)
{
    size_t n = 0;
    const lxb_char_t *v = lxb_dom_element_get_attribute(el, (const lxb_char_t *)name, strlen(name), &n);

    if (out_n) *out_n = v ? n : 0;
    return (const char *)v;
}

/* §4.6.8 "Link types": "the keywords are ASCII case-insensitive" and the attribute is "a set of space-separated
   tokens", so membership is a walk of the value split on ASCII whitespace with an ASCII-case-insensitive
   compare. Written here rather than through core/dom/dom_token_list.h because that component's `relList` is a
   LIVE OBJECT the page owns and this is a question C asks of the attribute, and because a token list would have
   to be minted for every `<link>` a parse produced in order to answer it once. */
static bool rel_has(const char *rel, size_t rel_n, const char *kw)
{
    size_t kw_n = strlen(kw), i = 0;

    if (!rel) return false;
    while (i < rel_n) {
        size_t start;
        while (i < rel_n && (rel[i] == ' ' || rel[i] == '\t' || rel[i] == '\n' ||
                             rel[i] == '\f' || rel[i] == '\r')) i++;
        start = i;
        while (i < rel_n && !(rel[i] == ' ' || rel[i] == '\t' || rel[i] == '\n' ||
                              rel[i] == '\f' || rel[i] == '\r')) i++;
        if (i - start == kw_n) {
            size_t k;
            for (k = 0; k < kw_n; k++) {
                char c = rel[start + k];
                if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
                if (c != kw[k]) break;
            }
            if (k == kw_n) return true;
        }
    }
    return false;
}

/* WHICH OF §4.2.4 "The link element"'s TWO CATEGORIES THIS ELEMENT IS IN — "two categories of links can be
   created using the link element: links to external resources and hyperlinks". Four answers and not two,
   because "there is no external resource link here" and "there is one whose steps this engine has not built"
   are different facts about the document and a reader that cannot tell them apart cannot tell an ordinary page
   from an unreachable capability. See html_link.h for why the last is not a fallback and not debt against
   the ones above it. */
typedef enum {
    LINK_EXT_NONE = 0,        /* no keyword in `rel` creates an external resource link */
    LINK_EXT_PRELOAD,         /* §4.6.8.20 — the steps this component runs */
    LINK_EXT_MODULEPRELOAD,   /* §4.6.8.12 — a module GRAPH, whose own fourteen steps this component runs */
    LINK_EXT_UNBUILT,         /* an external resource link whose own steps this engine has no component for */
} LinkExternalType;

/* §4.6.8's external resource link types, as the set this classifier must be able to NAME — the ones that "create
   an external resource link" in their own words. `alternate` is deliberately absent: §4.6.8.1 makes it an
   external resource link ONLY when it also carries `stylesheet`, which the `stylesheet` row already answers. */
static const char *const LINK_EXT_KEYWORDS[] = {
    "stylesheet", "icon", "manifest", "prefetch", "preconnect", "dns-prefetch", "expect",
};

/* THE CLASSIFICATION OVER A `rel` VALUE, taken apart from the element it was read off so the SAME question can
   be asked about ONE keyword — which is what HTML §4.2.4 "The link element" makes `rel`'s supported tokens
   (see html_link_rel_supported). Two spellings of "which state does this keyword put a link element in" would
   be two answers, and the one a page reads through `relList.supports` would drift from the one the trigger
   acts on — a feature detection saying yes to steps that never run. */
static LinkExternalType link_rel_external_type(const char *rel, size_t rel_n)
{
    size_t i;

    if (!rel) return LINK_EXT_NONE;
    if (rel_has(rel, rel_n, "preload")) return LINK_EXT_PRELOAD;
    if (rel_has(rel, rel_n, "modulepreload")) return LINK_EXT_MODULEPRELOAD;
    for (i = 0; i < sizeof(LINK_EXT_KEYWORDS) / sizeof(LINK_EXT_KEYWORDS[0]); i++)
        if (rel_has(rel, rel_n, LINK_EXT_KEYWORDS[i])) return LINK_EXT_UNBUILT;
    return LINK_EXT_NONE;
}

static LinkExternalType link_external_type_of(lxb_dom_element_t *el)
{
    size_t rel_n = 0;
    const char *rel = link_attr(el, "rel", &rel_n);

    return link_rel_external_type(rel, rel_n);
}

/* WHICH OF §4.2.4's STATES THIS COMPONENT HAS THE PROCESSING MODEL FOR — asked of the TYPE, by the two
   questions whose answers may never differ: which keywords `relList.supports` reports, and whose appropriate
   times this file registers. §4.2.4 ties them together in one MUST — "rel's supported tokens must only include
   the tokens from this list that the user agent implements the processing model for" — so a keyword this
   answers yes for and whose times nothing registers is a page taking a feature-detected branch behind steps
   that never run, and the reverse is a page declining a capability the engine has. */
static bool link_type_processed(LinkExternalType t)
{
    return t == LINK_EXT_PRELOAD || t == LINK_EXT_MODULEPRELOAD;
}

bool html_link_rel_supported(const char *token, size_t len)
{
    DCHECK(token != NULL,
           "HTML §4.2.4 \"The link element\"'s supported-token question was asked with no keyword — the "
           "caller resolves the candidate string before asking, so a null here is a caller that asked "
           "without one");
    /* §4.2.4 "The link element" makes the filter a MUST: "rel's supported tokens must only include the tokens
       from this list that the user agent implements the processing model for." So the answer is not that
       section's thirteen possible tokens (`alternate`, `dns-prefetch`, `expect`, `icon`, `manifest`,
       `modulepreload`, `next`, `pingback`, `preconnect`, `prefetch`, `preload`, `search`, `stylesheet`) — it is
       the subset this component runs steps for, which is the two states the classifier above calls
       LINK_EXT_PRELOAD (§4.6.8.20 Link type "preload") and LINK_EXT_MODULEPRELOAD (§4.6.8.12 Link type
       "modulepreload"). Reporting `stylesheet` would state a processing model that is named-and-unbuilt right
       here, and a page that feature-detects one takes the branch behind it — which is not a hypothetical for
       these two keywords in particular: core/dom/dom_token_list.c records that `.supports("modulepreload")`
       and `.supports("preload")` are how a modern bundler's chunk loader picks the `rel` it emits, so this
       answer decides which of the two algorithms below a page hands its lazy chunks to. */
    return link_type_processed(link_rel_external_type(token, len));
}

/* WHICH TYPES' APPROPRIATE TIMES THIS COMPONENT REGISTERS — ONE PREDICATE, asked by the two places that must
   agree about it. HTML §4.6.8 "Link types" states the appropriate times PER LINK TYPE (see html_link.h), so a
   type this file has no steps for is a type whose times it does not register; the TRIGGER asks because it
   serves them, and the parsed walk asks because it COUNTS them — its cursor indexes exactly the elements the
   trigger will act on, so an element the trigger silently declines can never make that cursor step over one it
   would have served. Two spellings of this question would put the walk and the trigger on different sets and
   the difference would look like a preload that simply never fired. */
static bool link_times_registered(lxb_dom_element_t *el)
{
    return link_type_processed(link_external_type_of(el));
}

/* ---- the element's state ---------------------------------------------------------------------------------- */

/* ONE BOOLEAN, AND THE DERIVATION IS WHY IT IS ONE RATHER THAN THREE. §4.6.8.20's last two appropriate times
   are conditional on the PAST — a `type` or `media` change re-triggers only for a link "that was previously not
   obtained DUE TO the type attribute specifying an unsupported type" / "DUE TO the media attribute not matching
   the environment" — so the prose distinguishes the REASON a fetch did not happen. It does not need to be
   stored, because every reason that is not the named one re-declines on its own at the re-run: a link not
   obtained because `as` was absent still translates to a null destination and returns, and one not obtained
   because its media still does not match still fails §4.2.4.1 "Processing the media attribute". So triggering
   on any `type`/`media` change of a NOT-YET-OBTAINED link fetches in exactly the cases the three-reason version
   does, and the single flag is the whole state.
   The one place they could diverge — `as` set, then `type` changed — is covered because the `as`
   change is itself an appropriate time and obtains the link before the `type` change is asked.
   IT IS AN ORDINARY PROPERTY ON A SLOTS OBJECT, so the write is captured by the running flow's COW delta and
   two arms that each reach this element hold their own answer — core/html/html_image.c's rule for a record a
   flow may write, and this is its only creation site, so there is no write site left to miss. */
static JSValue link_state(JSContext *ctx, JSValueConst el)
{
    JSValue st;

    DCHECK(g_ready, "a link element's processing state was reached before html_link_declare interned its key — "
                    "HTML §4.6.8.20 Link type \"preload\"'s conditional appropriate times are what read it");
    if (JS_GetOwnSlot(ctx, &st, el, g_atom_state) > 0) return st;

    st = idl_slots_new(ctx);
    CHECK(!JS_IsException(st),
          "HTML §4.6.8.20 Link type \"preload\": OOM building a link element's processing state");
    JS_SetPropertyStr(ctx, st, "obtained", JS_FALSE);
    JS_DefinePropertyValue(ctx, (JSValue)el, g_atom_state, JS_DupValue(ctx, st),
                           JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE);
    return st;
}

static bool link_obtained(JSContext *ctx, JSValueConst el)
{
    JSValue st = link_state(ctx, el);
    JSValue v = JS_GetPropertyStr(ctx, st, "obtained");
    bool b;

    DCHECK(JS_IsBool(v),
           "the state HTML §4.6.8.20 Link type \"preload\"'s \"previously not obtained\" times read answered "
           "`obtained` with something that is not a boolean — the field is written by this file and nothing else");
    b = JS_ToBool(ctx, v) != 0;
    JS_FreeValue(ctx, v);
    JS_FreeValue(ctx, st);
    return b;
}

static void link_set_obtained(JSContext *ctx, JSValueConst el)
{
    JSValue st = link_state(ctx, el);

    JS_SetPropertyStr(ctx, st, "obtained", JS_TRUE);
    JS_FreeValue(ctx, st);
}

/* ---- the queued element task: §4.6.8.20's `load` and `error` ---------------------------------------------- */

/* IT IS A TASK FOR THE REASON core/html/html_image.c's IS. The fire runs the PAGE's listener list — the whole
   point of this component is that a `link.onload` handler creates a `<script>` — so it must have a flow base
   under it and it must sit at HTML §8.1.7's task position rather than inside the reply's delivery, where a
   page's `head.appendChild(link)` would see its own `onload` run before the next statement. */
#define LINK_TASK_STAGES(X) \
    X(LINKT_FIRE, "HTML §4.2.4 \"The link element\" — the queued task's fire of `load` or `error`")
enum { LINK_TASK_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const link_task_steps[] = { LINK_TASK_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    JSStepHdr   hdr;       /* FIRST — the driver writes the def and the operand bounds through it */
    uint8_t     fphase;    /* the dispatch's own phase, held across a suspension */
    uint8_t     started;
    JSValue     ev;        /* the Event being dispatched (owned) */
    EventFireCb cb;        /* DOM §2.9 "Dispatching events"' request buffer, width travelling with its type */
} LinkTask;

static void link_task_visit(JSContext *ctx, void *stp, JSStepVisit *v)
{
    LinkTask *s = stp;
    int k;

    /* NOTHING IS OWNED UNTIL THE FIRST STEP RUNS — the state's block is ZEROED (quickjs-step.h) and a zeroed
       JSValue is not JS_UNDEFINED, so a visit that reached a machine the driver allocated and has not entered
       would hand the collector a value nobody wrote. */
    if (!s->started) return;
    v->val(ctx, &s->ev);
    STEP_CB_FOREACH(s->cb, k) v->val(ctx, &s->cb[k]);
}

static int link_task_step(JSContext *ctx, void *stp, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    LinkTask *s = stp;
    JSValueConst element = step_arg(&s->hdr, 0);
    JSValueConst name = step_arg(&s->hdr, 1);
    int r;

    STEP_DISPATCH(LINK_TASK_STAGES, s->hdr.stage, s->hdr.def->algorithm, JS_STEP_ABRUPT);

    STEP_ARM(LINKT_FIRE);
        if (!s->started) {
            const char *type;
            int k;

            /* EVERY OWNED FIELD IS PLACED BEFORE THE FIRST THING THAT CAN FAIL — `JS_ToCString` allocates, and
               allocation is a moment the collector can walk this machine through `link_task_visit`. */
            s->started = 1;
            s->fphase = 0;
            s->ev = JS_UNDEFINED;
            STEP_CB_FOREACH(s->cb, k) s->cb[k] = JS_UNDEFINED;
            type = JS_ToCString(ctx, name);
            CHECK(type != NULL, "§4.6.8.20: OOM reading a queued link task's event name");
            /* §4.6.8.20 names each fire as "fire an event named load/error at el" and gives no initialiser, so
               neither bubbles and neither is cancelable. */
            s->ev = event_new(ctx, type, /*bubbles*/ false, /*cancelable*/ false);
            JS_FreeCString(ctx, type);
            if (JS_IsException(s->ev)) { s->ev = JS_UNDEFINED; return JS_STEP_ABRUPT; }
        }
        r = event_target_fire_run(ctx, &s->fphase, STEP_CB(s->cb), element, s->ev, JS_UNDEFINED, cb_result,
                                  NULL, out_cb, out_argc);
        if (r > 0) return r;   /* parked INSIDE the dispatch: a listener is the page's code */
        JS_FreeValue(ctx, s->ev);
        s->ev = JS_UNDEFINED;
        if (r < 0) return JS_STEP_ABRUPT;
        return JS_STEP_DONE;
}

static const JSTrampStepDef link_task_def = {
    sizeof(LinkTask), link_task_step, NULL, 0,
    .visit = link_task_visit,
    .algorithm = "HTML §4.2.4 \"The link element\" — the task that fires a linked resource's `load` or "
                 "`error` (§4.6.8.20 Link type \"preload\"'s response steps, §4.6.8.12 Link type "
                 "\"modulepreload\"'s step 3 and step 14)",
    .steps = link_task_steps
};

static void link_queue_fire(JSContext *ctx, JSValueConst el, const char *name)
{
    JSValueConst argv[2];
    JSValue fn, nm;

    DCHECK(g_task_stepid >= 0,
           "a link element task was queued before html_link_declare registered its step machine — the task "
           "fires HTML §4.2.4 \"The link element\"'s `load` or `error` for whichever of this file's two types "
           "queued it, so a queue with no machine is a component whose events have nothing to run them");
    /* THE CALLEE IS MINTED IN THE ENQUEUING REALM: a C function runs in the realm that DEFINED it, and Web IDL
       §3.7 "Interfaces" gives each realm its own objects for that reason ("the interface object for a given
       interface I with identifier id and in realm realm is created as follows") — and this one fires an event
       at an element of THIS document. */
    fn = JS_NewCFunction2(ctx, NULL, "linkElementTask", 2, JS_CFUNC_step, g_task_stepid);
    CHECK(!JS_IsException(fn), "§4.6.8.20: the queued link task's callee could not be allocated");
    nm = JS_NewString(ctx, name);
    CHECK(!JS_IsException(nm), "§4.6.8.20: OOM allocating a queued link task's event name");
    argv[0] = el;
    argv[1] = nm;
    JS_EnqueueCallTask(ctx, fn, 2, argv);
    JS_FreeValue(ctx, fn);
    JS_FreeValue(ctx, nm);
}

/* ---- §4.6.8.20's processResponse -------------------------------------------------------------------------- */

/* "Preload options, with the following steps given a response response: If response is a network error, fire an
   event named error at el. Otherwise, fire an event named load at el."
   A NETWORK ERROR IS THE JSON `null`, which is the same positive answer core/html/html_image.c's delivery
   asserts — and the ONLY thing this arm asks of a reply that is not one, deliberately: §4.6.8.20 branches on
   network-error-or-not and on nothing else, so a 404 is a RESPONSE and fires `load`. That is the standard's own
   answer and it differs from what browsers ship — the note beside the steps says so ("The actual browsers'
   behavior is different from the spec here") — and this engine follows the specification, which is the rule
   §Browser-half states for every such divergence. */
static JSValue link_deliver(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                            int magic, JSValueConst *func_data)
{
    JSValueConst el = func_data[0];

    (void)this_val; (void)magic;
    DCHECK(argc >= 1, "a preload reply was delivered with no response — the host calls this with the reply "
                      "record or with null for a network error, and never with nothing");
    DCHECK(argc < 1 || JS_IsObject(argv[0]) || JS_IsNull(argv[0]),
           "a preload reply arrived as something other than the host's reply record — every host builds one "
           "with fetch_reply_new or parses the trusted zone's JSON into one, and a bare string here is a host "
           "still delivering only bytes");
    link_queue_fire(ctx, el, (argc >= 1 && JS_IsNull(argv[0])) ? "error" : "load");
    return JS_UNDEFINED;
}

/* ---- §4.6.8.20's destination and type checks -------------------------------------------------------------- */

/* §4.6.8.20 "Link type `preload`": "A preload destination is `fetch`, `font`, `image`, `script`, `style`, or
   `track`." This is the `as` attribute's STATE and nothing more — §4.6.8.20's step 3 is "let destination be the
   result of TRANSLATING the keyword representing the state of el's `as` attribute", so the keyword and the
   request's destination are two values and this returns the first.
   THEY ARE NOT THE SAME STRING, AND THIS FUNCTION USED TO SAY THEY WERE. Its comment read "the six preload
   destinations translate to Fetch §2.2.5 destinations spelled identically", which is true of five of them and
   false of the one whose whole purpose is to differ: Fetch §2.2.7 "Miscellaneous" defines a POTENTIAL
   destination as "`fetch` or a destination which is not the empty string" precisely because `fetch` is NOT a
   destination — §2.2.5's enumeration does not contain it — and translating it yields the EMPTY string. So
   `<link rel=preload as=fetch>` was putting the six characters `fetch` on a request's destination field, which
   every consumer of that field asserts against §2.2.5's enumeration: the pending join's `fetch_is_destination_type`
   aborts a DEV build on any page carrying one, and the trusted zone's chokepoint refuses the line. The two
   consumers that did not abort got the right answer by ACCIDENT and are the reason this stood — CSP §6.8.1's
   trailing "Return connect-src" for an unlisted row happens to be the same directive as the empty
   destination's, and CORB's script-like predicate answers false for an unrecognised value as it does for `""`.
   A field decided by three consumers of which two are right by luck is not a working field.
   NULL is "the `as` attribute names something that is not a preload destination" — including its ABSENCE, which
   is the `as` attribute in its missing-value-less state, and which is why `<link rel=preload href=x>` with no
   `as` fetches nothing in a browser either. */
static const char *const PRELOAD_DESTINATIONS[] = { "fetch", "font", "image", "script", "style", "track" };

/* THE SAME SIX, AS A PREDICATE — because the vocabulary is asked in two places and a second hand-written copy
   of it in the assert below would be free to disagree with the list the answer is drawn FROM, which is the
   exact shape that let the preload keyword and Fetch §2.2.5's destination be one variable in the first place. */
static bool is_preload_destination(const char *d)
{
    size_t i;
    if (!d) return false;
    for (i = 0; i < sizeof(PRELOAD_DESTINATIONS) / sizeof(PRELOAD_DESTINATIONS[0]); i++)
        if (!strcmp(d, PRELOAD_DESTINATIONS[i])) return true;
    return false;
}

static const char *preload_destination_of(lxb_dom_element_t *el)
{
    size_t as_n = 0;
    const char *as = link_attr(el, "as", &as_n);
    size_t i;

    if (!as) return NULL;
    for (i = 0; i < sizeof(PRELOAD_DESTINATIONS) / sizeof(PRELOAD_DESTINATIONS[0]); i++) {
        size_t n = strlen(PRELOAD_DESTINATIONS[i]);
        size_t k;
        if (n != as_n) continue;
        /* §2.3's enumerated attributes are ASCII case-insensitive, which is what makes `as="SCRIPT"` the
           script state and not an invalid value. */
        for (k = 0; k < n; k++) {
            char c = as[k];
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            if (c != PRELOAD_DESTINATIONS[i][k]) break;
        }
        if (k == n) return PRELOAD_DESTINATIONS[i];
    }
    return NULL;
}

/* FETCH §2.2.7 "Miscellaneous"' TRANSLATE A POTENTIAL DESTINATION, verbatim: "If potentialDestination is
   `fetch`, then return the empty string. Assert: potentialDestination is a destination. Return
   potentialDestination."
   IT IS A FUNCTION AND NOT AN `if` AT THE ONE CALLER because it is a fact about the VOCABULARY rather than
   about preload: §4.6.8.20's "translate a preload destination" is defined as "if destination is not a preload
   destination, return null; return the result of TRANSLATING destination", and the `Link:` header path
   reaches the same translation the day it is built. §4.6.8.12 Link type "modulepreload" does NOT, and its
   not doing so is a fact about that algorithm rather than an omission here: its step 3 refuses everything
   that is not a module preload destination, and `fetch` — the one member translation exists for — is not
   one, so a `modulepreload` never reaches a value this function would have to change. The spec's own ASSERT is the DCHECK — a
   potential destination that is neither `fetch` nor a destination is a caller that skipped §2.2.5's
   enumeration, and the value it would hand on is one the pending join refuses and the chokepoint aborts. */
static const char *translate_potential_destination(const char *potential)
{
    DCHECK(potential != NULL,
           "Fetch §2.2.7 Miscellaneous' translate a potential destination was given nothing — its input is "
           "`fetch` or a destination which is not the empty string, and the absence of one is answered by the "
           "caller returning before it gets here");
    if (!strcmp(potential, "fetch"))
        return "";
    DCHECK(fetch_is_destination_type(potential),
           "Fetch §2.2.7 Miscellaneous asserts its input is a destination once `fetch` is handled, and this "
           "one is not in §2.2.5 Requests' enumeration — the value would land on a request's destination, "
           "where the pending join, CSP §6.8.1's effective directive and the chokepoint's CORB class each "
           "decide from it and none of the three can answer for a word the platform does not define");
    return potential;
}

/* §4.6.8.12 Link type "modulepreload": "A module preload destination is `json`, `style`, `text`, or a
   script-like destination."
   THE SCRIPT-LIKE HALF IS NOT SPELLED HERE. Fetch §2.2.5 "Requests"' script-like is a MOVING ENUMERATION of
   six members and core/fetch/fetch.h owns the one reading of it this program has — the same predicate the
   trusted zone's chokepoint asks to decide whether a reply may be ingested as CODE, and the same one CSP
   §6.7.1.1 "Script directives pre-request check" gates its whole step 1 on. A second copy here would be a
   destination this file admits as a module preload destination and the chokepoint refuses to hand back, which
   is a `<link rel=modulepreload>` that fetches and can never load. The three literals ARE written here because
   they are the three the standard writes literally, in this sentence and nowhere else. */
static bool is_module_preload_destination(const char *d)
{
    if (!d) return false;
    if (!strcmp(d, "json") || !strcmp(d, "style") || !strcmp(d, "text")) return true;
    return fetch_is_script_like(d);
}

/* HTML §4.2.4 "The link element"'s `as` ATTRIBUTE STATE — "It is an enumerated attribute. Each of the union of
 * preload destinations and module preload destinations is a keyword for this attribute, mapping to a state of
 * the same name", and "The attribute does not have a missing value default or invalid value default, meaning
 * that invalid or missing values for the attribute map to no state. This is accounted for in the processing
 * model."
 *
 * THE KEYWORD SET IS DERIVED FROM THE TWO PREDICATES THAT SENTENCE NAMES, never re-listed. The union is
 * `is_preload_destination` OR `is_module_preload_destination`, so the day either vocabulary gains a member
 * this attribute gains its keyword in the same edit. A third hand-kept table would be free to disagree with
 * the two it is a union OF — which is the shape that already let one variable in this file hold both a preload
 * destination and a Fetch §2.2.5 destination, at the one member where they differ.
 *
 * THE ANSWER GOES IN THE CALLER'S BUFFER RATHER THAN POINTING INTO A TABLE, and that follows from the same
 * sentence: the state's name is the keyword's, and the script-like six are answered for by a predicate that
 * does not hand back the string it matched. `buf` therefore holds the ASCII-lowercased attribute value, which
 * is that name — §2.3's enumerated attributes are ASCII case-insensitive, so `as="SCRIPT"` is the script state.
 *
 * A VALUE TOO LONG FOR `buf` IS "no state" AND THAT IS AN ANSWER RATHER THAN A NARROWING: every keyword of
 * both vocabularies is shorter than `LINK_AS_MAX`, so a longer value matches none of them and gets exactly the
 * answer the comparison would have given it. The DCHECK is what keeps that true if a vocabulary grows.
 * Answers `buf`, or NULL for §4.2.4's "no state" — which each type's own algorithm accounts for. */
#define LINK_AS_MAX 32
static const char *link_as_state(lxb_dom_element_t *el, char *buf, size_t cap)
{
    size_t as_n = 0;
    const char *as = link_attr(el, "as", &as_n);
    size_t i;

    DCHECK(buf != NULL && cap >= LINK_AS_MAX,
           "HTML §4.2.4 \"The link element\"'s `as` state was asked for into a buffer shorter than the "
           "longest keyword of the union it answers over — an attribute value that does not fit is answered "
           "\"no state\", which is only the same answer while no keyword can reach that length");
    if (!as || as_n == 0 || as_n >= cap) return NULL;
    for (i = 0; i < as_n; i++) {
        char c = as[i];
        buf[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    buf[as_n] = 0;
    if (is_preload_destination(buf) || is_module_preload_destination(buf)) return buf;
    return NULL;
}

/* §4.6.8.20: "a string type matches a preload destination destination if the following algorithm returns true".
   STEP 4 — "If mimeTypeRecord is not supported by the user agent, then return false" — IS NOT A SEPARATE ARM
   HERE, and that is a derivation rather than an omission: step 5 lists the five (destination, type-group) pairs
   this agent can serve and step 6 returns false for everything else, so a type this agent does not support
   either fails step 5 and reaches step 6, or matches step 5 and is by construction supported. Writing step 4
   would be a second statement of which types are supported, free to disagree with the first.
   ITS PARAMETER IS A PRELOAD DESTINATION AND NOT A DESTINATION, and the algorithm's own signature is what says
   so — "a string type matches A PRELOAD DESTINATION destination" — which is why step 2 can ask whether it is
   `fetch` at all. Handing it the TRANSLATED value would make step 2 dead code and silently change what
   `<link rel=preload as=fetch type=…>` does: `""` reaches no arm of step 5 and step 6 returns false, so the
   preload the standard accepts with any type would be dropped for stating one. The two vocabularies are one
   character apart at exactly one member and this is the site where confusing them is invisible, so it is
   asserted rather than commented. */
static bool type_matches_preload_destination(lxb_dom_element_t *el, const char *destination)
{
    size_t type_n = 0;
    const char *type = link_attr(el, "type", &type_n);
    MimeType m;
    bool ok = false;

    DCHECK(is_preload_destination(destination),
           "§4.6.8.20's type-matches-a-preload-destination was given something that is not a PRELOAD "
           "destination — the six are `fetch`, `font`, `image`, `script`, `style` and `track`, and the caller "
           "that hands it Fetch §2.2.7 Miscellaneous' TRANSLATED destination instead makes step 2 unreachable "
           "and drops every `<link rel=preload as=fetch>` that states a type");
    if (!type || type_n == 0) return true;                          /* step 1: the empty string matches */
    if (!strcmp(destination, "fetch")) return true;                 /* step 2 */
    /* Step 3: "Let mimeTypeRecord be the result of parsing type. If mimeTypeRecord is failure, return false."
       §4.4's PARSE and not §4.5's extract: the value is an attribute the page wrote, not a header field. */
    if (!mime_type_parse(&m, type, type_n)) { mime_type_free(&m); return false; }

    /* Step 5's five pairs, in the standard's own order. Each group question is core/mime/mime_type.h's, which
       is MIME Sniffing §4.6 "MIME type groups" — one implementation, so a `type` a preload accepts and a body
       this engine later classifies cannot disagree about what an image is. The two ESSENCE comparisons are the
       two the standard states as essences rather than as groups, and the essence is read only on those arms:
       it is a fresh allocation, and computing it for a `script` destination that never looks at it is a string
       built to be thrown away. */
    if (!strcmp(destination, "script"))     ok = mime_type_is_javascript(&m);
    else if (!strcmp(destination, "image")) ok = mime_type_is_image(&m);
    else if (!strcmp(destination, "font"))  ok = mime_type_is_font(&m);
    else if (!strcmp(destination, "style") || !strcmp(destination, "track")) {
        /* THE EXPECTED ESSENCE IS NAMED BY THE DESTINATION IT BELONGS TO, not derived from a letter of it: a
           `destination[0] == 's'` test reads correctly for exactly the two arms that exist and silently gives
           `text/css` to the next destination whose name begins with an s. */
        const char *want = !strcmp(destination, "style") ? "text/css" : "text/vtt";
        char *essence = mime_type_essence(&m);
        CHECK(essence, "§4.6.8.20: OOM reading a link `type` attribute's essence");
        ok = !strcmp(essence, want);
        free(essence);
    }
    mime_type_free(&m);
    return ok;                                                      /* step 6 */
}

/* §4.2.4.1 "Processing the media attribute": "if the link is an external resource link, then the media
   attribute is prescriptive. The user agent must apply the external resource when the media attribute's value
   MATCHES THE ENVIRONMENT … The default, if the media attribute is omitted, is `all`."
   ONE PREDICATE, THE SAME ONE. HTML §2.3.10 "Media queries" defines "matches the environment", and
   core/css/media_query.h is where it lives — the identical question `<source media>` under a `<picture>` asks
   (core/html/image_source_set.c) and `<source media>` under a media element asks (core/html/media_element.c),
   answered by `media_query_parse` + `media_query_matches_now`. A second spelling here could resolve one query
   differently from the way the page's own `matchMedia` and its cascade resolved it, which is one fact answered
   from two places. The `_now` read is the engine-side policy stated in that header: C cannot fork, so it takes
   the arm this flow has committed to and otherwise the modelled environment, while a PAGE's read of the same
   query stays concolic and still explores both worlds. */
static bool link_media_matches(JSContext *ctx, lxb_dom_element_t *el)
{
    size_t media_n = 0;
    const char *media = link_attr(el, "media", &media_n);
    MediaQuerySet *set;
    char *text;
    bool m;

    if (!media) return true;   /* "The default, if the media attribute is omitted, is `all`" */
    text = malloc(media_n + 1);
    CHECK(text, "HTML §4.2.4.1 \"Processing the media attribute\": OOM copying a link element's `media` value");
    memcpy(text, media, media_n);
    text[media_n] = 0;
    set = media_query_parse(text);
    free(text);
    DCHECK(set != NULL,
           "media_query_parse answered NULL for a link element's `media` attribute. MQ4 §3.1 makes an empty "
           "list match everything and an unparseable query `not all`, so every string this attribute can hold "
           "parses to a set — a NULL is the parser refusing input it defines an answer for");
    m = media_query_matches_now(ctx, set);
    media_query_free(set);
    return m;
}

/* ---- §4.2.4.3's "create a link request", and §4.6.8.20's "preload" ---------------------------------------- */

/* "To create a link request given a link processing options options: Assert: options's href is not the empty
   string. Let url be the result of encoding-parsing a URL given options's href, relative to options's BASE
   URL. … If url is failure, then return null."
   THE BASE URL IS THE DOCUMENT'S, which is what "create link options from element" fills it with ("base URL:
   document's document base URL") — so `<base href>` moves a preload's address, and the resolution is the one
   core/dom/document.h owns rather than a second reading of the document's address. Returns a malloc'd absolute
   serialization, or NULL for §4.2.4.3's "url is failure". */
static char *link_url_absolute(JSContext *ctx, const char *href, size_t href_n)
{
    UrlRecord base, rec;
    const char *base_url = document_base_url(ctx);
    bool have_base;
    char *abs_url = NULL;

    url_record_init(&base);
    have_base = base_url && url_parse(&base, base_url, strlen(base_url), NULL);
    url_record_init(&rec);
    if (url_parse(&rec, href, href_n, have_base ? &base : NULL))
        abs_url = url_serialize(&rec, /*exclude_fragment*/ false);
    url_record_free(&rec);
    url_record_free(&base);
    return abs_url;
}

/* FETCH §4.1 "Main fetch" STEP 7 AND THE REQUEST — the tail HTML §4.6.8.20 Link type "preload" and HTML
 * §4.6.8.12 Link type "modulepreload" share, which is everything from the two metadata members off THIS
 * element to the park.
 *
 * WHAT THE TWO CALLERS STATE, AND WHY THOSE ARE THE ONLY PARAMETERS. They differ at exactly the three places
 * their standards differ and nowhere else: the DESTINATION (§4.6.8.20 step 3's TRANSLATED preload destination
 * against §4.6.8.12 step 2's `as` state), the PARSER METADATA (§4.2.4.3 states none, so preload carries Fetch
 * §2.2.5's initial empty string, while §4.6.8.12 step 13 states "not-parser-inserted" outright), and the
 * processResponse steps (§4.6.8.20 branches on network-error-or-not, §4.6.8.12 step 14 on whether a MODULE
 * SCRIPT was created). Everything between is ONE algorithm asked twice — the nonce read, the integrity, the
 * CSP verdict and the park — and a second hand-written copy of it would be free to drift at the one member
 * that decides whether a page's chunk loads under its own policy. That is not hypothetical for this
 * particular step: fetch.h records that §4.1 step 7 was four hand-written copies, and that the entry nobody
 * remembered to add a fifth to ran no CSP check at all.
 *
 * IT OWNS THE NONCE AND THE METADATA AND NOTHING ELSE. `wrap` and `abs` outlive it at the caller, which has
 * steps of its own left to run; the bytes composed here are BORROWED by the request, so they are released
 * after `fetch_owe` has copied what the park needs and not before. */
static void link_fetch_request(JSContext *ctx, lxb_dom_element_t *el, JSValueConst wrap, const char *abs,
                               const char *destination, CspParserMetadata parser_metadata,
                               JSCFunctionData *deliver)
{
    size_t integrity_n = 0;
    const char *integrity = link_attr(el, "integrity", &integrity_n);
    JSValue nonce_slot;
    const char *nonce;
    CspRequestMetadata metadata;

    DCHECK(abs != NULL && destination != NULL,
           "a link element's request reached Fetch §4.1 \"Main fetch\" step 7 with no address or no "
           "destination — both are computed by the type's own algorithm before it creates a request, and step "
           "7's CSP walk decides from the destination which directive governs the address");
    DCHECK(parser_metadata != CSP_PARSER_METADATA_UNPLACED,
           "a link element's request was created with no parser metadata stated — Fetch §2.2.5 \"Requests\" "
           "gives the field an initial empty string and each of this file's two types says which of the three "
           "values its own algorithm sets, so the unplaced zero here is a caller that stated nothing");

    /* §4.2.4.3's create a link request STEPS 6-7 — "Set request's integrity metadata to options's integrity"
       and "Set request's cryptographic nonce metadata to options's cryptographic nonce metadata" — over the
       two members that §4.2.4.4 "Processing `Link` headers"' create link options from element fills from THIS
       element: its `integrity` content attribute, and "the current value of el's [[CryptographicNonce]]
       internal slot". The options algorithm lives in §4.2.4.4 rather than beside the request one, which reads
       like an editorial accident and is the document's, so a citation of it that follows the request's number
       is wrong about a section a reader can open. §4.6.8.12 reaches the same two facts by its own steps 8-9
       and 10 rather than through §4.2.4.4, and reads them off the same element.
       THE NONCE IS THE SLOT AND NOT THE ATTRIBUTE. HTML §2.5.6 "Nonce attributes" makes those two diverge on
       purpose — after its hiding step the attribute is blank while the slot holds the nonce, and after any
       `el.nonce = v` the slot holds v while the attribute holds the markup's — so reading the attribute here
       would hand CSP bytes the element no longer has, which under `style-src 'nonce-…'` is the difference
       between a stylesheet the policy admits and one it refuses. */
    nonce_slot = nonce_attribute_current(ctx, el);
    /* AN UNKNOWN NONCE IS AN UNDECIDED PREDICATE AND HAS NO ANSWER HERE. §2.5.6's slot deliberately holds a
       JS value so that `el.nonce = location.hash.slice(1)` survives as a source, and CSP §6.7.2.3 step 3.1
       then asks whether that value is identical to a policy's base64-value — a comparison an unconstrained
       domain answers NEITHER way. The arm must FORK: one flow in which the nonce matches and this request
       is allowed, one in which it does not and §6.7.2.5 decides it, with the domain narrowed on each side.
       Coercing the concolic instead picks one of those worlds by whichever example the value happens to
       carry and reports its verdict for both — and the verdict is what §@S attaches to every breakout it
       measures against this document's policy. */
    DCHECK(!concolic_is(nonce_slot),
           "a `<link>` element's [[CryptographicNonce]] is an unknown external value, and CSP §6.7.2.3 "
           "step 3.1 compares it against this policy's nonce-source expressions for identity — which an "
           "unconstrained domain does not answer. Fork the request here into the matching and "
           "non-matching arms and let each carry the narrowed domain, the way an opaque operand forks "
           "every other predicate; a coerced example reports ONE arm's verdict as though it were both");
    nonce = JS_ToCString(ctx, nonce_slot);
    CHECK(nonce != NULL, "§4.2.4.3: OOM reading a link element's cryptographic nonce metadata");
    metadata = csp_request_metadata(nonce, strlen(nonce),
                                    /* An element with no `integrity` attribute has Fetch §2.2.5's initial
                                       empty string, which is §4.2.4.4's own answer: its create link options
                                       from element sets the member only "If el has an integrity attribute".
                                       §4.6.8.12 step 9 states the same default in its own words, and its step
                                       10 — "If el does not have an integrity attribute, then set integrity
                                       metadata to the result of resolving a module integrity metadata with
                                       url and settings object" — answers the empty string too while this
                                       engine parses no import maps, since that algorithm reads the integrity
                                       entries of one and there are none. */
                                    integrity ? integrity : "", integrity_n,
                                    parser_metadata);

    /* Fetch §4.1 "Main fetch" step 7 — "If should request be blocked due to a bad port … or should request be
       blocked by Content Security Policy returns blocked, then set response to a network error". It runs HERE,
       in the engine, because a policy is the DOCUMENT's; the destination the caller computed is what CSP
       §6.8.1 "Get the effective directive for request" switches on, so `as=script` is governed by `script-src`
       and `as=style` by `style-src`. IT IS THE TRANSLATED VALUE and that matters at one member: `as=fetch`
       reaches §6.8.1 as the EMPTY string and is governed by its FIRST row, `connect-src`. Passing the keyword
       got the same directive out of §6.8.1's trailing "Return connect-src" for an unlisted destination — the
       right answer for the wrong reason, which is a coincidence rather than a rule and would have moved the
       day §2.2.5 or §6.8.1 gained a row. A blocked request is a NETWORK ERROR, and each type's own response
       steps answer one: §4.6.8.20's processResponse fires `error` for it, and §4.6.8.12 step 14.1 fires
       `error` because "fetch a single module script" runs its callback with null for a body that is failure.
       So the page's handler runs, and the `<script>` it would have injected is not created, which is exactly
       what a browser does under that policy.
       THE DISJUNCTION AND THE URL PARSE ARE NOT WRITTEN HERE — they were one of FOUR hand-written copies of
       §4.1 step 7, and core/fetch/fetch.h's fetch_main_blocked is the one component they collapsed into. What
       this site still states is what only it knows: the caller's destination and §4.2.4.3's metadata. */
    if (fetch_main_blocked(ctx, abs, destination, metadata)) {
        JS_FreeCString(ctx, nonce);
        JS_FreeValue(ctx, nonce_slot);
        link_queue_fire(ctx, wrap, "error");
        return;
    }

    {
        JSValueConst data[1];
        JSValue d;
        FetchRequest req = {0};

        data[0] = wrap;
        d = JS_NewCFunctionData(ctx, deliver, 1, 0, 1, data);
        CHECK(!JS_IsException(d), "§4.2.4.3: OOM allocating a link request's processResponse steps");
        req.method = "GET";
        req.url = abs;
        /* OPTIONS'S DESTINATION — §4.6.8.20 step 3's TRANSLATION or §4.6.8.12 step 2's `as` state, carried to
           the host rather than recomputed there, and never a preload's `as` keyword before step 3 translates
           it. That distinction is this field's whole correctness: `fetch` is a POTENTIAL destination (Fetch
           §2.2.7 "Miscellaneous") and not a destination, so an `as=fetch` preload carries the EMPTY string
           here, which is the same destination a `fetch()` has and is what makes CSP §6.8.1 govern it with
           `connect-src` by its own first row rather than by its trailing default. The trusted zone reads this
           field for exactly one decision: a SCRIPT-LIKE destination is bytes that will run as code, so its
           reply must be JS-typed or same-origin. `<link rel=preload as=script>` and every `modulepreload`
           whose `as` survives §4.6.8.12 step 3 are therefore code loads and are classified as ones at the
           chokepoint — which is the whole reason this field rides the request: the KIND of park cannot say
           it, because a preload and a `fetch()` park identically and mean different things about the same
           bytes. */
        req.destination = destination;
        req.headers = NULL;
        req.body = NULL;
        req.body_len = 0;
        /* THE SAME VALUE §4.1 STEP 7 WAS ASKED WITH, on the request — so the park's own step 7 asks the
           identical question of the identical request rather than re-reading the element's two members. */
        req.metadata = metadata;
        fetch_owe(ctx, d, &req);
        JS_FreeValue(ctx, d);
    }
    /* …AND THE METADATA'S BYTES LAST, because the request above BORROWED them: the park copies what it needs
       out of the record inside fetch_owe, so this is the first moment nothing is reading them. */
    JS_FreeCString(ctx, nonce);
    JS_FreeValue(ctx, nonce_slot);
}

/* §4.6.8.20's "The fetch and process the linked resource steps for this type of linked resource, given a link
 * element el", with its "preload options" inlined at the one call site that has options — there is no second
 * caller for the split to serve, because §4.6.8.20's other entry ("The process a link header step for this type
 * of link … is to preload options") is `Link:` header processing and this engine has no header-link path.
 *
 * STEP 1, "Update the source set for el", IS NOT RUN AND ITS ABSENCE IS ASSERTED rather than assumed: the
 * source set is only non-null for a link carrying `imagesrcset`, which is §4.8.4.3's grammar reached through
 * core/html/image_source_set.h, and the only step that reads it is the `as=image` branch of preload. So the
 * DCHECK below is where that arrives, at the element that would need it, rather than as a silent wrong answer
 * for a responsive preload.
 */
static void link_preload(JSContext *ctx, lxb_dom_element_t *el)
{
    const char *preload_destination, *destination;
    size_t href_n = 0;
    const char *href;
    char *abs;
    JSValue wrap;

    /* STEP 3: "Let destination be the result of TRANSLATING the keyword representing the state of el's `as`
       attribute. If destination is null, then return." — TWO values and two algorithms, which is what this
       step was collapsing into one. The keyword is §4.6.8.20's PRELOAD destination and is what §4.6.8.20's own
       type-match is defined over; the translation is Fetch §2.2.7 "Miscellaneous"' and is what step 4 sets
       options's destination to, and therefore what the REQUEST carries. They differ at exactly one member —
       `fetch` translates to the empty string — and while one variable held both, `<link rel=preload as=fetch>`
       put a word Fetch §2.2.5 Requests does not define onto a request's destination field.
       "If destination is null, then return" is asked of the KEYWORD, because that is the null the standard
       means: "if destination is not a preload destination, then return null". */
    preload_destination = preload_destination_of(el);
    if (!preload_destination) return;
    /* STEP 4: "Set options's destination to destination." This engine has no link-processing-options record to
       set a field on — §4.6.8.20's other entry is `Link:` header processing and there is no header-link path —
       so the local IS options's destination, and every later read that the standard writes as
       "options's destination" reads it. */
    destination = translate_potential_destination(preload_destination);

    DCHECK(strcmp(preload_destination, "image") != 0 ||
           !lxb_dom_element_has_attribute(el, (const lxb_char_t *)"imagesrcset", 11),
           "§4.6.8.20's preload reached an `as=image` link carrying `imagesrcset` — its step 1 updates the "
           "source set and its step 2 selects an image source out of it, which is §4.8.4.3's grammar in "
           "core/html/image_source_set.h. Run that selection here and set the href from its result, beside the "
           "identical selection core/html/html_image.c already performs");

    /* "To preload given a link processing options options: 1. If options's type doesn't match options's
       destination, then return."
       THE MATCH TAKES THE KEYWORD, because §4.6.8.20 declares its parameter as one ("a string type matches a
       PRELOAD DESTINATION destination") and its step 2 asks whether that value is `fetch` — a question the
       translated destination can never answer yes to, since `fetch` is exactly the member translation
       removes. Reading step 1's "options's destination" as the post-translation value makes step 2 dead and
       drops `<link rel=preload as=fetch type=…>`, which the standard accepts with any type at all. */
    if (!type_matches_preload_destination(el, preload_destination)) return;

    /* §4.2.4.1 "Processing the media attribute"'s prescriptive `media` — stated there over every external
       resource link at once ("the user agent must apply the external resource WHEN the media attribute's value
       matches the environment and the other relevant conditions apply, and MUST NOT apply it otherwise") rather
       than as a numbered step of §4.6.8.20, whose appropriate-times list names a media change as a re-trigger
       precisely because this check can decline. */
    if (!link_media_matches(ctx, el)) return;

    /* AN `href` COMPOSED OUT OF UNKNOWN EXTERNAL INPUT IS STILL A REQUEST THE PAGE MAKES, and it reaches the
       @H surface as the SHAPE it is rather than disappearing — the same answer core/html/html_script.c gives an
       unknown `<script src>` and core/html/html_image.c gives an undecided image source. It matters more here
       than at either of those: a chunk address is exactly the thing a bundler composes (`"/c/" + hash + ".js"`),
       and Lexbor would have ToString'd the taint away, so reading the raw attribute below is a read of a
       concrete string that is not the address. Nothing is fetched, because there is no address to fetch, and
       nothing is fired, because §4.6.8.20's two events are a RESPONSE's — an element left waiting for a load
       that cannot come is what a browser does with a URL it could not resolve either. The link is NOT marked
       obtained: it did not obtain a resource, so a later `type` or `media` change is still one of §4.6.8.20's
       appropriate times, and the surface dedupes the address if it is recorded twice. */
    {
        /* BORROWED, never freed — solver/dom_cow.h states the contract at the declaration and
           core/html/html_script.c reads it the same way at the same kind of site. */
        JSValueConst t = dom_cow_attr_taint(el, "href");
        if (!JS_IsUndefined(t)) { endpoint_record(ctx, "GET", t, NULL, 0, NULL, engine_prov_of_running_path()); return; }
    }

    /* "Assert: options's href is not the empty string, or options's source set is not null." With no source
       set (above), the href carries the whole of it — and a `<link rel=preload as=script>` with no `href` at
       all is the element §4.2.4 "The link element" states outright: "if both the href and imagesrcset
       attributes are absent, then the element does not define a link". */
    href = link_attr(el, "href", &href_n);
    if (!href || href_n == 0) return;

    /* "Let url be the result of encoding-parsing a URL given options's href … If url is failure, then return
       null." and "3. Let request be the result of creating a link request given options. If request is null,
       then return." */
    abs = link_url_absolute(ctx, href, href_n);
    if (!abs) return;

    /* THE ELEMENT IS OBTAINED THE MOMENT THE REQUEST EXISTS, which is what makes the conditional appropriate
       times answerable — see `link_obtained`. It is set before the policy check below for the same reason the
       endpoint is recorded before it: a request a policy refuses is a request this link MADE, and a link that
       re-fetched on the next `type` change because a CSP had blocked it would issue one request per attribute
       write. */
    wrap = node_wrap(ctx, lxb_dom_interface_node(el));
    CHECK(!JS_IsException(wrap), "§4.6.8.20: OOM reaching a link element's processing state");
    link_set_obtained(ctx, wrap);

    /* THE ADDRESS AS THE @H SURFACE MUST SEE IT, recorded BEFORE the policy check for the reason
       core/fetch/fetch.c and core/html/html_image.c record theirs before theirs: the endpoint is what the
       page's code COMPOSED, and a request a policy refuses is still a request the bundle can make. This is the
       @H half of why this component exists — a preloaded chunk is an address the bundle NAMED and, until the
       `load` below fired, never asked for. */
    {
        JSValue uv = JS_NewString(ctx, abs);
        CHECK(!JS_IsException(uv), "§4.6.8.20: OOM naming a preload for the endpoint surface");
        /* §4.2.4.3's create-a-link-request sets no method, so it is Fetch §2.2.5 "Requests"' `GET`. STATED,
           because the reply seam is keyed on the (method, url) pair. */
        endpoint_record(ctx, "GET", uv, NULL, 0, NULL, engine_prov_of_running_path());
        JS_FreeValue(ctx, uv);
    }

    /* §4.2.4.3's REQUEST, THROUGH THE ONE DOOR — see link_fetch_request. §4.2.4.3 states no parser metadata:
       its steps are the potential-CORS request, the policy container, the integrity, the cryptographic nonce,
       the referrer policy, the client and the priority, and Fetch §2.2.5 "Requests"' parser metadata is not
       among them — so a `<link>` request carries that field's initial EMPTY STRING whether the element came
       from the markup or from `document.createElement`. §4.6.8.12 Link type "modulepreload" is the sibling
       that states one, which is why the value is this caller's to pass rather than the callee's to assume. */
    link_fetch_request(ctx, el, wrap, abs, destination, CSP_PARSER_METADATA_EMPTY, link_deliver);
    JS_FreeValue(ctx, wrap);
    free(abs);
}


/* ---- §4.6.8.12 Link type "modulepreload" ------------------------------------------------------------------ */

/* §4.6.8.12's step 14 onComplete, "with the following steps given result": step 14.1 "If result is null, then
 * fire an event named `error` at el, and return", step 14.2 "Fire an event named `load` at el."
 *
 * WHAT MAKES `result` NULL IS "fetch a single module script"'S OWN processResponseConsumeBody, NOT A NETWORK
 * ERROR ALONE — and that is the whole difference between this delivery and the preload one above it, rather
 * than an inconsistency between them. §8.1.4.2 "Fetching scripts" answers null in three ways this engine can
 * see. Two are stated together: "If any of the following are true: bodyBytes is null or failure; or response's
 * status is not an ok status, then … For each callback of callbacks: run callback given null." The third is
 * the initialisation "Let moduleScript be null" standing over a list of arms of which only a matching MIME
 * type sets it — for this caller "If mimeType is a JavaScript MIME type and moduleType is
 * `javascript-or-wasm`, then set moduleScript to the result of creating a JavaScript module script", because
 * "fetch a modulepreload module script graph" hands "fetch a single module script" NO moduleRequest and its
 * step 1 therefore leaves moduleType at "javascript-or-wasm".
 * So a 404 fires `error` here and `load` at the preload beside it. Both are their own standard's answer:
 * §4.6.8.20's steps branch on network-error-or-not and on nothing else, and its own note says that is not what
 * browsers ship, while §4.6.8.12 reaches a module script or does not.
 *
 * NAMED RESIDUAL — `application/wasm`. §8.1.4.2's arm above the JavaScript one is "If mimeType's essence is
 * `application/wasm` and moduleType is `javascript-or-wasm`, then set moduleScript to the result of creating a
 * WebAssembly module script given bodyBytes, settingsObject, response's URL, and options", and this engine has
 * no WebAssembly module scripts at all — so a wasm-typed reply is answered `error` on this line where a
 * browser answers `load`. WHAT THE NEXT DIFF BUILDS: creating a WebAssembly module script, whose consumer is
 * the same module map a modulepreload populates, after which this arm admits that essence beside the
 * JavaScript one. HOW ITS ABSENCE SHOWS: `<link rel=modulepreload href=x.wasm>` runs the page's `onerror`
 * path instead of its `onload` path, so a bundle that feature-detects wasm chunks through this link type takes
 * the branch it takes with wasm switched off. */
static JSValue link_module_deliver(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                   int magic, JSValueConst *func_data)
{
    JSValueConst el = func_data[0];
    bool created = false;

    (void)this_val; (void)magic;
    DCHECK(argc >= 1, "a modulepreload reply was delivered with no response — the host calls this with the "
                      "reply record or with null for a network error, and never with nothing");
    DCHECK(argc < 1 || JS_IsObject(argv[0]) || JS_IsNull(argv[0]),
           "a modulepreload reply arrived as something other than the host's reply record — every host builds "
           "one with fetch_reply_new or parses the trusted zone's JSON into one, and a bare string here is a "
           "host still delivering only bytes");
    if (argc >= 1 && !JS_IsNull(argv[0])) {
        /* "or response's status is not an ok status". Fetch §2.2.3 "Statuses": "An ok status is a status in
           the range 200 to 299, inclusive." A network error is the JSON `null` handled by the test above, and
           §2.2.6 "Responses" gives one "status is 0" — so the two absences are told apart here rather than
           averaged into one number, which is what a reader of a bare 0 would have to do. */
        int status = fetch_reply_status(ctx, argv[0]);

        if (status >= 200 && status <= 299) {
            HeaderList hl = { 0 };
            char *content_type;
            MimeType mt;

            /* "Let mimeType be the result of extracting a MIME type from response's header list" — Fetch
               §3.5's EXTRACT over the joined field value, which is the same read core/loader/script_fetch.c's
               classic entry makes of the same header of the same reply, through the same component. A response
               carrying no `Content-Type` at all is Fetch's "values is null", which is FAILURE, and a failure
               is not a JavaScript MIME type — so an unlabelled body answers `error`, which is what a browser
               does with a module and is the point of the check: these bytes were going to be RUN. */
            fetch_reply_header_list(ctx, argv[0], &hl);
            content_type = header_list_get(&hl, "content-type");
            created = mime_type_extract(&mt, content_type) && mime_type_is_javascript(&mt);
            mime_type_free(&mt);
            free(content_type);
            header_list_free(&hl);
        }
    }
    link_queue_fire(ctx, el, created ? "load" : "error");
    return JS_UNDEFINED;
}

/* §4.6.8.12's "The fetch and process the linked resource algorithm for modulepreload links, given a link
 * element el" — its fourteen steps.
 *
 * IT IS NOT PRELOAD'S SLOW PATH AND SHARES NO REQUEST-CREATING ALGORITHM WITH IT, which is why this is a
 * second function rather than a flag on the one above. §4.6.8.12 says so of itself: "The modulepreload
 * keyword is a specialized alternative to the preload keyword, with a processing model geared toward
 * preloading module scripts. In particular, it uses the specific fetch behavior for module scripts (including,
 * e.g., a different interpretation of the crossorigin attribute), and places the result into the appropriate
 * module map for later evaluation. In contrast, a similar external resource link using the preload keyword
 * would place the result in the preload cache, without affecting the document's module map." Every difference
 * below is one of those: `as` DEFAULTS to `script` here and is an error when absent there; the destination is
 * tested against a different vocabulary; the request is created by §8.1.4.2's "fetch a single module script"
 * rather than by §4.2.4.3's create a link request; the parser metadata is stated; and the response steps
 * decide on a MODULE SCRIPT rather than on a network error.
 *
 * WHY THIS IS THE @H SURFACE AND NOT CONFORMANCE POLISH. A `<link rel=modulepreload>` is the application
 * NAMING a script it intends to load, in the markup, before any of its own code runs — §4.6.8.12's own example
 * calls a set of them "a sort of `manifest` of the application's modules". CLAUDE.md §What-the-tool-produces
 * names the lazy chunk as a headline target, and this is the one place a bundle writes its chunk graph down.
 *
 * WHAT IS NOT COVERED — STEP 14's MODULE MAP, which is a NAMED RESIDUAL rather than a stub, and is narrower
 * than the spec in exactly one observable. Step 14 ends at "Fetch a modulepreload module script graph", whose
 * own step 1 is "Fetch a single module script … and with the following steps given result: Run onComplete
 * given result" — the callback this file supplies — and whose only other effect is that "fetch a single module
 * script" sets `moduleMap[(url, moduleType)]` to the module script it created. This engine has no module map
 * to set: its module loading is ECMAScript §16.2.1.10 HostLoadImportedModule reaching
 * core/loader/module_loader.c, which parks each specifier as the importing flow asks for it. WHAT THE NEXT
 * DIFF BUILDS: a settings object's module map, keyed by (url, module type), written here and READ by that
 * loader before it parks — the two halves in one diff, because a map with no reader is a producer nothing
 * consumes and a reader with no writer is the same defect from the other side. HOW ITS ABSENCE SHOWS: a
 * document that preloads `app.mjs` and then `import()`s it issues TWO requests for it instead of one, which is
 * what a browser does on a preload MISS — the same modelled behaviour html_link.h already records for
 * §4.6.8.20's preload cache — and the `load` this fires is still the one the page's own handler waits for.
 * Its step 1.3's "optionally fetch the descendants of and link result" is not in that residual and never was:
 * the standard makes it OPTIONAL in its own word, and its note explains that skipping it is a bandwidth
 * decision, so a graph that stops at the named module is a conforming graph. §4.6.8.12 says the same thing
 * from the element's side — "the appropriate load or error events will occur after the specified module is
 * fetched, and will not wait for any dependencies." */
static void link_modulepreload(JSContext *ctx, lxb_dom_element_t *el)
{
    char as_buf[LINK_AS_MAX];
    const char *state, *destination;
    size_t href_n = 0;
    const char *href;
    char *abs;
    JSValue wrap;

    /* STEP 1: "If el's href attribute's value is the empty string, then return." An ABSENT `href` takes the
       same return by §4.2.4's own sentence — "if both the href and imagesrcset attributes are absent, then the
       element does not define a link" — and `imagesrcset` is §4.6.8.20's `as=image` branch, never this type's:
       §4.6.8.12 has no source set and no step that reads one.
       THE VALUE IS READ AGAIN AT STEP 4 RATHER THAN HELD FROM HERE, which is what the algorithm itself does —
       step 1 tests "el's href attribute's value" and step 4 encoding-parses "el's href attribute's value", two
       reads of the attribute — and it is also the only lifetime this borrowed pointer can be given: what
       stands between the two steps is §4.2.4.1's media evaluation, which runs this engine's own code, and a
       pointer into lexbor's arena is not a thing to carry across that on the strength of nobody currently
       moving it. */
    href = link_attr(el, "href", &href_n);
    if (!href || href_n == 0) return;

    /* STEP 2: "Let destination be the current state of el's `as` attribute (a destination), or `script` if it
       is in no state." NO TRANSLATION RUNS HERE and that is the standard's doing rather than a shortcut —
       §4.6.8.20 step 3 translates because a PRELOAD destination is not a destination at one member, and
       §4.6.8.12 takes the state itself because step 3 below refuses every value that is not already one.
       "In no state" is BOTH of §4.2.4's two ways of having none: "The attribute does not have a missing value
       default or invalid value default, meaning that invalid or missing values for the attribute map to no
       state." So `as=nonsense` is `script` here exactly as an absent `as` is — the algorithm says "in no
       state" where the attribute's own prose mentions only the missing case, and the algorithm governs. */
    state = link_as_state(el, as_buf, sizeof as_buf);
    destination = state ? state : "script";

    /* STEP 3: "If destination is not a module preload destination, then queue an element task on the
       networking task source given el to fire an event named `error` at el, and return."
       THE EVENT IS QUEUED RATHER THAN FIRED, which is the standard's word and is what keeps a
       `head.appendChild(link)` from running the page's own `onerror` before the next statement of the script
       that appended it — the same reason the response steps queue theirs. It is also the ONE arm of this
       algorithm that reports a failure the page can see without a request having been made: a `<link
       rel=modulepreload as=style>` fires `error` having asked for nothing. */
    if (!is_module_preload_destination(destination)) {
        wrap = node_wrap(ctx, lxb_dom_interface_node(el));
        CHECK(!JS_IsException(wrap), "§4.6.8.12: OOM reaching a link element to fire `error` at");
        link_queue_fire(ctx, wrap, "error");
        JS_FreeValue(ctx, wrap);
        return;
    }

    /* §4.2.4.1 "Processing the media attribute"'s prescriptive `media`. §4.6.8.12 has no numbered step for it
       and its appropriate times do not list a `media` change, and neither of those makes it inapplicable:
       §4.2.4.1 states the rule over the CATEGORY rather than per type — "if the link is an external resource
       link, then the media attribute is prescriptive. The user agent must apply the external resource when the
       media attribute's value matches the environment and the other relevant conditions apply, and must not
       apply it otherwise" — and §4.6.8.12 creates an external resource link in its first sentence. That the
       times omit a `media` change is consistent with applying it and not with skipping it: §4.6.8.12's note
       explains that re-fetching after an attribute change is pointless "because the document's module map has
       already been populated by a previous fetch", which is a statement about the RE-trigger and not about
       this gate. Answered by the same predicate `<source media>` and `matchMedia` answer — see
       link_media_matches. */
    if (!link_media_matches(ctx, el)) return;

    /* AN `href` COMPOSED OUT OF UNKNOWN EXTERNAL INPUT IS STILL A REQUEST THE PAGE MAKES, and it reaches the
       @H surface as the SHAPE it is rather than disappearing — the identical answer link_preload gives, and
       for a sharper reason here: a modulepreload's address is a CHUNK address, which is exactly the thing a
       bundler composes (`"/c/" + hash + ".mjs"`), and Lexbor would have ToString'd the taint away. Nothing is
       fetched, because there is no address to fetch, and nothing is fired, because §4.6.8.12's two events are
       a RESULT's. */
    {
        /* BORROWED, never freed — solver/dom_cow.h states the contract at the declaration. */
        JSValueConst t = dom_cow_attr_taint(el, "href");
        if (!JS_IsUndefined(t)) { endpoint_record(ctx, "GET", t, NULL, 0, NULL, engine_prov_of_running_path()); return; }
    }

    /* STEPS 4-5: "Let url be the result of encoding-parsing a URL given el's href attribute's value, relative
       to el's node document" and "If url is failure, then return." The base is the DOCUMENT's, so `<base
       href>` moves a modulepreload's address exactly as it moves a preload's. NOTE THE STEP THAT IS NOT HERE:
       step 5 returns and fires NOTHING, so a link whose `href` does not parse is left waiting — unlike step
       3's bad `as`, which the standard gives an `error`. The two are one line apart in the algorithm and mean
       different things to a page's handler. */
    href = link_attr(el, "href", &href_n);
    if (!href || href_n == 0) return;
    abs = link_url_absolute(ctx, href, href_n);
    if (!abs) return;

    wrap = node_wrap(ctx, lxb_dom_interface_node(el));
    CHECK(!JS_IsException(wrap), "§4.6.8.12: OOM reaching a link element's processing state");

    /* THE `obtained` FLAG IS DELIBERATELY NOT SET ON THIS PATH. It exists for §4.6.8.20's two CONDITIONAL
       appropriate times, which re-trigger a preload that is "already browsing-context connected, but was
       previously not obtained due to the type attribute specifying an unsupported type for the request
       destination" and the same sentence for `media` — and §4.6.8.12's three appropriate times contain no
       such condition, so nothing on this type's path reads it. Writing it anyway would not be harmless: the flag
       is per ELEMENT, so a link that modulepreloaded and later had its `rel` changed to `preload` would carry
       a claim made by the other algorithm into that one's conditional times and silently decline a re-fetch
       the standard grants it. A field a producer writes and nothing reads is the mirror of the defect
       CLAUDE.md counts seven of; one that a DIFFERENT algorithm reads is that defect with a consumer. */

    /* THE ADDRESS AS THE @H SURFACE MUST SEE IT, recorded BEFORE the policy check for the reason
       core/fetch/fetch.c and core/html/html_image.c record theirs before theirs: the endpoint is what the
       page's code COMPOSED, and a request a policy refuses is still a request the bundle can make. This is
       the @H half of why this arm exists — a modulepreloaded chunk is an address the bundle NAMED and, on
       every document this engine has loaded until now, never asked for. */
    {
        JSValue uv = JS_NewString(ctx, abs);
        CHECK(!JS_IsException(uv), "§4.6.8.12: OOM naming a modulepreload for the endpoint surface");
        /* §8.1.4.2's "fetch a single module script" creates its request with no method, so it is Fetch
           §2.2.5 "Requests"' `GET`. STATED, because the reply seam is keyed on the (method, url) pair. */
        endpoint_record(ctx, "GET", uv, NULL, 0, NULL, engine_prov_of_running_path());
        JS_FreeValue(ctx, uv);
    }

    /* STEPS 6-13, AND WHICH OF THEIR MEMBERS THIS ENGINE'S REQUEST RECORD CAN CARRY. Step 6's settings object
       is this document's realm, which is the realm this call is already in (see html_link_connected_step).
       Steps 8-10's cryptographic nonce and integrity metadata and step 13's parser metadata are Fetch §2.2.5's
       three metadata fields and ride the request — link_fetch_request composes them. The parser metadata is
       the one this algorithm states where §4.2.4.3 states none, and step 13 spells the value out: §4.6.8.12's
       options are "a script fetch options whose cryptographic nonce is cryptographic nonce, integrity
       metadata is integrity metadata, parser metadata is "not-parser-inserted", credentials mode is
       credentials mode, referrer policy is referrer policy, and fetch priority is fetch priority". That field
       is what CSP §6.7.1.1 "Script directives pre-request check" step 1.3 reads, and
       which matters here because a module preload destination is script-like and therefore reaches §6.7.1.1
       at all. Step 7's credentials mode, step 11's referrer policy and step 12's fetch priority have no field
       on this engine's FetchRequest — no request this engine builds carries any of the three, so computing
       them here would be three values written where nothing can read them, which is the shape §NO STUBS
       forbids rather than a gap peculiar to this algorithm.
       STEP 14 IS THE CALL BELOW: this file's processResponse steps are `link_module_deliver`, which is step
       14's "following steps given result", and the graph's own module-map effect is the named residual at
       this function's banner. */
    link_fetch_request(ctx, el, wrap, abs, destination, CSP_PARSER_METADATA_NOT_PARSER_INSERTED,
                       link_module_deliver);
    JS_FreeValue(ctx, wrap);
    free(abs);
}

/* §4.2.4.3's "fetch and process the linked resource algorithm, which takes a link element el" — the dispatch to
   whichever type's steps this element's `rel` names. Reached only from the appropriate times below, so a type
   arriving here with no steps is not a page doing something unusual, it is THIS FILE having registered a
   trigger it cannot serve — which is why it is an assert and not a silent return. */
static void link_fetch_and_process(JSContext *ctx, lxb_dom_element_t *el)
{
    LinkExternalType t = link_external_type_of(el);

    switch (t) {
    case LINK_EXT_PRELOAD:
        link_preload(ctx, el);
        return;
    case LINK_EXT_MODULEPRELOAD:
        link_modulepreload(ctx, el);
        return;
    case LINK_EXT_UNBUILT:
        DFAIL("§4.2.4.3's fetch and process the linked resource was entered for an external resource link "
              "whose own steps this engine has no component for — every such type defines its own algorithm "
              "and its own appropriate times, so this line is reachable only if a trigger below was registered "
              "for a type whose steps were not. Build the type's component (a `stylesheet` needs the CSS style "
              "sheet core/css/css_rule.h records as absent) and dispatch it above");
        return;
    case LINK_EXT_NONE:
        DFAIL("§4.2.4.3's fetch and process the linked resource was entered for a link element whose `rel` "
              "creates no external resource link — the algorithm is defined only over external resource links, "
              "so the triggers below must ask before they call and one of them did not");
        return;
    }
    DFAIL("HTML §4.2.4 \"The link element\"'s link-category classifier answered with a state this dispatch has "
          "no arm for");
}

/* ---- §4.6.8.20's "appropriate times to fetch and process the linked resource" ----------------------------- */

/* THE ONE PLACE THAT DECIDES WHETHER A TRIGGER FIRES, so the four entry points below state only WHICH of the
   appropriate times happened. `connected` is the qualifier BOTH types put on every one of their times: §4.6.8.20's
   five and §4.6.8.12's three each read
   "a link element that is already browsing-context connected" or "becomes browsing-context connected" — and a
   `<link>` built with `createElement` and configured before insertion must not fetch until it is inserted,
   which is the order the idiom this component exists for actually writes. */
static void link_trigger(JSContext *ctx, lxb_dom_element_t *el)
{
    if (!link_is_node(lxb_dom_interface_node(el))) return;
    if (!node_is_connected(lxb_dom_interface_node(el))) return;
    /* ONLY THE TYPES WHOSE TIMES THIS COMPONENT CARRIES. See html_link.h: §4.6.8 states the appropriate times
       PER LINK TYPE, so a type this file has no steps for is a type whose times it does not register — the
       standard's own structure, and the reason the dispatch's other arms are asserts rather than live paths. */
    if (!link_times_registered(el)) return;
    /* §4.2.4.3's default fetch and process the linked resource ENDS IN A FETCH — its last step is "Fetch
     * request with processResponseConsumeBody set to …", with no task and no microtask anywhere in it — so
     * every trigger below reaches a request. THIS ENGINE CANNOT ISSUE ONE OUTSIDE A FLOW: the park pushes onto
     * the flow's own pending register (solver/engine.c), which is per-flow because a global one would resolve
     * one flow's fetch into another flow's COW, so a caller with no flow has nowhere to be owed a reply and
     * nowhere to resume when it arrives.
     *
     * ALL THREE OF THIS FILE'S ENTRIES REACH THIS FROM A FLOW, AND THE THIRD IS WHY THE ASSERT MOVED RATHER
     * THAN BEING DELETED. `html_link_inserted` and `html_link_attr_changed` are reached from script or from a
     * job, and both of those ARE flows. The PARSER's own elements are the third, and they used to be
     * triggered by core/dom/document.c's parsed-tree walk — which for the document a session opens over runs at
     * the pre-boot BASELINE, where there is no flow at all, so a `<link rel=preload>` in the head aborted the
     * engine before a single line of the page ran. They are INVENTORIED by that walk now and SERVED by
     * html_link_connected_step, which the scheduler asks inside a flow; so this line has stopped naming
     * something to build and states the invariant that makes the four entries one algorithm.
     *
     * `<img>` REACHES ITS FETCH BY A DIFFERENT ROUTE AND THAT IS THE STANDARD'S DOING, not a design this file
     * could have copied. §4.8.4.3.5's "update the image data" queues the SPEC'S OWN microtask before it
     * fetches, and §scheduler makes every enqueued job a flow, so the image half of the same walk hands its
     * fetch to a flow by the standard's structure. Queueing one HERE would be inventing a step §4.2.4.3 does
     * not have, and §Browser-half makes ORDER the spec: the request would then be issued after work the parse
     * ordered before it. Deferring the whole TRIGGER keeps the algorithm's steps in the standard's order and
     * moves only the moment it is performed, which is what the parse's own lack of a per-token seam costs. */
    DCHECK(flow_running() != NULL,
           "§4.2.4.3's fetch and process the linked resource was entered with no running flow — it ends in a "
           "fetch, and a fetch parks on the FLOW's own pending register, so a caller with none has nowhere to "
           "be owed a reply. Every one of §4.6.8.20's appropriate times reaches this from a flow: the mutation "
           "entries are script or a job, and the parser's own elements are served by html_link_connected_step, "
           "which the scheduler asks INSIDE a flow. A fourth entry has been added without one");
    link_fetch_and_process(ctx, el);
}

void html_link_inserted(JSContext *ctx, lxb_dom_element_t *el)
{
    /* "When the external resource link's link element becomes browsing-context connected." */
    link_trigger(ctx, el);
}

void html_link_attr_changed(JSContext *ctx, lxb_dom_element_t *el, const char *ns, const char *local)
{
    /* A content attribute is the HTML namespace's; a `foo:href` is a different attribute entirely. */
    if (ns && *ns) return;
    DCHECK(local != NULL, "§4.6.8.20 was asked about an attribute change with no attribute name");
    if (!link_is_node(lxb_dom_interface_node(el))) return;

    /* "When the external resource link is CREATED on a link element that is already browsing-context
       connected" — which is what a `rel` gaining the keyword IS — and the `href` change beside it. These two
       are the times BOTH of this file's types list, in the same words, and each is unconditional: the link may
       already have been obtained under a DIFFERENT address, and re-running is what a browser does. The type is
       read AFTER the change, which is what makes the `rel` arm the creation of whichever link the new value
       names. */
    if (!strcmp(local, "rel") || !strcmp(local, "href")) {
        link_trigger(ctx, el);
        return;
    }

    /* …AND EVERY REMAINING TIME IS §4.6.8.20's ALONE, WHICH IS A FACT ABOUT THE TWO STANDARDS AND NOT A
       NARROWING HERE. HTML §4.6.8 states the appropriate times PER LINK TYPE, and the two lists are different
       lengths on purpose: §4.6.8.20 Link type "preload" names five times, of which an `as` change is the third
       and a `type` or `media` change on a not-yet-obtained link are the last two; §4.6.8.12 Link type
       "modulepreload" names THREE and stops at the `href` change above, then says outright why it stops —
       "Unlike some other link relations, changing the relevant attributes (such as as, crossorigin, and
       referrerpolicy) of such a link does not trigger a new fetch. This is because the document's module map
       has already been populated by a previous fetch, and so re-fetching would be pointless."
       So the question below is asked of the TYPE rather than of the attribute alone. Answering it with one
       list for both types would make `l.as = "style"` re-issue a modulepreload the standard does not re-issue —
       and, because §4.6.8.12 step 3 fires `error` for a destination that is not a module preload destination,
       it would fire that `error` at an element whose module had already loaded. */
    if (link_external_type_of(el) != LINK_EXT_PRELOAD) return;

    /* §4.6.8.20's third time: "When the `as` attribute of the link element of an external resource link that
       is already browsing-context connected is changed." Unconditional like the two above, and for the same
       reason: the link may have been obtained under a different destination. */
    if (!strcmp(local, "as")) {
        link_trigger(ctx, el);
        return;
    }
    /* …and its two CONDITIONAL times, "but was previously not obtained due to the type attribute specifying an
       unsupported type for the request destination" / "due to the media attribute not matching the
       environment". The condition is read as "not yet obtained" for the reason derived at `link_obtained`. */
    if (!strcmp(local, "type") || !strcmp(local, "media")) {
        JSValue wrap;
        bool done;

        if (!node_is_connected(lxb_dom_interface_node(el))) return;
        wrap = node_wrap(ctx, lxb_dom_interface_node(el));
        CHECK(!JS_IsException(wrap), "§4.6.8.20: OOM reaching a link element's processing state");
        done = link_obtained(ctx, wrap);
        JS_FreeValue(ctx, wrap);
        if (done) return;
        link_trigger(ctx, el);
    }
}

/* ---- the PARSER's own elements: the walk inventories, a flow serves ---------------------------------------- */

/* ONE FIELD OF ONE RECORD, read and written through these two so that a field this file writes and reads back
   is never DEFAULTED — CLAUDE.md counts seven live defects whose whole shape was a `||` standing where a
   producer had not written. Both records here are null-prototype (core/idl_slots.h), so neither read nor write
   can reach the page. */
static int link_rec_int(JSContext *ctx, JSValueConst rec, const char *field)
{
    JSValue v = JS_GetPropertyStr(ctx, rec, field);
    int32_t i = 0;

    DCHECK(JS_IsNumber(v),
           "the parsed-document record serving HTML §4.6.8.20 Link type \"preload\"'s browsing-context-connected "
           "time answered a counter with something that is not a number — every field of this record is written "
           "by this file at the one site that creates it, so a non-number here is a read of a field that was "
           "never written rather than a value to interpret");
    JS_ToInt32(ctx, &i, v);
    JS_FreeValue(ctx, v);
    return (int)i;
}

static void link_rec_set_int(JSContext *ctx, JSValueConst rec, const char *field, int v)
{
    JS_SetPropertyStr(ctx, (JSValue)rec, field, JS_NewInt32(ctx, v));
}

/* THE `want`TH OF `root`'s `<link>` ELEMENTS WHOSE TIMES THIS COMPONENT REGISTERS, in shadow-including tree
   order, or NULL when the tree holds fewer than that.
   SHADOW-INCLUDING, for the reason core/html/html_image.c's walk is: a `<template shadowrootmode>` has by now
   become a shadow root whose children are in the tree and are not reachable by the ordinary walk.
   THE POSITION IS THE IDENTITY, and that is sound because of WHEN this is asked rather than by assumption: the
   scheduler asks the service below before this flow runs any program of the document (solver/engine.c), and a
   SIBLING flow's DOM writes are unapplied while this flow is switched in (CLAUDE.md §State-isolation), so the
   tree this walk indexes is the tree the parse produced and it does not move between two calls. `connected` is
   asked per element because §4.6.8.20's every appropriate time is stated over one that is browsing-context
   connected, and because it is what keeps a `<template>`'s inert contents out of the count. */
static lxb_dom_element_t *link_parsed_nth(JSContext *ctx, lxb_dom_node_t *root, int want)
{
    lxb_dom_node_t *n;
    int i = 0;

    for (n = root; n; n = shadow_root_next_in_shadow_including(ctx, n, root)) {
        if (!link_is_node(n)) continue;
        if (!node_is_connected(n)) continue;
        if (!link_times_registered(lxb_dom_interface_element(n))) continue;
        if (i++ == want) return lxb_dom_interface_element(n);
    }
    return NULL;
}

/* See html_link.h. THE WALK INVENTORIES AND DOES NOT TRIGGER — core/loader/document_scripts.h is the same
   shape one step earlier, and the reason is the same one: §4.2.4.3 ends in a fetch, a fetch parks on a flow,
   and the document a session opens over is installed at the pre-boot BASELINE where there is no flow.
   NOTHING IS RECORDED FOR A DOCUMENT WITH NOTHING TO SERVE, which is what keeps the service below free for
   every document that ships no `<link rel=preload>`: the list holds only documents that owe a trigger, so a
   drained list is one comparison per scheduler step rather than a tree walk per step for ever. */
void html_link_parsed(JSContext *ctx, lxb_dom_node_t *root)
{
    JSValue rec;
    int n;

    DCHECK(root != NULL,
           "HTML §4.6.8.20 Link type \"preload\"'s parsed-tree walk was given no tree to walk");
    DCHECK(g_ready,
           "a parsed tree reached HTML §4.6.8.20 Link type \"preload\"'s inventory before html_link_declare ran");
    if (!link_parsed_nth(ctx, root, 0)) return;
    rec = idl_slots_new(ctx);
    CHECK(!JS_IsException(rec),
          "HTML §4.6.8.20 Link type \"preload\": OOM recording a parsed document's link elements");
    /* THE DOCUMENT OBJECT AND NOT ITS NODE POINTER, because this entry is read by a flow that may be many
       scheduler steps away and a raw tree pointer is not a thing a timeline can hold: node_of reads the node
       back at the service, and the reference is DROPPED there the moment the document is drained, so the list
       keeps a document alive only while it still owes a request. */
    DCHECK(JS_IsObject(document_object(ctx)),
           "HTML §4.6.8.20 Link type \"preload\"'s parsed-tree walk ran in a realm with no Document object — "
           "the walk is reached from the install that BUILDS one, so an entry filed here without it would name "
           "no document at the service and every one of that document's preloads would be a request nothing "
           "could issue");
    JS_SetPropertyStr(ctx, rec, "doc", JS_DupValue(ctx, document_object(ctx)));
    link_rec_set_int(ctx, rec, "served", 0);
    n = link_rec_int(ctx, g_parsed, "n");
    JS_SetPropertyUint32(ctx, g_parsed, (uint32_t)n, rec);
    link_rec_set_int(ctx, g_parsed, "n", n + 1);
}

/* See html_link.h. ONE ELEMENT PER CALL, then return, because this is a work item on the one frontier like
 * everything else the scheduler asks for — core/dom/document.c's document_lifecycle_step states the same
 * sentence for the same reason, and it is what keeps a document that ships seventy preloads from being a
 * stretch of the scheduler's time with no suspend point on it.
 *
 * THE CURSOR IS THE FLOW'S, WHICH IS THE WHOLE POINT: `served` and `i` are properties of baseline records, so
 * a write to either rides the writing flow's COW delta and every timeline drains this list once, on its own.
 * That is the same answer the document's own `<script>` rows already get — every base flow of a document runs
 * its programs — and it is what makes a resumed flow re-issue the preloads it was in the middle of.
 *
 * IN THE DOCUMENT'S OWN REALM. §4.6.8.20's steps read that document's base URL, its policy container and its
 * media environment, so the realm is asked of the ENTRY and never of whoever is running: a same-origin agent
 * cluster holds a realm per document, and answering a child's preload with the root's policy is the defect
 * CLAUDE.md §per-realm names — one fact answered from one place for many agents. */
int html_link_connected_step(JSContext *ctx)
{
    int i, n;

    if (!g_ready) return 0;
    n = link_rec_int(ctx, g_parsed, "n");
    for (i = link_rec_int(ctx, g_parsed, "i"); i < n; i++) {
        JSValue rec = JS_GetPropertyUint32(ctx, g_parsed, (uint32_t)i);
        int served;

        DCHECK(JS_IsObject(rec),
               "HTML §4.6.8.20 Link type \"preload\"'s parsed-document list holds no entry at an index below "
               "its own length — the entry and the length are written together at the one site that appends, "
               "so a hole here is a list whose count outran what a flow can see of it");
        served = link_rec_int(ctx, rec, "served");
        if (served >= 0) {
            JSValue docv = JS_GetPropertyStr(ctx, rec, "doc");
            lxb_dom_node_t *root = node_of(docv);
            JSContext *dctx;
            lxb_dom_element_t *el;

            DCHECK(root != NULL,
                   "HTML §4.6.8.20 Link type \"preload\"'s parsed-document list holds an entry whose `doc` is "
                   "not a node — it is written from that Document's own object and from nowhere else, and it "
                   "is cleared only together with the `served` sentinel that stops this arm being reached");
            dctx = document_realm_of(root);
            /* NOT AN IMPOSSIBLE STATE — A COUPLING THAT IS NOT BUILT, which is why it crashes here naming it
               rather than skipping the entry. The entry is appended by the install that BUILDS that realm, so
               a null means the Document's record went away while a timeline still owed its preloads a request:
               HTML §7.5.10 "Destroying documents"' step 7 removes that document's queued TASKS, and this
               inventory is not one of them, so the destruction must drop this timeline's entry the same way.
               Skipping instead would leave the flow's cursor standing on a document that no longer exists and
               call the preloads served, which is a page whose whole chunk graph silently never loads. */
            DCHECK(dctx != NULL,
                   "the parsed-document list holds a document with no realm — a Document was DESTROYED while a "
                   "timeline still owed its parsed `<link>` elements HTML §4.6.8.20 Link type \"preload\"'s "
                   "browsing-context-connected time. HTML §7.5.10 \"Destroying documents\"' step 7 takes that "
                   "document's queued tasks off the flow and this inventory is not one of them: drop this "
                   "timeline's entry there, beside that removal");
            el = link_parsed_nth(dctx, root, served);
            if (el) {
                link_rec_set_int(ctx, rec, "served", served + 1);
                JS_FreeValue(ctx, docv);
                JS_FreeValue(ctx, rec);
                link_trigger(dctx, el);
                return 1;
            }
            link_rec_set_int(ctx, rec, "served", -1);
            JS_SetPropertyStr(ctx, rec, "doc", JS_UNDEFINED);
            JS_FreeValue(ctx, docv);
        }
        JS_FreeValue(ctx, rec);
        /* THE ENTRY CURSOR ONLY EVER ADVANCES, because this loop drains in index order and returns the moment
           it serves one: an entry below `i` is drained in this timeline, so the scan a drained list costs is
           one integer read rather than a pass over every document this agent has ever parsed. */
        link_rec_set_int(ctx, g_parsed, "i", i + 1);
    }
    return 0;
}

/* ---- the agent's declaration ------------------------------------------------------------------------------ */

void html_link_declare(JSContext *ctx)
{
    JSRuntime *rt = JS_GetRuntime(ctx);

    DCHECK(!g_ready, "html_link_declare ran twice — HTML §4.6.8.20 Link type \"preload\"'s state key and task "
                     "machine are declared once per AGENT, and a second Symbol would leave every link element "
                     "that already carries its processing state under the first key answering as though it had "
                     "none under the second");
    g_state_key = JS_NewSymbol(ctx, "linkProcessing", false);
    CHECK(!JS_IsException(g_state_key),
          "HTML §4.6.8.20 Link type \"preload\": the link processing state key allocation failed");
    g_atom_state = JS_ValueToAtom(ctx, g_state_key);
    CHECK(g_atom_state != JS_ATOM_NULL,
          "HTML §4.6.8.20 Link type \"preload\": the link processing state key could not be interned");
    g_task_stepid = JS_RegisterStepDef(rt, &link_task_def);
    /* THE PARSED-DOCUMENT LIST IS BUILT WITH THE AGENT AND NOT ON FIRST USE, for the reason CLAUDE.md gives
       for every per-realm intrinsic: a record minted lazily is minted INSIDE whichever flow happened to touch
       it first, and every other timeline would then see a list that does not exist. This line is the pre-boot
       baseline, so the empty list is a fact every flow forks from. */
    g_parsed = idl_slots_new(ctx);
    CHECK(!JS_IsException(g_parsed),
          "HTML §4.6.8.20 Link type \"preload\": the parsed-document list allocation failed");
    JS_SetPropertyStr(ctx, g_parsed, "n", JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, g_parsed, "i", JS_NewInt32(ctx, 0));
    /* §4.6.8.20's browsing-context-connected time for the PARSER's own elements is a work item on the ONE
       frontier — see html_link_connected_step. Claimed here, once per AGENT, beside the machine above. */
    engine_set_link_connected_hook(html_link_connected_step);
    g_ready = 1;
}

void html_link_free(JSRuntime *rt)
{
    if (!g_ready) return;
    engine_set_link_connected_hook(NULL);
    JS_FreeAtomRT(rt, g_atom_state);
    g_atom_state = JS_ATOM_NULL;
    JS_FreeValueRT(rt, g_state_key);
    g_state_key = JS_UNDEFINED;
    JS_FreeValueRT(rt, g_parsed);
    g_parsed = JS_UNDEFINED;
    g_task_stepid = -1;
    g_ready = 0;
}
